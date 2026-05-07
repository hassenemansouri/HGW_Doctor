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
    bool     enable;
} DmConfig;

int datamodel_init(amxd_dm_t *dm, amxo_parser_t *parser, const char *odl_path);
void datamodel_cleanup(amxd_dm_t *dm, amxo_parser_t *parser);
void datamodel_start_sync(void);  /* call after amxb_register() to activate amxs TR-181 sync */

void datamodel_set_status(const char *status_str);
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
bool datamodel_consume_on_demand_trigger(void);
bool datamodel_get_config(DmConfig *out);
void datamodel_sync_startup(const char *action_type, const char *upload_url,
                             const char *diag_output_dir);
void datamodel_sync_counters(void);

amxd_status_t dm_on_param_changed(amxd_object_t *obj, amxd_param_t *param,
                                  amxd_action_t reason, const amxc_var_t *args,
                                  amxc_var_t *retval, void *priv);
amxd_status_t dm_trigger_diagnostics(amxd_object_t *obj, amxd_function_t *fn,
                                     amxc_var_t *args, amxc_var_t *ret);
amxd_status_t dm_reset_counters(amxd_object_t *obj, amxd_function_t *fn,
                                amxc_var_t *args, amxc_var_t *ret);
amxd_status_t dm_set_profile(amxd_object_t *obj, amxd_function_t *fn,
                             amxc_var_t *args, amxc_var_t *ret);

#endif /* HGW_DATAMODEL_H */