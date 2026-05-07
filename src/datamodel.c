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

#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <syslog.h>

#include <amxc/amxc.h>
#include <amxp/amxp.h>
#include <amxd/amxd_types.h>
#include <amxd/amxd_dm.h>
#include <amxd/amxd_object.h>
#include <amxd/amxd_object_parameter.h>
#include <amxd/amxd_object_action.h>
#include <amxd/amxd_parameter_action.h>
#include <amxd/amxd_parameter.h>
#include <amxd/amxd_action.h>
#include <amxd/amxd_transaction.h>
#include <amxo/amxo.h>

#include "datamodel.h"
#include "tr181_params.h"
#include "types.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */
static amxd_dm_t         *s_dm = NULL;
static _Atomic uint32_t   s_anomaly_count = 0;
static _Atomic uint32_t   s_total_actions = 0;
static _Atomic uint32_t   s_total_uploads = 0;

static int dm_split_path(const char *param_path, char *object_path,
                         size_t object_path_len, char *param_name,
                         size_t param_name_len) {
    const char *dot = strrchr(param_path, '.');
    size_t obj_len, name_len;

    if (!param_path || !object_path || !param_name || !dot || dot == param_path) {
        return -1;
    }

    obj_len = (size_t)(dot - param_path);
    if (obj_len >= object_path_len) {
        return -1;
    }

    name_len = strlen(dot + 1);
    if (name_len == 0 || name_len >= param_name_len) {
        return -1;
    }

    memcpy(object_path, param_path, obj_len);
    object_path[obj_len] = '\0';
    memcpy(param_name, dot + 1, name_len);
    param_name[name_len] = '\0';
    return 0;
}

