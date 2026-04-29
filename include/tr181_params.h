/**
 * @file tr181_params.h
 * @brief String constants for all TR-181 parameter paths used by HGW-Doctor.
 *
 * Centralising path strings avoids typos scattered across modules.
 * Use these macros in all amxd_object_get_param / amxd_object_set_param calls.
 */

#ifndef HGW_TR181_PARAMS_H
#define HGW_TR181_PARAMS_H

/* Root object path */
#define TR181_ROOT          "HGWDoctor"

/* ── Global control ──────────────────────────────────────────────────────── */
#define TR181_ENABLE              TR181_ROOT ".Enable"
#define TR181_STATUS              TR181_ROOT ".Status"
#define TR181_PROFILE             TR181_ROOT ".Profile"

/* ── Thresholds ──────────────────────────────────────────────────────────── */
#define TR181_CPU_THRESHOLD       TR181_ROOT ".CPUThreshold"
#define TR181_MEM_THRESHOLD       TR181_ROOT ".MemThreshold"
#define TR181_THRESHOLD_DURATION  TR181_ROOT ".ThresholdDuration"
#define TR181_POLL_INTERVAL       TR181_ROOT ".PollInterval"

/* ── Recovery config ─────────────────────────────────────────────────────── */
#define TR181_ACTION_TYPE         TR181_ROOT ".ActionType"
#define TR181_PROCESS_LIST        TR181_ROOT ".ProcessList"

/* ── Recovery status (RO) ────────────────────────────────────────────────── */
#define TR181_LAST_ACTION_TYPE    TR181_ROOT ".LastActionType"
#define TR181_LAST_ACTION_TIME    TR181_ROOT ".LastActionTime"
#define TR181_LAST_ACTION_STATUS  TR181_ROOT ".LastActionStatus"
#define TR181_ANOMALY_COUNT       TR181_ROOT ".AnomalyCount"

/* ── Diagnostics ─────────────────────────────────────────────────────────── */
#define TR181_ON_DEMAND_TRIGGER   TR181_ROOT ".OnDemandTrigger"
#define TR181_DIAG_OUTPUT_DIR     TR181_ROOT ".DiagOutputDir"
#define TR181_DIAG_ARCHIVE_PATH   TR181_ROOT ".DiagArchivePath"
#define TR181_UPLOAD_URL          TR181_ROOT ".UploadURL"
#define TR181_UPLOAD_STATUS       TR181_ROOT ".UploadStatus"
#define TR181_UPLOAD_TIMESTAMP    TR181_ROOT ".UploadTimestamp"

/* ── Statistics ──────────────────────────────────────────────────────────── */
#define TR181_STAT_CPU            TR181_ROOT ".Stats.CurrentCPUUsage"
#define TR181_STAT_MEM            TR181_ROOT ".Stats.CurrentMemUsage"
#define TR181_STAT_MEM_FREE       TR181_ROOT ".Stats.CurrentMemFreeKB"
#define TR181_STAT_TOTAL_ACTIONS  TR181_ROOT ".Stats.TotalRecoveryActions"
#define TR181_STAT_TOTAL_UPLOADS  TR181_ROOT ".Stats.TotalDiagUploads"
#define TR181_STAT_UPTIME         TR181_ROOT ".Stats.UptimeSeconds"

/* ── Self stats (hgw-doctor own resource usage) ──────────────────────────── */
#define TR181_SELF_CPU            TR181_ROOT ".SelfStats.CurrentCPUUsage"
#define TR181_SELF_MEM            TR181_ROOT ".SelfStats.CurrentMemUsage"
#define TR181_SELF_RSS_KB         TR181_ROOT ".SelfStats.CurrentRSSKB"

/* ── Profiles table ──────────────────────────────────────────────────────── */
#define TR181_PROFILES            TR181_ROOT ".Profiles"

/* ── AnomalyLog table ────────────────────────────────────────────────────── */
#define TR181_ANOMALY_LOG         TR181_ROOT ".AnomalyLog"

/* ── String values for ActionType parameter ─────────────────────────────── */
#define ACTSTR_NONE               "None"
#define ACTSTR_PROCESS_RESTART    "ProcessRestart"
#define ACTSTR_CACHE_CLEAR        "CacheClear"
#define ACTSTR_REBOOT             "Reboot"

/* ── String values for Status parameter ─────────────────────────────────── */
#define STATUS_ENABLED            "Enabled"
#define STATUS_DISABLED           "Disabled"
#define STATUS_ERROR              "Error"

/* ── String values for UploadStatus / LastActionStatus ──────────────────── */
#define RESULT_STR_NONE           "None"
#define RESULT_STR_SUCCESS        "Success"
#define RESULT_STR_FAILURE        "Failure"
#define RESULT_STR_IN_PROGRESS    "InProgress"
#define RESULT_STR_PENDING        "Pending"

#endif /* HGW_TR181_PARAMS_H */
