/**
 * @file main.c
 * @brief HGW-Doctor daemon entry point and main event loop.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>

#include <amxc/amxc.h>
#include <amxp/amxp.h>
#include <amxp/amxp_signal.h>

#include <amxd/amxd_types.h>
#include <amxd/amxd_dm.h>
#include <amxo/amxo.h>
#include <amxb/amxb.h>
#include <amxb/amxb_register.h>

#include "config.h"
#include "datamodel.h"
#include "tr181_params.h"
#include "monitor.h"
#include "analyzer.h"
#include "recovery.h"
#include "diag_collector.h"
#include "uploader.h"
#include "logger.h"
#include "types.h"

/* -------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------- */
#define RECOVERY_COOLDOWN_S  300  /* min seconds between recovery actions of same type */
#define WD_PET_INTERVAL_S     10  /* watchdog keep-alive interval */

static volatile sig_atomic_t g_running           = 1;
static volatile sig_atomic_t g_reload_cfg        = 0;
static volatile sig_atomic_t g_diag_req          = 0;  /* set by SIGUSR1 — manual trigger */
static _Atomic int           g_anomaly_diag      = ATOMIC_VAR_INIT(0); /* set by on_anomaly */
static _Atomic int           g_monitoring_enabled = ATOMIC_VAR_INIT(1);
static _Atomic int           g_dm_changed        = ATOMIC_VAR_INIT(0); /* set by DM signal */
static _Atomic int           g_reboot_pending    = ATOMIC_VAR_INIT(0); /* deferred reboot armed */
static int                   s_reboot_ticks      = 0;  /* 100ms ticks until reboot; main loop only */

/* Per-AnomalyType last recovery dispatch time — cooldown enforcement.
 * Accessed only from on_anomaly(), which is called serially by the analyzer thread. */
static time_t s_last_recovery[6] = {0};

static int g_wd_fd = -1;

static amxd_dm_t        g_dm;
static amxo_parser_t    g_parser;
static MetricCircBuf    g_metric_buf;
static amxb_bus_ctx_t  *g_bus_ctx = NULL;
static AnomalyEvent     s_last_event;
static pthread_mutex_t  s_event_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Pending DM updates — filled by background threads, drained by main loop.
 * All Ambiorix DM writes must happen on the main thread; callbacks only
 * store results here and let the event loop apply them. */
typedef struct {
    int            recovery_valid;
    RecoveryResult recovery_result;
    AnomalyEvent   recovery_event;
    int            upload_valid;
    UploadStatus   upload_status;
    char           upload_path[HGW_MAX_PATH];
} PendingDmUpdate;
static PendingDmUpdate  s_pending       = {0};
static pthread_mutex_t  s_pending_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Per-dispatch userdata for on-demand recovery threads */
typedef struct { AnomalyEvent ev; } OnDemandData;

static _Atomic int    s_ondemand_result_ready = ATOMIC_VAR_INIT(0);
static RecoveryResult s_ondemand_result        = {0};
static AnomalyEvent   s_ondemand_event         = {0};

/* Deferred DM writes from execute_on_demand_action() — applied in the
 * main loop after amxb_read() has sent the ubus write response. */
static _Atomic int s_reset_target          = ATOMIC_VAR_INIT(0);
static _Atomic int s_ondemand_log_ready    = ATOMIC_VAR_INIT(0);
static char        s_ondemand_log_act[32]  = {0};
static char        s_ondemand_log_res[32]  = {0};
static _Atomic int s_ondemand_status_ready = ATOMIC_VAR_INIT(0);
static char        s_ondemand_status_val[32] = {0};

/* Last DM config applied to the running modules — used by periodic poll to
 * detect runtime ubus _set changes. */
static DmConfig s_last_applied_dmc = {0};

/* Read HGWDoctor config directly from the local DM — avoids the stale-read
 * problem with amxb_get() over the ubus socket, where a value written via
 * _set may not have propagated by the time the main loop polls. */
static bool fetch_config_from_bus(DmConfig *out) {
    amxd_object_t *obj = amxd_dm_findf(&g_dm, "HGWDoctor.");
    if (!obj) return datamodel_get_config(out);

    amxc_var_t val;
    amxc_var_init(&val);

    memset(out, 0, sizeof(*out));

#define READ_U32(param, field) do { \
    if (amxd_object_get_param(obj, param, &val) == amxd_status_ok) \
        out->field = amxc_var_dyncast(uint32_t, &val); \
} while(0)

#define READ_STR(param, field) do { \
    if (amxd_object_get_param(obj, param, &val) == amxd_status_ok) { \
        const char *s = amxc_var_constcast(cstring_t, &val); \
        if (s) strncpy(out->field, s, sizeof(out->field) - 1); \
    } \
} while(0)

    READ_U32("CPUThreshold",      cpu_threshold_pct);
    READ_U32("MemThreshold",      mem_threshold_pct);
    READ_U32("ThresholdDuration", threshold_duration_s);
    READ_U32("PollInterval",      poll_interval_s);
    READ_STR("ActionType",        action_type);
    READ_STR("ProcessList",       process_list);
    READ_STR("UploadURL",         upload_url);
    READ_STR("DiagOutputDir",     diag_output_dir);
    READ_STR("OnDemandTarget",    on_demand_target);

#undef READ_U32
#undef READ_STR

    if (amxd_object_get_param(obj, "Enable", &val) == amxd_status_ok)
        out->enable = amxc_var_dyncast(bool, &val);

    amxc_var_clean(&val);
    return out->poll_interval_s > 0;
}
/* Fallbacks for apply_dm_config() — set from flat config at startup/SIGHUP */
static char s_scripts_dir[HGW_MAX_PATH]                                    = {0};
static char s_fallback_proc_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME]   = {{0}};
static int  s_fallback_proc_count                                           = 0;

