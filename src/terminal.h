/*
 * kmscon - Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Terminal
 * This provides the basic terminal object. This ties together the vt emulation
 * and the output console.
 */

#ifndef KMSCON_TERMINAL_H
#define KMSCON_TERMINAL_H

#include <stdbool.h>

struct conf_ctx;
struct display;
struct ev_eloop;
struct kmscon_terminal;
struct kmscon_session;
struct input;

struct kmscon_terminal *terminal_new(struct kmscon_session *session, unsigned int vtnr,
				     struct conf_ctx *conf_ctx, struct ev_eloop *eloop,
				     struct input *input, const char *seat_name);
void terminal_destroy(struct kmscon_terminal *term);
int terminal_add_display(struct kmscon_terminal *term, struct display *disp);
void terminal_rm_display(struct kmscon_terminal *term, struct display *disp);
void terminal_refresh_displays(struct kmscon_terminal *term);
void terminal_activate(struct kmscon_terminal *term);
void terminal_deactivate(struct kmscon_terminal *term);

/*
 * Gives the terminal the focus or takes it away, for mode 1004: the program
 * hears of a change (CSI I, CSI O) if it set the mode, and a repeat of the
 * last state is dropped. The seat decides when a terminal has the focus
 * (seat.c, seat_update_focus); terminal_activate and terminal_deactivate do
 * not, since they also run when the display blanks.
 */
void terminal_set_focus(struct kmscon_terminal *term, bool focus);

#endif /* KMSCON_TERMINAL_H */
