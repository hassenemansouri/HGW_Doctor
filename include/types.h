/**
 * @file types.h
 * @brief Shared data structures used across all HGW-Doctor modules.
 *
 * All inter-module communication uses these types.
 * No module should define its own structs for metrics or events.
 */

#ifndef HGW_DOCTOR_TYPES_H
#define HGW_DOCTOR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */
#define HGW_MAX_PROC_NAME       64
#define HGW_MAX_PATH            256
#define HGW_MAX_URL             512
#define HGW_MAX_STATUS_STR      32
#define HGW_CIRC_BUF_SIZE       120   /**< 10 min at 5s interval            */
#define HGW_ANOMALY_LOG_MAX     50    /**< max AnomalyLog entries in data model */

/* -------------------------------------------------------------------------
 * Metric snapshot - produced by monitor.c, consumed by analyzer.c
 * ------------------------------------------------------------------------- */
typedef struct {
    struct timespec ts;           /**< Monotonic timestamp of sample         */
    uint32_t        cpu_pct;      /**< CPU usage 0-100                       */
    uint32_t        mem_used_pct; /**< Memory usage 0-100                    */
    uint32_t        mem_free_kb;  /**< Free memory in kB                     */
    uint32_t        mem_total_kb; /**< Total memory in kB                    */
    bool            proc_alive;   /**< true if monitored process is running  */
    pid_t           proc_pid;     /**< PID of monitored process (0 if none)  */
} MetricSnapshot;

/* -------------------------------------------------------------------------
 * Anomaly types
 * ------------------------------------------------------------------------- */
typedef enum {
    ANOMALY_NONE    = 0,
    ANOMALY_CPU     = 1,
    ANOMALY_MEMORY  = 2,
    ANOMALY_PROCESS = 3,
} AnomalyType;

/* -------------------------------------------------------------------------
 * Anomaly event - produced by analyzer.c, consumed by recovery.c
 * ------------------------------------------------------------------------- */
typedef struct {
    AnomalyType  type;
    uint32_t     metric_value;    /**< Observed value at detection time      */
    uint32_t     duration_s;      /**< How long threshold was exceeded (s)   */
    struct timespec detected_at;  /**< Wall-clock time of detection          */
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

#endif /* HGW_DOCTOR_TYPES_H */
