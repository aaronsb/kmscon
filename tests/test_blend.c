/*
 * The blend maths of drm2d and fbdev (ADR-102): a premultiplied ARGB source
 * over the request's background, out = src + bg * (255 - a) / 255 (drm2d,
 * rounded) or / 256 (fbdev, truncated), at alpha 0, 128 and 255; fbdev at 16,
 * 24 and 32 bpp; and the alpha-mask blend both had before, pinned.
 *
 * The expected values are worked out by hand in the comments, not computed
 * from the code under test.
 */

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/video/drm2d_render.c"
#undef LOG_SUBSYSTEM
/* both files name their static helpers alike */
#define blend_mask fb_blend_mask
#define blend_argb fb_blend_argb
#include "../src/video/fbdev_render.c"

/* 3 x 2 premultiplied ARGB source: alpha 0, 128 and 255 in each row */
static struct {
	struct video_buffer b;
	uint32_t px[6];
} argb = {
	.b = {.width = 3, .height = 2, .format = VIDEO_FORMAT_ARGB32},
	.px = {
		0x00000000, 0x80408000, 0xff0a141e, /* a=0; a=128 (64,128,0); a=255 (10,20,30) */
		0xff0a141e, 0x00000000, 0x80408000,
	},
};

/* 1 x 1 alpha mask at 128 */
static struct {
	struct video_buffer b;
	uint8_t px[1];
} mask = {
	.b = {.width = 1, .height = 1, .format = VIDEO_FORMAT_A8},
	.px = {128},
};

static struct video_blend_req argb_req(void)
{
	/* fg is ignored by the ARGB blend: make it loud */
	struct video_blend_req req = {
		.buf = &argb.b, .x = 1, .y = 1, .w = 3, .h = 2,
		.fr = 255, .fg = 0, .fb = 255,
		.br = 200, .bg = 100, .bb = 50,
	};
	return req;
}

static struct video_blend_req mask_req(void)
{
	struct video_blend_req req = {
		.buf = &mask.b, .x = 0, .y = 0, .w = 1, .h = 1,
		.fr = 255, .fg = 255, .fb = 255,
		.br = 0, .bg = 0, .bb = 0,
	};
	return req;
}

/* ---- drm2d: XRGB8888, exact /255 ---- */

static void test_drm2d(void)
{
	static uint32_t fb[3 * 5];
	struct drm2d_display d2d = {0};
	struct display disp = {0};
	struct video_blend_req req = argb_req();

	disp.width = 5;
	disp.height = 3;
	disp.data = &d2d;
	d2d.current_rb = 0;
	d2d.rb[1].map = fb;
	d2d.rb[1].stride = 5 * 4;

	memset(fb, 0xee, sizeof(fb));
	assert(drm2d_display_blend(&disp, &req) == 0);

	/*
	 * a=0: the background, (200,100,50).
	 * a=128, src (64,128,0), 255-a = 127:
	 *   r = 64 + round(200*127/255 = 99.61) = 164
	 *   g = 128 + round(100*127/255 = 49.80) = 178
	 *   b = 0 + round(50*127/255 = 24.90) = 25
	 * a=255: the source, (10,20,30).
	 */
	assert(fb[1 * 5 + 1] == 0xc86432);
	assert(fb[1 * 5 + 2] == 0xa4b219);
	assert(fb[1 * 5 + 3] == 0x0a141e);
	assert(fb[2 * 5 + 1] == 0x0a141e);
	assert(fb[2 * 5 + 2] == 0xc86432);
	assert(fb[2 * 5 + 3] == 0xa4b219);
	/* nothing outside the request */
	assert(fb[0] == 0xeeeeeeee && fb[1 * 5 + 4] == 0xeeeeeeee && fb[1 * 5] == 0xeeeeeeee);

	/* the mask blend: white at 128 over black, round(255*128/255) = 128 */
	req = mask_req();
	assert(drm2d_display_blend(&disp, &req) == 0);
	assert(fb[0] == 0x808080);
}

/* ---- fbdev: /256, then the device format ---- */

static void fb_init(struct display *disp, struct fbdev_display *fbdev, uint8_t *map,
		    unsigned int Bpp, bool xrgb32, unsigned int off_r, unsigned int off_g,
		    unsigned int off_b, unsigned int len_r, unsigned int len_g, unsigned int len_b)
{
	memset(disp, 0, sizeof(*disp));
	memset(fbdev, 0, sizeof(*fbdev));
	disp->data = fbdev;
	fbdev->xres = 5;
	fbdev->yres = 3;
	fbdev->stride = 5 * Bpp;
	fbdev->Bpp = Bpp;
	fbdev->xrgb32 = xrgb32;
	fbdev->map = map;
	fbdev->off_r = off_r;
	fbdev->off_g = off_g;
	fbdev->off_b = off_b;
	fbdev->len_r = len_r;
	fbdev->len_g = len_g;
	fbdev->len_b = len_b;
}

