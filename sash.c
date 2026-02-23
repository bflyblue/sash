/*
 * sash - tee with a live tail window
 *
 * Pipes stdin to output files (like tee) while showing only the last N lines
 * in a fixed terminal region that redraws in place.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ── Ring buffer ─────────────────────────────────────────────────── */

typedef struct {
    char  **lines;
    size_t *lengths;
    size_t  capacity;
    size_t  head;
    size_t  count;
} RingBuf;

static void ringbuf_init(RingBuf *rb, size_t cap)
{
    rb->lines    = calloc(cap, sizeof(char *));
    rb->lengths  = calloc(cap, sizeof(size_t));
    rb->capacity = cap;
    rb->head     = 0;
    rb->count    = 0;
    if (!rb->lines || !rb->lengths) {
        perror("sash: calloc");
        exit(1);
    }
}

static void ringbuf_push(RingBuf *rb, const char *line, size_t len)
{
    size_t slot;
    if (rb->count < rb->capacity) {
        slot = (rb->head + rb->count) % rb->capacity;
        rb->count++;
    } else {
        slot = rb->head;
        rb->head = (rb->head + 1) % rb->capacity;
        free(rb->lines[slot]);
    }
    rb->lines[slot]   = strndup(line, len);
    rb->lengths[slot]  = len;
}

static const char *ringbuf_get(const RingBuf *rb, size_t i, size_t *len)
{
    if (i >= rb->count) {
        *len = 0;
        return "";
    }
    size_t idx = (rb->head + i) % rb->capacity;
    *len = rb->lengths[idx];
    return rb->lines[idx];
}

static void ringbuf_free(RingBuf *rb)
{
    for (size_t i = 0; i < rb->capacity; i++)
        free(rb->lines[i]);
    free(rb->lines);
    free(rb->lengths);
}

/* ── Globals ─────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_resize  = 0;
static volatile sig_atomic_t g_sigint  = 0;
static volatile sig_atomic_t g_sigpipe = 0;

static RingBuf  g_ring;
static FILE   **g_files      = NULL;
static int      g_nfiles     = 0;
static FILE    *g_tty        = NULL;
static int      g_tty_fd     = -1;
static bool     g_is_tty     = false;
static bool     g_flush      = false;
static int      g_win_height = 10;
static int      g_term_cols  = 80;
static int      g_term_rows  = 24;
static bool     g_started    = false;  /* has the window been drawn at least once? */
static size_t   g_total_lines = 0;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
        "Usage: command | sash [-n lines] [-f] [-h] [file ...]\n"
        "\n"
        "  -n N    Window height (default: 10)\n"
        "  -f      Flush output files after each line\n"
        "  -h      Show this help\n"
        "\n"
        "Pipes stdin to files while showing a live tail of the last N lines.\n");
}

static void get_terminal_size(void)
{
    struct winsize ws;
    if (g_tty_fd >= 0 && ioctl(g_tty_fd, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) g_term_cols = ws.ws_col;
        if (ws.ws_row > 0) g_term_rows = ws.ws_row;
    }
}

/* ── Display ─────────────────────────────────────────────────────── */

/*
 * Build the full redraw in a buffer and emit with a single write()
 * to minimise flicker. We use /dev/tty exclusively for display.
 */

/* Growable write buffer */
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
        dbuf_append(tmp, (size_t)n);
}

static void tty_write(const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(g_tty_fd, buf, len);
        if (n <= 0) break;
        buf += n;
        len -= (size_t)n;
    }
}

static void dbuf_flush(void)
{
    if (g_tty_fd >= 0 && g_draw_len > 0)
        tty_write(g_draw_buf, g_draw_len);
}

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

static void redraw_window(void)
{
    if (!g_is_tty) return;

    int height = g_win_height;
    if (height > g_term_rows - 1)
        height = g_term_rows - 1;
    if (height < 1) height = 1;

    dbuf_reset();

    /* hide cursor */
    dbuf_append("\033[?25l", 6);

    /* move cursor up to the top of our window */
    if (height - 1 > 0)
        dbuf_printf("\033[%dA", height - 1);

    /* draw each row */
    char *san = malloc((size_t)g_term_cols + 1);
    if (!san) return;

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
        } else {
            line = "";
            len  = 0;
        }

        size_t slen = sanitize_line(san, (size_t)g_term_cols, line, len);
        if (slen > 0)
            dbuf_append(san, slen);

        /* move down (except on last row) */
        if (row < height - 1)
            dbuf_append("\n", 1);
    }

    free(san);

    /* show cursor */
    dbuf_append("\033[?25h", 6);

    dbuf_flush();
}

