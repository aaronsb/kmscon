/*
 * kmscon - Box drawing and block element sprites
 *
 * Copyright (c) 2026 the kmscon fork authors
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
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
 * The geometry is adapted from Ghostty's sprite font (MIT, Copyright (c)
 * 2024 Mitchell Hashimoto, Ghostty contributors), src/font/sprite/draw/:
 * - lines and junctions: box.zig linesChar(), each edge's line meeting the
 *   others at the centre, light lines the font's underline thickness and heavy
 *   ones twice that, double lines two light strokes a light stroke apart;
 * - dashes: box.zig dashHorizontal() and dashVertical(), which divide the
 *   cell so the dashes tile across cells;
 * - rounded corners: box.zig arc(), a cubic curve between the two half lines;
 * - diagonals: box.zig lightDiagonal*(), corner to corner, overshooting the
 *   corners slightly so neighbours join;
 * - blocks: block.zig blockShade() and quadrant(), with common.zig
 *   Fraction.min()/max() rounding, so two halves of an odd cell overlap by a
 *   pixel rather than leave one empty;
 * - shades: block.zig fullBlockShade(), the whole cell at 25%, 50% and 75%
 *   coverage (common.zig Shade), which the renderer blends like any glyph.
 * Curves and diagonals are drawn by supersampling instead of Ghostty's path
 * rasteriser.
 */

#include <stdlib.h>
#include <string.h>
#include "font_box.h"

/* samples per pixel side for curves and diagonals */
#define BOX_SS 8

/* line styles, two bits per edge */
enum {
	BOX_N = 0, /* none */
	BOX_L = 1, /* light */
	BOX_H = 2, /* heavy */
	BOX_D = 3, /* double */
};

#define BOX_LINES(up, right, down, left) ((up) | (right) << 2 | (down) << 4 | (left) << 6)

/* U+2500-U+257F made of straight lines from the edges to the centre; 0 for
 * the dashes, curves and diagonals, which are drawn otherwise */
