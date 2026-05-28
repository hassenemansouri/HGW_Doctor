#ifndef HGW_RECOVERY_H
#define HGW_RECOVERY_H

#include <time.h>
#include "types.h"

typedef struct {
    ActionType action_type;
    char       process_names[HGW_MAX_PROC_LIST][HGW_MAX_PROC_NAME];
    int        process_count;
    char       scripts_dir[HGW_MAX_PATH];
    bool       escalation_enabled;
    uint32_t   escalation_reset_minutes;
} RecoveryConfig;

typedef void (*recovery_callback)(const RecoveryResult *result, void *userdata);

int  recovery_init(const RecoveryConfig *cfg, recovery_callback cb, void *userdata);
bool recovery_check_dispatch_cooldown(int type_idx);
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

/* Escalation chain */
ActionType get_escalated_action(const char *process_name, uint32_t current_level);
uint32_t   escalation_get_level(const char *process_name);
void       escalation_advance(const char *process_name);
void       escalation_check_reset(const char *process_name);

/* Reset cooldown timers only — preserves escalation levels */
void recovery_reset_cooldowns(void);
/* Reset escalation levels and cooldown timers — full escalation state wipe */
void recovery_reset_escalation(void);

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