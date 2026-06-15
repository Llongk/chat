# 高并发多人在线 IM 聊天室服务端

## 项目简介

基于 **epoll 边缘触发 + 主从 Reactor 模型 + 线程池 + 环形缓冲区 + MySQL** 的 C 语言高并发 IM 聊天室服务端。

支持海量用户长连接在线实时通信，具备 **注册/登录(MySQL 密码验证)**、**管理员权限管理**、公屏群发、点对点私聊、在线成员查询、心跳超时检测、刷屏限流等完整功能。

## 技术栈

| 技术 | 说明 |
|------|------|
| C 语言 | 核心实现语言 |
| Linux Socket | TCP 网络通信 |
| epoll ET | 边缘触发高并发事件驱动 |
| 主从 Reactor | 主线程 accept + N 子线程处理 IO |
| 非阻塞 IO | 所有 socket 设 O_NONBLOCK |
| 自定义协议 | 8 字节包头 + 可变包体, 解决 TCP 粘包拆包 |
| 线程池 | 两层隔离: 网络 IO 线程 + 业务逻辑线程 |
| 环形缓冲区 | 每连接独立 ringbuf 暂存收发数据 |
| eventfd | 跨线程异步通知机制 |
| MySQL | 用户注册/登录验证、密码 SHA256 哈希、管理员权限 |

---

## 文件编译顺序 (依赖关系)

编译顺序由底层模块到上层模块，遵循依赖关系。Makefile 中按文件名排序，实际依赖链如下:

```
第一层 (无内部依赖, 最先编译)
  common.h          → 通用宏/结构体/工具函数 (被所有模块依赖)
  protocol.h        → 协议定义 (依赖 common.h)

第二层 (依赖 common.h + protocol.h)
  ringbuf.h/c       → 环形缓冲区 (依赖 common.h)
  logger.h/c        → 日志系统 (依赖 common.h)

第三层 (依赖 ringbuf, logger)
  threadpool.h/c    → 线程池 (依赖 common.h, logger)
  client.h/c        → 客户端管理 (依赖 common.h, ringbuf, protocol)
  db.h/c            → MySQL 数据库 (依赖 common.h, logger)

第四层 (依赖 client, db)
  ratelimit.h/c     → 限流 (依赖 common.h, client)
  heartbeat.h/c     → 心跳 (依赖 common.h, client, logger)

第五层 (依赖 client, threadpool, ratelimit)
  chat_service.h/c  → 业务逻辑 (依赖 common.h, protocol, client, ratelimit, logger)

第六层 (依赖 threadpool, chat_service, client)
  reactor.h/c       → Reactor 模型 (依赖 common.h, threadpool, client, chat_service, logger)

第七层 (入口, 依赖所有模块)
  main.c            → 主入口 (依赖 common.h, logger, threadpool, client, reactor, heartbeat)
```

### Makefile 编译目标

| 目标 | 命令 | 生成文件 |
|------|------|----------|
| 全部 | `make` | chat_server + chat_client |
| 服务端 | `make server` | chat_server |
| 客户端 | `make client` | chat_client |
| 清理 | `make clean` | 删除 obj/ 和可执行文件 |

### 各源文件编译产物

```
src/logger.c      → obj/logger.o       (日志系统)
src/ringbuf.c     → obj/ringbuf.o      (环形缓冲区)
src/threadpool.c  → obj/threadpool.o   (线程池)
src/client.c      → obj/client.o       (客户端管理)
src/db.c          → obj/db.o           (MySQL 数据库)
src/ratelimit.c   → obj/ratelimit.o    (限流)
src/heartbeat.c   → obj/heartbeat.o    (心跳)
src/chat_service.c→ obj/chat_service.o (业务逻辑)
src/reactor.c     → obj/reactor.o      (Reactor模型)
src/main.c        → obj/main.o         (主入口)

链接: 以上所有 .o → chat_server (-lpthread -lmysqlclient)

src/client_main.c → obj/client_main.o → chat_client (-lpthread)
```

---

## 服务端执行流程 (函数调用链)

### 启动阶段: main() → 逐层初始化

