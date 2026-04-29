#ifndef HGW_DIAG_COLLECTOR_H
#define HGW_DIAG_COLLECTOR_H

#include <stdint.h>

#include "types.h"

typedef struct {
    char     output_dir[HGW_MAX_PATH];
    uint32_t max_archives;
} DiagConfig;

typedef void (*diag_done_callback)(const char *archive_path, void *userdata);

int diag_collector_init(const DiagConfig *cfg, diag_done_callback cb, void *userdata);
int diag_collect(const AnomalyEvent *event);
void diag_collector_update_output_dir(const char *path);
void diag_collector_cleanup(void);

#endif /* HGW_DIAG_COLLECTOR_H */