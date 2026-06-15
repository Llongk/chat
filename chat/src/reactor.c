#include "reactor.h"
#include "client.h"
#include "chat_service.h"
#include "logger.h"
#include <sys/eventfd.h>

/* ============ 子 Reactor 实现 ============ */

/* 处理客户端可读事件 (ET 模式, 循环读直到 EAGAIN) */
static void sub_handle_read(client_t *cli)
{
    uint8_t buf[4096];

    while (1) {
        ssize_t n = recv(cli->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            /* 写入接收环形缓冲区 */
            size_t written = ringbuf_write(cli->inbuf, buf, n);
            if (written < (size_t)n) {
                LOG_WARN("fd=%d inbuf full, dropped %zd bytes",
                         cli->fd, n - written);
            }
        } else if (n == 0) {
            /* 对端关闭连接 */
            LOG_INFO("fd=%d connection closed by peer", cli->fd);
            cli->alive = 0;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 数据已读完 */
                break;
            }
            /* 其他错误 */
            LOG_WARN("fd=%d recv error: %s", cli->fd, strerror(errno));
            cli->alive = 0;
            break;
        }
    }
}

/* 处理客户端可写事件: 从 outbuf 取出数据发送 (ET 模式) */
static void sub_handle_write(client_t *cli, int epoll_fd)
{
    uint8_t buf[4096];

    pthread_mutex_lock(&cli->outbuf_mutex);

    while (ringbuf_readable(cli->outbuf) > 0) {
        size_t peek_len = ringbuf_peek(cli->outbuf, buf, sizeof(buf));

        ssize_t n = send(cli->fd, buf, peek_len, MSG_NOSIGNAL);
        if (n > 0) {
            ringbuf_skip(cli->outbuf, n);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 发送缓冲区满, 需要等待下次 EPOLLOUT */
                break;
            }
            /* 发送错误 */
            LOG_WARN("fd=%d send error: %s", cli->fd, strerror(errno));
            cli->alive = 0;
            break;
        }
    }

    int has_pending = (ringbuf_readable(cli->outbuf) > 0) ? 1 : 0;

    pthread_mutex_unlock(&cli->outbuf_mutex);

    /* 控制 EPOLLOUT: 有数据待发时保持监听, 发完时移除 */
    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.ptr = cli;
    if (has_pending) {
        ev.events |= EPOLLOUT;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, cli->fd, &ev);
}

/* 尝试从环形缓冲区组装完整协议包, 提交到业务线程池 */
static void sub_try_assemble_packet(client_t *cli, threadpool_t *biz_pool)
{
    proto_ctx_t *ctx = &cli->proto_ctx;

    while (1) {
        if (ctx->state == PARSE_STATE_HEADER) {
            /* 需要至少 PROTO_HEADER_LEN 字节 */
            if (ringbuf_readable(cli->inbuf) < PROTO_HEADER_LEN) {
                break;
            }

            /* 窥探包头 */
            uint8_t hdr_buf[PROTO_HEADER_LEN];
            ringbuf_peek(cli->inbuf, hdr_buf, PROTO_HEADER_LEN);

            proto_header_t hdr;
            proto_deserialize_header(hdr_buf, &hdr);

            if (!proto_validate_header(&hdr)) {
                LOG_WARN("fd=%d invalid magic number, closing", cli->fd);
                cli->alive = 0;
                return;
            }

            ctx->header    = hdr;
            ctx->body_read = 0;

            /* 跳过包头 */
            ringbuf_skip(cli->inbuf, PROTO_HEADER_LEN);

            if (hdr.body_len == 0) {
                /* 无包体, 直接提交 */
                chat_task_t *task = (chat_task_t *)malloc(sizeof(chat_task_t));
                if (task) {
                    task->fd       = cli->fd;
                    task->msg_type = hdr.type;
                    task->body     = NULL;
                    task->body_len = 0;
                    threadpool_add_task(biz_pool,
                                        chat_service_process, task);
                }
                ctx->state = PARSE_STATE_HEADER;
            } else {
                /* body_len 是 uint16_t, 最大 65535, 在 PROTO_MAX_BODY_LEN 范围内 */
                ctx->state = PARSE_STATE_BODY;
            }
        }

        if (ctx->state == PARSE_STATE_BODY) {
            uint16_t need = ctx->header.body_len - ctx->body_read;
            size_t available = ringbuf_readable(cli->inbuf);

            if (available < need) {
                /* 包体数据不完整, 等待更多数据 */
                break;
            }

            /* 读取完整包体 */
            uint8_t *body = (uint8_t *)malloc(need);
            if (!body) {
                cli->alive = 0;
                return;
            }

            ringbuf_read(cli->inbuf, body, need);

            /* 提交到业务线程池 */
            chat_task_t *task = (chat_task_t *)malloc(sizeof(chat_task_t));
            if (task) {
                task->fd       = cli->fd;
                task->msg_type = ctx->header.type;
                task->body     = body;
                task->body_len = need;
                threadpool_add_task(biz_pool, chat_service_process, task);
            } else {
                free(body);
            }

            /* 重置状态, 等待下一个包头 */
            ctx->state     = PARSE_STATE_HEADER;
            ctx->body_read = 0;
        }
    }
}

