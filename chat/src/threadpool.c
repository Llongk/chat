#include "threadpool.h"
#include "logger.h"

static void *threadpool_worker(void *arg)
{
    threadpool_t *pool = (threadpool_t *)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        /* 等待任务或关闭信号 */
        while (pool->task_count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }

        /* 收到关闭信号且没有任务, 退出 */
        if (pool->shutdown && pool->task_count == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* 从队列取出一个任务 */
        task_node_t *node = pool->task_head;
        pool->task_head = node->next;
        if (!pool->task_head) pool->task_tail = NULL;
        pool->task_count--;

        pthread_mutex_unlock(&pool->mutex);

        /* 执行任务 */
        if (node->func) {
            node->func(node->arg);
        }
        free(node);
    }

    return NULL;
}

threadpool_t *threadpool_create(int thread_count)
{
    if (thread_count <= 0) thread_count = 1;

    threadpool_t *pool = (threadpool_t *)malloc(sizeof(threadpool_t));
    if (!pool) return NULL;

    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    pool->thread_count = thread_count;
    pool->task_head    = NULL;
    pool->task_tail    = NULL;
    pool->shutdown     = 0;
    pool->task_count   = 0;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL,
                           threadpool_worker, pool) != 0) {
            LOG_ERROR("Failed to create threadpool worker %d", i);
            /* 通知已创建的线程退出 */
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->cond);
            /* 等待已创建的线程 */
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->cond);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }

    LOG_INFO("Threadpool created with %d threads", thread_count);
    return pool;
}

void threadpool_destroy(threadpool_t *pool)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* 清理剩余任务 */
    task_node_t *node = pool->task_head;
    while (node) {
        task_node_t *next = node->next;
        free(node);
        node = next;
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool);

    LOG_INFO("Threadpool destroyed");
}

int threadpool_add_task(threadpool_t *pool, task_func_t func, void *arg)
{
    if (!pool) return -1;

    task_node_t *node = (task_node_t *)malloc(sizeof(task_node_t));
    if (!node) return -1;

    node->func = func;
    node->arg  = arg;
    node->next = NULL;

    pthread_mutex_lock(&pool->mutex);

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        free(node);
        return -1;
    }

    if (pool->task_tail) {
        pool->task_tail->next = node;
    } else {
        pool->task_head = node;
    }
    pool->task_tail = node;
    pool->task_count++;

    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}

int threadpool_task_count(threadpool_t *pool)
{
    if (!pool) return 0;
    int count;
    pthread_mutex_lock(&pool->mutex);
    count = pool->task_count;
    pthread_mutex_unlock(&pool->mutex);
    return count;
}
