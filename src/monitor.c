/**
 * @file monitor.c
 * @brief System health metrics collection.
 *
 * Spawns a POSIX thread that:
 *  1. Reads /proc/stat to compute CPU usage delta between two samples.
 *  2. Reads /proc/meminfo to get MemTotal, MemFree, MemAvailable.
 *  3. Checks whether the configured process is alive via /proc/<pid>/status
 *     (or scans /proc for the process name if PID is unknown).
 *  4. Pushes a MetricSnapshot into the shared circular buffer.
 */

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
static MetricCircBuf  *s_buf         = NULL;
static char            s_proc_name[HGW_MAX_PROC_NAME] = {0};
static uint32_t        s_interval_s  = 5;
static pthread_t       s_thread;
static volatile int    s_stop        = 0;

/* Previous /proc/stat values for CPU delta calculation */
static unsigned long long s_prev_total = 0;
static unsigned long long s_prev_idle  = 0;

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

    if (dtotal == 0) return 0;
    return (uint32_t)(100ULL * (dtotal - didle) / dtotal);
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
 * Process liveness check
 * ------------------------------------------------------------------------- */
static bool proc_is_alive(const char *name, pid_t *out_pid) {
    if (!name || name[0] == '\0') { *out_pid = 0; return true; }

    DIR *d = opendir("/proc");
    if (!d) return false;

    struct dirent *ent;
    char comm_path[64], comm[HGW_MAX_PROC_NAME];
    bool found = false;

    while ((ent = readdir(d)) != NULL) {
        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 0) continue;

        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        FILE *f = fopen(comm_path, "r");
        if (!f) continue;

        if (fgets(comm, sizeof(comm), f)) {
            /* strip newline */
            comm[strcspn(comm, "\n")] = '\0';
            if (strncmp(comm, name, HGW_MAX_PROC_NAME - 1) == 0) {
                *out_pid = pid;
                found = true;
            }
        }
        fclose(f);
        if (found) break;
    }
    closedir(d);
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

    while (!s_stop) {
        sleep(s_interval_s);
        if (s_stop) break;

        MetricSnapshot snap = {0};
        clock_gettime(CLOCK_MONOTONIC, &snap.ts);

        snap.cpu_pct      = compute_cpu_pct();
        read_mem_stats(&snap.mem_total_kb, &snap.mem_free_kb, &snap.mem_used_pct);
        snap.proc_alive   = proc_is_alive(s_proc_name, &snap.proc_pid);

        /* Push to circular buffer (overwrite oldest if full) */
        int next_head = (s_buf->head + 1) % HGW_CIRC_BUF_SIZE;
        s_buf->slots[s_buf->head] = snap;
        s_buf->head = next_head;

        LOG_DEBUG("Sample: cpu=%u%% mem=%u%% free=%ukB proc_alive=%d",
                  snap.cpu_pct, snap.mem_used_pct,
                  snap.mem_free_kb, snap.proc_alive);
    }

    LOG_INFO("Monitor thread exiting");
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
int monitor_init(MetricCircBuf *buf, const char *proc_name, uint32_t interval_s) {
    s_buf        = buf;
    s_interval_s = (interval_s > 0) ? interval_s : 5;
    s_stop       = 0;
    memset(buf, 0, sizeof(*buf));

    if (proc_name)
        strncpy(s_proc_name, proc_name, sizeof(s_proc_name) - 1);

    return 0;
}

int monitor_start(void) {
    return pthread_create(&s_thread, NULL, monitor_thread, NULL);
}

void monitor_stop(void) {
    s_stop = 1;
    pthread_join(s_thread, NULL);
}

bool monitor_peek_latest(MetricSnapshot *out) {
    if (s_buf->head == s_buf->tail) return false;
    int latest = (s_buf->head - 1 + HGW_CIRC_BUF_SIZE) % HGW_CIRC_BUF_SIZE;
    *out = s_buf->slots[latest];
    return true;
}