static const uint8_t box_lines[0x80] = {
	[0x00] = BOX_LINES(BOX_N, BOX_L, BOX_N, BOX_L),
	[0x01] = BOX_LINES(BOX_N, BOX_H, BOX_N, BOX_H),
	[0x02] = BOX_LINES(BOX_L, BOX_N, BOX_L, BOX_N),
	[0x03] = BOX_LINES(BOX_H, BOX_N, BOX_H, BOX_N),
	[0x0c] = BOX_LINES(BOX_N, BOX_L, BOX_L, BOX_N),
	[0x0d] = BOX_LINES(BOX_N, BOX_H, BOX_L, BOX_N),
	[0x0e] = BOX_LINES(BOX_N, BOX_L, BOX_H, BOX_N),
	[0x0f] = BOX_LINES(BOX_N, BOX_H, BOX_H, BOX_N),
	[0x10] = BOX_LINES(BOX_N, BOX_N, BOX_L, BOX_L),
	[0x11] = BOX_LINES(BOX_N, BOX_N, BOX_L, BOX_H),
	[0x12] = BOX_LINES(BOX_N, BOX_N, BOX_H, BOX_L),
	[0x13] = BOX_LINES(BOX_N, BOX_N, BOX_H, BOX_H),
	[0x14] = BOX_LINES(BOX_L, BOX_L, BOX_N, BOX_N),
	[0x15] = BOX_LINES(BOX_L, BOX_H, BOX_N, BOX_N),
	[0x16] = BOX_LINES(BOX_H, BOX_L, BOX_N, BOX_N),
	[0x17] = BOX_LINES(BOX_H, BOX_H, BOX_N, BOX_N),
	[0x18] = BOX_LINES(BOX_L, BOX_N, BOX_N, BOX_L),
	[0x19] = BOX_LINES(BOX_L, BOX_N, BOX_N, BOX_H),
	[0x1a] = BOX_LINES(BOX_H, BOX_N, BOX_N, BOX_L),
	[0x1b] = BOX_LINES(BOX_H, BOX_N, BOX_N, BOX_H),
	[0x1c] = BOX_LINES(BOX_L, BOX_L, BOX_L, BOX_N),
	[0x1d] = BOX_LINES(BOX_L, BOX_H, BOX_L, BOX_N),
	[0x1e] = BOX_LINES(BOX_H, BOX_L, BOX_L, BOX_N),
	[0x1f] = BOX_LINES(BOX_L, BOX_L, BOX_H, BOX_N),
	[0x20] = BOX_LINES(BOX_H, BOX_L, BOX_H, BOX_N),
	[0x21] = BOX_LINES(BOX_H, BOX_H, BOX_L, BOX_N),
	[0x22] = BOX_LINES(BOX_L, BOX_H, BOX_H, BOX_N),
	[0x23] = BOX_LINES(BOX_H, BOX_H, BOX_H, BOX_N),
	[0x24] = BOX_LINES(BOX_L, BOX_N, BOX_L, BOX_L),
	[0x25] = BOX_LINES(BOX_L, BOX_N, BOX_L, BOX_H),
	[0x26] = BOX_LINES(BOX_H, BOX_N, BOX_L, BOX_L),
	[0x27] = BOX_LINES(BOX_L, BOX_N, BOX_H, BOX_L),
	[0x28] = BOX_LINES(BOX_H, BOX_N, BOX_H, BOX_L),
	[0x29] = BOX_LINES(BOX_H, BOX_N, BOX_L, BOX_H),
	[0x2a] = BOX_LINES(BOX_L, BOX_N, BOX_H, BOX_H),
	[0x2b] = BOX_LINES(BOX_H, BOX_N, BOX_H, BOX_H),
	[0x2c] = BOX_LINES(BOX_N, BOX_L, BOX_L, BOX_L),
	[0x2d] = BOX_LINES(BOX_N, BOX_L, BOX_L, BOX_H),
	[0x2e] = BOX_LINES(BOX_N, BOX_H, BOX_L, BOX_L),
	[0x2f] = BOX_LINES(BOX_N, BOX_H, BOX_L, BOX_H),
	[0x30] = BOX_LINES(BOX_N, BOX_L, BOX_H, BOX_L),
	[0x31] = BOX_LINES(BOX_N, BOX_L, BOX_H, BOX_H),
	[0x32] = BOX_LINES(BOX_N, BOX_H, BOX_H, BOX_L),
	[0x33] = BOX_LINES(BOX_N, BOX_H, BOX_H, BOX_H),
	[0x34] = BOX_LINES(BOX_L, BOX_L, BOX_N, BOX_L),
	[0x35] = BOX_LINES(BOX_L, BOX_L, BOX_N, BOX_H),
	[0x36] = BOX_LINES(BOX_L, BOX_H, BOX_N, BOX_L),
	[0x37] = BOX_LINES(BOX_L, BOX_H, BOX_N, BOX_H),
	[0x38] = BOX_LINES(BOX_H, BOX_L, BOX_N, BOX_L),
	[0x39] = BOX_LINES(BOX_H, BOX_L, BOX_N, BOX_H),
	[0x3a] = BOX_LINES(BOX_H, BOX_H, BOX_N, BOX_L),
	[0x3b] = BOX_LINES(BOX_H, BOX_H, BOX_N, BOX_H),
	[0x3c] = BOX_LINES(BOX_L, BOX_L, BOX_L, BOX_L),
	[0x3d] = BOX_LINES(BOX_L, BOX_L, BOX_L, BOX_H),
	[0x3e] = BOX_LINES(BOX_L, BOX_H, BOX_L, BOX_L),
	[0x3f] = BOX_LINES(BOX_L, BOX_H, BOX_L, BOX_H),
	[0x40] = BOX_LINES(BOX_H, BOX_L, BOX_L, BOX_L),
	[0x41] = BOX_LINES(BOX_L, BOX_L, BOX_H, BOX_L),
	[0x42] = BOX_LINES(BOX_H, BOX_L, BOX_H, BOX_L),
	[0x43] = BOX_LINES(BOX_H, BOX_L, BOX_L, BOX_H),
	[0x44] = BOX_LINES(BOX_H, BOX_H, BOX_L, BOX_L),
	[0x45] = BOX_LINES(BOX_L, BOX_L, BOX_H, BOX_H),
	[0x46] = BOX_LINES(BOX_L, BOX_H, BOX_H, BOX_L),
	[0x47] = BOX_LINES(BOX_H, BOX_H, BOX_L, BOX_H),
	[0x48] = BOX_LINES(BOX_L, BOX_H, BOX_H, BOX_H),
	[0x49] = BOX_LINES(BOX_H, BOX_L, BOX_H, BOX_H),
	[0x4a] = BOX_LINES(BOX_H, BOX_H, BOX_H, BOX_L),
	[0x4b] = BOX_LINES(BOX_H, BOX_H, BOX_H, BOX_H),
	[0x50] = BOX_LINES(BOX_N, BOX_D, BOX_N, BOX_D),
	[0x51] = BOX_LINES(BOX_D, BOX_N, BOX_D, BOX_N),
	[0x52] = BOX_LINES(BOX_N, BOX_D, BOX_L, BOX_N),
	[0x53] = BOX_LINES(BOX_N, BOX_L, BOX_D, BOX_N),
	[0x54] = BOX_LINES(BOX_N, BOX_D, BOX_D, BOX_N),
	[0x55] = BOX_LINES(BOX_N, BOX_N, BOX_L, BOX_D),
	[0x56] = BOX_LINES(BOX_N, BOX_N, BOX_D, BOX_L),
	[0x57] = BOX_LINES(BOX_N, BOX_N, BOX_D, BOX_D),
	[0x58] = BOX_LINES(BOX_L, BOX_D, BOX_N, BOX_N),
	[0x59] = BOX_LINES(BOX_D, BOX_L, BOX_N, BOX_N),
	[0x5a] = BOX_LINES(BOX_D, BOX_D, BOX_N, BOX_N),
	[0x5b] = BOX_LINES(BOX_L, BOX_N, BOX_N, BOX_D),
	[0x5c] = BOX_LINES(BOX_D, BOX_N, BOX_N, BOX_L),
	[0x5d] = BOX_LINES(BOX_D, BOX_N, BOX_N, BOX_D),
	[0x5e] = BOX_LINES(BOX_L, BOX_D, BOX_L, BOX_N),
	[0x5f] = BOX_LINES(BOX_D, BOX_L, BOX_D, BOX_N),
	[0x60] = BOX_LINES(BOX_D, BOX_D, BOX_D, BOX_N),
	[0x61] = BOX_LINES(BOX_L, BOX_N, BOX_L, BOX_D),
	[0x62] = BOX_LINES(BOX_D, BOX_N, BOX_D, BOX_L),
	[0x63] = BOX_LINES(BOX_D, BOX_N, BOX_D, BOX_D),
	[0x64] = BOX_LINES(BOX_N, BOX_D, BOX_L, BOX_D),
	[0x65] = BOX_LINES(BOX_N, BOX_L, BOX_D, BOX_L),
	[0x66] = BOX_LINES(BOX_N, BOX_D, BOX_D, BOX_D),
	[0x67] = BOX_LINES(BOX_L, BOX_D, BOX_N, BOX_D),
	[0x68] = BOX_LINES(BOX_D, BOX_L, BOX_N, BOX_L),
	[0x69] = BOX_LINES(BOX_D, BOX_D, BOX_N, BOX_D),
	[0x6a] = BOX_LINES(BOX_L, BOX_D, BOX_L, BOX_D),
	[0x6b] = BOX_LINES(BOX_D, BOX_L, BOX_D, BOX_L),
	[0x6c] = BOX_LINES(BOX_D, BOX_D, BOX_D, BOX_D),
	[0x74] = BOX_LINES(BOX_N, BOX_N, BOX_N, BOX_L),
	[0x75] = BOX_LINES(BOX_L, BOX_N, BOX_N, BOX_N),
	[0x76] = BOX_LINES(BOX_N, BOX_L, BOX_N, BOX_N),
	[0x77] = BOX_LINES(BOX_N, BOX_N, BOX_L, BOX_N),
	[0x78] = BOX_LINES(BOX_N, BOX_N, BOX_N, BOX_H),
	[0x79] = BOX_LINES(BOX_H, BOX_N, BOX_N, BOX_N),
	[0x7a] = BOX_LINES(BOX_N, BOX_H, BOX_N, BOX_N),
	[0x7b] = BOX_LINES(BOX_N, BOX_N, BOX_H, BOX_N),
	[0x7c] = BOX_LINES(BOX_N, BOX_H, BOX_N, BOX_L),
	[0x7d] = BOX_LINES(BOX_L, BOX_N, BOX_H, BOX_N),
	[0x7e] = BOX_LINES(BOX_N, BOX_L, BOX_N, BOX_H),
	[0x7f] = BOX_LINES(BOX_H, BOX_N, BOX_L, BOX_N),
};

