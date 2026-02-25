/*
 * ringbuf.h - Ring buffer data structure
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>

typedef struct {
    char  **lines;
    size_t *lengths;
    size_t  capacity;
    size_t  head;
    size_t  count;
} RingBuf;

void ringbuf_init(RingBuf *rb, size_t cap);
void ringbuf_push(RingBuf *rb, const char *line, size_t len);
const char *ringbuf_get(const RingBuf *rb, size_t i, size_t *len);
void ringbuf_free(RingBuf *rb);

#endif /* RINGBUF_H */
