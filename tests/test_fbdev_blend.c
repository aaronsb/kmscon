/*
 * fbdev's blend and clear, one loop per depth, against the single loop they
 * replaced (tests/fbdev_ref.h): the same framebuffer bytes, the same return
 * value and the same dither error after every request, for each depth and
 * channel layout, both source formats, dithering on and off, the front and
 * the back buffer. The requests are a sweep of every mask and alpha value
 * under fixed colours, then a seeded random run of glyphs, colours,
 * positions (clipped and rejected ones included) and clears.
 */

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fbdev_ref.h"

#define XRES 61
#define YRES 37
#define RUN 2000

struct layout {
	const char *name;
	unsigned int Bpp;
	bool xrgb32;
	unsigned int off[3], len[3];
};

/* each store, with and without a dithered conversion in front of it */
static const struct layout layouts[] = {
	{"32 XRGB8888", 4, true, {16, 8, 0}, {8, 8, 8}},
	{"32 XBGR8888", 4, false, {0, 8, 16}, {8, 8, 8}},
	{"32 RGB666", 4, false, {12, 6, 0}, {6, 6, 6}},
	{"24 RGB888", 3, false, {16, 8, 0}, {8, 8, 8}},
	{"24 BGR888", 3, false, {0, 8, 16}, {8, 8, 8}},
	{"24 RGB666", 3, false, {12, 6, 0}, {6, 6, 6}},
	{"16 RGB565", 2, false, {11, 5, 0}, {5, 6, 5}},
	{"16 RGB555", 2, false, {10, 5, 0}, {5, 5, 5}},
};

static uint32_t rng = 0x2545f491;

static uint32_t next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* mostly the 0 and 255 of a glyph's inside and outside, some edges between */
static uint8_t coverage(void)
{
	uint32_t n = next() % 8;

	return n < 3 ? 0 : n < 5 ? 255 : next() & 0xff;
}

static struct video_buffer *new_buf(unsigned int w, unsigned int h, bool argb)
{
	struct video_buffer *buf = calloc(1, sizeof(*buf) + w * h * (argb ? 4 : 1));

	assert(buf);
	buf->width = w;
	buf->height = h;
	buf->format = argb ? VIDEO_FORMAT_ARGB32 : VIDEO_FORMAT_A8;
	return buf;
}

#define NBUF 24
static struct video_buffer *bufs[NBUF];

static void make_bufs(void)
{
	unsigned int i, j, n;
	uint32_t a, *px;

	/* every mask value, and every alpha with premultiplied colours */
	bufs[0] = new_buf(16, 16, false);
	for (i = 0; i < 256; i++)
		bufs[0]->data[i] = i;
	bufs[1] = new_buf(16, 16, true);
	px = (uint32_t *)bufs[1]->data;
	for (i = 0; i < 256; i++)
		px[i] = i << 24 | (i * 200 / 255) << 16 | (i * 80 / 255) << 8 | (i * 30 / 255);

	/* glyph-sized and larger, odd widths included */
	for (n = 2; n < NBUF; n++) {
		unsigned int w = 1 + next() % 24, h = 1 + next() % 40;
		bool argb = n & 1;

		bufs[n] = new_buf(w, h, argb);
		px = (uint32_t *)bufs[n]->data;
		for (j = 0; j < w * h; j++) {
			if (!argb) {
				bufs[n]->data[j] = coverage();
				continue;
			}
			a = coverage();
			/* premultiplied, but now and then a channel above alpha */
			if (next() % 16)
				px[j] = a << 24 | (next() % (a + 1)) << 16 |
					(next() % (a + 1)) << 8 | (next() % (a + 1));
			else
				px[j] = a << 24 | (next() & 0xffffff);
		}
	}
}

static uint8_t channel(void)
{
	uint32_t n = next() % 4;

	return n == 0 ? 0 : n == 1 ? 255 : next() & 0xff;
}

static void random_colours(struct video_blend_req *req)
{
	req->fr = channel();
	req->fg = channel();
	req->fb = channel();
	req->br = channel();
	req->bg = channel();
	req->bb = channel();
}

struct side {
	struct display disp;
	struct fbdev_display fb;
	uint8_t *map;
};

static size_t map_len(const struct layout *l)
{
	return (size_t)XRES * l->Bpp * YRES * 2;
}

static void side_init(struct side *s, const struct layout *l, unsigned int flags,
		      unsigned int bufid)
{
	size_t i, len = map_len(l);

	memset(s, 0, sizeof(*s));
	s->map = malloc(len);
	assert(s->map);
	/* a pattern, so a byte written outside the request shows */
	for (i = 0; i < len; i++)
		s->map[i] = i * 7 + 0xa5;
	s->disp.data = &s->fb;
	s->disp.flags = flags;
	s->fb.xres = XRES;
	s->fb.yres = YRES;
	s->fb.stride = XRES * l->Bpp;
	s->fb.Bpp = l->Bpp;
	s->fb.xrgb32 = l->xrgb32;
	s->fb.map = s->map;
	s->fb.bufid = bufid;
	s->fb.off_r = l->off[0];
	s->fb.off_g = l->off[1];
	s->fb.off_b = l->off[2];
	s->fb.len_r = l->len[0];
	s->fb.len_g = l->len[1];
	s->fb.len_b = l->len[2];
}

