/**
 * 高并发 IM 聊天室 - 命令行客户端
 * 用法: ./chat_client <server_ip> <server_port>
 */

#include "common.h"
#include "protocol.h"
#include <sys/ioctl.h>

#define RECV_BUF_SIZE (64 * 1024)

/* ── ANSI 颜色 ── */
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

/* ── 全局状态 ── */
static int            g_sockfd    = -1;
static int            g_running   = 1;
static char           g_username[MAX_USERNAME_LEN];
static int            g_logged_in = 0;
static int            g_is_admin  = 0;
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ══════════════════════════════════════════
 *  工具函数
 * ══════════════════════════════════════════ */

/* 获取当前时间, 格式 "HH:MM:SS" */
static void get_time_str(char *buf, size_t size)
{
    struct timeval tv;
    struct tm tm_info;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_info);
    strftime(buf, size, "%H:%M:%S", &tm_info);
}

/* 获取终端窗口宽度 (列数), 用于居中/对齐 */
static int get_term_width(void)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

/* 打印一条灰色分隔线 */
static void print_separator(void)
{
    printf(CLR_DIM "──────────────────────────────────────────────────" CLR_RESET "\n");
}

/* 打印带图标的系统提示消息 (如加入/离开) */
static void print_system_msg(const char *icon, const char *color, const char *msg)
{
    printf("\n" CLR_DIM "%s" CLR_RESET " %s%s %s" CLR_RESET "\n",
           "     ", color, icon, msg);
}

/* 打印输入提示符, 已登录青色, 未登录灰色 */
static void print_prompt(void)
{
    printf(g_logged_in ? CLR_CYAN "» " CLR_RESET : CLR_DIM "» " CLR_RESET);
    fflush(stdout);
}

/* ── JSON 字段提取 ── */

/* 从 JSON 字符串中提取指定 key 的字符串值 */
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

/* 提取 JSON 中的整数字段, 如 "is_admin":1 */
static int json_get_int(const char *json, const char *key)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    return p ? atoi(p + strlen(search)) : 0;
}

