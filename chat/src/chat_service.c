#include "chat_service.h"
#include "client.h"
#include "ratelimit.h"
#include "logger.h"
#include "db.h"

/* 子 Reactor 通知机制: 全局 eventfd 数组 (由 reactor 初始化) */
#define MAX_SUB_REACTORS 64
static int g_sub_eventfds[MAX_SUB_REACTORS];

/* 向指定子Reactor的eventfd写入通知, 唤醒它处理待发送数据 */
void chat_notify_sub_reactor(int sub_reactor_idx)
{
    if (sub_reactor_idx >= 0 && sub_reactor_idx < MAX_SUB_REACTORS &&
        g_sub_eventfds[sub_reactor_idx] >= 0) {
        uint64_t val = 1;
        ssize_t ret __attribute__((unused));
        ret = write(g_sub_eventfds[sub_reactor_idx], &val, sizeof(val));
    }
}

/* 保存子Reactor的eventfd, 供后续跨线程通知使用 */
void chat_set_eventfd(int idx, int efd)
{
    if (idx >= 0 && idx < MAX_SUB_REACTORS) {
        g_sub_eventfds[idx] = efd;
    }
}

/* 将消息写入客户端发送缓冲区, 并通知子Reactor发送 */
int chat_send_message(int fd, uint8_t msg_type,
                      const uint8_t *body, uint16_t body_len)
{
    client_t *cli = client_find_by_fd(fd);
    if (!cli || !cli->alive) {
        LOG_WARN("chat_send_message: client fd=%d not found or not alive", fd);
        return -1;
    }

    /* 计算总包大小: 包头 + 包体 */
    size_t total_len = PROTO_HEADER_LEN + body_len;
    uint8_t *packet = (uint8_t *)malloc(total_len);
    if (!packet) return -1;

    /* 构建包头 */
    proto_header_t hdr;
    proto_build_header(&hdr, msg_type, body_len);
    proto_serialize_header(packet, &hdr);

    /* 复制包体 */
    if (body && body_len > 0) {
        memcpy(packet + PROTO_HEADER_LEN, body, body_len);
    }

    /* 写入客户端发送缓冲区 (线程安全) */
    pthread_mutex_lock(&cli->outbuf_mutex);
    size_t written = ringbuf_write(cli->outbuf, packet, total_len);
    pthread_mutex_unlock(&cli->outbuf_mutex);

    free(packet);

    if (written < total_len) {
        LOG_WARN("chat_send_message: outbuf full for fd=%d, wrote=%zu/%zu",
                 fd, written, total_len);
        return -1;
    }

    /* 通知子 Reactor 有数据待发送 */
    chat_notify_sub_reactor(cli->sub_reactor_idx);

    return 0;
}

