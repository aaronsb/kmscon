/*
 * kmscon - Decoration sprites
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
 * The sprite shapes follow figure ADR-102-decoration-sprites.svg:
 * - single: the font's underline, full width;
 * - double: the single line and a second one a thickness below it;
 * - curly: one sine period per cell, starting at its centre line and going
 *   down first, in the space from the single line's top downwards;
 * - dotted: square dots a thickness wide, as many per cell as fit with a gap
 *   of the same size, centred in their slots;
 * - dashed: one dash per cell over its middle two thirds, so dashes are
 *   separated by a third of a cell;
 * - strike: the font's strike-through, full width.
 * Lines that would leave the cell are moved up into it.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "deco.h"
#include "shl/misc.h"

/* samples per pixel side for the curly wave's coverage */
#define CURLY_SS 8

static void fill_rows(struct video_buffer *buf, unsigned int x0, unsigned int x1, unsigned int y0,
		      unsigned int rows)
{
	unsigned int y;

	if (x1 > buf->width)
		x1 = buf->width;
	for (y = y0; y < y0 + rows && y < buf->height; y++)
		if (x1 > x0)
			memset(&buf->data[y * buf->width + x0], 0xff, x1 - x0);
}

/* move a band of @rows starting at @top up so it ends inside @h */
static unsigned int fit(unsigned int top, unsigned int rows, unsigned int h)
{
	if (top + rows <= h)
		return top;
	return rows >= h ? 0 : h - rows;
}

static void draw_curly(struct video_buffer *buf, const struct kmscon_font_deco *m)
{
	unsigned int w = buf->width, h = buf->height;
	unsigned int t = m->ul_thick;
	unsigned int amp = w / 4 > 2 ? w / 4 : 2; /* peak to peak */
	unsigned int top = fit(m->ul_pos, amp + t, h);
	double mid = top + (amp + t) / 2.0;
	double k = 2.0 * M_PI / w;
	unsigned int x, y, sx, sy, hits;

	for (y = top; y < top + amp + t && y < h; y++) {
		for (x = 0; x < w; x++) {
			hits = 0;
			for (sx = 0; sx < CURLY_SS; sx++) {
				double fx = x + (sx + 0.5) / CURLY_SS;
				double yc = mid + amp / 2.0 * sin(k * fx);
				double slope = amp / 2.0 * k * cos(k * fx);
				/* a stroke of thickness t, measured across the curve */
				double half = t / 2.0 * sqrt(1.0 + slope * slope);

				for (sy = 0; sy < CURLY_SS; sy++) {
					double fy = y + (sy + 0.5) / CURLY_SS;

					if (fabs(fy - yc) <= half)
						hits++;
				}
			}
			buf->data[y * w + x] = hits * 255 / (CURLY_SS * CURLY_SS);
		}
	}
}

static void draw_dotted(struct video_buffer *buf, const struct kmscon_font_deco *m)
{
	unsigned int w = buf->width, t = m->ul_thick;
	unsigned int n = (w + t) / (2 * t), i;
	double slot;

	if (!n)
		n = 1;
	slot = (double)w / n;
	for (i = 0; i < n; i++) {
		unsigned int x0 = floor(i * slot + slot / 4 + 0.5);
		unsigned int x1 = floor(i * slot + 3 * slot / 4 + 0.5);

		if (x1 <= x0)
			x1 = x0 + 1;
		fill_rows(buf, x0, x1, m->ul_pos, t);
	}
}

SHL_EXPORT
struct kmscon_glyph *kmscon_deco_render(enum kmscon_deco kind, unsigned int w, unsigned int h,
					const struct kmscon_font_deco *m)
{
	struct kmscon_glyph *g;
	unsigned int t = m->ul_thick, top;

	if (!w || !h || kind <= KMSCON_DECO_NONE || kind >= KMSCON_DECO_COUNT)
		return NULL;

	g = calloc(1, sizeof(*g) + w * h);
	if (!g)
		return NULL;
	g->buf.width = w;
	g->buf.height = h;
	g->buf.format = VIDEO_FORMAT_A8;

	switch (kind) {
	case KMSCON_DECO_SINGLE:
		fill_rows(&g->buf, 0, w, m->ul_pos, t);
		break;
	case KMSCON_DECO_DOUBLE:
		top = fit(m->ul_pos, 3 * t, h);
		fill_rows(&g->buf, 0, w, top, t);
		fill_rows(&g->buf, 0, w, top + 2 * t, t);
		break;
	case KMSCON_DECO_CURLY:
		draw_curly(&g->buf, m);
		break;
	case KMSCON_DECO_DOTTED:
		draw_dotted(&g->buf, m);
		break;
	case KMSCON_DECO_DASHED:
		fill_rows(&g->buf, (w + 3) / 6, (5 * w + 3) / 6, m->ul_pos, t);
		break;
	case KMSCON_DECO_STRIKE:
		fill_rows(&g->buf, 0, w, m->strike_pos, m->strike_thick);
		break;
	default:
		break;
	}
	return g;
}