static ActionType action_str_to_type(const char *s) {
    if (!s || s[0] == '\0') return ACTION_NONE;
    if (strcasecmp(s, "ProcessRestart") == 0) return ACTION_PROCESS_RESTART;
    if (strcasecmp(s, "CacheClear")     == 0) return ACTION_CACHE_CLEAR;
    if (strcasecmp(s, "Reboot")         == 0) return ACTION_REBOOT;
    return ACTION_NONE;
}

static void parse_process_list(const char *list,
                                char names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME],
                                int *count) {
    char tmp[HGW_MAX_PROC_LIST * HGW_MAX_PROC_NAME];
    char *saveptr = NULL;
    *count = 0;
    if (!list || list[0] == '\0') return;
    strncpy(tmp, list, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *tok = strtok_r(tmp, ",", &saveptr);
    while (tok && *count < HGW_MAX_PROC_LIST) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && *(end - 1) == ' ') end--;
        *end = '\0';
        if (*tok != '\0') {
            strncpy(names[*count], tok, HGW_MAX_PROC_NAME - 1);
            names[*count][HGW_MAX_PROC_NAME - 1] = '\0';
            (*count)++;
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
}

static const char *action_type_to_string(ActionType action) {
    switch (action) {
        case ACTION_PROCESS_RESTART: return "ProcessRestart";
        case ACTION_CACHE_CLEAR:     return "CacheClear";
        case ACTION_REBOOT:          return "Reboot";
        case ACTION_NONE:
        default:                     return "None";
    }
}

static bool dmconfig_changed(const DmConfig *a, const DmConfig *b) {
    return a->cpu_threshold_pct    != b->cpu_threshold_pct    ||
           a->mem_threshold_pct    != b->mem_threshold_pct    ||
           a->threshold_duration_s != b->threshold_duration_s ||
           a->poll_interval_s      != b->poll_interval_s      ||
           a->enable               != b->enable               ||
           strncmp(a->action_type,      b->action_type,      sizeof(a->action_type))      != 0 ||
           strncmp(a->process_list,     b->process_list,     sizeof(a->process_list))     != 0 ||
           strncmp(a->upload_url,       b->upload_url,       sizeof(a->upload_url))       != 0 ||
           strncmp(a->diag_output_dir,  b->diag_output_dir,  sizeof(a->diag_output_dir))  != 0;
}

/* forward declaration — defined after on_recovery_done */
static void on_ondemand_done(const RecoveryResult *result, void *userdata);

/* -------------------------------------------------------------------------
 * On-demand action dispatch — called when ActionType is written to a
 * non-None value.  ActionType is reset to "None" immediately after so that
 * the next poll does not re-trigger.  For Reboot, arms a deferred countdown;
 * for ProcessRestart / CacheClear, dispatches an async recovery task.
 * ------------------------------------------------------------------------- */
static void execute_on_demand_action(const char *action_str, const DmConfig *dmc) {
    ActionType action = action_str_to_type(action_str);
    if (action == ACTION_NONE) return;

    LOG_INFO("On-demand action requested: %s", action_str);

    if (action == ACTION_REBOOT) {
        /* Safety guards — read-only DM access, no socket writes */
        char last_rt[32] = {0};
        datamodel_get_last_reboot_time(last_rt, sizeof(last_rt));
        RebootGuardResult guard = recovery_reboot_guard_check(last_rt);

        if (guard == REBOOT_GUARD_SAFE_MODE) {
            LOG_WARN("On-demand reboot denied: rate limit exceeded — entering SafeMode");
            strncpy(s_ondemand_log_act, "Reboot",   sizeof(s_ondemand_log_act) - 1);
            strncpy(s_ondemand_log_res, "Rejected", sizeof(s_ondemand_log_res) - 1);
            atomic_store(&s_ondemand_log_ready, 1);
            strncpy(s_ondemand_status_val, STATUS_SAFE_MODE, sizeof(s_ondemand_status_val) - 1);
            atomic_store(&s_ondemand_status_ready, 1);
            return;
        }
        if (guard == REBOOT_GUARD_UPTIME_TOO_LOW || guard == REBOOT_GUARD_COOLDOWN) {
            strncpy(s_ondemand_log_act, "Reboot",   sizeof(s_ondemand_log_act) - 1);
            strncpy(s_ondemand_log_res, "Rejected", sizeof(s_ondemand_log_res) - 1);
            atomic_store(&s_ondemand_log_ready, 1);
            return;
        }

        /* Guard OK — arm deferred reboot */
        uint32_t delay_s = datamodel_get_reboot_delay_sec();
        if (delay_s == 0) delay_s = 10;
        strncpy(s_ondemand_log_act, "Reboot",  sizeof(s_ondemand_log_act) - 1);
        strncpy(s_ondemand_log_res, "Pending", sizeof(s_ondemand_log_res) - 1);
        atomic_store(&s_ondemand_log_ready, 1);
        strncpy(s_ondemand_status_val, STATUS_REBOOT_PENDING, sizeof(s_ondemand_status_val) - 1);
        atomic_store(&s_ondemand_status_ready, 1);
        diag_collect(NULL);
        s_reboot_ticks = (int)(delay_s * 10);  /* 100ms ticks */
        atomic_store_explicit(&g_reboot_pending, 1, memory_order_release);
        LOG_INFO("Deferred reboot armed: %us countdown (%d ticks)", delay_s, s_reboot_ticks);

    } else if (action == ACTION_PROCESS_RESTART) {
        char target[HGW_MAX_PROC_NAME] = {0};
        datamodel_get_on_demand_target(target, sizeof(target));

        if (target[0] != '\0') {
            /* Single named target — defer the DM reset to the drain block */
            atomic_store(&s_reset_target, 1);
            OnDemandData *data = calloc(1, sizeof(*data));
            if (data) {
                data->ev.type = ANOMALY_ON_DEMAND;
                clock_gettime(CLOCK_REALTIME, &data->ev.detected_at);
                strncpy(data->ev.process_name, target, HGW_MAX_PROC_NAME - 1);
                recovery_dispatch_ondemand(ACTION_PROCESS_RESTART, target,
                                           s_scripts_dir, on_ondemand_done, data);
            }
        } else {
            /* Restart every service in ProcessList — one dispatch per process */
            char proc_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME] = {{0}};
            int  proc_count = 0;
            parse_process_list(dmc->process_list, proc_names, &proc_count);
            if (proc_count == 0) {
                memcpy(proc_names, s_fallback_proc_names, sizeof(proc_names));
                proc_count = s_fallback_proc_count;
            }
            LOG_INFO("On-demand restart all services (%d)", proc_count);
            for (int i = 0; i < proc_count; i++) {
                OnDemandData *data = calloc(1, sizeof(*data));
                if (!data) continue;
                data->ev.type = ANOMALY_ON_DEMAND;
                clock_gettime(CLOCK_REALTIME, &data->ev.detected_at);
                strncpy(data->ev.process_name, proc_names[i], HGW_MAX_PROC_NAME - 1);
                recovery_dispatch_ondemand(ACTION_PROCESS_RESTART, proc_names[i],
                                           s_scripts_dir, on_ondemand_done, data);
            }
        }
    } else {
        /* CacheClear */
        OnDemandData *data = calloc(1, sizeof(*data));
        if (data) {
            data->ev.type = ANOMALY_ON_DEMAND;
            clock_gettime(CLOCK_REALTIME, &data->ev.detected_at);
            recovery_dispatch_ondemand(ACTION_CACHE_CLEAR, NULL,
                                       s_scripts_dir, on_ondemand_done, data);
        }
    }
}

