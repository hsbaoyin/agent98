#include "log.h"

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_cs;
static int g_log_cs_inited = 0;
#endif

static FILE *g_log_file = NULL;
static int g_log_level = LOG_LEVEL_INFO;

void log_init(const char *path, int level)
{
#ifdef _WIN32
    if (!g_log_cs_inited) {
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_inited = 1;
    }
    EnterCriticalSection(&g_log_cs);
#endif

    log_shutdown();
    g_log_level = level;
    if (path != NULL && path[0] != '\0') {
        g_log_file = fopen(path, "a");
        if (g_log_file == NULL) {
            g_log_file = NULL;
        }
    }

#ifdef _WIN32
    LeaveCriticalSection(&g_log_cs);
#endif
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
    struct tm local_tm;
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

#ifdef _WIN32
    if (g_log_cs_inited) {
        EnterCriticalSection(&g_log_cs);
    }
#endif

    now = time(NULL);
    tmv = localtime(&now);
    if (tmv != NULL) {
        local_tm = *tmv;
        sprintf(line, "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
                local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
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
    if (g_log_cs_inited) {
        LeaveCriticalSection(&g_log_cs);
    }
#else
    fputs(line, stderr);
#endif
}