```
main()                                          [src/main.c]
  ├── print_banner()                            显示欢迎横幅
  ├── logger_init("chat_server.log")            初始化日志 [src/logger.c]
  │     └── fopen() 打开日志文件
  ├── setup_signals()                           注册 SIGINT/SIGTERM 信号处理
  ├── client_manager_init()                     初始化全局客户端表 [src/client.c]
  │     └── memset(g_clients, 0, ...)
  ├── db_init()                                 初始化 MySQL 数据库 [src/db.c]
  │     ├── mysql_init() + mysql_real_connect() 连接 MySQL
  │     ├── CREATE DATABASE IF NOT EXISTS       建库
  │     ├── CREATE TABLE IF NOT EXISTS users    建用户表
  │     └── 若无用户 → INSERT 默认管理员 admin  自动创建管理员
  ├── threadpool_create(biz_threads)            创建业务线程池 [src/threadpool.c]
  │     ├── malloc pool + threads[]
  │     ├── pthread_mutex_init / pthread_cond_init
  │     └── pthread_create × N → threadpool_worker() 启动工作线程
  ├── reactor_init(&g_reactor, port, sub_count, pool) 初始化Reactor [src/reactor.c]
  │     ├── socket() + bind() + listen()         创建监听socket
  │     ├── set_nonblock(listen_fd)              设为非阻塞
  │     ├── epoll_create1() → main_epoll_fd      主Reactor epoll
  │     ├── epoll_ctl(ADD, listen_fd, EPOLLIN|ET)  注册监听fd
  │     └── for i in sub_count:                  创建子Reactor
  │           ├── epoll_create1() → sub_epoll_fd   子epoll实例
  │           ├── eventfd() → event_fd             跨线程通知fd
  │           ├── epoll_ctl(ADD, event_fd)         注册到子epoll
  │           └── chat_set_eventfd(i, event_fd)    记录eventfd给chat_service
  ├── heartbeat_start(&g_heartbeat, 30, 5)      启动心跳检测线程 [src/heartbeat.c]
  │     └── pthread_create → heartbeat_thread()
  └── reactor_run(&g_reactor)                   启动Reactor [src/reactor.c]
        ├── for i in sub_count:
        │     └── pthread_create → sub_reactor_thread()  启动子Reactor线程
        └── main_reactor_thread()               主Reactor在当前线程运行(阻塞)
```

### 运行阶段: 事件驱动循环

```
━━━ 主 Reactor 线程 ━━━
main_reactor_thread()                            [src/reactor.c]
  └── while(running):
        └── epoll_wait(main_epoll_fd, timeout=100ms)
              └── EPOLLIN on listen_fd:
                    └── while(1):                ET模式循环accept
                          ├── accept() → client_fd
                          ├── set_nonblock(client_fd)
                          ├── client_create(fd, addr)   [src/client.c]
                          │     ├── malloc client_t
                          │     ├── ringbuf_create(64KB) → inbuf
                          │     └── ringbuf_create(64KB) → outbuf
                          ├── rr_index % sub_count → 轮询选子Reactor
                          └── epoll_ctl(sub_epoll, ADD, client_fd, EPOLLIN|ET)
                                注册到子Reactor

━━━ 子 Reactor 线程 (×N) ━━━
sub_reactor_thread()                             [src/reactor.c]
  └── while(running):
        └── epoll_wait(sub_epoll, timeout=100ms)  等待事件
              │
              ├── eventfd 可读 → 业务线程通知有数据待发
              │     ├── read(event_fd)            消费通知
              │     └── sub_flush_pending_sends() 遍历客户端发送待发数据
              │
              ├── EPOLLIN on client_fd → 客户端发来数据
              │     ├── sub_handle_read(cli)      [src/reactor.c]
              │     │     └── while(1):            ET模式循环recv
              │     │           ├── recv() → 数据写入 cli->inbuf (ringbuf)
              │     │           └── EAGAIN → break
              │     └── sub_try_assemble_packet(cli)  [src/reactor.c]
              │           └── while(ringbuf_readable > 0):
              │                 ├── 状态=HEADER:
              │                 │     ├── ringbuf_peek(8字节) 窥探包头
              │                 │     ├── proto_deserialize_header() 解析
              │                 │     ├── proto_validate_header() 校验魔数
              │                 │     ├── ringbuf_skip(8) 跳过包头
              │                 │     └── 状态→BODY, 记录body_len
              │                 └── 状态=BODY:
              │                       ├── ringbuf_read(body_len) 读完整包体
              │                       ├── malloc chat_task_t{fd,type,body,len}
              │                       └── threadpool_add_task(biz_pool,
              │                             chat_service_process, task)
              │                             提交到业务线程池 [src/threadpool.c]
              │
              ├── EPOLLOUT on client_fd → 发送缓冲区有数据
              │     └── sub_handle_write(cli)     [src/reactor.c]
              │           └── while(outbuf可读):
              │                 ├── ringbuf_peek → send() → ringbuf_skip
              │                 ├── EAGAIN → 保持EPOLLOUT等待下次可写
              │                 └── outbuf空 → epoll_ctl(MOD, EPOLLIN|ET) 移除EPOLLOUT
              │
              └── 定期清理:
                    └── sub_cleanup_clients()     清理!alive的客户端
                          ├── 广播离开消息(给在线用户)
                          ├── epoll_ctl(DEL) 移除fd
                          └── client_destroy()    释放ringbuf+关闭socket

━━━ 业务线程池 (×M) ━━━
threadpool_worker()                              [src/threadpool.c]
  └── while(!shutdown):
        └── pthread_cond_wait()                  等待任务
              └── 取出 task_node
                    └── task->func(task->arg)
                          │
                          └── chat_service_process(task)  [src/chat_service.c]
                                │
                                ├── MSG_LOGIN_REQ → handle_login()
                                │     ├── 解析 JSON: username + password
                                │     ├── db_user_login() 验证密码哈希
                                │     ├── 检查 is_admin / status (封禁)
                                │     ├── client_set_logged_in() + 设置 is_admin
                                │     ├── chat_send_message(LOGIN_RESP)  响应登录者
                                │     │     ├── proto_build_header() 构建包头
                                │     │     ├── ringbuf_write(cli->outbuf) 写入发送缓冲
                                │     │     └── chat_notify_sub_reactor() 写eventfd唤醒子Reactor
                                │     └── 遍历所有在线客户端广播JOIN消息
                                │
                                ├── MSG_REGISTER_REQ → handle_register()
                                │     ├── 解析 JSON: username + password
                                │     ├── db_user_register() 插入 MySQL
                                │     └── chat_send_message(REGISTER_RESP)
                                │
                                ├── MSG_PUBLIC_MSG → handle_public_msg()
                                │     ├── 检查登录态
                                │     ├── ratelimit_check() 限流 [src/ratelimit.c]
                                │     └── 遍历所有在线客户端广播
                                │
                                ├── MSG_PRIVATE_REQ → handle_private_msg()
                                │     ├── 检查登录态 + 限流
                                │     ├── 解析 target:message 格式
                                │     ├── client_find_by_name(target) 查找目标
                                │     └── chat_send_message() → 目标客户端
                                │
                                ├── MSG_ONLINE_LIST_REQ → handle_online_list()
                                │     ├── client_get_online_list() 收集在线用户名
                                │     └── chat_send_message(ONLINE_LIST_RESP)
                                │
                                ├── MSG_HEARTBEAT_REQ → handle_heartbeat()
                                │     ├── client_update_active() 刷新活跃时间
                                │     └── chat_send_message(HEARTBEAT_RESP) "pong"
                                │
                                ├── MSG_KICK_REQ → handle_kick()
                                │     ├── 检查 is_admin 权限
                                │     ├── client_find_by_name() 查找目标
                                │     ├── 发送被踢通知 → 目标客户端
                                │     └── target->alive = 0 (Reactor 清理)
                                │
                                └── free(task->body) + free(task)  释放任务资源

━━━ 心跳检测线程 ━━━
heartbeat_thread()                               [src/heartbeat.c]
  └── while(running):
        └── sleep(5s)
              └── client_foreach(hb_check_callback)
                    └── 对每个客户端:
                          if (now - last_active > 30s):
                            cli->alive = 0      标记断开(子Reactor会清理)
```