/* 将字面量 \n (反斜杠+n) 转换为真正的换行符, 原地修改 */
static void convert_newlines(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && *(r + 1) == 'n') {
            *w++ = '\n';
            r += 2;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* ── 解析 "用户名 密码" 参数 ── */
/* 从 "用户名 密码" 格式的参数中拆分出用户名和密码 */
static int parse_user_pass(const char *rest, char *user, char *pass, size_t sz)
{
    while (*rest == ' ') rest++;
    const char *space = strchr(rest, ' ');
    if (!space) return -1;

    /* 用户名 */
    size_t ulen = (size_t)(space - rest);
    if (ulen >= sz) ulen = sz - 1;
    memcpy(user, rest, ulen);
    user[ulen] = '\0';

    /* 密码 */
    const char *p = space + 1;
    while (*p == ' ') p++;
    if (*p == '\0') return -1;
    size_t plen = strlen(p);
    if (plen >= sz) plen = sz - 1;
    memcpy(pass, p, plen);
    pass[plen] = '\0';
    return 0;
}

/* ══════════════════════════════════════════
 *  消息气泡渲染 (支持自动换行 + 左右对齐)
 *
 *  align:  1=右对齐(自己)  0=左对齐(别人)
 *  color:  气泡边框颜色
 *  label:  底部标签, 如 "18:30:05 qhl"
 *  msg:    消息内容 (支持长文本自动换行)
 * ══════════════════════════════════════════ */

#define MAX_BUBBLE_LINES 64

static void draw_bubble(int align, const char *color,
                        const char *label, const char *msg)
{
    int term_w = get_term_width();
    int content_w = term_w - 8;          /* 最大内容可视宽度 */
    if (content_w < 8) content_w = 8;

    /* 将消息按可视宽度拆分为多行 (中文字符占2列) */
    const char *lines[MAX_BUBBLE_LINES];
    int line_lens[MAX_BUBBLE_LINES];
    int line_cols[MAX_BUBBLE_LINES];
    int line_count = 0;

    lines[0] = msg;
    int col = 0;
    for (const char *p = msg; *p && line_count < MAX_BUBBLE_LINES; ) {
        unsigned char ch = (unsigned char)*p;
        int cw;
        if (ch >= 0xE0 && ch <= 0xEF && p[1] && p[2]) {
            cw = 2; p += 3;  /* 中文字符: 3字节UTF-8, 占2列 */
        } else if (ch >= 0xC0 && p[1]) {
            cw = 1; p += 2;  /* 2字节UTF-8 */
        } else {
            cw = 1; p += 1;  /* ASCII */
        }

        /* 到达行宽或遇到换行符时, 切行 */
        if (col + cw > content_w || *(p - 1) == '\n') {
            line_lens[line_count] = (int)(p - lines[line_count]);
            line_cols[line_count] = col;
            line_count++;
            if (line_count >= MAX_BUBBLE_LINES) break;
            lines[line_count] = p;
            col = 0;
        } else {
            col += cw;
        }
    }
    /* 最后一行 */
    line_lens[line_count] = (int)(strlen(lines[line_count]));
    line_cols[line_count] = col;
    line_count++;

    /* 气泡宽度 = 最长行的可视宽度 + 2 (左右空格) */
    int max_cols = 0;
    for (int i = 0; i < line_count; i++)
        if (line_cols[i] > max_cols) max_cols = line_cols[i];
    int box_w = max_cols + 2;
    int lbl_w = (int)strlen(label);
    if (lbl_w > box_w) box_w = lbl_w;
    if (box_w < 10) box_w = 10;

    /* 左对齐固定 2 列缩进, 右对齐动态计算 */
    int pad = align ? (term_w - box_w - 4) : 2;
    if (pad < 2) pad = 2;

    printf("\n");

    /* ╭───╮ */
    printf("%*s%s╭", pad, "", color);
    for (int i = 0; i < box_w - 2; i++) printf("─");
    printf("╮" CLR_RESET "\n");

    /* │ 每行消息 │ (打印时去掉行尾的\n) */
    for (int i = 0; i < line_count; i++) {
        printf("%*s%s│" CLR_RESET " ", pad, "", color);
        int plen = line_lens[i];
        if (plen > 0 && lines[i][plen - 1] == '\n') plen--;
        printf("%.*s", plen, lines[i]);
        int fill = (box_w - 2) - line_cols[i] - 1;
        for (int j = 0; j < (fill > 0 ? fill : 0); j++) printf(" ");
        printf("%s│" CLR_RESET "\n", color);
    }

    /* ╰───╯ */
    printf("%*s%s╰", pad, "", color);
    for (int i = 0; i < box_w - 2; i++) printf("─");
    printf("╯" CLR_RESET "\n");

    /* 底部标签 */
    printf("%*s" CLR_DIM "%s" CLR_RESET "\n", pad, "", label);
}

/* 快捷: 构建底部标签并绘制气泡 */
/* 绘制自己发出的公屏消息气泡 (右对齐, 青色) */
static void show_self_public(const char *msg, const char *time_str)
{
    char label[128];
    snprintf(label, sizeof(label), "%s %s%s", time_str,
             g_is_admin ? "★" : "", g_username);
    draw_bubble(1, CLR_CYAN, label, msg);
}

/* 绘制自己发出的私聊气泡 (右对齐, 黄色) */
static void show_self_private(const char *to, const char *msg, const char *time_str)
{
    char label[128];
    snprintf(label, sizeof(label), "%s 你 → %s", time_str, to);
    draw_bubble(1, CLR_YELLOW, label, msg);
}

/* 绘制别人发的公屏消息气泡 (左对齐, 绿色) */
static void show_other_public(const char *from, const char *msg,
                              const char *time_str, int is_admin)
{
    char label[128];
    snprintf(label, sizeof(label), "%s%s %s",
             is_admin ? "★" : "", from, time_str);
    draw_bubble(0, CLR_GREEN, label, msg);
}

/* 绘制别人发给我的私聊气泡 (左对齐, 黄色) */
static void show_other_private(const char *from, const char *msg,
                               const char *time_str, int is_admin)
{
    char label[128];
    snprintf(label, sizeof(label), "%s%s → 你  %s",
             is_admin ? "★" : "", from, time_str);
    draw_bubble(0, CLR_YELLOW, label, msg);
}

/* ══════════════════════════════════════════
 *  协议包收发
 * ══════════════════════════════════════════ */

/* 组装并发送协议包 (包头+包体), 加锁保证线程安全 */
static int send_packet(uint8_t type, const uint8_t *body, uint16_t body_len)
{
    uint8_t packet[PROTO_HEADER_LEN + PROTO_MAX_BODY_LEN];
    proto_header_t hdr;
    proto_build_header(&hdr, type, body_len);
    proto_serialize_header(packet, &hdr);

    if (body && body_len > 0)
        memcpy(packet + PROTO_HEADER_LEN, body, body_len);

    size_t total = PROTO_HEADER_LEN + body_len;
    pthread_mutex_lock(&g_send_mutex);
    ssize_t n = send(g_sockfd, packet, total, MSG_NOSIGNAL);
    pthread_mutex_unlock(&g_send_mutex);
    return (n == (ssize_t)total) ? 0 : -1;
}

/* ══════════════════════════════════════════
 *  接收线程 - 解析协议包并渲染消息
 * ══════════════════════════════════════════ */

static void *receiver_thread(void *arg);
static void *heartbeat_thread(void *arg);

/* 处理公屏广播 (含 join/leave 事件) */
static void handle_public_broadcast(const char *body, const char *time_str)
{
    char from[64] = "", msg[4096] = "", type[32] = "";
    json_get_str(body, "from", from, sizeof(from));
    json_get_str(body, "msg",  msg,  sizeof(msg));
    json_get_str(body, "type", type, sizeof(type));
    int is_admin = json_get_int(body, "is_admin");
    convert_newlines(msg);  /* 将 \n 转换为真正换行 */

    if (strcmp(type, "join") == 0) {
        print_system_msg("✦", CLR_GREEN, "");
        printf(CLR_GREEN CLR_BOLD "  %s" CLR_RESET, from);
        if (is_admin) printf(CLR_YELLOW "★" CLR_RESET);
        printf(CLR_DIM " 加入聊天室" CLR_RESET);
        /* 在线人数 */
        int online = json_get_int(body, "online");
        if (online > 0) printf(CLR_DIM "  当前在线: %d 人" CLR_RESET, online);
        printf("\n");
    } else if (strcmp(type, "leave") == 0) {
        print_system_msg("✧", CLR_RED, "");
        printf(CLR_RED CLR_BOLD "  %s" CLR_RESET CLR_DIM " 离开聊天室" CLR_RESET, from);
        printf("\n");
    } else {
        /* 普通公屏消息: 根据是否自己发的决定对齐方向 */
        if (strcmp(from, g_username) == 0)
            show_self_public(msg, time_str);
        else
            show_other_public(from, msg, time_str, is_admin);
    }
}

/* 处理私聊消息 */
static void handle_private_resp(const char *body, const char *time_str)
{
    char from[64] = "", msg[4096] = "", status[32] = "";
    json_get_str(body, "from", from, sizeof(from));
    json_get_str(body, "msg",  msg,  sizeof(msg));
    json_get_str(body, "status", status, sizeof(status));
    int is_admin = json_get_int(body, "is_admin");
    convert_newlines(msg);  /* 将 \n 转换为真正换行 */

    if (from[0]) {
        /* 别人发给我的私聊 */
        show_other_private(from, msg, time_str, is_admin);
    } else if (strcmp(status, "ok") == 0) {
        /* 发送成功确认 */
        char to[64] = "";
        json_get_str(body, "to", to, sizeof(to));
        printf("\n" CLR_DIM "  ✓ 消息已送达 %s" CLR_RESET "\n", to);
    } else {
        print_system_msg("✗", CLR_RED, body);
        printf("\n");
    }
}

/* 处理登录响应 */
static void handle_login_resp(const char *body)
{
    char status[32] = "", username[64] = "", err[256] = "";
    json_get_str(body, "status",   status,   sizeof(status));
    json_get_str(body, "username", username, sizeof(username));
    json_get_str(body, "msg",      err,      sizeof(err));
    int is_admin = json_get_int(body, "is_admin");

    if (strcmp(status, "ok") == 0) {
        g_logged_in = 1;
        g_is_admin  = is_admin;
        safe_strncpy(g_username, username, MAX_USERNAME_LEN);
        printf("\n" CLR_GREEN " ✓ 登录成功!" CLR_RESET
               "  欢迎 " CLR_BOLD CLR_CYAN "%s" CLR_RESET, username);
        if (is_admin) printf(" " CLR_YELLOW "[管理员]" CLR_RESET);
        printf("\n");
        /* 登录成功后启动心跳线程 */
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
}

/* 处理简单状态响应 (注册/踢人) */
static void handle_status_resp(const char *body, const char *ok_msg, const char *icon)
{
    char status[32] = "", msg[256] = "";
    json_get_str(body, "status", status, sizeof(status));
    json_get_str(body, "msg",    msg,    sizeof(msg));

    if (strcmp(status, "ok") == 0) {
        printf("\n" CLR_GREEN " %s %s" CLR_RESET "\n", icon,
               msg[0] ? msg : ok_msg);
    } else {
        printf("\n" CLR_RED " %s %s" CLR_RESET "\n", icon,
               msg[0] ? msg : "未知错误");
    }
}

/* 处理在线列表响应 */
static void handle_online_list(const char *body)
{
    printf("\n");
    print_separator();
    printf(CLR_CYAN CLR_BOLD "  ● 在线成员" CLR_RESET "\n");

    char *arr = strstr(body, "\"users\":[");
    if (arr) {
        arr += 9;
        int count = 0;
        while (*arr && *arr != ']') {
            char *name_start = strstr(arr, "\"name\":\"");
            char *brace = strchr(arr, '}');
            if (name_start && brace && name_start < brace) {
                name_start += 8;
                char name[64];
                int i = 0;
                while (*name_start && *name_start != '"' && i < 63)
                    name[i++] = *name_start++;
                name[i] = '\0';

                int is_admin = 0;
                char *adm = strstr(arr, "\"admin\":");
                if (adm && adm < brace) is_admin = atoi(adm + 8);

                if (name[0]) {
                    printf("    " CLR_MAGENTA "●" CLR_RESET " %s", name);
                    if (is_admin) printf(" " CLR_YELLOW "★管理员" CLR_RESET);
                    printf("\n");
                    count++;
                }
                arr = brace + 1;
            } else {
                arr++;
            }
        }
        printf(CLR_DIM "  ── 共 %d 人在线 ──" CLR_RESET "\n", count);
    }
    print_separator();
}

/* 接收线程主循环 */
static void *receiver_thread(void *arg)
{
    (void)arg;
    uint8_t buf[RECV_BUF_SIZE];
    size_t  buf_len = 0;
    char    time_str[16];

    while (g_running) {
        /* select 超时 100ms, 便于检查 g_running */
        fd_set rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        FD_ZERO(&rfds);
        FD_SET(g_sockfd, &rfds);

        int ret = select(g_sockfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) break;
        if (ret == 0) continue;

        ssize_t n = recv(g_sockfd, buf + buf_len, sizeof(buf) - buf_len, 0);
        if (n <= 0) {
            printf("\n" CLR_RED " ══ 连接已断开 ══" CLR_RESET "\n");
            g_running = 0;
            break;
        }
        buf_len += n;

        /* 拆包循环: 从 buf 中逐个提取完整协议包 */
        while (buf_len >= PROTO_HEADER_LEN) {
            proto_header_t hdr;
            proto_deserialize_header(buf, &hdr);

            if (!proto_validate_header(&hdr)) {
                memmove(buf, buf + 1, buf_len - 1);
                buf_len--;
                continue;
            }

            size_t total_len = PROTO_HEADER_LEN + hdr.body_len;
            if (buf_len < total_len) break;  /* 半包, 等待更多数据 */

            char body[PROTO_MAX_BODY_LEN + 1];
            if (hdr.body_len > 0)
                memcpy(body, buf + PROTO_HEADER_LEN, hdr.body_len);
            body[hdr.body_len] = '\0';

            memmove(buf, buf + total_len, buf_len - total_len);
            buf_len -= total_len;
            get_time_str(time_str, sizeof(time_str));

            /* 按消息类型分发处理 */
            switch (hdr.type) {
            case MSG_PUBLIC_BROADCAST: handle_public_broadcast(body, time_str); break;
            case MSG_PRIVATE_RESP:     handle_private_resp(body, time_str);     break;
            case MSG_LOGIN_RESP:       handle_login_resp(body);                 break;
            case MSG_REGISTER_RESP:    handle_status_resp(body, "注册成功! 请使用 /login 登录", "✓"); break;
            case MSG_KICK_RESP:        handle_status_resp(body, "操作成功", "✓"); break;
            case MSG_ONLINE_LIST_RESP: handle_online_list(body);                break;
            case MSG_ERROR_RESP: {
                char msg[256] = "";
                json_get_str(body, "msg", msg, sizeof(msg));
                print_system_msg("⚠", CLR_RED, msg[0] ? msg : body);
                printf("\n");
                break;
            }
            case MSG_HEARTBEAT_RESP: break;  /* 静默 */
            default: break;
            }
            fflush(stdout);
        }
    }
    return NULL;
}

/* 心跳线程: 每 10 秒发送心跳保活 */
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

/* ══════════════════════════════════════════
 *  启动画面
 * ══════════════════════════════════════════ */

/* 显示居中欢迎画面: 标题 + 命令说明 */
static void show_welcome(const char *ip, int port)
{
    printf("\033[2J\033[H");
    int pad = (get_term_width() - 40) / 2;
    if (pad < 0) pad = 0;
    printf("\n\n");
    printf("%*s" CLR_CYAN CLR_BOLD "════════════════════════════════" CLR_RESET "\n", pad, "");
    printf("%*s" CLR_CYAN CLR_BOLD "    💬  IM 聊天室  客户端       " CLR_RESET "\n", pad, "");
    printf("%*s" CLR_CYAN CLR_BOLD "════════════════════════════════" CLR_RESET "\n", pad, "");
    printf("\n");
    printf("%*s" CLR_DIM "%s:%d" CLR_RESET "\n\n", pad + 4, "", ip, port);
    printf("%*s" CLR_CYAN "/register" CLR_RESET " <用户> <密码>    注册\n", pad, "");
    printf("%*s" CLR_CYAN "/login" CLR_RESET "    <用户> <密码>    登录\n", pad, "");
    printf("\n");
    printf("%*s" CLR_GREEN "直接输入" CLR_RESET "                  公屏消息\n", pad, "");
    printf("%*s" CLR_YELLOW "/to" CLR_RESET "      <用户> <消息>    私聊\n", pad, "");
    printf("%*s" CLR_MAGENTA "/online" CLR_RESET "                   在线列表\n", pad, "");
    printf("\n");
    printf("%*s" CLR_RED "/kick" CLR_RESET "      <用户>          踢人\n", pad, "");
    printf("%*s" CLR_RED "/quit" CLR_RESET "                       退出\n", pad, "");
    printf("\n\n");
}

/* ══════════════════════════════════════════
 *  主入口 - 连接服务器 + 命令循环
 * ══════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("用法: %s <服务器IP> <端口>\n", argv[0]);
        printf("示例: %s 127.0.0.1 8888\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    /* 建立 TCP 连接 */
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

    signal(SIGPIPE, SIG_IGN);
    show_welcome(server_ip, port);

    /* 启动接收线程 */
    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, receiver_thread, NULL);

    /* ── 主循环: 读取用户输入并分发命令 ── */
    char input[MAX_MSG_LEN];
    print_prompt();

    while (g_running && fgets(input, sizeof(input), stdin)) {
        size_t len = strlen(input);
        if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';
        if (input[0] == '\0') { print_prompt(); continue; }

        /* /quit - 退出 */
        if (strcmp(input, "/quit") == 0) {
            g_running = 0;
            break;
        }

        /* /help - 帮助 */
        if (strcmp(input, "/help") == 0 || strcmp(input, "/h") == 0) {
            printf("\n");
            printf(CLR_DIM "  账号" CLR_RESET "  /register /login\n");
            printf(CLR_DIM "  聊天" CLR_RESET "  直接输入文字 /to /online\n");
            printf(CLR_DIM "  管理" CLR_RESET "  /kick /quit\n\n");
            print_prompt();
            continue;
        }

        /* /register <用户> <密码> */
        if (strncmp(input, "/register ", 10) == 0) {
            char user[MAX_USERNAME_LEN] = "", pass[MAX_USERNAME_LEN] = "";
            if (parse_user_pass(input + 10, user, pass, MAX_USERNAME_LEN) < 0) {
                printf(CLR_RED "  ✗ 用法: /register <用户名> <密码>" CLR_RESET "\n");
            } else {
                char body[512];
                snprintf(body, sizeof(body),
                         "{\"username\":\"%s\",\"password\":\"%s\"}", user, pass);
                send_packet(MSG_REGISTER_REQ, (const uint8_t *)body, strlen(body));
                printf(CLR_DIM "       注册中..." CLR_RESET "\n");
            }
            print_prompt();
            continue;
        }

        /* /login <用户> <密码> */
        if (strncmp(input, "/login ", 7) == 0) {
            char user[MAX_USERNAME_LEN] = "", pass[MAX_USERNAME_LEN] = "";
            const char *rest = input + 7;
            while (*rest == ' ') rest++;

            /* 支持无密码格式: /login user */
            if (strchr(rest, ' ')) {
                if (parse_user_pass(rest, user, pass, MAX_USERNAME_LEN) < 0) {
                    printf(CLR_RED "  ✗ 用法: /login <用户名> <密码>" CLR_RESET "\n");
                    print_prompt();
                    continue;
                }
            } else {
                safe_strncpy(user, rest, MAX_USERNAME_LEN);
            }

            if (user[0] == '\0') {
                printf(CLR_RED "  ✗ 用法: /login <用户名> <密码>" CLR_RESET "\n");
            } else {
                char body[512];
                snprintf(body, sizeof(body),
                         "{\"username\":\"%s\",\"password\":\"%s\"}", user, pass);
                send_packet(MSG_LOGIN_REQ, (const uint8_t *)body, strlen(body));
                safe_strncpy(g_username, user, MAX_USERNAME_LEN);
                printf(CLR_DIM "       登录中..." CLR_RESET "\n");
            }
            print_prompt();
            continue;
        }

        /* /kick <用户名> */
        if (strncmp(input, "/kick ", 6) == 0) {
            const char *target = input + 6;
            while (*target == ' ') target++;
            if (target[0] == '\0') {
                printf(CLR_RED "  ✗ 用法: /kick <用户名>" CLR_RESET "\n");
            } else {
                char body[256];
                snprintf(body, sizeof(body), "{\"username\":\"%s\"}", target);
                send_packet(MSG_KICK_REQ, (const uint8_t *)body, strlen(body));
            }
            print_prompt();
            continue;
        }

        /* /online - 查询在线列表 */
        if (strcmp(input, "/online") == 0) {
            send_packet(MSG_ONLINE_LIST_REQ, NULL, 0);
            print_prompt();
            continue;
        }

        /* /to <用户> <消息> - 私聊 */
        if (strncmp(input, "/to ", 4) == 0) {
            const char *rest = input + 4;
            while (*rest == ' ') rest++;
            const char *space = strchr(rest, ' ');
            if (!space) {
                printf(CLR_RED "  ✗ 用法: /to <用户名> <消息>" CLR_RESET "\n");
                print_prompt();
                continue;
            }
            char target[MAX_USERNAME_LEN];
            size_t nlen = (size_t)(space - rest);
            if (nlen >= MAX_USERNAME_LEN) nlen = MAX_USERNAME_LEN - 1;
            memcpy(target, rest, nlen);
            target[nlen] = '\0';

            const char *msg_text = space + 1;
            while (*msg_text == ' ') msg_text++;

            /* 复制并转换换行符, 用于本地显示 */
            char msg_copy[MAX_MSG_LEN];
            safe_strncpy(msg_copy, msg_text, MAX_MSG_LEN);
            convert_newlines(msg_copy);

            char body[MAX_MSG_LEN];
            snprintf(body, sizeof(body),
                     "{\"to\":\"%s\",\"msg\":\"%s\"}", target, msg_text);
            send_packet(MSG_PRIVATE_REQ, (const uint8_t *)body, strlen(body));

            /* 本地立即显示右对齐气泡 */
            char t_str[16];
            get_time_str(t_str, sizeof(t_str));
            show_self_private(target, msg_copy, t_str);
            print_prompt();
            continue;
        }

        /* 公屏消息: 直接输入文字或 /msg */
        const char *msg_text = input;
        if (strncmp(input, "/msg ", 5) == 0) {
            msg_text = input + 5;
            while (*msg_text == ' ') msg_text++;
        }
        if (g_logged_in && msg_text[0])
            send_packet(MSG_PUBLIC_MSG, (const uint8_t *)msg_text, strlen(msg_text));
        print_prompt();
    }

    /* 清理资源 */
    g_running = 0;
    shutdown(g_sockfd, SHUT_RDWR);
    close(g_sockfd);
    pthread_join(recv_tid, NULL);

    printf(CLR_DIM "\n  已断开连接。\n" CLR_RESET);
    return 0;
}
