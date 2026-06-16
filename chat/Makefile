CC       = gcc
CFLAGS   = -Wall -Wextra -g -O2
INCLUDES = -Iinclude -I/home/qhl/code/chat/mysql_dev/usr/include
LDFLAGS  = -lpthread -lssl -lcrypto -L/home/qhl/code/chat/mysql_dev/usr/lib/x86_64-linux-gnu -lmysqlclient -Wl,-rpath,/home/qhl/code/chat/mysql_dev/usr/lib/x86_64-linux-gnu

SERVER   = chat_server
CLIENT   = chat_client

SRC_DIR  = src
OBJ_DIR  = obj

# 服务端源文件 (排除 client_main.c)
SERVER_SRCS = $(filter-out $(SRC_DIR)/client_main.c, $(wildcard $(SRC_DIR)/*.c))
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SERVER_SRCS))

# 客户端源文件
CLIENT_SRCS = $(SRC_DIR)/client_main.c
CLIENT_OBJS = $(OBJ_DIR)/client_main.o

.PHONY: all server client clean run help setup-db

# ============================================
# 默认目标: 编译服务端 + 客户端
# ============================================
all: $(OBJ_DIR) $(SERVER) $(CLIENT)

server: $(OBJ_DIR) $(SERVER)

client: $(OBJ_DIR) $(CLIENT)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(SERVER): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(SERVER)"

$(CLIENT): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(CLIENT)"

# 头文件列表 (修改任意头文件都会触发重编)
HEADERS  = $(wildcard include/*.h)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(SERVER) $(CLIENT)
	@echo "Clean complete"

# ============================================
# 初始化 MySQL 数据库 (需要 root 权限)
# ============================================
setup-db:
	@echo "========================================"
	@echo "  初始化 MySQL 数据库"
	@echo "  默认管理员: admin / admin123"
	@echo "========================================"
	@echo ""
	@echo "方式一: 使用 SQL 脚本 (推荐)"
	@echo "  sudo mysql < init_db.sql"
	@echo ""
	@echo "方式二: 直接启动服务端 (自动建库建表)"
	@echo "  make run"
	@echo ""
	@echo "方式三: 手动创建"
	@echo "  mysql -u root -p -e \"CREATE DATABASE IF NOT EXISTS chat_db DEFAULT CHARACTER SET utf8mb4;\""
	@echo "  mysql -u root -p chat_db -e \""
	@echo "    CREATE TABLE IF NOT EXISTS users ("
	@echo "      id INT AUTO_INCREMENT PRIMARY KEY,"
	@echo "      username VARCHAR(32) NOT NULL UNIQUE,"
	@echo "      password_hash VARCHAR(128) NOT NULL,"
	@echo "      is_admin TINYINT DEFAULT 0,"
	@echo "      status TINYINT DEFAULT 1,"
	@echo "      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
	@echo "      last_login TIMESTAMP NULL"
	@echo "    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\""
	@echo ""

# ============================================
# 运行服务端
# ============================================
# 默认端口 8888, 可用 ARGS 覆盖
#   make run                        → 启动在 8888 端口
#   make run ARGS='-p 9999'         → 启动在 9999 端口
#   make run ARGS='-p 8888 -s 8'    → 启动在 8888 端口, 8 个子 Reactor
# ============================================
run: $(SERVER)
	@echo "========================================"
	@echo "  启动聊天服务器..."
	@echo "  默认管理员: admin / admin123"
	@echo "========================================"
	./$(SERVER) $(ARGS)

# ============================================
# 运行客户端
# ============================================
#   make client-run ARGS='127.0.0.1 8888'  → 连接到本机 8888
# ============================================
client-run: $(CLIENT)
	./$(CLIENT) $(ARGS)

# ============================================
# 帮助信息
# ============================================
help:
	@echo ""
	@echo "╔══════════════════════════════════════════════════════╗"
	@echo "║    高并发 IM 聊天室 — MySQL + AI 助手              ║"
	@echo "╚══════════════════════════════════════════════════════╝"
	@echo ""
	@echo "【快速启动】"
	@echo "  1. 确保 MySQL 已运行"
	@echo "  2. make setup-db     — 查看数据库初始化方式"
	@echo "  3. make run          — 启动服务端 (自动建库建表)"
	@echo "  4. make client-run ARGS='127.0.0.1 8888' — 启动客户端"
	@echo ""
	@echo "【构建目标】"
	@echo "  make                — 编译服务端 + 客户端"
	@echo "  make server         — 只编译服务端"
	@echo "  make client         — 只编译客户端"
	@echo "  make clean          — 清理编译产物"
	@echo ""
	@echo "【运行目标】"
	@echo "  make run                           — 运行服务端 (端口 8888)"
	@echo "  make run ARGS='-p 9999 -s 8'       — 指定端口/子Reactor数"
	@echo "  make client-run ARGS='127.0.0.1 8888' — 运行客户端"
	@echo ""
	@echo "【数据库】"
	@echo "  make setup-db                      — 查看数据库初始化方法"
	@echo "  sudo mysql < init_db.sql           — 手动执行 SQL 初始化"
	@echo ""
	@echo "【客户端命令 (登录后)】"
	@echo "  /register <用户名> <密码>  注册新账号"
	@echo "  /login    <用户名> <密码>  登录"
	@echo "  /msg      <消息>           公屏发言"
	@echo "  /to       <用户> <消息>    私聊"
	@echo "  /to       AI <消息>        和 AI 助手小D 对话"
	@echo "  /online                    查看在线成员"
	@echo "  /kick     <用户名>         管理员踢人"
	@echo "  /quit                      退出"
	@echo ""
	@echo "【默认管理员账号】"
	@echo "  用户名: admin    密码: admin123"
	@echo "  首次启动服务端时自动创建, 或通过 init_db.sql 手动创建"
	@echo ""
	@echo "【服务端参数】"
	@echo "  -p <port>      监听端口 (默认: 8888)"
	@echo "  -s <num>       子 Reactor 数量 (默认: 4)"
	@echo "  -t <num>       业务线程数 (默认: 4)"
	@echo "  -k <key>       DeepSeek API Key (启用 AI 助手)"
	@echo "  -l <file>      日志文件路径 (默认: chat_server.log)"
	@echo ""
	@echo "【启动示例 (含 AI)】"
	@echo "  make run ARGS='-k sk-your-deepseek-api-key'"
	@echo ""