static void dm_apply_trans(amxd_trans_t *trans, const char *param_path) {
    amxd_status_t status = amxd_trans_apply(trans, s_dm);
    if (status != amxd_status_ok) {
        syslog(LOG_WARNING, "Failed to apply transaction for %s (status=%d)",
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
        case ANOMALY_CPU:         return "CPU";
        case ANOMALY_MEMORY:      return "Memory";
        case ANOMALY_PROCESS:     return "Process";
        case ANOMALY_PROCESS_CPU: return "ProcessCPU";
        case ANOMALY_PROCESS_MEM: return "ProcessMem";
        case ANOMALY_NONE:
        default:                  return "";
    }
}

static void dm_set_param(const char *param_path, amxc_var_t *val) {
    char object_path[HGW_MAX_PATH];
    char param_name[HGW_MAX_PROC_NAME];

    if (!s_dm || !val || dm_split_path(param_path, object_path,
                                       sizeof(object_path),
                                       param_name, sizeof(param_name)) != 0)
        return;

    /* amxd_dm_findf requires a trailing dot for sub-objects */
    size_t obj_len = strlen(object_path);
    if (obj_len > 0 && object_path[obj_len - 1] != '.' && obj_len + 1 < HGW_MAX_PATH) {
        object_path[obj_len]     = '.';
        object_path[obj_len + 1] = '\0';
    }

    amxd_object_t *obj = amxd_dm_findf(s_dm, "%s", object_path);
    if (!obj) {
        syslog(LOG_ERR, "dm_set_param: object not found for '%s'", object_path);
        return;
    }

    amxd_trans_t trans;
    amxd_trans_init(&trans);
    amxd_trans_set_attr(&trans, amxd_tattr_change_ro, true);
    amxd_trans_select_object(&trans, obj);
    amxd_trans_set_param(&trans, param_name, val);
    amxd_status_t st = amxd_trans_apply(&trans, s_dm);
    if (st != amxd_status_ok)
        syslog(LOG_ERR, "Failed to set param %s (status=%d)", param_path, st);
    amxd_trans_clean(&trans);
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

static uint32_t dm_read_uint32(const char *param_path) {
    char object_path[HGW_MAX_PATH];
    char param_name[HGW_MAX_PROC_NAME];
    amxc_var_t val;
    uint32_t result = 0;

    if (!s_dm) return 0;
    if (dm_split_path(param_path, object_path, sizeof(object_path),
                      param_name, sizeof(param_name)) != 0)
        return 0;

    size_t obj_len = strlen(object_path);
    if (obj_len > 0 && object_path[obj_len - 1] != '.' && obj_len + 1 < HGW_MAX_PATH) {
        object_path[obj_len]     = '.';
        object_path[obj_len + 1] = '\0';
    }

    amxd_object_t *obj = amxd_dm_findf(s_dm, "%s", object_path);
    if (!obj) return 0;
    amxc_var_init(&val);
    if (amxd_object_get_param(obj, param_name, &val) == amxd_status_ok)
        result = amxc_var_dyncast(uint32_t, &val);
    amxc_var_clean(&val);
    return result;
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
/* -------------------------------------------------------------------------
 * ------------------------------------------------------------------------- */
void datamodel_start_sync(void) {
    /* amxs sync removed — Device.X_TELNET_HGWDoctor is proxied by
     * the platform tr181-device mapping, not by amxs. */
}

int datamodel_init(amxd_dm_t *dm, amxo_parser_t *parser, const char *odl_path) {
    atomic_store(&s_anomaly_count, 0);
    atomic_store(&s_total_actions, 0);
    atomic_store(&s_total_uploads, 0);

    amxd_dm_init(dm);
    amxo_parser_init(parser);
    s_dm = dm;

    int rc = amxo_parser_parse_file(parser, odl_path, amxd_dm_get_root(dm));
    if (rc != 0) {
        syslog(LOG_ERR, "Failed to parse ODL file: %s (rc=%d)", odl_path, rc);
        return -1;
    }
    amxo_parser_invoke_entry_points(parser, dm, AMXO_START);

    /* libamxo has just restored %persistent values from disk — pull the saved
     * counter values back into the in-memory atomics so they survive restarts. */
    atomic_store(&s_anomaly_count, dm_read_uint32(TR181_ANOMALY_COUNT));
    atomic_store(&s_total_actions, dm_read_uint32(TR181_STAT_TOTAL_ACTIONS));
    atomic_store(&s_total_uploads, dm_read_uint32(TR181_STAT_TOTAL_UPLOADS));

    syslog(LOG_INFO, "Data model initialised from %s", odl_path);
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
    syslog(LOG_INFO, "Status -> %s", status_str);
}

void datamodel_set_process_list(const char *process_list) {
    if (!s_dm || !process_list || process_list[0] == '\0') return;
    dm_set_string(TR181_PROCESS_LIST, process_list);
}

void datamodel_sync_startup(const char *action_type, const char *upload_url,
                             const char *diag_output_dir) {
    if (action_type    && action_type[0])    dm_set_string(TR181_ACTION_TYPE,     action_type);
    if (upload_url     && upload_url[0])     dm_set_string(TR181_UPLOAD_URL,      upload_url);
    if (diag_output_dir && diag_output_dir[0]) dm_set_string(TR181_DIAG_OUTPUT_DIR, diag_output_dir);
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
    atomic_fetch_add(&s_total_actions, 1);
    /* Counter DM write deferred to datamodel_sync_counters() on main thread */

    syslog(LOG_INFO, "Recovery action recorded: %s -> %s", type_str, result_str);
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
        atomic_fetch_add(&s_total_uploads, 1);
        dm_set_datetime_now(TR181_UPLOAD_TIMESTAMP);
        /* Counter DM write deferred to datamodel_sync_counters() on main thread */
    }
}

void datamodel_increment_anomaly_count(void) {
    atomic_fetch_add(&s_anomaly_count, 1);
    /* DM write deferred — datamodel_sync_counters() writes from main thread */
}

void datamodel_sync_counters(void) {
    dm_set_uint32(TR181_ANOMALY_COUNT,      atomic_load(&s_anomaly_count));
    dm_set_uint32(TR181_STAT_TOTAL_ACTIONS, atomic_load(&s_total_actions));
    dm_set_uint32(TR181_STAT_TOTAL_UPLOADS, atomic_load(&s_total_uploads));
}

void datamodel_append_anomaly_log(const AnomalyEvent *event,
                                  const char *action_taken,
                                  const char *action_result) {
    char timestamp[32];

    if (!s_dm || !event) return;

    /* Enforce HGW_ANOMALY_LOG_MAX cap — delete oldest instance when full */
    amxd_object_t *log_obj = amxd_dm_findf(s_dm, "%s", TR181_ANOMALY_LOG);
    if (log_obj) {
        uint32_t count = 0;
        uint32_t oldest_idx = 0;
        bool have_oldest = false;
        amxd_object_for_each(instance, it, log_obj) {
            amxd_object_t *inst = amxc_container_of(it, amxd_object_t, it);
            if (!have_oldest) {
                oldest_idx = amxd_object_get_index(inst);
                have_oldest = true;
            }
            count++;
        }
        if (count >= HGW_ANOMALY_LOG_MAX && have_oldest) {
            amxd_trans_t del_trans;
            amxd_trans_init(&del_trans);
            amxd_trans_set_attr(&del_trans, amxd_tattr_change_ro, true);
            amxd_trans_select_object(&del_trans, log_obj);
            amxd_trans_del_inst(&del_trans, oldest_idx, NULL);
            amxd_trans_apply(&del_trans, s_dm);
            amxd_trans_clean(&del_trans);
        }
    }

    amxd_trans_t trans;
    amxd_trans_init(&trans);
    amxd_trans_set_attr(&trans, amxd_tattr_change_ro, true);
    amxd_trans_select_pathf(&trans, "%s", TR181_ANOMALY_LOG);
    amxd_trans_add_inst(&trans, 0, NULL);

    dm_format_utc(event->detected_at.tv_sec, timestamp, sizeof(timestamp));
    amxd_trans_set_value(cstring_t, &trans, "Timestamp", timestamp);
    amxd_trans_set_value(cstring_t, &trans, "AnomalyType",
                         anomaly_type_to_string(event->type));
    amxd_trans_set_value(uint32_t, &trans, "MetricValue", event->metric_value);
    amxd_trans_set_value(cstring_t, &trans, "ProcessName", event->process_name);
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

void datamodel_update_self_stats(void) {
    /* CPU: read /proc/self/stat fields utime+stime, compare with previous call */
    static unsigned long long prev_proc_ticks = 0;
    static unsigned long long prev_total_ticks = 0;

    FILE *f = fopen("/proc/self/stat", "r");
    if (f) {
        unsigned long utime = 0, stime = 0;
        char stat_buf[512];
        int r = 0;
        if (fgets(stat_buf, sizeof(stat_buf), f)) {
            /* Skip past comm field's closing ')' — handles names with spaces */
            char *p = strrchr(stat_buf, ')');
            if (p)
                r = sscanf(p + 2,
                    "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                    &utime, &stime);
        }
        fclose(f);
        if (r == 2) {
            unsigned long long proc_ticks = (unsigned long long)utime + stime;
            unsigned long long total_ticks = 0;
            FILE *sf = fopen("/proc/stat", "r");
            if (sf) {
                unsigned long long u, n, s, i, iow, irq, sirq;
                if (fscanf(sf, "cpu %llu %llu %llu %llu %llu %llu %llu",
                           &u, &n, &s, &i, &iow, &irq, &sirq) == 7)
                    total_ticks = u + n + s + i + iow + irq + sirq;
                fclose(sf);
            }
            if (prev_total_ticks > 0 && total_ticks > prev_total_ticks) {
                unsigned long long dt_proc  = proc_ticks  - prev_proc_ticks;
                unsigned long long dt_total = total_ticks - prev_total_ticks;
                uint32_t cpu_pct = (uint32_t)((dt_proc * 100ULL) / dt_total);
                dm_set_uint32(TR181_SELF_CPU, cpu_pct);
            }
            prev_proc_ticks  = proc_ticks;
            prev_total_ticks = total_ticks;
        }
    }

    /* Memory: read /proc/self/status for VmRSS */
    f = fopen("/proc/self/status", "r");
    if (f) {
        char line[128];
        uint32_t vmrss_kb = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                sscanf(line + 6, "%u", &vmrss_kb);
                break;
            }
        }
        fclose(f);
        if (vmrss_kb > 0) {
            /* mem_pct: fraction of total system RAM used by this daemon */
            uint32_t mem_total_kb = 0;
            FILE *mi = fopen("/proc/meminfo", "r");
            if (mi) {
                char mi_line[128];
                while (fgets(mi_line, sizeof(mi_line), mi)) {
                    if (sscanf(mi_line, "MemTotal: %u kB", &mem_total_kb) == 1) break;
                }
                fclose(mi);
            }
            uint32_t mem_pct = (mem_total_kb > 0)
                ? (uint32_t)((vmrss_kb * 100ULL) / mem_total_kb) : 0;
            dm_set_uint32(TR181_SELF_MEM,     mem_pct);
            dm_set_uint32(TR181_SELF_RSS_KB,  vmrss_kb);
        }
    }
}

void datamodel_reset_on_demand_trigger(void) {
    dm_set_bool(TR181_ON_DEMAND_TRIGGER, false);
}

/* -------------------------------------------------------------------------
 * RPC handlers (bound via ODL 'call' directives)
 * ------------------------------------------------------------------------- */

amxd_status_t dm_trigger_diagnostics(amxd_object_t *obj, amxd_function_t *fn,
                                      amxc_var_t *args, amxc_var_t *ret) {
    (void)obj; (void)fn; (void)args;
    syslog(LOG_INFO, "OnDemand diagnostic trigger received from ACS/CLI");

    dm_set_bool(TR181_ON_DEMAND_TRIGGER, false);
    /* Signal the daemon to collect diagnostics via trigger file */
    FILE *f = fopen("/tmp/hgw_diag_trigger", "w");
    if (f) fclose(f);
    amxc_var_set(uint32_t, ret, 0);
    return amxd_status_ok;
}

amxd_status_t dm_reset_counters(amxd_object_t *obj, amxd_function_t *fn,
                                 amxc_var_t *args, amxc_var_t *ret) {
    (void)obj; (void)fn; (void)args; (void)ret;
    atomic_store(&s_anomaly_count, 0);
    atomic_store(&s_total_actions, 0);
    atomic_store(&s_total_uploads, 0);
    dm_set_uint32(TR181_ANOMALY_COUNT,      0);
    dm_set_uint32(TR181_STAT_TOTAL_ACTIONS, 0);
    dm_set_uint32(TR181_STAT_TOTAL_UPLOADS, 0);

    /* Clear all AnomalyLog instances — collect indices first, then delete */
    if (s_dm) {
        amxd_object_t *log_obj = amxd_dm_findf(s_dm, "%s", TR181_ANOMALY_LOG);
        if (log_obj) {
            uint32_t indices[HGW_ANOMALY_LOG_MAX];
            size_t n = 0;
            amxd_object_for_each(instance, it, log_obj) {
                amxd_object_t *inst = amxc_container_of(it, amxd_object_t, it);
                if (n < HGW_ANOMALY_LOG_MAX)
                    indices[n++] = amxd_object_get_index(inst);
            }
            if (n > 0) {
                amxd_trans_t del_trans;
                amxd_trans_init(&del_trans);
                amxd_trans_set_attr(&del_trans, amxd_tattr_change_ro, true);
                amxd_trans_select_object(&del_trans, log_obj);
                for (size_t i = 0; i < n; i++)
                    amxd_trans_del_inst(&del_trans, indices[i], NULL);
                amxd_trans_apply(&del_trans, s_dm);
                amxd_trans_clean(&del_trans);
            }
        }
    }

    syslog(LOG_INFO, "Counters reset via RPC");
    return amxd_status_ok;
}

amxd_status_t dm_set_profile(amxd_object_t *obj, amxd_function_t *fn,
                              amxc_var_t *args, amxc_var_t *ret) {
    (void)obj; (void)fn;
    const char *profile_name = GET_CHAR(args, "ProfileName");
    if (!profile_name || profile_name[0] == '\0') {
        amxc_var_set(int32_t, ret, -1);
        return amxd_status_invalid_value;
    }
    if (!s_dm) {
        amxc_var_set(int32_t, ret, -1);
        return amxd_status_unknown_error;
    }

    /* Find matching profile instance by Name key */
    amxd_object_t *profiles = amxd_dm_findf(s_dm, "%s", TR181_PROFILES);
    if (!profiles) {
        syslog(LOG_ERR, "SetProfile: Profiles table not found");
        amxc_var_set(int32_t, ret, -1);
        return amxd_status_object_not_found;
    }

    amxd_object_t *found = NULL;
    amxc_var_t     name_var;
    amxc_var_init(&name_var);
    amxd_object_for_each(instance, it, profiles) {
        amxd_object_t *inst = amxc_container_of(it, amxd_object_t, it);
        if (amxd_object_get_param(inst, "Name", &name_var) == amxd_status_ok) {
            const char *n = amxc_var_constcast(cstring_t, &name_var);
            if (n && strcmp(n, profile_name) == 0) {
                found = inst;
                break;
            }
        }
    }
    amxc_var_clean(&name_var);

    if (!found) {
        syslog(LOG_WARNING, "SetProfile: profile '%s' not found", profile_name);
        amxc_var_set(int32_t, ret, -1);
        return amxd_status_object_not_found;
    }

    /* Copy profile parameters onto the HGWDoctor root object */
    amxd_object_t *root = amxd_dm_findf(s_dm, "HGWDoctor.");
    if (!root) {
        amxc_var_set(int32_t, ret, -1);
        return amxd_status_unknown_error;
    }

    static const char * const profile_params[] = {
        "CPUThreshold", "MemThreshold", "ThresholdDuration", "PollInterval",
        "ActionType", "ProcessList",
        NULL
    };
    amxd_trans_t trans;
    amxd_trans_init(&trans);
    amxd_trans_select_object(&trans, root);
    amxc_var_t pval;
    amxc_var_init(&pval);
    for (int i = 0; profile_params[i]; i++) {
        if (amxd_object_get_param(found, profile_params[i], &pval) == amxd_status_ok)
            amxd_trans_set_param(&trans, profile_params[i], &pval);
    }
    amxc_var_clean(&pval);

    amxd_status_t st = amxd_trans_apply(&trans, s_dm);
    amxd_trans_clean(&trans);
    if (st != amxd_status_ok) {
        syslog(LOG_ERR, "SetProfile: failed to apply profile '%s' (status=%d)",
               profile_name, st);
        amxc_var_set(int32_t, ret, -1);
        return st;
    }

    dm_set_string(TR181_PROFILE, profile_name);

    /* Signal main loop to propagate the new param values to running modules */
    FILE *f = fopen("/tmp/hgw_cfg_changed", "w");
    if (f) fclose(f);

    syslog(LOG_INFO, "Profile '%s' activated", profile_name);
    amxc_var_set(int32_t, ret, 0);
    return amxd_status_ok;
}

/* -------------------------------------------------------------------------
 * Read all writable DM parameters back into a DmConfig snapshot.
 * Called by the main loop whenever g_dm_changed fires so that every
 * write via ACS or ubus is immediately reflected in the running daemon.
 * ------------------------------------------------------------------------- */
bool datamodel_get_config(DmConfig *out) {
    char object_path[HGW_MAX_PATH];
    char param_name[HGW_MAX_PROC_NAME];
    amxc_var_t val;
    amxd_object_t *obj;

    if (!s_dm || !out) return false;
    memset(out, 0, sizeof(*out));
    amxc_var_init(&val);

#define DM_READ_U32(path, field) do { \
    if (dm_split_path((path), object_path, sizeof(object_path), \
                      param_name, sizeof(param_name)) == 0) { \
        size_t _ol = strlen(object_path); \
        if (_ol > 0 && object_path[_ol-1] != '.' && _ol+1 < HGW_MAX_PATH) \
            { object_path[_ol] = '.'; object_path[_ol+1] = '\0'; } \
        obj = amxd_dm_findf(s_dm, "%s", object_path); \
        if (obj && amxd_object_get_param(obj, param_name, &val) == amxd_status_ok) \
            (field) = amxc_var_dyncast(uint32_t, &val); \
    } \
} while (0)

#define DM_READ_STR(path, field, size) do { \
    if (dm_split_path((path), object_path, sizeof(object_path), \
                      param_name, sizeof(param_name)) == 0) { \
        size_t _ol = strlen(object_path); \
        if (_ol > 0 && object_path[_ol-1] != '.' && _ol+1 < HGW_MAX_PATH) \
            { object_path[_ol] = '.'; object_path[_ol+1] = '\0'; } \
        obj = amxd_dm_findf(s_dm, "%s", object_path); \
        if (obj && amxd_object_get_param(obj, param_name, &val) == amxd_status_ok) { \
            const char *s = amxc_var_constcast(cstring_t, &val); \
            if (s) { strncpy((field), s, (size) - 1); (field)[(size) - 1] = '\0'; } \
        } \
    } \
} while (0)

    DM_READ_U32(TR181_CPU_THRESHOLD,      out->cpu_threshold_pct);
    DM_READ_U32(TR181_MEM_THRESHOLD,      out->mem_threshold_pct);
    DM_READ_U32(TR181_THRESHOLD_DURATION, out->threshold_duration_s);
    DM_READ_U32(TR181_POLL_INTERVAL,      out->poll_interval_s);
    DM_READ_STR(TR181_ACTION_TYPE,        out->action_type,      sizeof(out->action_type));
    DM_READ_STR(TR181_PROCESS_LIST,       out->process_list,     sizeof(out->process_list));
    DM_READ_STR(TR181_UPLOAD_URL,         out->upload_url,       sizeof(out->upload_url));
    DM_READ_STR(TR181_DIAG_OUTPUT_DIR,    out->diag_output_dir,  sizeof(out->diag_output_dir));

    /* Enable */
    if (dm_split_path(TR181_ENABLE, object_path, sizeof(object_path),
                      param_name, sizeof(param_name)) == 0) {
        size_t _ol = strlen(object_path);
        if (_ol > 0 && object_path[_ol-1] != '.' && _ol+1 < HGW_MAX_PATH)
            { object_path[_ol] = '.'; object_path[_ol+1] = '\0'; }
        obj = amxd_dm_findf(s_dm, "%s", object_path);
        if (obj && amxd_object_get_param(obj, param_name, &val) == amxd_status_ok)
            out->enable = amxc_var_dyncast(bool, &val);
        else
            out->enable = true;
    }

#undef DM_READ_U32
#undef DM_READ_STR
    amxc_var_clean(&val);
    return true;
}

