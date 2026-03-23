/**
 * @file hgwd_metrics.c
 * @brief /proc metric readers: CPU, memory, process liveness, PID files.
 *
 * All functions are stateless — callers own the delta state for CPU.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>

#include "hgwdoctor.h"

/* ── CPU ─────────────────────────────────────────────────────────────────── */

/**
 * hgwd_metrics_read_cpu - compute CPU usage % since last call.
 *
 * @prev_total  in/out: cumulative total jiffies from previous sample
 * @prev_idle   in/out: cumulative idle+iowait jiffies from previous sample
 * @out_pct     output: 0-100 integer CPU usage
 *
 * Returns 0 on success, -1 on read/parse error.
 */
int hgwd_metrics_read_cpu(unsigned long long *prev_total,
                           unsigned long long *prev_idle,
                           uint32_t *out_pct) {
    FILE *f = NULL;
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total, idle_val, dtotal, didle;

    when_null_trace(prev_total, exit_err, WARNING, "prev_total is NULL");
    when_null_trace(prev_idle,  exit_err, WARNING, "prev_idle is NULL");
    when_null_trace(out_pct,    exit_err, WARNING, "out_pct is NULL");

    f = fopen("/proc/stat", "r");
    when_null_trace(f, exit_err, WARNING, "Cannot open /proc/stat");

    if(fscanf(f, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
              &user, &nice, &system, &idle,
              &iowait, &irq, &softirq, &steal) != 8) {
        fclose(f);
        goto exit_err;
    }
    fclose(f);

    idle_val = idle + iowait;
    total    = user + nice + system + idle + iowait + irq + softirq + steal;

    dtotal = (total > *prev_total) ? (total - *prev_total) : 0;
    didle  = (idle_val > *prev_idle) ? (idle_val - *prev_idle) : 0;

    *out_pct     = (dtotal > 0) ? (uint32_t)(100ULL * (dtotal - didle) / dtotal) : 0;
    *prev_total  = total;
    *prev_idle   = idle_val;
    return 0;

exit_err:
    if(out_pct) *out_pct = 0;
    return -1;
}

/* ── Memory ──────────────────────────────────────────────────────────────── */

/**
 * hgwd_metrics_read_mem - read memory usage from /proc/meminfo.
 *
 * @out_pct     output: used memory as 0-100%
 * @out_free_kb output: available memory in kB
 *
 * Returns 0 on success, -1 on error.
 */
int hgwd_metrics_read_mem(uint32_t *out_pct, uint32_t *out_free_kb) {
    FILE *f = NULL;
    char line[128];
    unsigned long mem_total = 0, mem_available = 0;

    when_null_trace(out_pct,     exit_err, WARNING, "out_pct is NULL");
    when_null_trace(out_free_kb, exit_err, WARNING, "out_free_kb is NULL");

    f = fopen("/proc/meminfo", "r");
    when_null_trace(f, exit_err, WARNING, "Cannot open /proc/meminfo");

    while(fgets(line, sizeof(line), f)) {
        sscanf(line, "MemTotal: %lu kB",     &mem_total);
        sscanf(line, "MemAvailable: %lu kB", &mem_available);
    }
    fclose(f);

    if(mem_total == 0) goto exit_err;

    *out_free_kb = (uint32_t)mem_available;
    *out_pct     = (uint32_t)(100ULL * (mem_total - mem_available) / mem_total);
    return 0;

exit_err:
    if(out_pct)     *out_pct     = 0;
    if(out_free_kb) *out_free_kb = 0;
    return -1;
}

/* ── Process liveness ────────────────────────────────────────────────────── */

/**
 * hgwd_metrics_proc_alive - check if a named process is running.
 *
 * Scans /proc/<pid>/comm for a match.  If name is empty, returns true
 * (no process configured to watch).
 *
 * @name    process name (as in /proc/<pid>/comm, 15 chars max on Linux)
 * @out_pid set to the PID if found; may be NULL
 *
 * Returns true if found, false otherwise.
 */
bool hgwd_metrics_proc_alive(const char *name, pid_t *out_pid) {
    DIR *d = NULL;
    struct dirent *ent;
    char comm_path[64];
    char comm[64];
    bool found = false;

    if(!name || name[0] == '\0') {
        if(out_pid) *out_pid = 0;
        return true;  /* no process configured — trivially alive */
    }

    d = opendir("/proc");
    when_null_trace(d, exit, WARNING, "Cannot open /proc");

    while((ent = readdir(d)) != NULL && !found) {
        pid_t pid = (pid_t)atoi(ent->d_name);
        if(pid <= 0) continue;

        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)pid);
        FILE *f = fopen(comm_path, "r");
        if(!f) continue;

        if(fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = '\0';
            if(strncmp(comm, name, sizeof(comm) - 1) == 0) {
                if(out_pid) *out_pid = pid;
                found = true;
            }
        }
        fclose(f);
    }

exit:
    if(d) closedir(d);
    return found;
}

/* ── PID file reader ─────────────────────────────────────────────────────── */

/**
 * hgwd_metrics_read_pidfile - read a PID from a PID file.
 *
 * Returns the PID on success, -1 if the file cannot be opened or parsed.
 */
pid_t hgwd_metrics_read_pidfile(const char *path) {
    FILE *f = NULL;
    int pid = -1;

    when_null_trace(path, exit, WARNING, "pidfile path is NULL");
    when_null_trace(path[0] != '\0' ? path : NULL, exit,
                    WARNING, "pidfile path is empty");

    f = fopen(path, "r");
    when_null_trace(f, exit, WARNING, "Cannot open pidfile: %s", path);

    fscanf(f, "%d", &pid);
    fclose(f);

exit:
    return (pid_t)pid;
}
