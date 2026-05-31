/**
 * @file analyzer.c
 * @brief Anomaly detection based on metric history.
 */

#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "analyzer.h"
#include "datamodel.h"
#include "logger.h"
#include "monitor.h"
#include "types.h"

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */
static MetricCircBuf    *s_buf          = NULL;
static AnalyzerConfig    s_cfg;
static anomaly_callback  s_callback;
static void             *s_userdata;

static pthread_t         s_thread;
static _Atomic int       s_stop = ATOMIC_VAR_INIT(0);

/* Protects s_cfg against analyzer_update_config() called from main thread */
static pthread_mutex_t   s_cfg_mutex    = PTHREAD_MUTEX_INITIALIZER;

/* Set by analyzer_update_config() to clear per-process history on next sample */
static _Atomic int       s_reset_proc_history = ATOMIC_VAR_INIT(0);

/* History buffers for each metric type */
#define HISTORY_MAX 300  /* enough for 5 minutes at 1s interval, adjust as needed */
static struct {
    uint32_t sys_cpu[HISTORY_MAX];
    uint32_t sys_mem[HISTORY_MAX];
    /* per-process histories */
    uint32_t proc_cpu[HGW_MAX_PROC_LIST][HISTORY_MAX];
    uint32_t proc_mem[HGW_MAX_PROC_LIST][HISTORY_MAX];
    int      proc_alive[HGW_MAX_PROC_LIST][HISTORY_MAX]; /* 1=alive, 0=dead */
    uint32_t proc_threads[HGW_MAX_PROC_LIST][HISTORY_MAX];
    uint32_t proc_zombie[HGW_MAX_PROC_LIST][HISTORY_MAX];
    uint32_t proc_blocked[HGW_MAX_PROC_LIST][HISTORY_MAX];
    int      idx;
    int      count;
} s_history;

static int timespec_equal(const struct timespec *lhs, const struct timespec *rhs) {
    return lhs->tv_sec == rhs->tv_sec && lhs->tv_nsec == rhs->tv_nsec;
}

/* -------------------------------------------------------------------------
 * History management
 * ------------------------------------------------------------------------- */
static void history_push(const MetricSnapshot *snap) {
    s_history.sys_cpu[s_history.idx] = snap->sys_cpu_pct;
    s_history.sys_mem[s_history.idx] = snap->sys_mem_pct;

    for (int i = 0; i < snap->proc_count && i < HGW_MAX_PROC_LIST; i++) {
        s_history.proc_cpu[i][s_history.idx]     = snap->procs[i].cpu_pct;
        s_history.proc_mem[i][s_history.idx]     = snap->procs[i].mem_pct;
        s_history.proc_alive[i][s_history.idx]   = snap->procs[i].alive ? 1 : 0;
        s_history.proc_threads[i][s_history.idx] = snap->procs[i].thread_count;
        s_history.proc_zombie[i][s_history.idx]  = snap->procs[i].zombie_thread_count;
        s_history.proc_blocked[i][s_history.idx] = snap->procs[i].blocked_thread_count;
    }

    s_history.idx = (s_history.idx + 1) % HISTORY_MAX;
    if (s_history.count < HISTORY_MAX)
        s_history.count++;
}

/* Check if a metric has been above threshold for the last N samples */
static int sustained_threshold(const uint32_t *history, int count,
                               uint32_t threshold, int required_samples) {
    if (count < required_samples) return 0;
    int start = (s_history.idx - required_samples + HISTORY_MAX) % HISTORY_MAX;
    for (int i = 0; i < required_samples; i++) {
        int pos = (start + i) % HISTORY_MAX;
        if (history[pos] < threshold) return 0;
    }
    return 1;
}

/* Check process dead for required samples */
static int sustained_dead(const int *history, int count, int required_samples) {
    if (count < required_samples) return 0;
    int start = (s_history.idx - required_samples + HISTORY_MAX) % HISTORY_MAX;
    for (int i = 0; i < required_samples; i++) {
        int pos = (start + i) % HISTORY_MAX;
        if (history[pos] != 0) return 0;
    }
    return 1;
}

