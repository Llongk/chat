#ifndef DB_H
#define DB_H

#include "common.h"

/* 数据库初始化 (连接 MySQL, 创建表, 可选创建默认管理员) */
int  db_init(void);

/* 关闭数据库连接 */
void db_close(void);

/* 用户注册: 成功返回 0, 用户已存在返回 ERR_ALREADY_EXISTS, 其他错误返回 -1 */
int  db_user_register(const char *username, const char *password);

/* 用户登录验证: 成功返回 0 并设置 is_admin/status, 密码错误返回 -1, 用户不存在返回 ERR_NOT_FOUND, 被封禁返回 -2 */
int  db_user_login(const char *username, const char *password,
                   int *is_admin, int *status);

/* 更新用户最后登录时间 */
void db_user_update_login(const char *username);

/* 检查用户是否存在 */
int  db_user_exists(const char *username);

/* 管理员踢人 (标记用户离线, 由 Reactor 清理) */
int  db_user_get_status(const char *username, int *status, int *is_admin);

/* 管理员封禁/解封用户 */
int  db_user_set_ban(const char *username, int banned);

#endif /* DB_H */