### 退出阶段: 信号 → 优雅关闭

```
SIGINT/SIGTERM → signal_handler()                [src/main.c]
  └── reactor_stop(&g_reactor)                   [src/reactor.c]
        ├── shutdown(listen_fd)                  唤醒主Reactor
        ├── for each sub_reactor:
        │     ├── running = 0
        │     ├── write(event_fd)                唤醒子Reactor
        │     └── pthread_join()                 等待子Reactor线程退出
        └── close all fds + free subs

main() 继续执行:
  ├── heartbeat_stop()                           等待心跳线程退出
  ├── threadpool_destroy()                       等待业务线程退出+清理任务队列
  ├── client_manager_destroy()                   释放所有客户端
  ├── db_close()                                 关闭 MySQL 连接
  └── logger_close()                             关闭日志文件
```

---

## 自定义通信协议

用于彻底解决 TCP 粘包/拆包问题:

```
包头 (固定 8 字节):
┌──────────────────┬───────┬───────┬──────────────────┐
│ Magic (4B)       │ Type  │ Flags │ Body Len (2B)    │
│ 0x48494D53       │ (1B)  │ (1B)  │ (网络字节序)      │
└──────────────────┴───────┴───────┴──────────────────┘

包体: 可变长度 (最大 65535 字节, JSON 文本)
```

### 消息类型

| 类型码 | 名称 | 方向 | 说明 |
|--------|------|------|------|
| 0x01 | LOGIN_REQ | C→S | 登录请求 |
| 0x02 | LOGIN_RESP | S→C | 登录响应 |
| 0x03 | PUBLIC_MSG | C→S | 公屏消息 |
| 0x04 | PUBLIC_BROADCAST | S→C | 公屏广播 |
| 0x05 | PRIVATE_REQ | C→S | 私聊请求 |
| 0x06 | PRIVATE_RESP | S→C | 私聊响应 |
| 0x07 | ONLINE_LIST_REQ | C→S | 在线列表请求 |
| 0x08 | ONLINE_LIST_RESP | S→C | 在线列表响应 |
| 0x09 | HEARTBEAT_REQ | C→S | 心跳请求 |
| 0x0A | HEARTBEAT_RESP | S→C | 心跳响应 |
| 0x0B | REGISTER_REQ | C→S | 注册请求 |
| 0x0C | REGISTER_RESP | S→C | 注册响应 |
| 0x0D | KICK_REQ | C→S | 管理员踢人请求 |
| 0x0E | KICK_RESP | S→C | 管理员踢人响应 |
| 0xFF | ERROR_RESP | S→C | 错误响应 |

