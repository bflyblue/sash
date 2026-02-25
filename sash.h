/*
 * sash.h - Shared global declarations
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef SASH_H
#define SASH_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

#include "ringbuf.h"

extern volatile sig_atomic_t g_resize;

extern RingBuf  g_ring;
extern int      g_tty_fd;
extern bool     g_is_tty;
extern bool     g_line_numbers;
extern bool     g_color;
extern int      g_win_height;
extern int      g_term_cols;
extern int      g_term_rows;
extern int      g_scroll_bottom;
extern int      g_win_top;
extern bool     g_started;
extern size_t   g_total_lines;

#endif /* SASH_H */
