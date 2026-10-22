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
unsigned int display_get_width(struct display *disp)
{
	(void)disp;
	return 640;
}
unsigned int display_get_height(struct display *disp)
{
	(void)disp;
	return 480;
}

/* Keep geometry tiny to minimize allocations */
#define FAKE_CELL_W 8
#define FAKE_CELL_H 16

/* Stub font metrics APIs used indirectly by text.c/text_bbulk.c */
unsigned int kmscon_text_get_cols(struct kmscon_text *txt, unsigned int font_width)
{
	(void)txt;
	(void)font_width;
	return 640 / FAKE_CELL_W;
}

unsigned int kmscon_text_get_rows(struct kmscon_text *txt, unsigned int font_height)
{
	(void)txt;
	(void)font_height;
	return 480 / FAKE_CELL_H;
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
	g->buf.width = w;
	g->buf.height = h;
	return g;
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

struct tsm_screen_color kmscon_text_ul_color(struct kmscon_text *txt,
					     const struct tsm_frame_cell *cell)
{
	(void)txt;
	return cell->fg;
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
bool display_has_damage(struct display *disp)
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

static struct tsm_frame_cell cells[(640 / FAKE_CELL_W) * (480 / FAKE_CELL_H)];
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

static void run_frame(struct kmscon_text *txt, bool cursor_on)
{
	struct tsm_screen_attr attr = {0};

	n_blended = 0;
	assert(bbulk_prepare(txt, &attr) == 0);
	assert(bbulk_draw(txt, &frame, cursor_on) == 0);
	assert(bbulk_render(txt) == 0);
}

/* blank cells, but @x, @y holds @key with @flags, if @x is set */
static void draw_frame_cell(struct kmscon_text *txt, int x, int y, uint64_t key, uint16_t flags)
{
	blank_frame(txt);
	if (x >= 0) {
		cell_at(x, y)->key = key;
		cell_at(x, y)->flags = flags;
	}
	run_frame(txt, false);
}

/*
 * One frame of blank cells; @changed_x, @changed_y is marked changed and holds
 * @key, if set.
 */
static void draw_frame_key(struct kmscon_text *txt, int changed_x, int changed_y, uint64_t key)
{
	draw_frame_cell(txt, changed_x, changed_y, key, TSM_FRAME_CHANGED);
}

static void draw_frame(struct kmscon_text *txt, int changed_x, int changed_y)
{
	draw_frame_key(txt, changed_x, changed_y, 'x');
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
	draw_frame(txt, 5, 3);
	assert(n_blended == 1 && was_blended(x, y));
	assert(bb->damage_rect_len == 1);

	/* frame N+1: unchanged, but drawn again into the other buffer */
	draw_frame(txt, -1, -1);
	assert(n_blended == 1 && was_blended(x, y));
	assert(bb->damage_rect_len == 0);

	/* frame N+2: both buffers hold it */
	draw_frame(txt, -1, -1);
	assert(n_blended == 0);
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
	test_draw_failure_retried(&txt);

	bbulk_unset(&txt);
	assert(bb->cell_flags == NULL);
	assert(bb->damage_rects == NULL);
	kmscon_text_bbulk_ops.destroy(&txt);
	return 0;
}
