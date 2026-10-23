/*
 * kmscon - What the Braille, sextant and octant sprites should be
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
 * The sprites src/font/font_box.c should draw for Braille (U+2800-U+28FF),
 * the sextants (U+1FB00-U+1FB3B) and the octants (U+1CD00-U+1CDE5), written
 * apart from it for test_font_box and test_pixels (#76). The mosaics come from
 * the Unicode 16.0.0 character names, BLOCK SEXTANT-n and BLOCK OCTANT-n,
 * each block's edges at its exact fraction of the cell, rounded as Ghostty's
 * common.zig Fraction.min() and max() round them. The Braille dots follow
 * Ghostty's braille.zig (MIT, src/font/sprite/draw/ at f987028f), whose
 * layout is a choice rather than a fraction of the cell, so this restates
 * its steps; test_font_box holds that restatement and font_box.c to dot
 * layouts worked out by hand from braille.zig for a few cell sizes, so one
 * transcription error made in both cannot pass.
 */

#ifndef KMSCON_TEST_BOX_MODEL_H
#define KMSCON_TEST_BOX_MODEL_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* the digits of BLOCK SEXTANT-n, U+1FB00 on: 1 2 / 3 4 / 5 6 from the top */
// clang-format off
static const char *const model_sextants[60] = {
	"1", "2", "12", "3", "13", "23", "123", "4",
	"14", "24", "124", "34", "134", "234", "1234", "5",
	"15", "25", "125", "35", "235", "1235", "45", "145",
	"245", "1245", "345", "1345", "2345", "12345", "6", "16",
	"26", "126", "36", "136", "236", "1236", "46", "146",
	"1246", "346", "1346", "2346", "12346", "56", "156", "256",
	"1256", "356", "1356", "2356", "12356", "456", "1456", "2456",
	"12456", "3456", "13456", "23456",
};

/* the digits of BLOCK OCTANT-n, U+1CD00 on: 1 2 / 3 4 / 5 6 / 7 8 */
static const char *const model_octants[230] = {
	"3", "23", "123", "4", "14", "124", "34", "134",
	"234", "5", "15", "25", "125", "135", "235", "1235",
	"45", "145", "245", "1245", "345", "1345", "2345", "12345",
	"6", "16", "26", "126", "36", "136", "236", "1236",
	"146", "246", "1246", "346", "1346", "2346", "12346", "56",
	"156", "256", "1256", "356", "1356", "2356", "12356", "456",
	"1456", "2456", "12456", "3456", "13456", "23456", "17", "27",
	"127", "37", "137", "237", "1237", "47", "147", "247",
	"1247", "347", "1347", "2347", "12347", "157", "257", "1257",
	"357", "2357", "12357", "457", "1457", "12457", "3457", "13457",
	"23457", "67", "167", "267", "1267", "367", "1367", "2367",
	"12367", "467", "1467", "2467", "12467", "3467", "13467", "23467",
	"123467", "567", "1567", "2567", "12567", "3567", "13567", "23567",
	"123567", "4567", "14567", "24567", "124567", "34567", "134567", "234567",
	"1234567", "18", "28", "128", "38", "138", "238", "1238",
	"48", "148", "248", "1248", "348", "1348", "2348", "12348",
	"58", "158", "258", "1258", "358", "1358", "2358", "12358",
	"458", "1458", "2458", "12458", "3458", "13458", "23458", "123458",
	"168", "268", "1268", "368", "2368", "12368", "468", "1468",
	"12468", "3468", "13468", "23468", "568", "1568", "2568", "12568",
	"3568", "13568", "23568", "123568", "4568", "14568", "24568", "124568",
	"34568", "134568", "234568", "1234568", "178", "278", "1278", "378",
	"1378", "2378", "12378", "478", "1478", "2478", "12478", "3478",
	"13478", "23478", "123478", "578", "1578", "2578", "12578", "3578",
	"13578", "23578", "123578", "4578", "14578", "24578", "124578", "34578",
	"134578", "234578", "1234578", "678", "1678", "2678", "12678", "3678",
	"13678", "23678", "123678", "4678", "14678", "24678", "124678", "34678",
	"134678", "234678", "1234678", "15678", "25678", "125678", "35678", "235678",
	"1235678", "45678", "145678", "1245678", "1345678", "2345678",
};
// clang-format on

