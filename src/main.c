/**
 * @file main.c
 * @brief HGW-Doctor daemon entry point and main event loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

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
static volatile sig_atomic_t g_running    = 1;
static volatile sig_atomic_t g_reload_cfg = 0;
static volatile sig_atomic_t g_diag_req   = 0;  /* set by SIGUSR1 */

static amxd_dm_t      g_dm;
static amxo_parser_t  g_parser;
static MetricCircBuf  g_metric_buf;
static amxb_bus_ctx_t *g_bus_ctx = NULL;
static AnomalyEvent   s_last_event;

static const char *action_type_to_string(ActionType action) {
    switch (action) {
        case ACTION_PROCESS_RESTART: return "ProcessRestart";
        case ACTION_CACHE_CLEAR:     return "CacheClear";
        case ACTION_REBOOT:          return "Reboot";
        case ACTION_NONE:
        default:                     return "None";
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
    LOG_WARN("Anomaly detected: type=%d value=%u%% duration=%us",
             event->type, event->metric_value, event->duration_s);

    s_last_event = *event;

    datamodel_increment_anomaly_count();
    diag_collect(event);
    recovery_dispatch(event);
}

/* -------------------------------------------------------------------------
 * Recovery done callback (recovery → datamodel)
 * ------------------------------------------------------------------------- */
static void on_recovery_done(const RecoveryResult *result, void *userdata) {
    (void)userdata;
    datamodel_record_action(result);
    datamodel_append_anomaly_log(
        &s_last_event,
        action_type_to_string(result->action),
        result->result == RESULT_SUCCESS ? "Success" : "Failure"
    );
}

/* -------------------------------------------------------------------------
 * Diagnostics done callback (diag_collector → uploader)
 * ------------------------------------------------------------------------- */
static void on_diag_done(const char *archive_path, void *userdata) {
    (void)userdata;
    LOG_INFO("Diagnostics archive ready: %s", archive_path);
    datamodel_record_upload(UPLOAD_STATUS_PENDING, archive_path);
    uploader_send(archive_path);
}

/* -------------------------------------------------------------------------
 * Upload done callback (uploader → datamodel)
 * ------------------------------------------------------------------------- */
static void on_upload_done(UploadStatus status, const char *path, void *userdata) {
    (void)userdata;

    datamodel_record_upload(status, path);

    if (status == UPLOAD_STATUS_SUCCESS) {
        LOG_INFO("Upload succeeded: %s", path ? path : "<unknown>");
    } else {
        LOG_WARN("Upload failed for: %s", path ? path : "<unknown>");
    }
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

    /* 3. Signal handling */
    install_signals();

    /* 4. Ambiorix data model */
    amxd_dm_init(&g_dm);
    amxo_parser_init(&g_parser);
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
        } else {
            LOG_WARN("Failed to register data model on ubus");
        }
    } else {
        LOG_WARN("Failed to connect to ubus");
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
    datamodel_set_config(proc_list, cfg.cpu_threshold_pct,
                         cfg.mem_threshold_pct, cfg.threshold_duration_s,
                         cfg.poll_interval_s);

    RecoveryConfig rcfg = {0};
    rcfg.action_type   = cfg.action_type;
    rcfg.process_count = cfg.process_count;
    memcpy(rcfg.process_names, cfg.process_names, sizeof(rcfg.process_names));
    __builtin_strncpy(rcfg.scripts_dir, cfg.scripts_dir, HGW_MAX_PATH - 1);
    recovery_init(&rcfg, on_recovery_done, NULL);

    DiagConfig dcfg = {0};
    dcfg.max_archives = cfg.diag_max_archives;
    dcfg.watch_pid    = 0;
    __builtin_strncpy(dcfg.output_dir, cfg.diag_output_dir, HGW_MAX_PATH - 1);
    diag_collector_init(&dcfg, on_diag_done, NULL);

    UploaderConfig ucfg = {0};
    ucfg.timeout_s      = cfg.upload_timeout_s;
    ucfg.max_retries    = cfg.upload_max_retries;
    ucfg.retry_delay_s  = cfg.upload_retry_delay_s;
    ucfg.tls_verify     = cfg.tls_verify;
    __builtin_strncpy(ucfg.url,          cfg.upload_url,   HGW_MAX_URL - 1);
    __builtin_strncpy(ucfg.ca_cert_path, cfg.ca_cert_path, HGW_MAX_PATH - 1);
    uploader_init(&ucfg, on_upload_done, NULL);

    /* 6. Start threads */
    monitor_start();
    analyzer_start();
    datamodel_set_status("Enabled");

    LOG_INFO("HGW-Doctor running");

    /* 7. Main event loop */
    uint32_t uptime_s = 0;
    while (g_running) {
        if (g_reload_cfg) { g_reload_cfg = 0; config_reload(); }
        if (g_diag_req) {
            g_diag_req = 0;
            LOG_INFO("On-demand diagnostics requested via SIGUSR1");
            diag_collect(NULL);
        }

        MetricSnapshot snap;
        if (monitor_peek_latest(&snap)) {
            datamodel_update_stats(snap.sys_cpu_pct, snap.sys_mem_pct,
                                   snap.sys_mem_free_kb);
        }
        uptime_s++;
        datamodel_update_uptime(uptime_s);

        /* Process ubus events */
        if (g_bus_ctx != NULL) {
            int fd = amxb_get_fd(g_bus_ctx);
            if (fd >= 0) {
                fd_set rfds;
                struct timeval tv = {0, 100000}; /* 100ms */
                FD_ZERO(&rfds);
                FD_SET(fd, &rfds);
                if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0) {
                    amxb_read(g_bus_ctx);
                }
            }
        }
        amxp_sigmngr_handle(NULL);
    }

    /* 8. Graceful shutdown */
    LOG_INFO("HGW-Doctor shutting down");
    datamodel_set_status("Disabled");
    analyzer_stop();
    monitor_stop();
    recovery_cleanup();
    diag_collector_cleanup();
    uploader_cleanup();
    if (g_bus_ctx) {
        amxb_disconnect(g_bus_ctx);
        amxb_free(&g_bus_ctx);
    }
    amxb_be_remove_all();
    datamodel_cleanup(&g_dm, &g_parser);
    amxo_parser_clean(&g_parser);
    amxd_dm_clean(&g_dm);
    logger_cleanup();

    return EXIT_SUCCESS;
}