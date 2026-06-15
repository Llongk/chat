#include "heartbeat.h"
#include "client.h"
#include "logger.h"

/* 心跳回调: 遍历客户端, 检查超时 */
static struct {
    int timeout;
} g_hb_ctx;

static int hb_check_callback(client_t *cli, void *arg)
{
    (void)arg;
    if (!cli->alive) return 0;

    time_t now = time(NULL);
    if (now - cli->last_active > g_hb_ctx.timeout) {
        LOG_INFO("Heartbeat timeout: fd=%d, user=%s, idle=%lds",
                 cli->fd, cli->username, (long)(now - cli->last_active));
        cli->alive = 0;  /* 标记为不活跃, Reactor 会清理 */
    }
    return 0;
}

static void *heartbeat_thread(void *arg)
{
    heartbeat_t *hb = (heartbeat_t *)arg;

    while (hb->running) {
        sleep(hb->interval);

        if (!hb->running) break;

        g_hb_ctx.timeout = hb->timeout;
        client_foreach(hb_check_callback, NULL);
    }

    return NULL;
}

int heartbeat_start(heartbeat_t *hb, int timeout, int interval)
{
    if (!hb) return -1;

    hb->timeout  = timeout;
    hb->interval = interval;
    hb->running  = 1;

    if (pthread_create(&hb->thread, NULL, heartbeat_thread, hb) != 0) {
        LOG_ERROR("Failed to create heartbeat thread");
        hb->running = 0;
        return -1;
    }

    LOG_INFO("Heartbeat started: timeout=%ds, interval=%ds", timeout, interval);
    return 0;
}

void heartbeat_stop(heartbeat_t *hb)
{
    if (!hb || !hb->running) return;

    hb->running = 0;
    pthread_join(hb->thread, NULL);
    LOG_INFO("Heartbeat stopped");
}