static unsigned int box_sub(unsigned int a, unsigned int b)
{
	return a > b ? a - b : 0;
}

static unsigned int box_min(unsigned int a, unsigned int b)
{
	return a < b ? a : b;
}

/* fill [x0, x1) x [y0, y1) with @alpha, clipped to the buffer */
static void box_fill(struct video_buffer *buf, unsigned int x0, unsigned int y0, unsigned int x1,
		     unsigned int y1, uint8_t alpha)
{
	unsigned int y;

	x1 = box_min(x1, buf->width);
	y1 = box_min(y1, buf->height);
	if (x1 <= x0)
		return;
	for (y = y0; y < y1; y++)
		memset(&buf->data[y * buf->width + x0], alpha, x1 - x0);
}

/* @num/@den of @size, rounded, as the far edge of a block (Fraction.max) */
static unsigned int box_frac_max(unsigned int size, unsigned int num, unsigned int den)
{
	return (2 * num * size + den) / (2 * den);
}

/* the same as a near edge (Fraction.min): measured from the far side, so a
 * block from 0 to a fraction and one from it to the end together cover the
 * cell */
static unsigned int box_frac_min(unsigned int size, unsigned int num, unsigned int den)
{
	return size - box_frac_max(size, den - num, den);
}

/* linesChar() */
static void box_draw_lines(struct video_buffer *buf, uint8_t spec, unsigned int t)
{
	unsigned int w = buf->width, h = buf->height;
	unsigned int up = spec & 3, right = spec >> 2 & 3, down = spec >> 4 & 3, left = spec >> 6;
	unsigned int ht = 2 * t;

	unsigned int h_light_top = box_sub(h, t) / 2, h_light_bottom = h_light_top + t;
	unsigned int h_heavy_top = box_sub(h, ht) / 2, h_heavy_bottom = h_heavy_top + ht;
	unsigned int h_double_top = box_sub(h_light_top, t), h_double_bottom = h_light_bottom + t;
	unsigned int v_light_left = box_sub(w, t) / 2, v_light_right = v_light_left + t;
	unsigned int v_heavy_left = box_sub(w, ht) / 2, v_heavy_right = v_heavy_left + ht;
	unsigned int v_double_left = box_sub(v_light_left, t), v_double_right = v_light_right + t;

	/* how far each line reaches into the centre, so that it meets the
	 * crossing lines without a notch or a stub */
	unsigned int up_bottom, down_top, left_right, right_left;
	unsigned int a, b;

	if (left == BOX_H || right == BOX_H)
		up_bottom = h_heavy_bottom;
	else if (left != right || down == up)
		up_bottom = left == BOX_D || right == BOX_D ? h_double_bottom : h_light_bottom;
	else if (left == BOX_N && right == BOX_N)
		up_bottom = h_light_bottom;
	else
		up_bottom = h_light_top;

	if (left == BOX_H || right == BOX_H)
		down_top = h_heavy_top;
	else if (left != right || up == down)
		down_top = left == BOX_D || right == BOX_D ? h_double_top : h_light_top;
	else if (left == BOX_N && right == BOX_N)
		down_top = h_light_top;
	else
		down_top = h_light_bottom;

	if (up == BOX_H || down == BOX_H)
		left_right = v_heavy_right;
	else if (up != down || left == right)
		left_right = up == BOX_D || down == BOX_D ? v_double_right : v_light_right;
	else if (up == BOX_N && down == BOX_N)
		left_right = v_light_right;
	else
		left_right = v_light_left;

	if (up == BOX_H || down == BOX_H)
		right_left = v_heavy_left;
	else if (up != down || right == left)
		right_left = up == BOX_D || down == BOX_D ? v_double_left : v_light_left;
	else if (up == BOX_N && down == BOX_N)
		right_left = v_light_left;
	else
		right_left = v_light_right;

	switch (up) {
	case BOX_L:
		box_fill(buf, v_light_left, 0, v_light_right, up_bottom, 0xff);
		break;
	case BOX_H:
		box_fill(buf, v_heavy_left, 0, v_heavy_right, up_bottom, 0xff);
		break;
	case BOX_D:
		a = left == BOX_D ? h_light_top : up_bottom;
		b = right == BOX_D ? h_light_top : up_bottom;
		box_fill(buf, v_double_left, 0, v_light_left, a, 0xff);
		box_fill(buf, v_light_right, 0, v_double_right, b, 0xff);
		break;
	}

	switch (right) {
	case BOX_L:
		box_fill(buf, right_left, h_light_top, w, h_light_bottom, 0xff);
		break;
	case BOX_H:
		box_fill(buf, right_left, h_heavy_top, w, h_heavy_bottom, 0xff);
		break;
	case BOX_D:
		a = up == BOX_D ? v_light_right : right_left;
		b = down == BOX_D ? v_light_right : right_left;
		box_fill(buf, a, h_double_top, w, h_light_top, 0xff);
		box_fill(buf, b, h_light_bottom, w, h_double_bottom, 0xff);
		break;
	}

	switch (down) {
	case BOX_L:
		box_fill(buf, v_light_left, down_top, v_light_right, h, 0xff);
		break;
	case BOX_H:
		box_fill(buf, v_heavy_left, down_top, v_heavy_right, h, 0xff);
		break;
	case BOX_D:
		a = left == BOX_D ? h_light_bottom : down_top;
		b = right == BOX_D ? h_light_bottom : down_top;
		box_fill(buf, v_double_left, a, v_light_left, h, 0xff);
		box_fill(buf, v_light_right, b, v_double_right, h, 0xff);
		break;
	}

	switch (left) {
	case BOX_L:
		box_fill(buf, 0, h_light_top, left_right, h_light_bottom, 0xff);
		break;
	case BOX_H:
		box_fill(buf, 0, h_heavy_top, left_right, h_heavy_bottom, 0xff);
		break;
	case BOX_D:
		a = up == BOX_D ? v_light_left : left_right;
		b = down == BOX_D ? v_light_left : left_right;
		box_fill(buf, 0, h_double_top, a, h_light_top, 0xff);
		box_fill(buf, 0, h_light_bottom, b, h_double_bottom, 0xff);
		break;
	}
}

