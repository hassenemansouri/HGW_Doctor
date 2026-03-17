#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "diag_collector.h"
#include "logger.h"

static DiagConfig         s_cfg;
static diag_done_callback s_callback = NULL;
static void              *s_userdata = NULL;

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

static int run_shell_command(const char *command, int required_success) {
    int rc;

    if (!command || command[0] == '\0') return -1;

    rc = system(command);
    if (rc != 0) {
        LOG_WARN("Command failed rc=%d: %s", rc, command);
        return required_success ? -1 : rc;
    }

    return 0;
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
        case ANOMALY_CPU:     return "CPU";
        case ANOMALY_MEMORY:  return "Memory";
        case ANOMALY_PROCESS: return "Process";
        case ANOMALY_NONE:
        default:              return "None";
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

static void prune_archives(void) {
    DIR *dir;
    struct dirent *entry;
    struct {
        char path[HGW_MAX_PATH * 2];
        time_t mtime;
    } archives[64];
    size_t count = 0;

    if (s_cfg.max_archives == 0) return;

    dir = opendir(s_cfg.output_dir);
    if (!dir) return;

    while ((entry = readdir(dir)) != NULL && count < 64) {
        struct stat st;
        char full_path[HGW_MAX_PATH * 2];

        if (entry->d_name[0] == '.') continue;
        if (!strstr(entry->d_name, ".tar.gz")) continue;

        if (join_path(full_path, sizeof(full_path), s_cfg.output_dir, entry->d_name) != 0) {
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

    for (size_t i = s_cfg.max_archives; i < count; ++i) {
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

int diag_collect(const AnomalyEvent *event) {
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
    char command[HGW_MAX_PATH * 8];
    time_t now = time(NULL);
    struct tm tm_info;

    if (ensure_dir(s_cfg.output_dir) != 0) return -1;

    localtime_r(&now, &tm_info);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm_info);

    snprintf(dir_name, sizeof(dir_name), "diag_%s.d", timestamp);
    snprintf(archive_name, sizeof(archive_name), "diag_%s.tar.gz", timestamp);
    if (join_path(temp_dir, sizeof(temp_dir), s_cfg.output_dir, dir_name) != 0) return -1;
    if (join_path(archive_path, sizeof(archive_path), s_cfg.output_dir, archive_name) != 0) return -1;

    if (ensure_dir(temp_dir) != 0) return -1;

    if (join_path(event_path, sizeof(event_path), temp_dir, "event.txt") != 0) return -1;
    if (join_path(proc_stat_path, sizeof(proc_stat_path), temp_dir, "proc_stat.txt") != 0) return -1;
    if (join_path(proc_mem_path, sizeof(proc_mem_path), temp_dir, "proc_meminfo.txt") != 0) return -1;
    if (join_path(ps_path, sizeof(ps_path), temp_dir, "ps.txt") != 0) return -1;
    if (join_path(dmesg_path, sizeof(dmesg_path), temp_dir, "dmesg.txt") != 0) return -1;

    write_event_file(event_path, event);
    copy_text_file("/proc/stat", proc_stat_path);
    copy_text_file("/proc/meminfo", proc_mem_path);

    snprintf(command, sizeof(command), "ps -eo pid,ppid,comm,%%cpu,%%mem > '%s' 2>/dev/null", ps_path);
    (void) run_shell_command(command, 0);
    snprintf(command, sizeof(command), "dmesg | tail -n 200 > '%s' 2>/dev/null", dmesg_path);
    (void) run_shell_command(command, 0);

    snprintf(command, sizeof(command), "tar -czf '%s' -C '%s' .", archive_path, temp_dir);
    if (run_shell_command(command, 1) != 0) {
        snprintf(command, sizeof(command), "rm -rf '%s'", temp_dir);
        (void) run_shell_command(command, 0);
        return -1;
    }

    snprintf(command, sizeof(command), "rm -rf '%s'", temp_dir);
    (void) run_shell_command(command, 0);

    prune_archives();
    LOG_INFO("Diagnostics collected into %s", archive_path);
    if (s_callback) s_callback(archive_path, s_userdata);
    return 0;
}

void diag_collector_cleanup(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_callback = NULL;
    s_userdata = NULL;
}