/* 尝试发送客户端缓冲区中待发送的数据 */
static void sub_flush_pending_sends(sub_reactor_t *sub)
{
    for (int fd = 0; fd < MAX_CLIENTS; fd++) {
        client_t *cli = client_find_by_fd(fd);
        if (!cli || !cli->alive) continue;
        if (cli->sub_reactor_idx != sub->idx) continue;

        /* 检查是否有待发送数据 */
        int has_pending;
        pthread_mutex_lock(&cli->outbuf_mutex);
        has_pending = (ringbuf_readable(cli->outbuf) > 0) ? 1 : 0;
        pthread_mutex_unlock(&cli->outbuf_mutex);

        if (has_pending) {
            sub_handle_write(cli, sub->epoll_fd);
        }
    }
}

/* 清理不活跃/已断开的客户端 (仅清理属于本子Reactor的客户端) */
static void sub_cleanup_clients(int epoll_fd, int sub_idx)
{
    for (int fd = 0; fd < MAX_CLIENTS; fd++) {
        client_t *cli = client_find_by_fd(fd);
        if (!cli) continue;
        if (cli->sub_reactor_idx != sub_idx) continue;  /* 只清理自己的客户端 */
        if (!cli->alive) {
            /* 通知其他用户有人下线 */
            if (cli->logged_in && cli->username[0]) {
                char leave_msg[256];
                snprintf(leave_msg, sizeof(leave_msg),
                         "{\"type\":\"leave\",\"username\":\"%s\",\"online\":%d}",
                         cli->username, client_online_count() - 1);

                for (int i = 0; i < MAX_CLIENTS; i++) {
                    client_t *target = client_find_by_fd(i);
                    if (target && target->alive && target->logged_in &&
                        target->fd != cli->fd) {
                        chat_send_message(target->fd, MSG_PUBLIC_BROADCAST,
                                          (const uint8_t *)leave_msg,
                                          strlen(leave_msg));
                    }
                }
            }

            /* 从 epoll 移除 */
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cli->fd, NULL);
            /* 销毁客户端 */
            client_destroy(cli);
        }
    }
}

/* 子 Reactor 线程主循环 */
static void *sub_reactor_thread(void *arg)
{
    sub_reactor_t *sub = (sub_reactor_t *)arg;
    struct epoll_event events[256];

    LOG_INFO("Sub-reactor[%d] started, epoll_fd=%d", sub->idx, sub->epoll_fd);

    while (sub->running) {
        int nfds = epoll_wait(sub->epoll_fd, events, 256, EPOLL_TIMEOUT_MS);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Sub-reactor[%d] epoll_wait error: %s",
                      sub->idx, strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            /* eventfd 通知: 业务线程有数据待发送 */
            if (events[i].data.fd == sub->event_fd) {
                uint64_t val;
                ssize_t ret __attribute__((unused));
                ret = read(sub->event_fd, &val, sizeof(val));
                sub_flush_pending_sends(sub);
                continue;
            }

            client_t *cli = (client_t *)events[i].data.ptr;
            if (!cli) continue;

            uint32_t ev = events[i].events;

            /* 对端关闭或出错 */
            if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                LOG_INFO("fd=%d error/hangup event", cli->fd);
                cli->alive = 0;
                continue;
            }

            /* 可读事件 */
            if (ev & EPOLLIN) {
                sub_handle_read(cli);
                if (cli->alive) {
                    sub_try_assemble_packet(cli, sub->biz_pool);
                }
            }

            /* 可写事件 */
            if (ev & EPOLLOUT && cli->alive) {
                sub_handle_write(cli, sub->epoll_fd);
            }
        }

        /* 定期清理断开连接 */
        sub_cleanup_clients(sub->epoll_fd, sub->idx);
    }

    LOG_INFO("Sub-reactor[%d] stopped", sub->idx);
    return NULL;
}

/* ============ 主 Reactor 实现 ============ */

