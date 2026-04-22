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

int datamodel_init(amxd_dm_t *dm, amxo_parser_t *parser, const char *odl_path);
void datamodel_cleanup(amxd_dm_t *dm, amxo_parser_t *parser);

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
bool datamodel_get_thresholds(uint32_t *cpu_pct, uint32_t *mem_pct,
                               uint32_t *duration_s, uint32_t *poll_s);

amxd_status_t dm_trigger_diagnostics(amxd_object_t *obj, amxd_function_t *fn,
                                     amxc_var_t *args, amxc_var_t *ret);
amxd_status_t dm_reset_counters(amxd_object_t *obj, amxd_function_t *fn,
                                amxc_var_t *args, amxc_var_t *ret);
amxd_status_t dm_set_profile(amxd_object_t *obj, amxd_function_t *fn,
                             amxc_var_t *args, amxc_var_t *ret);

#endif /* HGW_DATAMODEL_H */