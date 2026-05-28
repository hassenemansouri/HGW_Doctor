#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>

#include "logger.h"
#include "recovery.h"
#include "datamodel.h"

#define RECOVERY_MAX_CONCURRENT  4
#define REBOOT_UPTIME_MIN_S      60
#define REBOOT_COOLDOWN_S        300
#define REBOOT_MAX_PER_HOUR      3
#define REBOOT_HOUR_WINDOW_S     3600
#define REBOOT_RING_SIZE         4
#define DISPATCH_COOLDOWN_S      300  /* min seconds between recovery dispatches of same anomaly type */
#define DISPATCH_TYPE_MAX        7    /* ANOMALY_NONE … ANOMALY_ON_DEMAND */

static RecoveryConfig    s_cfg;
static recovery_callback s_callback = NULL;
static void             *s_userdata = NULL;
static pthread_mutex_t   s_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int       s_active_threads = ATOMIC_VAR_INIT(0);

/* Per-type dispatch cooldown — prevents hammering while an anomaly persists */
static time_t          s_last_dispatch[DISPATCH_TYPE_MAX] = {0};
static pthread_mutex_t s_dispatch_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Reboot rate-limit ring — records timestamps of recent HGWDoctor reboots */
static time_t          s_reboot_ring[REBOOT_RING_SIZE] = {0};
static int             s_reboot_ring_count = 0;
static pthread_mutex_t s_reboot_ring_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Per-process escalation state — in-memory authoritative source for dispatch
 * (main thread writes via escalation_advance/check_reset; analyzer thread
 *  reads via escalation_get_level, both under s_escl_mutex). */
typedef struct {
    char     process_name[HGW_MAX_PROC_NAME];
    uint32_t level;
    time_t   last_time;
    bool     used;
} EscalationEntry;
static EscalationEntry s_escl[HGW_MAX_PROC_LIST];
static pthread_mutex_t s_escl_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    AnomalyEvent      event;
    RecoveryConfig    cfg;
    recovery_callback callback;
    void             *userdata;
    bool              use_escalated_action; /* bypass normal action-select logic */
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

/* Fork and exec argv[0] with no shell — argv must be NULL-terminated.
 * No user-controlled data ever reaches a shell interpreter this way. */
