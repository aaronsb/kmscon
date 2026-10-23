/*
 * kmscon - Braille, sextant and octant sprites
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
 * The Braille, sextant and octant sprites of font_box.c (#76), at every cell
 * size from 1x1 to 32x64 and in one large cell:
 * - each codepoint is narrow in libtsm's width tables, and font_box claims
 *   it and nothing either side of its range;
 * - each sprite is the cell's size and equals box_model.h's, pixel for pixel;
 * - a mosaic and its complement cover the cell exactly once over, so blocks
 *   of neighbouring patterns meet with no gap;
 * - Braille's eight dots, apart from any layout rule: equal squares inside
 *   the cell, the two columns and four rows aligned and in order, evenly
 *   spaced down the cell, not overlapping, at least a pixel wide once the cell
 *   is 4x8, and every pattern the union of its dots.
 * Built with -Db_sanitize=address,undefined it is also the sanitizer loop.
 */

#undef NDEBUG
#include <assert.h>
#include <libtsm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/font/font_box.c"
#include "box_model.h"

static const struct {
	uint32_t first, last;
} ranges[] = {
	{0x2800, 0x28ff},
	{0x1fb00, 0x1fb3b},
	{0x1cd00, 0x1cde5},
};

#define BIG_W 1000
#define BIG_H 2000

static unsigned int failures;

static void fail(uint32_t cp, unsigned int w, unsigned int h, const char *why)
{
	if (failures++ < 20)
		fprintf(stderr, "test_font_box: U+%04X at %ux%u: %s\n", cp, w, h, why);
}

static struct kmscon_glyph *render(uint32_t cp, unsigned int w, unsigned int h)
{
	struct kmscon_glyph *g = kmscon_font_box_render(cp, w, h, 1);

	assert(g);
	assert(g->buf.width == w && g->buf.height == h && g->buf.format == VIDEO_FORMAT_A8);
	return g;
}

/* each sprite against the model */
static void check_model(unsigned int w, unsigned int h, uint8_t *want)
{
	for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		for (uint32_t cp = ranges[i].first; cp <= ranges[i].last; cp++) {
			struct kmscon_glyph *g = render(cp, w, h);

			assert(model_render(cp, w, h, want));
			if (memcmp(g->buf.data, want, (size_t)w * h))
				fail(cp, w, h, "differs from the model");
			free(g);
		}
	}
}

/* @mask's sprite in @base, its pattern's index found by @digits */
static uint32_t mosaic_cp(const char *const *names, unsigned int n, uint32_t base,
			  unsigned int mask)
{
	char digits[9];
	unsigned int k = 0;

	for (unsigned int b = 0; b < 8; b++)
		if (mask >> b & 1)
			digits[k++] = '1' + b;
	digits[k] = 0;
	for (unsigned int i = 0; i < n; i++)
		if (!strcmp(names[i], digits))
			return base + i;
	return 0;
}

/* every mosaic whose complement is a mosaic too: together, each pixel once */
static void check_complements(unsigned int w, unsigned int h, const char *const *names,
			      unsigned int n, uint32_t base, unsigned int full)
{
	for (unsigned int mask = 1; mask < full; mask++) {
		uint32_t a = mosaic_cp(names, n, base, mask);
		uint32_t b = mosaic_cp(names, n, base, full & ~mask);
		struct kmscon_glyph *ga, *gb;

		if (!a || !b)
			continue;
		ga = render(a, w, h);
		gb = render(b, w, h);
		for (size_t i = 0; i < (size_t)w * h; i++) {
			if (!ga->buf.data[i] && !gb->buf.data[i]) {
				fail(a, w, h, "a pixel neither it nor its complement covers");
				break;
			}
		}
		free(ga);
		free(gb);
	}
}

struct dot {
	unsigned int x0, y0, x1, y1, n;
};

/* the lit pixels' bounding box and count */
static struct dot find_dot(const struct kmscon_glyph *g)
{
	struct dot d = {g->buf.width, g->buf.height, 0, 0, 0};

	for (unsigned int y = 0; y < g->buf.height; y++) {
		for (unsigned int x = 0; x < g->buf.width; x++) {
			uint8_t v = g->buf.data[y * g->buf.width + x];

			if (v != 0 && v != 0xff)
				fail(0x2800, g->buf.width, g->buf.height,
				     "a pixel neither on nor off");
			if (!v)
				continue;
			d.n++;
			d.x0 = x < d.x0 ? x : d.x0;
			d.y0 = y < d.y0 ? y : d.y0;
			d.x1 = x + 1 > d.x1 ? x + 1 : d.x1;
			d.y1 = y + 1 > d.y1 ? y + 1 : d.y1;
		}
	}
	return d;
}

