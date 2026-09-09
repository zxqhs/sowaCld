#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static LogLevel g_level = LOG_INFO;

void log_set_level(LogLevel level)
{
    g_level = level;
}

LogLevel log_get_level(void)
{
    return g_level;
}

void log_msg(LogLevel level, const char *fmt, ...)
{
    static const char *names[] = { "ERROR", "WARN", "INFO", "DEBUG" };
    struct tm tm;
    time_t now;
    char stamp[32];
    va_list ap;

    if (level > g_level)
        return;

    now = time(NULL);
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "%s [%s] ", stamp, names[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