static int run_argv(const char *const argv[]) {
    pid_t pid;
    int status;

    if (!argv || !argv[0]) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_action_script(const char *scripts_dir, const char *script_name, const char *arg) {
    char script_path[HGW_MAX_PATH * 2];
    const char *argv[3];

    if (!scripts_dir || scripts_dir[0] == '\0') return -1;

    if (join_path(script_path, sizeof(script_path), scripts_dir, script_name) != 0) {
        return -1;
    }
    if (access(script_path, X_OK) != 0) return -1;

    argv[0] = script_path;
    argv[1] = (arg && arg[0] != '\0') ? arg : NULL;
    argv[2] = NULL;
    return run_argv(argv);
}

static int do_process_restart(const char *scripts_dir, const char *proc_name) {
    int rc;
    const char *hup_argv[]  = { "pkill", "-HUP",  "-x", proc_name, NULL };
    const char *term_argv[] = { "pkill", "-TERM", "-x", proc_name, NULL };

    if (!proc_name || proc_name[0] == '\0') return -1;

    rc = run_action_script(scripts_dir, "restart_process.sh", proc_name);
    if (rc == 0) return 0;

    rc = run_argv(hup_argv);
    if (rc == 0) return 0;
    return run_argv(term_argv);
}

static int do_drop_caches(void) {
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    if (!f) return -1;
    int rc = (fputs("3\n", f) >= 0) ? 0 : -1;
    fclose(f);
    return rc;
}

static int do_cache_clear(const char *scripts_dir) {
    const char *sync_argv[] = { "sync", NULL };
    int rc = run_action_script(scripts_dir, "clear_cache.sh", NULL);
    if (rc == 0) return 0;
    run_argv(sync_argv);
    return do_drop_caches();
}

static int do_reboot(const char *scripts_dir) {
    const char *ubus_argv[] = {
        "ubus", "call", "Device", "Reboot",
        "{\"Cause\":\"LocalReboot\",\"Reason\":\"HGWDoctor\"}",
        NULL
    };
    const char *reboot_argv[] = { "/sbin/reboot", NULL };
    const char *reboot_fallback[] = { "reboot", NULL };
    int rc;

    rc = run_action_script(scripts_dir, "reboot_system.sh", NULL);
    if (rc == 0) return 0;

    rc = run_argv(ubus_argv);
    if (rc == 0) return 0;

    if (access("/sbin/reboot", X_OK) == 0) return run_argv(reboot_argv);
    return run_argv(reboot_fallback);
}

static void *recovery_run(void *arg) {
    RecoveryTask   *task = (RecoveryTask *)arg;
    const AnomalyEvent *event = &task->event;
    RecoveryResult  result = {0};

    ActionType action;
    if (task->use_escalated_action) {
        /* Escalation chain explicitly set the action — use it verbatim */
        action = task->cfg.action_type;
    } else if (event->type == ANOMALY_PROCESS ||
               event->type == ANOMALY_PROCESS_CPU ||
               event->type == ANOMALY_PROCESS_MEM) {
        action = ACTION_PROCESS_RESTART;
    } else if (event->type == ANOMALY_CPU || event->type == ANOMALY_MEMORY) {
        action = (task->cfg.action_type == ACTION_PROCESS_RESTART)
               ? ACTION_CACHE_CLEAR : task->cfg.action_type;
    } else {
        action = task->cfg.action_type;
    }
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
    atomic_fetch_sub(&s_active_threads, 1);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Reboot guard helpers
 * ------------------------------------------------------------------------- */

static time_t parse_iso8601_utc(const char *s) {
    int y, mo, d, h, mi, sec;
    struct tm tm = {0};
    if (!s || s[0] == '\0') return 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = sec;
    return timegm(&tm);
}

RebootGuardResult recovery_reboot_guard_check(const char *last_reboot_time_str) {
    /* 1. Uptime check using CLOCK_BOOTTIME */
    struct timespec boot_ts = {0};
    if (clock_gettime(CLOCK_BOOTTIME, &boot_ts) == 0) {
        if (boot_ts.tv_sec < REBOOT_UPTIME_MIN_S) {
            LOG_WARN("Reboot rejected: uptime %lds < %ds",
                     (long)boot_ts.tv_sec, REBOOT_UPTIME_MIN_S);
            return REBOOT_GUARD_UPTIME_TOO_LOW;
        }
    }

    /* 2. Cooldown: reject if last reboot was < 300s ago */
    time_t last_t = parse_iso8601_utc(last_reboot_time_str);
    if (last_t > 0) {
        time_t now = time(NULL);
        long since = (long)(now - last_t);
        if (since >= 0 && since < REBOOT_COOLDOWN_S) {
            LOG_WARN("Reboot rejected: cooldown active (%lds remaining)",
                     (long)(REBOOT_COOLDOWN_S - since));
            return REBOOT_GUARD_COOLDOWN;
        }
    }

    /* 3. Rate limit: > 3 HGWDoctor reboots in the last hour → SafeMode */
    pthread_mutex_lock(&s_reboot_ring_mutex);
    time_t now2 = time(NULL);
    int recent = 0;
    for (int i = 0; i < s_reboot_ring_count; i++) {
        if ((now2 - s_reboot_ring[i]) < REBOOT_HOUR_WINDOW_S)
            recent++;
    }
    pthread_mutex_unlock(&s_reboot_ring_mutex);

    if (recent > REBOOT_MAX_PER_HOUR) {
        LOG_WARN("Reboot rejected: %d reboots in last hour (> %d) — SafeMode",
                 recent, REBOOT_MAX_PER_HOUR);
        return REBOOT_GUARD_SAFE_MODE;
    }

    return REBOOT_GUARD_OK;
}

void recovery_record_reboot_completed(time_t when) {
    pthread_mutex_lock(&s_reboot_ring_mutex);
    if (s_reboot_ring_count < REBOOT_RING_SIZE) {
        s_reboot_ring[s_reboot_ring_count++] = when;
    } else {
        memmove(s_reboot_ring, s_reboot_ring + 1,
                (REBOOT_RING_SIZE - 1) * sizeof(time_t));
        s_reboot_ring[REBOOT_RING_SIZE - 1] = when;
    }
    pthread_mutex_unlock(&s_reboot_ring_mutex);
}

int recovery_do_reboot(const char *scripts_dir) {
    return do_reboot(scripts_dir);
}

/* -------------------------------------------------------------------------
 * On-demand dispatch — explicit action, bypasses stored config
 * ------------------------------------------------------------------------- */
int recovery_dispatch_ondemand(ActionType action,
                                const char *proc_name,
                                const char *scripts_dir,
                                recovery_callback cb,
                                void *userdata) {
    int prev = atomic_fetch_add(&s_active_threads, 1);
    if (prev >= RECOVERY_MAX_CONCURRENT) {
        atomic_fetch_sub(&s_active_threads, 1);
        LOG_WARN("Recovery: max concurrent actions reached, dropping on-demand dispatch");
        free(userdata);  /* caller expects us to own userdata on failure too */
        return -1;
    }

    RecoveryTask *task = calloc(1, sizeof(*task));
    if (!task) {
        atomic_fetch_sub(&s_active_threads, 1);
        free(userdata);
        return -1;
    }

    task->event.type = ANOMALY_ON_DEMAND;
    clock_gettime(CLOCK_REALTIME, &task->event.detected_at);
    if (proc_name && proc_name[0] != '\0') {
        strncpy(task->event.process_name, proc_name, HGW_MAX_PROC_NAME - 1);
        task->cfg.process_count = 1;
        strncpy(task->cfg.process_names[0], proc_name, HGW_MAX_PROC_NAME - 1);
    } else {
        pthread_mutex_lock(&s_cfg_mutex);
        task->cfg.process_count = s_cfg.process_count;
        memcpy(task->cfg.process_names, s_cfg.process_names,
               sizeof(s_cfg.process_names));
        pthread_mutex_unlock(&s_cfg_mutex);
    }

    task->cfg.action_type = action;
    if (scripts_dir && scripts_dir[0] != '\0')
        strncpy(task->cfg.scripts_dir, scripts_dir, HGW_MAX_PATH - 1);

    if (cb) {
        task->callback = cb;
        task->userdata  = userdata;
    } else {
        pthread_mutex_lock(&s_cfg_mutex);
        task->callback = s_callback;
        task->userdata  = s_userdata;
        pthread_mutex_unlock(&s_cfg_mutex);
    }

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, recovery_run, task);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(task);
        atomic_fetch_sub(&s_active_threads, 1);
        return -1;
    }
    return 0;
}

