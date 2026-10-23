/*
 * Lightweight test for repeated bbulk_set calls (no leaks, all cells re-damaged),
 * and for the two-frame damage rule: a cell the frame marks changed on frame N
 * is drawn on N and again on N+1, because the back buffer is two frames old.
 * We include the implementation to access static helpers.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../src/render/text.h"

/* ---- Stubs for external dependencies used by text_bbulk.c ---- */
#include "../src/font/font.h"	/* for kmscon_font_* */
#include "../src/render/deco.h"
#include "../src/video/video.h" /* for display_* prototypes */
/* the fake display's size; test_budget_8k changes it */
static unsigned int disp_w = 640, disp_h = 480;

unsigned int display_get_width(struct display *disp)
{
	(void)disp;
	return disp_w;
}
unsigned int display_get_height(struct display *disp)
{
	(void)disp;
	return disp_h;
}

/* Keep geometry tiny to minimize allocations */
#define FAKE_CELL_W 8
#define FAKE_CELL_H 16

/* Stub font metrics APIs used indirectly by text.c/text_bbulk.c */
unsigned int kmscon_text_get_cols(struct kmscon_text *txt, unsigned int font_width)
{
	(void)txt;
	(void)font_width;
	return disp_w / FAKE_CELL_W;
}

unsigned int kmscon_text_get_rows(struct kmscon_text *txt, unsigned int font_height)
{
	(void)txt;
	(void)font_height;
	return disp_h / FAKE_CELL_H;
}

/* Stub font rendering APIs used by text_bbulk.c; rendering fails while set */
static bool fail_render;
static unsigned int n_rendered; /* glyphs rendered */
static uint32_t last_rendered;	/* the first codepoint of the last one */

struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, struct kmscon_font_attr *attr,
					const uint32_t *cps, size_t len, unsigned int cwidth)
{
	struct kmscon_glyph *g;
	(void)font;
	(void)attr;
	(void)cps;
	(void)len;
	if (fail_render)
		return NULL;
	n_rendered++;
	last_rendered = cps[0];
	g = calloc(1, sizeof(*g) + cwidth * FAKE_CELL_W * FAKE_CELL_H);
	g->buf.width = cwidth * FAKE_CELL_W;
	g->buf.height = FAKE_CELL_H;
	g->double_width = cwidth == 2;
	return g;
}

void kmscon_font_get_deco(struct kmscon_font *font, struct kmscon_font_deco *out)
{
	(void)font;
	out->ul_pos = FAKE_CELL_H - 2;
	out->ul_thick = 1;
	out->strike_pos = FAKE_CELL_H / 2;
	out->strike_thick = 1;
}

struct kmscon_glyph *kmscon_deco_render(enum kmscon_deco kind, unsigned int w, unsigned int h,
					const struct kmscon_font_deco *m)
{
	struct kmscon_glyph *g = calloc(1, sizeof(*g) + w * h);
	(void)kind;
	(void)m;
	/* solid, so a composed cell's first pixel is the decoration's colour */
	memset(g->buf.data, 0xff, w * h);
	g->buf.width = w;
	g->buf.height = h;
	return g;
}

/* the images placements show, by key; test placements name them */
static struct tsm_frame_image test_images[16];

int kmscon_text_get_image(struct kmscon_text *txt, bool overlay, uint64_t key,
			  struct tsm_frame_image *out)
{
	(void)txt;
	if (overlay || key >= 16 || !test_images[key].pixels)
		return -ENOENT;
	*out = test_images[key];
	return 0;
}

/* Stub text helpers used by text_bbulk.c */
const uint32_t *kmscon_text_get_cluster(struct kmscon_text *txt, uint64_t key, uint32_t *one,
					size_t *len)
{
	(void)txt;
	*one = key;
	*len = 1;
	return one;
}

/* the configured underline colour, as underline-color= would set it */
static const struct tsm_screen_color ul_conf = {.r = 1, .g = 2, .b = 3};

struct tsm_screen_color kmscon_text_ul_color(struct kmscon_text *txt,
					     const struct tsm_frame_cell *cell)
{
	(void)txt;
	(void)cell;
	return ul_conf;
}

