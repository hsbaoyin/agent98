#include "log.h"

#ifdef _WIN32
#include <windows.h>
#endif

static FILE *g_log_file = NULL;
static int g_log_level = LOG_LEVEL_INFO;

void log_init(const char *path, int level)
{
    log_shutdown();
    g_log_level = level;
    if (path != NULL && path[0] != '\0') {
        g_log_file = fopen(path, "a");
        if (g_log_file == NULL) {
            g_log_file = NULL;
        }
    }
}

void log_shutdown(void)
{
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_set_level(int level)
{
    g_log_level = level;
}

int log_get_level(void)
{
    return g_log_level;
}

static const char *level_name(int level)
{
    switch (level) {
    case LOG_LEVEL_ERROR:
        return "ERROR";
    case LOG_LEVEL_WARN:
        return "WARN ";
    case LOG_LEVEL_INFO:
        return "INFO ";
    case LOG_LEVEL_DEBUG:
        return "DEBUG";
    case LOG_LEVEL_TRACE:
        return "TRACE";
    default:
        return "?????";
    }
}

void log_write(int level, const char *fmt, ...)
{
    char msg[2048];
    char line[2200];
    time_t now;
    struct tm *tmv;
    va_list ap;
    int n;

    if (level > g_log_level) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) {
        msg[sizeof(msg) - 1] = '\0';
    }

    now = time(NULL);
    tmv = localtime(&now);
    if (tmv != NULL) {
        sprintf(line, "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
                tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                tmv->tm_hour, tmv->tm_min, tmv->tm_sec,
                level_name(level), msg);
    } else {
        sprintf(line, "[%s] %s\n", level_name(level), msg);
    }

    if (g_log_file != NULL) {
        fputs(line, g_log_file);
        fflush(g_log_file);
    }
#ifdef _WIN32
    OutputDebugStringA(line);
#else
    fputs(line, stderr);
#endif
}
