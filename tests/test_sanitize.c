/*
 * test_sanitize.c - Unit tests for sanitize_line()
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Includes display.c directly to access static functions and globals.
 * Stub globals satisfy the extern declarations from sash.h.
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../ringbuf.h"
#include "../sash.h"

/* Stub globals required by display.c via sash.h */
volatile sig_atomic_t g_resize = 0;
RingBuf g_ring = {0};
int g_tty_fd = -1;
bool g_is_tty = false;
bool g_line_numbers = false;
bool g_color = false;
int g_win_height = 10;
int g_term_cols = 80;
int g_term_rows = 24;
int g_scroll_bottom = 0;
int g_win_top = 0;
bool g_started = false;
size_t g_total_lines = 0;
bool g_ansi = false;

/* Stub ringbuf functions referenced by display.c */
void ringbuf_init(RingBuf *rb, size_t cap) {
  (void)rb;
  (void)cap;
}
void ringbuf_push(RingBuf *rb, const char *line, size_t len) {
  (void)rb;
  (void)line;
  (void)len;
}
const char *ringbuf_get(const RingBuf *rb, size_t i, size_t *len) {
  (void)rb;
  (void)i;
  *len = 0;
  return "";
}
void ringbuf_free(RingBuf *rb) { (void)rb; }

#include "../display.c"

/* ── Test harness ────────────────────────────────────────────────── */

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *desc, const char *expected, size_t expected_len) {
  if (g_draw_len == expected_len &&
      memcmp(g_draw_buf, expected, expected_len) == 0) {
    printf("  PASS: %s\n", desc);
    pass_count++;
  } else {
    printf("  FAIL: %s\n", desc);
    printf("    expected (%zu): \"", expected_len);
    fwrite(expected, 1, expected_len, stdout);
    printf("\"\n    got      (%zu): \"", g_draw_len);
    if (g_draw_buf)
      fwrite(g_draw_buf, 1, g_draw_len, stdout);
    printf("\"\n");
    fail_count++;
  }
}

/* Helper: reset dbuf, call sanitize_line, then check result */
static void test(const char *desc, bool ansi, const char *input,
                 size_t max_cols, const char *expected, size_t expected_len) {
  g_ansi = ansi;
  dbuf_reset();
  sanitize_line(input, strlen(input), max_cols);
  check(desc, expected, expected_len);
}

/* ── Tests ───────────────────────────────────────────────────────── */

int main(void) {
  printf("=== sanitize_line unit tests ===\n\n");

  /* -- Default mode (g_ansi = false) -- */

  test("printable text passes through", false, "hello", 80, "hello", 5);

  test("truncate at max_cols", false, "hello world", 5, "hello", 5);

  test("newline stripped", false, "abc\n", 80, "abc", 3);

  test("carriage return stripped", false, "abc\r", 80, "abc", 3);

  test("control char replaced with dot", false, "a\x01z", 80, "a.z", 3);

  test("DEL replaced with dot", false, "a\x7fz", 80, "a.z", 3);

  test("tab expands to spaces", false, "\t", 80, "        ", 8);

  test("tab mid-line expands to next stop", false, "ab\t", 80, "ab      ", 8);

  test("ESC replaced with dot (default mode)", false, "\033[31m", 80, ".[31m",
       5);

  test("empty input", false, "", 80, "", 0);

  /* -- ANSI mode (g_ansi = true) -- */

  test("ANSI: printable text passes through", true, "hello", 80, "hello\033[0m",
       5 + 4);

  test("ANSI: CSI sequence passed through", true, "\033[31mred\033[0m", 80,
       "\033[31mred\033[0m\033[0m", 3 + 5 + 4 + 4);

  test("ANSI: CSI doesn't count as visible columns", true, "\033[31mred\033[0m",
       3, "\033[31mred\033[0m", 5 + 3 + 4);

  test("ANSI: truncation counts only visible cols", true,
       "\033[31mhello world\033[0m", 5, "\033[31mhello\033[0m", 5 + 5 + 4);

  test("ANSI: two-byte escape passed through", true, "\033Mtext", 80,
       "\033Mtext\033[0m", 2 + 4 + 4);

  test("ANSI: control chars still replaced with dot", true, "a\x01z", 80,
       "a.z\033[0m", 3 + 4);

  test("ANSI: newline stripped", true, "abc\n", 80, "abc\033[0m", 3 + 4);

  test("ANSI: tab still expands", true, "\t", 80, "        \033[0m", 8 + 4);

  printf("\n=== Results: %d/%d passed, %d failed ===\n", pass_count,
         pass_count + fail_count, fail_count);

  return fail_count > 0 ? 1 : 0;
}
