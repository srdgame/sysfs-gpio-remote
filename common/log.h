/*
 * sysfs-gpio-remote - logging
 */
#ifndef LOG_H
#define LOG_H

#include <stdarg.h>

enum log_level {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
};

/* set maximum level that gets printed (default LOG_WARN) */
void log_set_level(enum log_level level);

void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
void log_info(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
void log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* LOG_H */