/*
 * dashHorizontal(): @count dashes @thick high with gaps of up to @gap, half a
 * gap at each end so the dashes tile; leftover pixels widen the dashes.
 */
static void box_dash_h(struct video_buffer *buf, unsigned int count, unsigned int thick,
		       unsigned int gap, unsigned int t)
{
	unsigned int w = buf->width, h = buf->height;
	unsigned int dash, extra, x, x1, y, i;

	if (w < 2 * count) {
		box_fill(buf, 0, box_sub(h, t) / 2, w, box_sub(h, t) / 2 + t, 0xff);
		return;
	}
	gap = box_min(gap, w / (2 * count));
	dash = (w - count * gap) / count;
	extra = (w - count * gap) % count;
	y = box_sub(h, thick) / 2;
	x = gap / 2;
	for (i = 0; i < count; i++) {
		x1 = x + dash + (extra ? 1 : 0);
		if (extra)
			extra--;
		box_fill(buf, x, y, x1, y + thick, 0xff);
		x = x1 + gap;
	}
}

/* dashVertical(): the same down the cell, with the whole gap at the bottom */
static void box_dash_v(struct video_buffer *buf, unsigned int count, unsigned int thick,
		       unsigned int gap, unsigned int t)
{
	unsigned int w = buf->width, h = buf->height;
	unsigned int dash, extra, x, y, y1, i;

	if (h < 2 * count) {
		box_fill(buf, box_sub(w, t) / 2, 0, box_sub(w, t) / 2 + t, h, 0xff);
		return;
	}
	gap = box_min(gap, h / (2 * count));
	dash = (h - count * gap) / count;
	extra = (h - count * gap) % count;
	x = box_sub(w, thick) / 2;
	y = 0;
	for (i = 0; i < count; i++) {
		y1 = y + dash + (extra ? 1 : 0);
		if (extra)
			extra--;
		box_fill(buf, x, y, x + thick, y1, 0xff);
		y = y1 + gap;
	}
}

