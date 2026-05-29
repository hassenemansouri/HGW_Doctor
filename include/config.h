/**
 * @file config.h
 * @brief Configuration loading and management.
 */

#ifndef HGW_CONFIG_H
#define HGW_CONFIG_H

#include <stdbool.h>

#include "types.h"

/* Main configuration structure (loaded from file) */
typedef struct {
    /* Thresholds (global, may be overridden by profile) */
    uint32_t cpu_threshold_pct;
    uint32_t mem_threshold_pct;
    uint32_t threshold_duration_s;
    uint32_t poll_interval_s;

    /* Recovery */
    ActionType action_type;
    char       process_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME];
    int        process_count;
    char       scripts_dir[HGW_MAX_PATH];
    bool       allow_reboot;
    bool       enable_watchdog;

    /* Diagnostics */
    char       diag_output_dir[HGW_MAX_PATH];
    uint32_t   diag_max_archives;

    /* Upload */
    char       upload_url[HGW_MAX_URL];
    uint32_t   upload_timeout_s;
    uint32_t   upload_max_retries;
    uint32_t   upload_retry_delay_s;
    bool       tls_verify;
    char       ca_cert_path[HGW_MAX_PATH];

    /* ODL path (where the main ODL file resides) */
    char       odl_path[HGW_MAX_PATH];
} HgwConfig;

/* Load configuration from file (NULL = default) */
int config_load(const char *conf_path, HgwConfig *cfg);

/* Reload configuration (called on SIGHUP or after data model changes) */
void config_reload(void);

/* Get current config (for modules that need to read it) */
const HgwConfig* config_get(void);

#endif /* HGW_CONFIG_H */
