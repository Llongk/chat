#include "db.h"
#include "logger.h"
#include <mysql/mysql.h>

static MYSQL *g_db = NULL;

/* 安全的 SQL 字符串转义 (防止 SQL 注入) */
static void db_escape_string(const char *src, char *dst, size_t dst_size)
{
    if (!g_db || !src || !dst || dst_size == 0) {
        if (dst && dst_size > 0) dst[0] = '\0';
        return;
    }
    mysql_real_escape_string(g_db, dst, src, strlen(src));
    /* 安全截断 */
    if (strlen(dst) >= dst_size) {
        dst[dst_size - 1] = '\0';
    }
}

int db_init(void)
{
    /* 初始化 MySQL 客户端库 */
    if (mysql_library_init(0, NULL, NULL) != 0) {
        LOG_ERROR("Failed to initialize MySQL library");
        return -1;
    }

    g_db = mysql_init(NULL);
    if (!g_db) {
        LOG_ERROR("mysql_init() failed");
        mysql_library_end();
        return -1;
    }

    /* 设置字符集 */
    mysql_options(g_db, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    /* 连接数据库 */
    if (!mysql_real_connect(g_db, DB_HOST, DB_USER, DB_PASS,
                            NULL, DB_PORT, NULL, 0)) {
        LOG_ERROR("MySQL connect failed: %s", mysql_error(g_db));
        mysql_close(g_db);
        g_db = NULL;
        mysql_library_end();
        return -1;
    }
    LOG_INFO("MySQL connected: %s@%s:%d", DB_USER, DB_HOST, DB_PORT);

    /* 创建数据库 (如果不存在) */
    char create_db_sql[256];
    snprintf(create_db_sql, sizeof(create_db_sql),
             "CREATE DATABASE IF NOT EXISTS `%s` "
             "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci",
             DB_NAME);

    if (mysql_query(g_db, create_db_sql) != 0) {
        LOG_ERROR("Failed to create database: %s", mysql_error(g_db));
        mysql_close(g_db);
        g_db = NULL;
        mysql_library_end();
        return -1;
    }

    /* 选择数据库 */
    if (mysql_select_db(g_db, DB_NAME) != 0) {
        LOG_ERROR("Failed to select database '%s': %s",
                  DB_NAME, mysql_error(g_db));
        mysql_close(g_db);
        g_db = NULL;
        mysql_library_end();
        return -1;
    }
    LOG_INFO("Database '%s' selected", DB_NAME);

    /* 创建用户表 */
    const char *create_tbl_sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id          INT AUTO_INCREMENT PRIMARY KEY,"
        "  username    VARCHAR(32)  NOT NULL UNIQUE,"
        "  password_hash VARCHAR(128) NOT NULL,"
        "  is_admin    TINYINT      DEFAULT 0,"
        "  status      TINYINT      DEFAULT 1,"
        "  created_at  TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,"
        "  last_login  TIMESTAMP    NULL DEFAULT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (mysql_query(g_db, create_tbl_sql) != 0) {
        LOG_ERROR("Failed to create users table: %s", mysql_error(g_db));
        mysql_close(g_db);
        g_db = NULL;
        mysql_library_end();
        return -1;
    }
    LOG_INFO("Users table ready");

    /* 检查是否需要创建默认管理员 */
    if (mysql_query(g_db, "SELECT COUNT(*) FROM users") == 0) {
        MYSQL_RES *res = mysql_store_result(g_db);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && atoi(row[0]) == 0) {
                mysql_free_result(res);

                /* 创建默认管理员账号 */
                char admin_sql[512];
                char esc_user[96], esc_pass[96];
                db_escape_string(DEFAULT_ADMIN_USER, esc_user, sizeof(esc_user));
                db_escape_string(DEFAULT_ADMIN_PASS, esc_pass, sizeof(esc_pass));

                snprintf(admin_sql, sizeof(admin_sql),
                         "INSERT INTO users (username, password_hash, is_admin, status) "
                         "VALUES ('%s', SHA2(CONCAT('%s', ':', '%s'), 256), 1, 1)",
                         esc_user, esc_user, esc_pass);

                if (mysql_query(g_db, admin_sql) == 0) {
                    LOG_INFO("Default admin '%s' created (password: '%s')",
                             DEFAULT_ADMIN_USER, DEFAULT_ADMIN_PASS);
                } else {
                    LOG_WARN("Failed to create default admin: %s", mysql_error(g_db));
                }
            } else {
                mysql_free_result(res);
            }
        }
    }

    LOG_INFO("Database initialization complete");
    return 0;
}

void db_close(void)
{
    if (g_db) {
        mysql_close(g_db);
        g_db = NULL;
        LOG_INFO("MySQL connection closed");
    }
    mysql_library_end();
}

