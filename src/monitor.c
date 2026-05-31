/**
 * @file monitor.c
 * @brief Periodic collection of CPU, memory and process liveness metrics.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "monitor.h"
#include "logger.h"

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */
static MetricCircBuf   *s_buf         = NULL;
static char             s_proc_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME];
static int              s_proc_count  = 0;
static uint32_t         s_interval_s  = 5;
static pthread_t        s_thread;
static _Atomic int      s_stop        = ATOMIC_VAR_INIT(0);
static volatile int     s_count       = 0;  /* total samples written */

/* Protects s_proc_names, s_proc_count, s_interval_s against monitor_update_config() */
static pthread_mutex_t  s_cfg_mutex   = PTHREAD_MUTEX_INITIALIZER;

/* Previous /proc/stat values for CPU delta calculation */
static unsigned long long s_prev_total    = 0;
static unsigned long long s_prev_idle     = 0;
static unsigned long long s_last_cpu_delta = 0; /* ticks in last sample period */

/* Previous per-process CPU ticks and cached PIDs */
static unsigned long long s_prev_proc_cpu[HGW_MAX_PROC_LIST];
static pid_t              s_cached_pids[HGW_MAX_PROC_LIST];

/* Set by monitor_update_config() to signal the monitor thread to reset CPU deltas */
static _Atomic int s_reset_cpu_history = ATOMIC_VAR_INIT(0);

/* -------------------------------------------------------------------------
 * /proc/stat CPU parsing
 * ------------------------------------------------------------------------- */
static int read_cpu_stats(unsigned long long *total, unsigned long long *idle) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;

    unsigned long long user, nice, system, id, iowait, irq, softirq, steal;
    int rc = fscanf(f, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                    &user, &nice, &system, &id, &iowait, &irq, &softirq, &steal);
    fclose(f);
    if (rc != 8) return -1;

    *idle  = id + iowait;
    *total = user + nice + system + id + iowait + irq + softirq + steal;
    return 0;
}

static uint32_t compute_cpu_pct(void) {
    unsigned long long total, idle;
    if (read_cpu_stats(&total, &idle) != 0) return 0;

    unsigned long long dtotal = total - s_prev_total;
    unsigned long long didle  = idle  - s_prev_idle;
    s_prev_total = total;
    s_prev_idle  = idle;
    s_last_cpu_delta = dtotal;

    if (dtotal == 0) return 0;
    return (uint32_t)(100ULL * (dtotal - didle) / dtotal);
}

/* -------------------------------------------------------------------------
 * Per-process CPU % from /proc/<pid>/stat
 * ------------------------------------------------------------------------- */
static uint32_t proc_cpu_pct(int proc_idx, pid_t pid) {
    if (pid <= 0 || s_last_cpu_delta == 0) return 0;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[512];
    bool ok = (fgets(buf, sizeof(buf), f) != NULL);
    fclose(f);
    if (!ok) return 0;

    /* Skip past the closing ')' of the comm field (may contain spaces) */
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    p++;

    char state;
    int ppid, pgrp, session, tty, tpgid;
    unsigned int flags;
    unsigned long long minflt, cminflt, majflt, cmajflt, utime, stime;

    int rc = sscanf(p, " %c %d %d %d %d %d %u %llu %llu %llu %llu %llu %llu",
                    &state, &ppid, &pgrp, &session, &tty, &tpgid,
                    &flags, &minflt, &cminflt, &majflt, &cmajflt,
                    &utime, &stime);
    if (rc < 13) return 0;

    unsigned long long cur  = utime + stime;
    unsigned long long prev = s_prev_proc_cpu[proc_idx];
    s_prev_proc_cpu[proc_idx] = cur;

    if (prev == 0) return 0; /* first sample: just prime the counter */
    unsigned long long delta = (cur >= prev) ? (cur - prev) : 0;
    uint32_t pct = (uint32_t)(100ULL * delta / s_last_cpu_delta);
    return (pct > 100) ? 100 : pct;
}

/* -------------------------------------------------------------------------
 * Per-process memory % from /proc/<pid>/status VmRSS
 * ------------------------------------------------------------------------- */
static uint32_t proc_mem_pct(pid_t pid, uint32_t total_mem_kb) {
    if (pid <= 0 || total_mem_kb == 0) return 0;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[128];
    unsigned long vmrss = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %lu kB", &vmrss) == 1) break;
    }
    fclose(f);

    if (vmrss == 0) return 0;
    return (uint32_t)(100ULL * vmrss / total_mem_kb);
}

/* -------------------------------------------------------------------------
 * Per-process thread stats from /proc/<pid>/status + /proc/<pid>/task/
 * ------------------------------------------------------------------------- */