static void apply_dm_config(const DmConfig *dmc) {
    /* Work on a copy so we can override action_type after on-demand dispatch */
    DmConfig effective = *dmc;

    /* Detect on-demand action: ActionType just changed to a non-None value.
     * Guard g_reboot_pending prevents re-arming during an active countdown. */
    ActionType new_act  = action_str_to_type(effective.action_type);
    ActionType prev_act = action_str_to_type(s_last_applied_dmc.action_type);
    if (new_act != ACTION_NONE && new_act != prev_act &&
        !atomic_load_explicit(&g_reboot_pending, memory_order_relaxed)) {
        execute_on_demand_action(effective.action_type, &effective);
        strncpy(effective.action_type, "None", sizeof(effective.action_type) - 1);
    }

    char proc_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME] = {{0}};
    int  proc_count = 0;
    parse_process_list(effective.process_list, proc_names, &proc_count);
    if (proc_count == 0) {
        memcpy(proc_names, s_fallback_proc_names, sizeof(proc_names));
        proc_count = s_fallback_proc_count;
    }

    AnalyzerConfig acfg2 = {0};
    acfg2.cpu_threshold_pct    = effective.cpu_threshold_pct;
    acfg2.mem_threshold_pct    = effective.mem_threshold_pct;
    acfg2.threshold_duration_s = effective.threshold_duration_s;
    acfg2.poll_interval_s      = effective.poll_interval_s;
    acfg2.process_count        = proc_count;
    memcpy(acfg2.process_names, proc_names, sizeof(acfg2.process_names));
    analyzer_update_config(&acfg2);
    monitor_update_config(
        (const char (*)[HGW_MAX_PROC_NAME]) proc_names,
        proc_count, effective.poll_interval_s);

    RecoveryConfig rcfg2 = {0};
    rcfg2.action_type   = action_str_to_type(effective.action_type);
    rcfg2.process_count = proc_count;
    memcpy(rcfg2.process_names, proc_names, sizeof(rcfg2.process_names));
    strncpy(rcfg2.scripts_dir, s_scripts_dir, HGW_MAX_PATH - 1);
    recovery_update_config(&rcfg2);

    if (effective.upload_url[0] != '\0')
        uploader_update_url(effective.upload_url);
    if (effective.diag_output_dir[0] != '\0')
        diag_collector_update_output_dir(effective.diag_output_dir);

    if (!effective.enable && atomic_load_explicit(&g_monitoring_enabled, memory_order_relaxed)) {
        atomic_store_explicit(&g_monitoring_enabled, 0, memory_order_relaxed);

        /* Full shutdown: stop threads, clear cache, reset logs */
        analyzer_stop();
        monitor_stop();

        /* Clear diagnostic archives */
        system("rm -f /tmp/hgw_diag/*.tar.gz");

        /* Reset data model counters and logs */
        datamodel_reset_all();

        datamodel_set_status(STATUS_DISABLED);
        LOG_INFO("Monitoring fully stopped and cache cleared");
    } else if (effective.enable && !atomic_load_explicit(&g_monitoring_enabled, memory_order_relaxed)) {
        atomic_store_explicit(&g_monitoring_enabled, 1, memory_order_relaxed);

        monitor_start();
        analyzer_start();

        datamodel_set_status(STATUS_ENABLED);
        LOG_INFO("Monitoring re-started from scratch");
    }

    LOG_INFO("Live config updated from DM: "
             "cpu=%u mem=%u dur=%u poll=%u action=%s procs=%d enable=%d",
             effective.cpu_threshold_pct, effective.mem_threshold_pct,
             effective.threshold_duration_s, effective.poll_interval_s,
             effective.action_type, proc_count, effective.enable);

    s_last_applied_dmc = effective;
}