struct tsm_screen_color kmscon_text_strike_color(struct kmscon_text *txt,
						 const struct tsm_frame_cell *cell)
{
	(void)txt;
	return cell->fg;
}

bool kmscon_font_has_glyph(struct kmscon_font *font, struct kmscon_font_attr *attr,
			   const uint32_t ch)
{
	(void)font;
	(void)attr;
	(void)ch;
	return true;
}

int kmscon_rotate_glyph(struct kmscon_glyph *vb, const struct kmscon_glyph *glyph,
			enum Orientation o, uint8_t align)
{
	(void)o;
	(void)align;
	if (!vb || !glyph)
		return -EINVAL;
	*vb = *glyph;
	if (glyph->buf.width == 0 || glyph->buf.height == 0)
		return 0;
	size_t buf_size = glyph->buf.width * glyph->buf.height;
	memset(vb->buf.data, 0x11, buf_size);
	return 0;
}

/* Stub uterm display APIs used by text_bbulk.c */
bool display_need_redraw(struct display *disp)
{
	(void)disp;
	return false;
}
bool display_supports_damage(struct display *disp)
{
	(void)disp;
	return true;
}
int display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	(void)disp;
	(void)r;
	(void)g;
	(void)b;
	return 0;
}
/* the requests blended since the last reset */
static struct {
	unsigned int x, y;
	const struct video_buffer *buf;
	uint32_t first; /* the buffer's first four bytes */
} blended[4096];
static unsigned int n_blended;

int display_blend(struct display *disp, const struct video_blend_req *req)
{
	(void)disp;
	if (n_blended < 4096) {
		blended[n_blended].x = req->x;
		blended[n_blended].y = req->y;
		blended[n_blended].buf = req->buf;
		memcpy(&blended[n_blended].first, req->buf->data, 4);
		n_blended++;
	}
	return 0;
}

/* the request blended with its top-left corner at @x, @y, or NULL */
static typeof(&blended[0]) blended_at(unsigned int x, unsigned int y)
{
	for (unsigned int i = 0; i < n_blended; i++)
		if (blended[i].x == x && blended[i].y == y)
			return &blended[i];
	return NULL;
}

static bool was_blended(unsigned int x, unsigned int y)
{
	return blended_at(x, y);
}
void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages)
{
	(void)disp;
	(void)n_rect;
	(void)damages;
}
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y)
{
	(void)disp;
	(void)x;
	(void)y;
}
#include "shl/log.h"
#undef log_warning
#define log_warning(f, ...)
#undef log_debug
#define log_debug(f, ...)
/* Pull in the implementation so we can call bbulk_set directly */
#include "../src/render/picture.c"
#include "../src/render/bbulk.c"

/* Fake font objects with valid width/height for FONT_WIDTH/FONT_HEIGHT macros */
static struct kmscon_font fake_font = {.width = FAKE_CELL_W, .height = FAKE_CELL_H};

static void init_fake_txt(struct kmscon_text *txt)
{
	memset(txt, 0, sizeof(*txt));
	txt->ops = &kmscon_text_bbulk_ops;
	txt->disp = (struct display *)0x1; /* non-null stub */
	txt->font = &fake_font;
	txt->orientation = OR_NORMAL;
	txt->ref = 1;
}

static struct tsm_frame_cell cells[(7680 / FAKE_CELL_W) * (2160 / FAKE_CELL_H)];
static struct tsm_frame frame;

/* a frame of blank, unchanged cells, with no cursor */
static void blank_frame(struct kmscon_text *txt)
{
	unsigned int i;

	memset(&frame, 0, sizeof(frame));
	frame.cols = txt->cols;
	frame.rows = txt->rows;
	frame.cell_size = sizeof(cells[0]);
	frame.cells = cells;
	for (i = 0; i < frame.cols * frame.rows; i++) {
		memset(&cells[i], 0, sizeof(cells[i]));
		cells[i].width = 1;
	}
}

static struct tsm_frame_cell *cell_at(unsigned int x, unsigned int y)
{
	return &cells[y * frame.cols + x];
}