int recovery_init(const RecoveryConfig *cfg, recovery_callback cb, void *userdata) {
    if (!cfg) return -1;

    s_cfg = *cfg;
    s_callback = cb;
    s_userdata = userdata;
    return 0;
}

/* Returns true and records the timestamp when the cooldown has elapsed.
 * Returns false (and logs) when the same anomaly type fired too recently. */
bool recovery_check_dispatch_cooldown(int type_idx) {
    if (type_idx < 0 || type_idx >= DISPATCH_TYPE_MAX) return true;
    time_t now = time(NULL);
    bool ok;
    pthread_mutex_lock(&s_dispatch_mutex);
    ok = (now - s_last_dispatch[type_idx] >= DISPATCH_COOLDOWN_S);
    if (ok)
        s_last_dispatch[type_idx] = now;
    pthread_mutex_unlock(&s_dispatch_mutex);
    if (!ok)
        LOG_INFO("Recovery cooldown active for anomaly type %d (%lds remaining)",
                 type_idx,
                 (long)(DISPATCH_COOLDOWN_S - (time(NULL) - s_last_dispatch[type_idx])));
    return ok;
}

int recovery_dispatch(const AnomalyEvent *event) {
    /* Increment before pthread_create so the cap is enforced atomically */
    int prev = atomic_fetch_add(&s_active_threads, 1);
    if (prev >= RECOVERY_MAX_CONCURRENT) {
        atomic_fetch_sub(&s_active_threads, 1);
        LOG_WARN("Recovery: max concurrent actions (%d) reached, dropping dispatch",
                 RECOVERY_MAX_CONCURRENT);
        return -1;
    }

    RecoveryTask *task = malloc(sizeof(*task));
    if (!task) {
        atomic_fetch_sub(&s_active_threads, 1);
        return -1;
    }

    task->event = *event;
    task->use_escalated_action = false;
    pthread_mutex_lock(&s_cfg_mutex);
    task->cfg      = s_cfg;
    task->callback = s_callback;
    task->userdata = s_userdata;
    /* Escalation: override action_type with escalated action when enabled */
    if (s_cfg.escalation_enabled) {
        pthread_mutex_lock(&s_escl_mutex);
        uint32_t level = 0;
        for (int i = 0; i < HGW_MAX_PROC_LIST; i++) {
            if (s_escl[i].used &&
                strcmp(s_escl[i].process_name, event->process_name) == 0) {
                level = s_escl[i].level;
                break;
            }
        }
        pthread_mutex_unlock(&s_escl_mutex);
        task->cfg.action_type = get_escalated_action(event->process_name, level);
        /* A dead process must be restarted — cache clear cannot revive it */
        if (event->type == ANOMALY_PROCESS &&
            task->cfg.action_type == ACTION_CACHE_CLEAR) {
            task->cfg.action_type = ACTION_PROCESS_RESTART;
        }
        task->use_escalated_action = true;
        LOG_INFO("Escalation dispatch: proc=%s level=%d action=%d",
                 event->process_name, (int)level, (int)task->cfg.action_type);
    }
    pthread_mutex_unlock(&s_cfg_mutex);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, recovery_run, task);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(task);
        atomic_fetch_sub(&s_active_threads, 1);
        return -1;
    }
    return 0;
}

