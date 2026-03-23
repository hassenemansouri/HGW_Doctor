/**
 * @file hgwd_events.c
 * @brief Anomaly detection and event emission via amxp_sigmngr.
 *
 * hgwd_events_init()  — registers the three anomaly signals on the singleton's
 *                        signal manager.
 * hgwd_events_check() — called on every poll tick; compares sampled metrics
 *                        against DM thresholds, maintains sustained-high
 *                        counters, emits signals when thresholds are crossed,
 *                        and updates the GatewayHealth.Health string.
 *                        Also iterates WatchedProcess[] instances and checks
 *                        each process for liveness.
 */

#include <signal.h>
#include <string.h>

#include "hgwdoctor.h"

/* ── Internal helpers ────────────────────────────────────────────────────── */

static uint32_t dm_get_uint32(hgwdoctor_t *hgwd, const char *param) {
    amxd_object_t *obj;
    amxc_var_t var;
    uint32_t result = 0;

    obj = amxd_dm_findf(hgwd->dm, "GatewayHealth");
    when_null_trace(obj, exit, WARNING, "GatewayHealth object not found");

    amxc_var_init(&var);
    if(amxd_object_get_param(obj, param, &var) == amxd_status_ok) {
        result = amxc_var_dyncast(uint32_t, &var);
    }
    amxc_var_clean(&var);

exit:
    return result;
}

static void dm_set_health(hgwdoctor_t *hgwd, const char *health_str) {
    amxd_trans_t trans;
    amxd_status_t status;

    amxd_trans_init(&trans);
    amxd_trans_select_pathf(&trans, "GatewayHealth");
    amxd_trans_set_value(cstring_t, &trans, "Health", health_str);
    status = amxd_trans_apply(&trans, hgwd->dm);
    if(status != amxd_status_ok) {
        SAH_TRACEZ_WARNING(ME, "Failed to set Health=%s (status=%d)",
                           health_str, status);
    }
    amxd_trans_clean(&trans);
}

static void dm_set_instance_health(hgwdoctor_t *hgwd, amxd_object_t *inst,
                                    const char *health_str) {
    amxd_trans_t trans;
    amxd_status_t status;
    const char *path;

    path = amxd_object_get_path(inst, AMXD_OBJECT_NAMED);
    when_null_trace(path, exit, WARNING, "Cannot get instance path");

    amxd_trans_init(&trans);
    amxd_trans_select_pathf(&trans, "%s", path);
    amxd_trans_set_value(cstring_t, &trans, "Health", health_str);
    status = amxd_trans_apply(&trans, hgwd->dm);
    if(status != amxd_status_ok) {
        SAH_TRACEZ_WARNING(ME, "Failed to set WatchedProcess Health (status=%d)",
                           status);
    }
    amxd_trans_clean(&trans);

exit:
    return;
}

/* ── WatchedProcess liveness ─────────────────────────────────────────────── */