struct box_pt {
	double x, y;
};

/* the squared distance from @p to the segment @a-@b */
static double box_dist2(struct box_pt p, struct box_pt a, struct box_pt b)
{
	double dx = b.x - a.x, dy = b.y - a.y;
	double len2 = dx * dx + dy * dy;
	double u = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;

	if (u < 0)
		u = 0;
	else if (u > 1)
		u = 1;
	dx = a.x + u * dx - p.x;
	dy = a.y + u * dy - p.y;
	return dx * dx + dy * dy;
}

/*
 * Stroke the polyline @p (@n points) @thick wide, adding to what the buffer
 * holds. The ends lie outside the cell, so their caps are cut off.
 */
static void box_stroke(struct video_buffer *buf, const struct box_pt *p, unsigned int n,
		       double thick)
{
	double r2 = thick * thick / 4;
	unsigned int x, y, sx, sy, i, hits, v;
	uint8_t *px;

	for (y = 0; y < buf->height; y++) {
		for (x = 0; x < buf->width; x++) {
			hits = 0;
			for (sy = 0; sy < BOX_SS; sy++) {
				for (sx = 0; sx < BOX_SS; sx++) {
					struct box_pt s = {x + (sx + 0.5) / BOX_SS,
							   y + (sy + 0.5) / BOX_SS};

					for (i = 0; i + 1 < n; i++) {
						if (box_dist2(s, p[i], p[i + 1]) <= r2) {
							hits++;
							break;
						}
					}
				}
			}
			px = &buf->data[y * buf->width + x];
			v = hits * 255 / (BOX_SS * BOX_SS);
			if (v > *px)
				*px = v;
		}
	}
}