/* 主 Reactor 线程 */
static void *main_reactor_thread(void *arg)
{
    reactor_t *r = (reactor_t *)arg;
    struct epoll_event events[64];

    LOG_INFO("Main reactor started, listening on port (fd=%d)", r->listen_fd);

    while (r->running) {
        int nfds = epoll_wait(r->main_epoll_fd, events, 64, EPOLL_TIMEOUT_MS);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Main reactor epoll_wait error: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd != r->listen_fd) continue;

            /* ET 模式: 循环 accept 直到 EAGAIN */
            while (1) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = accept(r->listen_fd,
                                       (struct sockaddr *)&client_addr,
                                       &addr_len);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;  /* 已 accept 完所有连接 */
                    }
                    if (errno == EINTR) {
                        continue;  /* 被信号中断 */
                    }
                    LOG_WARN("accept error: %s", strerror(errno));
                    break;
                }

                /* 设置非阻塞 */
                if (set_nonblock(client_fd) < 0) {
                    LOG_WARN("set_nonblock failed for fd=%d", client_fd);
                    close(client_fd);
                    continue;
                }

                /* 创建客户端 */
                client_t *cli = client_create(client_fd, &client_addr);
                if (!cli) {
                    close(client_fd);
                    continue;
                }

                /* 轮询分发到子 Reactor */
                int sub_idx = r->rr_index % r->sub_count;
                r->rr_index++;

                cli->sub_reactor_idx = sub_idx;
                cli->epoll_fd        = r->subs[sub_idx].epoll_fd;

                /* 注册到子 Reactor 的 epoll */
                struct epoll_event ev;
                ev.events   = EPOLLIN | EPOLLET | EPOLLRDHUP;
                ev.data.ptr = cli;
                if (epoll_ctl(r->subs[sub_idx].epoll_fd, EPOLL_CTL_ADD,
                              client_fd, &ev) < 0) {
                    LOG_ERROR("Failed to add fd=%d to sub-reactor[%d]: %s",
                              client_fd, sub_idx, strerror(errno));
                    client_destroy(cli);
                }

                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr,
                          ip_str, sizeof(ip_str));
                LOG_INFO("Accepted connection: fd=%d, ip=%s:%d -> sub[%d]",
                         client_fd, ip_str, ntohs(client_addr.sin_port),
                         sub_idx);
            }
        }
    }

    LOG_INFO("Main reactor stopped");
    return NULL;
}

/* ============ Reactor API ============ */

