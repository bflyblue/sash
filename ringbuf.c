/*
 * ringbuf.c - Ring buffer data structure
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ringbuf.h"

void ringbuf_init(RingBuf *rb, size_t cap) {
  rb->lines = calloc(cap, sizeof(char *));
  rb->lengths = calloc(cap, sizeof(size_t));
  rb->capacity = cap;
  rb->head = 0;
  rb->count = 0;
  if (!rb->lines || !rb->lengths) {
    perror("sash: calloc");
    exit(1);
  }
}

void ringbuf_push(RingBuf *rb, const char *line, size_t len) {
  size_t slot;
  if (rb->count < rb->capacity) {
    slot = (rb->head + rb->count) % rb->capacity;
    rb->count++;
  } else {
    slot = rb->head;
    rb->head = (rb->head + 1) % rb->capacity;
    free(rb->lines[slot]);
  }
  rb->lines[slot] = strndup(line, len);
  if (!rb->lines[slot]) {
    rb->lengths[slot] = 0;
    return;
  }
  rb->lengths[slot] = len;
}

const char *ringbuf_get(const RingBuf *rb, size_t i, size_t *len) {
  if (i >= rb->count) {
    *len = 0;
    return "";
  }
  size_t idx = (rb->head + i) % rb->capacity;
  *len = rb->lengths[idx];
  return rb->lines[idx];
}

void ringbuf_free(RingBuf *rb) {
  for (size_t i = 0; i < rb->capacity; i++)
    free(rb->lines[i]);
  free(rb->lines);
  free(rb->lengths);
}
