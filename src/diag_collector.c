#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "diag_collector.h"
#include "logger.h"

static DiagConfig         s_cfg;
static diag_done_callback s_callback = NULL;
static void              *s_userdata = NULL;
static pthread_mutex_t    s_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

static _Atomic int  s_diag_active = ATOMIC_VAR_INIT(0);
static pthread_t    s_diag_thread;

typedef struct {
    AnomalyEvent event;
    int          has_event;
    DiagConfig   cfg;    /* snapshot at dispatch — safe against concurrent update_output_dir */
} DiagArg;
static DiagArg s_diag_arg;

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
    size_t total_len;
    int needs_sep;

    if (!dst || dst_size == 0 || !base || !leaf || base[0] == '\0' || leaf[0] == '\0') return -1;

    base_len = strlen(base);
    leaf_len = strlen(leaf);
    needs_sep = (base[base_len - 1] != '/');
    total_len = base_len + (size_t) needs_sep + leaf_len;
    if (total_len >= dst_size) return -1;

    memcpy(dst, base, base_len);
    if (needs_sep) dst[base_len++] = '/';
    memcpy(dst + base_len, leaf, leaf_len);
    dst[base_len + leaf_len] = '\0';
    return 0;
}

/* Fork and exec argv[0..] — no shell, so paths with metacharacters are safe. */
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

/* Fork, redirect child stdout to outfile, then exec argv[0..]. */
static int run_argv_to_file(const char *const argv[], const char *outfile) {
    pid_t pid;
    int fd, status;

    if (!argv || !argv[0] || !outfile) return -1;
    fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    pid = fork();
    if (pid < 0) { close(fd); return -1; }
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO);
        close(fd);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fd);
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


static int ensure_dir(const char *path) {
    char tmp[HGW_MAX_PATH];
    size_t len;

    if (!path || path[0] == '\0') return -1;

    len = strnlen(path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp)) return -1;

    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_text_file(const char *src_path, const char *dst_path) {
    FILE *src = fopen(src_path, "r");
    FILE *dst;
    char buffer[4096];
    size_t nread;

    if (!src) return -1;
    dst = fopen(dst_path, "w");
    if (!dst) {
        fclose(src);
        return -1;
    }

    while ((nread = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, nread, dst) != nread) {
            fclose(src);
            fclose(dst);
            return -1;
        }
    }

    fclose(src);
    fclose(dst);
    return 0;
}

static const char *event_type_to_string(AnomalyType type) {
    switch (type) {
        case ANOMALY_CPU:         return "CPU";
        case ANOMALY_MEMORY:      return "Memory";
        case ANOMALY_PROCESS:     return "Process";
        case ANOMALY_PROCESS_CPU: return "ProcessCPU";
        case ANOMALY_PROCESS_MEM: return "ProcessMem";
        case ANOMALY_NONE:
        default:                  return "None";
    }
}

static void write_event_file(const char *path, const AnomalyEvent *event) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    if (!event) {
        fprintf(f, "trigger=manual\n");
    } else {
        fprintf(f, "trigger=anomaly\n");
        fprintf(f, "type=%s\n", event_type_to_string(event->type));
        fprintf(f, "metric_value=%u\n", event->metric_value);
        fprintf(f, "duration_s=%u\n", event->duration_s);
        fprintf(f, "detected_at=%lld\n", (long long) event->detected_at.tv_sec);
    }

    fclose(f);
}

static void prune_archives(const char *output_dir, uint32_t max_archives) {
    DIR *dir;
    struct dirent *entry;
    struct {
        char path[HGW_MAX_PATH * 2];
        time_t mtime;
    } archives[64];
    size_t count = 0;

    if (max_archives == 0) return;

    dir = opendir(output_dir);
    if (!dir) return;

    while ((entry = readdir(dir)) != NULL && count < 64) {
        struct stat st;
        char full_path[HGW_MAX_PATH * 2];

        if (entry->d_name[0] == '.') continue;
        if (!strstr(entry->d_name, ".tar.gz")) continue;

        if (join_path(full_path, sizeof(full_path), output_dir, entry->d_name) != 0) {
            continue;
        }
        if (stat(full_path, &st) != 0) continue;

        copy_string(archives[count].path, sizeof(archives[count].path), full_path);
        archives[count].mtime = st.st_mtime;
        count++;
    }
    closedir(dir);

    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (archives[j].mtime > archives[i].mtime) {
                typeof(archives[0]) tmp = archives[i];
                archives[i] = archives[j];
                archives[j] = tmp;
            }
        }
    }

    for (size_t i = max_archives; i < count; ++i) {
        unlink(archives[i].path);
    }
}

int diag_collector_init(const DiagConfig *cfg, diag_done_callback cb, void *userdata) {
    if (!cfg) return -1;

    s_cfg = *cfg;
    s_callback = cb;
    s_userdata = userdata;
    return ensure_dir(s_cfg.output_dir);
}