int reactor_init(reactor_t *r, int port, int sub_count,
                 threadpool_t *biz_pool)
{
    if (!r || sub_count <= 0) return -1;

    memset(r, 0, sizeof(reactor_t));
    r->sub_count = sub_count;

    /* 创建监听 socket */
    r->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (r->listen_fd < 0) {
        LOG_ERROR("Failed to create listen socket: %s", strerror(errno));
        return ERR_SOCKET_CREATE;
    }

    /* 设置 SO_REUSEADDR */
    int opt = 1;
    setsockopt(r->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 设置非阻塞 */
    if (set_nonblock(r->listen_fd) < 0) {
        LOG_ERROR("Failed to set listen socket nonblock");
        close(r->listen_fd);
        return -1;
    }

    /* 绑定地址 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(r->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind port %d: %s", port, strerror(errno));
        close(r->listen_fd);
        return ERR_SOCKET_BIND;
    }

    /* 开始监听 */
    if (listen(r->listen_fd, LISTEN_BACKLOG) < 0) {
        LOG_ERROR("Failed to listen: %s", strerror(errno));
        close(r->listen_fd);
        return ERR_SOCKET_LISTEN;
    }

    LOG_INFO("Listening on port %d", port);

    /* 创建主 Reactor epoll */
    r->main_epoll_fd = epoll_create1(0);
    if (r->main_epoll_fd < 0) {
        LOG_ERROR("Failed to create main epoll: %s", strerror(errno));
        close(r->listen_fd);
        return ERR_EPOLL_CREATE;
    }

    /* 将 listen fd 注册到主 epoll (ET 模式) */
    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLET;
    ev.data.fd  = r->listen_fd;
    if (epoll_ctl(r->main_epoll_fd, EPOLL_CTL_ADD, r->listen_fd, &ev) < 0) {
        LOG_ERROR("Failed to add listen fd to main epoll");
        close(r->main_epoll_fd);
        close(r->listen_fd);
        return -1;
    }

    /* 创建子 Reactor 数组 */
    r->subs = (sub_reactor_t *)malloc(sizeof(sub_reactor_t) * sub_count);
    if (!r->subs) {
        close(r->main_epoll_fd);
        close(r->listen_fd);
        return ERR_MEMORY;
    }

    for (int i = 0; i < sub_count; i++) {
        sub_reactor_t *sub = &r->subs[i];
        memset(sub, 0, sizeof(sub_reactor_t));

        sub->idx      = i;
        sub->biz_pool = biz_pool;
        sub->running  = 0;

        /* 创建子 epoll */
        sub->epoll_fd = epoll_create1(0);
        if (sub->epoll_fd < 0) {
            LOG_ERROR("Failed to create epoll for sub-reactor[%d]", i);
            /* 清理已创建的 */
            for (int j = 0; j < i; j++) {
                close(r->subs[j].epoll_fd);
                close(r->subs[j].event_fd);
            }
            free(r->subs);
            close(r->main_epoll_fd);
            close(r->listen_fd);
            return ERR_EPOLL_CREATE;
        }

        /* 创建 eventfd 用于跨线程通知 */
        sub->event_fd = eventfd(0, EFD_NONBLOCK);
        if (sub->event_fd < 0) {
            LOG_ERROR("Failed to create eventfd for sub-reactor[%d]", i);
            close(sub->epoll_fd);
            for (int j = 0; j < i; j++) {
                close(r->subs[j].epoll_fd);
                close(r->subs[j].event_fd);
            }
            free(r->subs);
            close(r->main_epoll_fd);
            close(r->listen_fd);
            return -1;
        }

        /* 将 eventfd 注册到子 epoll */
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = sub->event_fd;
        if (epoll_ctl(sub->epoll_fd, EPOLL_CTL_ADD, sub->event_fd, &ev) < 0) {
            LOG_ERROR("Failed to add eventfd to sub-reactor[%d] epoll", i);
            close(sub->epoll_fd);
            close(sub->event_fd);
            for (int j = 0; j < i; j++) {
                close(r->subs[j].epoll_fd);
                close(r->subs[j].event_fd);
            }
            free(r->subs);
            close(r->main_epoll_fd);
            close(r->listen_fd);
            return -1;
        }
    }

    reactor_setup_eventfds(r);

    LOG_INFO("Reactor initialized: %d sub-reactors", sub_count);
    return 0;
}

void reactor_setup_eventfds(reactor_t *r)
{
    if (!r || !r->subs) return;
    for (int i = 0; i < r->sub_count; i++) {
        chat_set_eventfd(i, r->subs[i].event_fd);
    }
}

int reactor_run(reactor_t *r)
{
    if (!r) return -1;

    r->running = 1;

    /* 启动子 Reactor 线程 */
    for (int i = 0; i < r->sub_count; i++) {
        r->subs[i].running = 1;
        if (pthread_create(&r->subs[i].thread, NULL,
                           sub_reactor_thread, &r->subs[i]) != 0) {
            LOG_ERROR("Failed to create sub-reactor[%d] thread", i);
            /* 停止已启动的 */
            r->running = 0;
            for (int j = 0; j < i; j++) {
                r->subs[j].running = 0;
                /* 唤醒子 reactor 使其退出 */
                uint64_t val = 1;
                ssize_t ret __attribute__((unused));
                ret = write(r->subs[j].event_fd, &val, sizeof(val));
                pthread_join(r->subs[j].thread, NULL);
            }
            return ERR_THREAD_CREATE;
        }
    }

    /* 主 Reactor 在当前线程运行 */
    main_reactor_thread(r);

    return 0;
}

void reactor_stop(reactor_t *r)
{
    if (!r || r->stopped) return;
    r->stopped = 1;

    LOG_INFO("Stopping reactor...");
    r->running = 0;

    /* 唤醒主 Reactor (通过关闭 listen_fd 让 epoll_wait 返回) */
    if (r->listen_fd >= 0) {
        shutdown(r->listen_fd, SHUT_RDWR);
    }

    /* 停止子 Reactor (无条件 join, 因为信号处理器可能已设 running=0) */
    for (int i = 0; i < r->sub_count; i++) {
        if (r->subs[i].running) {
            r->subs[i].running = 0;
            /* 唤醒子 reactor (如果还没被信号处理器唤醒) */
            uint64_t val = 1;
            ssize_t ret __attribute__((unused));
            ret = write(r->subs[i].event_fd, &val, sizeof(val));
        }
        /* 无论 running 状态如何, 都要 join 线程 */
        pthread_join(r->subs[i].thread, NULL);
        if (r->subs[i].event_fd >= 0) {
            close(r->subs[i].event_fd);
        }
        if (r->subs[i].epoll_fd >= 0) {
            close(r->subs[i].epoll_fd);
        }
    }

    if (r->main_epoll_fd >= 0) close(r->main_epoll_fd);
    if (r->listen_fd >= 0) close(r->listen_fd);
    if (r->subs) free(r->subs);

    LOG_INFO("Reactor stopped");
}