---

## 架构设计

```
                    ┌──────────────────┐
                    │   Main Reactor   │  (单线程)
                    │  epoll + accept  │
                    └────────┬─────────┘
                             │ 轮询分发新连接
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
        ┌──────────┐  ┌──────────┐  ┌──────────┐
        │ Sub-R 0  │  │ Sub-R 1  │  │ Sub-R N  │  (N线程, 各独立epoll)
        │ epoll ET │  │ epoll ET │  │ epoll ET │
        └────┬─────┘  └────┬─────┘  └────┬─────┘
             │              │              │
             └──────────────┼──────────────┘
                            │ 提交完整包
                    ┌───────▼────────┐
                    │ Business Pool  │  (M线程)
                    │ 登录/聊天/限流  │
                    └───────┬────────┘
                            │ 写响应到 outbuf + eventfd 通知
                            ▼
                    ┌──────────────┐
                    │  Sub Reactor │  发送数据给客户端
                    └──────────────┘
```

### 数据流

```
客户端 → TCP → epoll ET → ringbuf(in) → 协议解析(粘包处理)
       → 业务线程池 → 消息路由 → ringbuf(out) → epoll ET → 发送 → 客户端
```

---

## 编译与运行

### 环境要求

- Linux 操作系统 (内核 2.6+)
- GCC 编译器
- GNU Make
- MySQL 8.0+ (或 MariaDB 10.3+)
- libmysqlclient 运行时库 (`apt install libmysqlclient21`)

### 快速启动

```bash
# 1. 确保 MySQL 正在运行
sudo systemctl start mysql

# 2. 初始化数据库 (三选一)
sudo mysql < init_db.sql              # 方式一: 手动执行 SQL 脚本
make run                               # 方式二: 启动服务端自动建库建表
# 方式三: 见 make setup-db 输出

# 3. 编译
make

# 4. 启动服务端 (默认端口 8888)
make run

# 5. 启动客户端 (新终端)
make client-run ARGS='127.0.0.1 8888'
```

### 编译

```bash
make          # 编译服务端 + 客户端
make server   # 仅编译服务端
make client   # 仅编译客户端
```

### 运行服务端

```bash
# 默认配置 (端口 8888, 4 子 Reactor, 4 业务线程)
./chat_server

# 自定义配置
./chat_server -p 9999 -s 8 -t 8

# 指定日志文件
./chat_server -l /var/log/chat.log
```

### 运行客户端

```bash
# 本机连接
./chat_client 127.0.0.1 8888

# 局域网其他电脑连接 (替换为实际服务器IP)
./chat_client 10.11.8.54 8888
```

### 客户端命令

| 命令 | 示例 | 功能 |
|------|------|------|
| `/register <用户名> <密码>` | `/register alice 123456` | 注册新账号 |
| `/login <用户名> <密码>` | `/login alice 123456` | 登录 (必须先注册) |
| `/msg <消息>` | `/msg 大家好` | 公屏消息 |
| `直接输入文本` | `你好` | 等同 /msg |
| `/to <用户> <消息>` | `/to bob 你好` | 私聊 |
| `/online` | `/online` | 查看在线用户 (含管理员★) |
| `/kick <用户名>` | `/kick alice` | 管理员踢人下线 |
| `/quit` | `/quit` | 退出 |

### 默认管理员账号

首次启动服务端时自动创建，或通过 `init_db.sql` 手动创建:

| 用户名 | 密码 | 权限 |
|--------|------|------|
| admin | admin123 | 管理员 (可踢人) |

### 命令行参数 (服务端)

| 参数 | 默认值 | 说明 |
|------|--------|------|
| -p \<port\> | 8888 | 监听端口 |
| -s \<num\> | 4 | 子 Reactor 线程数 |
| -t \<num\> | 4 | 业务线程池大小 |
| -l \<file\> | chat_server.log | 日志文件路径 |
| -h | - | 显示帮助信息 |

### 清理

```bash
make clean
```

---

## 功能说明

### 1. 用户注册 (MySQL)
客户端连接后可发送 `/register <用户名> <密码>` 注册新账号。用户名唯一，密码通过 MySQL `SHA2(CONCAT(username, ':', password), 256)` 安全哈希存储。

