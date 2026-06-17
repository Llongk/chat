#ifndef AI_SERVICE_H
#define AI_SERVICE_H

#include "common.h"

/* DeepSeek API 配置 */
#define AI_API_URL      "https://api.deepseek.com/chat/completions"
#define AI_MODEL        "deepseek-chat"
#define AI_MAX_REPLY    8192    /* AI 回复最大长度 */
#define AI_TIMEOUT_SEC  60      /* API 请求超时(秒) */

/* 系统提示词 (定义 AI 的角色和行为) */
#define AI_SYSTEM_PROMPT \
    "你是一个友好的聊天室助手，名字叫小D。" \
    "你在这个IM聊天室里帮助用户回答问题。" \
    "回答要简洁明了。" \
    "如果用户问编程相关问题，给出完整的代码示例，不要截断代码。"

/* 初始化 AI 服务: 设置 API Key */
int  ai_init(const char *api_key);

/* 向 AI 发送消息并获取回复 (返回 0 成功, -1 失败) */
int  ai_chat(const char *user_msg, char *reply, size_t reply_size);

/* 获取 AI 是否已初始化 */
int  ai_is_ready(void);

#endif /* AI_SERVICE_H */