/* a block's edges along @size, from @f0 to @f1 of it: the near edge
 * Fraction.min(), size - round((1 - f) * size), the far edge Fraction.max(),
 * round(f * size) */
static void model_edges(unsigned int size, double f0, double f1, unsigned int *a, unsigned int *b)
{
	*a = size - (unsigned int)lround((1 - f0) * size);
	*b = (unsigned int)lround(f1 * size);
}

/* the blocks named by @digits on a grid two wide and @rows high */
static void model_mosaic(const char *digits, unsigned int rows, unsigned int w, unsigned int h,
			 uint8_t *out)
{
	for (const char *d = digits; *d; d++) {
		unsigned int n = *d - '1', c = n % 2, r = n / 2;
		unsigned int x0, x1, y0, y1;

		model_edges(w, c / 2.0, (c + 1) / 2.0, &x0, &x1);
		model_edges(h, (double)r / rows, (double)(r + 1) / rows, &y0, &y1);
		for (unsigned int y = y0; y < y1; y++)
			memset(&out[y * w + x0], 0xff, x1 - x0);
	}
}

/*
 * braille.zig: a dot min(w/4, h/8) square, gaps of w/4 and h/8 between the
 * dots, margins of half a gap; then the pixels left over, as many as each
 * step takes and in this order, buy a one-pixel dot, a one-pixel margin, a
 * pixel more of gap (one across, three down), a pixel more of margin each
 * side, and a pixel more of dot.
 */
static void model_braille(uint32_t cp, unsigned int w, unsigned int h, uint8_t *out)
{
	int dot = (int)(w / 4 < h / 8 ? w / 4 : h / 8);
	int gx = w / 4, gy = h / 8, mx = gx / 2, my = gy / 2;
	int lx = (int)w - 2 * mx - gx - 2 * dot, ly = (int)h - 2 * my - 3 * gy - 4 * dot;

	if (!dot && lx >= 2 && ly >= 4)
		dot = 1, lx -= 2, ly -= 4;
	if (!mx && lx >= 2)
		mx = 1, lx -= 2;
	if (!my && ly >= 2)
		my = 1, ly -= 2;
	if (lx >= 1)
		gx += 1, lx -= 1;
	if (ly >= 3)
		gy += 1, ly -= 3;
	if (lx >= 2)
		mx += 1, lx -= 2;
	if (ly >= 2)
		my += 1, ly -= 2;
	if (lx >= 2 && ly >= 4)
		dot += 1;

	/* dots 1-3 and 7 down the left, 4-6 and 8 down the right */
	for (unsigned int n = 0; n < 8; n++) {
		int c = n == 3 || n == 4 || n == 5 || n == 7;
		int r = n < 6 ? (int)n % 3 : 3;
		int x0 = mx + c * (dot + gx), y0 = my + r * (dot + gy);

		if (!(cp >> n & 1))
			continue;
		for (int y = y0; y < y0 + dot; y++)
			memset(&out[y * w + x0], 0xff, dot);
	}
}

/* fill @out (@w x @h, cleared first) with @cp's coverage, 0 or 0xff per
 * pixel; false if @cp is not one the model knows */
static bool model_render(uint32_t cp, unsigned int w, unsigned int h, uint8_t *out)
{
	memset(out, 0, (size_t)w * h);
	if (cp >= 0x2800 && cp <= 0x28ff)
		model_braille(cp, w, h, out);
	else if (cp >= 0x1fb00 && cp <= 0x1fb3b)
		model_mosaic(model_sextants[cp - 0x1fb00], 3, w, h, out);
	else if (cp >= 0x1cd00 && cp <= 0x1cde5)
		model_mosaic(model_octants[cp - 0x1cd00], 4, w, h, out);
	else
		return false;
	return true;
}

#endif /* KMSCON_TEST_BOX_MODEL_H */
