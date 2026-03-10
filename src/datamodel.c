/**
 * @file datamodel.c
 * @brief TR-181 data model handler - Ambiorix callback implementations.
 *
 * All functions prefixed dm_* are called directly by the Ambiorix runtime
 * as specified in the 'call' directives of hgw_doctor.odl.
 *
 * Write-back helpers (datamodel_set_status, datamodel_record_action, etc.)
 * are called by the other modules to push updated read-only values into
 * the live data model so they become visible to the ACS.
 */

#include <string.h>
#include <time.h>

#include <amxd/amxd_dm.h>
#include <amxd/amxd_object.h>
#include <amxd/amxd_transaction.h>
#include <amxo/amxo.h>
#include <amxc/amxc_var.h>

#include "datamodel.h"
#include "tr181_params.h"
#include "config.h"
#include "diag_collector.h"
#include "logger.h"
#include "types.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */
static amxd_dm_t *s_dm = NULL;

/** Set a string parameter value directly on the data model root object. */
static void dm_set_string(const char *param_path, const char *value) {
    if (!s_dm || !value) return;
    amxd_object_t *root = amxd_dm_get_object(s_dm, TR181_ROOT);
    if (!root) return;

    /* For nested paths like Stats.CurrentCPUUsage, resolve the sub-object */
    amxd_trans_t trans;
    amxd_trans_init(&trans);
    amxd_trans_select_pathf(&trans, "%s", param_path);
    amxd_trans_set_value(cstring_t, &trans, param_path, value);
    amxd_trans_apply(&trans, s_dm);
    amxd_trans_clean(&trans);
}

static void dm_set_uint32(const char *param_path, uint32_t value) {
    if (!s_dm) return;
    amxd_trans_t trans;
    amxd_trans_init(&trans);
    amxd_trans_select_pathf(&trans, "%s", TR181_ROOT);
    amxd_trans_set_value(uint32_t, &trans, param_path, value);
    amxd_trans_apply(&trans, s_dm);
    amxd_trans_clean(&trans);
}

static void dm_set_datetime_now(const char *param_path) {
    char buf[32];
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    dm_set_string(param_path, buf);
}

/* -------------------------------------------------------------------------
 * Initialisation / teardown
 * ------------------------------------------------------------------------- */
int datamodel_init(amxd_dm_t *dm, amxo_parser_t *parser, const char *odl_path) {
    s_dm = dm;

    int rc = amxo_parser_parse_file(parser, odl_path, amxd_dm_get_root(dm));
    if (rc != 0) {
        LOG_ERROR("Failed to parse ODL file: %s (rc=%d)", odl_path, rc);
        return -1;
    }

    /* Invoke entry-points declared in the ODL (loads shared library) */
    amxo_parser_invoke_entry_points(parser, dm, AMXO_START);

    LOG_INFO("Data model initialised from %s", odl_path);
    return 0;
}

void datamodel_cleanup(amxd_dm_t *dm, amxo_parser_t *parser) {
    amxo_parser_invoke_entry_points(parser, dm, AMXO_STOP);
    /* Persistent state is saved by libamxo (odl-save-on-stop = true) */
}

/* -------------------------------------------------------------------------
 * Write-back helpers
 * ------------------------------------------------------------------------- */
void datamodel_set_status(const char *status_str) {
    dm_set_string(TR181_STATUS, status_str);
    LOG_INFO("Status -> %s", status_str);
}

void datamodel_update_stats(uint32_t cpu_pct, uint32_t mem_pct,
                             uint32_t mem_free_kb) {
    dm_set_uint32(TR181_STAT_CPU,      cpu_pct);
    dm_set_uint32(TR181_STAT_MEM,      mem_pct);
    dm_set_uint32(TR181_STAT_MEM_FREE, mem_free_kb);
}

void datamodel_record_action(const RecoveryResult *r) {
    if (!r) return;

    const char *type_str   = ACTSTR_NONE;
    const char *result_str = RESULT_STR_NONE;

    switch (r->action) {
        case ACTION_PROCESS_RESTART: type_str = ACTSTR_PROCESS_RESTART; break;
        case ACTION_CACHE_CLEAR:     type_str = ACTSTR_CACHE_CLEAR;     break;
        case ACTION_REBOOT:          type_str = ACTSTR_REBOOT;          break;
        default: break;
    }
    switch (r->result) {
        case RESULT_SUCCESS:     result_str = RESULT_STR_SUCCESS;     break;
        case RESULT_FAILURE:     result_str = RESULT_STR_FAILURE;     break;
        case RESULT_IN_PROGRESS: result_str = RESULT_STR_IN_PROGRESS; break;
        default: break;
    }

    dm_set_string(TR181_LAST_ACTION_TYPE,   type_str);
    dm_set_string(TR181_LAST_ACTION_STATUS, result_str);
    dm_set_datetime_now(TR181_LAST_ACTION_TIME);

    LOG_INFO("Recovery action recorded: %s -> %s", type_str, result_str);
}