### 2. 用户登录 (MySQL 密码验证)
客户端发送 `/login <用户名> <密码>`，服务端查询 MySQL 验证密码哈希。登录成功后在 `client_t` 中设置 `is_admin` 标志。账号被封禁 (status=0) 时拒绝登录。

### 3. 管理员权限
- 管理员登录后用户名旁显示 `★` 标识
- 管理员可使用 `/kick <用户名>` 踢人下线
- 默认管理员: `admin` / `admin123` (首次启动自动创建)

### 4. 公屏群发
登录后发送 MSG_PUBLIC_MSG, 服务端广播给所有在线用户。消息含 `is_admin` 标识。

### 5. 点对点私聊
发送 MSG_PRIVATE_REQ, 格式: `{"to":"target","msg":"hello"}`, 消息仅转发给目标用户。

### 6. 在线成员查询
发送 MSG_ONLINE_LIST_REQ, 返回当前所有在线用户名及管理员标识。

### 7. 心跳超时检测
- 默认 30 秒无任何消息则判定超时断开
- 客户端需定期发送 MSG_HEARTBEAT_REQ
- 服务端每 5 秒轮询检测

### 8. 刷屏限流 (令牌桶算法)
- 令牌桶容量 5, 每秒补充 5 个令牌
- 消息到达时消耗 1 个令牌, 无令牌则拒绝
- 支持突发流量: 初始满令牌可连发 5 条
- 使用 `gettimeofday()` 微秒级精度计算时间差

### 9. 日志系统
- 带毫秒级时间戳
- 同时输出到文件和 stdout
- 使用 `gettimeofday()` + `localtime_r()` + `strftime()` 生成时间
- `pthread_mutex` 保护日志写入, 防止多线程交错

---

## 数据库设计

### users 表结构

```sql
CREATE TABLE users (
    id            INT AUTO_INCREMENT PRIMARY KEY,
    username      VARCHAR(32)   NOT NULL UNIQUE,
    password_hash VARCHAR(128)  NOT NULL,
    is_admin      TINYINT       DEFAULT 0,
    status        TINYINT       DEFAULT 1   COMMENT '1=正常, 0=封禁',
    created_at    TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    last_login    TIMESTAMP     NULL DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### 密码安全

- 密码哈希公式: `SHA2(CONCAT(username, ':', password), 256)`
- 在 MySQL 服务端计算哈希，减少客户端依赖
- 用户名作为盐值 (salt)，保证相同密码不同用户产生不同哈希
- 所有用户输入通过 `mysql_real_escape_string()` 转义防 SQL 注入

### 数据库配置

默认配置见 `include/common.h`，可按需修改:

```c
#define DB_HOST  "127.0.0.1"
#define DB_USER  "root"
#define DB_PASS  ""
#define DB_NAME  "chat_db"
#define DB_PORT  3306
```

---

## Linux 系统编程方法详解

### 1. epoll 事件驱动 (reactor.c, main.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `epoll_create1(0)` | reactor.c | 创建 epoll 实例 | 内核分配事件表, 返回文件描述符。主 Reactor 1 个 + N 个子 Reactor 各 1 个 |
| `epoll_ctl(ADD/MOD/DEL)` | reactor.c | 增/改/删监听 fd | 向内核事件表注册要监听的文件描述符和事件类型 |
| `epoll_wait(timeout=100ms)` | reactor.c | 等待事件就绪 | 阻塞等待, 返回就绪事件数组。timeout 100ms 既保证响应速度又避免空转 |

**边缘触发 (ET) 模式原理:**

```
文件: src/reactor.c — sub_handle_read() / main_reactor_thread()

epoll_ctl 时使用 EPOLLET 标志:
  ev.events = EPOLLIN | EPOLLET;   // 边缘触发读
  ev.events = EPOLLOUT | EPOLLET;  // 边缘触发写

ET 与 LT 的区别:
  LT(水平触发): 只要缓冲区有数据, epoll_wait 每次都会返回 → 可能重复通知
  ET(边缘触发): 只在状态变化时通知一次 → 必须循环读写直到 EAGAIN

ET 模式的关键代码模式:
  while (1) {
      n = recv(fd, buf, size, 0);
      if (n > 0)  ringbuf_write(inbuf, buf, n);  // 持续读取
      if (n == 0) { cli->alive = 0; break; }       // 对端关闭
      if (n < 0 && errno == EAGAIN) break;          // 数据读完, 退出循环
      if (n < 0)  { cli->alive = 0; break; }       // 读错误
  }

必须配合非阻塞 IO 使用, 否则最后一次 read 会永久阻塞。
```

**EPOLLRDHUP 对端关闭检测:**

```
文件: src/reactor.c — sub_reactor_thread()

ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;

