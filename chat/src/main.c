#include "common.h"
#include "logger.h"
#include "threadpool.h"
#include "client.h"
#include "reactor.h"
#include "heartbeat.h"
#include "chat_service.h"
#include "db.h"

/* 全局变量, 用于信号处理 */
static reactor_t     g_reactor;
static threadpool_t *g_biz_pool   = NULL;
static heartbeat_t   g_heartbeat;
static volatile int  g_running    = 1;  /* volatile: 信号和主线程共享 */

/* 信号处理器 —— 只设标志, 不阻塞 */
static void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO("Received signal %d, shutting down...", sig);
        g_running = 0;

        /* 设置 reactor 停止标志 */
        g_reactor.running = 0;

        /* 唤醒主 Reactor (关闭监听 fd 让 epoll_wait 返回) */
        if (g_reactor.listen_fd >= 0) {
            shutdown(g_reactor.listen_fd, SHUT_RDWR);
        }

        /* 唤醒所有子 Reactor */
        for (int i = 0; i < g_reactor.sub_count; i++) {
            g_reactor.subs[i].running = 0;
            if (g_reactor.subs[i].event_fd >= 0) {
                uint64_t val = 1;
                ssize_t ret __attribute__((unused));
                ret = write(g_reactor.subs[i].event_fd, &val, sizeof(val));
            }
        }
    }
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags   = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* 忽略 SIGPIPE */
    signal(SIGPIPE, SIG_IGN);
}

static void print_banner(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║      High-Concurrency IM Chat Server             ║\n");
    printf("║      epoll ET + Reactor + ThreadPool + RingBuf   ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    printf("\n");
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -p <port>      Listen port (default: %d)\n", DEFAULT_PORT);
    printf("  -s <num>       Sub-reactor count (default: %d)\n",
           DEFAULT_SUB_REACTORS);
    printf("  -t <num>       Business thread count (default: %d)\n",
           DEFAULT_BIZ_THREADS);
    printf("  -l <file>      Log file path (default: stdout only)\n");
    printf("  -h             Show this help\n");
}

int main(int argc, char *argv[])
{
    int port       = DEFAULT_PORT;
    int sub_count  = DEFAULT_SUB_REACTORS;
    int biz_threads = DEFAULT_BIZ_THREADS;
    const char *log_file = "chat_server.log";

    /* 解析命令行参数 */
    int opt;
    while ((opt = getopt(argc, argv, "p:s:t:l:h")) != -1) {
        switch (opt) {
        case 'p': port        = atoi(optarg); break;
        case 's': sub_count   = atoi(optarg); break;
        case 't': biz_threads = atoi(optarg); break;
        case 'l': log_file    = optarg;       break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    print_banner();

    /* 初始化日志 */
    if (logger_init(log_file) != 0) {
        fprintf(stderr, "Failed to initialize logger\n");
        return 1;
    }

    LOG_INFO("========================================");
    LOG_INFO("IM Chat Server starting...");
    LOG_INFO("Configuration: port=%d, sub_reactors=%d, biz_threads=%d",
             port, sub_count, biz_threads);
    LOG_INFO("========================================");

    /* 初始化数据库 */
    if (db_init() != 0) {
        LOG_ERROR("Failed to initialize database");
        LOG_WARN("Continuing without database...");
        /* 不退出, 允许无数据库运行 (登录会失败) */
    }

    /* 设置信号处理 */
    setup_signals();

    /* 初始化客户端管理 */
    client_manager_init();

    /* 创建业务线程池 */
    g_biz_pool = threadpool_create(biz_threads);
    if (!g_biz_pool) {
        LOG_ERROR("Failed to create business thread pool");
        return 1;
    }

    /* 初始化 Reactor (内含子 Reactor 和 eventfd 设置) */
    if (reactor_init(&g_reactor, port, sub_count, g_biz_pool) != 0) {
        LOG_ERROR("Failed to initialize reactor");
        threadpool_destroy(g_biz_pool);
        client_manager_destroy();
        logger_close();
        return 1;
    }

    /* 启动心跳检测 */
    if (heartbeat_start(&g_heartbeat,
                        HEARTBEAT_TIMEOUT,
                        HEARTBEAT_CHECK_INTERVAL) != 0) {
        LOG_ERROR("Failed to start heartbeat");
        reactor_stop(&g_reactor);
        threadpool_destroy(g_biz_pool);
        client_manager_destroy();
        logger_close();
        return 1;
    }

    LOG_INFO("Server is ready. Waiting for connections...");
    LOG_INFO("Press Ctrl+C to stop the server");

    /* 启动 Reactor (主 Reactor 在当前线程运行, 阻塞直到停止) */
    reactor_run(&g_reactor);

    /* 优雅退出 */
    LOG_INFO("Shutting down...");

    heartbeat_stop(&g_heartbeat);
    reactor_stop(&g_reactor);
    threadpool_destroy(g_biz_pool);
    client_manager_destroy();
    db_close();
    logger_close();

    LOG_INFO("Server stopped. Goodbye!");
    printf("\nServer stopped successfully.\n");

    return 0;
}
