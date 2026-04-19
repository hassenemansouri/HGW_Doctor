#ifndef HGW_MONITOR_H
#define HGW_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

int  monitor_init(MetricCircBuf *buf,
                  const char (*proc_names)[HGW_MAX_PROC_NAME], int proc_count,
                  uint32_t interval_s);
int  monitor_start(void);
void monitor_stop(void);
bool monitor_peek_latest(MetricSnapshot *out);
void monitor_update_config(const char (*proc_names)[HGW_MAX_PROC_NAME],
                            int proc_count, uint32_t interval_s);

#endif /* HGW_MONITOR_H */