/**
 * @file hgwd_dm_methods.c
 * @brief Data model action callbacks bound in the ODL via <!import:...!>.
 *
 * TriggerRecovery(%in string type)
 *   Manually fires a CPU or memory anomaly signal so the recovery module
 *   executes the configured action immediately.
 *
 * ResetHealth()
 *   Clears all sustained-threshold counters and sets Health back to Good.
 */

#include <string.h>

#include "hgwdoctor.h"

/* ── TriggerRecovery ─────────────────────────────────────────────────────── */

amxd_status_t hgwd_dm_trigger_recovery(amxd_object_t *obj, amxd_function_t *fn,
                                        amxc_var_t *args, amxc_var_t *ret) {
    hgwdoctor_t *hgwd = hgwdoctor_get();
    const char *type;
    amxc_var_t data;

    (void)obj;
    (void)fn;
    (void)ret;

    type = GET_CHAR(args, "type");
    when_null_trace(type, exit_err, WARNING,
                    "TriggerRecovery: missing 'type' argument");

    SAH_TRACEZ_INFO(ME, "Manual recovery triggered via DM: type=%s", type);

    amxc_var_init(&data);
    amxc_var_set(cstring_t, &data, type);

    if(strcmp(type, "cpu") == 0) {
        amxp_sigmngr_emit_signal(&hgwd->sigmngr, SIG_ANOMALY_CPU, &data);
    } else if(strcmp(type, "mem") == 0) {
        amxp_sigmngr_emit_signal(&hgwd->sigmngr, SIG_ANOMALY_MEM, &data);
    } else {
        SAH_TRACEZ_WARNING(ME, "TriggerRecovery: unknown type '%s'", type);
        amxc_var_clean(&data);
        goto exit_err;
    }

    amxc_var_clean(&data);
    return amxd_status_ok;

exit_err:
    return amxd_status_invalid_value;
}

/* ── ResetHealth ─────────────────────────────────────────────────────────── */

amxd_status_t hgwd_dm_reset_health(amxd_object_t *obj, amxd_function_t *fn,
                                    amxc_var_t *args, amxc_var_t *ret) {
    hgwdoctor_t *hgwd = hgwdoctor_get();
    amxd_trans_t trans;
    amxd_status_t status;

    (void)obj;
    (void)fn;
    (void)args;
    (void)ret;

    /* Reset in-memory counters */
    hgwd->cpu_high_count   = 0;
    hgwd->mem_high_count   = 0;
    hgwd->cpu_alert_active = false;
    hgwd->mem_alert_active = false;

    /* Reset Health in the data model */
    amxd_trans_init(&trans);
    amxd_trans_select_pathf(&trans, "GatewayHealth");
    amxd_trans_set_value(cstring_t, &trans, "Health", HEALTH_GOOD);
    status = amxd_trans_apply(&trans, hgwd->dm);
    amxd_trans_clean(&trans);

    if(status != amxd_status_ok) {
        SAH_TRACEZ_WARNING(ME, "ResetHealth transaction failed (status=%d)", status);
        return amxd_status_unknown_error;
    }

    SAH_TRACEZ_INFO(ME, "Health reset to Good");
    return amxd_status_ok;
}
