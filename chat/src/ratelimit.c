#include "ratelimit.h"
#include <sys/time.h>

/*
 * 令牌桶算法:
 *
 *   每个客户端有一个令牌桶:
 *     - capacity: 桶容量 (最大可存放的令牌数, 允许突发流量)
 *     - rate:     令牌补充速率 (个/秒)
 *     - tokens:   当前令牌数
 *     - last_refill: 上次补充时间 (微秒级精度)
 *
 *   处理流程:
 *     1. 计算距离上次补充的时间间隔
 *     2. 补充 rate * elapsed 个令牌 (不超过 capacity)
 *     3. 如果 tokens >= 1, 消耗 1 个令牌, 放行
 *     4. 否则拒绝
 */

void ratelimit_init(client_t *cli, double capacity, double rate)
{
    if (!cli) return;
    cli->capacity = capacity;
    cli->rate     = rate;
    cli->tokens   = capacity;  /* 初始满令牌, 允许首次突发 */
    gettimeofday(&cli->last_refill, NULL);
}

int ratelimit_check(client_t *cli, double capacity, double rate)
{
    if (!cli) return -1;

    /* 如果容量/速率变化, 重新初始化 */
    if (cli->capacity != capacity || cli->rate != rate) {
        ratelimit_init(cli, capacity, rate);
    }

    struct timeval now;
    gettimeofday(&now, NULL);

    /* 计算时间差 (秒, 浮点) */
    double elapsed = (now.tv_sec  - cli->last_refill.tv_sec) +
                     (now.tv_usec - cli->last_refill.tv_usec) / 1000000.0;

    /* 补充令牌 */
    cli->tokens += elapsed * cli->rate;
    if (cli->tokens > cli->capacity) {
        cli->tokens = cli->capacity;  /* 不超过桶容量 */
    }
    cli->last_refill = now;

    /* 判断是否有可用令牌 */
    if (cli->tokens >= 1.0) {
        cli->tokens -= 1.0;
        return 0;  /* 放行 */
    }

    return -1;  /* 限流 */
}

double ratelimit_tokens(const client_t *cli)
{
    if (!cli) return 0.0;
    return cli->tokens;
}
