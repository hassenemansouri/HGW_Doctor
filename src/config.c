#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "logger.h"

static HgwConfig s_cfg;
static char      s_conf_path[HGW_MAX_PATH];

static char *trim(char *s) {
    char *end;

    while (*s && isspace((unsigned char) *s)) s++;
    if (*s == '\0') return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char) *end)) *end-- = '\0';
    return s;
}

static ActionType parse_action_type(const char *value) {
    if (strcasecmp(value, "ProcessRestart") == 0) return ACTION_PROCESS_RESTART;
    if (strcasecmp(value, "CacheClear") == 0) return ACTION_CACHE_CLEAR;
    if (strcasecmp(value, "Reboot") == 0) return ACTION_REBOOT;
    return ACTION_NONE;
}

static uint32_t parse_uint(const char *value, uint32_t fallback) {
    char *end;
    if (!value || value[0] == '\0') return fallback;
    errno = 0;
    unsigned long v = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') return fallback;
    return (uint32_t)v;
}

static bool parse_bool(const char *value) {
    return strcasecmp(value, "1") == 0 ||
           strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "yes") == 0 ||
           strcasecmp(value, "on") == 0;
}

static void config_defaults(HgwConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->cpu_threshold_pct = 90;
    cfg->mem_threshold_pct = 90;
    cfg->threshold_duration_s = 60;
    cfg->poll_interval_s = 5;
    cfg->action_type = ACTION_PROCESS_RESTART;
    cfg->diag_max_archives = 5;
    cfg->upload_timeout_s = 30;
    cfg->upload_max_retries = 3;
    cfg->upload_retry_delay_s = 5;
    cfg->tls_verify = true;

    strncpy(cfg->scripts_dir, "/usr/lib/hgw_doctor/actions", sizeof(cfg->scripts_dir) - 1);
    cfg->scripts_dir[sizeof(cfg->scripts_dir) - 1] = '\0';
    strncpy(cfg->diag_output_dir, "/tmp/hgw_diag", sizeof(cfg->diag_output_dir) - 1);
    cfg->diag_output_dir[sizeof(cfg->diag_output_dir) - 1] = '\0';
    strncpy(cfg->upload_url, "https://diagnostics.telnet.tn/upload",
            sizeof(cfg->upload_url) - 1);
    cfg->upload_url[sizeof(cfg->upload_url) - 1] = '\0';
    strncpy(cfg->odl_path, "/etc/amx/hgw_doctor/hgw_doctor.odl", sizeof(cfg->odl_path) - 1);
    cfg->odl_path[sizeof(cfg->odl_path) - 1] = '\0';
}

