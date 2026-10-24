/*
 * kmscon - Pictures for the image layer
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
 * The scaler is separable: each output pixel is a weighted sum of source
 * pixels, with one list of weights per output column and one per output row.
 * Weights are 16-bit fixed point and each list sums to exactly 1 << 16, so an
 * opaque picture stays opaque and premultiplied channels never pass their
 * alpha. Source rows are filtered across once each, into a ring holding as
 * many rows as one output row needs, then down. The ring is bounded: output
 * columns go in slices narrow enough that a ring of a slice holds at most
 * RING_MAX sums, whatever the shrink.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "picture.h"

#define ONE 65536u
#define RING_MAX (1u << 20)

/* the weights of each output pixel along one axis, [o0, o1) */
struct taps {
	unsigned int *first; /* the first source pixel */
	unsigned int *count;
	uint32_t *w; /* @max per output pixel */
	unsigned int max;
};

static void taps_free(struct taps *t)
{
	free(t->first);
	free(t->count);
	free(t->w);
}

static int taps_make(struct taps *t, unsigned int s, unsigned int d, unsigned int o0,
		     unsigned int o1)
{
	unsigned int n = o1 - o0, o, i, k;

	t->max = s > d ? s / d + 2 : 2;
	t->first = calloc(n, sizeof(*t->first));
	t->count = calloc(n, sizeof(*t->count));
	t->w = calloc((size_t)n * t->max, sizeof(*t->w));
	if (!t->first || !t->count || !t->w) {
		taps_free(t);
		return -ENOMEM;
	}

	for (o = o0; o < o1; o++) {
		unsigned int *first = &t->first[o - o0], *count = &t->count[o - o0];
		uint32_t *w = &t->w[(size_t)(o - o0) * t->max];

		if (s == d) {
			*first = o;
			*count = 1;
			w[0] = ONE;
		} else if (s > d) {
			/* box: the source span [o * s, (o + 1) * s) in units
			 * where a source pixel is @d wide */
			uint64_t lo = (uint64_t)o * s, hi = lo + s;
			uint32_t sum = 0;
			unsigned int big = 0;

			*first = lo / d;
			*count = 0;
			for (i = *first; i < s && (uint64_t)i * d < hi && *count < t->max; i++) {
				uint64_t a = (uint64_t)i * d, b = a + d;
				uint64_t ov = (hi < b ? hi : b) - (lo > a ? lo : a);

				k = (*count)++;
				w[k] = ov * ONE / s;
				sum += w[k];
				if (w[k] > w[big])
					big = k;
			}
			w[big] += ONE - sum;
		} else {
			/* bilinear: the output pixel's centre in source pixels */
			int64_t p = (int64_t)(2 * (uint64_t)o + 1) * s * ONE / (2 * (uint64_t)d) -
				    ONE / 2;
			uint32_t f;

			if (p < 0)
				p = 0;
			i = p / ONE;
			f = p % ONE;
			if (i >= s - 1) {
				i = s - 1;
				f = 0;
			}
			*first = i;
			*count = f ? 2 : 1;
			w[0] = ONE - f;
			w[1] = f;
		}
	}
	return 0;
}

static inline unsigned int chan(uint32_t p, unsigned int c)
{
	return (p >> (8 * c)) & 0xff;
}

/* the output columns [@c0, @c1) of every output row, into @dst, @nx a row */
static void scale_slice(const uint32_t *src, size_t stride, const struct taps *tx,
			const struct taps *ty, unsigned int ny, unsigned int c0, unsigned int c1,
			unsigned int nx, uint32_t *ring, long *ring_row, uint32_t *dst)
{
	unsigned int w = c1 - c0, x, y, k, c, r, slot;

	for (k = 0; k < ty->max; k++)
		ring_row[k] = -1;

	for (y = 0; y < ny; y++) {
		const uint32_t *wy = &ty->w[(size_t)y * ty->max];

		/* the rows this output row reads span at most ty->max, so a row
		 * is never replaced while it is needed */
		for (k = 0; k < ty->count[y]; k++) {
			r = ty->first[y] + k;
			slot = r % ty->max;
			if (ring_row[slot] == (long)r)
				continue;
			ring_row[slot] = r;
			for (x = 0; x < w; x++) {
				const uint32_t *wx = &tx->w[(size_t)(c0 + x) * tx->max];
				const uint32_t *s = &src[(size_t)r * stride + tx->first[c0 + x]];
				uint32_t *h = &ring[((size_t)slot * w + x) * 4];

				h[0] = h[1] = h[2] = h[3] = 0;
				for (unsigned int i = 0; i < tx->count[c0 + x]; i++)
					for (c = 0; c < 4; c++)
						h[c] += wx[i] * chan(s[i], c);
			}
		}

		for (x = 0; x < w; x++) {
			uint64_t acc[4] = {0};
			uint32_t out = 0;

			for (k = 0; k < ty->count[y]; k++) {
				const uint32_t *h =
					&ring[(((ty->first[y] + k) % ty->max) * (size_t)w + x) * 4];

				for (c = 0; c < 4; c++)
					acc[c] += (uint64_t)wy[k] * h[c];
			}
			for (c = 0; c < 4; c++) {
				uint64_t v = (acc[c] + (1ull << 31)) >> 32;

				out |= (uint32_t)(v > 255 ? 255 : v) << (8 * c);
			}
			dst[(size_t)y * nx + c0 + x] = out;
		}
	}
}