/* returns bbulk_render's result: 0, or KMSCON_TEXT_UNCHANGED */
static int run_frame(struct kmscon_text *txt, bool cursor_on)
{
	struct tsm_screen_attr attr = {0};
	int ret;

	n_blended = 0;
	assert(bbulk_prepare(txt, &attr) == 0);
	assert(bbulk_draw(txt, &frame, cursor_on) == 0);
	ret = bbulk_render(txt);
	assert(ret == 0 || ret == KMSCON_TEXT_UNCHANGED);
	return ret;
}

/* blank cells, but @x, @y holds @key with @flags, if @x is set */
static int draw_frame_cell(struct kmscon_text *txt, int x, int y, uint64_t key, uint16_t flags)
{
	blank_frame(txt);
	if (x >= 0) {
		cell_at(x, y)->key = key;
		cell_at(x, y)->flags = flags;
	}
	return run_frame(txt, false);
}

/*
 * One frame of blank cells; @changed_x, @changed_y is marked changed and holds
 * @key, if set.
 */
static int draw_frame_key(struct kmscon_text *txt, int changed_x, int changed_y, uint64_t key)
{
	return draw_frame_cell(txt, changed_x, changed_y, key, TSM_FRAME_CHANGED);
}

static int draw_frame(struct kmscon_text *txt, int changed_x, int changed_y)
{
	return draw_frame_key(txt, changed_x, changed_y, 'x');
}

static void test_two_frame_rule(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	unsigned int x, y;

	/* a fresh renderer redraws everything for two frames */
	draw_frame(txt, -1, -1);
	assert(n_blended == bb->cell_count);
	draw_frame(txt, -1, -1);
	assert(n_blended == bb->cell_count);
	draw_frame(txt, -1, -1);
	assert(n_blended == 0);

	/* frame N: the changed cell is drawn and reported as damage */
	set_coordinate(txt, &x, &y, 5, 3);
	assert(draw_frame(txt, 5, 3) == 0);
	assert(n_blended == 1 && was_blended(x, y));
	assert(bb->damage_rect_len == 1);
	assert(bb->damage_rects[0].x1 == x && bb->damage_rects[0].y1 == y);

	/* frame N+1: unchanged, but drawn again into the other buffer, and
	 * that buffer's damage */
	assert(draw_frame(txt, -1, -1) == 0);
	assert(n_blended == 1 && was_blended(x, y));
	assert(bb->damage_rect_len == 1);
	assert(bb->damage_rects[0].x1 == x && bb->damage_rects[0].y1 == y);

	/* frame N+2: both buffers hold it; nothing drawn, nothing to present */
	assert(draw_frame(txt, -1, -1) == KMSCON_TEXT_UNCHANGED);
	assert(n_blended == 0);
	assert(bb->damage_rect_len == 0);
}

/*
 * The render timer's damage area: the pixels in the frame's rectangles while
 * the text measures, and no work, the field untouched, while it does not.
 */
static void test_damage_measured(struct kmscon_text *txt)
{
	/* settle: both buffers hold the blank screen */
	while (draw_frame(txt, -1, -1) != KMSCON_TEXT_UNCHANGED)
		;

	txt->measure = false;
	txt->frame_damage_px = 12345;
	assert(draw_frame(txt, 2, 1) == 0);
	assert(txt->frame_damage_px == 12345);
	assert(draw_frame(txt, -1, -1) == 0);
	assert(draw_frame(txt, -1, -1) == KMSCON_TEXT_UNCHANGED);

	txt->measure = true;
	assert(draw_frame(txt, 2, 1) == 0);
	assert(txt->frame_damage_px == FAKE_CELL_W * FAKE_CELL_H);
	/* two cells three apart merge into one rectangle spanning the gap */
	blank_frame(txt);
	cell_at(2, 1)->key = 'x';
	cell_at(2, 1)->flags = TSM_FRAME_CHANGED;
	cell_at(5, 1)->key = 'x';
	cell_at(5, 1)->flags = TSM_FRAME_CHANGED;
	assert(run_frame(txt, false) == 0);
	assert(txt->frame_damage_px == 4 * FAKE_CELL_W * FAKE_CELL_H);
	assert(draw_frame(txt, -1, -1) == 0);
	assert(draw_frame(txt, -1, -1) == KMSCON_TEXT_UNCHANGED);
	assert(txt->frame_damage_px == 0);
	txt->measure = false;
}

