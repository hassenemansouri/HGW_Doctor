/**
 * @file analyzer.c
 * @brief Anomaly detection based on metric history.
 */

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "analyzer.h"
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
static volatile int      s_stop = 0;

/* History buffers for each metric type */
#define HISTORY_MAX 300  /* enough for 5 minutes at 1s interval, adjust as needed */
static struct {
    uint32_t sys_cpu[HISTORY_MAX];
    uint32_t sys_mem[HISTORY_MAX];
    /* per-process histories */
    uint32_t proc_cpu[HGW_MAX_PROC_LIST][HISTORY_MAX];
    uint32_t proc_mem[HGW_MAX_PROC_LIST][HISTORY_MAX];
    int      proc_alive[HGW_MAX_PROC_LIST][HISTORY_MAX]; /* 1=alive, 0=dead */
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
        s_history.proc_cpu[i][s_history.idx]   = snap->procs[i].cpu_pct;
        s_history.proc_mem[i][s_history.idx]   = snap->procs[i].mem_pct;
        s_history.proc_alive[i][s_history.idx] = snap->procs[i].alive ? 1 : 0;
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

/* -------------------------------------------------------------------------
 * Analyzer thread
 * ------------------------------------------------------------------------- */
static void *analyzer_thread(void *arg) {
    (void)arg;
    LOG_INFO("Analyzer thread started");

    int required_samples =
        (int) ((s_cfg.threshold_duration_s + s_cfg.poll_interval_s - 1) / s_cfg.poll_interval_s);
    if (required_samples < 1) required_samples = 1;

    struct timespec last_seen = {0};
    int have_last_seen = 0;
    int cpu_alert_active = 0;
    int mem_alert_active = 0;
    int proc_dead_alert[HGW_MAX_PROC_LIST] = {0};
    int proc_cpu_alert[HGW_MAX_PROC_LIST]  = {0};
    int proc_mem_alert[HGW_MAX_PROC_LIST]  = {0};

    while (!s_stop) {
        /* Wait for new data – simple polling every 100ms */
        usleep(100000);

        /* Read the latest snapshot from circular buffer (non-destructive) */
        MetricSnapshot snap;
        if (!monitor_peek_latest(&snap)) continue;  /* buffer empty */
        if (have_last_seen && timespec_equal(&last_seen, &snap.ts)) continue;

        last_seen = snap.ts;
        have_last_seen = 1;

        history_push(&snap);

        /* --- System-wide CPU --- */
        if (snap.sys_cpu_pct >= s_cfg.cpu_threshold_pct) {
            if (!cpu_alert_active &&
                sustained_threshold(s_history.sys_cpu, s_history.count,
                                    s_cfg.cpu_threshold_pct, required_samples)) {
                AnomalyEvent ev = {
                    .type = ANOMALY_CPU,
                    .metric_value = snap.sys_cpu_pct,
                    .duration_s = s_cfg.threshold_duration_s
                };
                clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                if (s_callback) s_callback(&ev, s_userdata);
                cpu_alert_active = 1;
            }
        } else {
            cpu_alert_active = 0;
        }

        /* --- System-wide Memory --- */
        if (snap.sys_mem_pct >= s_cfg.mem_threshold_pct) {
            if (!mem_alert_active &&
                sustained_threshold(s_history.sys_mem, s_history.count,
                                    s_cfg.mem_threshold_pct, required_samples)) {
                AnomalyEvent ev = {
                    .type = ANOMALY_MEMORY,
                    .metric_value = snap.sys_mem_pct,
                    .duration_s = s_cfg.threshold_duration_s
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
                        .duration_s = s_cfg.threshold_duration_s
                    };
                    strncpy(ev.process_name, ps->name, HGW_MAX_PROC_NAME - 1);
                    clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                    if (s_callback) s_callback(&ev, s_userdata);
                    proc_dead_alert[i] = 1;
                }
                proc_cpu_alert[i] = 0;
                proc_mem_alert[i] = 0;
            } else {
                proc_dead_alert[i] = 0;

                /* Per-process CPU */
                if (ps->cpu_pct >= s_cfg.cpu_threshold_pct) {
                    if (!proc_cpu_alert[i] &&
                        sustained_threshold(s_history.proc_cpu[i], s_history.count,
                                            s_cfg.cpu_threshold_pct, required_samples)) {
                        AnomalyEvent ev = {
                            .type = ANOMALY_PROCESS_CPU,
                            .metric_value = ps->cpu_pct,
                            .duration_s = s_cfg.threshold_duration_s
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
                if (ps->mem_pct >= s_cfg.mem_threshold_pct) {
                    if (!proc_mem_alert[i] &&
                        sustained_threshold(s_history.proc_mem[i], s_history.count,
                                            s_cfg.mem_threshold_pct, required_samples)) {
                        AnomalyEvent ev = {
                            .type = ANOMALY_PROCESS_MEM,
                            .metric_value = ps->mem_pct,
                            .duration_s = s_cfg.threshold_duration_s
                        };
                        strncpy(ev.process_name, ps->name, HGW_MAX_PROC_NAME - 1);
                        clock_gettime(CLOCK_REALTIME, &ev.detected_at);
                        if (s_callback) s_callback(&ev, s_userdata);
                        proc_mem_alert[i] = 1;
                    }
                } else {
                    proc_mem_alert[i] = 0;
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
    s_stop = 0;
    memset(&s_history, 0, sizeof(s_history));
    return 0;
}

int analyzer_start(void) {
    if (!s_buf) return -1;
    return pthread_create(&s_thread, NULL, analyzer_thread, NULL);
}

void analyzer_stop(void) {
    s_stop = 1;
    pthread_join(s_thread, NULL);
}