当客户端 close() 或 shutdown(SHUT_WR) 时, 内核产生 EPOLLRDHUP 事件,
不需要等待 read() 返回 0 就能感知, 响应更快。
```

### 2. 非阻塞 IO (common.h, reactor.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `fcntl(fd, F_GETFL)` | common.h — set_nonblock() | 获取文件状态标志 | 读取当前 fd 的属性 |
| `fcntl(fd, F_SETFL, flags \| O_NONBLOCK)` | common.h — set_nonblock() | 设置非阻塞 | 添加 O_NONBLOCK 标志, 之后 read/write 不会阻塞 |

```
文件: include/common.h — set_nonblock()
      src/reactor.c  — accept 后调用

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

所有 socket (listen_fd + client_fd) 都设为非阻塞:
  - listen_fd: 防止 accept 阻塞主 Reactor
  - client_fd: epoll ET 模式要求必须是 nonblock, 否则循环读写时最后会阻塞
```

### 3. TCP Socket 编程 (reactor.c, client_main.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `socket(AF_INET, SOCK_STREAM, 0)` | reactor.c, client_main.c | 创建 TCP socket | AF_INET=IPv4, SOCK_STREAM=TCP |
| `setsockopt(SO_REUSEADDR)` | reactor.c | 端口复用 | 服务端重启时可立即绑定之前被占用的端口 |
| `bind()` | reactor.c | 绑定地址和端口 | 绑定 INADDR_ANY(0.0.0.0) 监听所有网卡 |
| `listen(fd, 128)` | reactor.c | 开始监听 | 128 是内核 backlog 队列长度 |
| `accept()` | reactor.c | 接受新连接 | ET 模式下循环 accept 直到返回 EAGAIN |
| `connect()` | client_main.c | 客户端连接服务器 | 三次握手 |
| `send(fd, buf, len, MSG_NOSIGNAL)` | reactor.c, client_main.c | 发送数据 | MSG_NOSIGNAL 防止对端关闭时触发 SIGPIPE |
| `recv(fd, buf, len, 0)` | reactor.c, client_main.c | 接收数据 | 非阻塞模式下返回 -1 + EAGAIN 表示暂无数据 |
| `shutdown(fd, SHUT_RDWR)` | reactor.c, client_main.c | 半关闭连接 | 优雅断开, 先停止读写再 close |
| `close(fd)` | reactor.c, client.c | 关闭 socket | 释放文件描述符 |
| `inet_pton()` / `inet_ntop()` | reactor.c, client.c | IP 地址转换 | 字符串与二进制 IP 互转, 兼容 IPv4/IPv6 |

```
服务端流程 (src/reactor.c — reactor_init):
  socket() → setsockopt(SO_REUSEADDR) → bind() → listen()
  → epoll_ctl(ADD, listen_fd, EPOLLIN|ET)
  → while(1): epoll_wait → accept() 循环

客户端流程 (src/client_main.c — main):
  socket() → connect() → send/recv
```

### 4. eventfd 跨线程通知 (reactor.c, chat_service.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `eventfd(0, EFD_NONBLOCK)` | reactor.c | 创建事件通知 fd | 内核维护一个 uint64 计数器, 可被 epoll 监听 |
| `write(eventfd, &val, 8)` | chat_service.c, reactor.c | 发送通知 | 向计数器写入数值, 触发 epoll 可读事件 |
| `read(eventfd, &val, 8)` | reactor.c | 消费通知 | 读取计数器(归零), 清除可读状态 |

```
文件: src/reactor.c — reactor_init() 创建 eventfd
      src/chat_service.c — chat_notify_sub_reactor() 写 eventfd
      src/reactor.c — sub_reactor_thread() 读 eventfd

工作流程:
  1. 每个子 Reactor 创建 eventfd, 注册到自己的 epoll (EPOLLIN|ET)
  2. 业务线程处理完消息后, 把响应写入客户端的 outbuf
  3. write(eventfd) → 唤醒正在 epoll_wait 的子 Reactor
  4. 子 Reactor 读到 eventfd 事件 → 遍历客户端发送 outbuf 数据

