/*
 * sysfs-gpio-remote - logging
 */
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/time.h>

#include "log.h"

static enum log_level current_level = LOG_WARN;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_tag[] = {
    [LOG_ERROR] = "ERROR",
    [LOG_WARN]  = "WARN",
    [LOG_INFO]  = "INFO",
    [LOG_DEBUG] = "DEBUG",
};

void log_set_level(enum log_level level) {
    current_level = level;
}

static void log_vprintf(enum log_level level, const char *fmt, va_list ap) {
    if (level > current_level)
        return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "%s.%03ld [%s] ", stamp, tv.tv_usec / 1000,
            level_tag[level]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&log_lock);
}

#define LOG_FN(name, lvl)                       \
    void name(const char *fmt, ...) {           \
        va_list ap;                             \
        va_start(ap, fmt);                      \
        log_vprintf(lvl, fmt, ap);              \
        va_end(ap);                             \
    }

LOG_FN(log_error, LOG_ERROR)
LOG_FN(log_warn, LOG_WARN)
LOG_FN(log_info, LOG_INFO)
LOG_FN(log_debug, LOG_DEBUG)
