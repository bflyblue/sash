/*
 * test_ringbuf.c - Unit tests for ring buffer
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>

#include "../ringbuf.h"
#include "../ringbuf.c"

/* ── Test harness ────────────────────────────────────────────────── */

static int pass_count = 0;
static int fail_count = 0;

static void pass(const char *desc)
{
    printf("  PASS: %s\n", desc);
    pass_count++;
}

static void fail(const char *desc)
{
    printf("  FAIL: %s\n", desc);
    fail_count++;
}

static void assert_eq_str(const char *desc,
                          const char *expected, size_t expected_len,
                          const char *actual, size_t actual_len)
{
    if (actual_len == expected_len &&
        memcmp(actual, expected, expected_len) == 0) {
        pass(desc);
    } else {
        fail(desc);
        printf("    expected (%zu): \"%.*s\"\n",
               expected_len, (int)expected_len, expected);
        printf("    got      (%zu): \"%.*s\"\n",
               actual_len, (int)actual_len, actual);
    }
}

static void assert_eq_size(const char *desc, size_t expected, size_t actual)
{
    if (expected == actual) {
        pass(desc);
    } else {
        fail(desc);
        printf("    expected: %zu, got: %zu\n", expected, actual);
    }
}

/* ── Tests ───────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== ringbuf unit tests ===\n\n");

    /* -- Init -- */
    {
        RingBuf rb;
        ringbuf_init(&rb, 3);
        assert_eq_size("init: count is 0", 0, rb.count);
        assert_eq_size("init: capacity is 3", 3, rb.capacity);
        ringbuf_free(&rb);
    }

    /* -- Push and get -- */
    {
        RingBuf rb;
        ringbuf_init(&rb, 3);

        ringbuf_push(&rb, "aaa", 3);
        assert_eq_size("push 1: count", 1, rb.count);

        size_t len;
        const char *line = ringbuf_get(&rb, 0, &len);
        assert_eq_str("push 1: get 0", "aaa", 3, line, len);

        ringbuf_push(&rb, "bbb", 3);
        ringbuf_push(&rb, "ccc", 3);
        assert_eq_size("push 3: count", 3, rb.count);

        line = ringbuf_get(&rb, 0, &len);
        assert_eq_str("push 3: get 0 (oldest)", "aaa", 3, line, len);
        line = ringbuf_get(&rb, 1, &len);
        assert_eq_str("push 3: get 1", "bbb", 3, line, len);
        line = ringbuf_get(&rb, 2, &len);
        assert_eq_str("push 3: get 2 (newest)", "ccc", 3, line, len);

        ringbuf_free(&rb);
    }

    /* -- Wrap-around -- */
    {
        RingBuf rb;
        ringbuf_init(&rb, 3);

        ringbuf_push(&rb, "aaa", 3);
        ringbuf_push(&rb, "bbb", 3);
        ringbuf_push(&rb, "ccc", 3);
        ringbuf_push(&rb, "ddd", 3);  /* overwrites "aaa" */
        assert_eq_size("wrap: count stays at capacity", 3, rb.count);

        size_t len;
        const char *line = ringbuf_get(&rb, 0, &len);
        assert_eq_str("wrap: oldest is bbb", "bbb", 3, line, len);
        line = ringbuf_get(&rb, 1, &len);
        assert_eq_str("wrap: middle is ccc", "ccc", 3, line, len);
        line = ringbuf_get(&rb, 2, &len);
        assert_eq_str("wrap: newest is ddd", "ddd", 3, line, len);

        ringbuf_free(&rb);
    }

    /* -- Multiple wraps -- */
    {
        RingBuf rb;
        ringbuf_init(&rb, 2);

        ringbuf_push(&rb, "1", 1);
        ringbuf_push(&rb, "2", 1);
        ringbuf_push(&rb, "3", 1);
        ringbuf_push(&rb, "4", 1);
        ringbuf_push(&rb, "5", 1);

        size_t len;
        const char *line = ringbuf_get(&rb, 0, &len);
        assert_eq_str("multi-wrap: oldest is 4", "4", 1, line, len);
        line = ringbuf_get(&rb, 1, &len);
        assert_eq_str("multi-wrap: newest is 5", "5", 1, line, len);

        ringbuf_free(&rb);
    }

    /* -- Get out of bounds -- */
    {
        RingBuf rb;
        ringbuf_init(&rb, 3);

        ringbuf_push(&rb, "x", 1);
        size_t len;
        const char *line = ringbuf_get(&rb, 5, &len);
        assert_eq_size("out of bounds: len is 0", 0, len);
        assert_eq_str("out of bounds: returns empty", "", 0, line, len);

        ringbuf_free(&rb);
    }

    /* -- Variable length lines -- */
    {
        RingBuf rb;
        ringbuf_init(&rb, 3);

        ringbuf_push(&rb, "short", 5);
        ringbuf_push(&rb, "a longer line here", 18);
        ringbuf_push(&rb, "x", 1);

        size_t len;
        const char *line = ringbuf_get(&rb, 0, &len);
        assert_eq_str("varlen: short", "short", 5, line, len);
        line = ringbuf_get(&rb, 1, &len);
        assert_eq_str("varlen: long", "a longer line here", 18, line, len);
        line = ringbuf_get(&rb, 2, &len);
        assert_eq_str("varlen: single char", "x", 1, line, len);

        ringbuf_free(&rb);
    }

    /* -- Capacity 1 -- */
    {
        RingBuf rb;
        ringbuf_init(&rb, 1);

        ringbuf_push(&rb, "first", 5);
        size_t len;
        const char *line = ringbuf_get(&rb, 0, &len);
        assert_eq_str("cap 1: first", "first", 5, line, len);

        ringbuf_push(&rb, "second", 6);
        line = ringbuf_get(&rb, 0, &len);
        assert_eq_str("cap 1: replaced", "second", 6, line, len);
        assert_eq_size("cap 1: count is 1", 1, rb.count);

        ringbuf_free(&rb);
    }

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           pass_count, pass_count + fail_count, fail_count);

    return fail_count > 0 ? 1 : 0;
}
