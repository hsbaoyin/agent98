/*
 * Agent98 - logging to file and debugger (OutputDebugString on Win32).
 */
#ifndef A98_LOG_H
#define A98_LOG_H

#include "base.h"

#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_DEBUG 3
#define LOG_LEVEL_TRACE 4

void log_init(const char *path, int level);
void log_shutdown(void);
void log_set_level(int level);
int log_get_level(void);

void log_write(int level, const char *fmt, ...);

#define log_error(...) log_write(LOG_LEVEL_ERROR, __VA_ARGS__)
#define log_warn(...) log_write(LOG_LEVEL_WARN, __VA_ARGS__)
#define log_info(...) log_write(LOG_LEVEL_INFO, __VA_ARGS__)
#define log_debug(...) log_write(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define log_trace(...) log_write(LOG_LEVEL_TRACE, __VA_ARGS__)

#endif /* A98_LOG_H */
