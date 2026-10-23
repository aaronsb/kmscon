/*
 * Kmscon - FBDEV Video backend
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
 * FBDEV module rendering functions
 */

#include <errno.h>
#include <stdbool.h>
#include "fbdev_internal.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "video.h"
#include "video_internal.h"

#define LOG_SUBSYSTEM "fbdev_render"

static int clamp_value(int val, int low, int up)
{
	if (val < low)
		return low;
	else if (val > up)
		return up;
	else
		return val;
}

/* the device's channel layout, read once per request */
struct px_fmt {
	unsigned int off_r, off_g, off_b;
	unsigned int len_r, len_g, len_b;
	bool dither;
};

/* the dither error carried from pixel to pixel, held in locals during a loop */
struct px_dither {
	int_fast32_t r, g, b;
};

static inline struct px_fmt px_fmt_of(const struct display *disp)
{
	const struct fbdev_display *fbdev = disp->data;
	struct px_fmt f = {
		.off_r = fbdev->off_r,
		.off_g = fbdev->off_g,
		.off_b = fbdev->off_b,
		.len_r = fbdev->len_r,
		.len_g = fbdev->len_g,
		.len_b = fbdev->len_b,
	};

	/*
	 * With 8-bit channels the dither error is r - r = 0 after every pixel,
	 * so dithering writes what the plain conversion writes. The error is
	 * zeroed whenever the mode is set, and stays zero here.
	 */
	f.dither = (disp->flags & DISPLAY_DITHERING) &&
		   !(f.len_r == 8 && f.len_g == 8 && f.len_b == 8);
	return f;
}

static inline struct px_dither dither_load(const struct fbdev_display *fbdev)
{
	struct px_dither d = {fbdev->dither_r, fbdev->dither_g, fbdev->dither_b};

	return d;
}

static inline void dither_save(struct fbdev_display *fbdev, const struct px_dither *d)
{
	fbdev->dither_r = d->r;
	fbdev->dither_g = d->g;
	fbdev->dither_b = d->b;
}

/* XRGB32 @pixel in the device's channel layout */
static inline uint_fast32_t to_device(const struct px_fmt *f, struct px_dither *d, uint32_t pixel)
{
	uint8_t r, g, b, nr, ng, nb;
	int i;
	uint_fast32_t res;

	r = (pixel >> 16) & 0xff;
	g = (pixel >> 8) & 0xff;
	b = (pixel >> 0) & 0xff;

	if (f->dither) {
		/* This is some very basic dithering which simply does small
		 * rotations in the lower pixel bits. TODO: Let's take a look
		 * at Floyd-Steinberg dithering which should give much better
		 * results. It is slightly slower, though.
		 * Or even better would be some Sierra filter like the Sierra
		 * LITE. */
		d->r = r - d->r;
		d->g = g - d->g;
		d->b = b - d->b;
		r = clamp_value(d->r, 0, 255) >> (8 - f->len_r);
		g = clamp_value(d->g, 0, 255) >> (8 - f->len_g);
		b = clamp_value(d->b, 0, 255) >> (8 - f->len_b);
		nr = r << (8 - f->len_r);
		ng = g << (8 - f->len_g);
		nb = b << (8 - f->len_b);

		for (i = f->len_r; i < 8; i <<= 1)
			nr |= nr >> i;
		for (i = f->len_g; i < 8; i <<= 1)
			ng |= ng >> i;
		for (i = f->len_b; i < 8; i <<= 1)
			nb |= nb >> i;

		d->r = nr - d->r;
		d->g = ng - d->g;
		d->b = nb - d->b;

		res = r << f->off_r;
		res |= g << f->off_g;
		res |= b << f->off_b;
	} else {
		res = (r >> (8 - f->len_r)) << f->off_r;
		res |= (g >> (8 - f->len_g)) << f->off_g;
		res |= (b >> (8 - f->len_b)) << f->off_b;
	}

	return res;
}

static void write_24bit(uint8_t *dst, uint_fast32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	dst[0] = value;
	dst[1] = value >> 8;
	dst[2] = value >> 16;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	dst[0] = value >> 16;
	dst[1] = value >> 8;
	dst[2] = value;
#else
#error "Unknown endianness"
#endif
}

/*
 * Division by 256 instead of 255 increases speed by like 20% on slower
 * machines. Downside is, full white is 254/254/254 instead of 255/255/255.
 * Mask values 0 and 255, and alpha 0 and 255, are exact.
 */

/* @fg painted over @bg through mask value @a, as XRGB32 */
static inline uint32_t blend_mask(const struct video_blend_req *req, uint8_t a)
{
	unsigned int r, g, b;

	if (a == 0)
		return (req->br << 16) | (req->bg << 8) | req->bb;
	if (a == 255)
		return (req->fr << 16) | (req->fg << 8) | req->fb;

	r = (req->fr * a + req->br * (255 - a)) / 256;
	g = (req->fg * a + req->bg * (255 - a)) / 256;
	b = (req->fb * a + req->bb * (255 - a)) / 256;
	return (r << 16) | (g << 8) | b;
}

/*
 * Premultiplied @src over the request's background (ADR-102), as XRGB32:
 * out = src + bg * (255 - a) / 256. The renderer fills the cell's own
 * background in the same request, so the destination is never read back.
 */
static inline uint32_t blend_argb(const struct video_blend_req *req, uint32_t src)
{
	unsigned int a = src >> 24, na = 255 - a;
	unsigned int r, g, b;

	if (a == 0)
		return (req->br << 16) | (req->bg << 8) | req->bb;
	if (a == 255)
		return src & 0xffffff;

	r = ((src >> 16) & 0xff) + req->br * na / 256;
	g = ((src >> 8) & 0xff) + req->bg * na / 256;
	b = (src & 0xff) + req->bb * na / 256;
	return (r << 16) | (g << 8) | b;
}

