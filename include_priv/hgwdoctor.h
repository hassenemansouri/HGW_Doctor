#ifndef HGWDOCTOR_H
#define HGWDOCTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#include <amxc/amxc.h>
#include <amxp/amxp.h>
#include <amxd/amxd_dm.h>
#include <amxd/amxd_object.h>
#include <amxd/amxd_transaction.h>
#include <amxd/amxd_function.h>
#include <amxo/amxo.h>
#include <debug/sahtrace.h>

/* ── Logging module name ─────────────────────────────────────────────────── */
#define ME "hgwdoctor"

/* ── Health strings ──────────────────────────────────────────────────────── */
#define HEALTH_GOOD      "Good"
#define HEALTH_WARNING   "Warning"
#define HEALTH_CRITICAL  "Critical"
#define HEALTH_UNKNOWN   "Unknown"

/* ── Recovery action strings (match ODL enum values) ────────────────────── */
#define RECOVERY_NO_ACTION    "NO_ACTION"
#define RECOVERY_KILL_TOP     "KILL_TOP"
#define RECOVERY_DROP_CACHES  "DROP_CACHES"
#define RECOVERY_REBOOT       "REBOOT"
#define RECOVERY_RESTART      "RESTART"

/* ── Internal signal names for anomaly events ───────────────────────────── */
#define SIG_ANOMALY_CPU      "anomaly-cpu"
#define SIG_ANOMALY_MEM      "anomaly-mem"
#define SIG_ANOMALY_PROCESS  "anomaly-process"

/* ── Polling defaults ────────────────────────────────────────────────────── */
#define HGWD_POLL_INTERVAL_MS   5000   /* 5 s between /proc samples          */
#define HGWD_SUSTAINED_SAMPLES  3      /* consecutive ticks above threshold  */

/* ── Convenience macros ──────────────────────────────────────────────────── */

/* Check pointer; trace at LEVEL and goto label if NULL. */
#define when_null_trace(x, label, level, ...) \
    do { \
        if ((x) == NULL) { \
            SAH_TRACEZ_##level(ME, __VA_ARGS__); \
            goto label; \
        } \
    } while(0)

/* ── Singleton state ─────────────────────────────────────────────────────── */
typedef struct {
    amxd_dm_t       *dm;
    amxo_parser_t   *parser;

    amxp_timer_t    *poll_timer;   /* fires every HGWD_POLL_INTERVAL_MS      */
    amxp_sigmngr_t   sigmngr;      /* internal anomaly signal bus            */

    /* Last sampled values (written by poll timer, read by events/recovery) */
    uint32_t         cpu_pct;
    uint32_t         mem_pct;
    uint32_t         mem_free_kb;

    /* /proc/stat delta state */
    unsigned long long prev_cpu_total;
    unsigned long long prev_cpu_idle;

    /* Sustained-threshold counters and latch flags */
    int              cpu_high_count;
    int              mem_high_count;
    bool             cpu_alert_active;
    bool             mem_alert_active;
} hgwdoctor_t;

/* ── Global singleton accessor (defined in hgwdoctor_main.c) ────────────── */
hgwdoctor_t *hgwdoctor_get(void);

/* ── hgwd_metrics.c ──────────────────────────────────────────────────────── */
int   hgwd_metrics_read_cpu(unsigned long long *prev_total,
                             unsigned long long *prev_idle,
                             uint32_t *out_pct);
int   hgwd_metrics_read_mem(uint32_t *out_pct, uint32_t *out_free_kb);
bool  hgwd_metrics_proc_alive(const char *name, pid_t *out_pid);
pid_t hgwd_metrics_read_pidfile(const char *path);

/* ── hgwd_events.c ───────────────────────────────────────────────────────── */
void hgwd_events_init(hgwdoctor_t *hgwd);
void hgwd_events_check(hgwdoctor_t *hgwd);

/* ── hgwd_recovery.c ─────────────────────────────────────────────────────── */
void hgwd_recovery_init(hgwdoctor_t *hgwd);
void hgwd_recovery_cleanup(hgwdoctor_t *hgwd);

/* ── hgwd_dm_methods.c  (bound via ODL <!import:...!> directives) ────────── */
amxd_status_t hgwd_dm_trigger_recovery(amxd_object_t *obj, amxd_function_t *fn,
                                        amxc_var_t *args, amxc_var_t *ret);
amxd_status_t hgwd_dm_reset_health(amxd_object_t *obj, amxd_function_t *fn,
                                    amxc_var_t *args, amxc_var_t *ret);

#endif /* HGWDOCTOR_H */