static int diag_collect_sync(const DiagArg *a) {
    const char *output_dir = a->cfg.output_dir;
    const AnomalyEvent *event = a->has_event ? &a->event : NULL;
    char timestamp[32];
    char dir_name[64];
    char archive_name[64];
    char temp_dir[HGW_MAX_PATH * 2];
    char archive_path[HGW_MAX_PATH * 2];
    char event_path[HGW_MAX_PATH * 2];
    char proc_stat_path[HGW_MAX_PATH * 2];
    char proc_mem_path[HGW_MAX_PATH * 2];
    char ps_path[HGW_MAX_PATH * 2];
    char dmesg_path[HGW_MAX_PATH * 2];
    time_t now = time(NULL);
    struct tm tm_info;
    const char *tar_argv[7];
    const char *rm_argv[4];
    const char *ps_argv[5];

    if (ensure_dir(output_dir) != 0) {
        if (s_callback) s_callback(NULL, s_userdata);
        return -1;
    }

    localtime_r(&now, &tm_info);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm_info);

    snprintf(dir_name, sizeof(dir_name), "diag_%s.d", timestamp);
    snprintf(archive_name, sizeof(archive_name), "diag_%s.tar.gz", timestamp);
    if (join_path(temp_dir, sizeof(temp_dir), output_dir, dir_name) != 0 ||
        join_path(archive_path, sizeof(archive_path), output_dir, archive_name) != 0) {
        if (s_callback) s_callback(NULL, s_userdata);
        return -1;
    }

    if (ensure_dir(temp_dir) != 0) {
        if (s_callback) s_callback(NULL, s_userdata);
        return -1;
    }

    if (join_path(event_path, sizeof(event_path), temp_dir, "event.txt") != 0 ||
        join_path(proc_stat_path, sizeof(proc_stat_path), temp_dir, "proc_stat.txt") != 0 ||
        join_path(proc_mem_path, sizeof(proc_mem_path), temp_dir, "proc_meminfo.txt") != 0 ||
        join_path(ps_path, sizeof(ps_path), temp_dir, "ps.txt") != 0 ||
        join_path(dmesg_path, sizeof(dmesg_path), temp_dir, "dmesg.txt") != 0) {
        rm_argv[0] = "rm"; rm_argv[1] = "-rf"; rm_argv[2] = temp_dir; rm_argv[3] = NULL;
        run_argv(rm_argv);
        if (s_callback) s_callback(NULL, s_userdata);
        return -1;
    }

    write_event_file(event_path, event);
    copy_text_file("/proc/stat", proc_stat_path);
    copy_text_file("/proc/meminfo", proc_mem_path);

    ps_argv[0] = "ps"; ps_argv[1] = "-eo"; ps_argv[2] = "pid,ppid,comm,%cpu,%mem";
    ps_argv[3] = NULL;
    (void) run_argv_to_file(ps_argv, ps_path);
    {
        const char *dmesg_argv[] = { "dmesg", NULL };
        (void) run_argv_to_file(dmesg_argv, dmesg_path);
    }

    tar_argv[0] = "tar"; tar_argv[1] = "-czf"; tar_argv[2] = archive_path;
    tar_argv[3] = "-C";  tar_argv[4] = temp_dir; tar_argv[5] = "."; tar_argv[6] = NULL;
    rm_argv[0] = "rm"; rm_argv[1] = "-rf"; rm_argv[2] = temp_dir; rm_argv[3] = NULL;

    if (run_argv(tar_argv) != 0) {
        LOG_WARN("tar failed for %s", archive_path);
        run_argv(rm_argv);
        if (s_callback) s_callback(NULL, s_userdata);
        return -1;
    }

    run_argv(rm_argv);
    prune_archives(output_dir, a->cfg.max_archives);
    LOG_INFO("Diagnostics collected into %s", archive_path);
    if (s_callback) s_callback(archive_path, s_userdata);
    return 0;
}

static void *diag_thread_fn(void *arg) {
    diag_collect_sync((DiagArg *)arg);
    atomic_store(&s_diag_active, 0);
    return NULL;
}

int diag_collect(const AnomalyEvent *event) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s_diag_active, &expected, 1)) {
        LOG_WARN("Diag collection already in progress, skipping");
        return -1;
    }
    /* Hold mutex for the entire s_diag_arg setup so the spawned thread sees
     * a consistent snapshot — pthread_create acts as a release barrier. */
    pthread_mutex_lock(&s_cfg_mutex);
    s_diag_arg.has_event = (event != NULL);
    if (event) s_diag_arg.event = *event;
    s_diag_arg.cfg = s_cfg;
    pthread_mutex_unlock(&s_cfg_mutex);

    if (pthread_create(&s_diag_thread, NULL, diag_thread_fn, &s_diag_arg) != 0) {
        atomic_store(&s_diag_active, 0);
        LOG_ERROR("Failed to start diag collection thread");
        return -1;
    }
    pthread_detach(s_diag_thread);
    return 0;
}

void diag_collector_update_output_dir(const char *path) {
    if (!path || path[0] == '\0') return;
    pthread_mutex_lock(&s_cfg_mutex);
    strncpy(s_cfg.output_dir, path, sizeof(s_cfg.output_dir) - 1);
    s_cfg.output_dir[sizeof(s_cfg.output_dir) - 1] = '\0';
    pthread_mutex_unlock(&s_cfg_mutex);
    LOG_INFO("Diag output directory updated to: %s", path);
}

void diag_collector_cleanup(void) {
    /* Wait up to 30s for any in-flight collection to finish */
    for (int i = 0; i < 300 && atomic_load(&s_diag_active); i++)
        usleep(100000);
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_callback = NULL;
    s_userdata = NULL;
    pthread_mutex_destroy(&s_cfg_mutex);
}