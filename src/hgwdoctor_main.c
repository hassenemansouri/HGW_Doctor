/**
 * @file hgwdoctor_main.c
 * @brief HGW-Doctor plugin entry point.
 *
 * Implements _hgwdoctor_main() called by amxrt:
 *   reason 0 (AMXO_START) — initialise singleton, register signals,
 *                            connect recovery slots, start poll timer.
 *   reason 1 (AMXO_STOP)  — stop timer, disconnect slots, clean up.
 *
 * The singleton `hgwdoctor` is the only global state; all other modules
 * receive a pointer to it.
 */

#include <string.h>

#include "hgwdoctor.h"

/* ── Singleton ───────────────────────────────────────────────────────────── */
static hgwdoctor_t hgwdoctor;

hgwdoctor_t *hgwdoctor_get(void) {
    return &hgwdoctor;
}

/* ── Poll timer callback ─────────────────────────────────────────────────── */
static void poll_timer_cb(amxp_timer_t *timer, void *priv) {
    hgwdoctor_t *hgwd = (hgwdoctor_t *)priv;
    amxd_trans_t trans;
    amxd_status_t status;

    /* 1. Read fresh /proc metrics into the singleton */
    hgwd_metrics_read_cpu(&hgwd->prev_cpu_total, &hgwd->prev_cpu_idle,
                           &hgwd->cpu_pct);
    hgwd_metrics_read_mem(&hgwd->mem_pct, &hgwd->mem_free_kb);

    /* 2. Push live values into the data model via transaction */
    amxd_trans_init(&trans);
    amxd_trans_select_pathf(&trans, "GatewayHealth");
    amxd_trans_set_value(uint32_t, &trans, "CpuUsage", hgwd->cpu_pct);
    amxd_trans_set_value(uint32_t, &trans, "MemUsage", hgwd->mem_pct);
    status = amxd_trans_apply(&trans, hgwd->dm);
    if(status != amxd_status_ok) {
        SAH_TRACEZ_WARNING(ME, "Failed to update live metrics (status=%d)", status);
    }
    amxd_trans_clean(&trans);

    /* 3. Check sustained thresholds, update Health, emit anomaly signals */
    hgwd_events_check(hgwd);

    /* 4. Re-arm one-shot timer for next tick */
    amxp_timer_start(timer, HGWD_POLL_INTERVAL_MS);
}

/* ── Plugin entry point ──────────────────────────────────────────────────── */
int _hgwdoctor_main(int reason, amxd_dm_t *dm, amxo_parser_t *parser) {
    hgwdoctor_t *hgwd = &hgwdoctor;

    if(reason == 0) {
        /* START */
        memset(hgwd, 0, sizeof(*hgwd));
        hgwd->dm     = dm;
        hgwd->parser = parser;

        /* Init internal signal bus and register anomaly signal names */
        amxp_sigmngr_init(&hgwd->sigmngr);
        hgwd_events_init(hgwd);

        /* Connect recovery action slots to anomaly signals */
        hgwd_recovery_init(hgwd);

        /* Start the poll timer (one-shot; re-armed in callback) */
        amxp_timer_new(&hgwd->poll_timer, poll_timer_cb, hgwd);
        amxp_timer_start(hgwd->poll_timer, HGWD_POLL_INTERVAL_MS);

        SAH_TRACEZ_INFO(ME, "HGW-Doctor started (poll=%dms, sustained=%d ticks)",
                        HGWD_POLL_INTERVAL_MS, HGWD_SUSTAINED_SAMPLES);
    } else {
        /* STOP */
        amxp_timer_stop(hgwd->poll_timer);
        amxp_timer_delete(&hgwd->poll_timer);

        hgwd_recovery_cleanup(hgwd);
        amxp_sigmngr_clean(&hgwd->sigmngr);

        SAH_TRACEZ_INFO(ME, "HGW-Doctor stopped");
    }

    return 0;
}
