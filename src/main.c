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
static volatile sig_atomic_t g_running          = 1;
static volatile sig_atomic_t g_reload_cfg       = 0;
static volatile sig_atomic_t g_diag_req         = 0;  /* set by SIGUSR1 — manual trigger */
static volatile sig_atomic_t g_anomaly_diag     = 0;  /* set by on_anomaly — anomaly trigger */
static volatile int          g_monitoring_enabled = 1; /* cleared when Enable=false written to DM */

static amxd_dm_t        g_dm;
static amxo_parser_t    g_parser;
static MetricCircBuf    g_metric_buf;
static amxb_bus_ctx_t  *g_bus_ctx = NULL;
static AnomalyEvent     s_last_event;
static pthread_mutex_t  s_event_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    *count = 0;
    if (!list || list[0] == '\0') return;
    strncpy(tmp, list, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *tok = strtok(tmp, ",");
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
        tok = strtok(NULL, ",");
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
    if (!g_monitoring_enabled) return;
    LOG_WARN("Anomaly detected: type=%d value=%u%% duration=%us",
             event->type, event->metric_value, event->duration_s);

    /* Write s_last_event and set g_anomaly_diag under lock so the main thread
     * always sees a consistent snapshot (memory ordering guarantee). */
    pthread_mutex_lock(&s_event_mutex);
    s_last_event = *event;
    g_anomaly_diag = 1;
    pthread_mutex_unlock(&s_event_mutex);

    datamodel_increment_anomaly_count();
    recovery_dispatch(event);
}

/* -------------------------------------------------------------------------
 * Recovery done callback (recovery → datamodel)
 * ------------------------------------------------------------------------- */
static void on_recovery_done(const RecoveryResult *result, void *userdata) {
    (void)userdata;
    datamodel_record_action(result);
    /* Take a local copy of s_last_event under lock to avoid racing with the
     * main thread reading it in the event loop. */
    AnomalyEvent ev_copy;
    pthread_mutex_lock(&s_event_mutex);
    ev_copy = s_last_event;
    pthread_mutex_unlock(&s_event_mutex);
    datamodel_append_anomaly_log(
        &ev_copy,
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
    datamodel_set_process_list(proc_list);
    datamodel_sync_startup(action_type_to_string(cfg.action_type),
                            cfg.upload_url, cfg.diag_output_dir);

    RecoveryConfig rcfg = {0};
    rcfg.action_type   = cfg.action_type;
    rcfg.process_count = cfg.process_count;
    memcpy(rcfg.process_names, cfg.process_names, sizeof(rcfg.process_names));
    strncpy(rcfg.scripts_dir, cfg.scripts_dir, HGW_MAX_PATH - 1);
    recovery_init(&rcfg, on_recovery_done, NULL);

    DiagConfig dcfg = {0};
    dcfg.max_archives = cfg.diag_max_archives;
    dcfg.watch_pid    = 0;
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
    monitor_start();
    analyzer_start();
    write_pid_file();
    datamodel_set_status("Enabled");

    LOG_INFO("HGW-Doctor running");

    /* 7. Main event loop */
    time_t start_time = time(NULL);
    time_t last_stats_update = 0;
    time_t last_diag_collect = 0;
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

            LOG_INFO("Config reloaded: all modules updated");
        }
        if (g_diag_req) {
            g_diag_req = 0;
            LOG_INFO("On-demand diagnostics requested via SIGUSR1");
            diag_collect(NULL);
        }
        /* Check for trigger file written by dm_trigger_diagnostics() RPC */
        if (access("/tmp/hgw_diag_trigger", F_OK) == 0) {
            unlink("/tmp/hgw_diag_trigger");
            LOG_INFO("On-demand diagnostics triggered via TR-181 RPC");
            diag_collect(NULL);
        }
        /* Propagate any DM write (ACS/ubus) to the running daemon */
        if (access("/tmp/hgw_cfg_changed", F_OK) == 0) {
            unlink("/tmp/hgw_cfg_changed");
            DmConfig dmc = {0};
            if (datamodel_get_config(&dmc) && dmc.poll_interval_s > 0) {
                /* Resolve process list: DM value takes priority, fall back to file cfg */
                char dm_proc_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME];
                int  dm_proc_count = 0;
                parse_process_list(dmc.process_list, dm_proc_names, &dm_proc_count);
                if (dm_proc_count == 0) {
                    memcpy(dm_proc_names, cfg.process_names, sizeof(dm_proc_names));
                    dm_proc_count = cfg.process_count;
                }

                AnalyzerConfig acfg2 = {0};
                acfg2.cpu_threshold_pct    = dmc.cpu_threshold_pct;
                acfg2.mem_threshold_pct    = dmc.mem_threshold_pct;
                acfg2.threshold_duration_s = dmc.threshold_duration_s;
                acfg2.poll_interval_s      = dmc.poll_interval_s;
                acfg2.process_count        = dm_proc_count;
                memcpy(acfg2.process_names, dm_proc_names, sizeof(acfg2.process_names));
                analyzer_update_config(&acfg2);
                monitor_update_config(
                    (const char (*)[HGW_MAX_PROC_NAME]) dm_proc_names,
                    dm_proc_count, dmc.poll_interval_s);

                RecoveryConfig rcfg2 = {0};
                rcfg2.action_type   = action_str_to_type(dmc.action_type);
                rcfg2.process_count = dm_proc_count;
                memcpy(rcfg2.process_names, dm_proc_names, sizeof(rcfg2.process_names));
                strncpy(rcfg2.scripts_dir, cfg.scripts_dir, HGW_MAX_PATH - 1);
                recovery_update_config(&rcfg2);

                if (dmc.upload_url[0] != '\0')
                    uploader_update_url(dmc.upload_url);

                if (dmc.diag_output_dir[0] != '\0')
                    diag_collector_update_output_dir(dmc.diag_output_dir);

                /* Enable / disable monitoring */
                if (!dmc.enable && g_monitoring_enabled) {
                    g_monitoring_enabled = 0;
                    datamodel_set_status(STATUS_DISABLED);
                    LOG_INFO("Monitoring disabled via DM");
                } else if (dmc.enable && !g_monitoring_enabled) {
                    g_monitoring_enabled = 1;
                    datamodel_set_status(STATUS_ENABLED);
                    LOG_INFO("Monitoring re-enabled via DM");
                }

                LOG_INFO("Live config updated from DM: "
                         "cpu=%u mem=%u dur=%u poll=%u action=%s procs=%d enable=%d",
                         dmc.cpu_threshold_pct, dmc.mem_threshold_pct,
                         dmc.threshold_duration_s, dmc.poll_interval_s,
                         dmc.action_type, dm_proc_count, dmc.enable);
            }
        }
        if (g_anomaly_diag) {
            /* Take a consistent copy of the event under lock */
            AnomalyEvent ev_copy;
            pthread_mutex_lock(&s_event_mutex);
            g_anomaly_diag = 0;
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
        }

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
        amxp_signal_read();
    }

    /* 8. Graceful shutdown */
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
    logger_cleanup();

    return EXIT_SUCCESS;
}