static void check_braille(unsigned int w, unsigned int h)
{
	/* dot n's column and row */
	static const unsigned int col[8] = {0, 0, 0, 1, 1, 1, 0, 1};
	static const unsigned int row[8] = {0, 1, 2, 0, 1, 2, 3, 3};
	struct dot d[8];
	unsigned int side, x[2], y[4];

	for (unsigned int n = 0; n < 8; n++) {
		struct kmscon_glyph *g = render(0x2800 + (1u << n), w, h);

		d[n] = find_dot(g);
		free(g);
	}
	side = d[0].n ? d[0].x1 - d[0].x0 : 0;
	if (!side) {
		/* too small for a dot: every pattern blank */
		for (unsigned int n = 0; n < 8; n++)
			if (d[n].n)
				fail(0x2800 + (1u << n), w, h, "a dot where dot 1 has none");
		if (w >= 4 && h >= 8)
			fail(0x2801, w, h, "no dot in a cell of at least 4x8");
		return;
	}
	for (unsigned int n = 0; n < 8; n++) {
		if (d[n].x1 - d[n].x0 != side || d[n].y1 - d[n].y0 != side ||
		    d[n].n != side * side) {
			fail(0x2800 + (1u << n), w, h, "not a square the size of dot 1");
			return;
		}
	}
	x[0] = d[0].x0;
	x[1] = d[3].x0;
	for (unsigned int r = 0; r < 4; r++)
		y[r] = d[r == 3 ? 6 : r].y0;
	for (unsigned int n = 0; n < 8; n++)
		if (d[n].x0 != x[col[n]] || d[n].y0 != y[row[n]])
			fail(0x2800 + (1u << n), w, h, "off its column or row");
	if (x[1] < x[0] + side)
		fail(0x2808, w, h, "the right column overlaps the left");
	for (unsigned int r = 0; r < 3; r++)
		if (y[r + 1] < y[r] + side || y[r + 1] - y[r] != y[1] - y[0])
			fail(0x2800, w, h, "rows overlapping or unevenly spaced");
	if (side < (w / 4 < h / 8 ? w / 4 : h / 8))
		fail(0x2801, w, h, "a dot smaller than the cell allows");

	/* every pattern is the union of its dots */
	for (uint32_t cp = 0x2800; cp <= 0x28ff; cp++) {
		struct kmscon_glyph *g = render(cp, w, h);

		for (unsigned int py = 0; py < h; py++) {
			for (unsigned int px = 0; px < w; px++) {
				bool on = false;

				for (unsigned int n = 0; n < 8; n++)
					on |= (cp >> n & 1) && px >= d[n].x0 && px < d[n].x1 &&
					      py >= d[n].y0 && py < d[n].y1;
				if (on != !!g->buf.data[py * w + px]) {
					fail(cp, w, h, "not the union of its dots");
					py = h;
					break;
				}
			}
		}
		free(g);
	}
}

static void check_size(unsigned int w, unsigned int h, uint8_t *want)
{
	check_model(w, h, want);
	check_complements(w, h, model_sextants, 60, 0x1fb00, 0x3f);
	check_complements(w, h, model_octants, 230, 0x1cd00, 0xff);
	check_braille(w, h);
}

int main(void)
{
	uint8_t *want = malloc((size_t)BIG_W * BIG_H);

	assert(want);
	for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		assert(!kmscon_font_box_has(ranges[i].first - 1));
		assert(!kmscon_font_box_has(ranges[i].last + 1));
		for (uint32_t cp = ranges[i].first; cp <= ranges[i].last; cp++) {
			assert(kmscon_font_box_has(cp));
			if (tsm_ucs4_get_width(cp) != 1)
				fail(cp, 0, 0, "not narrow in libtsm");
		}
	}
	/* a mosaic's complement is found by name, so the pairs exist */
	assert(mosaic_cp(model_sextants, 60, 0x1fb00, 0x19) == 0x1fb17);
	assert(mosaic_cp(model_octants, 230, 0x1cd00, 0x99) == 0x1cd89);

	for (unsigned int h = 1; h <= 64; h++)
		for (unsigned int w = 1; w <= 32; w++)
			check_size(w, h, want);
	check_size(BIG_W, BIG_H, want);
	free(want);

	if (failures) {
		fprintf(stderr, "test_font_box: %u failures\n", failures);
		return 1;
	}
	return 0;
}