int db_user_register(const char *username, const char *password)
{
    if (!g_db || !username || !password) return -1;

    /* 检查用户名是否已存在 */
    if (db_user_exists(username)) {
        return ERR_ALREADY_EXISTS;
    }

    /* 转义用户输入 */
    char esc_user[96], esc_pass[96];
    db_escape_string(username, esc_user, sizeof(esc_user));
    db_escape_string(password, esc_pass, sizeof(esc_pass));

    /* 插入新用户: 密码哈希 = SHA2(CONCAT(username, ':', password), 256) */
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO users (username, password_hash, is_admin, status) "
             "VALUES ('%s', SHA2(CONCAT('%s', ':', '%s'), 256), 0, 1)",
             esc_user, esc_user, esc_pass);

    if (mysql_query(g_db, sql) != 0) {
        LOG_ERROR("Register user '%s' failed: %s", username, mysql_error(g_db));
        return -1;
    }

    LOG_INFO("User registered: %s", username);
    return 0;
}

int db_user_login(const char *username, const char *password,
                  int *is_admin, int *status)
{
    if (!g_db || !username || !password) return -1;

    /* 转义用户输入 */
    char esc_user[96], esc_pass[96];
    db_escape_string(username, esc_user, sizeof(esc_user));
    db_escape_string(password, esc_pass, sizeof(esc_pass));

    /* 查询用户: 验证密码哈希 */
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id, is_admin, status FROM users "
             "WHERE username = '%s' "
             "AND password_hash = SHA2(CONCAT('%s', ':', '%s'), 256)",
             esc_user, esc_user, esc_pass);

    if (mysql_query(g_db, sql) != 0) {
        LOG_ERROR("Login query failed: %s", mysql_error(g_db));
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(g_db);
    if (!res) return -1;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        LOG_WARN("Login query returned no rows, sql: %s", sql);
        mysql_free_result(res);

        /* 用户不存在或密码错误: 区分两种情况 */
        char check_sql[256];
        snprintf(check_sql, sizeof(check_sql),
                 "SELECT id FROM users WHERE username = '%s'", esc_user);

        if (mysql_query(g_db, check_sql) == 0) {
            MYSQL_RES *check_res = mysql_store_result(g_db);
            if (check_res) {
                MYSQL_ROW check_row = mysql_fetch_row(check_res);
                if (check_row) {
                    mysql_free_result(check_res);
                    return -1;  /* 用户名存在但密码错误 */
                }
                mysql_free_result(check_res);
            }
        }

        return ERR_NOT_FOUND;  /* 用户名不存在 */
    }

    int admin_flag = atoi(row[1]);
    int user_status = atoi(row[2]);

    if (is_admin) *is_admin = admin_flag;
    if (status) *status = user_status;

    mysql_free_result(res);

    /* 检查用户状态 */
    if (user_status == 0) {
        return -2;  /* 账号已被封禁 */
    }

    LOG_INFO("User login: %s (admin=%d, status=%d)",
             username, admin_flag, user_status);
    return 0;
}

void db_user_update_login(const char *username)
{
    if (!g_db || !username) return;

    char esc_user[96];
    db_escape_string(username, esc_user, sizeof(esc_user));

    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE users SET last_login = NOW() WHERE username = '%s'",
             esc_user);

    if (mysql_query(g_db, sql) != 0) {
        LOG_WARN("Update last_login for '%s' failed: %s",
                 username, mysql_error(g_db));
    }
}

int db_user_exists(const char *username)
{
    if (!g_db || !username) return 0;

    char esc_user[96];
    db_escape_string(username, esc_user, sizeof(esc_user));

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT 1 FROM users WHERE username = '%s'", esc_user);

    if (mysql_query(g_db, sql) != 0) return 0;

    MYSQL_RES *res = mysql_store_result(g_db);
    if (!res) return 0;

    int exists = (mysql_fetch_row(res) != NULL) ? 1 : 0;
    mysql_free_result(res);
    return exists;
}

int db_user_get_status(const char *username, int *status, int *is_admin)
{
    if (!g_db || !username) return -1;

    char esc_user[96];
    db_escape_string(username, esc_user, sizeof(esc_user));

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT status, is_admin FROM users WHERE username = '%s'",
             esc_user);

    if (mysql_query(g_db, sql) != 0) return -1;

    MYSQL_RES *res = mysql_store_result(g_db);
    if (!res) return -1;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return ERR_NOT_FOUND;
    }

    if (status) *status = atoi(row[0]);
    if (is_admin) *is_admin = atoi(row[1]);

    mysql_free_result(res);
    return 0;
}

int db_user_set_ban(const char *username, int banned)
{
    if (!g_db || !username) return -1;

    char esc_user[96];
    db_escape_string(username, esc_user, sizeof(esc_user));

    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE users SET status = %d WHERE username = '%s'",
             banned ? 0 : 1, esc_user);

    if (mysql_query(g_db, sql) != 0) {
        LOG_ERROR("Set ban status for '%s' failed: %s",
                  username, mysql_error(g_db));
        return -1;
    }

    if (mysql_affected_rows(g_db) == 0) {
        return ERR_NOT_FOUND;
    }

    LOG_INFO("User '%s' %s", username, banned ? "banned" : "unbanned");
    return 0;
}
