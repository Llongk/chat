-- ============================================
-- 高并发 IM 聊天室 - MySQL 数据库初始化脚本
-- ============================================
-- 使用方法:
--   sudo mysql < init_db.sql
--   或者:
--   mysql -u root -p < init_db.sql
-- ============================================

-- 创建数据库
CREATE DATABASE IF NOT EXISTS chat_db
  DEFAULT CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE chat_db;

-- 创建用户表
CREATE TABLE IF NOT EXISTS users (
    id            INT AUTO_INCREMENT PRIMARY KEY,
    username      VARCHAR(32)   NOT NULL UNIQUE,
    password_hash VARCHAR(128)  NOT NULL,
    is_admin      TINYINT       DEFAULT 0,
    status        TINYINT       DEFAULT 1  COMMENT '1=正常, 0=封禁',
    created_at    TIMESTAMP     DEFAULT CURRENT_TIMESTAMP,
    last_login    TIMESTAMP     NULL DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 创建默认管理员账号 (用户名: admin, 密码: admin123)
-- 密码哈希 = SHA2(CONCAT(username, ':', password), 256)
INSERT INTO users (username, password_hash, is_admin, status)
VALUES ('admin', SHA2(CONCAT('admin', ':', 'admin123'), 256), 1, 1)
ON DUPLICATE KEY UPDATE username = username;

-- 验证
SELECT '初始化完成!' AS status;
SELECT id, username, is_admin, status, created_at FROM users;
