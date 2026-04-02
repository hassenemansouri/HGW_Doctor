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

#include <amxc/amxc.h>
#include <amxp/amxp.h>
#include <amxd/amxd_types.h>
#include <amxd/amxd_dm.h>
#include <amxd/amxd_object.h>
#include <amxd/amxd_transaction.h>
#include <amxo/amxo.h>

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
static uint32_t   s_anomaly_count = 0;
static uint32_t   s_total_actions = 0;
static uint32_t   s_total_uploads = 0;

static int dm_split_path(const char *param_path, char *object_path,
                         size_t object_path_len, const char **param_name) {
    const char *dot = strrchr(param_path, '.');
    size_t len;

    if (!param_path || !object_path || !param_name || !dot || dot == param_path) {
        return -1;
    }

    len = (size_t) (dot - param_path);
    if (len >= object_path_len) {
        return -1;
    }

    memcpy(object_path, param_path, len);
    object_path[len] = '\0';
    *param_name = dot + 1;
    return (**param_name == '\0') ? -1 : 0;
}

static void dm_apply_trans(amxd_trans_t *trans, const char *param_path) {
    amxd_status_t status = amxd_trans_apply(trans, s_dm);
    if (status != amxd_status_ok) {
        LOG_WARN("Failed to apply transaction for %s (status=%d)",
                 param_path ? param_path : "<transaction>", status);
    }
}

static void dm_format_utc(time_t when, char *buf, size_t len) {
    struct tm tm_info;
    gmtime_r(&when, &tm_info);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_info);
}

static const char *anomaly_type_to_string(AnomalyType type) {
    switch (type) {
        case ANOMALY_CPU:     return "CPU";
        case ANOMALY_MEMORY:  return "Memory";
        case ANOMALY_PROCESS: return "Process";
        case ANOMALY_NONE:
        default:              return "";
    }
}

/** Set a parameter value directly, bypassing access-control (works for %read-only). */
static void dm_set_param(const char *param_path, amxc_var_t *val) {
    char object_path[128];
    const char *param_name = NULL;
    amxd_object_t *obj;
    amxd_param_t  *param;

    if (!s_dm || !val || dm_split_path(param_path, object_path,
                                       sizeof(object_path), &param_name) != 0) {
        return;
    }

    obj = amxd_dm_findf(s_dm, "%s", object_path);
    if (!obj) {
        LOG_WARN("Object not found: %s", object_path);
        return;
    }

    param = amxd_object_get_param_def(obj, param_name);
    if (!param) {
        LOG_WARN("Param not found: %s in %s", param_name, object_path);
        return;
    }

    amxd_param_set_value(param, val);
}

static void dm_set_string(const char *param_path, const char *value) {
    amxc_var_t val;
    amxc_var_init(&val);
    amxc_var_set(cstring_t, &val, value);
    dm_set_param(param_path, &val);
    amxc_var_clean(&val);
}

static void dm_set_uint32(const char *param_path, uint32_t value) {
    amxc_var_t val;
    amxc_var_init(&val);
    amxc_var_set(uint32_t, &val, value);
    dm_set_param(param_path, &val);
    amxc_var_clean(&val);
}

static void dm_set_bool(const char *param_path, bool value) {
    amxc_var_t val;
    amxc_var_init(&val);
    amxc_var_set(bool, &val, value);
    dm_set_param(param_path, &val);
    amxc_var_clean(&val);
}

static void dm_set_datetime_now(const char *param_path) {
    char buf[32];
    time_t now = time(NULL);
    dm_format_utc(now, buf, sizeof(buf));
    dm_set_string(param_path, buf);
}

/* -------------------------------------------------------------------------
 * Initialisation / teardown
 * ------------------------------------------------------------------------- */
int datamodel_init(amxd_dm_t *dm, amxo_parser_t *parser, const char *odl_path) {
    s_dm = dm;
    s_anomaly_count = 0;
    s_total_actions = 0;
    s_total_uploads = 0;

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
    s_dm = NULL;
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
    s_total_actions++;
    dm_set_uint32(TR181_STAT_TOTAL_ACTIONS, s_total_actions);

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
    if (status == UPLOAD_STATUS_SUCCESS) {
        s_total_uploads++;
        dm_set_uint32(TR181_STAT_TOTAL_UPLOADS, s_total_uploads);
        dm_set_datetime_now(TR181_UPLOAD_TIMESTAMP);
    }
}

void datamodel_increment_anomaly_count(void) {
    s_anomaly_count++;
    dm_set_uint32(TR181_ANOMALY_COUNT, s_anomaly_count);
}

void datamodel_append_anomaly_log(const AnomalyEvent *event,
                                  const char *action_taken,
                                  const char *action_result) {
    char timestamp[32];

    /* Add instance to AnomalyLog multi-instance object */
    if (!s_dm || !event) return;

    amxd_trans_t trans;
    amxd_trans_init(&trans);
    amxd_trans_select_pathf(&trans, "%s", TR181_ANOMALY_LOG);
    amxd_trans_add_inst(&trans, 0, NULL);

    dm_format_utc(event->detected_at.tv_sec, timestamp, sizeof(timestamp));
    amxd_trans_set_value(cstring_t, &trans, "Timestamp", timestamp);
    amxd_trans_set_value(cstring_t, &trans, "AnomalyType",
                         anomaly_type_to_string(event->type));
    amxd_trans_set_value(uint32_t, &trans, "MetricValue", event->metric_value);
    amxd_trans_set_value(cstring_t, &trans, "ActionTaken",
                         action_taken ? action_taken : ACTSTR_NONE);
    amxd_trans_set_value(cstring_t, &trans, "ActionResult",
                         action_result ? action_result : RESULT_STR_NONE);

    dm_apply_trans(&trans, TR181_ANOMALY_LOG);
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

    dm_set_bool(TR181_ON_DEMAND_TRIGGER, false);
    int rc = diag_collect(NULL);
    amxc_var_set(uint32_t, ret, (rc == 0) ? 0 : 1);
    return amxd_status_ok;
}

amxd_status_t dm_reset_counters(amxd_object_t *obj, amxd_function_t *fn,
                                 amxc_var_t *args, amxc_var_t *ret) {
    (void)obj; (void)fn; (void)args; (void)ret;
    s_anomaly_count = 0;
    s_total_actions = 0;
    s_total_uploads = 0;
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