/*
 * A cell whose glyph failed to draw gets its background at once, and its glyph
 * on a later frame.
 */
static void test_draw_failure_retried(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	unsigned int x, y, rendered;

	draw_frame(txt, -1, -1);
	draw_frame(txt, -1, -1);
	set_coordinate(txt, &x, &y, 7, 2);

	/* the frame marks the cell changed, but its glyph cannot be had */
	fail_render = true;
	draw_frame_key(txt, 7, 2, 'q');
	assert(was_blended(x, y) && blended_at(x, y)->buf == &bb->blank->buf);
	fail_render = false;

	/* the frame's flag is spent, the cell still holds 'q': its glyph is
	 * rendered and drawn into both buffers */
	rendered = n_rendered;
	draw_frame_cell(txt, 7, 2, 'q', 0);
	assert(n_blended == 1 && was_blended(x, y));
	assert(n_rendered == rendered + 1 && last_rendered == 'q');
	assert(blended_at(x, y)->buf != &bb->blank->buf);
	draw_frame_cell(txt, 7, 2, 'q', 0);
	assert(n_blended == 1 && was_blended(x, y));
	assert(blended_at(x, y)->buf != &bb->blank->buf);
	draw_frame_cell(txt, 7, 2, 'q', 0);
	assert(n_blended == 0);
}

/*
 * An underline cursor draws in the cursor's foreground, whatever colour the
 * cell's underline would take; an underlined cell still takes its own.
 */
static void test_underline_cursor_colour(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	struct tsm_frame_cell *c;
	unsigned int x, y;

	draw_frame(txt, -1, -1);
	draw_frame(txt, -1, -1);

	blank_frame(txt);
	/* an underlined cell, drawn in the configured underline colour */
	c = cell_at(5, 1);
	c->underline = TSM_UNDERLINE_SINGLE;
	c->flags = TSM_FRAME_CHANGED;
	/* the cursor on a cell whose SGR 58 colour is the background */
	c = &frame.cursor.cell;
	*c = *cell_at(3, 1);
	c->fg = (struct tsm_screen_color){.r = 200, .g = 100, .b = 50};
	c->underline = TSM_UNDERLINE_SINGLE;
	c->flags |= TSM_FRAME_UL_SET;
	frame.cursor.x = 3;
	frame.cursor.y = 1;
	frame.cursor.style = TSM_SCREEN_CURSOR_UNDERLINE_STEADY;
	frame.cursor.visible = true;
	run_frame(txt, true);

	set_coordinate(txt, &x, &y, 3, 1);
	assert(was_blended(x, y) && blended_at(x, y)->buf == bb->scratch);
	assert(blended_at(x, y)->first == 0xffc86432);
	set_coordinate(txt, &x, &y, 5, 1);
	assert(was_blended(x, y) && blended_at(x, y)->buf == bb->scratch);
	assert(blended_at(x, y)->first == 0xff010203);
}

/* ---- the image layer (ADR-103) ---- */

static uint32_t picture_px[128 * 32];
static struct tsm_frame_placement placements[16];

/* placement @i: image 1 (opaque, 128 x 32) over @cols x 2 cells at @col, @row */
static void set_placement(unsigned int i, uint64_t key, int row, unsigned int col,
			  unsigned int cols, uint8_t band)
{
	struct tsm_frame_placement *p = &placements[i];

	memset(p, 0, sizeof(*p));
	p->key = key;
	p->image_key = 1;
	p->row = row;
	p->col = col;
	p->cols = cols;
	p->rows = 2;
	p->width = cols * FAKE_CELL_W;
	p->height = 2 * FAKE_CELL_H;
	p->src_width = p->width;
	p->src_height = p->height;
	p->cell_width = FAKE_CELL_W;
	p->cell_height = FAKE_CELL_H;
	p->band = band;
	p->flags = TSM_FRAME_OPAQUE | TSM_FRAME_CHANGED;
}

/* blank cells under the first @n placements; returns bbulk_render's result */
static int draw_placements(struct kmscon_text *txt, unsigned int n)
{
	blank_frame(txt);
	frame.placement_size = sizeof(placements[0]);
	frame.placement_count = n;
	frame.placements = n ? placements : NULL;
	return run_frame(txt, false);
}

