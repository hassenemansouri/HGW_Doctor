#ifndef HGW_ANALYZER_H
#define HGW_ANALYZER_H

#include <stdint.h>

#include "types.h"

typedef struct {
    uint32_t cpu_threshold_pct;
    uint32_t mem_threshold_pct;
    uint32_t threshold_duration_s;
    uint32_t poll_interval_s;
    char     process_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME];
    int      process_count;
} AnalyzerConfig;

typedef void (*anomaly_callback)(const AnomalyEvent *event, void *userdata);

int  analyzer_init(MetricCircBuf *buf, const AnalyzerConfig *cfg,
                   anomaly_callback cb, void *userdata);
int  analyzer_start(void);
void analyzer_stop(void);
void analyzer_update_config(const AnalyzerConfig *cfg);

#endif /* HGW_ANALYZER_H */