/* -------------------------------------------------------------------------
 * PID file
 * ------------------------------------------------------------------------- */
#define HGW_PID_FILE "/var/run/hgw-doctor.pid"

static void write_pid_file(void) {
    FILE *f = fopen(HGW_PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    } else {
        LOG_WARN("Failed to write PID file %s", HGW_PID_FILE);
    }
}

/* -------------------------------------------------------------------------
 * Hardware watchdog
 * ------------------------------------------------------------------------- */
static void watchdog_open(void) {
    g_wd_fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    if (g_wd_fd < 0)
        LOG_INFO("No watchdog device found — hardware watchdog disabled");
    else
        LOG_INFO("Watchdog opened — petting every %ds", WD_PET_INTERVAL_S);
}

static void watchdog_pet(void) {
    if (g_wd_fd >= 0 && write(g_wd_fd, "1", 1) < 0)
        LOG_WARN("Watchdog pet failed: %s", strerror(errno));
}

static void watchdog_close(void) {
    if (g_wd_fd >= 0) {
        /* "V" magic char signals the driver this was an intentional close —
         * prevents a reboot when CONFIG_WATCHDOG_NOWAYOUT is not set. */
        (void)write(g_wd_fd, "V", 1);
        close(g_wd_fd);
        g_wd_fd = -1;
    }
}

/* -------------------------------------------------------------------------
 * Signal handlers
 * ------------------------------------------------------------------------- */
static void sig_handler(int signo) {
    if (signo == SIGTERM || signo == SIGINT) {
        g_running = 0;
    } else if (signo == SIGHUP) {
        g_reload_cfg = 1;
    } else if (signo == SIGUSR1) {
        g_diag_req = 1;
    }
}

