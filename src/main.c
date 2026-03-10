/**
 * @file main.c
 * @brief HGW-Doctor daemon entry point.
 *
 * Responsibilities:
 *  1. Parse command-line arguments.
 *  2. Load configuration (config.c).
 *  3. Initialise the Ambiorix data model (datamodel.c).
 *  4. Initialise and start all worker modules (monitor, analyzer, recovery,
 *     diag_collector, uploader).
 *  5. Install signal handlers (SIGTERM/SIGINT for clean shutdown, SIGHUP for
 *     config reload).
 *  6. Enter the Ambiorix event loop (amxrt_run or manual el_add_fd loop).
 *  7. On exit: stop all modules, deregister data model, save persistent state.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include <amxd/amxd_dm.h>
#include <amxo/amxo.h>

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

static amxd_dm_t      g_dm;
static amxo_parser_t  g_parser;
static MetricCircBuf  g_metric_buf;

/* -------------------------------------------------------------------------
 * Signal handlers
 * ------------------------------------------------------------------------- */
static void sig_handler(int signo) {
    if (signo == SIGTERM || signo == SIGINT) {
        g_running = 0;
    } else if (signo == SIGHUP) {
        g_reload_cfg = 1;
    } else if (signo == SIGUSR1) {
        /* Manual on-demand diagnostics trigger from shell */
        diag_collect(NULL);
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
static void on_upload_done(UploadStatus status, const char *path, void *ud) {
    (void)ud;
    datamodel_record_upload(status, path);
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

    /* 5. Worker modules */
    monitor_init(&g_metric_buf, cfg.process_name, cfg.poll_interval_s);

    AnalyzerConfig acfg = {
        .cpu_threshold_pct   = cfg.cpu_threshold_pct,
        .mem_threshold_pct   = cfg.mem_threshold_pct,
        .threshold_duration_s = cfg.threshold_duration_s,
        .poll_interval_s     = cfg.poll_interval_s,
    };
    analyzer_init(&g_metric_buf, &acfg, on_anomaly, NULL);

    RecoveryConfig rcfg = { .action_type = cfg.action_type, .scripts_dir = "" };
    __builtin_strncpy(rcfg.process_name, cfg.process_name, HGW_MAX_PROC_NAME - 1);
    __builtin_strncpy(rcfg.scripts_dir,  cfg.scripts_dir,  HGW_MAX_PATH - 1);
    recovery_init(&rcfg, on_recovery_done, NULL);

    DiagConfig dcfg = {
        .max_archives = cfg.diag_max_archives,
        .watch_pid    = 0,
    };
    __builtin_strncpy(dcfg.output_dir, cfg.diag_output_dir, HGW_MAX_PATH - 1);
    diag_collector_init(&dcfg, on_diag_done, NULL);

    UploaderConfig ucfg = {
        .timeout_s      = cfg.upload_timeout_s,
        .max_retries    = cfg.upload_max_retries,
        .retry_delay_s  = cfg.upload_retry_delay_s,
        .tls_verify     = cfg.tls_verify,
    };
    __builtin_strncpy(ucfg.url,          cfg.upload_url,    HGW_MAX_URL - 1);
    __builtin_strncpy(ucfg.ca_cert_path, cfg.ca_cert_path,  HGW_MAX_PATH - 1);
    uploader_init(&ucfg, on_upload_done, NULL);

    /* 6. Start threads */
    monitor_start();
    analyzer_start();
    datamodel_set_status("Enabled");

    LOG_INFO("HGW-Doctor running");

    /* 7. Main event loop */
    while (g_running) {
        if (g_reload_cfg) {
            g_reload_cfg = 0;
            config_reload();
        }
        /* TODO: replace sleep with amxrt event loop when bus backend integrated */
        sleep(1);
    }

    /* 8. Graceful shutdown */
    LOG_INFO("HGW-Doctor shutting down");
    datamodel_set_status("Disabled");
    analyzer_stop();
    monitor_stop();
    recovery_cleanup();
    diag_collector_cleanup();
    uploader_cleanup();
    datamodel_cleanup(&g_dm, &g_parser);
    amxo_parser_clean(&g_parser);
    amxd_dm_clean(&g_dm);
    logger_cleanup();

    return EXIT_SUCCESS;
}
