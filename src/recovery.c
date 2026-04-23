#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

#include "logger.h"
#include "recovery.h"

static RecoveryConfig    s_cfg;
static recovery_callback s_callback = NULL;
static void             *s_userdata = NULL;
static pthread_mutex_t   s_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    AnomalyEvent      event;
    RecoveryConfig    cfg;
    recovery_callback callback;
    void             *userdata;
} RecoveryTask;

static void copy_string(char *dst, size_t dst_size, const char *src) {
    size_t len;

    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int join_path(char *dst, size_t dst_size, const char *base, const char *leaf) {
    size_t base_len;
    size_t leaf_len;
    int needs_sep;

    if (!dst || dst_size == 0 || !base || !leaf || base[0] == '\0' || leaf[0] == '\0') return -1;

    base_len = strlen(base);
    leaf_len = strlen(leaf);
    needs_sep = (base[base_len - 1] != '/');
    if (base_len + (size_t) needs_sep + leaf_len >= dst_size) return -1;

    memcpy(dst, base, base_len);
    if (needs_sep) dst[base_len++] = '/';
    memcpy(dst + base_len, leaf, leaf_len);
    dst[base_len + leaf_len] = '\0';
    return 0;
}

static int run_command(const char *command) {
    int rc;

    if (!command || command[0] == '\0') return -1;

    rc = system(command);
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

static int run_action_script(const char *scripts_dir, const char *script_name, const char *arg) {
    char script_path[HGW_MAX_PATH * 2];
    char command[HGW_MAX_PATH * 4];

    if (!scripts_dir || scripts_dir[0] == '\0') return -1;

    if (join_path(script_path, sizeof(script_path), scripts_dir, script_name) != 0) {
        return -1;
    }
    if (access(script_path, X_OK) != 0) return -1;

    if (arg && arg[0] != '\0') {
        snprintf(command, sizeof(command), "'%s' '%s'", script_path, arg);
    } else {
        snprintf(command, sizeof(command), "'%s'", script_path);
    }

    return run_command(command);
}

static int do_process_restart(const char *scripts_dir, const char *proc_name) {
    char command[HGW_MAX_PATH * 4];
    int rc;

    if (!proc_name || proc_name[0] == '\0') return -1;

    rc = run_action_script(scripts_dir, "restart_process.sh", proc_name);
    if (rc == 0) return 0;

    snprintf(command, sizeof(command),
             "sh -c \"pkill -HUP -x '%s' >/dev/null 2>&1 || pkill -TERM -x '%s' >/dev/null 2>&1\"",
             proc_name, proc_name);
    return run_command(command);
}

static int do_cache_clear(const char *scripts_dir) {
    int rc = run_action_script(scripts_dir, "clear_cache.sh", NULL);
    if (rc == 0) return 0;
    return run_command("sh -c \"sync; echo 3 > /proc/sys/vm/drop_caches\"");
}

static int do_reboot(const char *scripts_dir) {
    int rc = run_action_script(scripts_dir, "reboot_system.sh", NULL);
    if (rc == 0) return 0;

    /* Follow PrplOS pattern: call Device.Reboot() via ubus if available,
     * fall back to shell reboot otherwise. */
    rc = run_command("ubus call Device Reboot '{\"Cause\":\"LocalReboot\",\"Reason\":\"HGWDoctor\"}' 2>/dev/null");
    if (rc == 0) return 0;

    if (access("/sbin/reboot", X_OK) == 0) return run_command("/sbin/reboot");
    return run_command("reboot");
}

static void *recovery_run(void *arg) {
    RecoveryTask   *task = (RecoveryTask *)arg;
    const AnomalyEvent *event = &task->event;
    RecoveryResult  result = {0};

    ActionType action;
    if (event->type == ANOMALY_PROCESS ||
        event->type == ANOMALY_PROCESS_CPU ||
        event->type == ANOMALY_PROCESS_MEM)
        action = ACTION_PROCESS_RESTART;
    else if (event->type == ANOMALY_CPU || event->type == ANOMALY_MEMORY)
        action = (task->cfg.action_type == ACTION_PROCESS_RESTART) ? ACTION_CACHE_CLEAR : task->cfg.action_type;
    else
        action = task->cfg.action_type;
    result.action = action;
    result.result = RESULT_NONE;
    result.exit_code = 0;
    clock_gettime(CLOCK_REALTIME, &result.executed_at);
    copy_string(result.process_name, sizeof(result.process_name),
                (event->process_name[0] != '\0') ? event->process_name
                : (task->cfg.process_count > 0 ? task->cfg.process_names[0] : ""));

    /* For process restart: prefer the specific process reported by the event,
     * fall back to the first configured process name. */
    const char *restart_target = (event->process_name[0] != '\0')
                                 ? event->process_name
                                 : (task->cfg.process_count > 0 ? task->cfg.process_names[0] : "");

    switch (action) {
        case ACTION_PROCESS_RESTART:
            result.exit_code = do_process_restart(task->cfg.scripts_dir, restart_target);
            result.result = (result.exit_code == 0) ? RESULT_SUCCESS : RESULT_FAILURE;
            break;
        case ACTION_CACHE_CLEAR:
            result.exit_code = do_cache_clear(task->cfg.scripts_dir);
            result.result = (result.exit_code == 0) ? RESULT_SUCCESS : RESULT_FAILURE;
            break;
        case ACTION_REBOOT:
            result.exit_code = do_reboot(task->cfg.scripts_dir);
            result.result = (result.exit_code == 0) ? RESULT_SUCCESS : RESULT_FAILURE;
            break;
        case ACTION_NONE:
        default:
            result.result = RESULT_NONE;
            break;
    }

    LOG_INFO("Recovery done: action=%d result=%d exit=%d",
             result.action, result.result, result.exit_code);

    if (task->callback) task->callback(&result, task->userdata);
    free(task);
    return NULL;
}

int recovery_init(const RecoveryConfig *cfg, recovery_callback cb, void *userdata) {
    if (!cfg) return -1;

    s_cfg = *cfg;
    s_callback = cb;
    s_userdata = userdata;
    return 0;
}

int recovery_dispatch(const AnomalyEvent *event) {
    RecoveryTask *task = malloc(sizeof(*task));
    if (!task) return -1;

    task->event = *event;
    pthread_mutex_lock(&s_cfg_mutex);
    task->cfg      = s_cfg;
    task->callback = s_callback;
    task->userdata = s_userdata;
    pthread_mutex_unlock(&s_cfg_mutex);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, recovery_run, task);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(task);
        return -1;
    }
    return 0;
}

void recovery_update_config(const RecoveryConfig *cfg) {
    if (!cfg) return;
    pthread_mutex_lock(&s_cfg_mutex);
    s_cfg.action_type   = cfg->action_type;
    s_cfg.process_count = cfg->process_count;
    memcpy(s_cfg.process_names, cfg->process_names, sizeof(s_cfg.process_names));
    /* scripts_dir is host-side config, not exposed via DM — leave unchanged */
    pthread_mutex_unlock(&s_cfg_mutex);
    LOG_INFO("Recovery config updated: action=%d processes=%d",
             cfg->action_type, cfg->process_count);
}

void recovery_cleanup(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_callback = NULL;
    s_userdata = NULL;
    pthread_mutex_destroy(&s_cfg_mutex);
}