/* the light lines' centre, where curves and diagonals meet them */
static double box_centre(unsigned int size, unsigned int t)
{
	return box_sub(size, t) / 2 + t / 2.0;
}

#define BOX_ARC_STEPS 16

/*
 * arc(): a light line from the vertical edge @dy (1 bottom, -1 top) up to
 * a radius from the centre, a cubic curve with its control points a quarter
 * radius from the centre, and on to the horizontal edge @dx (1 right, -1
 * left).
 */
static void box_draw_arc(struct video_buffer *buf, int dx, int dy, unsigned int t)
{
	unsigned int w = buf->width, h = buf->height;
	double cx = box_centre(w, t), cy = box_centre(h, t);
	double r = (w < h ? w : h) / 2.0, s = 0.25;
	struct box_pt c[4] = {
		{cx, cy + dy * r},
		{cx, cy + dy * s * r},
		{cx + dx * s * r, cy},
		{cx + dx * r, cy},
	};
	struct box_pt p[BOX_ARC_STEPS + 3];
	unsigned int i;

	p[0].x = cx;
	p[0].y = dy > 0 ? h + t : -(double)t;
	for (i = 0; i <= BOX_ARC_STEPS; i++) {
		double u = (double)i / BOX_ARC_STEPS, v = 1 - u;
		double k0 = v * v * v, k1 = 3 * v * v * u, k2 = 3 * v * u * u, k3 = u * u * u;

		p[i + 1].x = k0 * c[0].x + k1 * c[1].x + k2 * c[2].x + k3 * c[3].x;
		p[i + 1].y = k0 * c[0].y + k1 * c[1].y + k2 * c[2].y + k3 * c[3].y;
	}
	p[BOX_ARC_STEPS + 2].x = dx > 0 ? w + t : -(double)t;
	p[BOX_ARC_STEPS + 2].y = cy;
	box_stroke(buf, p, BOX_ARC_STEPS + 3, t);
}

/*
 * lightDiagonal*(): from the top corner @top_x (0 left, 1 right) to the
 * opposite bottom corner, overshooting each by half a pixel along the slope,
 * then on by a line width so the cut-off caps leave the corners as Ghostty's
 * butt caps would.
 */
