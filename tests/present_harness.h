/*
 * The display and the program's output that test_present and test_coalesce
 * share: a double-buffered display in memory drawn by the real bbulk and
 * drm2d, frames shaped like the showcase's written into it, and the pinned
 * font. Included once by each test, which then calls harness_init.
 */

#ifndef KMSCON_TEST_PRESENT_HARNESS_H
#define KMSCON_TEST_PRESENT_HARNESS_H

#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <libtsm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/font/font.c"
#include "../src/font/font_box.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_freetype.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_colour.c"
#undef LOG_SUBSYSTEM
#include "../src/render/text.c"
#undef LOG_SUBSYSTEM
#include "../src/render/bbulk.c"
#include "../src/render/image_budget.c"
#include "../src/render/picture.c"
#undef LOG_SUBSYSTEM
#include "../src/render/deco.c"
#undef LOG_SUBSYSTEM
#include "../src/video/drm2d_render.c"
#include "config.h"

void shl_module_ref(struct shl_module *module) {}
void shl_module_unref(struct shl_module *module) {}

#define COLS 120
#define ROWS 30
#define FONT_PX 16
#define NREAD 16384 /* KMSCON_NREAD, src/misc/pty.c */
#define ESC "\x1b"

static unsigned int fb_w, fb_h;
static size_t fb_bytes;

/* ---- double-buffered in-memory display ---- */

unsigned int display_get_width(struct display *disp)
{
	return fb_w;
}
unsigned int display_get_height(struct display *disp)
{
	return fb_h;
}
void display_ref(struct display *disp) {}
void display_unref(struct display *disp) {}
bool display_need_redraw(struct display *disp)
{
	return false;
}
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y) {}
bool display_need_commit(struct display *disp)
{
	return false;
}
int display_show_cursor(struct display *disp, int32_t x, int32_t y)
{
	return 0;
}
int display_hide_cursor(struct display *disp)
{
	return 0;
}

int display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	return drm2d_display_clear(disp, r, g, b);
}

int display_blend(struct display *disp, const struct video_blend_req *req)
{
	return drm2d_display_blend(disp, req);
}

/*
 * One display: drm2d draws into rb[current_rb ^ 1], and a swap flips
 * current_rb, so the next frame lands in the buffer drawn two frames ago.
 */
struct output {
	struct display disp;
	struct drm2d_display d2d;
	struct kmscon_text *txt;
	bool damage;			      /* reports damage, as with FB_DAMAGE_CLIPS */
	struct video_rect rects[COLS * ROWS]; /* the damage of the last frame */
	size_t n_rects;
};

static struct output *output_of(struct display *disp)
{
	return (struct output *)((char *)disp - offsetof(struct output, disp));
}

bool display_supports_damage(struct display *disp)
{
	return output_of(disp)->damage;
}

/* what display_swap returns; a failed swap leaves the buffers as they were */
static int swap_ret;

int display_swap(struct display *disp)
{
	if (swap_ret)
		return swap_ret;
	output_of(disp)->d2d.current_rb ^= 1;
	return 0;
}

void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages)
{
	struct output *o = output_of(disp);

	assert(n_rect <= sizeof(o->rects) / sizeof(o->rects[0]));
	memcpy(o->rects, damages, n_rect * sizeof(*damages));
	o->n_rects = n_rect;
}

static struct kmscon_font *font;
static const char *font_dir;
static int failures;

static void output_init(struct output *o, bool damage)
{
	int ret;

	memset(o, 0, sizeof(*o));
	o->damage = damage;
	o->disp.width = fb_w;
	o->disp.height = fb_h;
	o->disp.data = &o->d2d;
	for (int i = 0; i < 2; i++) {
		o->d2d.rb[i].map = calloc(1, fb_bytes);
		o->d2d.rb[i].stride = fb_w * 4;
		assert(o->d2d.rb[i].map);
	}
	ret = kmscon_text_new(&o->txt, "bbulk", OR_NORMAL, &o->disp);
	assert(ret == 0);
	ret = kmscon_text_set(o->txt, font);
	assert(ret == 0);
	assert(o->txt->cols == COLS && o->txt->rows == ROWS);
}

static void output_destroy(struct output *o)
{
	kmscon_text_unref(o->txt);
	free(o->d2d.rb[0].map);
	free(o->d2d.rb[1].map);
}

/*
 * Draws @con on @o and presents it, as terminal.c does; returns whether it
 * swapped. A frame that changed nothing stays in the back buffer.
 */
static bool output_frame(struct output *o, struct tsm_screen *con, struct tsm_vte *vte)
{
	struct tsm_screen_attr attr;
	int ret;

	tsm_vte_get_def_attr(vte, &attr);
	ret = kmscon_text_prepare(o->txt, &attr, false);
	assert(ret == 0);
	ret = kmscon_text_draw(o->txt, con, false);
	assert(ret == 0);
	ret = kmscon_text_present(o->txt);
	assert(ret == 0);
	return o->txt->frame_queued;
}

static const uint32_t *on_screen(const struct output *o)
{
	return o->d2d.rb[o->d2d.current_rb].map;
}

/* ---- the program's output ---- */

static uint32_t rnd_state;

static uint32_t rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 8;
}

/* what the program last drew in each cell: a codepoint (0: a space) and its colours */
struct cell {
	uint32_t cp;
	uint32_t fg, bg; /* 0xRRGGBB, or ~0u: the default */
};