static void install_signals(void) {
    struct sigaction sa = { .sa_handler = sig_handler, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* -------------------------------------------------------------------------
 * Anomaly callback (analyzer → recovery + diag_collector)
 * ------------------------------------------------------------------------- */
static void on_anomaly(const AnomalyEvent *event, void *userdata) {
    (void)userdata;
    if (!atomic_load_explicit(&g_monitoring_enabled, memory_order_relaxed)) return;
    LOG_WARN("Anomaly detected: type=%d value=%u%% duration=%us",
             event->type, event->metric_value, event->duration_s);

    /* Write s_last_event under lock, then publish the flag atomically.
     * The mutex unlock acts as a release barrier for s_last_event; the
     * acquire load in the main loop pairs with this release store. */
    pthread_mutex_lock(&s_event_mutex);
    s_last_event = *event;
    pthread_mutex_unlock(&s_event_mutex);
    atomic_store_explicit(&g_anomaly_diag, 1, memory_order_release);

    datamodel_increment_anomaly_count();

    /* Per-type cooldown — prevents hammering recovery while an anomaly persists */
    int type_idx = ((int)event->type >= 0 && (int)event->type < 6) ? (int)event->type : 0;
    time_t now_rc = time(NULL);
    if (now_rc - s_last_recovery[type_idx] >= RECOVERY_COOLDOWN_S) {
        s_last_recovery[type_idx] = now_rc;
        recovery_dispatch(event);
    } else {
        LOG_INFO("Recovery cooldown active for anomaly type %d (%lds remaining)",
                 event->type,
                 (long)(RECOVERY_COOLDOWN_S - (now_rc - s_last_recovery[type_idx])));
    }
}

/* -------------------------------------------------------------------------
 * Recovery done callback (recovery thread → pending struct → main thread DM)
 * ------------------------------------------------------------------------- */
static void on_recovery_done(const RecoveryResult *result, void *userdata) {
    (void)userdata;
    AnomalyEvent ev_copy;
    pthread_mutex_lock(&s_event_mutex);
    ev_copy = s_last_event;
    pthread_mutex_unlock(&s_event_mutex);

    pthread_mutex_lock(&s_pending_mutex);
    s_pending.recovery_result = *result;
    s_pending.recovery_event  = ev_copy;
    s_pending.recovery_valid  = 1;
    pthread_mutex_unlock(&s_pending_mutex);
}

/* -------------------------------------------------------------------------
 * On-demand recovery done callback — one per dispatched thread, each with
 * its own heap-allocated OnDemandData.  Queues result for the main loop.
 * ------------------------------------------------------------------------- */
static void on_ondemand_done(const RecoveryResult *result, void *userdata) {
    OnDemandData *data = (OnDemandData *)userdata;
    AnomalyEvent ev = {0};
    if (data) {
        ev = data->ev;
        free(data);
    }
    atomic_store(&s_ondemand_result_ready, 1);
    s_ondemand_result = *result;
    s_ondemand_event  = ev;
}

/* -------------------------------------------------------------------------
 * Diagnostics done callback (diag thread → pending struct → main thread DM)
 * ------------------------------------------------------------------------- */
static void on_diag_done(const char *archive_path, void *userdata) {
    (void)userdata;
    if (!archive_path) {
        LOG_WARN("Diagnostics collection failed — no archive produced");
        pthread_mutex_lock(&s_pending_mutex);
        s_pending.upload_path[0] = '\0';
        s_pending.upload_status  = UPLOAD_STATUS_FAILED;
        s_pending.upload_valid   = 1;
        pthread_mutex_unlock(&s_pending_mutex);
        return;
    }

    LOG_INFO("Diagnostics archive ready: %s", archive_path);
    pthread_mutex_lock(&s_pending_mutex);
    strncpy(s_pending.upload_path, archive_path, sizeof(s_pending.upload_path) - 1);
    s_pending.upload_path[sizeof(s_pending.upload_path) - 1] = '\0';
    s_pending.upload_status = UPLOAD_STATUS_PENDING;
    s_pending.upload_valid  = 1;
    pthread_mutex_unlock(&s_pending_mutex);

    uploader_send(archive_path);
}

/* -------------------------------------------------------------------------
 * Upload done callback (upload thread → pending struct → main thread DM)
 * ------------------------------------------------------------------------- */
static void on_dm_object_changed(const char *const sig_name,
                                 const amxc_var_t *const data,
                                 void *const priv) {
    (void)sig_name; (void)data; (void)priv;
    atomic_store_explicit(&g_dm_changed, 1, memory_order_release);
}

static void on_upload_done(UploadStatus status, const char *path, void *userdata) {
    (void)userdata;
    if (status == UPLOAD_STATUS_SUCCESS)
        LOG_INFO("Upload succeeded: %s", path ? path : "<unknown>");
    else
        LOG_WARN("Upload failed for: %s", path ? path : "<unknown>");

    pthread_mutex_lock(&s_pending_mutex);
    s_pending.upload_status = status;
    if (path)
        strncpy(s_pending.upload_path, path, sizeof(s_pending.upload_path) - 1);
    s_pending.upload_valid = 1;
    pthread_mutex_unlock(&s_pending_mutex);
}

/* -------------------------------------------------------------------------
 * Device.X_TELNET_HGWDoctor. change callback
 * Fires when the ACS writes a param via TR-069 proxy object.
 * Mirrors the new value to HGWDoctor. so the local DM and modules pick it up.
 * ------------------------------------------------------------------------- */
static void on_device_object_changed(const char *sig_name,
                                      const amxc_var_t *data,
                                      void *priv) {
    (void)sig_name; (void)data; (void)priv;
    atomic_store_explicit(&g_dm_changed, 1, memory_order_release);
}

/* -------------------------------------------------------------------------
 * main()
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    HgwConfig cfg;
    const char *conf_path = (argc > 1) ? argv[1] : NULL;

    /* 1. Logging (syslog, LOG_DAEMON facility) */
    logger_init(LOG_LEVEL_INFO, NULL);
    LOG_INFO("HGW-Doctor starting (pid=%d)", getpid());

    /* 2. Configuration */
    if (config_load(conf_path, &cfg) < 0) {
        LOG_WARN("Config load failed, using defaults");
    }
    strncpy(s_scripts_dir, cfg.scripts_dir, HGW_MAX_PATH - 1);
    memcpy(s_fallback_proc_names, cfg.process_names, sizeof(s_fallback_proc_names));
    s_fallback_proc_count = cfg.process_count;

    /* 3. Signal handling */
    install_signals();

    /* 4. Ambiorix data model */
    if (datamodel_init(&g_dm, &g_parser, cfg.odl_path) != 0) {
        LOG_ERROR("Failed to initialise data model - aborting");
        return EXIT_FAILURE;
    }

    /* Connect to ubus and register the data model */
    amxb_be_load("/usr/bin/mods/amxb/mod-amxb-ubus.so");
    if (amxb_connect(&g_bus_ctx, "ubus:/var/run/ubus/ubus.sock") == 0) {
        LOG_INFO("Connected to ubus");
        if (amxb_register(g_bus_ctx, &g_dm) == 0) {
            LOG_INFO("Data model registered on ubus");
            /* datamodel_start_sync(); */  /* disabled: bidirectional sync overwrites user _set values */
            /* Local DM signal — fires when the daemon's own DM changes */
            amxp_slot_connect(&g_dm.sigmngr, "dm:object-changed", NULL,
                              on_dm_object_changed, NULL);
            /* Subscribe to ACS proxy object so TR-069 writes are mirrored to HGWDoctor */
            amxb_subscribe(g_bus_ctx, "Device.X_TELNET_HGWDoctor.", NULL,
                           on_device_object_changed, NULL);
        } else {
            LOG_WARN("Failed to register data model on ubus");
        }
    } else {
        LOG_WARN("Failed to connect to ubus");
    }

    /* 4b. Merge DM persistent values into cfg — takes priority over config file.
     * Reads from the bus (amxd's authoritative DM) so persisted values from a
     * prior run that were written via ubus/ACS are correctly restored. */
    {
        DmConfig dmc = {0};
        if (fetch_config_from_bus(&dmc) && dmc.poll_interval_s > 0) {
            cfg.cpu_threshold_pct    = dmc.cpu_threshold_pct;
            cfg.mem_threshold_pct    = dmc.mem_threshold_pct;
            cfg.threshold_duration_s = dmc.threshold_duration_s;
            cfg.poll_interval_s      = dmc.poll_interval_s;
            cfg.action_type          = action_str_to_type(dmc.action_type);
            if (dmc.process_list[0] != '\0')
                parse_process_list(dmc.process_list, cfg.process_names, &cfg.process_count);
            if (dmc.upload_url[0] != '\0')
                strncpy(cfg.upload_url, dmc.upload_url, sizeof(cfg.upload_url) - 1);
            if (dmc.diag_output_dir[0] != '\0')
                strncpy(cfg.diag_output_dir, dmc.diag_output_dir, sizeof(cfg.diag_output_dir) - 1);
            atomic_store_explicit(&g_monitoring_enabled, dmc.enable ? 1 : 0, memory_order_relaxed);
            s_last_applied_dmc = dmc;
            LOG_INFO("Startup: DM persistent state applied "
                     "(cpu=%u%% mem=%u%% dur=%us poll=%us action=%s enable=%d)",
                     cfg.cpu_threshold_pct, cfg.mem_threshold_pct,
                     cfg.threshold_duration_s, cfg.poll_interval_s,
                     dmc.action_type, dmc.enable);
        }
    }

    /* 5. Worker modules */
    monitor_init(&g_metric_buf, cfg.process_names, cfg.process_count,
                 cfg.poll_interval_s);

    AnalyzerConfig acfg = {0};
    acfg.cpu_threshold_pct    = cfg.cpu_threshold_pct;
    acfg.mem_threshold_pct    = cfg.mem_threshold_pct;
    acfg.threshold_duration_s = cfg.threshold_duration_s;
    acfg.poll_interval_s      = cfg.poll_interval_s;
    acfg.process_count        = cfg.process_count;
    memcpy(acfg.process_names, cfg.process_names, sizeof(acfg.process_names));
    analyzer_init(&g_metric_buf, &acfg, on_anomaly, NULL);

    /* Build comma-separated list for the data model */
    char proc_list[HGW_MAX_PROC_LIST * HGW_MAX_PROC_NAME] = {0};
    for (int i = 0; i < cfg.process_count; i++) {
        if (i > 0) strncat(proc_list, ",", sizeof(proc_list) - strlen(proc_list) - 1);
        strncat(proc_list, cfg.process_names[i], sizeof(proc_list) - strlen(proc_list) - 1);
    }
    datamodel_set_process_list(proc_list);
    datamodel_sync_startup(action_type_to_string(cfg.action_type),
                            cfg.upload_url, cfg.diag_output_dir);

    RecoveryConfig rcfg = {0};
    rcfg.action_type   = cfg.action_type;
    rcfg.process_count = cfg.process_count;
    memcpy(rcfg.process_names, cfg.process_names, sizeof(rcfg.process_names));
    strncpy(rcfg.scripts_dir, cfg.scripts_dir, HGW_MAX_PATH - 1);
    recovery_init(&rcfg, on_recovery_done, NULL);

    /* If we booted after a HGWDoctor-triggered reboot, record it in the
     * reboot ring so the guard's rate-limit counts it correctly. */
    if (datamodel_was_deferred_reboot_boot()) {
        struct timespec bts = {0};
        clock_gettime(CLOCK_BOOTTIME, &bts);
        recovery_record_reboot_completed(time(NULL) - bts.tv_sec);
    }

    DiagConfig dcfg = {0};
    dcfg.max_archives = cfg.diag_max_archives;
    strncpy(dcfg.output_dir, cfg.diag_output_dir, HGW_MAX_PATH - 1);
    diag_collector_init(&dcfg, on_diag_done, NULL);

    UploaderConfig ucfg = {0};
    ucfg.timeout_s      = cfg.upload_timeout_s;
    ucfg.max_retries    = cfg.upload_max_retries;
    ucfg.retry_delay_s  = cfg.upload_retry_delay_s;
    ucfg.tls_verify     = cfg.tls_verify;
    strncpy(ucfg.url,          cfg.upload_url,   HGW_MAX_URL - 1);
    strncpy(ucfg.ca_cert_path, cfg.ca_cert_path, HGW_MAX_PATH - 1);
    uploader_init(&ucfg, on_upload_done, NULL);

    /* 6. Start threads */
    if (monitor_start() != 0) {
        LOG_ERROR("Failed to start monitor thread — aborting");
        return EXIT_FAILURE;
    }
    if (analyzer_start() != 0) {
        LOG_ERROR("Failed to start analyzer thread — aborting");
        monitor_stop();
        return EXIT_FAILURE;
    }
    write_pid_file();
    watchdog_open();
    datamodel_set_status("Enabled");

    LOG_INFO("HGW-Doctor running");

    /* 7. Main event loop */
    time_t start_time        = time(NULL);
    time_t last_stats_update = 0;
    time_t last_diag_collect = 0;
    time_t last_wd_pet       = 0;
    time_t last_cfg_poll     = 0;
    int    s_dm_changed_delay = 0;  /* countdown before applying DM signal (loop iterations) */
#define DIAG_COOLDOWN_S 60  /* min seconds between anomaly-triggered collections */
    while (g_running) {
        if (g_reload_cfg) {
            g_reload_cfg = 0;
            config_reload();
            const HgwConfig *cur = config_get();

            monitor_update_config(
                (const char (*)[HGW_MAX_PROC_NAME]) cur->process_names,
                cur->process_count, cur->poll_interval_s);

            AnalyzerConfig acfg = {0};
            acfg.cpu_threshold_pct    = cur->cpu_threshold_pct;
            acfg.mem_threshold_pct    = cur->mem_threshold_pct;
            acfg.threshold_duration_s = cur->threshold_duration_s;
            acfg.poll_interval_s      = cur->poll_interval_s;
            acfg.process_count        = cur->process_count;
            memcpy(acfg.process_names, cur->process_names, sizeof(acfg.process_names));
            analyzer_update_config(&acfg);

            RecoveryConfig rcfg_hup = {0};
            rcfg_hup.action_type   = cur->action_type;
            rcfg_hup.process_count = cur->process_count;
            memcpy(rcfg_hup.process_names, cur->process_names, sizeof(rcfg_hup.process_names));
            strncpy(rcfg_hup.scripts_dir, cur->scripts_dir, HGW_MAX_PATH - 1);
            recovery_update_config(&rcfg_hup);

            if (cur->diag_output_dir[0] != '\0')
                diag_collector_update_output_dir(cur->diag_output_dir);

            strncpy(s_scripts_dir, cur->scripts_dir, HGW_MAX_PATH - 1);
            memcpy(s_fallback_proc_names, cur->process_names, sizeof(s_fallback_proc_names));
            s_fallback_proc_count = cur->process_count;

            UploaderConfig ucfg_hup = {0};
            ucfg_hup.timeout_s     = cur->upload_timeout_s;
            ucfg_hup.max_retries   = cur->upload_max_retries;
            ucfg_hup.retry_delay_s = cur->upload_retry_delay_s;
            ucfg_hup.tls_verify    = cur->tls_verify;
            strncpy(ucfg_hup.url,          cur->upload_url,   HGW_MAX_URL - 1);
            strncpy(ucfg_hup.ca_cert_path, cur->ca_cert_path, HGW_MAX_PATH - 1);
            uploader_update_config(&ucfg_hup);

            LOG_INFO("Config reloaded: all modules updated");
        }
        if (g_diag_req) {
            g_diag_req = 0;
            LOG_INFO("On-demand diagnostics requested via SIGUSR1");
            diag_collect(NULL);
        }
        /* RPC path: trigger file written by dm_trigger_diagnostics().
         * unlink() is atomic — avoids the access()+unlink() race. */
        if (unlink("/tmp/hgw_diag_trigger") == 0) {
            LOG_INFO("On-demand diagnostics triggered via TriggerDiagnostics() RPC");
            diag_collect(NULL);
        }
        /* _set path: OnDemandTrigger param written directly via ubus _set. */
        if (datamodel_consume_on_demand_trigger()) {
            LOG_INFO("On-demand diagnostics triggered via TR-181 OnDemandTrigger write");
            diag_collect(NULL);
        }
        /* Propagate DM writes to running modules.
         * Path 1: trigger file (startup/restore via dm_on_param_changed).
         * Path 2: dm:object-changed Ambiorix signal (reactive, catches ubus _set).
         * Path 3: periodic poll fallback every 5 s. */
        if (unlink("/tmp/hgw_cfg_changed") == 0) {
            DmConfig dmc = {0};
            if (fetch_config_from_bus(&dmc) && dmc.poll_interval_s > 0 &&
                dmconfig_changed(&dmc, &s_last_applied_dmc)) {
                LOG_INFO("DM config updated (trigger): cpu=%u dur=%u enable=%d",
                         dmc.cpu_threshold_pct, dmc.threshold_duration_s, dmc.enable);
                apply_dm_config(&dmc);
            }
        }
        /* dm:object-changed fires when the Ambiorix client path is used
         * (ubus-cli, TR-069/ACS via libamxb). Raw "ubus call HGWDoctor _set"
         * bypasses the Ambiorix write mechanism and does not trigger this
         * signal — for that path the 5-second periodic poll below is the
         * fallback. fetch_config_from_bus() reads directly from the local
         * amxd object so it sees the value as soon as it is committed. */
        if (atomic_load_explicit(&g_dm_changed, memory_order_acquire)) {
            atomic_store_explicit(&g_dm_changed, 0, memory_order_relaxed);
            s_dm_changed_delay = 2;
        }
        if (s_dm_changed_delay > 0) {
            s_dm_changed_delay--;
            if (s_dm_changed_delay == 0) {
                DmConfig dmc = {0};
                if (fetch_config_from_bus(&dmc) && dmc.poll_interval_s > 0 &&
                    dmconfig_changed(&dmc, &s_last_applied_dmc)) {
                    LOG_INFO("DM config updated (signal): cpu=%u dur=%u enable=%d",
                             dmc.cpu_threshold_pct, dmc.threshold_duration_s, dmc.enable);
                    apply_dm_config(&dmc);
                }
            }
        }
        {
            time_t now_poll = time(NULL);
            if (now_poll - last_cfg_poll >= 5) {
                last_cfg_poll = now_poll;
                DmConfig dmc = {0};
                bool cfg_ok = fetch_config_from_bus(&dmc);
                if (!cfg_ok || dmc.poll_interval_s == 0) {
                    LOG_WARN("cfg-poll: DM read failed (ok=%d poll_s=%u cpu=%u)",
                             cfg_ok, dmc.poll_interval_s, dmc.cpu_threshold_pct);
                } else if (dmconfig_changed(&dmc, &s_last_applied_dmc)) {
                    LOG_INFO("DM config updated (poll): cpu=%u dur=%u enable=%d",
                             dmc.cpu_threshold_pct, dmc.threshold_duration_s, dmc.enable);
                    apply_dm_config(&dmc);
                }
            }
        }
        if (atomic_load_explicit(&g_anomaly_diag, memory_order_acquire)) {
            AnomalyEvent ev_copy;
            atomic_store_explicit(&g_anomaly_diag, 0, memory_order_relaxed);
            pthread_mutex_lock(&s_event_mutex);
            ev_copy = s_last_event;
            pthread_mutex_unlock(&s_event_mutex);
            time_t now_diag = time(NULL);
            if (now_diag - last_diag_collect >= DIAG_COOLDOWN_S) {
                last_diag_collect = now_diag;
                LOG_INFO("Collecting diagnostics after anomaly (type=%d)", ev_copy.type);
                diag_collect(&ev_copy);
            } else {
                LOG_INFO("Anomaly diag skipped — cooldown (%lds remaining)",
                         (long)(DIAG_COOLDOWN_S - (now_diag - last_diag_collect)));
            }
        }

        /* Drain pending DM updates from background threads — runs every 100ms */
        {
            PendingDmUpdate snap = {0};
            pthread_mutex_lock(&s_pending_mutex);
            if (s_pending.recovery_valid || s_pending.upload_valid) {
                snap = s_pending;
                s_pending.recovery_valid = 0;
                s_pending.upload_valid   = 0;
            }
            pthread_mutex_unlock(&s_pending_mutex);

            if (snap.recovery_valid) {
                datamodel_record_action(&snap.recovery_result);
                datamodel_append_anomaly_log(
                    &snap.recovery_event,
                    action_type_to_string(snap.recovery_result.action),
                    snap.recovery_result.result == RESULT_SUCCESS ? "Success" : "Failure"
                );
            }
            if (snap.upload_valid)
                datamodel_record_upload(snap.upload_status, snap.upload_path);
        }

        /* Drain deferred DM writes from execute_on_demand_action() — safe here,
         * after amxb_read() has already sent the write response to the caller. */
        if (atomic_exchange(&s_ondemand_log_ready, 0))
            datamodel_append_ondemand_log(s_ondemand_log_act, s_ondemand_log_res);
        if (atomic_exchange(&s_ondemand_status_ready, 0))
            datamodel_set_status(s_ondemand_status_val);
        if (atomic_exchange(&s_reset_target, 0))
            datamodel_reset_on_demand_target();

        /* Drain on-demand recovery results (ProcessRestart / CacheClear) */
        if (atomic_exchange(&s_ondemand_result_ready, 0)) {
            datamodel_set_action_type("None");
            datamodel_record_action(&s_ondemand_result);
            datamodel_increment_anomaly_count();
            datamodel_append_anomaly_log(&s_ondemand_event,
                action_type_to_string(s_ondemand_result.action),
                s_ondemand_result.result == RESULT_SUCCESS ? "Success" : "Failure");
        }

        /* Update data model stats at poll_interval_s cadence, not every 100ms */
        time_t now = time(NULL);
        if (now - last_stats_update >= (time_t)cfg.poll_interval_s) {
            last_stats_update = now;
            MetricSnapshot snap;
            if (monitor_peek_latest(&snap)) {
                datamodel_update_stats(snap.sys_cpu_pct, snap.sys_mem_pct,
                                       snap.sys_mem_free_kb);
            }
            datamodel_update_uptime((uint32_t)(time(NULL) - start_time));
            datamodel_update_self_stats();
            datamodel_sync_counters();
        }

        /* Deferred reboot countdown — tick every 100ms iteration */
        if (atomic_load_explicit(&g_reboot_pending, memory_order_acquire)
            && s_reboot_ticks > 0) {
            s_reboot_ticks--;
            if (s_reboot_ticks == 0) {
                LOG_INFO("Reboot countdown complete — executing reboot");
                /* Write flag file on persistent storage so next boot knows
                 * this was a HGWDoctor-triggered reboot. */
                FILE *rf = fopen(HGW_REBOOT_PENDING_FILE, "w");
                if (rf) fclose(rf);
                watchdog_close();
                recovery_do_reboot(s_scripts_dir);
                /* Should not reach here; clear pending state if reboot fails */
                LOG_WARN("Reboot command returned unexpectedly — clearing pending state");
                unlink(HGW_REBOOT_PENDING_FILE);
                atomic_store_explicit(&g_reboot_pending, 0, memory_order_relaxed);
                datamodel_append_ondemand_log("Reboot", "Failure");
                datamodel_set_status(STATUS_ENABLED);
            }
        }

        /* Pet the hardware watchdog at a fixed interval independent of poll_interval_s */
        {
            time_t now_wd = time(NULL);
            if (now_wd - last_wd_pet >= WD_PET_INTERVAL_S) {
                last_wd_pet = now_wd;
                watchdog_pet();
            }
        }

        /* Process ubus events; fallback sleep ensures we never busy-spin.
         * poll() has no fd-number upper limit unlike FD_SET/select. */
        bool did_sleep = false;
        if (g_bus_ctx != NULL) {
            int fd = amxb_get_fd(g_bus_ctx);
            if (fd >= 0) {
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN))
                    amxb_read(g_bus_ctx);
                did_sleep = true;
            }
        }
        if (!did_sleep)
            usleep(100000);
        amxp_signal_read();
    }

    /* 8. Graceful shutdown */
    watchdog_close();  /* disarm before slow cleanup — writes "V" magic char */
    LOG_INFO("HGW-Doctor shutting down");
    datamodel_set_status("Disabled");

    /* Disconnect from ubus immediately so the bus stops routing commands to us.
     * Any ubus call arriving after this gets an instant "not found" rather than
     * hanging until the per-call timeout fires while we do slow cleanup below. */
    if (g_bus_ctx) {
        amxb_disconnect(g_bus_ctx);
        amxb_free(&g_bus_ctx);
        g_bus_ctx = NULL;
    }
    amxb_be_remove_all();

    analyzer_stop();
    monitor_stop();
    recovery_cleanup();
    diag_collector_cleanup();
    uploader_cleanup();
    datamodel_cleanup(&g_dm, &g_parser);
    amxo_parser_clean(&g_parser);
    amxd_dm_clean(&g_dm);

    /* Remove PID file and any leftover trigger files */
    unlink(HGW_PID_FILE);
    unlink("/tmp/hgw_cfg_changed");
    unlink("/tmp/hgw_diag_trigger");

    pthread_mutex_destroy(&s_event_mutex);
    pthread_mutex_destroy(&s_pending_mutex);
    logger_cleanup();

    return EXIT_SUCCESS;
}