/* -------------------------------------------------------------------------
 * OnDemandTrigger helper — polled by the main loop each iteration.
 * Reads the param; if true, resets it and returns true so the caller
 * knows to kick off a diagnostic collection.
 * ------------------------------------------------------------------------- */
bool datamodel_consume_on_demand_trigger(void) {
    char object_path[HGW_MAX_PATH];
    char param_name[HGW_MAX_PROC_NAME];
    amxc_var_t val;
    bool triggered = false;

    if (!s_dm) return false;
    if (dm_split_path(TR181_ON_DEMAND_TRIGGER, object_path, sizeof(object_path),
                      param_name, sizeof(param_name)) != 0)
        return false;

    size_t obj_len = strlen(object_path);
    if (obj_len > 0 && object_path[obj_len - 1] != '.' && obj_len + 1 < HGW_MAX_PATH) {
        object_path[obj_len]     = '.';
        object_path[obj_len + 1] = '\0';
    }

    amxd_object_t *obj = amxd_dm_findf(s_dm, "%s", object_path);
    if (!obj) return false;

    amxc_var_init(&val);
    if (amxd_object_get_param(obj, param_name, &val) == amxd_status_ok)
        triggered = amxc_var_dyncast(bool, &val);
    amxc_var_clean(&val);

    if (triggered)
        dm_set_bool(TR181_ON_DEMAND_TRIGGER, false);

    return triggered;
}