/* every cell of [@col, @col + @cols) x [@row, @row + 2) was blended */
static bool box_blended(struct kmscon_text *txt, unsigned int col, unsigned int row,
			unsigned int cols)
{
	unsigned int x, y, px, py, k;

	for (y = row; y < row + 2; y++) {
		for (x = col; x < col + cols; x++) {
			set_coordinate(txt, &px, &py, x, y);
			for (k = 0; k < n_blended; k++)
				if (blended[k].y == py && blended[k].x <= px &&
				    px < blended[k].x + blended[k].buf->width)
					break;
			if (k == n_blended)
				return false;
		}
	}
	return true;
}

static void settle(struct kmscon_text *txt, unsigned int n)
{
	while (draw_placements(txt, n) != KMSCON_TEXT_UNCHANGED)
		;
}

/*
 * A placement added, moved or removed draws its old and new boxes on that
 * frame and the next, and then nothing; one libtsm flags changed that draws
 * the same, as every placement is flagged when anything scrolls, is not drawn
 * again.
 */
static void test_placement_damage(struct kmscon_text *txt)
{
	test_images[1] = (struct tsm_frame_image){
		.width = 128, .height = 32, .stride = 128 * 4, .pixels = picture_px, .opaque = true};

	settle(txt, 0);

	/* added: drawn on two frames, as row strips */
	set_placement(0, 7, 3, 4, 6, TSM_FRAME_BAND_ABOVE_TEXT);
	assert(draw_placements(txt, 1) == 0);
	assert(n_blended == 2 && box_blended(txt, 4, 3, 6));
	placements[0].flags &= ~TSM_FRAME_CHANGED;
	assert(draw_placements(txt, 1) == 0);
	assert(n_blended == 2 && box_blended(txt, 4, 3, 6));
	assert(draw_placements(txt, 1) == KMSCON_TEXT_UNCHANGED);

	/* flagged changed by libtsm, drawing the same: nothing to draw */
	placements[0].flags |= TSM_FRAME_CHANGED;
	assert(draw_placements(txt, 1) == KMSCON_TEXT_UNCHANGED);

	/* moved: the old box and the new, on two frames */
	set_placement(0, 7, 6, 20, 6, TSM_FRAME_BAND_ABOVE_TEXT);
	assert(draw_placements(txt, 1) == 0);
	assert(box_blended(txt, 4, 3, 6) && box_blended(txt, 20, 6, 6));
	assert(draw_placements(txt, 1) == 0);
	assert(box_blended(txt, 4, 3, 6) && box_blended(txt, 20, 6, 6));
	assert(draw_placements(txt, 1) == KMSCON_TEXT_UNCHANGED);

	/* its image replaced: the same box again */
	placements[0].image_key = 2;
	test_images[2] = test_images[1];
	assert(draw_placements(txt, 1) == 0);
	assert(n_blended == 2 && box_blended(txt, 20, 6, 6));
	assert(draw_placements(txt, 1) == 0);
	assert(draw_placements(txt, 1) == KMSCON_TEXT_UNCHANGED);

	/* removed: the old box, on two frames */
	assert(draw_placements(txt, 0) == 0);
	assert(n_blended == 12 && box_blended(txt, 20, 6, 6));
	assert(blended_at(0, 0) == NULL);
	assert(draw_placements(txt, 0) == 0);
	assert(n_blended == 12 && box_blended(txt, 20, 6, 6));
	assert(draw_placements(txt, 0) == KMSCON_TEXT_UNCHANGED);
	test_images[2].pixels = NULL;
}

/*
 * Cells wholly under an opaque picture with nothing over them go as one
 * request per row; a glyph under a picture below the text splits the run, and
 * its cell is composed, while one under a top-band picture is hidden by it.
 */
