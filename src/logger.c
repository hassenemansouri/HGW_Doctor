#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

enum {
    SYSLOG_PRI_INFO = LOG_INFO,
    SYSLOG_PRI_DEBUG = LOG_DEBUG,
};

#undef LOG_INFO
#undef LOG_DEBUG

#include "logger.h"

static LogLevel s_level = LOG_LEVEL_INFO;
static int      s_open = 0;

static int to_syslog_priority(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return LOG_ERR;
        case LOG_LEVEL_WARN:  return LOG_WARNING;
        case LOG_LEVEL_INFO:  return SYSLOG_PRI_INFO;
        case LOG_LEVEL_DEBUG:
        default:              return SYSLOG_PRI_DEBUG;
    }
}

static const char *level_name(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_DEBUG:
        default:              return "DEBUG";
    }
}

int logger_init(LogLevel level, const char *ident) {
    s_level = level;
    openlog((ident && ident[0] != '\0') ? ident : "hgw-doctor", LOG_PID, LOG_DAEMON);
    s_open = 1;
    return 0;
}

void logger_cleanup(void) {
    if (s_open) closelog();
    s_open = 0;
}

void logger_log(LogLevel level, const char *file, int line, const char *fmt, ...) {
    va_list ap;
    char message[1024];
    int written;

    if (level > s_level) return;

    va_start(ap, fmt);
    written = vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    if (written < 0) return;

    if (s_open) {
        syslog(to_syslog_priority(level), "%s:%d %s", file, line, message);
    } else {
        fprintf(stderr, "[%s] %s:%d %s\n", level_name(level), file, line, message);
    }
}