/**
 * AI 服务模块 - 通过 DeepSeek API 实现聊天室 AI 助手
 *
 * 原理: 使用 popen() 调用 curl 命令行发送 HTTP 请求,
 *       解析返回的 JSON 提取 AI 回复内容。
 *       无需 libcurl 开发库, 只需系统装有 curl 即可。
 */

#include "ai_service.h"
#include "logger.h"
#include <ctype.h>

/* 全局 API Key (初始化时设置) */
static char g_api_key[128] = "";
static int  g_ready = 0;

/* ══════════════════════════════════════════
 *  内部工具函数
 * ══════════════════════════════════════════ */

/* 将字符串转义为 JSON 安全格式 (处理引号、换行、反斜杠等) */
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 2; si++) {
        switch (src[si]) {
        case '"':  dst[di++] = '\\'; dst[di++] = '"';  break;
        case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
        case '\n': dst[di++] = '\\'; dst[di++] = 'n';  break;
        case '\r': dst[di++] = '\\'; dst[di++] = 'r';  break;
        case '\t': dst[di++] = '\\'; dst[di++] = 't';  break;
        default:
            if ((unsigned char)src[si] < 0x20) {
                /* 跳过其他控制字符 */
            } else {
                dst[di++] = src[si];
            }
            break;
        }
    }
    dst[di] = '\0';
}

/* 从 DeepSeek API 响应 JSON 中提取回复内容
 * 响应格式: {"choices":[{"message":{"content":"AI回复内容"}}]} */
static int extract_reply(const char *json, char *reply, size_t reply_size)
{
    /* 查找 "content":" 字段 */
    const char *key = "\"content\":\"";
    const char *p = strstr(json, key);
    if (!p) return -1;

    p += strlen(key);

    /* 提取到下一个未转义的引号 */
    size_t ri = 0;
    while (*p && ri < reply_size - 1) {
        if (*p == '\\' && *(p + 1)) {
            /* 处理转义序列 */
            p++;
            switch (*p) {
            case 'n':  reply[ri++] = '\n'; break;
            case 'r':  reply[ri++] = '\r'; break;
            case 't':  reply[ri++] = '\t'; break;
            case '"':  reply[ri++] = '"';  break;
            case '\\': reply[ri++] = '\\'; break;
            default:   reply[ri++] = *p;   break;
            }
            p++;
        } else if (*p == '"') {
            break;  /* 结束引号 */
        } else {
            reply[ri++] = *p++;
        }
    }
    reply[ri] = '\0';
    return (ri > 0) ? 0 : -1;
}

/* ══════════════════════════════════════════
 *  公共接口
 * ══════════════════════════════════════════ */

/* 初始化 AI 服务: 保存 API Key, 检查 curl 是否可用 */
int ai_init(const char *api_key)
{
    if (!api_key || strlen(api_key) == 0) {
        LOG_WARN("AI service: no API key provided, AI disabled");
        return -1;
    }

    safe_strncpy(g_api_key, api_key, sizeof(g_api_key));

    /* 检查 curl 命令是否可用 */
    FILE *fp = popen("which curl 2>/dev/null", "r");
    if (!fp) {
        LOG_ERROR("AI service: cannot check curl availability");
        return -1;
    }
    char buf[64] = "";
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        LOG_ERROR("AI service: curl not found in PATH");
        return -1;
    }
    pclose(fp);

    g_ready = 1;
    LOG_INFO("AI service initialized (DeepSeek, model=%s)", AI_MODEL);
    return 0;
}

/* 向 DeepSeek API 发送消息并获取回复 */
int ai_chat(const char *user_msg, char *reply, size_t reply_size)
{
    if (!g_ready || !user_msg || !reply || reply_size == 0)
        return -1;

    reply[0] = '\0';

    /* 转义用户消息和系统提示词为 JSON 安全字符串 */
    char escaped_msg[2048];
    char escaped_sys[512];
    json_escape(user_msg, escaped_msg, sizeof(escaped_msg));
    json_escape(AI_SYSTEM_PROMPT, escaped_sys, sizeof(escaped_sys));

    /* 构建 curl 命令: POST 请求到 DeepSeek API */
    char curl_cmd[4096];
    snprintf(curl_cmd, sizeof(curl_cmd),
        "curl -s -m %d -X POST '%s' "
        "-H 'Content-Type: application/json' "
        "-H 'Authorization: Bearer %s' "
        "-d '{\"model\":\"%s\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}"
        "],\"max_tokens\":1024}' 2>/dev/null",
        AI_TIMEOUT_SEC, AI_API_URL, g_api_key,
        AI_MODEL, escaped_sys, escaped_msg);

    /* 执行 curl 命令并读取响应 */
    FILE *fp = popen(curl_cmd, "r");
    if (!fp) {
        LOG_ERROR("AI service: popen failed");
        snprintf(reply, reply_size, "AI 服务暂时不可用");
        return -1;
    }

    /* 读取完整响应 (最大 8KB) */
    char response[8192] = "";
    size_t total = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        if (total + n < sizeof(response) - 1) {
            memcpy(response + total, buf, n);
            total += n;
        }
    }
    response[total] = '\0';

    int status = pclose(fp);

    if (status != 0 || total == 0) {
        LOG_WARN("AI service: curl failed (status=%d, len=%zu)", status, total);
        snprintf(reply, reply_size, "AI 请求超时或网络异常");
        return -1;
    }

    /* 检查是否有错误响应 */
    if (strstr(response, "\"error\"")) {
        LOG_WARN("AI service: API error: %.200s", response);
        snprintf(reply, reply_size, "AI 返回错误，请检查 API Key");
        return -1;
    }

    /* 从 JSON 响应中提取回复内容 */
    if (extract_reply(response, reply, reply_size) != 0) {
        LOG_WARN("AI service: cannot parse response: %.200s", response);
        snprintf(reply, reply_size, "AI 回复解析失败");
        return -1;
    }

    LOG_INFO("AI chat: Q=%s A=%.100s", user_msg, reply);
    return 0;
}

/* 检查 AI 服务是否已初始化 */
int ai_is_ready(void)
{
    return g_ready;
}
