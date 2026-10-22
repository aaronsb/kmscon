/*
 * Kmscon - DRM2D Video backend
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * DRM2D Video backend rendering functions
 */

#include <fcntl.h>
#include <inttypes.h>
#include <shl/misc.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "drm2d_internal.h"
#include "video.h"
#include "video_internal.h"

#define LOG_SUBSYSTEM "drm2d_render"

/* t / 255 rounded, exact for t up to 255 * 255, without a division (~20%
 * faster): t += 0x80; t = (t + (t >> 8)) >> 8 */
static inline uint_fast32_t div255(uint_fast32_t t)
{
	t += 0x80;
	return (t + (t >> 8)) >> 8;
}

/* @fg painted over @bg through mask value @a */
static inline uint32_t blend_mask(const struct video_blend_req *req, uint8_t a)
{
	uint_fast32_t r, g, b;

	if (a == 0)
		return (req->br << 16) | (req->bg << 8) | req->bb;
	if (a == 255)
		return (req->fr << 16) | (req->fg << 8) | req->fb;

	r = div255(req->fr * a + req->br * (255 - a));
	g = div255(req->fg * a + req->bg * (255 - a));
	b = div255(req->fb * a + req->bb * (255 - a));
	return (r << 16) | (g << 8) | b;
}

/*
 * Premultiplied @src over the request's background (ADR-102):
 * out = src + bg * (255 - a) / 255. The renderer fills the cell's own
 * background in the same request, so the destination is never read back.
 */
static inline uint32_t blend_argb(const struct video_blend_req *req, uint32_t src)
{
	uint_fast32_t a = src >> 24, na = 255 - a;
	uint_fast32_t r, g, b;

	if (a == 0)
		return (req->br << 16) | (req->bg << 8) | req->bb;
	if (a == 255)
		return src & 0xffffff;

	r = ((src >> 16) & 0xff) + div255(req->br * na);
	g = ((src >> 8) & 0xff) + div255(req->bg * na);
	b = (src & 0xff) + div255(req->bb * na);
	return (r << 16) | (g << 8) | b;
}

int drm2d_display_blend(struct display *disp, const struct video_blend_req *req)
{
	unsigned int tmp;
	uint8_t *dst;
	const uint8_t *src;
	unsigned int width, height, i, src_stride;
	unsigned int sw, sh;
	struct drm2d_rb *rb;
	struct drm2d_display *d2d = disp->data;
	bool argb;

	if (!req || !req->buf)
		return -EINVAL;

	rb = &d2d->rb[d2d->current_rb ^ 1];
	sw = disp->width;
	sh = disp->height;

	width = min(req->w, req->buf->width);
	tmp = req->x + width;
	if (tmp < req->x || req->x >= sw)
		return -EINVAL;
	if (tmp > sw)
		width = sw - req->x;

	height = min(req->h, req->buf->height);
	tmp = req->y + height;
	if (tmp < req->y || req->y >= sh)
		return -EINVAL;
	if (tmp > sh)
		height = sh - req->y;

	argb = req->buf->format == VIDEO_FORMAT_ARGB32;
	src_stride = req->buf->width * (argb ? 4 : 1);

	dst = rb->map;
	dst = &dst[req->y * rb->stride + req->x * 4];
	src = req->buf->data;

	while (height--) {
		if (argb) {
			for (i = 0; i < width; ++i)
				((uint32_t *)dst)[i] = blend_argb(req, ((const uint32_t *)src)[i]);
		} else {
			for (i = 0; i < width; ++i)
				((uint32_t *)dst)[i] = blend_mask(req, src[i]);
		}
		dst += rb->stride;
		src += src_stride;
	}
	return 0;
}

int drm2d_display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	struct drm2d_rb *rb;
	struct drm2d_display *d2d = disp->data;
	unsigned int width = disp->width;
	unsigned int height = disp->height;
	uint8_t *dst;
	int i;

	rb = &d2d->rb[d2d->current_rb ^ 1];
	dst = rb->map;

	while (height--) {
		for (i = 0; i < width; ++i)
			((uint32_t *)dst)[i] = (r << 16) | (g << 8) | b;
		dst += rb->stride;
	}

	return 0;
}
