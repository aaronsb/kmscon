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

#include <errno.h>
#include <libtsm.h>
#include "image_helper_proto.h"
#include "image_identity.h"
#include "render/image_budget.h"
#include "shl/log.h"
#include "terminal_image.h"

#define LOG_SUBSYSTEM "terminal"

#ifdef TSM_VTE_IMAGE_HOST
/* kmscon passes libtsm's flags and outcomes through unchanged */
_Static_assert(KMSCON_IMAGE_INFLATE == TSM_VTE_IMAGE_INFLATE &&
		       KMSCON_IMAGE_PNG == TSM_VTE_IMAGE_PNG &&
		       KMSCON_IMAGE_EXACT == TSM_VTE_IMAGE_EXACT &&
		       KMSCON_IMAGE_DISCARD == TSM_VTE_IMAGE_DISCARD,
	       "the helper's flags are libtsm's");
_Static_assert(KMSCON_IMAGE_OK == TSM_VTE_IMAGE_OK &&
		       KMSCON_IMAGE_UNREADABLE == TSM_VTE_IMAGE_UNREADABLE &&
		       KMSCON_IMAGE_TOO_SHORT == TSM_VTE_IMAGE_TOO_SHORT &&
		       KMSCON_IMAGE_CORRUPT == TSM_VTE_IMAGE_CORRUPT &&
		       KMSCON_IMAGE_TOO_LARGE == TSM_VTE_IMAGE_TOO_LARGE &&
		       KMSCON_IMAGE_OVER_BUDGET == TSM_VTE_IMAGE_OVER_BUDGET &&
		       IMAGE_HOST_FAILED == TSM_VTE_IMAGE_FAILED,
	       "the helper's outcomes are libtsm's");

/*
 * libtsm's host hook. A negative return answers the command at once and
 * input goes on: before login, or for a root pty owner by fallback, that is
 * every file medium, and inline data when neither pre-login user exists
 * (image_identity.h); and any request no helper could take.
 */
static int image_event(struct tsm_vte *vte, const struct tsm_vte_image_request *req, void *data)
{
	struct terminal_image *ti = data;
	struct image_host_request r = {
		.id = req->id,
		.medium = req->medium,
		.flags = req->flags,
		.offset = req->offset,
		.read_max = req->read_max,
		.min_bytes = req->min_bytes,
		.name = req->name,
		.data = req->data,
		.data_len = req->data_len,
		.max_width = ti->conf->max_graphic_size.width,
		.max_height = ti->conf->max_graphic_size.height,
		.max_bytes = image_budget_limit(),
	};
	struct image_user user;
	uid_t owner = 0;
	int ret;

	(void)vte;
	ret = kmscon_pty_get_slave_owner(ti->pty, &owner);
	ret = image_identity_find(kmscon_pty_get_child(ti->pty), ret, owner,
				  req->medium != TSM_VTE_IMAGE_DIRECT, &user);
	if (ret) {
		log_debug("image request %llu refused: no user may read it (%d)",
			  (unsigned long long)req->id, ret);
		return -EBADF;
	}
	if (image_host_send(ti->host, &user, &r))
		return -EBADF;
	kmscon_pty_pause(ti->pty);
	kmscon_asciinema_pause(ti->asciinema);
	return 0;
}

/* -ENOENT from libtsm, a request it cancelled meanwhile, resumes all the same */
static void image_reply(struct image_host *host, uint64_t id, unsigned int status,
			unsigned int width, unsigned int height, const uint8_t *bytes, size_t len,
			void *data)
{
	struct terminal_image *ti = data;
	struct tsm_vte_image_result res = {
		.status = status,
		.width = width,
		.height = height,
		.data = bytes,
		.len = len,
	};

	(void)host;
	tsm_vte_image_reply(ti->vte, id, &res);
	ti->replied(ti->data);
	kmscon_pty_resume(ti->pty);
	if (ti->playing)
		kmscon_asciinema_resume(ti->asciinema);
}
#endif

int terminal_image_init(struct terminal_image *ti, struct tsm_vte *vte, struct kmscon_pty *pty,
			const struct kmscon_conf_t *conf, const char *helper,
			terminal_image_replied_cb replied, void *data)
{
	*ti = (struct terminal_image){
		.vte = vte,
		.pty = pty,
		.conf = conf,
		.replied = replied,
		.data = data,
	};
#ifdef TSM_VTE_IMAGE_HOST
	int ret = image_host_new(&ti->host, kmscon_pty_get_eloop(pty), helper, image_reply, ti);

	if (ret)
		return ret;
	tsm_vte_set_image_cb(vte, image_event, ti);
#else
	(void)helper;
#endif
	return 0;
}

void terminal_image_destroy(struct terminal_image *ti)
{
#ifdef TSM_VTE_IMAGE_HOST
	if (ti->vte)
		tsm_vte_set_image_cb(ti->vte, NULL, NULL);
#endif
	image_host_free(ti->host);
	ti->host = NULL;
}

void terminal_image_set_playback(struct terminal_image *ti, struct kmscon_asciinema *asciinema)
{
	ti->asciinema = asciinema;
}

void terminal_image_play(struct terminal_image *ti, bool playing)
{
	ti->playing = playing;
	if (!playing)
		kmscon_asciinema_pause(ti->asciinema);
	else if (!image_host_busy(ti->host))
		kmscon_asciinema_resume(ti->asciinema);
}

void terminal_image_close(struct terminal_image *ti)
{
	kmscon_asciinema_pause(ti->asciinema);
	if (image_host_busy(ti->host))
		tsm_vte_reset(ti->vte);
	image_host_cancel(ti->host);
}

bool terminal_image_busy(struct terminal_image *ti)
{
	return image_host_busy(ti->host);
}

bool terminal_image_input(struct terminal_image *ti, const char *u8, size_t len)
{
	if (image_host_busy(ti->host)) {
		log_warning("%zu bytes of kmscon's own input held back: an image request is out",
			    len);
		return false;
	}
	tsm_vte_input(ti->vte, u8, len);
	return true;
}
