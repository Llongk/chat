#include "ringbuf.h"

/* 创建环形缓冲区: 分配指定容量的内存, 初始化读写指针 */
ringbuf_t *ringbuf_create(size_t capacity)
{
    ringbuf_t *rb = (ringbuf_t *)malloc(sizeof(ringbuf_t));
    if (!rb) return NULL;

    rb->buffer = (uint8_t *)malloc(capacity);
    if (!rb->buffer) {
        free(rb);
        return NULL;
    }

    rb->capacity  = capacity;
    rb->read_pos  = 0;
    rb->write_pos = 0;
    return rb;
}

/* 销毁环形缓冲区: 释放内部buffer和结构体本身 */
void ringbuf_destroy(ringbuf_t *rb)
{
    if (rb) {
        free(rb->buffer);
        free(rb);
    }
}

/* 计算可读数据量 (写指针和读指针之间的数据) */
size_t ringbuf_readable(const ringbuf_t *rb)
{
    if (rb->write_pos >= rb->read_pos)
        return rb->write_pos - rb->read_pos;
    else
        return rb->capacity - rb->read_pos + rb->write_pos;
}

/* 计算可写入空间 (保留1字节防止读写指针重叠) */
size_t ringbuf_writable(const ringbuf_t *rb)
{
    size_t readable = ringbuf_readable(rb);
    return rb->capacity - readable - 1;  /* 保留 1 字节防止读写指针重叠 */
}

/* 写入数据到环形缓冲区, 处理环形拷贝 (返回实际写入量) */
size_t ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    size_t writable = ringbuf_writable(rb);
    if (len > writable) len = writable;
    if (len == 0) return 0;

    size_t first_part = rb->capacity - rb->write_pos;
    if (len <= first_part) {
        memcpy(rb->buffer + rb->write_pos, data, len);
        rb->write_pos = (rb->write_pos + len) % rb->capacity;
    } else {
        memcpy(rb->buffer + rb->write_pos, data, first_part);
        memcpy(rb->buffer, data + first_part, len - first_part);
        rb->write_pos = len - first_part;
    }
    return len;
}

/* 读取数据并移动读指针, 处理环形拷贝 */
size_t ringbuf_read(ringbuf_t *rb, uint8_t *buf, size_t len)
{
    size_t readable = ringbuf_readable(rb);
    if (len > readable) len = readable;
    if (len == 0) return 0;

    size_t first_part = rb->capacity - rb->read_pos;
    if (len <= first_part) {
        memcpy(buf, rb->buffer + rb->read_pos, len);
        rb->read_pos = (rb->read_pos + len) % rb->capacity;
    } else {
        memcpy(buf, rb->buffer + rb->read_pos, first_part);
        memcpy(buf + first_part, rb->buffer, len - first_part);
        rb->read_pos = len - first_part;
    }
    return len;
}

/* 窥探数据但不移动读指针 (用于解析包头后判断是否完整) */
size_t ringbuf_peek(const ringbuf_t *rb, uint8_t *buf, size_t len)
{
    size_t readable = ringbuf_readable(rb);
    if (len > readable) len = readable;
    if (len == 0) return 0;

    size_t first_part = rb->capacity - rb->read_pos;
    if (len <= first_part) {
        memcpy(buf, rb->buffer + rb->read_pos, len);
    } else {
        memcpy(buf, rb->buffer + rb->read_pos, first_part);
        memcpy(buf + first_part, rb->buffer, len - first_part);
    }
    return len;
}

/* 跳过指定长度数据, 只移动读指针不拷贝 */
size_t ringbuf_skip(ringbuf_t *rb, size_t len)
{
    size_t readable = ringbuf_readable(rb);
    if (len > readable) len = readable;
    if (len == 0) return 0;

    rb->read_pos = (rb->read_pos + len) % rb->capacity;
    return len;
}

/* 重置读写指针到初始位置 (清空缓冲区) */
void ringbuf_reset(ringbuf_t *rb)
{
    rb->read_pos  = 0;
    rb->write_pos = 0;
}
