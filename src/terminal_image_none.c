/*
 * kmscon - A terminal without images
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
 * terminal_image.h for a build without images (ADR-103, The images option): no
 * host hook and no helper, in place of terminal_image.c, image_host.c and
 * image_identity.c. Input goes straight to the vte, nothing is ever busy, and
 * asciicast playback follows the terminal into and out of the foreground, as
 * it does with images when no request is out.
 */

#include <libtsm.h>
#include "terminal_image.h"

int terminal_image_init(struct terminal_image *ti, struct tsm_vte *vte, struct kmscon_pty *pty,
			const struct kmscon_conf_t *conf, const char *helper,
			terminal_image_replied_cb replied, void *data)
{
	(void)helper;
	*ti = (struct terminal_image){
		.vte = vte,
		.pty = pty,
		.conf = conf,
		.replied = replied,
		.data = data,
	};
	return 0;
}

void terminal_image_destroy(struct terminal_image *ti)
{
	(void)ti;
}

void terminal_image_set_playback(struct terminal_image *ti, struct kmscon_asciinema *asciinema)
{
	ti->asciinema = asciinema;
}

void terminal_image_play(struct terminal_image *ti, bool playing)
{
	ti->playing = playing;
	if (playing)
		kmscon_asciinema_resume(ti->asciinema);
	else
		kmscon_asciinema_pause(ti->asciinema);
}

void terminal_image_close(struct terminal_image *ti)
{
	kmscon_asciinema_pause(ti->asciinema);
}

bool terminal_image_busy(struct terminal_image *ti)
{
	(void)ti;
	return false;
}

bool terminal_image_input(struct terminal_image *ti, const char *u8, size_t len)
{
	tsm_vte_input(ti->vte, u8, len);
	return true;
}
