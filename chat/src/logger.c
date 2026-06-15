#include "logger.h"
#include <sys/time.h>

static FILE      *g_log_file   = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    default:        return "???? ";
    }
}

int logger_init(const char *filepath)
{
    if (filepath) {
        g_log_file = fopen(filepath, "a");
        if (!g_log_file) {
            fprintf(stderr, "[LOGGER] Cannot open log file: %s\n", filepath);
            return -1;
        }
    }
    return 0;
}

void logger_close(void)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void logger_write(log_level_t level, const char *file, int line,
                  const char *fmt, ...)
{
    struct timeval tv;
    struct tm tm_info;
    char time_buf[32];
    char msg_buf[2048];

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    pthread_mutex_lock(&g_log_mutex);

    /* 写入文件 */
    if (g_log_file) {
        fprintf(g_log_file, "[%s.%03ld] [%s] %s:%d - %s\n",
                time_buf, tv.tv_usec / 1000, level_str(level),
                file, line, msg_buf);
        fflush(g_log_file);
    }

    /* 同时输出到 stdout (方便调试) */
    fprintf(stdout, "[%s.%03ld] [%s] %s:%d - %s\n",
            time_buf, tv.tv_usec / 1000, level_str(level),
            file, line, msg_buf);
    fflush(stdout);

    pthread_mutex_unlock(&g_log_mutex);
}
