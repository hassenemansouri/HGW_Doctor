/**
 * @file analyzer.h
 * @brief Common type definitions for HGW-Doctor.
 */

#ifndef HGW_ANALYZER_H
#define HGW_ANALYZER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Circular buffer for metric snapshots
 * ------------------------------------------------------------------------- */
#define HGW_CIRC_BUF_SIZE 60  /* e.g., 60 samples at 1s interval = 1 minute */

typedef struct {
    struct timespec ts;        /* monotonic timestamp */
    uint32_t cpu_pct;          /* CPU usage percentage (0-100) */
    uint32_t mem_used_pct;     /* Memory used percentage (0-100) */
    uint32_t mem_free_kb;      /* Free memory in kB */
    uint32_t mem_total_kb;     /* Total memory in kB */
    bool     proc_alive;       /* Whether monitored process is alive */
    pid_t    proc_pid;         /* PID of found process (0 if not found) */
} MetricSnapshot;

typedef struct {
    MetricSnapshot slots[HGW_CIRC_BUF_SIZE];
    int head;                  /* next write index */
    int tail;                  /* oldest read index (not used in this design) */
} MetricCircBuf;

/* -------------------------------------------------------------------------
 * Anomaly types
 * ------------------------------------------------------------------------- */
typedef enum {
    ANOMALY_CPU,
    ANOMALY_MEMORY,
    ANOMALY_PROCESS
} AnomalyType;

typedef struct {
    AnomalyType type;
    uint32_t    metric_value;  /* e.g., CPU% at detection */
    uint32_t    duration_s;    /* sustained duration that triggered */
    time_t      timestamp;     /* time of detection (optional) */
} AnomalyEvent;

/* -------------------------------------------------------------------------
 * Recovery action types and results
 * ------------------------------------------------------------------------- */
typedef enum {
    ACTION_NONE,
    ACTION_PROCESS_RESTART,
    ACTION_CACHE_CLEAR,
    ACTION_REBOOT
} ActionType;

typedef enum {
    RESULT_NONE,
    RESULT_SUCCESS,
    RESULT_FAILURE,
    RESULT_IN_PROGRESS
} RecoveryResultStatus;

typedef struct {
    ActionType           action;
    RecoveryResultStatus result;
    time_t               timestamp;
} RecoveryResult;

/* -------------------------------------------------------------------------
 * Upload status
 * ------------------------------------------------------------------------- */
typedef enum {
    UPLOAD_STATUS_NONE,
    UPLOAD_STATUS_PENDING,
    UPLOAD_STATUS_SUCCESS,
    UPLOAD_STATUS_FAILED
} UploadStatus;

#endif /* HGW_ANALYZER_H */