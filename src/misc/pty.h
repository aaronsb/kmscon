/*
 * kmscon - Pseudo Terminal Handling
 *
 * Copyright (c) 2012 Ran Benita <ran234@gmail.com>
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
 * The pty object provides an interface for communicating with a child process
 * over a pseudo terminal. The child is the host, we act as the TTY terminal,
 * and the kernel is the driver.
 *
 * To use this, create a new pty object and open it. You will start receiving
 * output notifications through the output_cb callback. To communicate with
 * the other end of the terminal, use the kmscon_pty_input method. All
 * communication is done using byte streams (presumably UTF-8).
 *
 * The pty can be closed voluntarily using the kmson_pty_close method. The
 * child process can also exit at will; this will be communicated through the
 * input callback. The pty object does not wait on the child processes it
 * spawns; this is the responsibility of the object's user.
 */

#ifndef KMSCON_PTY_H
#define KMSCON_PTY_H

#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include "shl/eloop.h"

struct kmscon_pty;

/*
 * The child's output: returns how many bytes of it were taken. The rest is
 * held, and no more is read, until it is all taken: the pty hands it over
 * again on later turns of its loop, from an idle source.
 */
typedef size_t (*kmscon_pty_input_cb)(struct kmscon_pty *pty, const char *u8, size_t len,
				      void *data);
typedef void (*kmscon_pty_exit_cb)(struct kmscon_pty *pty, bool restart, void *data);

int kmscon_pty_new(struct kmscon_pty **out, kmscon_pty_input_cb input_cb,
		   kmscon_pty_exit_cb exit_cb, void *data);
void kmscon_pty_ref(struct kmscon_pty *pty);
void kmscon_pty_unref(struct kmscon_pty *pty);
int kmscon_pty_set_conf(struct kmscon_pty *pty, const char *term, const char *colorterm,
			char **argv, const char *seat, unsigned int vtnr, bool do_reset,
			bool backspace, bool oneshot);

int kmscon_pty_get_fd(struct kmscon_pty *pty);
void kmscon_pty_dispatch(struct kmscon_pty *pty);
/* the pty's own loop, which reaps its children */
struct ev_eloop *kmscon_pty_get_eloop(struct kmscon_pty *pty);

/*
 * The size in cells, and the cell size in pixels (0: unknown): TIOCSWINSZ
 * carries the text area in pixels, at most 65535 each way.
 */
int kmscon_pty_open(struct kmscon_pty *pty, unsigned short width, unsigned short height,
		    unsigned int cell_w, unsigned int cell_h, bool drm);
void kmscon_pty_close(struct kmscon_pty *pty);

/*
 * Queues what the child cannot take yet. Replies to the child are bounded:
 * what the input callback writes, and what is written between
 * kmscon_pty_begin_reply and kmscon_pty_end_reply (scopes nest). While the
 * child leaves too many of them unread, its output is neither read nor fed,
 * as in a pause, and past a ceiling they are dropped, each write whole, so
 * each reply must be one write: libtsm writes each in one call. What is
 * written otherwise (keys, pastes) is always queued. Closing drops the queue.
 */
int kmscon_pty_write(struct kmscon_pty *pty, const char *u8, size_t len);
void kmscon_pty_begin_reply(struct kmscon_pty *pty);
void kmscon_pty_end_reply(struct kmscon_pty *pty);
void kmscon_pty_signal(struct kmscon_pty *pty, int signum);
/* as kmscon_pty_open; a size the same as the last one sets nothing */
void kmscon_pty_resize(struct kmscon_pty *pty, unsigned short width, unsigned short height,
		       unsigned int cell_w, unsigned int cell_h);

int kmscon_pty_get_slave_name(struct kmscon_pty *pty, char *buf, size_t len);
/* the process the pty started, or 0 when it is not open */
pid_t kmscon_pty_get_child(struct kmscon_pty *pty);
/* the slave's owner, from fstat on the slave, as /bin/login sets it */
int kmscon_pty_get_slave_owner(struct kmscon_pty *pty, uid_t *uid);

/*
 * While paused, the pty neither reads nor hands its held output to the input
 * callback; a pause from inside the callback takes effect when it returns, and
 * what it did not take stays held. Resuming hands the held output over from
 * the next turn of the pty's loop, then reads again, unless the child still
 * leaves too many replies unread. Opening and closing the pty clear the pause.
 */
void kmscon_pty_pause(struct kmscon_pty *pty);
void kmscon_pty_resume(struct kmscon_pty *pty);

#endif /* KMSCON_PTY_H */
