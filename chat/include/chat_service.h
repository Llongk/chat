#ifndef CHAT_SERVICE_H
#define CHAT_SERVICE_H

#include "common.h"
#include "protocol.h"

/* 聊天任务参数 */
typedef struct {
    int         fd;            /* 客户端 fd */
    uint8_t     msg_type;      /* 消息类型 */
    uint8_t    *body;          /* 包体数据 (malloc, 由处理函数 free) */
    uint16_t    body_len;      /* 包体长度 */
} chat_task_t;

/* 聊天服务处理入口 (作为线程池任务回调) */
void chat_service_process(void *arg);

/* 发送协议包到客户端 (带包头组装) */
int  chat_send_message(int fd, uint8_t msg_type,
                       const uint8_t *body, uint16_t body_len);

/* 向子 Reactor 发送通知 (有新数据待发送) */
void chat_notify_sub_reactor(int sub_reactor_idx);

/* 设置子 Reactor 的 eventfd (由 reactor 初始化时调用) */
void chat_set_eventfd(int idx, int efd);

#endif /* CHAT_SERVICE_H */