static struct cell grid[ROWS][COLS];

#define DEFAULT_COLOUR (~0u)

/* a cell the way the showcase's effects fill one: mostly blank, else a dot pattern or a block */
static struct cell new_cell(unsigned int frame, unsigned int row, unsigned int col)
{
	static const uint32_t blocks[] = {0x2588, 0x2580, 0x2584, 0x258c, 0x2590, 'o'};
	static const uint32_t palette[] = {0x1f6f3f, 0x448daa, 0x4ab99c,
					   0xff8020, 0xa02090, 0xffffff};
	struct cell c = {0, DEFAULT_COLOUR, DEFAULT_COLOUR};
	uint32_t r = rnd();

	if (r % 100 < 60)
		c.cp = 0;
	else if (r % 100 < 90)
		c.cp = r % 7 ? 0x2800 + (rnd() & 0xff) : 0x28ff;
	else
		c.cp = blocks[rnd() % (sizeof(blocks) / sizeof(blocks[0]))];
	/* runs of one colour, as a line drawn across cells has */
	c.fg = palette[(frame + col / 9 + row / 4) % (sizeof(palette) / sizeof(palette[0]))];
	if (rnd() % 8 == 0)
		c.bg = palette[rnd() % (sizeof(palette) / sizeof(palette[0]))];
	return c;
}

static size_t put_cp(char *out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = cp;
		return 1;
	}
	out[0] = 0xe0 | cp >> 12;
	out[1] = 0x80 | (cp >> 6 & 0x3f);
	out[2] = 0x80 | (cp & 0x3f);
	return 3;
}

/*
 * Frame @frame as one write into @out: a share of the cells change, and each
 * row with a change is drawn whole from its first column, with SGR only where
 * a colour changes; the rest are left alone. Returns its length.
 */
static size_t frame_bytes(char *out, size_t size, unsigned int frame)
{
	size_t n = 0;

	for (unsigned int row = 0; row < ROWS; row++) {
		bool changed = frame == 0;
		uint32_t fg = DEFAULT_COLOUR - 1, bg = DEFAULT_COLOUR - 1;

		for (unsigned int col = 0; col < COLS; col++) {
			if (frame == 0 || rnd() % 5 == 0) {
				grid[row][col] = new_cell(frame, row, col);
				changed = true;
			}
		}
		/* some rows stay as they are, as rows the picture does not reach */
		if (!changed || (frame && rnd() % 6 == 0))
			continue;
		n += snprintf(out + n, size - n, ESC "[%u;1H", row + 1);
		for (unsigned int col = 0; col < COLS; col++) {
			const struct cell *c = &grid[row][col];

			if (c->bg != bg) {
				bg = c->bg;
				n += bg == DEFAULT_COLOUR
					     ? snprintf(out + n, size - n, ESC "[49m")
					     : snprintf(out + n, size - n, ESC "[48;2;%u;%u;%um",
							bg >> 16, bg >> 8 & 0xff, bg & 0xff);
			}
			if (c->cp && c->fg != fg) {
				fg = c->fg;
				n += snprintf(out + n, size - n, ESC "[38;2;%u;%u;%um", fg >> 16,
					      fg >> 8 & 0xff, fg & 0xff);
			}
			n += put_cp(out + n, c->cp ? c->cp : ' ');
			assert(n + 64 < size);
		}
	}
	n += snprintf(out + n, size - n, ESC "[0m");
	return n;
}

static void vte_write(struct tsm_vte *vte, const char *u8, size_t len, void *data) {}

/* Replace the process's fontconfig with one that knows only the pinned files. */
static void pin_fonts(const char *name)
{
	static const char *const files[] = {"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf"};
	FcConfig *cfg = FcConfigCreate();
	char path[4096];

	assert(cfg);
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", font_dir, files[i]);
		if (!FcConfigAppFontAddFile(cfg, (const FcChar8 *)path)) {
			fprintf(stderr, "%s: cannot load font %s\n", name, path);
			exit(2);
		}
	}
	if (!FcConfigSetCurrent(cfg)) {
		fprintf(stderr, "%s: cannot install the font configuration\n", name);
		exit(2);
	}
}

/*
 * Registers the font and the renderer, pins the font in FONT_DIR, the only
 * argument, and sizes the display; returns 0, or the exit code on failure.
 */
static int harness_init(int argc, char **argv, const char *name)
{
	int ret;

	if (argc != 2) {
		fprintf(stderr, "usage: %s FONT_DIR\n", argv[0]);
		return 2;
	}
	font_dir = argv[1];

	ret = kmscon_font_register(&kmscon_font_freetype_ops);
	assert(ret == 0);
	ret = kmscon_text_register(&kmscon_text_bbulk_ops);
	assert(ret == 0);

	pin_fonts(name);
	ret = kmscon_font_find(&font, "DejaVu Sans Mono", FONT_PX, "freetype");
	if (ret || !font || font->ops != &kmscon_font_freetype_ops) {
		fprintf(stderr, "%s: pinned font not found\n", name);
		return 2;
	}
	fb_w = COLS * font->width;
	fb_h = ROWS * font->height;
	fb_bytes = (size_t)fb_w * fb_h * 4;
	return 0;
}

#endif /* KMSCON_TEST_PRESENT_HARNESS_H */