static void test_opaque_runs(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	unsigned int x, y;

	settle(txt, 0);
	set_placement(0, 9, 1, 2, 10, TSM_FRAME_BAND_BELOW_TEXT);
	draw_placements(txt, 1);
	assert(n_blended == 2 && blended_at(16, 16)->buf == bb->strip);

	/* a glyph in the middle of the first row */
	blank_frame(txt);
	frame.placement_size = sizeof(placements[0]);
	frame.placement_count = 1;
	frame.placements = placements;
	cell_at(6, 1)->key = 'x';
	cell_at(6, 1)->flags = TSM_FRAME_CHANGED;
	run_frame(txt, false);
	set_coordinate(txt, &x, &y, 6, 1);
	assert(blended_at(x, y) && blended_at(x, y)->buf == bb->scratch);
	set_coordinate(txt, &x, &y, 2, 1);
	assert(blended_at(x, y) && blended_at(x, y)->buf == bb->strip);
	set_coordinate(txt, &x, &y, 7, 1);
	assert(blended_at(x, y) && blended_at(x, y)->buf == bb->strip);
	assert(n_blended == 4);

	/* in the top band the picture hides the glyph: one run again */
	settle(txt, 0);
	set_placement(0, 10, 1, 2, 10, TSM_FRAME_BAND_ABOVE_TEXT);
	blank_frame(txt);
	frame.placement_size = sizeof(placements[0]);
	frame.placement_count = 1;
	frame.placements = placements;
	cell_at(6, 1)->key = 'x';
	cell_at(6, 1)->flags = TSM_FRAME_CHANGED;
	run_frame(txt, false);
	assert(n_blended == 2 && blended_at(16, 16) && blended_at(16, 16)->buf == bb->strip);
	settle(txt, 0);
}

/* the lowest band shows through the default background only */
static void test_below_bg(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	unsigned int x, y;

	settle(txt, 0);
	set_placement(0, 11, 0, 0, 4, TSM_FRAME_BAND_BELOW_BG);
	blank_frame(txt);
	frame.placement_size = sizeof(placements[0]);
	frame.placement_count = 1;
	frame.placements = placements;
	cell_at(1, 0)->bg = (struct tsm_screen_color){.r = 10, .g = 20, .b = 30};
	run_frame(txt, false);
	/* cell 0 starts a run; cell 1 is composed and takes its background */
	assert(blended_at(0, 0) && blended_at(0, 0)->buf == bb->strip);
	set_coordinate(txt, &x, &y, 1, 0);
	assert(blended_at(x, y) && blended_at(x, y)->buf == bb->scratch);
	assert(blended_at(x, y)->first == 0xff0a141e);
	settle(txt, 0);
}

/* a full-screen placement @i of image @image, made in half-size cells, so
 * drawn at twice its size: a scaled picture of the whole screen */
static void set_scaled(unsigned int i, uint64_t key, uint64_t image)
{
	struct tsm_frame_placement *p = &placements[i];

	memset(p, 0, sizeof(*p));
	p->key = key;
	p->image_key = image;
	p->cols = disp_w / FAKE_CELL_W;
	p->rows = disp_h / FAKE_CELL_H;
	p->width = p->cols * FAKE_CELL_W / 2;
	p->height = p->rows * FAKE_CELL_H / 2;
	p->src_width = p->width;
	p->src_height = p->height;
	p->cell_width = FAKE_CELL_W / 2;
	p->cell_height = FAKE_CELL_H / 2;
	p->band = TSM_FRAME_BAND_ABOVE_TEXT;
}

static unsigned int pictures_of(struct bbulk *bb, uint64_t image)
{
	unsigned int n = 0;

	for (struct picture *p = bb->pictures; p; p = p->next)
		n += p->image_key == image;
	return n;
}

/*
 * The scaled pictures stay within twice the screen between frames, however
 * many a run of frames draws; within a frame, a picture the frame draws is
 * never freed to make room, so three half-transparent full-screen pictures
 * past the limit still compose from all three; past the process's image
 * budget a picture is left out and its cells retried on later frames.
 */
