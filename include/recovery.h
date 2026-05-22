#ifndef HGW_RECOVERY_H
#define HGW_RECOVERY_H

#include <time.h>
#include "types.h"

typedef struct {
    ActionType action_type;
    char       process_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME];
    int        process_count;
    char       scripts_dir[HGW_MAX_PATH];
} RecoveryConfig;

typedef void (*recovery_callback)(const RecoveryResult *result, void *userdata);

int  recovery_init(const RecoveryConfig *cfg, recovery_callback cb, void *userdata);
int  recovery_dispatch(const AnomalyEvent *event);
void recovery_update_config(const RecoveryConfig *cfg);
void recovery_cleanup(void);

/* On-demand dispatch: explicit action + process name, bypasses stored config.
 * If cb is NULL the stored recovery_callback is used instead. */
int  recovery_dispatch_ondemand(ActionType action,
                                 const char *proc_name,
                                 const char *scripts_dir,
                                 recovery_callback cb,
                                 void *userdata);

/* Synchronous action runner — blocks until the action completes.
 * Fills *out with the result (may be NULL). Returns exit code (0 = success). */
int  recovery_run_sync(ActionType action, const char *proc_name,
                       const char *scripts_dir, RecoveryResult *out);

/* Deferred reboot helpers */
typedef enum {
    REBOOT_GUARD_OK             = 0,
    REBOOT_GUARD_UPTIME_TOO_LOW = 1,
    REBOOT_GUARD_COOLDOWN       = 2,
    REBOOT_GUARD_SAFE_MODE      = 3,
} RebootGuardResult;

RebootGuardResult recovery_reboot_guard_check(const char *last_reboot_time_str);
void recovery_record_reboot_completed(time_t when);
int  recovery_do_reboot(const char *scripts_dir);

#endif /* HGW_RECOVERY_H */