static void check_watched_processes(hgwdoctor_t *hgwd) {
    amxd_object_t *wp_tmpl;
    amxc_var_t var;

    wp_tmpl = amxd_dm_findf(hgwd->dm, "GatewayHealth.WatchedProcess");
    when_null_trace(wp_tmpl, exit, WARNING, "WatchedProcess template not found");

    amxc_var_init(&var);

    amxd_object_for_each(instance, it, wp_tmpl) {
        amxd_object_t *inst = amxd_object_from_it(it);
        pid_t pid = -1;
        bool alive = false;
        const char *pidfile = NULL;
        const char *proc_name = NULL;
        const char *action = RECOVERY_NO_ACTION;

        /* Try PidFile first */
        amxc_var_clean(&var);
        if(amxd_object_get_param(inst, "PidFile", &var) == amxd_status_ok) {
            pidfile = amxc_var_constcast(cstring_t, &var);
        }

        if(pidfile && pidfile[0] != '\0') {
            pid   = hgwd_metrics_read_pidfile(pidfile);
            alive = (pid > 0) && (kill(pid, 0) == 0);
        } else {
            /* Fall back to scanning /proc by Name */
            amxc_var_clean(&var);
            if(amxd_object_get_param(inst, "Name", &var) == amxd_status_ok) {
                proc_name = amxc_var_constcast(cstring_t, &var);
                alive = hgwd_metrics_proc_alive(proc_name, &pid);
            }
        }

        if(alive) {
            dm_set_instance_health(hgwd, inst, HEALTH_GOOD);
            continue;
        }

        /* Process is down — update health and emit anomaly signal */
        dm_set_instance_health(hgwd, inst, HEALTH_CRITICAL);

        amxc_var_clean(&var);
        if(amxd_object_get_param(inst, "RecoveryAction", &var) == amxd_status_ok) {
            action = amxc_var_constcast(cstring_t, &var);
        }

        SAH_TRACEZ_WARNING(ME, "WatchedProcess '%s' is down (action=%s)",
                           proc_name ? proc_name : "?", action);

        /* Build signal data: htable { RecoveryAction, ProcessName } */
        amxc_var_t data;
        amxc_var_init(&data);
        amxc_var_set_type(&data, AMXC_VAR_ID_HTABLE);
        amxc_var_add_key(cstring_t, &data, "RecoveryAction", action);
        amxc_var_add_key(cstring_t, &data, "ProcessName",
                         proc_name ? proc_name : "");
        amxp_sigmngr_emit_signal(&hgwd->sigmngr, SIG_ANOMALY_PROCESS, &data);
        amxc_var_clean(&data);
    }

    amxc_var_clean(&var);
exit:
    return;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void hgwd_events_init(hgwdoctor_t *hgwd) {
    amxp_sigmngr_add_signal(&hgwd->sigmngr, SIG_ANOMALY_CPU);
    amxp_sigmngr_add_signal(&hgwd->sigmngr, SIG_ANOMALY_MEM);
    amxp_sigmngr_add_signal(&hgwd->sigmngr, SIG_ANOMALY_PROCESS);
    SAH_TRACEZ_INFO(ME, "Anomaly signals registered");
}

void hgwd_events_check(hgwdoctor_t *hgwd) {
    uint32_t cpu_thr = dm_get_uint32(hgwd, "CpuThreshold");
    uint32_t mem_thr = dm_get_uint32(hgwd, "MemThreshold");
    /* Start optimistic; degrade below */
    const char *health = HEALTH_GOOD;

    /* ── CPU ─────────────────────────────────────────────────────────────── */
    if(hgwd->cpu_pct >= cpu_thr) {
        hgwd->cpu_high_count++;

        if(!hgwd->cpu_alert_active &&
           hgwd->cpu_high_count >= HGWD_SUSTAINED_SAMPLES) {

            SAH_TRACEZ_WARNING(ME, "CPU anomaly: %u%% >= threshold %u%% "
                               "sustained %d ticks",
                               hgwd->cpu_pct, cpu_thr, hgwd->cpu_high_count);

            amxc_var_t data;
            amxc_var_init(&data);
            amxc_var_set(uint32_t, &data, hgwd->cpu_pct);
            amxp_sigmngr_emit_signal(&hgwd->sigmngr, SIG_ANOMALY_CPU, &data);
            amxc_var_clean(&data);

            hgwd->cpu_alert_active = true;
            health = HEALTH_CRITICAL;
        } else {
            /* Approaching but not yet sustained */
            if(hgwd->cpu_alert_active) {
                health = HEALTH_CRITICAL;
            } else {
                health = HEALTH_WARNING;
            }
        }
    } else {
        hgwd->cpu_high_count   = 0;
        hgwd->cpu_alert_active = false;
    }

    /* ── Memory ──────────────────────────────────────────────────────────── */
    if(hgwd->mem_pct >= mem_thr) {
        hgwd->mem_high_count++;

        if(!hgwd->mem_alert_active &&
           hgwd->mem_high_count >= HGWD_SUSTAINED_SAMPLES) {

            SAH_TRACEZ_WARNING(ME, "Memory anomaly: %u%% >= threshold %u%% "
                               "sustained %d ticks",
                               hgwd->mem_pct, mem_thr, hgwd->mem_high_count);

            amxc_var_t data;
            amxc_var_init(&data);
            amxc_var_set(uint32_t, &data, hgwd->mem_pct);
            amxp_sigmngr_emit_signal(&hgwd->sigmngr, SIG_ANOMALY_MEM, &data);
            amxc_var_clean(&data);

            hgwd->mem_alert_active = true;
            health = HEALTH_CRITICAL;
        } else {
            if(hgwd->mem_alert_active) {
                health = HEALTH_CRITICAL;
            } else if(strcmp(health, HEALTH_CRITICAL) != 0) {
                health = HEALTH_WARNING;
            }
        }
    } else {
        hgwd->mem_high_count   = 0;
        hgwd->mem_alert_active = false;
    }

    dm_set_health(hgwd, health);

    /* ── WatchedProcess liveness ─────────────────────────────────────────── */
    check_watched_processes(hgwd);
}
