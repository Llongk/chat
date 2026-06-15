#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "common.h"

/* 任务回调函数类型 */
typedef void (*task_func_t)(void *arg);

/* 任务节点 */
typedef struct task_node {
    task_func_t        func;
    void              *arg;
    struct task_node  *next;
} task_node_t;

/* 线程池结构体 */
typedef struct {
    pthread_t     *threads;       /* 线程数组 */
    int            thread_count;  /* 线程数量 */
    task_node_t   *task_head;     /* 任务队列头 */
    task_node_t   *task_tail;     /* 任务队列尾 */
    pthread_mutex_t mutex;        /* 互斥锁 */
    pthread_cond_t  cond;         /* 条件变量 */
    int            shutdown;      /* 关闭标志 */
    int            task_count;    /* 当前任务数 */
} threadpool_t;

/* 创建线程池 */
threadpool_t *threadpool_create(int thread_count);

/* 销毁线程池 */
void threadpool_destroy(threadpool_t *pool);

/* 向线程池添加任务 */
int threadpool_add_task(threadpool_t *pool, task_func_t func, void *arg);

/* 获取当前任务队列中的任务数 */
int threadpool_task_count(threadpool_t *pool);

#endif /* THREADPOOL_H */