int picture_scale(const uint32_t *src, size_t stride, unsigned int sw, unsigned int sh,
		  unsigned int dw, unsigned int dh, unsigned int x0, unsigned int y0,
		  unsigned int x1, unsigned int y1, uint32_t *dst)
{
	struct taps tx = {0}, ty = {0};
	unsigned int nx = x1 - x0, slice, c0;
	uint32_t *ring = NULL;
	long *ring_row = NULL;
	int ret;

	if (!src || !dst || !sw || !sh || !dw || !dh || stride < sw || x0 >= x1 || y0 >= y1 ||
	    x1 > dw || y1 > dh)
		return -EINVAL;

	ret = taps_make(&tx, sw, dw, x0, x1);
	if (!ret)
		ret = taps_make(&ty, sh, dh, y0, y1);
	if (ret)
		goto out;

	/* rows filtered across, each a slice of four 24-bit sums a pixel */
	slice = RING_MAX / ty.max;
	if (!slice)
		slice = 1;
	if (slice > nx)
		slice = nx;
	ring = malloc((size_t)ty.max * slice * 4 * sizeof(*ring));
	ring_row = malloc(ty.max * sizeof(*ring_row));
	if (!ring || !ring_row) {
		ret = -ENOMEM;
		goto out;
	}
	for (c0 = 0; c0 < nx; c0 += slice)
		scale_slice(src, stride, &tx, &ty, y1 - y0, c0, c0 + slice < nx ? c0 + slice : nx,
			    nx, ring, ring_row, dst);

out:
	free(ring);
	free(ring_row);
	taps_free(&tx);
	taps_free(&ty);
	return ret;
}

/* the orientations of enum Orientation (text.h) */
#define ROT_NORMAL 0
#define ROT_RIGHT 1
#define ROT_UPSIDE_DOWN 2
#define ROT_LEFT 3

/* picture_rotate() for one pixel size, which each caller passes as a constant */
static inline void rotate(const uint8_t *s, unsigned int w, unsigned int h, size_t bpp, uint8_t *d,
			  int orientation)
{
	unsigned int i, j;

#define PX(p, n) (&(p)[(size_t)(n) * bpp])
	switch (orientation) {
	default:
	case ROT_NORMAL:
		memcpy(d, s, (size_t)w * h * bpp);
		break;
	case ROT_RIGHT:
		/* source row i becomes the column h - 1 - i, top to bottom */
		for (i = 0; i < h; i++)
			for (j = 0; j < w; j++)
				memcpy(PX(d, (size_t)j * h + (h - 1 - i)), PX(s, (size_t)i * w + j),
				       bpp);
		break;
	case ROT_UPSIDE_DOWN:
		for (i = 0; i < h; i++)
			for (j = 0; j < w; j++)
				memcpy(PX(d, (size_t)(h - 1 - i) * w + (w - 1 - j)),
				       PX(s, (size_t)i * w + j), bpp);
		break;
	case ROT_LEFT:
		/* source row i becomes the column i, bottom to top */
		for (i = 0; i < h; i++)
			for (j = 0; j < w; j++)
				memcpy(PX(d, (size_t)(w - 1 - j) * h + i), PX(s, (size_t)i * w + j),
				       bpp);
		break;
	}
#undef PX
}

void picture_rotate(const void *src, unsigned int w, unsigned int h, size_t bpp, void *dst,
		    int orientation)
{
	/* a constant size makes each copy a plain load and store, not a call */
	if (bpp == 4)
		rotate(src, w, h, 4, dst, orientation);
	else if (bpp == 1)
		rotate(src, w, h, 1, dst, orientation);
	else
		rotate(src, w, h, bpp, dst, orientation);
}