void recovery_update_config(const RecoveryConfig *cfg) {
    if (!cfg) return;
    pthread_mutex_lock(&s_cfg_mutex);
    s_cfg.action_type             = cfg->action_type;
    s_cfg.process_count           = cfg->process_count;
    memcpy(s_cfg.process_names, cfg->process_names, sizeof(s_cfg.process_names));
    s_cfg.escalation_enabled      = cfg->escalation_enabled;
    s_cfg.escalation_reset_minutes = cfg->escalation_reset_minutes;
    /* scripts_dir is host-side config, not exposed via DM — leave unchanged */
    pthread_mutex_unlock(&s_cfg_mutex);
    LOG_INFO("Recovery config updated: action=%d processes=%d escalation=%d",
             cfg->action_type, cfg->process_count, cfg->escalation_enabled);
}

void recovery_cleanup(void) {
    /* Wait for all in-flight detached recovery threads to finish (up to 30s) */
    for (int i = 0; i < 300 && atomic_load(&s_active_threads) > 0; i++)
        usleep(100000);
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_callback = NULL;
    s_userdata = NULL;
    pthread_mutex_destroy(&s_cfg_mutex);
    pthread_mutex_destroy(&s_dispatch_mutex);
    pthread_mutex_destroy(&s_escl_mutex);
}

/* -------------------------------------------------------------------------
 * Escalation chain
 * ------------------------------------------------------------------------- */

ActionType get_escalated_action(const char *process_name, uint32_t current_level) {
    (void)process_name;
    switch (current_level) {
        case 0:  return ACTION_CACHE_CLEAR;
        case 1:  return ACTION_PROCESS_RESTART;
        case 2:  return ACTION_REBOOT;
        default: return ACTION_REBOOT;
    }
}

/* Thread-safe read of in-memory escalation level (called from analyzer thread). */
uint32_t escalation_get_level(const char *process_name) {
    if (!process_name || process_name[0] == '\0') return 0;
    uint32_t level = 0;
    pthread_mutex_lock(&s_escl_mutex);
    for (int i = 0; i < HGW_MAX_PROC_LIST; i++) {
        if (s_escl[i].used &&
            strcmp(s_escl[i].process_name, process_name) == 0) {
            level = s_escl[i].level;
            break;
        }
    }
    pthread_mutex_unlock(&s_escl_mutex);
    return level;
}

