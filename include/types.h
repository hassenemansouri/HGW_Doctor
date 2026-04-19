/**
 * @file types.h
 * @brief Shared data structures used across all HGW-Doctor modules.
 */

#ifndef HGW_DOCTOR_TYPES_H
#define HGW_DOCTOR_TYPES_H

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */
#define HGW_MAX_PROC_NAME       64
#define HGW_MAX_PROC_LIST       16
#define HGW_MAX_PATH            256
#define HGW_MAX_URL             512
#define HGW_MAX_STATUS_STR      32
#define HGW_CIRC_BUF_SIZE       120   /**< 10 min at 5s interval            */
#define HGW_ANOMALY_LOG_MAX     50    /**< max AnomalyLog entries in data model */

/* -------------------------------------------------------------------------
 * Per-process metrics snapshot
 * ------------------------------------------------------------------------- */
typedef struct {
    char     name[HGW_MAX_PROC_NAME];
    pid_t    pid;
    bool     alive;
    uint32_t cpu_pct;
    uint32_t mem_pct;
} ProcessStat;

/* -------------------------------------------------------------------------
 * Metric snapshot - produced by monitor.c, consumed by analyzer.c
 * ------------------------------------------------------------------------- */
typedef struct {
    struct timespec ts;
    uint32_t        sys_cpu_pct;
    uint32_t        sys_mem_pct;
    uint32_t        sys_mem_free_kb;
    uint32_t        sys_mem_total_kb;
    ProcessStat     procs[HGW_MAX_PROC_LIST];
    int             proc_count;
} MetricSnapshot;

typedef struct {
    MetricSnapshot  slots[HGW_CIRC_BUF_SIZE];
    int             head;          /**< next write index                      */
    int             tail;          /**< reserved for future consumers         */
    pthread_mutex_t buf_mutex;     /**< protects head/slots against monitor+analyzer concurrent access */
} MetricCircBuf;

/* -------------------------------------------------------------------------
 * Anomaly types
 * ------------------------------------------------------------------------- */
typedef enum {
    ANOMALY_NONE        = 0,
    ANOMALY_CPU         = 1,
    ANOMALY_MEMORY      = 2,
    ANOMALY_PROCESS     = 3,
    ANOMALY_PROCESS_CPU = 4,
    ANOMALY_PROCESS_MEM = 5,
} AnomalyType;

/* -------------------------------------------------------------------------
 * Anomaly event - produced by analyzer.c, consumed by recovery.c
 * ------------------------------------------------------------------------- */
typedef struct {
    AnomalyType  type;
    uint32_t     metric_value;                    /**< Observed value at detection time      */
    uint32_t     duration_s;                      /**< How long threshold was exceeded (s)   */
    struct timespec detected_at;                  /**< Wall-clock time of detection          */
    char         process_name[HGW_MAX_PROC_NAME]; /**< process involved (ANOMALY_PROCESS*)   */
} AnomalyEvent;

/* -------------------------------------------------------------------------
 * Recovery action types
 * ------------------------------------------------------------------------- */
typedef enum {
    ACTION_NONE             = 0,
    ACTION_PROCESS_RESTART  = 1,
    ACTION_CACHE_CLEAR      = 2,
    ACTION_REBOOT           = 3,
} ActionType;

/* -------------------------------------------------------------------------
 * Recovery result - produced by recovery.c, fed back to datamodel.c
 * ------------------------------------------------------------------------- */
typedef enum {
    RESULT_NONE       = 0,
    RESULT_SUCCESS    = 1,
    RESULT_FAILURE    = 2,
    RESULT_IN_PROGRESS = 3,
} ActionResult;

typedef struct {
    ActionType      action;
    ActionResult    result;
    struct timespec executed_at;
    char            process_name[HGW_MAX_PROC_NAME];
    int             exit_code;    /**< Exit code of helper script            */
} RecoveryResult;

typedef enum {
    UPLOAD_STATUS_NONE    = 0,
    UPLOAD_STATUS_PENDING = 1,
    UPLOAD_STATUS_SUCCESS = 2,
    UPLOAD_STATUS_FAILED  = 3,
} UploadStatus;

#endif /* HGW_DOCTOR_TYPES_H */
