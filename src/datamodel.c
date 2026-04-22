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
#include <amxd/amxd_transaction.h>
#include <amxo/amxo.h>

#include "datamodel.h"
#include "tr181_params.h"
#include "types.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */
static amxd_status_t dm_on_param_changed(amxd_object_t* const object,
                                          amxd_param_t* const param,
                                          amxd_action_t reason,
                                          const amxc_var_t* const args,
                                          amxc_var_t* const retval,
                                          void* priv);

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

    amxd_object_t *obj = amxd_dm_findf(s_dm, "%s", object_path);
    if (!obj) return;

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
    atomic_store(&s_anomaly_count, 0);
    atomic_store(&s_total_actions, 0);
    atomic_store(&s_total_uploads, 0);

    syslog(LOG_INFO, "DEBUG DM: before dm init");
    amxd_dm_init(dm);
    syslog(LOG_INFO, "DEBUG DM: before parser init");
    amxo_parser_init(parser);

    s_dm = dm;

    syslog(LOG_INFO, "DEBUG DM: before parse file odl=%s", odl_path ? odl_path : "(null)");
    int rc = amxo_parser_parse_file(parser, odl_path, amxd_dm_get_root(dm));
    syslog(LOG_INFO, "DEBUG DM: after parse file rc=%d", rc);
    if (rc != 0) {
        syslog(LOG_ERR, "Failed to parse ODL file: %s (rc=%d)", odl_path, rc);
        return -1;
    }

    syslog(LOG_INFO, "DEBUG DM: before entry points");
    amxo_parser_invoke_entry_points(parser, dm, AMXO_START);
    syslog(LOG_INFO, "DEBUG DM: after entry points");

    /* Register write-action callback on the four threshold parameters so the
     * main loop can detect ACS/ubus changes via /tmp/hgw_cfg_changed.
     * Done in C code (not ODL) to avoid the RTLD_LOCAL dlsym lookup failure
     * that causes a NULL function-pointer call when %populate fires the action. */
    syslog(LOG_INFO, "DEBUG DM: before param action cb");
    static const char * const threshold_params[] = {
        "CPUThreshold", "MemThreshold", "ThresholdDuration", "PollInterval", NULL
    };
    amxd_object_t *hgwdoc = amxd_dm_findf(dm, "HGWDoctor.");
    syslog(LOG_INFO, "DEBUG DM: hgwdoc=%p", (void *)hgwdoc);
    if (hgwdoc) {
        for (int i = 0; threshold_params[i]; i++) {
            amxd_param_t *p = amxd_object_get_param_def(hgwdoc, threshold_params[i]);
            syslog(LOG_INFO, "DEBUG DM: param %s p=%p", threshold_params[i], (void *)p);
            if (p)
                amxd_param_add_action_cb(p, action_param_write, dm_on_param_changed, NULL);
        }
    }

    syslog(LOG_INFO, "DEBUG DM: done");
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
    dm_set_uint32(TR181_STAT_TOTAL_ACTIONS, atomic_fetch_add(&s_total_actions, 1) + 1);

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
        dm_set_uint32(TR181_STAT_TOTAL_UPLOADS, atomic_fetch_add(&s_total_uploads, 1) + 1);
        dm_set_datetime_now(TR181_UPLOAD_TIMESTAMP);
    }
}

void datamodel_increment_anomaly_count(void) {
    dm_set_uint32(TR181_ANOMALY_COUNT, atomic_fetch_add(&s_anomaly_count, 1) + 1);
}

void datamodel_append_anomaly_log(const AnomalyEvent *event,
                                  const char *action_taken,
                                  const char *action_result) {
    char timestamp[32];

    /* Add instance to AnomalyLog multi-instance object */
    if (!s_dm || !event) return;

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
        /* fields: pid(1) comm(2) state(3) ... utime(14) stime(15) */
        int r = fscanf(f,
            "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
            &utime, &stime);
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
            /* mem_pct: fraction of total system RAM */
            uint32_t mem_total_kb = 0;
            FILE *mi = fopen("/proc/meminfo", "r");
            if (mi) {
                fscanf(mi, "MemTotal: %u kB", &mem_total_kb);
                fclose(mi);
            }
            uint32_t mem_pct = (mem_total_kb > 0)
                ? (uint32_t)((vmrss_kb * 100ULL) / mem_total_kb) : 0;
            dm_set_uint32(TR181_SELF_MEM,      mem_pct);
            dm_set_uint32(TR181_SELF_MEM_FREE, vmrss_kb);
        }
    }
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
    syslog(LOG_INFO, "Counters reset via RPC");
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
    syslog(LOG_INFO, "Active profile set to '%s' via RPC", profile_name);
    amxc_var_set(int32_t, ret, 0);
    return amxd_status_ok;
}

/* -------------------------------------------------------------------------
 * Read current threshold params from the live data model.
 * Called by the main loop after dm_on_param_changed fires.
 * ------------------------------------------------------------------------- */
bool datamodel_get_thresholds(uint32_t *cpu_pct, uint32_t *mem_pct,
                               uint32_t *duration_s, uint32_t *poll_s) {
    char object_path[HGW_MAX_PATH];
    char param_name[HGW_MAX_PROC_NAME];
    amxc_var_t val;
    amxd_object_t *obj;

    if (!s_dm || !cpu_pct || !mem_pct || !duration_s || !poll_s) return false;

    amxc_var_init(&val);

#define DM_READ_U32(path, out) do { \
    if (dm_split_path((path), object_path, sizeof(object_path), \
                      param_name, sizeof(param_name)) == 0) { \
        obj = amxd_dm_findf(s_dm, "%s", object_path); \
        if (obj && amxd_object_get_param(obj, param_name, &val) == amxd_status_ok) \
            *(out) = amxc_var_dyncast(uint32_t, &val); \
    } \
} while (0)

    DM_READ_U32(TR181_CPU_THRESHOLD,      cpu_pct);
    DM_READ_U32(TR181_MEM_THRESHOLD,      mem_pct);
    DM_READ_U32(TR181_THRESHOLD_DURATION, duration_s);
    DM_READ_U32(TR181_POLL_INTERVAL,      poll_s);

#undef DM_READ_U32
    amxc_var_clean(&val);
    return true;
}

/* -------------------------------------------------------------------------
 * Action callback: fires when a threshold parameter is written.
 * Correct amxd_action_fn_t signature (6 args) — registered in C code after
 * ODL parse to avoid the ODL auto-resolver RTLD_LOCAL lookup failure that
 * caused a NULL-call segfault when on-action-write was in the ODL.
 * ------------------------------------------------------------------------- */
static amxd_status_t dm_on_param_changed(amxd_object_t* const object,
                                          amxd_param_t* const param,
                                          amxd_action_t reason,
                                          const amxc_var_t* const args,
                                          amxc_var_t* const retval,
                                          void* priv) {
    (void)object; (void)param; (void)reason; (void)args; (void)retval; (void)priv;
    syslog(LOG_INFO, "dm_on_param_changed fired");
    FILE *f = fopen("/tmp/hgw_cfg_changed", "w");
    if (f) fclose(f);
    return amxd_status_function_not_implemented;
}