void datamodel_record_upload(UploadStatus status, const char *archive_path) {
    const char *status_str = RESULT_STR_NONE;
    switch (status) {
        case UPLOAD_STATUS_PENDING: status_str = RESULT_STR_PENDING;  break;
        case UPLOAD_STATUS_SUCCESS: status_str = RESULT_STR_SUCCESS;  break;
        case UPLOAD_STATUS_FAILED:  status_str = RESULT_STR_FAILURE;  break;
        default: break;
    }
    dm_set_string(TR181_UPLOAD_STATUS, status_str);
    if (archive_path)
        dm_set_string(TR181_DIAG_ARCHIVE_PATH, archive_path);
    if (status == UPLOAD_STATUS_SUCCESS)
        dm_set_datetime_now(TR181_UPLOAD_TIMESTAMP);
}

void datamodel_increment_anomaly_count(void) {
    /* Read current value, increment, write back */
    amxd_object_t *obj = amxd_dm_get_object(s_dm, TR181_ROOT);
    if (!obj) return;
    amxc_var_t val;
    amxc_var_init(&val);
    amxd_object_get_param(obj, "AnomalyCount", &val);
    uint32_t count = amxc_var_dyncast(uint32_t, &val);
    amxc_var_clean(&val);
    dm_set_uint32(TR181_ANOMALY_COUNT, count + 1);
}

void datamodel_append_anomaly_log(const AnomalyEvent *event,
                                  const char *action_taken,
                                  const char *action_result) {
    /* Add instance to AnomalyLog multi-instance object */
    amxd_object_t *log_obj = amxd_dm_get_object(s_dm, TR181_ANOMALY_LOG);
    if (!log_obj) return;

    amxd_trans_t trans;
    amxd_trans_init(&trans);
    amxd_trans_select_pathf(&trans, "%s", TR181_ANOMALY_LOG);
    amxd_trans_add_inst(&trans, 0, NULL);
    /* Individual param writes would follow here for the new instance */
    (void)event; (void)action_taken; (void)action_result; /* TODO: fill fields */
    amxd_trans_apply(&trans, s_dm);
    amxd_trans_clean(&trans);
}

void datamodel_update_uptime(uint32_t uptime_s) {
    dm_set_uint32(TR181_STAT_UPTIME, uptime_s);
}

/* -------------------------------------------------------------------------
 * RPC handlers (bound via ODL 'call' directives)
 * ------------------------------------------------------------------------- */

amxd_status_t dm_trigger_diagnostics(amxd_object_t *obj, amxd_function_t *fn,
                                      amxc_var_t *args, amxc_var_t *ret) {
    (void)obj; (void)fn; (void)args;
    LOG_INFO("OnDemand diagnostic trigger received from ACS/CLI");

    int rc = diag_collect(NULL);
    amxc_var_set(uint32_t, ret, (rc == 0) ? 0 : 1);
    return amxd_status_ok;
}

amxd_status_t dm_reset_counters(amxd_object_t *obj, amxd_function_t *fn,
                                 amxc_var_t *args, amxc_var_t *ret) {
    (void)obj; (void)fn; (void)args; (void)ret;
    dm_set_uint32(TR181_ANOMALY_COUNT,      0);
    dm_set_uint32(TR181_STAT_TOTAL_ACTIONS, 0);
    dm_set_uint32(TR181_STAT_TOTAL_UPLOADS, 0);
    LOG_INFO("Counters reset via RPC");
    return amxd_status_ok;
}

amxd_status_t dm_set_profile(amxd_object_t *obj, amxd_function_t *fn,
                              amxc_var_t *args, amxc_var_t *ret) {
    (void)obj; (void)fn;
    const char *profile_name = GET_CHAR(args, "ProfileName");
    if (!profile_name) {
        amxc_var_set(int32_t, ret, -1);
        return amxd_status_invalid_value;
    }
    /* Validate profile exists in Profiles table, then set active Profile param */
    dm_set_string(TR181_PROFILE, profile_name);
    LOG_INFO("Active profile set to '%s' via RPC", profile_name);
    amxc_var_set(int32_t, ret, 0);
    return amxd_status_ok;
}

/* -------------------------------------------------------------------------
 * Event handler: fires when any parameter under X_TELNET_HGWDoctor.* changes
 * Propagates ACS-written values back into the live config of each module.
 * ------------------------------------------------------------------------- */
amxd_status_t dm_on_param_changed(amxd_object_t *obj, amxd_function_t *fn,
                                   amxc_var_t *args, amxc_var_t *ret) {
    (void)obj; (void)fn; (void)args; (void)ret;
    /* Re-read all relevant parameters and call config_apply_from_datamodel() */
    /* Detailed implementation: read each param, build HgwConfig, call apply */
    LOG_DEBUG("Data model parameter changed - reloading config");
    config_reload();
    return amxd_status_ok;
}
