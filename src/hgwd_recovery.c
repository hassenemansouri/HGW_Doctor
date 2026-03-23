/**
 * @file hgwd_recovery.c
 * @brief Recovery action execution.
 *
 * Connects amxp slots to the three anomaly signals emitted by hgwd_events.c.
 * On signal receipt, reads the configured recovery action from the data model
 * (CpuRecoveryAction / MemRecoveryAction / WatchedProcess.RecoveryAction)
 * and executes it synchronously via system(3).
 *
 * Available actions:
 *   NO_ACTION    — log only
 *   KILL_TOP     — send SIGTERM to the highest-CPU process
 *   DROP_CACHES  — sync + echo 3 > /proc/sys/vm/drop_caches
 *   RESTART      — pkill -HUP then -TERM the named process
 *   REBOOT       — /sbin/reboot or reboot
 */

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hgwdoctor.h"

/* ── Low-level helpers ───────────────────────────────────────────────────── */

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if(rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void do_kill_top(void) {
    SAH_TRACEZ_INFO(ME, "Recovery: KILL_TOP — terminating highest-CPU process");
    run_cmd("kill -TERM $(ps -eo pid,%cpu --no-headers 2>/dev/null "
            "| sort -k2 -rn | head -1 | awk '{print $1}') 2>/dev/null");
}

static void do_drop_caches(void) {
    SAH_TRACEZ_INFO(ME, "Recovery: DROP_CACHES — dropping page cache");
    run_cmd("sync");
    run_cmd("echo 3 > /proc/sys/vm/drop_caches");
}

static void do_restart(const char *name) {
    char cmd[256];
    if(!name || name[0] == '\0') {
        SAH_TRACEZ_WARNING(ME, "Recovery: RESTART requested but no process name");
        return;
    }
    SAH_TRACEZ_INFO(ME, "Recovery: RESTART — restarting '%s'", name);
    snprintf(cmd, sizeof(cmd),
             "pkill -HUP -x '%s' 2>/dev/null || pkill -TERM -x '%s' 2>/dev/null",
             name, name);
    run_cmd(cmd);
}

static void do_reboot(void) {
    SAH_TRACEZ_WARNING(ME, "Recovery: REBOOT — rebooting system");
    if(access("/sbin/reboot", X_OK) == 0) {
        run_cmd("/sbin/reboot");
    } else {
        run_cmd("reboot");
    }
}

static void dispatch(const char *action, const char *proc_name) {
    if(!action || strcmp(action, RECOVERY_NO_ACTION) == 0) {
        SAH_TRACEZ_INFO(ME, "Recovery: NO_ACTION");
        return;
    }
    if(strcmp(action, RECOVERY_KILL_TOP) == 0)    { do_kill_top();           return; }
    if(strcmp(action, RECOVERY_DROP_CACHES) == 0) { do_drop_caches();        return; }
    if(strcmp(action, RECOVERY_RESTART) == 0)     { do_restart(proc_name);   return; }
    if(strcmp(action, RECOVERY_REBOOT) == 0)      { do_reboot();             return; }
    SAH_TRACEZ_WARNING(ME, "Recovery: unknown action '%s'", action);
}

/* ── Signal slot callbacks ───────────────────────────────────────────────── */

static void on_cpu_anomaly(const char *const sig_name,
                            const amxc_var_t *const data,
                            void *const priv) {
    hgwdoctor_t *hgwd = (hgwdoctor_t *)priv;
    amxd_object_t *obj;
    amxc_var_t var;
    const char *action = RECOVERY_NO_ACTION;

    (void)sig_name;
    (void)data;

    obj = amxd_dm_findf(hgwd->dm, "GatewayHealth");
    when_null_trace(obj, exit, WARNING, "GatewayHealth not found in cpu slot");

    amxc_var_init(&var);
    if(amxd_object_get_param(obj, "CpuRecoveryAction", &var) == amxd_status_ok) {
        action = amxc_var_constcast(cstring_t, &var);
    }
    dispatch(action, NULL);
    amxc_var_clean(&var);

exit:
    return;
}

static void on_mem_anomaly(const char *const sig_name,
                            const amxc_var_t *const data,
                            void *const priv) {
    hgwdoctor_t *hgwd = (hgwdoctor_t *)priv;
    amxd_object_t *obj;
    amxc_var_t var;
    const char *action = RECOVERY_NO_ACTION;

    (void)sig_name;
    (void)data;

    obj = amxd_dm_findf(hgwd->dm, "GatewayHealth");
    when_null_trace(obj, exit, WARNING, "GatewayHealth not found in mem slot");

    amxc_var_init(&var);
    if(amxd_object_get_param(obj, "MemRecoveryAction", &var) == amxd_status_ok) {
        action = amxc_var_constcast(cstring_t, &var);
    }
    dispatch(action, NULL);
    amxc_var_clean(&var);

exit:
    return;
}

static void on_proc_anomaly(const char *const sig_name,
                             const amxc_var_t *const data,
                             void *const priv) {
    const char *action    = RECOVERY_NO_ACTION;
    const char *proc_name = NULL;

    (void)sig_name;
    (void)priv;

    when_null_trace(data, exit, WARNING, "proc anomaly: signal data is NULL");

    /* data is a htable built by hgwd_events.c: {RecoveryAction, ProcessName} */
    action    = GET_CHAR(data, "RecoveryAction");
    proc_name = GET_CHAR(data, "ProcessName");

    if(!action) action = RECOVERY_NO_ACTION;

    SAH_TRACEZ_WARNING(ME, "Process anomaly: '%s' action=%s",
                       proc_name ? proc_name : "?", action);
    dispatch(action, proc_name);

exit:
    return;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void hgwd_recovery_init(hgwdoctor_t *hgwd) {
    amxp_slot_connect(&hgwd->sigmngr, SIG_ANOMALY_CPU,
                      NULL, on_cpu_anomaly, hgwd);
    amxp_slot_connect(&hgwd->sigmngr, SIG_ANOMALY_MEM,
                      NULL, on_mem_anomaly, hgwd);
    amxp_slot_connect(&hgwd->sigmngr, SIG_ANOMALY_PROCESS,
                      NULL, on_proc_anomaly, hgwd);
    SAH_TRACEZ_INFO(ME, "Recovery slots connected");
}

void hgwd_recovery_cleanup(hgwdoctor_t *hgwd) {
    amxp_slot_disconnect(&hgwd->sigmngr, SIG_ANOMALY_CPU,     on_cpu_anomaly);
    amxp_slot_disconnect(&hgwd->sigmngr, SIG_ANOMALY_MEM,     on_mem_anomaly);
    amxp_slot_disconnect(&hgwd->sigmngr, SIG_ANOMALY_PROCESS, on_proc_anomaly);
}
