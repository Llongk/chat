#include "ringbuf.h"

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

void ringbuf_destroy(ringbuf_t *rb)
{
    if (rb) {
        free(rb->buffer);
        free(rb);
    }
}

size_t ringbuf_readable(const ringbuf_t *rb)
{
    if (rb->write_pos >= rb->read_pos)
        return rb->write_pos - rb->read_pos;
    else
        return rb->capacity - rb->read_pos + rb->write_pos;
}

size_t ringbuf_writable(const ringbuf_t *rb)
{
    size_t readable = ringbuf_readable(rb);
    return rb->capacity - readable - 1;  /* 保留 1 字节防止读写指针重叠 */
}

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

size_t ringbuf_skip(ringbuf_t *rb, size_t len)
{
    size_t readable = ringbuf_readable(rb);
    if (len > readable) len = readable;
    if (len == 0) return 0;

    rb->read_pos = (rb->read_pos + len) % rb->capacity;
    return len;
}

void ringbuf_reset(ringbuf_t *rb)
{
    rb->read_pos  = 0;
    rb->write_pos = 0;
}