static void setup_window(void)
{
    if (!g_is_tty) return;

    get_terminal_size();

    int height = g_win_height;
    if (height > g_term_rows - 1)
        height = g_term_rows - 1;
    if (height < 1) height = 1;

    /* reserve space: print (height-1) newlines so the cursor sits at the
       bottom of our future window */
    for (int i = 0; i < height - 1; i++)
        tty_write("\n", 1);

    g_started = true;
    redraw_window();
}

/* ── File I/O ────────────────────────────────────────────────────── */

static void write_to_files(const char *buf, size_t len)
{
    for (int i = 0; i < g_nfiles; i++) {
        if (g_files[i]) {
            if (fwrite(buf, 1, len, g_files[i]) < len) {
                fprintf(stderr, "sash: write error on file %d: %s\n",
                        i, strerror(errno));
                fclose(g_files[i]);
                g_files[i] = NULL;
            } else if (g_flush) {
                fflush(g_files[i]);
            }
        }
    }
}

/* ── Signal handling ─────────────────────────────────────────────── */

static void sig_handler(int sig)
{
    switch (sig) {
    case SIGWINCH: g_resize  = 1; break;
    case SIGINT:   g_sigint  = 1; break;
    case SIGPIPE:  g_sigpipe = 1; break;
    }
}

static void setup_signals(void)
{
    struct sigaction sa;

    /* SIGWINCH - restart syscalls */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    /* SIGINT - do NOT restart, so getline returns */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags   = 0;
    sigaction(SIGINT, &sa, NULL);

    /* SIGPIPE - do NOT restart */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags   = 0;
    sigaction(SIGPIPE, &sa, NULL);
}

static void handle_resize(void)
{
    g_resize = 0;
    get_terminal_size();
    if (g_started)
        redraw_window();
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

static void cleanup(void)
{
    /* move cursor below the window and show it */
    if (g_is_tty && g_started && g_tty_fd >= 0) {
        tty_write("\n\033[?25h", 7);
    }

    /* close output files */
    for (int i = 0; i < g_nfiles; i++) {
        if (g_files[i])
            fclose(g_files[i]);
    }
    free(g_files);
    g_files  = NULL;
    g_nfiles = 0;

    /* close tty */
    if (g_tty) {
        fclose(g_tty);
        g_tty    = NULL;
        g_tty_fd = -1;
    }

    /* free ring buffer & draw buffer */
    ringbuf_free(&g_ring);
    free(g_draw_buf);
    g_draw_buf = NULL;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "n:fh")) != -1) {
        switch (opt) {
        case 'n':
            g_win_height = atoi(optarg);
            if (g_win_height < 1) {
                fprintf(stderr, "sash: window height must be >= 1\n");
                return 1;
            }
            break;
        case 'f':
            g_flush = true;
            break;
        case 'h':
            usage();
            return 0;
        default:
            usage();
            return 1;
        }
    }

    /* remaining args are output files */
    int nfiles = argc - optind;
    if (nfiles > 0) {
        g_files  = calloc((size_t)nfiles, sizeof(FILE *));
        g_nfiles = nfiles;
        if (!g_files) {
            perror("sash: calloc");
            return 1;
        }
        for (int i = 0; i < nfiles; i++) {
            g_files[i] = fopen(argv[optind + i], "w");
            if (!g_files[i]) {
                fprintf(stderr, "sash: cannot open '%s': %s\n",
                        argv[optind + i], strerror(errno));
                /* non-fatal: continue with other files */
            }
        }
    }

    /* detect controlling terminal */
    g_tty = fopen("/dev/tty", "w");
    if (g_tty) {
        g_tty_fd = fileno(g_tty);
        g_is_tty = true;
    }

    /* warn if stdin is a terminal (user probably forgot to pipe) */
    if (isatty(STDIN_FILENO)) {
        fprintf(stderr, "sash: warning: reading from terminal "
                        "(did you forget to pipe input?)\n");
    }

    atexit(cleanup);
    setup_signals();

    ringbuf_init(&g_ring, (size_t)g_win_height);

    if (g_is_tty)
        setup_window();

    /* main loop */
    char   *line = NULL;
    size_t  line_cap = 0;
    ssize_t nread;
    int     exit_code = 0;

    while ((nread = getline(&line, &line_cap, stdin)) > 0) {
        /* check for resize before processing */
        if (g_resize)
            handle_resize();

        g_total_lines++;

        /* write raw line to output files */
        write_to_files(line, (size_t)nread);

        if (g_is_tty) {
            /* push to ring buffer and redraw */
            ringbuf_push(&g_ring, line, (size_t)nread);
            redraw_window();
        } else {
            /* passthrough mode: write to stdout */
            fwrite(line, 1, (size_t)nread, stdout);
        }
    }

    free(line);

    if (g_sigint) {
        exit_code = 130;
    } else if (g_sigpipe) {
        exit_code = 141;
    }

    return exit_code;
}