static void box_draw_diagonal(struct video_buffer *buf, int top_x, unsigned int t)
{
	double w = buf->width, h = buf->height;
	double ox = 0.5 * (w < h ? w / h : 1), oy = 0.5 * (h < w ? h / w : 1);
	/* the segment is at least max(w, h) long, so this moves each end out
	 * by at least a line width */
	double k = t / (w > h ? w : h);
	struct box_pt p[2];
	double ux, uy;

	p[0].x = top_x ? w + ox : -ox;
	p[0].y = -oy;
	p[1].x = top_x ? -ox : w + ox;
	p[1].y = h + oy;

	ux = (p[1].x - p[0].x) * k;
	uy = (p[1].y - p[0].y) * k;
	p[0].x -= ux;
	p[0].y -= uy;
	p[1].x += ux;
	p[1].y += uy;
	box_stroke(buf, p, 2, t);
}

/* block.zig blockShade(): a block @wn/8 wide and @hn/8 high */
enum box_align { BOX_ALIGN_LEFT, BOX_ALIGN_RIGHT, BOX_ALIGN_TOP, BOX_ALIGN_BOTTOM };

static void box_draw_block(struct video_buffer *buf, enum box_align a, unsigned int wn,
			   unsigned int hn)
{
	unsigned int w = buf->width, h = buf->height;
	unsigned int bw = box_frac_max(w, wn, 8), bh = box_frac_max(h, hn, 8);
	unsigned int x = a == BOX_ALIGN_RIGHT ? w - bw : 0;
	unsigned int y = a == BOX_ALIGN_BOTTOM ? h - bh : 0;

	box_fill(buf, x, y, x + bw, y + bh, 0xff);
}

/* quadrant(): @q holds top left, top right, bottom left, bottom right as bits
 * 0 to 3 */
static void box_draw_quadrants(struct video_buffer *buf, unsigned int q)
{
	unsigned int w = buf->width, h = buf->height;
	unsigned int xl = box_frac_max(w, 1, 2), xr = box_frac_min(w, 1, 2);
	unsigned int yt = box_frac_max(h, 1, 2), yb = box_frac_min(h, 1, 2);

	if (q & 1)
		box_fill(buf, 0, 0, xl, yt, 0xff);
	if (q & 2)
		box_fill(buf, xr, 0, w, yt, 0xff);
	if (q & 4)
		box_fill(buf, 0, yb, xl, h, 0xff);
	if (q & 8)
		box_fill(buf, xr, yb, w, h, 0xff);
}

#define BOX_Q_TL 1
#define BOX_Q_TR 2
#define BOX_Q_BL 4
#define BOX_Q_BR 8

static void box_draw_block_element(struct video_buffer *buf, uint32_t cp)
{
	switch (cp) {
	case 0x2580: /* upper half */
		box_draw_block(buf, BOX_ALIGN_TOP, 8, 4);
		break;
	case 0x2581 ... 0x2587: /* lower one to seven eighths */
		box_draw_block(buf, BOX_ALIGN_BOTTOM, 8, cp - 0x2580);
		break;
	case 0x2588: /* full */
		box_fill(buf, 0, 0, buf->width, buf->height, 0xff);
		break;
	case 0x2589 ... 0x258f: /* left seven eighths to one eighth */
		box_draw_block(buf, BOX_ALIGN_LEFT, 0x2590 - cp, 8);
		break;
	case 0x2590: /* right half */
		box_draw_block(buf, BOX_ALIGN_RIGHT, 4, 8);
		break;
	case 0x2591 ... 0x2593: /* light, medium and dark shade */
		box_fill(buf, 0, 0, buf->width, buf->height, 0x40 * (cp - 0x2590));
		break;
	case 0x2594: /* upper one eighth */
		box_draw_block(buf, BOX_ALIGN_TOP, 8, 1);
		break;
	case 0x2595: /* right one eighth */
		box_draw_block(buf, BOX_ALIGN_RIGHT, 1, 8);
		break;
	case 0x2596:
		box_draw_quadrants(buf, BOX_Q_BL);
		break;
	case 0x2597:
		box_draw_quadrants(buf, BOX_Q_BR);
		break;
	case 0x2598:
		box_draw_quadrants(buf, BOX_Q_TL);
		break;
	case 0x2599:
		box_draw_quadrants(buf, BOX_Q_TL | BOX_Q_BL | BOX_Q_BR);
		break;
	case 0x259a:
		box_draw_quadrants(buf, BOX_Q_TL | BOX_Q_BR);
		break;
	case 0x259b:
		box_draw_quadrants(buf, BOX_Q_TL | BOX_Q_TR | BOX_Q_BL);
		break;
	case 0x259c:
		box_draw_quadrants(buf, BOX_Q_TL | BOX_Q_TR | BOX_Q_BR);
		break;
	case 0x259d:
		box_draw_quadrants(buf, BOX_Q_TR);
		break;
	case 0x259e:
		box_draw_quadrants(buf, BOX_Q_TR | BOX_Q_BL);
		break;
	case 0x259f:
		box_draw_quadrants(buf, BOX_Q_TR | BOX_Q_BL | BOX_Q_BR);
		break;
	}
}

