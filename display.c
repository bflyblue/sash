/*
 * display.c - Terminal display & draw buffer
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "display.h"
#include "ringbuf.h"
#include "sash.h"

/* ── Draw buffer (internal) ──────────────────────────────────────── */

static char  *g_draw_buf  = NULL;
static size_t g_draw_cap  = 0;
static size_t g_draw_len  = 0;

static void dbuf_reset(void) { g_draw_len = 0; }

static void dbuf_ensure(size_t need)
{
    if (g_draw_len + need > g_draw_cap) {
        g_draw_cap = (g_draw_len + need) * 2;
        g_draw_buf = realloc(g_draw_buf, g_draw_cap);
        if (!g_draw_buf) {
            perror("sash: realloc");
            exit(1);
        }
    }
}

static void dbuf_append(const char *s, size_t n)
{
    dbuf_ensure(n);
    memcpy(g_draw_buf + g_draw_len, s, n);
    g_draw_len += n;
}

static void dbuf_printf(const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        dbuf_append(tmp, n < (int)sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* ── TTY output ──────────────────────────────────────────────────── */

/* Single write() call — the kernel's tty atomic_write_lock ensures this
   won't interleave with other writers.  We intentionally don't retry short
   writes: a torn frame is better than two syscalls with a gap between them. */
void tty_write(const char *buf, size_t len)
{
    if (g_tty_fd >= 0 && len > 0)
        (void)!write(g_tty_fd, buf, len);
}

static void dbuf_flush(void)
{
    tty_write(g_draw_buf, g_draw_len);
}

void display_free_drawbuf(void)
{
    free(g_draw_buf);
    g_draw_buf = NULL;
}

/* ── Terminal size ───────────────────────────────────────────────── */

void get_terminal_size(void)
{
    struct winsize ws;
    if (g_tty_fd >= 0 && ioctl(g_tty_fd, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) g_term_cols = ws.ws_col;
        if (ws.ws_row > 0) g_term_rows = ws.ws_row;
    }
}

/* ── Rendering ───────────────────────────────────────────────────── */

/*
 * Sanitise a line for terminal display: replace control characters (except
 * tab) with '.', strip trailing newline, and truncate to terminal width.
 * Returns number of characters written to dst.
 */
static size_t sanitize_line(char *dst, size_t dst_cap,
                            const char *src, size_t src_len)
{
    size_t col = 0;
    for (size_t i = 0; i < src_len && col < dst_cap; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\n' || ch == '\r')
            continue;
        if (ch == '\t') {
            /* expand tab to spaces up to next 8-col stop */
            size_t stop = ((col / 8) + 1) * 8;
            while (col < stop && col < dst_cap)
                dst[col++] = ' ';
        } else if (ch < 0x20 || ch == 0x7f) {
            dst[col++] = '.';
        } else {
            dst[col++] = (char)ch;
        }
    }
    return col;
}

/*
 * Append the window content to dbuf.  Does not reset or flush — the caller
 * can prepend setup sequences and still emit everything in one write().
 *
 * Uses absolute cursor positioning to the fixed window area at the bottom
 * of the screen (below the scroll region).  The scroll region isolates
 * the window from scrolling caused by other processes writing to the TTY.
 */
static void build_redraw(void)
{
    int height = g_win_height;
    if (height > g_term_rows - 1)
        height = g_term_rows - 1;
    if (height < 1) height = 1;

    int win_top = g_win_top;
    int margin = g_line_numbers ? 6 : 0;
    int content_cols = g_term_cols - margin;
    if (content_cols < 1) content_cols = 1;

    /* move to the first row of the window */
    dbuf_printf("\033[%d;1H", win_top);

    /* draw each row */
    char *san = malloc((size_t)content_cols + 1);
    if (!san) {
        perror("sash: malloc");
        exit(1);
    }

    /* compute base line number for visible rows */
    size_t visible = g_ring.count < (size_t)height
                   ? g_ring.count : (size_t)height;
    size_t base = g_total_lines - visible + 1;

    for (int row = 0; row < height; row++) {
        /* carriage return + clear line */
        dbuf_append("\r\033[2K", 5);

        size_t len;
        const char *line;

        if ((size_t)row < g_ring.count) {
            /* index from oldest visible to newest */
            size_t idx;
            if (g_ring.count <= (size_t)height)
                idx = (size_t)row;
            else
                idx = g_ring.count - (size_t)height + (size_t)row;
            line = ringbuf_get(&g_ring, idx, &len);

            if (g_line_numbers) {
                if (g_color) dbuf_append("\033[90m", 5);
                dbuf_printf("%5zu\xe2\x94\x82", base + (size_t)row);
                if (g_color) dbuf_append("\033[0m", 4);
            }
        } else {
            line = "";
            len  = 0;

            if (g_line_numbers) {
                if (g_color) dbuf_append("\033[90m", 5);
                dbuf_append("     \xe2\x94\x82", 8);
                if (g_color) dbuf_append("\033[0m", 4);
            }
        }

        size_t slen = sanitize_line(san, (size_t)content_cols, line, len);
        if (slen > 0)
            dbuf_append(san, slen);

        /* move down (except on last row) */
        if (row < height - 1)
            dbuf_append("\n", 1);
    }

    free(san);

    /* park cursor at the bottom of the scroll region so any concurrent
       output (e.g. stderr from the piped command) appears above the window */
    if (g_scroll_bottom > 0)
        dbuf_printf("\033[%d;1H", g_scroll_bottom);
}

void redraw_window(void)
{
    if (!g_is_tty) return;
    dbuf_reset();
    build_redraw();
    dbuf_flush();
}

/* ── Cursor & window setup ───────────────────────────────────────── */

/*
 * Query the cursor's current row via DSR (Device Status Report).
 * Returns the 1-based row number, or 0 on failure.
 */
static int get_cursor_row(void)
{
    if (g_tty_fd < 0) return 0;

    struct termios orig, raw;
    if (tcgetattr(g_tty_fd, &orig) == -1) return 0;

    raw = orig;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;  /* 100 ms timeout */
    if (tcsetattr(g_tty_fd, TCSANOW, &raw) == -1) return 0;

    /* send DSR */
    (void)!write(g_tty_fd, "\033[6n", 4);

    /* read response: \033[row;colR */
    char resp[32];
    size_t pos = 0;
    while (pos < sizeof(resp) - 1) {
        ssize_t n = read(g_tty_fd, &resp[pos], 1);
        if (n <= 0) break;
        if (resp[pos] == 'R') { pos++; break; }
        pos++;
    }
    resp[pos] = '\0';

    tcsetattr(g_tty_fd, TCSANOW, &orig);

    /* parse \033[row;colR */
    int row = 0;
    char *p = strchr(resp, '[');
    if (p) {
        row = atoi(p + 1);
    }
    return row;
}

void setup_window(void)
{
    if (!g_is_tty) return;

    get_terminal_size();

    int height = g_win_height;
    if (height > g_term_rows - 1)
        height = g_term_rows - 1;
    if (height < 1) height = 1;

    /* Decide where to place the window: just below the cursor if it fits,
       otherwise scroll to make room at the bottom. */
    int newlines;
    int cursor_row = get_cursor_row();
    if (cursor_row > 0 && cursor_row + height - 1 <= g_term_rows) {
        g_win_top = cursor_row;
        newlines = 0;
    } else {
        g_win_top = g_term_rows - height + 1;
        newlines = height - 1;
    }
    g_scroll_bottom = g_win_top - 1;

    /* Everything below is assembled into one buffer and emitted as a single
       atomic write() to avoid other TTY writers slipping in between. */
    dbuf_reset();

    /* Reserve space: push existing content (prompt, etc.) above the window. */
    for (int i = 0; i < newlines; i++)
        dbuf_append("\n", 1);

    /* Hide cursor — stays hidden for the lifetime of the tool */
    dbuf_append("\033[?25l", 6);

    /* Set scroll region to the rows above the window.  Anything writing to
       the TTY while the cursor is in this region (e.g. stderr from a piped
       command) will scroll within it, leaving the window untouched.
       DECSTBM requires top < bottom, so we need at least 2 rows. */
    if (g_scroll_bottom >= 2)
        dbuf_printf("\033[1;%dr", g_scroll_bottom);

    /* Draw the initial (empty) window and park cursor in the scroll region */
    build_redraw();

    dbuf_flush();
    g_started = true;
}

/* ── Resize handling ─────────────────────────────────────────────── */

void handle_resize(void)
{
    g_resize = 0;
    get_terminal_size();

    int height = g_win_height;
    if (height > g_term_rows - 1)
        height = g_term_rows - 1;
    if (height < 1) height = 1;

    g_win_top = g_term_rows - height + 1;
    g_scroll_bottom = g_win_top - 1;

    if (g_started) {
        dbuf_reset();
        /* update scroll region for new terminal size */
        if (g_scroll_bottom >= 2)
            dbuf_printf("\033[1;%dr", g_scroll_bottom);
        else
            dbuf_append("\033[r", 3);  /* reset to full screen */
        build_redraw();
        dbuf_flush();
    }
}
