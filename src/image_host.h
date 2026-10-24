/*
 * kmscon - A terminal's image helper
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
 * kmscon's side of kmscon-image-helper (ADR-103, The image helper, and the
 * Notes of 2026-10-24): it starts the helper as the session's user, sends it
 * one request at a time, and checks each reply before reading a pixel of it.
 * The helper runs as the user, who can trace it, so a reply is trusted no
 * more than the client's bytes: any reply that fails a check, a helper that
 * closes its socket, or one that does not answer within the timeout ends the
 * request with EBADF and the helper with SIGKILL through its pidfd. The next
 * request starts a new one. It knows nothing of libtsm, so its tests drive it
 * without a vte.
 */

#ifndef KMSCON_IMAGE_HOST_H
#define KMSCON_IMAGE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "shl/eloop.h"

/* ADR-103: a request not answered in 2 seconds fails with EBADF */
#define IMAGE_HOST_TIMEOUT_MS 2000

struct image_host;

/* tsm_vte_image_request's fields, and the limits the helper applies */
struct image_host_request {
	uint64_t id;
	unsigned int medium; /* 'd', 'f', 't' or 's' */
	unsigned int format; /* 24, 32 or 100 */
	bool zlib;
	bool discard;
	unsigned int width;  /* s, or 0 */
	unsigned int height; /* v, or 0 */
	uint64_t offset;
	uint64_t size;
	const char *name;    /* 'f', 't' and 's' */
	const uint8_t *data; /* 'd' */
	size_t data_len;
	unsigned int max_width; /* max-graphic-size */
	unsigned int max_height;
	uint64_t max_bytes; /* image-storage-limit, the whole budget */
};

/*
 * A request's end: @error 0 with @width x @height pixels of straight-alpha
 * RGBA at @rgba, valid only during the call, or -EBADF, -EINVAL or -ENOSPC.
 * Called from the loop, never from image_host_send; a request cancelled
 * gets none.
 */
typedef void (*image_host_reply_cb)(struct image_host *host, uint64_t id, int error,
				    unsigned int width, unsigned int height, const uint8_t *rgba,
				    void *data);

/* @helper is the executable's absolute path; its timers and descriptors go on @loop */
int image_host_new(struct image_host **out, struct ev_eloop *loop, const char *helper,
		   image_host_reply_cb cb, void *data);
/* kills the helper; a request out gets no reply */
void image_host_free(struct image_host *host);

/*
 * Sends @req to the helper running as @uid, first starting one when none
 * runs, when the running one has another user, or when its memory limit was
 * set from a smaller max_bytes. Returns 0 when the request is out, -EBUSY
 * when one already is, or -EBADF when no helper could take it.
 */
int image_host_send(struct image_host *host, uid_t uid, const struct image_host_request *req);
/* kills the helper and drops the request out, without a reply */
void image_host_cancel(struct image_host *host);
/* a request is out */
bool image_host_busy(struct image_host *host);

/* for tests: the running helper's pid, or 0 */
pid_t image_host_pid(struct image_host *host);
void image_host_set_timeout(struct image_host *host, unsigned int ms);
/* why the helper was last stopped for a failure, or NULL */
const char *image_host_last_failure(struct image_host *host);

#endif /* KMSCON_IMAGE_HOST_H */
