#ifndef HGW_MONITOR_H
#define HGW_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

int monitor_init(MetricCircBuf *buf, const char *proc_name, uint32_t interval_s);
int monitor_start(void);
void monitor_stop(void);
bool monitor_peek_latest(MetricSnapshot *out);

#endif /* HGW_MONITOR_H */