/* Check if every sample in the window is strictly below threshold */
static int sustained_below(const uint32_t *history, int count,
                            uint32_t threshold, int required_samples) {
    if (count < required_samples) return 0;
    int start = (s_history.idx - required_samples + HISTORY_MAX) % HISTORY_MAX;
    for (int i = 0; i < required_samples; i++) {
        int pos = (start + i) % HISTORY_MAX;
        if (history[pos] >= threshold) return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Analyzer thread
 * ------------------------------------------------------------------------- */
static void *analyzer_thread(void *arg) {
    (void)arg;
    LOG_INFO("Analyzer thread started");

    AnalyzerConfig cfg;
    pthread_mutex_lock(&s_cfg_mutex);
    cfg = s_cfg;
    int required_samples =
        (int) ((cfg.threshold_duration_s + cfg.poll_interval_s - 1) / cfg.poll_interval_s);
    pthread_mutex_unlock(&s_cfg_mutex);
    if (required_samples < 1) required_samples = 1;
    if (required_samples > HISTORY_MAX) required_samples = HISTORY_MAX;

    struct timespec last_seen = {0};
    int have_last_seen = 0;
    int cpu_alert_active = 0;
    int mem_alert_active = 0;
    int proc_dead_alert[HGW_MAX_PROC_LIST]       = {0};
    int proc_cpu_alert[HGW_MAX_PROC_LIST]        = {0};
    int proc_mem_alert[HGW_MAX_PROC_LIST]        = {0};
    int proc_thread_low_alert[HGW_MAX_PROC_LIST] = {0};
    int proc_zombie_alert[HGW_MAX_PROC_LIST]     = {0};
    int proc_blocked_alert[HGW_MAX_PROC_LIST]    = {0};

    while (!atomic_load_explicit(&s_stop, memory_order_relaxed)) {
        /* Sleep half the poll interval — avoids 50 no-op wakes per sample.
         * Clamped to [100ms, 2s] so we never stall too long or spin too fast. */
        uint32_t sleep_us = cfg.poll_interval_s * 500000u;
        if (sleep_us < 100000u)  sleep_us = 100000u;
        if (sleep_us > 2000000u) sleep_us = 2000000u;
        usleep(sleep_us);

        /* Read the latest snapshot from circular buffer (non-destructive) */
        MetricSnapshot snap;
        if (!monitor_peek_latest(&snap)) continue;  /* buffer empty */
        if (have_last_seen && timespec_equal(&last_seen, &snap.ts)) continue;

        last_seen = snap.ts;
        have_last_seen = 1;

        /* Take a local config copy so analyzer_update_config() can run safely */
        pthread_mutex_lock(&s_cfg_mutex);
        cfg = s_cfg;
        required_samples = (int) ((cfg.threshold_duration_s + cfg.poll_interval_s - 1)
                                  / cfg.poll_interval_s);
        pthread_mutex_unlock(&s_cfg_mutex);
        if (required_samples < 1) required_samples = 1;
        if (required_samples > HISTORY_MAX) required_samples = HISTORY_MAX;

        /* Clear per-process history when the process list has changed so stale
         * data from a former process at the same slot doesn't trigger false anomalies. */
        if (atomic_load_explicit(&s_reset_proc_history, memory_order_acquire)) {
            atomic_store_explicit(&s_reset_proc_history, 0, memory_order_relaxed);
            memset(s_history.proc_cpu,     0, sizeof(s_history.proc_cpu));
            memset(s_history.proc_mem,     0, sizeof(s_history.proc_mem));
            memset(s_history.proc_alive,   0, sizeof(s_history.proc_alive));
            memset(s_history.proc_threads, 0, sizeof(s_history.proc_threads));
            memset(s_history.proc_zombie,  0, sizeof(s_history.proc_zombie));
            memset(s_history.proc_blocked, 0, sizeof(s_history.proc_blocked));
            memset(proc_dead_alert,        0, sizeof(proc_dead_alert));
            memset(proc_cpu_alert,         0, sizeof(proc_cpu_alert));
            memset(proc_mem_alert,         0, sizeof(proc_mem_alert));
            memset(proc_thread_low_alert,  0, sizeof(proc_thread_low_alert));
            memset(proc_zombie_alert,      0, sizeof(proc_zombie_alert));
            memset(proc_blocked_alert,     0, sizeof(proc_blocked_alert));
        }

        history_push(&snap);

        /* --- System-wide CPU --- */
        if (snap.sys_cpu_pct >= cfg.cpu_threshold_pct) {
            if (!cpu_alert_active &&
                sustained_threshold(s_history.sys_cpu, s_history.count,
                                    cfg.cpu_threshold_pct, required_samples)) {
                AnomalyEvent ev = {
                    .type = ANOMALY_CPU,
                    .metric_value = snap.sys_cpu_pct,
                    .duration_s = cfg.threshold_duration_s
                };
                clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                if (s_callback) s_callback(&ev, s_userdata);
                cpu_alert_active = 1;
            }
        } else {
            cpu_alert_active = 0;
        }

        /* --- System-wide Memory --- */
        if (snap.sys_mem_pct >= cfg.mem_threshold_pct) {
            if (!mem_alert_active &&
                sustained_threshold(s_history.sys_mem, s_history.count,
                                    cfg.mem_threshold_pct, required_samples)) {
                AnomalyEvent ev = {
                    .type = ANOMALY_MEMORY,
                    .metric_value = snap.sys_mem_pct,
                    .duration_s = cfg.threshold_duration_s
                };
                clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                if (s_callback) s_callback(&ev, s_userdata);
                mem_alert_active = 1;
            }
        } else {
            mem_alert_active = 0;
        }

        /* --- Per-process checks --- */
        for (int i = 0; i < snap.proc_count && i < HGW_MAX_PROC_LIST; i++) {
            const ProcessStat *ps = &snap.procs[i];

            if (!ps->alive) {
                /* Process dead */
                if (!proc_dead_alert[i] &&
                    sustained_dead(s_history.proc_alive[i], s_history.count,
                                   required_samples)) {
                    AnomalyEvent ev = {
                        .type = ANOMALY_PROCESS,
                        .metric_value = 0,
                        .duration_s = cfg.threshold_duration_s
                    };
                    strncpy(ev.process_name, ps->name, HGW_MAX_PROC_NAME - 1);
                    clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                    if (s_callback) s_callback(&ev, s_userdata);
                    proc_dead_alert[i] = 1;
                }
                proc_cpu_alert[i]        = 0;
                proc_mem_alert[i]        = 0;
                proc_thread_low_alert[i] = 0;
                proc_zombie_alert[i]     = 0;
                proc_blocked_alert[i]    = 0;
            } else {
                proc_dead_alert[i] = 0;

                /* Per-process CPU */
                uint32_t cpu_thr = datamodel_get_process_cpu_threshold(ps->name);
                if (ps->cpu_pct >= cpu_thr) {
                    if (!proc_cpu_alert[i] &&
                        sustained_threshold(s_history.proc_cpu[i], s_history.count,
                                            cpu_thr, required_samples)) {
                        AnomalyEvent ev = {
                            .type = ANOMALY_PROCESS_CPU,
                            .metric_value = ps->cpu_pct,
                            .duration_s = cfg.threshold_duration_s
                        };
                        strncpy(ev.process_name, ps->name, HGW_MAX_PROC_NAME - 1);
                        clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                        if (s_callback) s_callback(&ev, s_userdata);
                        proc_cpu_alert[i] = 1;
                    }
                } else {
                    proc_cpu_alert[i] = 0;
                }

                /* Per-process Memory */
                uint32_t mem_thr = datamodel_get_process_mem_threshold(ps->name);
                if (ps->mem_pct >= mem_thr) {
                    if (!proc_mem_alert[i] &&
                        sustained_threshold(s_history.proc_mem[i], s_history.count,
                                            mem_thr, required_samples)) {
                        AnomalyEvent ev = {
                            .type = ANOMALY_PROCESS_MEM,
                            .metric_value = ps->mem_pct,
                            .duration_s = cfg.threshold_duration_s
                        };
                        strncpy(ev.process_name, ps->name, HGW_MAX_PROC_NAME - 1);
                        clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                        if (s_callback) s_callback(&ev, s_userdata);
                        proc_mem_alert[i] = 1;
                    }
                } else {
                    proc_mem_alert[i] = 0;
                }

                /* Per-process thread checks */
                uint32_t min_thr = datamodel_get_process_min_thread_count(ps->name);

                /* ThreadLow: thread_count < MinThreadCount sustained */
                if (ps->thread_count < min_thr) {
                    if (!proc_thread_low_alert[i] &&
                        sustained_below(s_history.proc_threads[i], s_history.count,
                                        min_thr, required_samples)) {
                        AnomalyEvent ev = {
                            .type = ANOMALY_THREAD_LOW,
                            .metric_value = ps->thread_count,
                            .duration_s = cfg.threshold_duration_s
                        };
                        strncpy(ev.process_name, ps->name, HGW_MAX_PROC_NAME - 1);
                        clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                        if (s_callback) s_callback(&ev, s_userdata);
                        proc_thread_low_alert[i] = 1;
                    }
                } else {
                    proc_thread_low_alert[i] = 0;
                }

                /* ZombieThread: zombie_thread_count >= 1 sustained */
                if (ps->zombie_thread_count > 0) {
                    if (!proc_zombie_alert[i] &&
                        sustained_threshold(s_history.proc_zombie[i], s_history.count,
                                            1, required_samples)) {
                        AnomalyEvent ev = {
                            .type = ANOMALY_ZOMBIE_THREAD,
                            .metric_value = ps->zombie_thread_count,
                            .duration_s = cfg.threshold_duration_s
                        };
                        strncpy(ev.process_name, ps->name, HGW_MAX_PROC_NAME - 1);
                        clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                        if (s_callback) s_callback(&ev, s_userdata);
                        proc_zombie_alert[i] = 1;
                    }
                } else {
                    proc_zombie_alert[i] = 0;
                }

                /* BlockedThread: blocked_thread_count >= 1 sustained */
                if (ps->blocked_thread_count > 0) {
                    if (!proc_blocked_alert[i] &&
                        sustained_threshold(s_history.proc_blocked[i], s_history.count,
                                            1, required_samples)) {
                        AnomalyEvent ev = {
                            .type = ANOMALY_BLOCKED_THREAD,
                            .metric_value = ps->blocked_thread_count,
                            .duration_s = cfg.threshold_duration_s
                        };
                        strncpy(ev.process_name, ps->name, HGW_MAX_PROC_NAME - 1);
                        clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                        if (s_callback) s_callback(&ev, s_userdata);
                        proc_blocked_alert[i] = 1;
                    }
                } else {
                    proc_blocked_alert[i] = 0;
                }
            }
        }
    }

    LOG_INFO("Analyzer thread exiting");
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
int analyzer_init(MetricCircBuf *buf, const AnalyzerConfig *cfg,
                  anomaly_callback cb, void *userdata) {
    s_buf = buf;
    s_cfg = *cfg;
    s_callback = cb;
    s_userdata = userdata;
    atomic_store_explicit(&s_stop, 0, memory_order_relaxed);
    memset(&s_history, 0, sizeof(s_history));
    return 0;
}

int analyzer_start(void) {
    if (!s_buf) return -1;
    return pthread_create(&s_thread, NULL, analyzer_thread, NULL);
}

void analyzer_stop(void) {
    atomic_store_explicit(&s_stop, 1, memory_order_relaxed);
    pthread_join(s_thread, NULL);
    pthread_mutex_destroy(&s_cfg_mutex);
}

void analyzer_update_config(const AnalyzerConfig *cfg) {
    if (!cfg) return;
    pthread_mutex_lock(&s_cfg_mutex);
    s_cfg = *cfg;
    pthread_mutex_unlock(&s_cfg_mutex);
    atomic_store_explicit(&s_reset_proc_history, 1, memory_order_release);
}
