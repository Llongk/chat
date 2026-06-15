#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "common.h"

/* 心跳管理结构体 */
typedef struct {
    pthread_t  thread;      /* 检测线程 */
    int        running;     /* 运行标志 */
    int        timeout;     /* 超时时间 (秒) */
    int        interval;    /* 检测间隔 (秒) */
} heartbeat_t;

/* 启动心跳检测 (返回 0 成功) */
int heartbeat_start(heartbeat_t *hb, int timeout, int interval);

/* 停止心跳检测 */
void heartbeat_stop(heartbeat_t *hb);

#endif /* HEARTBEAT_H */
