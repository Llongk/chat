/**
 * 高并发 IM 聊天室 - 命令行客户端 
 *
 * 使用方法:
 *   ./chat_client <server_ip> <server_port>
 *
 * 命令:
 *   /login <用户名>       登录
 *   /msg <消息>          发送公屏消息 (直接输入文本也可)
 *   /to <用户名> <消息>   发送私聊消息
 *   /online              查看在线用户
 *   /quit                退出
 */

#include "common.h"
#include "protocol.h"

#define RECV_BUF_SIZE (64 * 1024)

/* ── ANSI 颜色代码 ── */
#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_DIM     "\033[2m"
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_MAGENTA "\033[35m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[37m"
#define CLR_BG_BLK  "\033[40m"

/* ── 全局状态 ── */
static int            g_sockfd    = -1;
static int            g_running   = 1;
static char           g_username[MAX_USERNAME_LEN];
static int            g_logged_in = 0;
static int            g_is_admin  = 0;
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── 工具函数 ── */
static void get_time_str(char *buf, size_t size)
{
    struct timeval tv;
    struct tm tm_info;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_info);
    strftime(buf, size, "%H:%M:%S", &tm_info);
}

static void print_separator(void)
{
    printf(CLR_DIM "──────────────────────────────────────────────────" CLR_RESET "\n");
}

static void print_system_msg(const char *icon, const char *color, const char *msg)
{
    printf("\n" CLR_DIM "%s" CLR_RESET " %s%s %s" CLR_RESET "\n",
           "     ", color, icon, msg);
}

/* ── 发送协议包 ── */
static int send_packet(uint8_t type, const uint8_t *body, uint16_t body_len)
{
    uint8_t packet[PROTO_HEADER_LEN + PROTO_MAX_BODY_LEN];

    proto_header_t hdr;
    proto_build_header(&hdr, type, body_len);
    proto_serialize_header(packet, &hdr);

    if (body && body_len > 0) {
        memcpy(packet + PROTO_HEADER_LEN, body, body_len);
    }

    size_t total = PROTO_HEADER_LEN + body_len;
    pthread_mutex_lock(&g_send_mutex);
    ssize_t n = send(g_sockfd, packet, total, MSG_NOSIGNAL);
    pthread_mutex_unlock(&g_send_mutex);

    return (n == (ssize_t)total) ? 0 : -1;
}

