#ifndef HGW_DATAMODEL_H
#define HGW_DATAMODEL_H

#include <stdbool.h>
#include <stdint.h>

#include <amxc/amxc.h>
#include <amxp/amxp.h>
#include <amxd/amxd_types.h>
#include <amxd/amxd_dm.h>
#include <amxd/amxd_object.h>
#include <amxd/amxd_transaction.h>
#include <amxo/amxo.h>

#include "types.h"

/* All writable DM parameters, read back after ACS/ubus writes them */
typedef struct {
    uint32_t cpu_threshold_pct;
    uint32_t mem_threshold_pct;
    uint32_t threshold_duration_s;
    uint32_t poll_interval_s;
    char     action_type[32];
    char     process_list[HGW_MAX_PROC_LIST * HGW_MAX_PROC_NAME];
    char     upload_url[HGW_MAX_URL];
    char     diag_output_dir[HGW_MAX_PATH];
    char     on_demand_target[HGW_MAX_PROC_NAME];
    bool     enable;
} DmConfig;

int datamodel_init(amxd_dm_t *dm, amxo_parser_t *parser, const char *odl_path);
void datamodel_cleanup(amxd_dm_t *dm, amxo_parser_t *parser);
void datamodel_start_sync(void);  /* call after amxb_register() to activate amxs TR-181 sync */

void datamodel_set_status(const char *status_str);
void datamodel_set_action_type(const char *action_type);
void datamodel_set_process_list(const char *process_list);
void datamodel_update_stats(uint32_t cpu_pct, uint32_t mem_pct,
                            uint32_t mem_free_kb);
void datamodel_record_action(const RecoveryResult *result);
void datamodel_record_upload(UploadStatus status, const char *archive_path);
void datamodel_increment_anomaly_count(void);
void datamodel_append_anomaly_log(const AnomalyEvent *event,
                                  const char *action_taken,
                                  const char *action_result);
void datamodel_update_uptime(uint32_t uptime_s);
void datamodel_update_self_stats(void);
void datamodel_reset_on_demand_trigger(void);
void datamodel_reset_all(void);
bool datamodel_consume_on_demand_trigger(void);
bool datamodel_get_config(DmConfig *out);
void datamodel_sync_startup(const char *action_type, const char *upload_url,
                             const char *diag_output_dir);
void datamodel_sync_counters(void);

/* On-demand reboot helpers */
uint32_t datamodel_get_reboot_delay_sec(void);
void     datamodel_get_last_reboot_time(char *buf, size_t len);
void     datamodel_append_ondemand_log(const char *action_taken,
                                       const char *action_result);
bool     datamodel_was_deferred_reboot_boot(void);

/* On-demand ProcessRestart target */
void datamodel_get_on_demand_target(char *buf, size_t len);
void datamodel_reset_on_demand_target(void);

/* Global flags set by dm_execute_action(); consumed by the main loop after
 * amxb_read() returns so amxd functions are safe to call. */
extern _Atomic int  g_ondemand_action;
extern char         g_ondemand_target[HGW_MAX_PROC_NAME];

amxd_status_t dm_trigger_diagnostics(amxd_object_t *obj, amxd_function_t *fn,
                                     amxc_var_t *args, amxc_var_t *ret);
amxd_status_t dm_reset_counters(amxd_object_t *obj, amxd_function_t *fn,
                                amxc_var_t *args, amxc_var_t *ret);
amxd_status_t dm_set_profile(amxd_object_t *obj, amxd_function_t *fn,
                             amxc_var_t *args, amxc_var_t *ret);
amxd_status_t dm_execute_action(amxd_object_t *obj, amxd_function_t *fn,
                                amxc_var_t *args, amxc_var_t *ret);

#endif /* HGW_DATAMODEL_H */