eventfd 比 pipe 更轻量: 只有一个 fd, 不需要管理读写两端。
```

### 5. POSIX 线程同步 (threadpool.c, client.c, chat_service.c, reactor.c, heartbeat.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `pthread_create()` | threadpool.c, reactor.c, heartbeat.c | 创建线程 | 产生新线程执行指定函数 |
| `pthread_join()` | threadpool.c, reactor.c, heartbeat.c | 等待线程结束 | 阻塞等待目标线程退出, 回收资源 |
| `pthread_detach()` | client_main.c | 分离线程 | 线程结束时自动回收, 不需要 join |
| `pthread_mutex_init()` | threadpool.c, client.c, logger.c | 初始化互斥锁 | 创建 mutex, 保护临界区 |
| `pthread_mutex_lock/unlock()` | 多个文件 | 加锁/解锁 | 保证同时只有一个线程访问共享数据 |
| `pthread_mutex_destroy()` | threadpool.c, client.c | 销毁互斥锁 | 释放 mutex 资源 |
| `pthread_cond_init()` | threadpool.c | 初始化条件变量 | 用于线程间"等待-唤醒" |
| `pthread_cond_wait()` | threadpool.c | 等待条件 | 原子操作: unlock + 阻塞 + 被唤醒后 lock |
| `pthread_cond_signal/broadcast()` | threadpool.c | 唤醒等待线程 | signal 唤醒 1 个, broadcast 唤醒全部 |

```
互斥锁使用场景 (每个锁保护一份共享数据):
  src/threadpool.c: pool->mutex    → 保护任务队列 (task_head/task_tail)
  src/client.c:    g_client_mutex  → 保护全局客户端表 (g_clients[])
  src/client.h:    outbuf_mutex    → 保护每个客户端的发送缓冲区 (outbuf ringbuf)
  src/logger.c:    g_log_mutex     → 保护日志文件 (防止多线程日志交错)

条件变量使用场景:
  src/threadpool.c — threadpool_worker():
    while (task_count == 0 && !shutdown)
        pthread_cond_wait(&cond, &mutex);  // 无任务时休眠, 不消耗 CPU
    // 被 threadpool_add_task() 的 pthread_cond_signal() 唤醒

线程创建场景:
  src/threadpool.c: pthread_create × M → threadpool_worker 业务线程
  src/reactor.c:    pthread_create × N → sub_reactor_thread 子Reactor线程
  src/heartbeat.c:  pthread_create × 1 → heartbeat_thread 心跳线程
  src/client_main.c:pthread_create × 1 → receiver_thread 客户端接收线程
  src/client_main.c:pthread_create × 1 → heartbeat_thread 客户端心跳(分离)
```

### 6. 信号处理 (main.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `sigaction(SIGINT, &sa, NULL)` | main.c | 注册 Ctrl+C 处理器 | 比 signal() 更可靠, 可控制更多行为 |
| `sigaction(SIGTERM, &sa, NULL)` | main.c | 注册 kill 信号处理器 | 响应 `kill <pid>` |
| `signal(SIGPIPE, SIG_IGN)` | main.c, client_main.c | 忽略 SIGPIPE | 防止向已关闭 socket 写数据时进程崩溃 |

```
文件: src/main.c — setup_signals()

优雅退出流程:
  Ctrl+C → SIGINT → signal_handler()
    → g_running = 0
    → reactor_stop() 关闭 listen_fd → 主Reactor 退出
    → write(eventfd) → 子Reactor 退出
    → main() 继续: heartbeat_stop → threadpool_destroy → client_manager_destroy
```

### 7. 环形缓冲区 (ringbuf.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `malloc(capacity)` | ringbuf.c | 分配缓冲区内存 | 堆上分配连续内存, 每连接分配 64KB |
| `memcpy()` | ringbuf.c | 内存复制 | 处理环形回绕: 分一段或两段复制 |
| `ringbuf_write()` | ringbuf.c | 写入数据 | 移动 write_pos 指针, 保留 1 字节防止满时 read_pos==write_pos |
| `ringbuf_read()` | ringbuf.c | 读取数据 | 移动 read_pos 指针, 同时消费数据 |
| `ringbuf_peek()` | ringbuf.c | 窥探数据 | 只复制不移动 read_pos, 用于协议解析时预读包头 |

```
文件: src/ringbuf.c

环形缓冲区结构 (双指针):
  read_pos  → 下次读的起始位置
  write_pos → 下次写的起始位置
  可读字节 = (write_pos - read_pos + capacity) % capacity
  可写字节 = capacity - 可读字节 - 1  (保留 1 字节防重叠)

应用场景:
  - inbuf:  reactor 收到数据 → ringbuf_write, 协议解析 → ringbuf_peek/read/skip
  - outbuf: 业务线程生成响应 → ringbuf_write, reactor 发送 → ringbuf_peek/read/skip

为什么用环形缓冲区:
  - 固定大小: 不会无限增长, 天然限流
  - 零拷贝: 数据在缓冲区中只存一份, peek 只是复制指针不移动
  - 高效: O(1) 读写, 无需像链表那样频繁 malloc/free
```

### 8. 网络字节序转换 (protocol.h, chat_service.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `htonl(uint32_t)` | protocol.h | 主机序→网络序 (4B) | 保证不同 CPU 架构间整数一致 (Big Endian) |
| `htons(uint16_t)` | protocol.h | 主机序→网络序 (2B) | 端口号、包体长度等 |
| `ntohl(uint32_t)` | protocol.h | 网络序→主机序 (4B) | 收到网络数据后还原为本地字节序 |
| `ntohs(uint16_t)` | protocol.h | 网络序→主机序 (2B) | 同上 |

```
文件: include/protocol.h