static unsigned long ops, failures;

static void same(const struct side *now, const struct side *ref, const struct layout *l,
		 const char *what, unsigned long step, int ret_now, int ret_ref)
{
	ops++;
	if (ret_now == ret_ref && !memcmp(now->map, ref->map, map_len(l)) &&
	    now->fb.dither_r == ref->fb.dither_r && now->fb.dither_g == ref->fb.dither_g &&
	    now->fb.dither_b == ref->fb.dither_b)
		return;

	if (failures++ < 10)
		fprintf(stderr,
			"test_fbdev_blend: %s, flags %#x, bufid %u: %s %lu differs from the "
			"reference (ret %d/%d, dither %ld,%ld,%ld/%ld,%ld,%ld)\n",
			l->name, now->disp.flags, now->fb.bufid, what, step, ret_now, ret_ref,
			(long)now->fb.dither_r, (long)now->fb.dither_g, (long)now->fb.dither_b,
			(long)ref->fb.dither_r, (long)ref->fb.dither_g, (long)ref->fb.dither_b);
}

static void blend_both(struct side *now, struct side *ref, const struct layout *l,
		       const struct video_blend_req *req, const char *what, unsigned long step)
{
	int a = fbdev_display_blend(&now->disp, req);
	int b = ref_fbdev_display_blend(&ref->disp, req);

	same(now, ref, l, what, step, a, b);
}

static void clear_both(struct side *now, struct side *ref, const struct layout *l, uint8_t r,
		       uint8_t g, uint8_t b, unsigned long step)
{
	int x = fbdev_display_clear(&now->disp, r, g, b);
	int y = ref_fbdev_display_clear(&ref->disp, r, g, b);

	same(now, ref, l, "clear", step, x, y);
}

/* fg over bg pairs for the sweep: white on black, black on white, the
 * renderer's default, and colours with no channel at an end */
static const uint8_t sweep_colours[][6] = {
	{255, 255, 255, 0, 0, 0},
	{0, 0, 0, 255, 255, 255},
	{230, 230, 230, 10, 20, 30},
	{17, 200, 99, 250, 3, 128},
};

static void run(const struct layout *l, unsigned int flags, unsigned int bufid)
{
	struct side now, ref;
	struct video_blend_req req;
	unsigned long step;
	unsigned int i;

	side_init(&now, l, flags, bufid);
	side_init(&ref, l, flags, bufid);

	for (i = 0; i < sizeof(sweep_colours) / sizeof(sweep_colours[0]); i++) {
		const uint8_t *c = sweep_colours[i];

		memset(&req, 0, sizeof(req));
		req.fr = c[0];
		req.fg = c[1];
		req.fb = c[2];
		req.br = c[3];
		req.bg = c[4];
		req.bb = c[5];
		req.w = 16;
		req.h = 16;
		req.buf = bufs[0];
		req.x = 1 + i * 3;
		req.y = 2;
		blend_both(&now, &ref, l, &req, "mask sweep", i);
		req.buf = bufs[1];
		req.y = 19;
		blend_both(&now, &ref, l, &req, "alpha sweep", i);
	}

	for (step = 0; step < RUN; step++) {
		if (next() % 300 == 0) {
			clear_both(&now, &ref, l, channel(), channel(), channel(), step);
			continue;
		}
		memset(&req, 0, sizeof(req));
		req.buf = bufs[next() % NBUF];
		req.w = next() % (req.buf->width + 3);
		req.h = next() % (req.buf->height + 3);
		/* past the right and bottom edges too: clipped, or refused */
		req.x = next() % (XRES + 4);
		req.y = next() % (YRES + 4);
		random_colours(&req);
		blend_both(&now, &ref, l, &req, "blend", step);
	}

	free(now.map);
	free(ref.map);
}

int main(void)
{
	unsigned int i, flags, bufid;

	make_bufs();
	for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++)
		for (flags = 0; flags < 4; flags++)
			for (bufid = 0; bufid < 2; bufid++)
				run(&layouts[i],
				    (flags & 1 ? DISPLAY_DITHERING : 0) |
					    (flags & 2 ? DISPLAY_DBUF : 0),
				    bufid);

	if (failures) {
		fprintf(stderr, "test_fbdev_blend: %lu of %lu requests differ from the reference\n",
			failures, ops);
		return 1;
	}
	printf("test_fbdev_blend: %lu blends and clears over %zu layouts byte-identical to the "
	       "reference\n",
	       ops, sizeof(layouts) / sizeof(layouts[0]));
	return 0;
}
