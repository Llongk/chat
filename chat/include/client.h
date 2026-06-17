#ifndef CLIENT_H
#define CLIENT_H

#include "common.h"
#include "ringbuf.h"
#include "protocol.h"

/* 客户端结构体 */
typedef struct client {
    int                fd;              /* socket 文件描述符 */
    struct sockaddr_in addr;            /* 客户端地址 */
    char               username[MAX_USERNAME_LEN]; /* 用户名 */
    int                logged_in;       /* 是否已登录 */
    int                is_admin;        /* 是否为管理员 */
    int                is_banned;       /* 是否被禁言 (1=禁言,0=正常) */
    time_t             last_active;     /* 最后活跃时间 */
    ringbuf_t         *inbuf;           /* 接收环形缓冲区 */
    ringbuf_t         *outbuf;          /* 发送环形缓冲区 */
    pthread_mutex_t    outbuf_mutex;    /* 发送缓冲区锁 */
    proto_ctx_t        proto_ctx;       /* 协议解析上下文 */
    int                sub_reactor_idx; /* 所属子 Reactor 索引 */
    int                epoll_fd;        /* 所属 epoll 实例 fd */
    /* 令牌桶限流字段 */
    double             tokens;          /* 当前令牌数 */
    double             capacity;        /* 桶容量(最大令牌数) */
    double             rate;            /* 令牌补充速率(个/秒) */
    struct timeval     last_refill;     /* 上次补充令牌的时间 */
    int                alive;           /* 连接是否存活 */
} client_t;

/* 初始化客户端管理模块 */
void client_manager_init(void);

/* 关闭客户端管理模块 */
void client_manager_destroy(void);

/* 创建新客户端 (分配 client_t 结构体) */
client_t *client_create(int fd, struct sockaddr_in *addr);

/* 销毁客户端 (释放所有资源) */
void client_destroy(client_t *cli);

/* 根据 fd 查找客户端 */
client_t *client_find_by_fd(int fd);

/* 根据用户名查找客户端 */
client_t *client_find_by_name(const char *username);

/* 获取所有在线客户端数量 */
int client_online_count(void);

/* 遍历所有在线客户端 (回调返回 0 继续, 非 0 停止) */
void client_foreach(int (*callback)(client_t *cli, void *arg), void *arg);

/* 获取在线用户名列表 (返回 JSON 格式字符串, 调用者需 free) */
char *client_get_online_list(void);

/* 标记客户端已登录 */
void client_set_logged_in(client_t *cli, const char *username);

/* 标记客户端已登出 */
void client_set_logged_out(client_t *cli);

/* 更新客户端活跃时间 */
void client_update_active(client_t *cli);

/* 设置客户端禁言状态 (banned: 1=禁言, 0=正常) */
void client_set_banned(client_t *cli, int banned);

#endif /* CLIENT_H */