/* ── 简易 JSON 字段提取 ── */
static int json_get_str(const char *json, const char *key,
                        char *out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *start = strstr(json, search);
    if (!start) return -1;
    start += strlen(search);
    const char *end = strchr(start, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

/* ── 接收线程 ── */
static void *receiver_thread(void *arg);

/* ── 心跳线程 ── */
static void *heartbeat_thread(void *arg);

static void *receiver_thread(void *arg)
{
    (void)arg;
    uint8_t buf[RECV_BUF_SIZE];
    size_t  buf_len = 0;
    char    time_str[16];

    while (g_running) {
        fd_set rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        FD_ZERO(&rfds);
        FD_SET(g_sockfd, &rfds);

        int ret = select(g_sockfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) break;
        if (ret == 0) continue;

        ssize_t n = recv(g_sockfd, buf + buf_len,
                         sizeof(buf) - buf_len, 0);
        if (n <= 0) {
            printf("\n" CLR_RED " ══ 连接已断开 ══" CLR_RESET "\n");
            g_running = 0;
            break;
        }

        buf_len += n;

        while (buf_len >= PROTO_HEADER_LEN) {
            proto_header_t hdr;
            proto_deserialize_header(buf, &hdr);

            if (!proto_validate_header(&hdr)) {
                memmove(buf, buf + 1, buf_len - 1);
                buf_len--;
                continue;
            }

            size_t total_len = PROTO_HEADER_LEN + hdr.body_len;
            if (buf_len < total_len) break;

            char body[PROTO_MAX_BODY_LEN + 1];
            if (hdr.body_len > 0)
                memcpy(body, buf + PROTO_HEADER_LEN, hdr.body_len);
            body[hdr.body_len] = '\0';

            memmove(buf, buf + total_len, buf_len - total_len);
            buf_len -= total_len;

            get_time_str(time_str, sizeof(time_str));

            switch (hdr.type) {

            /* ── 公屏广播 ── */
            case MSG_PUBLIC_BROADCAST: {
                char from[64] = "", msg[4096] = "", type[32] = "";
                int is_admin = 0;
                json_get_str(body, "from", from, sizeof(from));
                json_get_str(body, "msg",  msg,  sizeof(msg));
                json_get_str(body, "type", type, sizeof(type));

                /* 提取 is_admin */
                char *adm = strstr(body, "\"is_admin\":");
                if (adm) {
                    adm += 11;  /* strlen("\"is_admin\":") = 11 */
                    is_admin = atoi(adm);
                }

                if (strcmp(type, "join") == 0) {
                    print_system_msg("✦", CLR_GREEN, "");
                    printf(CLR_GREEN CLR_BOLD "  %s" CLR_RESET, from);
                    if (is_admin) printf(CLR_YELLOW "★" CLR_RESET);
                    printf(CLR_DIM " 加入聊天室" CLR_RESET);
                    /* 在线人数 */
                    char online_str[16] = "";
                    char *on = strstr(body, "\"online\":");
                    if (on) {
                        on += 9;
                        size_t i = 0;
                        while (*on >= '0' && *on <= '9' && i < 15)
                            online_str[i++] = *on++;
                        online_str[i] = '\0';
                    }
                    if (online_str[0])
                        printf(CLR_DIM "  当前在线: %s 人" CLR_RESET, online_str);
                    printf("\n");
                } else if (strcmp(type, "leave") == 0) {
                    print_system_msg("✧", CLR_RED, "");
                    printf(CLR_RED CLR_BOLD "  %s" CLR_RESET
                           CLR_DIM " 离开聊天室" CLR_RESET, from);
                    printf("\n");
                } else {
                    printf("\n" CLR_DIM "%s" CLR_RESET " ", time_str);
                    if (is_admin) printf(CLR_YELLOW "★" CLR_RESET " ");
                    printf(CLR_BOLD CLR_GREEN "%s" CLR_RESET
                           CLR_DIM " » " CLR_RESET "%s\n",
                           from, msg);
                }
                break;
            }

            /* ── 私聊消息 ── */
            case MSG_PRIVATE_RESP: {
                char from[64] = "", msg[4096] = "", status[32] = "";
                int is_admin = 0;
                json_get_str(body, "from", from, sizeof(from));
                json_get_str(body, "msg",  msg,  sizeof(msg));
                json_get_str(body, "status", status, sizeof(status));

                /* 提取 is_admin */
                char *adm = strstr(body, "\"is_admin\":");
                if (adm) {
                    adm += 11;  /* strlen("\"is_admin\":") = 11 */
                    is_admin = atoi(adm);
                }

                if (from[0]) {
                    /* 别人发给我的私聊 */
                    printf("\n" CLR_DIM "%s" CLR_RESET " "
                           CLR_BOLD CLR_YELLOW "[%s%s" CLR_YELLOW " → 你]" CLR_RESET "\n"
                           "       " CLR_YELLOW "%s" CLR_RESET "\n",
                           time_str, from, is_admin ? "★" : "", msg);
                } else if (strcmp(status, "ok") == 0) {
                    /* 我发出的私聊已送达 */
                    char to[64] = "";
                    json_get_str(body, "to", to, sizeof(to));
                    printf(CLR_DIM "       ✓ 已送达 " CLR_BOLD "%s" CLR_RESET "\n", to);
                } else {
                    print_system_msg("✗", CLR_RED, body);
                    printf("\n");
                }
                break;
            }

            /* ── 登录响应 ── */
            case MSG_LOGIN_RESP: {
                char status[32] = "", username[64] = "", err[256] = "";
                json_get_str(body, "status",   status,   sizeof(status));
                json_get_str(body, "username", username, sizeof(username));
                json_get_str(body, "msg",      err,      sizeof(err));

                /* 提取 is_admin */
                char *admin_start = strstr(body, "\"is_admin\":");
                int is_admin = 0;
                if (admin_start) {
                    admin_start += 11;  /* strlen("\"is_admin\":") = 11 */
                    is_admin = atoi(admin_start);
                }

                if (strcmp(status, "ok") == 0) {
                    g_logged_in = 1;
                    g_is_admin = is_admin;
                    safe_strncpy(g_username, username, MAX_USERNAME_LEN);
                    printf("\n" CLR_GREEN " ✓ 登录成功!" CLR_RESET
                           "  欢迎 " CLR_BOLD CLR_CYAN "%s" CLR_RESET,
                           username);
                    if (is_admin) {
                        printf(" " CLR_YELLOW "[管理员]" CLR_RESET);
                    }
                    printf("\n");
                    /* 启动心跳 */
                    pthread_t hb_tid;
                    pthread_create(&hb_tid, NULL, heartbeat_thread, NULL);
                    pthread_detach(hb_tid);
                } else {
                    printf("\n" CLR_RED " ✗ 登录失败: %s" CLR_RESET "\n",
                           err[0] ? err : "未知错误");
                    g_logged_in = 0;
                    g_username[0] = '\0';
                    g_is_admin = 0;
                }
                break;
            }

            /* ── 注册响应 ── */
            case MSG_REGISTER_RESP: {
                char status[32] = "", msg[256] = "";
                json_get_str(body, "status", status, sizeof(status));
                json_get_str(body, "msg",    msg,    sizeof(msg));

                if (strcmp(status, "ok") == 0) {
                    printf("\n" CLR_GREEN " ✓ %s" CLR_RESET "\n",
                           msg[0] ? msg : "注册成功! 请使用 /login 登录");
                } else {
                    printf("\n" CLR_RED " ✗ 注册失败: %s" CLR_RESET "\n",
                           msg[0] ? msg : "未知错误");
                }
                break;
            }

            /* ── 踢人响应 ── */
            case MSG_KICK_RESP: {
                char status[32] = "", msg[256] = "";
                json_get_str(body, "status", status, sizeof(status));
                json_get_str(body, "msg",    msg,    sizeof(msg));

                if (strcmp(status, "ok") == 0) {
                    printf(CLR_DIM "       ✓ %s" CLR_RESET "\n", msg[0] ? msg : "操作成功");
                } else {
                    print_system_msg("✗", CLR_RED, msg[0] ? msg : body);
                    printf("\n");
                }
                break;
            }

            /* ── 在线列表 ── */
            case MSG_ONLINE_LIST_RESP:
                printf("\n");
                print_separator();
                printf(CLR_CYAN CLR_BOLD "  ● 在线成员" CLR_RESET);
                /* 提取 users 数组 */
                {
                    char *arr = strstr(body, "\"users\":[");
                    if (arr) {
                        arr += 9;
                        int count = 0;
                        printf("\n");
                        while (*arr && *arr != ']') {
                            /* 解析 {"name":"xxx","admin":0} */
                            char *name_start = strstr(arr, "\"name\":\"");
                            if (name_start && name_start < strchr(arr, '}') + 1) {
                                name_start += 8;  /* strlen("\"name\":\"") = 8 */
                                char name[64];
                                int i = 0;
                                while (*name_start && *name_start != '"' && i < 63)
                                    name[i++] = *name_start++;
                                name[i] = '\0';

                                int is_admin = 0;
                                char *admin_start = strstr(arr, "\"admin\":");
                                if (admin_start && admin_start < strchr(arr, '}') + 1) {
                                    admin_start += 8;  /* strlen("\"admin\":") = 8 */
                                    is_admin = atoi(admin_start);
                                }

                                if (name[0]) {
                                    printf("    " CLR_MAGENTA "●" CLR_RESET " %s", name);
                                    if (is_admin) {
                                        printf(" " CLR_YELLOW "★管理员" CLR_RESET);
                                    }
                                    printf("\n");
                                    count++;
                                }
                                /* 跳到下一个对象 */
                                char *brace = strchr(arr, '}');
                                if (brace) arr = brace + 1;
                                else arr++;
                            } else {
                                arr++;
                            }
                        }
                        printf(CLR_DIM "  ── 共 %d 人在线 ──" CLR_RESET "\n", count);
                    }
                }
                print_separator();
                break;

            /* ── 错误响应 ── */
            case MSG_ERROR_RESP: {
                char msg[256] = "";
                json_get_str(body, "msg", msg, sizeof(msg));
                print_system_msg("⚠", CLR_RED, msg[0] ? msg : body);
                printf("\n");
                break;
            }

            /* ── 心跳响应静默 ── */
            case MSG_HEARTBEAT_RESP:
                break;

            default:
                break;
            }

            fflush(stdout);
        }
    }

    return NULL;
}

/* ── 心跳线程 ── */
static void *heartbeat_thread(void *arg)
{
    (void)arg;
    while (g_running && g_logged_in) {
        sleep(10);
        if (!g_running || !g_logged_in) break;
        send_packet(MSG_HEARTBEAT_REQ, (const uint8_t *)"ping", 4);
    }
    return NULL;
}

/* ── 输入提示 ── */
static void print_prompt(void)
{
    if (g_logged_in) {
        printf(CLR_CYAN CLR_BOLD "[%s]" CLR_RESET, g_username);
        if (g_is_admin) {
            printf(CLR_YELLOW "★" CLR_RESET);
        }
        printf(CLR_GREEN " ➤ " CLR_RESET);
    } else {
        printf(CLR_DIM "guest" CLR_RESET " ➤ ");
    }
    fflush(stdout);
}

/* ── 主入口 ── */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("用法: %s <服务器IP> <端口>\n", argv[0]);
        printf("示例: %s 127.0.0.1 8888\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    /* 创建 socket + 连接 */
    g_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton"); close(g_sockfd); return 1;
    }
    if (connect(g_sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(g_sockfd); return 1;
    }

    /* ── 启动画面 ── */
    printf("\033[2J\033[H");  /* 清屏 */
    printf(CLR_CYAN CLR_BOLD);
    printf("  ╔══════════════════════════════════════╗\n");
    printf("  ║         IM 聊天室 客户端              ║\n");
    printf("  ╚══════════════════════════════════════╝\n");
    printf(CLR_RESET);
    printf(CLR_DIM "  ● 服务器: %s:%d" CLR_RESET "\n", server_ip, port);
    print_separator();
    printf(CLR_DIM "  命令:" CLR_RESET "\n");
    printf("    " CLR_CYAN "/register <用户名> <密码>" CLR_RESET
           "  注册新账号\n");
    printf("    " CLR_CYAN "/login <用户名> <密码>" CLR_RESET
           "     登录\n");
    printf("    " CLR_GREEN "/msg <消息>" CLR_RESET
           "        公屏消息 (直接输入文本即可)\n");
    printf("    " CLR_YELLOW "/to <用户名> <消息>" CLR_RESET
           "  私聊\n");
    printf("    " CLR_MAGENTA "/online" CLR_RESET
           "              查看在线成员\n");
    printf("    " CLR_RED "/kick <用户名>" CLR_RESET
           "       管理员踢人\n");
    printf("    " CLR_RED "/quit" CLR_RESET
           "                退出\n");
    print_separator();
    printf("\n");

    signal(SIGPIPE, SIG_IGN);

    /* 接收线程 */
    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, receiver_thread, NULL);

    /* 主循环 */
    char input[MAX_MSG_LEN];
    print_prompt();

    while (g_running && fgets(input, sizeof(input), stdin)) {
        size_t len = strlen(input);
        if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';
        if (input[0] == '\0') { print_prompt(); continue; }

        /* /quit */
        if (strcmp(input, "/quit") == 0) {
            g_running = 0;
            break;
        }

        /* /help */
        if (strcmp(input, "/help") == 0 || strcmp(input, "/h") == 0) {
            printf(CLR_DIM "\n  命令列表:\n" CLR_RESET);
            printf("    /register <用户名> <密码>  /login <用户名> <密码>\n");
            printf("    /msg <消息>  /to <用户> <消息>\n");
            printf("    /online  /kick <用户名>  /quit  /help\n\n");
            print_prompt();
            continue;
        }

        /* /register <用户名> <密码> */
        if (strncmp(input, "/register ", 10) == 0) {
            const char *rest = input + 10;
            while (*rest == ' ') rest++;
            char reg_user[MAX_USERNAME_LEN], reg_pass[MAX_USERNAME_LEN];
            memset(reg_user, 0, sizeof(reg_user));
            memset(reg_pass, 0, sizeof(reg_pass));

            const char *space = strchr(rest, ' ');
            if (!space) {
                printf(CLR_RED "  ✗ 用法: /register <用户名> <密码>" CLR_RESET "\n");
                print_prompt();
                continue;
            }
            size_t name_len = (size_t)(space - rest);
            if (name_len >= MAX_USERNAME_LEN) name_len = MAX_USERNAME_LEN - 1;
            memcpy(reg_user, rest, name_len);
            reg_user[name_len] = '\0';

            const char *pass_text = space + 1;
            while (*pass_text == ' ') pass_text++;
            size_t pass_len = strlen(pass_text);
            if (pass_len >= MAX_USERNAME_LEN) pass_len = MAX_USERNAME_LEN - 1;
            memcpy(reg_pass, pass_text, pass_len);
            reg_pass[pass_len] = '\0';

            if (strlen(reg_user) == 0 || strlen(reg_pass) == 0) {
                printf(CLR_RED "  ✗ 用法: /register <用户名> <密码>" CLR_RESET "\n");
                print_prompt();
                continue;
            }

            char reg_body[512];
            snprintf(reg_body, sizeof(reg_body),
                     "{\"username\":\"%s\",\"password\":\"%s\"}",
                     reg_user, reg_pass);
            send_packet(MSG_REGISTER_REQ,
                       (const uint8_t *)reg_body, strlen(reg_body));
            printf(CLR_DIM "       注册中..." CLR_RESET "\n");
            print_prompt();
            continue;
        }

        /* /login <用户名> <密码> */
        if (strncmp(input, "/login ", 7) == 0) {
            const char *rest = input + 7;
            while (*rest == ' ') rest++;
            char login_user[MAX_USERNAME_LEN], login_pass[MAX_USERNAME_LEN];
            memset(login_user, 0, sizeof(login_user));
            memset(login_pass, 0, sizeof(login_pass));

            const char *space = strchr(rest, ' ');
            if (space) {
                /* 有密码: /login user pass */
                size_t name_len = (size_t)(space - rest);
                if (name_len >= MAX_USERNAME_LEN) name_len = MAX_USERNAME_LEN - 1;
                memcpy(login_user, rest, name_len);
                login_user[name_len] = '\0';

                const char *pass_text = space + 1;
                while (*pass_text == ' ') pass_text++;
                size_t pass_len = strlen(pass_text);
                if (pass_len >= MAX_USERNAME_LEN) pass_len = MAX_USERNAME_LEN - 1;
                memcpy(login_pass, pass_text, pass_len);
                login_pass[pass_len] = '\0';
            } else {
                /* 无密码: 兼容旧格式 */
                size_t name_len = strlen(rest);
                if (name_len >= MAX_USERNAME_LEN) name_len = MAX_USERNAME_LEN - 1;
                memcpy(login_user, rest, name_len);
                login_user[name_len] = '\0';
            }

            if (strlen(login_user) == 0) {
                printf(CLR_RED "  ✗ 用法: /login <用户名> <密码>" CLR_RESET "\n");
                print_prompt();
                continue;
            }

            char login_body[512];
            snprintf(login_body, sizeof(login_body),
                     "{\"username\":\"%s\",\"password\":\"%s\"}",
                     login_user, login_pass);
            send_packet(MSG_LOGIN_REQ,
                       (const uint8_t *)login_body, strlen(login_body));
            /* 先乐观设置, 服务器响应会最终确认 */
            safe_strncpy(g_username, login_user, MAX_USERNAME_LEN);
            printf(CLR_DIM "       登录中..." CLR_RESET "\n");
            print_prompt();
            continue;
        }

        /* /kick <用户名> */
        if (strncmp(input, "/kick ", 6) == 0) {
            const char *target = input + 6;
            while (*target == ' ') target++;
            if (strlen(target) == 0) {
                printf(CLR_RED "  ✗ 用法: /kick <用户名>" CLR_RESET "\n");
                print_prompt();
                continue;
            }
            char kick_body[256];
            snprintf(kick_body, sizeof(kick_body),
                     "{\"username\":\"%s\"}", target);
            send_packet(MSG_KICK_REQ,
                       (const uint8_t *)kick_body, strlen(kick_body));
            print_prompt();
            continue;
        }

        /* /online */
        if (strcmp(input, "/online") == 0) {
            send_packet(MSG_ONLINE_LIST_REQ, NULL, 0);
            print_prompt();
            continue;
        }

        /* /to <用户名> <消息> */
        if (strncmp(input, "/to ", 4) == 0) {
            const char *rest = input + 4;
            while (*rest == ' ') rest++;
            char target[MAX_USERNAME_LEN], msg[MAX_MSG_LEN];
            const char *space = strchr(rest, ' ');
            if (!space) {
                printf(CLR_RED "  ✗ 用法: /to <用户名> <消息>" CLR_RESET "\n");
                print_prompt();
                continue;
            }
            size_t name_len = (size_t)(space - rest);
            if (name_len >= MAX_USERNAME_LEN) name_len = MAX_USERNAME_LEN - 1;
            memcpy(target, rest, name_len);
            target[name_len] = '\0';
            const char *msg_text = space + 1;
            while (*msg_text == ' ') msg_text++;
            snprintf(msg, sizeof(msg),
                     "{\"to\":\"%s\",\"msg\":\"%s\"}", target, msg_text);
            send_packet(MSG_PRIVATE_REQ, (const uint8_t *)msg, strlen(msg));
            printf(CLR_DIM "       → " CLR_BOLD CLR_YELLOW "%s" CLR_RESET
                   CLR_DIM ": " CLR_RESET "%s\n", target, msg_text);
            print_prompt();
            continue;
        }

        /* 公屏消息 (/msg 或直接输入) */
        const char *msg_text = input;
        if (strncmp(input, "/msg ", 5) == 0) {
            msg_text = input + 5;
            while (*msg_text == ' ') msg_text++;
        }
        if (g_logged_in && strlen(msg_text) > 0) {
            printf(CLR_DIM "       " CLR_BOLD CLR_BLUE "[你]" CLR_RESET
                   CLR_DIM " » " CLR_RESET "%s\n", msg_text);
        }
        send_packet(MSG_PUBLIC_MSG, (const uint8_t *)msg_text, strlen(msg_text));
        print_prompt();
    }

    /* 清理 */
    g_running = 0;
    shutdown(g_sockfd, SHUT_RDWR);
    close(g_sockfd);
    pthread_join(recv_tid, NULL);

    printf(CLR_DIM "\n  已断开连接。\n" CLR_RESET);
    return 0;
}
