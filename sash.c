/*
 * sash - tee with a live tail window
 *
 * Copyright (c) 2026, Shaun Sharples
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pipes stdin to output files (like tee) while showing only the last N lines
 * in a fixed terminal region that redraws in place.
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

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
#include <sys/wait.h>
#include <termios.h>
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
    if (!rb->lines[slot]) {
        rb->lengths[slot] = 0;
        return;
    }
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

static pid_t g_child_pid = 0;

static RingBuf  g_ring;
static FILE   **g_files        = NULL;
static int      g_nfiles       = 0;
static FILE    *g_tty          = NULL;
static int      g_tty_fd       = -1;
static bool     g_is_tty       = false;
static bool     g_flush        = false;
static bool     g_exec         = false;
static bool     g_line_numbers = false;
static bool     g_color        = false;
static int      g_color_mode   = 0;  /* 0=auto, 1=force on, -1=force off */
static int      g_win_height   = 10;
static int      g_term_cols    = 80;
static int      g_term_rows    = 24;
static int      g_scroll_bottom = 0;  /* last row of scroll region (0 = none) */
static int      g_win_top       = 0;  /* first row of the tail window */
static bool     g_started      = false;
static size_t   g_total_lines  = 0;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void add_file(const char *path, const char *mode)
{
    g_files = realloc(g_files, (size_t)(g_nfiles + 1) * sizeof(FILE *));
    if (!g_files) {
        perror("sash: realloc");
        exit(1);
    }
    g_files[g_nfiles] = fopen(path, mode);
    if (!g_files[g_nfiles]) {
        fprintf(stderr, "sash: cannot open '%s': %s\n",
                path, strerror(errno));
        /* non-fatal: store NULL, skip during writes */
    }
    g_nfiles++;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: sash [-n lines] [-f] [-x] [-l] [-c|-C] [-w file] [-a file] [-h] [command [args...]]\n"
        "\n"
        "  -n N    Window height (default: 10)\n"
        "  -f      Flush output files after each line\n"
        "  -x      Use exec instead of shell (no pipes, &&, etc.)\n"
        "  -l      Show line numbers\n"
        "  -c      Force color on\n"
        "  -C      Force color off\n"
        "  -w FILE Write output to FILE (truncate)\n"
        "  -a FILE Append output to FILE\n"
        "  -h      Show this help\n"
        "\n"
        "Pipe mode:    command | sash [-w file ...]\n"
        "Command mode: sash [-w file ...] command [args...]\n");
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

/* Single write() call — the kernel's tty atomic_write_lock ensures this
   won't interleave with other writers.  We intentionally don't retry short
   writes: a torn frame is better than two syscalls with a gap between them. */
static void tty_write(const char *buf, size_t len)
{
    if (g_tty_fd >= 0 && len > 0)
        (void)!write(g_tty_fd, buf, len);
}

static void dbuf_flush(void)
{
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

static void redraw_window(void)
{
    if (!g_is_tty) return;
    dbuf_reset();
    build_redraw();
    dbuf_flush();
}

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

static void setup_window(void)
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

/* ── Command spawning ────────────────────────────────────────────── */

/* Join argv into a single space-separated string for sh -c */
static char *join_args(char **argv)
{
    size_t len = 0;
    for (int i = 0; argv[i]; i++)
        len += strlen(argv[i]) + 1;
    char *buf = malloc(len);
    if (!buf) {
        perror("sash: malloc");
        exit(1);
    }
    char *p = buf;
    for (int i = 0; argv[i]; i++) {
        if (i > 0) *p++ = ' ';
        size_t n = strlen(argv[i]);
        memcpy(p, argv[i], n);
        p += n;
    }
    *p = '\0';
    return buf;
}

static pid_t spawn_command(char **cmd_argv, bool use_exec, int *read_fd)
{
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("sash: pipe");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("sash: fork");
        exit(1);
    }

    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (use_exec) {
            execvp(cmd_argv[0], cmd_argv);
        } else {
            char *cmd = join_args(cmd_argv);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            free(cmd);
        }
        perror("sash: exec");
        _exit(127);
    }

    /* parent */
    close(pipefd[1]);
    *read_fd = pipefd[0];
    return pid;
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

static void cleanup(void)
{
    /* kill child if still running */
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        waitpid(g_child_pid, NULL, 0);
        g_child_pid = 0;
    }

    /* reset scroll region, move cursor below the window, show it */
    if (g_is_tty && g_started && g_tty_fd >= 0) {
        char buf[64];
        int height = g_win_height;
        if (height > g_term_rows - 1) height = g_term_rows - 1;
        if (height < 1) height = 1;
        int after = g_win_top + height;
        if (after > g_term_rows) after = g_term_rows;
        int n = snprintf(buf, sizeof(buf),
                         "\033[r\033[%d;1H\n\033[?25h", after);
        if (n > 0)
            tty_write(buf, (size_t)n);
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
    while ((opt = getopt(argc, argv, "n:fxlcCw:a:h")) != -1) {
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
        case 'x':
            g_exec = true;
            break;
        case 'l':
            g_line_numbers = true;
            break;
        case 'c':
            g_color_mode = 1;
            break;
        case 'C':
            g_color_mode = -1;
            break;
        case 'w':
            add_file(optarg, "w");
            break;
        case 'a':
            add_file(optarg, "a");
            break;
        case 'h':
            usage();
            return 0;
        default:
            usage();
            return 1;
        }
    }

    /* detect controlling terminal */
    g_tty = fopen("/dev/tty", "r+");
    if (g_tty) {
        g_tty_fd = fileno(g_tty);
        g_is_tty = true;
    }

    /* detect color support */
    if (g_color_mode == 1) {
        g_color = true;
    } else if (g_color_mode == -1) {
        g_color = false;
    } else if (g_is_tty && !getenv("NO_COLOR")) {
        const char *term = getenv("TERM");
        if (term && strcmp(term, "dumb") != 0)
            g_color = true;
    }

    /* set up input source — positional args are the command */
    FILE *input = stdin;

    if (optind < argc) {
        int pipe_fd;
        g_child_pid = spawn_command(&argv[optind], g_exec, &pipe_fd);
        input = fdopen(pipe_fd, "r");
        if (!input) {
            perror("sash: fdopen");
            return 1;
        }
    } else if (isatty(STDIN_FILENO)) {
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

    while ((nread = getline(&line, &line_cap, input)) > 0) {
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

    /* reap child and propagate exit code */
    if (g_child_pid > 0) {
        int status;
        waitpid(g_child_pid, &status, 0);
        g_child_pid = 0;
        if (WIFEXITED(status))
            exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            exit_code = 128 + WTERMSIG(status);
    }

    if (input != stdin)
        fclose(input);

    if (g_sigint) {
        exit_code = 130;
    } else if (g_sigpipe) {
        exit_code = 141;
    }

    return exit_code;
}
