#ifndef RATELIMIT_H
#define RATELIMIT_H

#include "common.h"
#include "client.h"

/* 初始化令牌桶 (在 client_create 时调用) */
void ratelimit_init(client_t *cli, double capacity, double rate);

/* 令牌桶限流检查: 返回 0 允许, -1 被限流 */
int  ratelimit_check(client_t *cli, double capacity, double rate);

/* 获取当前令牌数 (调试用) */
double ratelimit_tokens(const client_t *cli);

#endif /* RATELIMIT_H */
