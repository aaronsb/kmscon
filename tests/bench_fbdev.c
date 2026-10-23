/*
 * fbdev's blend, timed against the single loop it replaced (tests/fbdev_ref.h):
 * one frame of glyph-sized requests covering WIDTH x HEIGHT, per depth and
 * source format, with dithering on as fbdev sets it. Each run times the
 * reference and the current code once each, alternately, so a change in the
 * host's clock or load lands on both.
 *
 * Usage: bench_fbdev [WIDTH HEIGHT [RUNS [FRAMES]]]
 *
 * One line per depth and format: the median, minimum and maximum of RUNS
 * runs, in nanoseconds per pixel, for the reference (before) and the current
 * code (after). The framebuffer is ordinary memory, not a device mapping.
 */

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fbdev_ref.h"

#define CW 10
#define CH 20
#define NGLYPH 64
#define MAXRUNS 101

typedef int (*blend_fn)(struct display *, const struct video_blend_req *);

static struct video_buffer *glyphs[2][NGLYPH];

/* rings and bars: mostly 0, runs of 255, antialiased edges between */
static void make_glyphs(void)
{
	unsigned int g, x, y, a, argb;

	for (g = 0; g < NGLYPH; g++) {
		for (argb = 0; argb < 2; argb++) {
			glyphs[argb][g] =
				calloc(1, sizeof(struct video_buffer) + CW * CH * (argb ? 4 : 1));
			assert(glyphs[argb][g]);
			glyphs[argb][g]->width = CW;
			glyphs[argb][g]->height = CH;
			glyphs[argb][g]->format = argb ? VIDEO_FORMAT_ARGB32 : VIDEO_FORMAT_A8;
		}
		double cx = 4.5 + (g % 3) * 0.3, cy = 10 + (g % 5) * 0.4, r = 3 + (g % 4) * 0.5;
		for (y = 0; y < CH; y++)
			for (x = 0; x < CW; x++) {
				double dx = x + 0.5 - cx, dy = (y + 0.5 - cy) * 0.6;
				double cov =
					1.0 -
					__builtin_fabs(__builtin_sqrt(dx * dx + dy * dy) - r) / 1.2;

				if (g & 1 && x == 1 + g % 7 && y > 4 && y < 16)
					cov = 1.0;
				cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
				a = cov * 255 + 0.5;
				glyphs[0][g]->data[y * CW + x] = a;
				((uint32_t *)glyphs[1][g]->data)[y * CW + x] =
					a << 24 | (a * 200 / 255) << 16 | (a * 80 / 255) << 8 |
					(a * 30 / 255);
			}
	}
}

static double now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

struct layout {
	const char *name;
	unsigned int Bpp;
	bool xrgb32;
	unsigned int off[3], len[3];
};

static const struct layout layouts[] = {
	{"32 XRGB8888", 4, true, {16, 8, 0}, {8, 8, 8}},
	{"32 XBGR8888", 4, false, {0, 8, 16}, {8, 8, 8}},
	{"24 RGB888", 3, false, {16, 8, 0}, {8, 8, 8}},
	{"16 RGB565", 2, false, {11, 5, 0}, {5, 6, 5}},
};

/* one frame of cells through @fn, in ns per pixel */
static double frame(blend_fn fn, const struct layout *l, uint8_t *map, unsigned int w,
		    unsigned int h, unsigned int argb, unsigned int frames)
{
	struct fbdev_display fb = {0};
	struct display disp = {0};
	unsigned int f, x, y, g;
	double t0;

	disp.data = &fb;
	disp.flags = DISPLAY_DITHERING;
	fb.xres = w;
	fb.yres = h;
	fb.map = map;
	fb.Bpp = l->Bpp;
	fb.xrgb32 = l->xrgb32;
	fb.stride = w * l->Bpp;
	fb.off_r = l->off[0];
	fb.off_g = l->off[1];
	fb.off_b = l->off[2];
	fb.len_r = l->len[0];
	fb.len_g = l->len[1];
	fb.len_b = l->len[2];

	t0 = now_ns();
	for (f = 0; f < frames; f++) {
		g = f;
		for (y = 0; y + CH <= h; y += CH)
			for (x = 0; x + CW <= w; x += CW, g++) {
				struct video_blend_req req = {
					.buf = glyphs[argb][g % NGLYPH],
					.x = x,
					.y = y,
					.w = CW,
					.h = CH,
					.fr = 230,
					.fg = 230,
					.fb = 230,
					.br = 10 + (g & 3),
					.bg = 20,
					.bb = 30,
				};
				fn(&disp, &req);
			}
	}
	return (now_ns() - t0) / ((double)frames * (w / CW * CW) * (h / CH * CH));
}

int main(int argc, char **argv)
{
	unsigned int w = argc > 2 ? atoi(argv[1]) : 1920, h = argc > 2 ? atoi(argv[2]) : 1080;
	int runs = argc > 3 ? atoi(argv[3]) : 15, frames = argc > 4 ? atoi(argv[4]) : 10;
	double t[2][MAXRUNS];
	unsigned int li, argb;
	uint8_t *map;
	int r;

	assert(runs > 0 && runs <= MAXRUNS && frames > 0 && w >= CW && h >= CH);
	make_glyphs();
	map = calloc(1, (size_t)w * h * 4);
	assert(map);

	printf("# %ux%u, %d runs of %d frames, ns per pixel: median min max\n", w, h, runs, frames);
	for (li = 0; li < sizeof(layouts) / sizeof(layouts[0]); li++)
		for (argb = 0; argb < 2; argb++) {
			/* two untimed runs each to warm the caches */
			for (r = -2; r < runs; r++) {
				double a = frame(ref_fbdev_display_blend, &layouts[li], map, w, h,
						 argb, frames);
				double b = frame(fbdev_display_blend, &layouts[li], map, w, h, argb,
						 frames);

				if (r >= 0) {
					t[0][r] = a;
					t[1][r] = b;
				}
			}
			qsort(t[0], runs, sizeof(double), cmpd);
			qsort(t[1], runs, sizeof(double), cmpd);
			printf("%-12s %-5s before %.3f %.3f %.3f  after %.3f %.3f %.3f  %+.1f%%\n",
			       layouts[li].name, argb ? "ARGB" : "A8", t[0][runs / 2], t[0][0],
			       t[0][runs - 1], t[1][runs / 2], t[1][0], t[1][runs - 1],
			       100.0 * (t[1][runs / 2] - t[0][runs / 2]) / t[0][runs / 2]);
			fflush(stdout);
		}
	free(map);
	return 0;
}