static void box_draw_box_drawing(struct video_buffer *buf, uint32_t cp, unsigned int t)
{
	unsigned int gap = t > 4 ? t : 4;

	switch (cp) {
	case 0x2504: /* triple dash horizontal, light and heavy */
	case 0x2505:
		box_dash_h(buf, 3, cp == 0x2504 ? t : 2 * t, gap, t);
		break;
	case 0x2506: /* triple dash vertical */
	case 0x2507:
		box_dash_v(buf, 3, cp == 0x2506 ? t : 2 * t, gap, t);
		break;
	case 0x2508: /* quadruple dash horizontal */
	case 0x2509:
		box_dash_h(buf, 4, cp == 0x2508 ? t : 2 * t, gap, t);
		break;
	case 0x250a: /* quadruple dash vertical */
	case 0x250b:
		box_dash_v(buf, 4, cp == 0x250a ? t : 2 * t, gap, t);
		break;
	case 0x254c: /* double dash horizontal */
		box_dash_h(buf, 2, t, t, t);
		break;
	case 0x254d:
		box_dash_h(buf, 2, 2 * t, 2 * t, t);
		break;
	case 0x254e: /* double dash vertical */
		box_dash_v(buf, 2, t, 2 * t, t);
		break;
	case 0x254f:
		box_dash_v(buf, 2, 2 * t, 2 * t, t);
		break;
	case 0x256d: /* arc down and right */
		box_draw_arc(buf, 1, 1, t);
		break;
	case 0x256e: /* arc down and left */
		box_draw_arc(buf, -1, 1, t);
		break;
	case 0x256f: /* arc up and left */
		box_draw_arc(buf, -1, -1, t);
		break;
	case 0x2570: /* arc up and right */
		box_draw_arc(buf, 1, -1, t);
		break;
	case 0x2571: /* upper right to lower left */
		box_draw_diagonal(buf, 1, t);
		break;
	case 0x2572: /* upper left to lower right */
		box_draw_diagonal(buf, 0, t);
		break;
	case 0x2573: /* both */
		box_draw_diagonal(buf, 1, t);
		box_draw_diagonal(buf, 0, t);
		break;
	default:
		box_draw_lines(buf, box_lines[cp - 0x2500], t);
		break;
	}
}

bool kmscon_font_box_has(uint32_t cp)
{
	return cp >= 0x2500 && cp <= 0x259f;
}

struct kmscon_glyph *kmscon_font_box_render(uint32_t cp, unsigned int w, unsigned int h,
					    unsigned int thick)
{
	struct kmscon_glyph *g;

	if (!kmscon_font_box_has(cp) || !w || !h)
		return NULL;

	g = calloc(1, sizeof(*g) + (size_t)w * h);
	if (!g)
		return NULL;
	g->buf.width = w;
	g->buf.height = h;
	g->buf.format = VIDEO_FORMAT_A8;

	if (!thick)
		thick = 1;
	if (cp >= 0x2580)
		box_draw_block_element(&g->buf, cp);
	else
		box_draw_box_drawing(&g->buf, cp, thick);
	return g;
}