协议包头中的 body_len 字段使用网络字节序:
  发送方: hdr.body_len = htons(len);  // 主机 → 网络
  接收方: len = ntohs(hdr.body_len);  // 网络 → 主机

memcpy 直接复制结构体到缓冲区时, 多字节整数必须转换。
#pragma pack(push, 1) 确保结构体无填充, 8 字节精确匹配。
```

### 9. 时间函数 (logger.c, ratelimit.c, heartbeat.c, client.c)

| 函数 | 所在文件 | 作用 | 原理 |
|------|----------|------|------|
| `time(NULL)` | client.c, heartbeat.c | 获取秒级时间戳 | 返回自 1970-01-01 的秒数, 用于心跳/活跃时间 |
| `gettimeofday(&tv, NULL)` | logger.c, ratelimit.c | 获取微秒级时间 | 秒 + 微秒, 用于日志毫秒精度、令牌桶精确时间差 |
| `localtime_r(&tv_sec, &tm)` | logger.c | 秒数→本地时间结构体 | 线程安全版 localtime |
| `strftime(buf, size, fmt, &tm)` | logger.c | 格式化时间字符串 | 输出 "2026-06-13 15:16:31" 格式 |
| `sleep(5)` | heartbeat.c | 线程休眠 | 心跳线程每 5 秒检测一次 |

```
文件: src/ratelimit.c — 令牌桶时间计算 (微秒精度)
  double elapsed = (now.tv_sec  - last.tv_sec) +
                   (now.tv_usec - last.tv_usec) / 1000000.0;
  tokens += elapsed * rate;  // 按时间线性补充令牌

使用 gettimeofday 而非 time: time() 只有秒级精度,
令牌桶每秒补充 5 个, 0.2 秒就能补 1 个, 秒级精度不够。
```

### 10. 协议粘包拆包处理 (reactor.c)

```
文件: src/reactor.c — sub_try_assemble_packet()

TCP 是字节流协议, 没有消息边界, 会导致:
  - 粘包: 多条消息一起到达, 数据粘连 ["MSG1MSG2MSG3"]
  - 拆包: 一条消息分多次到达, 数据不完整 ["MSG1_PA"]

解决方案 (固定包头 + 长度字段):
  ┌──────────┬────────────────────────────────┐
  │ 8B 包头  │ Body (body_len 可变)            │
  │ 魔数+类型 │ 根据 body_len 确定截取长度      │
  │ +长度    │                                │
  └──────────┴────────────────────────────────┘

解析状态机:
  PARSE_STATE_HEADER: 缓冲区 ≥ 8 字节?
    → peek 8 字节 → 校验魔数 0x48494D53
    → 提取 body_len → skip 8 字节 → 进入 PARSE_STATE_BODY

  PARSE_STATE_BODY: 缓冲区 ≥ body_len?
    → read body_len 字节 → 提交到业务线程池
    → 回到 PARSE_STATE_HEADER

核心: 用 ringbuf 暂存不完整数据, 等到足够字节才处理,
      既不会丢数据也不会提前截断。
```

---

## 项目结构

```
chat/
├── Makefile
├── README.md
├── init_db.sql           # MySQL 数据库初始化脚本
├── include/
│   ├── common.h          # 通用宏、工具函数
│   ├── protocol.h        # 通信协议定义
│   ├── ringbuf.h         # 环形缓冲区
│   ├── threadpool.h      # 线程池
│   ├── reactor.h         # 主从 Reactor 模型
│   ├── client.h          # 客户端连接管理
│   ├── chat_service.h    # 聊天业务逻辑
│   ├── heartbeat.h       # 心跳超时检测
│   ├── ratelimit.h       # 刷屏限流
│   ├── logger.h          # 日志系统
│   └── db.h              # MySQL 数据库接口
└── src/
    ├── main.c            # 服务端入口
    ├── client_main.c     # 客户端程序
    ├── ringbuf.c         # 环形缓冲区实现
    ├── threadpool.c      # 线程池实现
    ├── reactor.c         # Reactor 模型实现
    ├── client.c          # 客户端管理实现
    ├── chat_service.c    # 业务逻辑实现
    ├── heartbeat.c       # 心跳检测实现
    ├── ratelimit.c       # 限流实现
    ├── logger.c          # 日志实现
    └── db.c              # MySQL 数据库实现
```

---

## 性能特点

- epoll ET + 非阻塞 IO, 单机可支持数千并发长连接
- 主从 Reactor 分离连接管理与数据收发, 充分利用多核
- 环形缓冲区零拷贝暂存, 减少内存分配
- 业务线程池隔离, IO 线程不被阻塞
- 线程安全的客户端管理和数据发送
- 令牌桶限流: 微秒级精度, 支持突发流量, 平滑控制速率
- MySQL 密码 SHA256 哈希存储, SQL 注入防护 (mysql_real_escape_string)
