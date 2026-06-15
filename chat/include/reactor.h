#ifndef REACTOR_H
#define REACTOR_H

#include "common.h"
#include "threadpool.h"

/* 子 Reactor 结构体 */
typedef struct {
    int            epoll_fd;      /* 子 Reactor epoll 实例 */
    int            event_fd;      /* eventfd 用于跨线程唤醒 */
    pthread_t      thread;        /* 工作线程 */
    int            idx;           /* 索引编号 (0-based) */
    int            running;       /* 运行标志 */
    threadpool_t  *biz_pool;      /* 业务线程池指针 */
} sub_reactor_t;

/* Reactor 管理结构体 */
typedef struct {
    int            listen_fd;     /* 监听 socket */
    int            main_epoll_fd; /* 主 Reactor epoll 实例 */
    int            sub_count;     /* 子 Reactor 数量 */
    sub_reactor_t *subs;          /* 子 Reactor 数组 */
    int            rr_index;      /* 轮询分发索引 */
    int            running;       /* 运行标志 */
    int            stopped;       /* 是否已停止 (防重入) */
} reactor_t;

/* 初始化 Reactor: 创建 listen socket + epoll + 子 Reactor */
int  reactor_init(reactor_t *r, int port, int sub_count,
                  threadpool_t *biz_pool);

/* 启动 Reactor (启动所有子 Reactor 线程 + 主 Reactor 事件循环) */
int  reactor_run(reactor_t *r);

/* 停止 Reactor */
void reactor_stop(reactor_t *r);

/* 设置子 Reactor 的 eventfd (供 chat_service 通知使用) */
void reactor_setup_eventfds(reactor_t *r);

#endif /* REACTOR_H */