static void proc_thread_stats(pid_t pid, uint32_t *out_total,
                               uint32_t *out_zombie, uint32_t *out_blocked) {
    *out_total = 0; *out_zombie = 0; *out_blocked = 0;
    if (pid <= 0) return;

    char path[80];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            unsigned long threads;
            if (sscanf(line, "Threads: %lu", &threads) == 1) {
                *out_total = (uint32_t)threads;
                break;
            }
        }
        fclose(f);
    }

    snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        pid_t tid = (pid_t)atoi(ent->d_name);
        if (tid <= 0) continue;

        char tid_path[96];
        snprintf(tid_path, sizeof(tid_path), "/proc/%d/task/%d/status",
                 (int)pid, (int)tid);
        FILE *tf = fopen(tid_path, "r");
        if (!tf) continue;

        char line[128];
        while (fgets(line, sizeof(line), tf)) {
            char state;
            if (sscanf(line, "State: %c", &state) == 1) {
                if (state == 'Z') (*out_zombie)++;
                else if (state == 'D') (*out_blocked)++;
                break;
            }
        }
        fclose(tf);
    }
    closedir(d);
}

/* -------------------------------------------------------------------------
 * /proc/meminfo parsing
 * ------------------------------------------------------------------------- */
static int read_mem_stats(uint32_t *total_kb, uint32_t *free_kb,
                           uint32_t *used_pct) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;

    char line[128];
    unsigned long mem_total = 0, mem_available = 0;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1) continue;
    }
    fclose(f);

    if (mem_total == 0) return -1;

    *total_kb  = (uint32_t)mem_total;
    *free_kb   = (uint32_t)mem_available;
    *used_pct  = (uint32_t)(100ULL * (mem_total - mem_available) / mem_total);
    return 0;
}

/* -------------------------------------------------------------------------
 * Process liveness check with PID caching
 * Tries the cached PID first; falls back to a full /proc scan on miss.
 * ------------------------------------------------------------------------- */
static bool proc_is_alive(const char *name, pid_t *out_pid, int proc_idx) {
    if (!name || name[0] == '\0') { *out_pid = 0; return true; }

    char comm_path[64], comm[HGW_MAX_PROC_NAME];

    /* Fast path: verify the cached PID */
    if (proc_idx >= 0 && proc_idx < HGW_MAX_PROC_LIST && s_cached_pids[proc_idx] > 0) {
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm",
                 (int)s_cached_pids[proc_idx]);
        FILE *f = fopen(comm_path, "r");
        if (f) {
            bool match = false;
            if (fgets(comm, sizeof(comm), f)) {
                comm[strcspn(comm, "\n")] = '\0';
                match = (strncmp(comm, name, HGW_MAX_PROC_NAME - 1) == 0);
            }
            fclose(f);
            if (match) {
                *out_pid = s_cached_pids[proc_idx];
                return true;
            }
        }
        s_cached_pids[proc_idx] = 0; /* stale cache entry */
    }

    /* Slow path: full /proc scan */
    DIR *d = opendir("/proc");
    if (!d) return false;

    struct dirent *ent;
    bool found = false;

    while ((ent = readdir(d)) != NULL) {
        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 0) continue;

        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)pid);
        FILE *f = fopen(comm_path, "r");
        if (!f) continue;

        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strncmp(comm, name, HGW_MAX_PROC_NAME - 1) == 0) {
                *out_pid = pid;
                if (proc_idx >= 0 && proc_idx < HGW_MAX_PROC_LIST)
                    s_cached_pids[proc_idx] = pid;
                found = true;
            }
        }
        fclose(f);
        if (found) break;
    }
    closedir(d);
    if (!found && proc_idx >= 0 && proc_idx < HGW_MAX_PROC_LIST)
        s_cached_pids[proc_idx] = 0;
    return found;
}

/* -------------------------------------------------------------------------
 * Monitor thread
 * ------------------------------------------------------------------------- */