/* pixel @i of a source row, blended */
static inline uint32_t blend_px(const struct video_blend_req *req, bool argb, const uint8_t *src,
				unsigned int i)
{
	if (argb)
		return blend_argb(req, ((const uint32_t *)src)[i]);
	return blend_mask(req, src[i]);
}

/*
 * This loop runs for every pixel of every glyph drawn, so it follows the
 * hot-loop rule in docs/fork/development.md: what it reads is copied into
 * const locals first (the build's -fno-strict-aliasing makes every store to
 * the device reload anything read through a pointer), and the depth is
 * chosen once, with one loop per depth that differs only in its store.
 */
int fbdev_display_blend(struct display *disp, const struct video_blend_req *reqp)
{
	unsigned int tmp;
	uint8_t *dst;
	const uint8_t *src;
	unsigned int width, height, i;
	struct fbdev_display *fbdev = disp->data;

	if (!reqp || !reqp->buf)
		return -EINVAL;

	if (!fbdev->xrgb32 && (fbdev->Bpp < 2 || fbdev->Bpp > 4)) {
		log_warning("invalid Bpp");
		return 0;
	}

	width = min(reqp->w, reqp->buf->width);
	tmp = reqp->x + width;
	if (tmp < reqp->x || reqp->x >= fbdev->xres)
		return -EINVAL;
	if (tmp > fbdev->xres)
		width = fbdev->xres - reqp->x;

	height = min(reqp->h, reqp->buf->height);
	tmp = reqp->y + height;
	if (tmp < reqp->y || reqp->y >= fbdev->yres)
		return -EINVAL;
	if (tmp > fbdev->yres)
		height = fbdev->yres - reqp->y;

	if (!(disp->flags & DISPLAY_DBUF) || fbdev->bufid)
		dst = fbdev->map;
	else
		dst = &fbdev->map[fbdev->yres * fbdev->stride];
	dst = &dst[reqp->y * fbdev->stride + reqp->x * fbdev->Bpp];
	src = reqp->buf->data;

	const struct video_blend_req req = *reqp;
	const bool argb = req.buf->format == VIDEO_FORMAT_ARGB32;
	const unsigned int src_stride = req.buf->width * (argb ? 4 : 1);
	const unsigned int dst_stride = fbdev->stride;
	const unsigned int depth = fbdev->xrgb32 ? 0 : fbdev->Bpp;
	const struct px_fmt f = px_fmt_of(disp);
	struct px_dither d = dither_load(fbdev);

	switch (depth) {
	case 0: /* XRGB32: the blend's own format */
		for (; height--; dst += dst_stride, src += src_stride)
			for (i = 0; i < width; ++i)
				((uint32_t *)dst)[i] = blend_px(&req, argb, src, i);
		break;
	case 2:
		for (; height--; dst += dst_stride, src += src_stride)
			for (i = 0; i < width; ++i)
				((uint16_t *)dst)[i] =
					to_device(&f, &d, blend_px(&req, argb, src, i));
		break;
	case 3:
		for (; height--; dst += dst_stride, src += src_stride)
			for (i = 0; i < width; ++i)
				write_24bit(&dst[i * 3],
					    to_device(&f, &d, blend_px(&req, argb, src, i)));
		break;
	case 4:
		for (; height--; dst += dst_stride, src += src_stride)
			for (i = 0; i < width; ++i)
				((uint32_t *)dst)[i] =
					to_device(&f, &d, blend_px(&req, argb, src, i));
		break;
	}

	dither_save(fbdev, &d);
	return 0;
}

int fbdev_display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	unsigned int i;
	uint8_t *dst;
	uint32_t full_val;
	struct fbdev_display *fbdev = disp->data;
	const struct px_fmt f = px_fmt_of(disp);
	unsigned int width = fbdev->xres;
	unsigned int height = fbdev->yres;

	if (!(disp->flags & DISPLAY_DBUF) || fbdev->bufid)
		dst = fbdev->map;
	else
		dst = &fbdev->map[fbdev->yres * fbdev->stride];

	full_val = ((r & 0xff) >> (8 - fbdev->len_r)) << fbdev->off_r;
	full_val |= ((g & 0xff) >> (8 - fbdev->len_g)) << fbdev->off_g;
	full_val |= ((b & 0xff) >> (8 - fbdev->len_b)) << fbdev->off_b;

	if (fbdev->Bpp == 2) {
		if (f.dither) {
			struct px_dither d = dither_load(fbdev);
			const uint32_t rgb32 = (r & 0xff) << 16 | (g & 0xff) << 8 | (b & 0xff);
			const unsigned int dst_stride = fbdev->stride;

			for (; height--; dst += dst_stride)
				for (i = 0; i < width; ++i)
					((uint16_t *)dst)[i] = to_device(&f, &d, rgb32);
			dither_save(fbdev, &d);
		} else {
			full_val &= 0xffff;
			while (height--) {
				for (i = 0; i < width; ++i)
					((uint16_t *)dst)[i] = full_val;
				dst += fbdev->stride;
			}
		}
	} else if (fbdev->Bpp == 3) {
		while (height--) {
			for (i = 0; i < width * 3; i += 3) {
				write_24bit(&dst[i], full_val);
			}
			dst += fbdev->stride;
		}
	} else if (fbdev->Bpp == 4) {
		while (height--) {
			for (i = 0; i < width; ++i)
				((uint32_t *)dst)[i] = full_val;
			dst += fbdev->stride;
		}
	} else {
		log_error("invalid Bpp");
		return -EFAULT;
	}

	return 0;
}
