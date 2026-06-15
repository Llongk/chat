#include "client.h"
#include "logger.h"
#include "ratelimit.h"

/* 全局客户端表: 以 fd 为索引, 支持快速 O(1) 查找 */
static client_t *g_clients[MAX_CLIENTS];
static int       g_online_count = 0;
static pthread_mutex_t g_client_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 初始化客户端管理器: 清空客户端表, 重置在线计数 */
void client_manager_init(void)
{
    memset(g_clients, 0, sizeof(g_clients));
    g_online_count = 0;
    LOG_INFO("Client manager initialized");
}

/* 销毁所有客户端, 释放客户端表资源 */
void client_manager_destroy(void)
{
    pthread_mutex_lock(&g_client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i]) {
            client_destroy(g_clients[i]);
            g_clients[i] = NULL;
        }
    }
    g_online_count = 0;
    pthread_mutex_unlock(&g_client_mutex);
    LOG_INFO("Client manager destroyed");
}

/* 创建客户端对象: 分配环形缓冲区, 注册到全局表, 更新在线计数 */
client_t *client_create(int fd, struct sockaddr_in *addr)
{
    if (fd < 0 || fd >= MAX_CLIENTS) {
        LOG_WARN("fd %d out of range (MAX_CLIENTS=%d)", fd, MAX_CLIENTS);
        return NULL;
    }

    client_t *cli = (client_t *)malloc(sizeof(client_t));
    if (!cli) return NULL;

    memset(cli, 0, sizeof(client_t));
    cli->fd     = fd;
    cli->addr   = *addr;
    cli->alive  = 1;
    cli->logged_in = 0;
    cli->is_admin = 0;
    cli->last_active = time(NULL);
    ratelimit_init(cli, RATE_LIMIT_CAPACITY, RATE_LIMIT_RATE);

    cli->inbuf  = ringbuf_create(RINGBUF_SIZE);
    cli->outbuf = ringbuf_create(RINGBUF_SIZE);
    if (!cli->inbuf || !cli->outbuf) {
        if (cli->inbuf)  ringbuf_destroy(cli->inbuf);
        if (cli->outbuf) ringbuf_destroy(cli->outbuf);
        free(cli);
        return NULL;
    }

    cli->proto_ctx.state     = PARSE_STATE_HEADER;
    cli->proto_ctx.body_read = 0;
    pthread_mutex_init(&cli->outbuf_mutex, NULL);

    pthread_mutex_lock(&g_client_mutex);
    g_clients[fd] = cli;
    g_online_count++;
    pthread_mutex_unlock(&g_client_mutex);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
    LOG_INFO("Client created: fd=%d, ip=%s:%d",
             fd, ip_str, ntohs(addr->sin_port));
    return cli;
}

/* 销毁客户端: 从全局表移除, 释放环形缓冲区和socket */
void client_destroy(client_t *cli)
{
    if (!cli) return;

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cli->addr.sin_addr, ip_str, sizeof(ip_str));
    LOG_INFO("Client destroyed: fd=%d, user=%s, ip=%s:%d",
             cli->fd, cli->username, ip_str, ntohs(cli->addr.sin_port));

    if (cli->fd >= 0 && cli->fd < MAX_CLIENTS) {
        pthread_mutex_lock(&g_client_mutex);
        g_clients[cli->fd] = NULL;
        g_online_count--;
        pthread_mutex_unlock(&g_client_mutex);
    }

    if (cli->inbuf)  ringbuf_destroy(cli->inbuf);
    if (cli->outbuf) ringbuf_destroy(cli->outbuf);
    pthread_mutex_destroy(&cli->outbuf_mutex);

    if (cli->fd >= 0) {
        shutdown(cli->fd, SHUT_RDWR);
        close(cli->fd);
    }

    free(cli);
}

/* 根据fd查找客户端对象, O(1)复杂度 */
client_t *client_find_by_fd(int fd)
{
    if (fd < 0 || fd >= MAX_CLIENTS) return NULL;
    return g_clients[fd];
}

/* 根据用户名查找已登录的客户端, 遍历全局表 */
client_t *client_find_by_name(const char *username)
{
    if (!username) return NULL;

    pthread_mutex_lock(&g_client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i] && g_clients[i]->logged_in &&
            strcmp(g_clients[i]->username, username) == 0) {
            pthread_mutex_unlock(&g_client_mutex);
            return g_clients[i];
        }
    }
    pthread_mutex_unlock(&g_client_mutex);
    return NULL;
}

/* 获取当前在线客户端数量 (线程安全) */
int client_online_count(void)
{
    int count;
    pthread_mutex_lock(&g_client_mutex);
    count = g_online_count;
    pthread_mutex_unlock(&g_client_mutex);
    return count;
}

/* 遍历所有活跃客户端, 执行回调 (返回非0停止) */
void client_foreach(int (*callback)(client_t *cli, void *arg), void *arg)
{
    pthread_mutex_lock(&g_client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i] && g_clients[i]->alive) {
            if (callback(g_clients[i], arg) != 0) break;
        }
    }
    pthread_mutex_unlock(&g_client_mutex);
}

/* 生成在线列表JSON数组, 调用者需free返回值 */
char *client_get_online_list(void)
{
    /* 预分配缓冲区 */
    size_t buf_size = MAX_CLIENTS * (MAX_USERNAME_LEN + 32);
    char *list = (char *)malloc(buf_size);
    if (!list) return NULL;

    int offset = 0;
    offset += snprintf(list + offset, buf_size - offset, "[");

    pthread_mutex_lock(&g_client_mutex);
    int first = 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i] && g_clients[i]->logged_in) {
            if (!first) {
                offset += snprintf(list + offset, buf_size - offset, ",");
            }
            offset += snprintf(list + offset, buf_size - offset,
                               "{\"name\":\"%s\",\"admin\":%d}",
                               g_clients[i]->username,
                               g_clients[i]->is_admin);
            first = 0;
        }
    }
    pthread_mutex_unlock(&g_client_mutex);

    offset += snprintf(list + offset, buf_size - offset, "]");
    return list;
}

/* 设置客户端为已登录状态, 保存用户名 */
void client_set_logged_in(client_t *cli, const char *username)
{
    if (!cli || !username) return;
    safe_strncpy(cli->username, username, MAX_USERNAME_LEN);
    cli->logged_in = 1;
    cli->last_active = time(NULL);
}

/* 设置客户端为未登录状态, 清除用户名和管理员标志 */
void client_set_logged_out(client_t *cli)
{
    if (!cli) return;
    cli->logged_in = 0;
    cli->is_admin = 0;
    cli->username[0] = '\0';
}

/* 更新客户端最后活跃时间 (用于心跳检测) */
void client_update_active(client_t *cli)
{
    if (!cli) return;
    cli->last_active = time(NULL);
}