static void *monitor_thread(void *arg) {
    (void)arg;
    LOG_INFO("Monitor thread started (interval=%us)", s_interval_s);

    /* Prime CPU delta */
    unsigned long long t, i;
    read_cpu_stats(&t, &i);
    s_prev_total = t; s_prev_idle = i;
    memset(s_prev_proc_cpu, 0, sizeof(s_prev_proc_cpu));

    while (!atomic_load_explicit(&s_stop, memory_order_relaxed)) {
        /* Take a local copy of config so monitor_update_config() can run safely */
        pthread_mutex_lock(&s_cfg_mutex);
        uint32_t interval = s_interval_s;
        int      proc_count = s_proc_count;
        char     proc_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME];
        memcpy(proc_names, s_proc_names, sizeof(proc_names));
        pthread_mutex_unlock(&s_cfg_mutex);

        sleep(interval);
        if (atomic_load_explicit(&s_stop, memory_order_relaxed)) break;

        /* Reset per-process CPU deltas and PID cache if config changed */
        if (atomic_load_explicit(&s_reset_cpu_history, memory_order_acquire)) {
            atomic_store_explicit(&s_reset_cpu_history, 0, memory_order_relaxed);
            memset(s_prev_proc_cpu, 0, sizeof(s_prev_proc_cpu));
            memset(s_cached_pids,   0, sizeof(s_cached_pids));
        }

        MetricSnapshot snap = {0};
        clock_gettime(CLOCK_MONOTONIC, &snap.ts);

        snap.sys_cpu_pct = compute_cpu_pct(); /* also updates s_last_cpu_delta */
        if (read_mem_stats(&snap.sys_mem_total_kb, &snap.sys_mem_free_kb,
                           &snap.sys_mem_pct) != 0)
            LOG_WARN("Failed to read /proc/meminfo — mem stats zeroed this sample");

        snap.proc_count = proc_count;
        for (int pi = 0; pi < proc_count; pi++) {
            ProcessStat *ps = &snap.procs[pi];
            strncpy(ps->name, proc_names[pi], HGW_MAX_PROC_NAME - 1);
            ps->name[HGW_MAX_PROC_NAME - 1] = '\0';
            pid_t pid = 0;
            ps->alive  = proc_is_alive(proc_names[pi], &pid, pi);
            ps->pid    = pid;
            if (ps->alive && pid > 0) {
                ps->cpu_pct = proc_cpu_pct(pi, pid);
                ps->mem_pct = proc_mem_pct(pid, snap.sys_mem_total_kb);
                proc_thread_stats(pid, &ps->thread_count,
                                  &ps->zombie_thread_count,
                                  &ps->blocked_thread_count);
            } else {
                ps->cpu_pct = 0;
                ps->mem_pct = 0;
                ps->thread_count = 0;
                ps->zombie_thread_count = 0;
                ps->blocked_thread_count = 0;
            }
        }

        /* Push to circular buffer (overwrite oldest if full) */
        pthread_mutex_lock(&s_buf->buf_mutex);
        s_buf->slots[s_buf->head] = snap;
        s_buf->head = (s_buf->head + 1) % HGW_CIRC_BUF_SIZE;
        s_count++;
        pthread_mutex_unlock(&s_buf->buf_mutex);

        LOG_DEBUG("Sample: cpu=%u%% mem=%u%% free=%ukB procs=%d",
                  snap.sys_cpu_pct, snap.sys_mem_pct,
                  snap.sys_mem_free_kb, snap.proc_count);
    }

    LOG_INFO("Monitor thread exiting");
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
int monitor_init(MetricCircBuf *buf,
                 const char (*proc_names)[HGW_MAX_PROC_NAME], int proc_count,
                 uint32_t interval_s) {
    s_buf        = buf;
    atomic_store_explicit(&s_stop, 0, memory_order_relaxed);
    s_count      = 0;
    memset(buf, 0, sizeof(*buf));
    pthread_mutex_init(&buf->buf_mutex, NULL);  /* must come after memset */
    memset(s_prev_proc_cpu, 0, sizeof(s_prev_proc_cpu));
    memset(s_cached_pids,   0, sizeof(s_cached_pids));
    s_reset_cpu_history = 0;

    pthread_mutex_lock(&s_cfg_mutex);
    s_interval_s = (interval_s > 0) ? interval_s : 5;
    memset(s_proc_names, 0, sizeof(s_proc_names));
    s_proc_count = (proc_count > HGW_MAX_PROC_LIST) ? HGW_MAX_PROC_LIST : proc_count;
    for (int i = 0; i < s_proc_count; i++)
        strncpy(s_proc_names[i], proc_names[i], HGW_MAX_PROC_NAME - 1);
    pthread_mutex_unlock(&s_cfg_mutex);

    return 0;
}

int monitor_start(void) {
    return pthread_create(&s_thread, NULL, monitor_thread, NULL);
}

void monitor_stop(void) {
    atomic_store_explicit(&s_stop, 1, memory_order_relaxed);
    pthread_join(s_thread, NULL);
    pthread_mutex_destroy(&s_buf->buf_mutex);
    pthread_mutex_destroy(&s_cfg_mutex);
}

bool monitor_peek_latest(MetricSnapshot *out) {
    pthread_mutex_lock(&s_buf->buf_mutex);
    if (s_count == 0) {
        pthread_mutex_unlock(&s_buf->buf_mutex);
        return false;
    }
    int latest = (s_buf->head - 1 + HGW_CIRC_BUF_SIZE) % HGW_CIRC_BUF_SIZE;
    *out = s_buf->slots[latest];
    pthread_mutex_unlock(&s_buf->buf_mutex);
    return true;
}

void monitor_update_config(const char (*proc_names)[HGW_MAX_PROC_NAME],
                            int proc_count, uint32_t interval_s) {
    pthread_mutex_lock(&s_cfg_mutex);
    s_interval_s = (interval_s > 0) ? interval_s : 5;
    memset(s_proc_names, 0, sizeof(s_proc_names));
    s_proc_count = (proc_count > HGW_MAX_PROC_LIST) ? HGW_MAX_PROC_LIST : proc_count;
    for (int i = 0; i < s_proc_count; i++)
        strncpy(s_proc_names[i], proc_names[i], HGW_MAX_PROC_NAME - 1);
    atomic_store_explicit(&s_reset_cpu_history, 1, memory_order_release);
    pthread_mutex_unlock(&s_cfg_mutex);
}