/* 处理登录请求 */
static void handle_login(chat_task_t *task)
{
    /* 包体格式: {"username":"xxx","password":"yyy"} */
    char username[MAX_USERNAME_LEN];
    char password[MAX_USERNAME_LEN];
    memset(username, 0, sizeof(username));
    memset(password, 0, sizeof(password));

    /* JSON 解析: 提取 username 和 password */
    char body[MAX_MSG_LEN];
    size_t body_len = task->body_len < MAX_MSG_LEN - 1
                      ? task->body_len : MAX_MSG_LEN - 1;
    memcpy(body, task->body, body_len);
    body[body_len] = '\0';

    char *u_start = strstr(body, "\"username\":\"");
    if (u_start) {
        u_start += 12;  /* strlen("\"username\":\"") = 12 */
        char *u_end = strchr(u_start, '"');
        if (u_end) {
            size_t len = (size_t)(u_end - u_start);
            if (len >= MAX_USERNAME_LEN) len = MAX_USERNAME_LEN - 1;
            memcpy(username, u_start, len);
            username[len] = '\0';
        }
    }

    char *p_start = strstr(body, "\"password\":\"");
    if (p_start) {
        p_start += 12;  /* strlen("\"password\":\"") = 12 */
        char *p_end = strchr(p_start, '"');
        if (p_end) {
            size_t len = (size_t)(p_end - p_start);
            if (len >= MAX_USERNAME_LEN) len = MAX_USERNAME_LEN - 1;
            memcpy(password, p_start, len);
            password[len] = '\0';
        }
    }

    /* 兼容旧格式: 纯文本用户名 (无密码) */
    if (strlen(username) == 0 && strlen(password) == 0) {
        size_t copy_len = task->body_len;
        if (copy_len >= MAX_USERNAME_LEN) copy_len = MAX_USERNAME_LEN - 1;
        memcpy(username, task->body, copy_len);
        username[copy_len] = '\0';

        /* 去除首尾空格/引号 */
        char *start = username;
        while (*start == ' ' || *start == '\n' || *start == '\r') start++;
        char *end = start + strlen(start) - 1;
        while (end > start && (*end == ' ' || *end == '\n' || *end == '\r')) {
            *end = '\0';
            end--;
        }
        if (*start == '"') start++;
        end = start + strlen(start) - 1;
        if (end > start && *end == '"') *end = '\0';

        if (strlen(start) == 0) {
            const char *err = "{\"status\":\"error\",\"msg\":\"username empty, use /login <user> <pass> or /register <user> <pass>\"}";
            chat_send_message(task->fd, MSG_LOGIN_RESP,
                              (const uint8_t *)err, strlen(err));
            return;
        }
        memmove(username, start, strlen(start) + 1);

        /* 旧格式无密码: 尝试只用用户名验证密码为空 */
        /* 如果数据库中有该用户且密码为空的, 则允许登入 (向后兼容) */
    }

    if (strlen(username) == 0 || strlen(username) >= MAX_USERNAME_LEN) {
        const char *err = "{\"status\":\"error\",\"msg\":\"invalid username\"}";
        chat_send_message(task->fd, MSG_LOGIN_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 检查是否已在其他连接登录 */
    client_t *exist = client_find_by_name(username);
    if (exist && exist->fd != task->fd && exist->logged_in) {
        const char *err = "{\"status\":\"error\",\"msg\":\"already logged in from another device\"}";
        chat_send_message(task->fd, MSG_LOGIN_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 数据库验证登录 */
    int is_admin = 0, user_status = 1;
    int login_ret = db_user_login(username, password, &is_admin, &user_status);

    if (login_ret == ERR_NOT_FOUND) {
        const char *err = "{\"status\":\"error\",\"msg\":\"user not found, please /register first\"}";
        chat_send_message(task->fd, MSG_LOGIN_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    if (login_ret == -2) {
        const char *err = "{\"status\":\"error\",\"msg\":\"account banned, contact admin\"}";
        chat_send_message(task->fd, MSG_LOGIN_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    if (login_ret != 0) {
        const char *err = "{\"status\":\"error\",\"msg\":\"wrong password\"}";
        chat_send_message(task->fd, MSG_LOGIN_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 标记登录 */
    client_t *cli = client_find_by_fd(task->fd);
    if (cli) {
        client_set_logged_in(cli, username);
        cli->is_admin = is_admin;
        LOG_INFO("User login: %s (fd=%d, admin=%d)", username, task->fd, is_admin);
    }

    /* 更新最后登录时间 */
    db_user_update_login(username);

    /* 登录成功响应 */
    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"username\":\"%s\",\"is_admin\":%d,\"online\":%d}",
             username, is_admin, client_online_count());
    chat_send_message(task->fd, MSG_LOGIN_RESP,
                      (const uint8_t *)resp, strlen(resp));

    /* 广播用户上线通知 */
    char join_msg[512];
    snprintf(join_msg, sizeof(join_msg),
             "{\"type\":\"join\",\"username\":\"%s\",\"online\":%d}",
             username, client_online_count());
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *iter = client_find_by_fd(i);
        if (iter && iter->alive && iter->logged_in && iter->fd != task->fd) {
            chat_send_message(iter->fd, MSG_PUBLIC_BROADCAST,
                              (const uint8_t *)join_msg, strlen(join_msg));
        }
    }
}

/* 处理注册请求 */
static void handle_register(chat_task_t *task)
{
    char username[MAX_USERNAME_LEN];
    char password[MAX_USERNAME_LEN];
    memset(username, 0, sizeof(username));
    memset(password, 0, sizeof(password));

    /* JSON 解析 */
    char body[MAX_MSG_LEN];
    size_t body_len = task->body_len < MAX_MSG_LEN - 1
                      ? task->body_len : MAX_MSG_LEN - 1;
    memcpy(body, task->body, body_len);
    body[body_len] = '\0';

    char *u_start = strstr(body, "\"username\":\"");
    if (u_start) {
        u_start += 12;  /* strlen("\"username\":\"") = 12 */
        char *u_end = strchr(u_start, '"');
        if (u_end) {
            size_t len = (size_t)(u_end - u_start);
            if (len >= MAX_USERNAME_LEN) len = MAX_USERNAME_LEN - 1;
            memcpy(username, u_start, len);
            username[len] = '\0';
        }
    }

    char *p_start = strstr(body, "\"password\":\"");
    if (p_start) {
        p_start += 12;  /* strlen("\"password\":\"") = 12 */
        char *p_end = strchr(p_start, '"');
        if (p_end) {
            size_t len = (size_t)(p_end - p_start);
            if (len >= MAX_USERNAME_LEN) len = MAX_USERNAME_LEN - 1;
            memcpy(password, p_start, len);
            password[len] = '\0';
        }
    }

    if (strlen(username) == 0 || strlen(username) >= MAX_USERNAME_LEN) {
        const char *err = "{\"status\":\"error\",\"msg\":\"invalid username (1-31 chars)\"}";
        chat_send_message(task->fd, MSG_REGISTER_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    if (strlen(password) == 0) {
        const char *err = "{\"status\":\"error\",\"msg\":\"password cannot be empty\"}";
        chat_send_message(task->fd, MSG_REGISTER_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    int ret = db_user_register(username, password);
    if (ret == ERR_ALREADY_EXISTS) {
        const char *err = "{\"status\":\"error\",\"msg\":\"username already exists\"}";
        chat_send_message(task->fd, MSG_REGISTER_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    if (ret != 0) {
        const char *err = "{\"status\":\"error\",\"msg\":\"registration failed, try again\"}";
        chat_send_message(task->fd, MSG_REGISTER_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"msg\":\"registration successful, please /login\"}");
    chat_send_message(task->fd, MSG_REGISTER_RESP,
                      (const uint8_t *)resp, strlen(resp));

    LOG_INFO("User registered: %s", username);
}

/* 处理管理员踢人请求 */
static void handle_kick(chat_task_t *task)
{
    client_t *cli = client_find_by_fd(task->fd);
    if (!cli || !cli->logged_in || !cli->is_admin) {
        const char *err = "{\"status\":\"error\",\"msg\":\"admin only\"}";
        chat_send_message(task->fd, MSG_KICK_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 解析目标用户名 */
    char target[MAX_USERNAME_LEN];
    memset(target, 0, sizeof(target));

    char *t_start = strstr((char *)task->body, "\"username\":\"");
    if (t_start) {
        t_start += 12;  /* strlen("\"username\":\"") = 12 */
        char *t_end = strchr(t_start, '"');
        if (t_end) {
            size_t len = (size_t)(t_end - t_start);
            if (len >= MAX_USERNAME_LEN) len = MAX_USERNAME_LEN - 1;
            memcpy(target, t_start, len);
        }
    }

    if (strlen(target) == 0) {
        /* 简单格式: 直接体就是用户名 */
        size_t copy_len = task->body_len < MAX_USERNAME_LEN - 1
                          ? task->body_len : MAX_USERNAME_LEN - 1;
        memcpy(target, task->body, copy_len);
        target[copy_len] = '\0';
    }

    if (strlen(target) == 0) {
        const char *err = "{\"status\":\"error\",\"msg\":\"usage: /kick <username>\"}";
        chat_send_message(task->fd, MSG_KICK_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 不能踢自己 */
    if (strcmp(target, cli->username) == 0) {
        const char *err = "{\"status\":\"error\",\"msg\":\"cannot kick yourself\"}";
        chat_send_message(task->fd, MSG_KICK_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 查找目标并踢下线 */
    client_t *target_cli = client_find_by_name(target);
    if (!target_cli || !target_cli->logged_in) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"status\":\"error\",\"msg\":\"user '%s' not online\"}", target);
        chat_send_message(task->fd, MSG_KICK_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 发送被踢通知 */
    const char *kicked_msg = "{\"type\":\"kicked\",\"msg\":\"you have been kicked by admin\"}";
    chat_send_message(target_cli->fd, MSG_ERROR_RESP,
                      (const uint8_t *)kicked_msg, strlen(kicked_msg));

    /* 标记下线 */
    target_cli->alive = 0;

    char ok_msg[256];
    snprintf(ok_msg, sizeof(ok_msg),
             "{\"status\":\"ok\",\"msg\":\"user '%s' has been kicked\"}", target);
    chat_send_message(task->fd, MSG_KICK_RESP,
                      (const uint8_t *)ok_msg, strlen(ok_msg));

    LOG_INFO("Admin '%s' kicked user '%s'", cli->username, target);
}

/* 处理公屏消息 */
static void handle_public_msg(chat_task_t *task)
{
    client_t *cli = client_find_by_fd(task->fd);
    if (!cli || !cli->logged_in) {
        const char *err = "{\"status\":\"error\",\"msg\":\"please login first\"}";
        chat_send_message(task->fd, MSG_ERROR_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 限流检查 */
    if (ratelimit_check(cli, RATE_LIMIT_CAPACITY, RATE_LIMIT_RATE) != 0) {
        const char *err = "{\"status\":\"error\",\"msg\":\"rate limited, slow down\"}";
        chat_send_message(task->fd, MSG_ERROR_RESP,
                          (const uint8_t *)err, strlen(err));
        LOG_WARN("Rate limit hit: fd=%d, user=%s", task->fd, cli->username);
        return;
    }

    client_update_active(cli);

    /* 构建广播消息 */
    size_t msg_size = MAX_USERNAME_LEN + task->body_len + 64;
    char *broadcast = (char *)malloc(msg_size);
    if (!broadcast) return;

    /* 截取消息体 (限制长度) */
    char msg_text[MAX_MSG_LEN];
    size_t text_len = task->body_len < MAX_MSG_LEN - 1
                      ? task->body_len : MAX_MSG_LEN - 1;
    memcpy(msg_text, task->body, text_len);
    msg_text[text_len] = '\0';

    snprintf(broadcast, msg_size,
             "{\"type\":\"public\",\"from\":\"%s\",\"is_admin\":%d,\"msg\":\"%s\"}",
             cli->username, cli->is_admin, msg_text);

    LOG_INFO("Public msg from %s: %s", cli->username, msg_text);

    /* 广播给所有已登录客户端 */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *target = client_find_by_fd(i);
        if (target && target->alive && target->logged_in) {
            chat_send_message(target->fd, MSG_PUBLIC_BROADCAST,
                              (const uint8_t *)broadcast, strlen(broadcast));
        }
    }

    free(broadcast);
}

/* 处理私聊消息 */
static void handle_private_msg(chat_task_t *task)
{
    client_t *cli = client_find_by_fd(task->fd);
    if (!cli || !cli->logged_in) {
        const char *err = "{\"status\":\"error\",\"msg\":\"please login first\"}";
        chat_send_message(task->fd, MSG_ERROR_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 限流检查 */
    if (ratelimit_check(cli, RATE_LIMIT_CAPACITY, RATE_LIMIT_RATE) != 0) {
        const char *err = "{\"status\":\"error\",\"msg\":\"rate limited, slow down\"}";
        chat_send_message(task->fd, MSG_ERROR_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    client_update_active(cli);

    /* 解析包体: "target_name:message" 或 JSON 格式 */
    char body[MAX_MSG_LEN];
    size_t body_len = task->body_len < MAX_MSG_LEN - 1
                      ? task->body_len : MAX_MSG_LEN - 1;
    memcpy(body, task->body, body_len);
    body[body_len] = '\0';

    /* 查找分隔符 (支持 "target:msg" 和 JSON 简单解析) */
    char *target_name = body;
    char *msg_content = NULL;

    /* 尝试 JSON 解析: {"to":"target","msg":"hello"} */
    char *to_start = strstr(body, "\"to\":\"");
    if (to_start) {
        to_start += 6;
        char *to_end = strchr(to_start, '"');

        /* 先解析 msg_content（在 body 被修改之前） */
        char *msg_start = strstr(body, "\"msg\":\"");
        if (msg_start) {
            msg_start += 7;
            char *msg_end = strchr(msg_start, '"');
            if (msg_end) *msg_end = '\0';
            msg_content = msg_start;
        }

        /* 再截断 target_name（不影响 msg 解析） */
        if (to_end) {
            *to_end = '\0';
            target_name = to_start;
        }
    }

    /* 尝试简单格式: target:message */
    if (!msg_content) {
        char *colon = strchr(body, ':');
        if (colon) {
            *colon = '\0';
            target_name = body;
            msg_content = colon + 1;
        } else {
            /* 纯文本, 无法解析 */
            const char *err = "{\"status\":\"error\",\"msg\":\"format: target:message\"}";
            chat_send_message(task->fd, MSG_PRIVATE_RESP,
                              (const uint8_t *)err, strlen(err));
            return;
        }
    }

    /* 查找目标客户端 */
    client_t *target = client_find_by_name(target_name);
    if (!target || !target->logged_in) {
        char err[256];
        size_t tn_len = strlen(target_name);
        if (tn_len > 200) tn_len = 200;  /* 限制长度避免 snprintf 警告 */
        snprintf(err, sizeof(err),
                 "{\"status\":\"error\",\"msg\":\"user '%.200s' not online\"}",
                 target_name);
        chat_send_message(task->fd, MSG_PRIVATE_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 发送私聊消息给目标 */
    char priv_msg[MAX_MSG_LEN + 128];
    snprintf(priv_msg, sizeof(priv_msg),
             "{\"type\":\"private\",\"from\":\"%.32s\",\"is_admin\":%d,\"msg\":\"%.4000s\"}",
             cli->username, cli->is_admin, msg_content ? msg_content : "");
    chat_send_message(target->fd, MSG_PRIVATE_RESP,
                      (const uint8_t *)priv_msg, strlen(priv_msg));

    /* 发送确认给发送方 */
    char ack[256];
    snprintf(ack, sizeof(ack),
             "{\"status\":\"ok\",\"to\":\"%.200s\",\"msg\":\"sent\"}",
             target_name);
    chat_send_message(task->fd, MSG_PRIVATE_RESP,
                      (const uint8_t *)ack, strlen(ack));

    LOG_INFO("Private msg: %s -> %s: %s",
             cli->username, target_name, msg_content ? msg_content : "");
}

/* 处理在线列表查询 */
static void handle_online_list(chat_task_t *task)
{
    client_t *cli = client_find_by_fd(task->fd);
    if (!cli) return;

    client_update_active(cli);

    char *list = client_get_online_list();
    if (!list) {
        const char *err = "{\"status\":\"error\",\"msg\":\"memory error\"}";
        chat_send_message(task->fd, MSG_ONLINE_LIST_RESP,
                          (const uint8_t *)err, strlen(err));
        return;
    }

    /* 包装为 JSON 响应 */
    size_t resp_len = strlen(list) + 64;
    char *resp = (char *)malloc(resp_len);
    if (resp) {
        snprintf(resp, resp_len,
                 "{\"status\":\"ok\",\"online\":%d,\"users\":%s}",
                 client_online_count(), list);
        chat_send_message(task->fd, MSG_ONLINE_LIST_RESP,
                          (const uint8_t *)resp, strlen(resp));
        free(resp);
    }

    free(list);
}

/* 处理心跳 */
static void handle_heartbeat(chat_task_t *task)
{
    client_t *cli = client_find_by_fd(task->fd);
    if (!cli) return;

    client_update_active(cli);

    /* 心跳响应 */
    const char *pong = "pong";
    chat_send_message(task->fd, MSG_HEARTBEAT_RESP,
                      (const uint8_t *)pong, strlen(pong));
}

/* 聊天服务主入口: 根据消息类型分发到各处理函数 */
void chat_service_process(void *arg)
{
    chat_task_t *task = (chat_task_t *)arg;
    if (!task) return;

    switch (task->msg_type) {
    case MSG_LOGIN_REQ:
        handle_login(task);
        break;
    case MSG_REGISTER_REQ:
        handle_register(task);
        break;
    case MSG_PUBLIC_MSG:
        handle_public_msg(task);
        break;
    case MSG_PRIVATE_REQ:
        handle_private_msg(task);
        break;
    case MSG_ONLINE_LIST_REQ:
        handle_online_list(task);
        break;
    case MSG_HEARTBEAT_REQ:
        handle_heartbeat(task);
        break;
    case MSG_KICK_REQ:
        handle_kick(task);
        break;
    default:
        LOG_WARN("Unknown message type: 0x%02X from fd=%d",
                 task->msg_type, task->fd);
        break;
    }

    /* 释放任务资源 */
    if (task->body) free(task->body);
    free(task);
}
