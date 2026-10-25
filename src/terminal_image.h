/*
 * kmscon - A terminal's kitty graphics host
 *
 * Copyright (c) 2026 the kmscon fork authors
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
 * The part of a terminal that serves libtsm's host hook (ADR-103, Notes of
 * 2026-10-24, kmscon: the pause and the helper's lifetime): a t=f, t=t or
 * t=s command, a discard among them, or inline PNG or zlib data goes to the
 * session's image helper, its user found again for each request. While a
 * request is out, the pty and asciicast playback are paused, so the bytes
 * after the command are parsed only after its reply and commands keep their
 * order; the reply resumes both. It is apart from terminal.c so that a test
 * drives it through a real vte, pty and helper.
 *
 * kmscon's own input to the vte, playback and the issue text, goes through
 * terminal_image_input, on tsm_vte_input: file and PNG commands in a
 * recording or an issue file are answered as without a hook, since their
 * paths name another machine's or another time's files.
 */

#ifndef KMSCON_TERMINAL_IMAGE_H
#define KMSCON_TERMINAL_IMAGE_H

#include <libtsm.h>
#include <stdbool.h>
#include <stddef.h>
#include "asciinema.h"
#include "config.h"
#include "image_host.h"
#include "pty.h"

/* a request's reply reached libtsm: the screen may have changed */
typedef void (*terminal_image_replied_cb)(void *data);

struct terminal_image {
	struct tsm_vte *vte;
	struct kmscon_pty *pty;
	const struct kmscon_conf_t *conf;
	struct image_host *host;
	/* asciicast playback, or NULL */
	struct kmscon_asciinema *asciinema;
	/* playback may run: the terminal is in the foreground */
	bool playing;
	terminal_image_replied_cb replied;
	void *data;
};

/*
 * Installs the hook on @vte, its helper @helper (an absolute path) on @pty's
 * loop, which reaps its children. Without a libtsm that has the hook it does
 * nothing and succeeds. Returns 0 or a negative error.
 */
int terminal_image_init(struct terminal_image *ti, struct tsm_vte *vte, struct kmscon_pty *pty,
			const struct kmscon_conf_t *conf, const char *helper,
			terminal_image_replied_cb replied, void *data);
/* removes the hook and kills the helper; a request out gets no reply */
void terminal_image_destroy(struct terminal_image *ti);

/* the playback a request pauses; set once it is made */
void terminal_image_set_playback(struct terminal_image *ti, struct kmscon_asciinema *asciinema);
/*
 * The terminal came to the foreground (@playing) or left it. Playback runs
 * only in the foreground and with no request out; otherwise the reply
 * resumes it.
 */
void terminal_image_play(struct terminal_image *ti, bool playing);

/*
 * The session ended (its child exited, or the terminal is destroyed):
 * playback pauses, libtsm's request out, if any, is cancelled with
 * tsm_vte_reset, and the helper is killed, so the next session starts with
 * none out. Closing the pty clears its pause.
 */
void terminal_image_close(struct terminal_image *ti);

/* a request is out */
bool terminal_image_busy(struct terminal_image *ti);

/*
 * kmscon's own bytes, playback's or the issue text's, through tsm_vte_input.
 * While a request is out libtsm would drop them, so they are not fed and
 * false is returned; playback is paused then, and the issue text is fed only
 * when a session opens, when none is out.
 */
bool terminal_image_input(struct terminal_image *ti, const char *u8, size_t len);

#endif /* KMSCON_TERMINAL_IMAGE_H */
