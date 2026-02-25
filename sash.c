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

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "display.h"
#include "process.h"
#include "ringbuf.h"
#include "sash.h"

/* ── Globals ─────────────────────────────────────────────────────── */

volatile sig_atomic_t g_resize  = 0;
static volatile sig_atomic_t g_sigint  = 0;
static volatile sig_atomic_t g_sigpipe = 0;

static pid_t g_child_pid = 0;

RingBuf  g_ring;
static FILE   **g_files        = NULL;
static int      g_nfiles       = 0;
static FILE    *g_tty          = NULL;
int      g_tty_fd       = -1;
bool     g_is_tty       = false;
static bool     g_flush        = false;
static bool     g_exec         = false;
bool     g_line_numbers = false;
bool     g_color        = false;
static int      g_color_mode   = 0;  /* 0=auto, 1=force on, -1=force off */
int      g_win_height   = 10;
int      g_term_cols    = 80;
int      g_term_rows    = 24;
int      g_scroll_bottom = 0;  /* last row of scroll region (0 = none) */
int      g_win_top       = 0;  /* first row of the tail window */
bool     g_started      = false;
size_t   g_total_lines  = 0;

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
    display_free_drawbuf();
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
            close(pipe_fd);
            kill(g_child_pid, SIGTERM);
            waitpid(g_child_pid, NULL, 0);
            g_child_pid = 0;
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
