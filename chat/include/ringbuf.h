#ifndef RINGBUF_H
#define RINGBUF_H

#include "common.h"

/* 环形缓冲区结构体 */
typedef struct {
    uint8_t *buffer;      /* 缓冲区内存 */
    size_t   capacity;    /* 总容量 */
    size_t   read_pos;    /* 读指针 */
    size_t   write_pos;   /* 写指针 */
} ringbuf_t;

/* 创建环形缓冲区 */
ringbuf_t *ringbuf_create(size_t capacity);

/* 销毁环形缓冲区 */
void ringbuf_destroy(ringbuf_t *rb);

/* 获取可读字节数 */
size_t ringbuf_readable(const ringbuf_t *rb);

/* 获取可写字节数 */
size_t ringbuf_writable(const ringbuf_t *rb);

/* 向环形缓冲区写入数据, 返回实际写入字节数 */
size_t ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len);

/* 从环形缓冲区读取数据, 返回实际读取字节数 */
size_t ringbuf_read(ringbuf_t *rb, uint8_t *buf, size_t len);

/* 窥探数据但不消费, 返回实际窥探字节数 */
size_t ringbuf_peek(const ringbuf_t *rb, uint8_t *buf, size_t len);

/* 跳过(消费)指定字节数, 返回实际跳过字节数 */
size_t ringbuf_skip(ringbuf_t *rb, size_t len);

/* 重置缓冲区 (清空) */
void ringbuf_reset(ringbuf_t *rb);

#endif /* RINGBUF_H */
