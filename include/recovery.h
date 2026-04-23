#ifndef HGW_RECOVERY_H
#define HGW_RECOVERY_H

#include "types.h"

typedef struct {
    ActionType action_type;
    char       process_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME];
    int        process_count;
    char       scripts_dir[HGW_MAX_PATH];
} RecoveryConfig;

typedef void (*recovery_callback)(const RecoveryResult *result, void *userdata);

int recovery_init(const RecoveryConfig *cfg, recovery_callback cb, void *userdata);
int recovery_dispatch(const AnomalyEvent *event);
void recovery_update_config(const RecoveryConfig *cfg);
void recovery_cleanup(void);

#endif /* HGW_RECOVERY_H */