/* Advance escalation level after a recovery action (main thread only). */
void escalation_advance(const char *process_name) {
    if (!process_name || process_name[0] == '\0') return;

    pthread_mutex_lock(&s_escl_mutex);
    int slot = -1;
    for (int i = 0; i < HGW_MAX_PROC_LIST; i++) {
        if (s_escl[i].used &&
            strcmp(s_escl[i].process_name, process_name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < HGW_MAX_PROC_LIST; i++) {
            if (!s_escl[i].used) {
                slot = i;
                strncpy(s_escl[i].process_name, process_name, HGW_MAX_PROC_NAME - 1);
                s_escl[i].process_name[HGW_MAX_PROC_NAME - 1] = '\0';
                s_escl[i].level    = 0;
                s_escl[i].last_time = 0;
                s_escl[i].used     = true;
                break;
            }
        }
    }
    if (slot >= 0 && s_escl[slot].level < 2) {
        s_escl[slot].level++;
        s_escl[slot].last_time = time(NULL);
    }
    pthread_mutex_unlock(&s_escl_mutex);

    /* Mirror new state to data model (main thread — DM access is safe here) */
    datamodel_set_escalation_level(process_name,
                                   escalation_get_level(process_name));
    datamodel_increment_escalation_count(process_name);
    datamodel_set_last_escalation_time(process_name);
    LOG_INFO("Escalation advanced for %s: level=%u",
             process_name, escalation_get_level(process_name));
}

/* Reset all escalation levels and cooldown timestamps — used by ResetCounters RPC. */
void recovery_reset_cooldowns(void) {
    pthread_mutex_lock(&s_dispatch_mutex);
    memset(s_last_dispatch, 0, sizeof(s_last_dispatch));
    pthread_mutex_unlock(&s_dispatch_mutex);

    pthread_mutex_lock(&s_escl_mutex);
    for (int i = 0; i < HGW_MAX_PROC_LIST; i++)
        s_escl[i].last_time = 0;
    pthread_mutex_unlock(&s_escl_mutex);

    pthread_mutex_lock(&s_reboot_ring_mutex);
    memset(s_reboot_ring, 0, sizeof(s_reboot_ring));
    s_reboot_ring_count = 0;
    pthread_mutex_unlock(&s_reboot_ring_mutex);

    LOG_INFO("Recovery cooldowns reset via RPC");
}

/* Reset escalation levels — used by ResetEscalation() RPC only. */
void recovery_reset_escalation(void) {
    pthread_mutex_lock(&s_escl_mutex);
    for (int i = 0; i < HGW_MAX_PROC_LIST; i++) {
        s_escl[i].level     = 0;
        s_escl[i].last_time = 0;
    }
    pthread_mutex_unlock(&s_escl_mutex);

    LOG_INFO("Escalation state reset via RPC");
}

/* Check whether escalation should reset due to a clean period (main thread). */
void escalation_check_reset(const char *process_name) {
    if (!process_name || process_name[0] == '\0') return;

    uint32_t reset_mins = datamodel_get_escalation_reset_minutes();
    if (reset_mins == 0) return;

    pthread_mutex_lock(&s_escl_mutex);
    int slot = -1;
    for (int i = 0; i < HGW_MAX_PROC_LIST; i++) {
        if (s_escl[i].used &&
            strcmp(s_escl[i].process_name, process_name) == 0) {
            slot = i;
            break;
        }
    }
    bool do_reset = false;
    if (slot >= 0 && s_escl[slot].level > 0 && s_escl[slot].last_time > 0 &&
        time(NULL) - s_escl[slot].last_time >= (time_t)(reset_mins * 60)) {
        s_escl[slot].level = 0;
        do_reset = true;
    }
    pthread_mutex_unlock(&s_escl_mutex);

    if (do_reset) {
        datamodel_set_escalation_level(process_name, 0);
        LOG_INFO("Escalation reset for %s after %u min clean", process_name, reset_mins);
    }
}