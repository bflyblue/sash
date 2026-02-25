/*
 * display.h - Terminal display & draw buffer
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stddef.h>

void get_terminal_size(void);
void setup_window(void);
void handle_resize(void);
void redraw_window(void);
void tty_write(const char *buf, size_t len);
void display_free_drawbuf(void);

#endif /* DISPLAY_H */
