#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* 服务端默认配置 */
#define DEFAULT_PORT            8888
#define MAX_EVENTS              10240
#define MAX_CLIENTS             10240
#define MAX_USERNAME_LEN        32
#define MAX_MSG_LEN             4096
#define LISTEN_BACKLOG          128

/* 环形缓冲区大小 */
#define RINGBUF_SIZE            (64 * 1024)

/* 子 Reactor 默认数量 */
#define DEFAULT_SUB_REACTORS    4

/* 业务线程池默认大小 */
#define DEFAULT_BIZ_THREADS     4

/* 心跳超时 (秒) */
#define HEARTBEAT_TIMEOUT       3600
#define HEARTBEAT_CHECK_INTERVAL 5

/* 令牌桶限流: 桶容量 + 每秒补充令牌数 */
#define RATE_LIMIT_CAPACITY     5     /* 桶容量(允许突发) */
#define RATE_LIMIT_RATE         5.0   /* 每秒补充令牌数 */

/* epoll 等待超时 (毫秒) */
#define EPOLL_TIMEOUT_MS        100

/* 数据库配置 */
#define DB_HOST                 "localhost"   /* localhost=Unix socket, 127.0.0.1=TCP */
#define DB_USER                 "chat_app"
#define DB_PASS                 "chat123"
#define DB_NAME                 "chat_db"
#define DB_PORT                 3306

/* 管理员自动创建 (首次运行时若用户表为空, 自动创建此管理员) */
#define DEFAULT_ADMIN_USER      "admin"
#define DEFAULT_ADMIN_PASS      "admin123"

/* 协议常量 */
#define PROTO_MAGIC             0x48494D53  /* "HIMS" */
#define PROTO_HEADER_LEN        8
#define PROTO_MAX_BODY_LEN      65535

/* 错误码 */
#define ERR_NONE                0
#define ERR_SOCKET_CREATE      -1
#define ERR_SOCKET_BIND        -2
#define ERR_SOCKET_LISTEN      -3
#define ERR_EPOLL_CREATE       -4
#define ERR_THREAD_CREATE      -5
#define ERR_MEMORY             -6
#define ERR_FULL               -7
#define ERR_NOT_FOUND          -8
#define ERR_ALREADY_EXISTS     -9
#define ERR_RATE_LIMITED       -10
#define ERR_PROTO_PARSE        -11
#define ERR_SEND_FAILED        -12

/* 非阻塞设置 */
static inline int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 安全的字符串复制 */
static inline size_t safe_strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dest[i] = src[i];
    dest[i] = '\0';
    return i;
}

#endif /* COMMON_H */