static void test_picture_cache(struct kmscon_text *txt)
{
	static uint32_t half[320 * 240];
	struct bbulk *bb = txt->data;
	uint32_t want = 0;
	unsigned int i, k;

	for (i = 0; i < 320 * 240; i++)
		half[i] = 0x80404040;
	for (i = 1; i <= 14; i++)
		test_images[i] = (struct tsm_frame_image){
			.width = 320, .height = 240, .stride = 320 * 4, .pixels = half};
	settle(txt, 0);

	/* five full-screen pictures, one a frame, twice over: 6 MiB of them */
	for (k = 0; k < 10; k++) {
		set_scaled(0, 30, k % 5 + 1);
		draw_placements(txt, 1);
		assert(pictures_of(bb, k % 5 + 1) == 1);
		assert(bb->picture_bytes <= picture_limit(bb));
	}

	/* three at once, 3.6 MiB against a 2.4 MiB limit, composed in a cell */
	for (i = 0; i < 3; i++)
		set_scaled(i, 40 + i, i + 1);
	draw_placements(txt, 3);
	for (i = 0; i < 3; i++)
		want = 0x80404040 + ((over_div255((want >> 24) * 127) << 24) |
				     (over_div255(((want >> 16) & 0xff) * 127) << 16) |
				     (over_div255(((want >> 8) & 0xff) * 127) << 8) |
				     over_div255((want & 0xff) * 127));
	assert(blended_at(0, 0) && blended_at(0, 0)->buf == bb->scratch);
	assert(blended_at(0, 0)->first == want);
	assert(bb->picture_bytes <= picture_limit(bb));

	/*
	 * fourteen at once, 17.2 MB, pass the 16 MiB budget: the last is left
	 * out, and its cells are drawn again on every later frame until it
	 * fits, not only on the two frames that follow the change
	 */
	for (i = 0; i < 14; i++)
		set_scaled(i, 50 + i, i + 1);
	pictures_peak = pictures_total;
	assert(draw_placements(txt, 14) == 0);
	assert(pictures_peak <= image_budget() && pictures_peak > 12 * 640 * 480 * 4);
	assert(draw_placements(txt, 14) == 0);
	assert(draw_placements(txt, 14) == 0 && n_blended > 0);
	set_scaled(0, 50, 1);
	draw_placements(txt, 1);
	assert(draw_placements(txt, 1) == 0);
	assert(draw_placements(txt, 1) == KMSCON_TEXT_UNCHANGED);

	settle(txt, 0);
	for (i = 1; i <= 14; i++)
		test_images[i].pixels = NULL;
}

/*
 * At 7680x2160 four frames are 265 MB, past the 192 MiB cap, so the budget is
 * the cap: three full-screen scaled pictures (199 MB) fit at once, a fourth
 * does not, and the frame's peak stays within the cap.
 */
static void test_budget_8k(void)
{
	struct kmscon_text txt;
	struct bbulk *bb;
	uint32_t *src = calloc(3840 * 1080, 4);
	size_t pic = (size_t)7680 * 2160 * 4, base, fit;
	unsigned int i;

	assert(src);
	for (i = 0; i < 3840 * 1080; i++)
		src[i] = 0xff204060;
	disp_w = 7680;
	disp_h = 2160;
	init_fake_txt(&txt);
	assert(kmscon_text_bbulk_ops.init(&txt) == 0);
	assert(bbulk_set(&txt) == 0);
	bb = txt.data;
	assert(image_budget() == IMAGE_BUDGET_MAX);
	for (i = 1; i <= 4; i++) {
		test_images[i] = (struct tsm_frame_image){
			.width = 3840, .height = 1080, .stride = 3840 * 4, .pixels = src};
		set_scaled(i - 1, 60 + i, i);
		placements[i - 1].flags = TSM_FRAME_OPAQUE;
	}

	/* other displays' pictures count against the same budget */
	base = pictures_total;
	fit = (IMAGE_BUDGET_MAX - base) / pic;
	assert(base == 0 && fit == 3);
	pictures_peak = pictures_total;
	draw_placements(&txt, 4);
	assert(pictures_peak <= IMAGE_BUDGET_MAX);
	assert(pictures_peak == base + fit * pic);
	assert(bb->picture_bytes <= picture_limit(bb));

	bbulk_unset(&txt);
	kmscon_text_bbulk_ops.destroy(&txt);
	for (i = 1; i <= 4; i++)
		test_images[i].pixels = NULL;
	free(src);
	disp_w = 640;
	disp_h = 480;
}