/*
 * The XRGB values every fbdev depth starts from:
 * a=0: (200,100,50). a=255: (10,20,30).
 * a=128, src (64,128,0), 255-a = 127:
 *   r = 64 + floor(200*127/256 = 99.22) = 163
 *   g = 128 + floor(100*127/256 = 49.61) = 177
 *   b = 0 + floor(50*127/256 = 24.80) = 24
 */

static void test_fbdev32(void)
{
	static uint32_t map[3 * 5];
	struct display disp;
	struct fbdev_display fbdev;
	struct video_blend_req req = argb_req();

	/* XRGB8888, written as is */
	fb_init(&disp, &fbdev, (uint8_t *)map, 4, true, 16, 8, 0, 8, 8, 8);
	assert(fbdev_display_blend(&disp, &req) == 0);
	assert(map[1 * 5 + 1] == 0xc86432);
	assert(map[1 * 5 + 2] == 0xa3b118);
	assert(map[1 * 5 + 3] == 0x0a141e);
	assert(map[2 * 5 + 1] == 0x0a141e);
	assert(map[2 * 5 + 2] == 0xc86432);
	assert(map[2 * 5 + 3] == 0xa3b118);

	/* the mask blend: white at 128 over black, floor(255*128/256) = 127 */
	req = mask_req();
	assert(fbdev_display_blend(&disp, &req) == 0);
	assert(map[0] == 0x7f7f7f);

	/* 32 bpp in another channel order (XBGR) goes through the device format */
	memset(map, 0, sizeof(map));
	req = argb_req();
	fb_init(&disp, &fbdev, (uint8_t *)map, 4, false, 0, 8, 16, 8, 8, 8);
	assert(fbdev_display_blend(&disp, &req) == 0);
	assert(map[1 * 5 + 1] == 0x3264c8);
	assert(map[1 * 5 + 2] == 0x18b1a3);
	assert(map[1 * 5 + 3] == 0x1e140a);
}

static void test_fbdev24(void)
{
	static uint8_t map[3 * 5 * 3];
	struct display disp;
	struct fbdev_display fbdev;
	struct video_blend_req req = argb_req();
	const uint8_t *p;

	/* RGB888, three bytes per pixel, the least significant first */
	fb_init(&disp, &fbdev, map, 3, false, 16, 8, 0, 8, 8, 8);
	assert(fbdev_display_blend(&disp, &req) == 0);
	p = &map[(1 * 5 + 1) * 3];
	assert(p[0] == 50 && p[1] == 100 && p[2] == 200);
	assert(p[3] == 24 && p[4] == 177 && p[5] == 163);
	assert(p[6] == 30 && p[7] == 20 && p[8] == 10);
	p = &map[(2 * 5 + 1) * 3];
	assert(p[0] == 30 && p[1] == 20 && p[2] == 10);
	assert(p[3] == 50 && p[4] == 100 && p[5] == 200);
	assert(p[6] == 24 && p[7] == 177 && p[8] == 163);
	/* nothing outside the request */
	assert(map[(1 * 5 + 4) * 3] == 0 && map[(1 * 5) * 3 + 2] == 0);
}

static void test_fbdev16(void)
{
	static uint16_t map[3 * 5];
	struct display disp;
	struct fbdev_display fbdev;
	struct video_blend_req req = argb_req();

	/*
	 * RGB565: r >> 3 at bit 11, g >> 2 at bit 5, b >> 3 at bit 0.
	 * (200,100,50): 25 << 11 | 25 << 5 | 6 = 0xcb26
	 * (163,177,24): 20 << 11 | 44 << 5 | 3 = 0xa583
	 * (10,20,30):    1 << 11 |  5 << 5 | 3 = 0x08a3
	 */
	fb_init(&disp, &fbdev, (uint8_t *)map, 2, false, 11, 5, 0, 5, 6, 5);
	assert(fbdev_display_blend(&disp, &req) == 0);
	assert(map[1 * 5 + 1] == 0xcb26);
	assert(map[1 * 5 + 2] == 0xa583);
	assert(map[1 * 5 + 3] == 0x08a3);
	assert(map[2 * 5 + 1] == 0x08a3);
	assert(map[2 * 5 + 2] == 0xcb26);
	assert(map[2 * 5 + 3] == 0xa583);
}

int main(void)
{
	test_drm2d();
	test_fbdev32();
	test_fbdev24();
	test_fbdev16();
	printf("test_blend: drm2d, fbdev 32/24/16 bpp: ok\n");
	return 0;
}
