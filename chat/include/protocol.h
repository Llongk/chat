#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"

/* 消息类型定义 */
enum msg_type {
    MSG_LOGIN_REQ        = 0x01,  /* 登录请求 */
    MSG_LOGIN_RESP       = 0x02,  /* 登录响应 */
    MSG_PUBLIC_MSG       = 0x03,  /* 公屏消息 */
    MSG_PUBLIC_BROADCAST = 0x04,  /* 公屏广播 */
    MSG_PRIVATE_REQ      = 0x05,  /* 私聊请求 */
    MSG_PRIVATE_RESP     = 0x06,  /* 私聊响应 */
    MSG_ONLINE_LIST_REQ  = 0x07,  /* 在线列表请求 */
    MSG_ONLINE_LIST_RESP = 0x08,  /* 在线列表响应 */
    MSG_HEARTBEAT_REQ    = 0x09,  /* 心跳请求 */
    MSG_HEARTBEAT_RESP   = 0x0A,  /* 心跳响应 */
    MSG_REGISTER_REQ     = 0x0B,  /* 注册请求 */
    MSG_REGISTER_RESP    = 0x0C,  /* 注册响应 */
    MSG_KICK_REQ         = 0x0D,  /* 管理员踢人请求 */
    MSG_KICK_RESP        = 0x0E,  /* 管理员踢人响应 */
    MSG_BAN_REQ          = 0x0F,  /* 管理员封禁请求 */
    MSG_BAN_RESP         = 0x10,  /* 管理员封禁响应 */
    MSG_ERROR_RESP       = 0xFF   /* 错误响应 */
};

/* 包头结构体 (8字节, 紧凑排列) */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;       /* 魔术字 0x48494D53 */
    uint8_t  type;        /* 消息类型 */
    uint8_t  flags;       /* 标志位 (预留) */
    uint16_t body_len;    /* 包体长度 (网络字节序) */
} proto_header_t;
#pragma pack(pop)

/* 协议解析状态 */
typedef enum {
    PARSE_STATE_HEADER = 0,  /* 等待解析包头 */
    PARSE_STATE_BODY   = 1   /* 等待解析包体 */
} parse_state_t;

/* 每个客户端连接的协议解析上下文 */
typedef struct {
    parse_state_t state;
    proto_header_t header;
    uint16_t      body_read;  /* 已读取的包体字节数 */
} proto_ctx_t;

/* 协议工具函数 */
static inline void proto_header_hton(proto_header_t *hdr)
{
    hdr->magic    = htonl(hdr->magic);
    hdr->body_len = htons(hdr->body_len);
}

static inline void proto_header_ntoh(proto_header_t *hdr)
{
    hdr->magic    = ntohl(hdr->magic);
    hdr->body_len = ntohs(hdr->body_len);
}

/* 构建协议包头 (存储主机序, 由 proto_serialize_header 统一做网络序转换) */
static inline void proto_build_header(proto_header_t *hdr, uint8_t type,
                                       uint16_t body_len)
{
    hdr->magic    = PROTO_MAGIC;     /* 主机序 */
    hdr->type     = type;
    hdr->flags    = 0;
    hdr->body_len = body_len;        /* 主机序 */
}

/* 校验包头 (hdr 已经过 proto_deserialize_header 转为主机序) */
static inline int proto_validate_header(const proto_header_t *hdr)
{
    return (hdr->magic == PROTO_MAGIC);
}

/* 序列化包头到缓冲区 (调用者确保 buf 至少 8 字节) */
static inline void proto_serialize_header(uint8_t *buf, const proto_header_t *hdr)
{
    proto_header_t tmp = *hdr;
    proto_header_hton(&tmp);
    memcpy(buf, &tmp, PROTO_HEADER_LEN);
}

/* 从缓冲区反序列化包头 */
static inline void proto_deserialize_header(const uint8_t *buf, proto_header_t *hdr)
{
    memcpy(hdr, buf, PROTO_HEADER_LEN);
    proto_header_ntoh(hdr);
}

#endif /* PROTOCOL_H */