/* the scaler: copies at 1:1, means when it shrinks, keeps opaque pictures
 * opaque, and gives the same pixels whether or not it slices the columns */
static void test_picture_scale(void)
{
	static const uint32_t two[2] = {0xff000000, 0xffffffff};
	uint32_t src[12], out[64], *tall, *whole, col[1];
	unsigned int i, x;

	for (i = 0; i < 12; i++)
		src[i] = 0xff000000 | (i * 0x111111);
	assert(picture_scale(src, 4, 4, 3, 4, 3, 0, 0, 4, 3, out) == 0);
	assert(!memcmp(src, out, sizeof(src)));
	/* a part of it, from a source row longer than the picture */
	assert(picture_scale(src, 4, 3, 3, 3, 3, 1, 1, 3, 3, out) == 0);
	assert(out[0] == src[5] && out[1] == src[6] && out[2] == src[9] && out[3] == src[10]);

	assert(picture_scale(two, 2, 2, 1, 1, 1, 0, 0, 1, 1, out) == 0);
	assert(out[0] == 0xff808080);

	assert(picture_scale(src, 4, 4, 3, 7, 5, 0, 0, 7, 5, out) == 0);
	for (i = 0; i < 35; i++)
		assert(out[i] >> 24 == 0xff);
	assert(picture_scale(src, 4, 4, 3, 3, 2, 0, 0, 3, 2, out) == 0);
	for (i = 0; i < 6; i++)
		assert(out[i] >> 24 == 0xff);

	assert(picture_scale(src, 4, 4, 3, 4, 3, 0, 0, 5, 3, out) == -EINVAL);

	/* 200 x 8000 shrunk to 200 x 1: a ring of 8002 rows, sliced */
	tall = malloc(200 * 8000 * 4);
	whole = malloc(200 * 4);
	assert(tall && whole);
	for (i = 0; i < 200 * 8000; i++)
		tall[i] = 0xff000000 | ((i * 2654435761u) & 0xffffff);
	assert(picture_scale(tall, 200, 200, 8000, 200, 1, 0, 0, 200, 1, whole) == 0);
	for (x = 0; x < 200; x += 37) {
		assert(picture_scale(tall, 200, 200, 8000, 200, 1, x, 0, x + 1, 1, col) == 0);
		assert(col[0] == whole[x]);
	}
	free(tall);
	free(whole);
}

int main(void)
{
	struct kmscon_text txt;
	int ret;

	init_fake_txt(&txt);

	ret = kmscon_text_bbulk_ops.init(&txt);
	assert(ret == 0);
	struct bbulk *bb = txt.data;
	assert(bb != NULL);

	/* First call allocates */
	ret = bbulk_set(&txt);
	assert(ret == 0);
	unsigned int prev_cells = bb->cell_count;

	bbulk_unset(&txt);

	/* Second call with identical geometry should remain valid and fully damaged */
	ret = bbulk_set(&txt);
	assert(ret == 0);
	assert(bb->cell_count == prev_cells);
	assert(bb->sprites[KMSCON_DECO_CURLY] != NULL && bb->blank != NULL && bb->scratch != NULL);
	assert(bb->cell_flags != NULL);
	assert(bb->damage_rects != NULL);
	/* All cells should be marked damaged */
	for (unsigned i = 0; i < bb->cell_count; ++i)
		assert(bb->cell_flags[i].force && bb->cell_flags[i].damaged);

	/* Exercise prepare/render + damage path */
	struct tsm_screen_attr attr = {0};
	ret = bbulk_prepare(&txt, &attr);
	assert(ret == 0);
	ret = bbulk_render(&txt);
	assert(ret == 0);
	assert(bb->damage_rect_len > 0);

	test_two_frame_rule(&txt);
	test_damage_measured(&txt);
	test_draw_failure_retried(&txt);
	test_underline_cursor_colour(&txt);
	test_placement_damage(&txt);
	test_opaque_runs(&txt);
	test_below_bg(&txt);
	test_picture_scale();
	test_budget_8k();
	test_picture_cache(&txt);

	bbulk_unset(&txt);
	assert(bb->cell_flags == NULL);
	assert(bb->damage_rects == NULL);
	kmscon_text_bbulk_ops.destroy(&txt);
	return 0;
}