static void apply_key_value(HgwConfig *cfg, const char *key, const char *value) {
    if (strcasecmp(key, "CPUThreshold") == 0) cfg->cpu_threshold_pct = parse_uint(value, cfg->cpu_threshold_pct);
    else if (strcasecmp(key, "MemThreshold") == 0) cfg->mem_threshold_pct = parse_uint(value, cfg->mem_threshold_pct);
    else if (strcasecmp(key, "ThresholdDuration") == 0) cfg->threshold_duration_s = parse_uint(value, cfg->threshold_duration_s);
    else if (strcasecmp(key, "PollInterval") == 0) cfg->poll_interval_s = parse_uint(value, cfg->poll_interval_s);
    else if (strcasecmp(key, "ActionType") == 0) cfg->action_type = parse_action_type(value);
    else if (strcasecmp(key, "ProcessList") == 0) {
        char tmp[HGW_MAX_PROC_LIST * HGW_MAX_PROC_NAME];
        char *saveptr = NULL;
        strncpy(tmp, value, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *token = strtok_r(tmp, ",", &saveptr);
        cfg->process_count = 0;
        while (token && cfg->process_count < HGW_MAX_PROC_LIST) {
            char *trimmed = token;
            while (*trimmed == ' ') trimmed++;
            char *end = trimmed + strlen(trimmed);
            while (end > trimmed && *(end - 1) == ' ') end--;
            *end = '\0';
            if (*trimmed != '\0') {
                strncpy(cfg->process_names[cfg->process_count], trimmed, HGW_MAX_PROC_NAME - 1);
                cfg->process_names[cfg->process_count][HGW_MAX_PROC_NAME - 1] = '\0';
                cfg->process_count++;
            }
            token = strtok_r(NULL, ",", &saveptr);
        }
    }
    else if (strcasecmp(key, "ScriptsDir") == 0) {
        strncpy(cfg->scripts_dir, value, sizeof(cfg->scripts_dir) - 1);
        cfg->scripts_dir[sizeof(cfg->scripts_dir) - 1] = '\0';
    }
    else if (strcasecmp(key, "DiagOutputDir") == 0) {
        strncpy(cfg->diag_output_dir, value, sizeof(cfg->diag_output_dir) - 1);
        cfg->diag_output_dir[sizeof(cfg->diag_output_dir) - 1] = '\0';
    }
    else if (strcasecmp(key, "DiagMaxArchives") == 0) cfg->diag_max_archives = parse_uint(value, cfg->diag_max_archives);
    else if (strcasecmp(key, "UploadURL") == 0) {
        strncpy(cfg->upload_url, value, sizeof(cfg->upload_url) - 1);
        cfg->upload_url[sizeof(cfg->upload_url) - 1] = '\0';
    }
    else if (strcasecmp(key, "UploadTimeout") == 0) cfg->upload_timeout_s = parse_uint(value, cfg->upload_timeout_s);
    else if (strcasecmp(key, "UploadMaxRetries") == 0) cfg->upload_max_retries = parse_uint(value, cfg->upload_max_retries);
    else if (strcasecmp(key, "UploadRetryDelay") == 0) cfg->upload_retry_delay_s = parse_uint(value, cfg->upload_retry_delay_s);
    else if (strcasecmp(key, "TLSVerify") == 0) cfg->tls_verify = parse_bool(value);
    else if (strcasecmp(key, "CACertPath") == 0) {
        strncpy(cfg->ca_cert_path, value, sizeof(cfg->ca_cert_path) - 1);
        cfg->ca_cert_path[sizeof(cfg->ca_cert_path) - 1] = '\0';
    }
    else if (strcasecmp(key, "ODLPath") == 0) {
        strncpy(cfg->odl_path, value, sizeof(cfg->odl_path) - 1);
        cfg->odl_path[sizeof(cfg->odl_path) - 1] = '\0';
    }
}

int config_load(const char *conf_path, HgwConfig *cfg) {
    FILE *f;
    char line[1024];
    const char *path = conf_path ? conf_path : "/etc/hgw_doctor/hgw_doctor.conf";

    if (!cfg) return -1;

    config_defaults(cfg);
    f = fopen(path, "r");
    if (!f) {
        s_cfg = *cfg;
        if (conf_path) {
            strncpy(s_conf_path, conf_path, sizeof(s_conf_path) - 1);
            s_conf_path[sizeof(s_conf_path) - 1] = '\0';
        }
        return (conf_path == NULL || errno == ENOENT) ? 0 : -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *key;
        char *value;
        char *eq;

        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';') continue;

        eq = strchr(key, '=');
        if (!eq) continue;
        *eq = '\0';
        value = trim(eq + 1);
        key = trim(key);
        apply_key_value(cfg, key, value);
    }

    fclose(f);
    /* Guard against zero values that cause division by zero in the analyzer */
    if (cfg->poll_interval_s == 0)      cfg->poll_interval_s = 5;
    if (cfg->threshold_duration_s == 0) cfg->threshold_duration_s = 60;
    s_cfg = *cfg;
    strncpy(s_conf_path, path, sizeof(s_conf_path) - 1);
    s_conf_path[sizeof(s_conf_path) - 1] = '\0';
    LOG_INFO("Configuration loaded from %s", path);
    return 0;
}

void config_reload(void) {
    HgwConfig tmp;

    if (config_load(s_conf_path[0] ? s_conf_path : NULL, &tmp) == 0) {
        s_cfg = tmp;
        LOG_INFO("Configuration reloaded");
    } else {
        LOG_WARN("Configuration reload failed; keeping previous values");
    }
}

const HgwConfig *config_get(void) {
    return &s_cfg;
}