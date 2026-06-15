#ifndef LOGGER_H
#define LOGGER_H

#include "common.h"

/* 日志级别 */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

/* 初始化日志系统 (filepath: 日志文件路径, NULL 表示仅输出到 stdout) */
int  logger_init(const char *filepath);

/* 关闭日志系统 */
void logger_close(void);

/* 核心日志函数 */
void logger_write(log_level_t level, const char *file, int line,
                  const char *fmt, ...);

/* 便捷宏 */
#define LOG_DEBUG(fmt, ...) \
    logger_write(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  \
    logger_write(LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  \
    logger_write(LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    logger_write(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* LOGGER_H */
