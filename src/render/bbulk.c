/*
 * kmscon - Bit-Blitting Bulk Text Renderer Backend
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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

/**
 * SECTION:text_bbulk.c
 * @short_description: Bit-Blitting Bulk Text Renderer Backend
 * @include: text.h
 *
 * Similar to the bblit renderer but assembles an array of blit-requests and
 * pushes all of them at once to the video device.
 *
 * It draws libtsm's frame (ADR-102). A cell is pushed when the frame marks it
 * changed, and once more on the next frame, as kmscon uses double buffering
 * and the back buffer is two frames old: cell_flags.damaged records that the
 * cell was drawn because it changed. The frame's changes are this display's
 * own, since its last frame (kmscon_text_draw), so with damaged they span the
 * two frames the back buffer is behind. Changes the frame cannot know about
 * (the cursor and text blink phases, the pointer, a full redraw) are tracked
 * here; a full redraw also asks the next frame for every cell.
 *
 * On a display with damage clips, a frame's damage is every cell drawn into
 * the buffer on that frame (cell_flags.drawn): the cells changed on this frame
 * or the one before, the two frames the buffer is behind. FB_DAMAGE_CLIPS asks
 * for less, the change since the last plane update (drm_plane.c), so this is a
 * superset of it at one extra frame per change; a driver that uploads each
 * buffer on its own ignores the clips when the buffer changes. A frame that
 * drew nothing is reported unchanged and not presented, as a commit without
 * clips would update the whole plane.
 *
 * A cell draws its background, then its underline, then its glyph, then its
 * strike-through. An undecorated cell is one alpha-mask request; a decorated
 * one is composed in system memory and sent as one premultiplied ARGB request
 * over its background, so the device's memory is never read back.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "deco.h"
#include "font/font.h"
#include "shl/log.h"
#include "shl/lru.h"
#include "shl/misc.h"
#include "text.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "text_bbulk"

// Max horizontal distance of two damaged cells to be merged in a damage rectangle
#define DAMAGE_MERGE_LEN 3

/* Flags for each cell */
typedef union {
	struct {
		uint8_t damaged : 1; /* drawn on the last frame because it changed */
		uint8_t force : 1;   /* draw on the next frame whatever the frame says */
		uint8_t cursor : 1;  /* last drawn as the cursor */
		uint8_t hidden : 1;  /* last drawn in the blink phase that hides text */
		uint8_t drawn : 1;   /* drawn on this frame: in its damage */
	};
	uint8_t u8;
} cell_flags_t;

struct bbulk {
	unsigned int sw;	     /* screen width */
	unsigned int sh;	     /* screen height */
	unsigned int off_x;	     /* offset of the first cell */
	unsigned int off_y;	     /* offset of the first cell */
	unsigned int max_x;	     /* maximum x offset of the last cell */
	unsigned int max_y;	     /* maximum y offset of the last cell */
	struct tsm_screen_attr attr; /* attributes for background color */

	unsigned int requests; /* number of blend calls, for debugging */
	struct shl_lru *glyphs;
	cell_flags_t *cell_flags;
	unsigned int cell_count;
	struct kmscon_text_deco deco; /* the decoration colours last drawn */

	/* one-cell sprites, rotated: decorations, and an empty glyph */
	struct kmscon_glyph *sprites[KMSCON_DECO_COUNT];
	struct kmscon_glyph *blank;
	struct video_buffer *scratch; /* a decorated cell is composed here */

	struct video_rect *damage_rects;
	unsigned int damage_rect_len;
	uint8_t redraw;
};

static int bbulk_init(struct kmscon_text *txt)
{
	struct bbulk *bb;

	bb = calloc(1, sizeof(*bb));
	if (!bb)
		return -ENOMEM;

	txt->data = bb;
	return 0;
}

static void bbulk_destroy(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	free(bb);
}

static void damage_cell(struct bbulk *bb, unsigned int off)
{
	bb->cell_flags[off].force = 1;
	bb->cell_flags[off].damaged = 1;
	bb->cell_flags[off].drawn = 1;
}

static void compute_border(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		bb->off_x = (bb->sw - txt->cols * FONT_WIDTH(txt)) / 2;
		bb->off_y = (bb->sh - txt->rows * FONT_HEIGHT(txt)) / 2;
		bb->max_x = bb->off_x + txt->cols * FONT_WIDTH(txt);
		bb->max_y = bb->off_y + txt->rows * FONT_HEIGHT(txt);
	} else {
		bb->off_x = (bb->sw - txt->rows * FONT_HEIGHT(txt)) / 2;
		bb->off_y = (bb->sh - txt->cols * FONT_WIDTH(txt)) / 2;
		bb->max_x = bb->off_x + txt->rows * FONT_HEIGHT(txt);
		bb->max_y = bb->off_y + txt->cols * FONT_WIDTH(txt);
	}
	display_set_cursor_offset(txt->disp, bb->off_x, bb->off_y);
}

/*
 * Rotate a glyph to the given orientation
 * Return a new rotated glyph, and free the original glyph
 */
static struct kmscon_glyph *bbulk_rotate_glyph(struct kmscon_glyph *glyph,
					       enum Orientation orientation)
{
	struct video_buffer *buf = &glyph->buf;
	struct kmscon_glyph *rglyph;
	unsigned int bpp = buf->format == VIDEO_FORMAT_ARGB32 ? 4 : 1;
	int width, height, i, j;
	uint8_t *dst, *src;

	rglyph = calloc(1, sizeof(*rglyph) + glyph->buf.width * glyph->buf.height * bpp);
	if (!rglyph)
		goto err_free;

	if (orientation == OR_NORMAL || orientation == OR_UPSIDE_DOWN) {
		width = buf->width;
		height = buf->height;
	} else {
		width = buf->height;
		height = buf->width;
	}
	src = buf->data;
	dst = rglyph->buf.data;

	switch (orientation) {
	default:
	case OR_NORMAL:
		/* should never happen */
		break;
	case OR_RIGHT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++)
				memcpy(&dst[(j * width + (width - i - 1)) * bpp], &src[j * bpp], bpp);
			src += buf->width * bpp;
		}
		break;
	case OR_UPSIDE_DOWN:
		src += (buf->height - 1) * buf->width * bpp;
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++)
				memcpy(&dst[j * bpp], &src[(buf->width - j - 1) * bpp], bpp);
			dst += width * bpp;
			src -= buf->width * bpp;
		}
		break;
	case OR_LEFT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++)
				memcpy(&dst[((height - j - 1) * width + i) * bpp], &src[j * bpp], bpp);
			src += buf->width * bpp;
		}
	}
	rglyph->buf.width = width;
	rglyph->buf.height = height;
	rglyph->buf.format = buf->format;
	rglyph->double_width = glyph->double_width;
	rglyph->key = glyph->key;

err_free:
	free(glyph);
	return rglyph;
}

static void free_sprites(struct bbulk *bb)
{
	int i;

	for (i = 0; i < KMSCON_DECO_COUNT; i++) {
		free(bb->sprites[i]);
		bb->sprites[i] = NULL;
	}
	free(bb->blank);
	bb->blank = NULL;
	free(bb->scratch);
	bb->scratch = NULL;
}

/*
 * The decoration sprites for this font's cell size and metrics, rotated like
 * the glyphs; a blank two-cell glyph for empty cells; and the scratch buffer a
 * decorated cell, at most two cells, is composed in.
 */
static int make_sprites(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	unsigned int fw = FONT_WIDTH(txt), fh = FONT_HEIGHT(txt);
	struct kmscon_font_deco m;
	int i;

	kmscon_font_get_deco(txt->font, &m);
	for (i = KMSCON_DECO_SINGLE; i < KMSCON_DECO_COUNT; i++) {
		bb->sprites[i] = kmscon_deco_render(i, fw, fh, &m);
		if (bb->sprites[i] && txt->orientation != OR_NORMAL)
			bb->sprites[i] = bbulk_rotate_glyph(bb->sprites[i], txt->orientation);
		if (!bb->sprites[i])
			goto err;
	}

	/* rotated or not, all zeros; only its dimensions differ */
	bb->blank = calloc(1, sizeof(*bb->blank) + 2 * fw * fh);
	if (!bb->blank)
		goto err;
	bb->blank->double_width = true;
	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		bb->blank->buf.width = 2 * fw;
		bb->blank->buf.height = fh;
	} else {
		bb->blank->buf.width = fh;
		bb->blank->buf.height = 2 * fw;
	}

	bb->scratch = calloc(1, sizeof(*bb->scratch) + 2 * fw * fh * 4);
	if (!bb->scratch)
		goto err;
	bb->scratch->format = VIDEO_FORMAT_ARGB32;
	return 0;

err:
	free_sprites(bb);
	return -ENOMEM;
}

static int bbulk_set(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int max_damage_rects;
	int i;

	memset(bb, 0, sizeof(*bb));

	bb->sw = display_get_width(txt->disp);
	bb->sh = display_get_height(txt->disp);

	if (!bb->sw || !bb->sh)
		return -EINVAL;

	txt->max_cols = kmscon_text_get_cols(txt, FONT_WIDTH(txt));
	txt->max_rows = kmscon_text_get_rows(txt, FONT_HEIGHT(txt));
	txt->cols = txt->max_cols;
	txt->rows = txt->max_rows;
	compute_border(txt);

	bb->cell_count = txt->max_cols * txt->max_rows;
	max_damage_rects = SHL_DIV_ROUND_UP(txt->max_cols, DAMAGE_MERGE_LEN + 1) * txt->max_rows;

	bb->cell_flags = calloc(bb->cell_count, sizeof(*bb->cell_flags));
	if (!bb->cell_flags)
		goto err_no_mem;

	bb->damage_rects = calloc(max_damage_rects, sizeof(*bb->damage_rects));
	if (!bb->damage_rects)
		goto free_damages;

	for (i = 0; i < (int)bb->cell_count; i++)
		damage_cell(bb, i);

	/* lru size should be at least bb->cells large */
	bb->glyphs = shl_lru_new(2 * bb->cell_count);
	if (!bb->glyphs)
		goto free_r_damages;

	if (make_sprites(txt))
		goto free_glyphs;
	return 0;

free_glyphs:
	shl_lru_free(bb->glyphs);
	bb->glyphs = NULL;
free_r_damages:
	free(bb->damage_rects);
	bb->damage_rects = NULL;
free_damages:
	free(bb->cell_flags);
	bb->cell_flags = NULL;
err_no_mem:
	return -ENOMEM;
}

static void bbulk_unset(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	shl_lru_free(bb->glyphs);
	free(bb->damage_rects);
	free(bb->cell_flags);
	free_sprites(bb);
	bb->glyphs = NULL;
	bb->damage_rects = NULL;
	bb->cell_flags = NULL;
}

static void bbulk_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows)
{
	struct bbulk *bb = txt->data;

	txt->cols = cols;
	txt->rows = rows;
	compute_border(txt);
	bb->redraw = 2;
}

static int bbulk_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	bbulk_unset(txt);
	txt->orientation = orientation;
	return bbulk_set(txt);
}

static bool glyph_matches(const void *value, const void *data)
{
	const struct kmscon_glyph *glyph = value;

	return kmscon_glyph_key_equal(&glyph->key, data);
}

/*
 * The glyph for frame key @key (a codepoint or a cluster), cached under the
 * key plus bold, italic and width; a hash collision is a miss.
 */
static struct kmscon_glyph *find_glyph(struct kmscon_text *txt, uint64_t key, uint16_t flags,
				       unsigned int width)
{
	struct bbulk *bb = txt->data;
	struct kmscon_glyph *glyph;
	struct kmscon_font *font = txt->font;
	struct kmscon_font_attr attr;
	struct kmscon_glyph_key k = {0};
	const uint32_t *cps;
	uint32_t one;
	size_t len;
	uint64_t hash;

	attr.bold = !!(flags & TSM_FRAME_BOLD);
	attr.italic = !!(flags & TSM_FRAME_ITALIC);

	k.key = key;
	k.bold = attr.bold;
	k.italic = attr.italic;
	k.width = width;
	hash = kmscon_glyph_key_hash(&k);

	glyph = shl_lru_find(bb->glyphs, hash, glyph_matches, &k);
	if (glyph)
		return glyph;

	cps = kmscon_text_get_cluster(txt, key, &one, &len);

	glyph = kmscon_font_render(font, &attr, cps, len, width);
	if (!glyph)
		return NULL;

	if (txt->orientation != OR_NORMAL)
		glyph = bbulk_rotate_glyph(glyph, txt->orientation);
	if (!glyph)
		return NULL;
	glyph->key = k;

	if (shl_lru_insert(bb->glyphs, hash, glyph)) {
		free(glyph);
		return NULL;
	}
	return glyph;
}

/*
 * Returns the top left corner of a Cell
 */
static void set_coordinate(struct kmscon_text *txt, unsigned int *x, unsigned int *y,
			   unsigned int posx, unsigned int posy)
{
	struct bbulk *bb = txt->data;

	switch (txt->orientation) {
	case OR_NORMAL:
		*x = posx * FONT_WIDTH(txt) + bb->off_x;
		*y = posy * FONT_HEIGHT(txt) + bb->off_y;
		break;
	case OR_UPSIDE_DOWN:
		*x = bb->max_x - (posx + 1) * FONT_WIDTH(txt);
		*y = bb->max_y - (posy + 1) * FONT_HEIGHT(txt);
		break;
	case OR_RIGHT:
		*x = bb->max_x - (posy + 1) * FONT_HEIGHT(txt);
		*y = posx * FONT_WIDTH(txt) + bb->off_y;
		break;
	case OR_LEFT:
		*x = posy * FONT_HEIGHT(txt) + bb->off_x;
		*y = bb->max_y - (posx + 1) * FONT_WIDTH(txt);
		break;
	}
}

static inline uint32_t over_div255(uint32_t t)
{
	t += 0x80;
	return (t + (t >> 8)) >> 8;
}

/*
 * Paint @src over the premultiplied ARGB scratch @dst, with @src's top-left at
 * @ox, @oy, clipped to @dst: a mask tinted with @c, or a colour glyph as it is
 * (premultiplied, @c unused). Both are packed.
 */
static void compose(struct video_buffer *dst, const struct video_buffer *src, unsigned int ox,
		    unsigned int oy, struct tsm_screen_color c)
{
	bool argb = src->format == VIDEO_FORMAT_ARGB32;
	unsigned int x, y, w, h;
	uint32_t *d;

	if (ox >= dst->width || oy >= dst->height)
		return;
	w = min(src->width, dst->width - ox);
	h = min(src->height, dst->height - oy);

	for (y = 0; y < h; y++) {
		d = &((uint32_t *)dst->data)[(oy + y) * dst->width + ox];
		for (x = 0; x < w; x++) {
			uint32_t a, na, p = d[x], sr, sg, sb;

			if (argb) {
				uint32_t s = ((const uint32_t *)src->data)[y * src->width + x];

				a = s >> 24;
				sr = (s >> 16) & 0xff;
				sg = (s >> 8) & 0xff;
				sb = s & 0xff;
			} else {
				a = src->data[y * src->width + x];
				sr = over_div255(c.r * a);
				sg = over_div255(c.g * a);
				sb = over_div255(c.b * a);
			}
			if (!a)
				continue;
			na = 255 - a;
			d[x] = ((a + over_div255((p >> 24) * na)) << 24) |
			       ((sr + over_div255(((p >> 16) & 0xff) * na)) << 16) |
			       ((sg + over_div255(((p >> 8) & 0xff) * na)) << 8) |
			       (sb + over_div255((p & 0xff) * na));
		}
	}
}

/* paint sprite @kind over each of the @ncells cells of the scratch buffer */
static void compose_sprite(struct kmscon_text *txt, enum kmscon_deco kind, unsigned int ncells,
			   struct tsm_screen_color c)
{
	struct bbulk *bb = txt->data;
	bool across = txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN;
	unsigned int i;

	for (i = 0; i < ncells; i++)
		compose(bb->scratch, &bb->sprites[kind]->buf, across ? i * FONT_WIDTH(txt) : 0,
			across ? 0 : i * FONT_WIDTH(txt), c);
}

/*
 * Draw @cell at @posx, @posy over @ncells cells (2 for a wide cell with room
 * for its second half), with glyph key @key (0: none) drawn @width cells wide.
 * When the glyph cannot be had, the cell is drawn without it and -ENOMEM
 * returned, for the caller to draw it again. @ul_cursor: @cell's underline is
 * an underline cursor, which is no decoration and draws in the foreground.
 */
static int bbulk_draw_cell(struct kmscon_text *txt, const struct tsm_frame_cell *cell, uint64_t key,
			   unsigned int width, unsigned int posx, unsigned int posy,
			   unsigned int ncells, bool ul_cursor)
{
	struct bbulk *bb = txt->data;
	struct kmscon_glyph *glyph = bb->blank;
	struct video_blend_req req;
	bool across = txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN;
	bool strike = cell->flags & TSM_FRAME_STRIKE;
	int ret = 0;

	if (key) {
		glyph = find_glyph(txt, key, cell->flags, width);
		if (!glyph) {
			glyph = bb->blank;
			key = 0;
			ret = -ENOMEM;
		}
	}

	if (ncells == 2 && (txt->orientation == OR_LEFT || txt->orientation == OR_UPSIDE_DOWN))
		/*
		 * In case of left or upside down orientation, we need to draw to the
		 * next cell, as the glyph is already rotated, so start on the next cell
		 * and end on this cell
		 */
		set_coordinate(txt, &req.x, &req.y, posx + 1, posy);
	else
		set_coordinate(txt, &req.x, &req.y, posx, posy);

	/* the cells drawn; a wide glyph in the last column is cut at the margin */
	req.w = across ? ncells * FONT_WIDTH(txt) : FONT_HEIGHT(txt);
	req.h = across ? FONT_HEIGHT(txt) : ncells * FONT_WIDTH(txt);
	req.fr = cell->fg.r;
	req.fg = cell->fg.g;
	req.fb = cell->fg.b;
	req.br = cell->bg.r;
	req.bg = cell->bg.g;
	req.bb = cell->bg.b;

	if (cell->underline == TSM_UNDERLINE_NONE && !strike) {
		req.buf = &glyph->buf;
	} else {
		struct video_buffer *sc = bb->scratch;

		sc->width = min(req.w, glyph->buf.width);
		sc->height = min(req.h, glyph->buf.height);
		memset(sc->data, 0, sc->width * sc->height * 4);

		if (cell->underline > TSM_UNDERLINE_NONE && cell->underline < KMSCON_DECO_STRIKE)
			compose_sprite(txt, cell->underline, ncells,
				       ul_cursor ? cell->fg : kmscon_text_ul_color(txt, cell));
		if (key)
			compose(sc, &glyph->buf, 0, 0, cell->fg);
		if (strike)
			compose_sprite(txt, KMSCON_DECO_STRIKE, ncells,
				       kmscon_text_strike_color(txt, cell));
		req.buf = sc;
	}

	display_blend(txt->disp, &req);
	bb->requests++;
	return ret;
}

static bool is_vbar(enum tsm_screen_cursor_style style)
{
	return style == TSM_SCREEN_CURSOR_VBAR_BLINK || style == TSM_SCREEN_CURSOR_VBAR_STEADY;
}

static bool is_underline(enum tsm_screen_cursor_style style)
{
	return style == TSM_SCREEN_CURSOR_UNDERLINE_BLINK ||
	       style == TSM_SCREEN_CURSOR_UNDERLINE_STEADY;
}

static int bbulk_draw(struct kmscon_text *txt, const struct tsm_frame *frame, bool cursor_on)
{
	struct bbulk *bb = txt->data;
	unsigned int posx, posy, off, cols, rows, ncells, width;
	const struct tsm_frame_cell *fc, *cell;
	bool is_cursor, hidden, changed;
	cell_flags_t *f;
	uint64_t key;

	cols = min(txt->cols, frame->cols);
	rows = min(txt->rows, frame->rows);

	for (posy = 0; posy < rows; posy++) {
		for (posx = 0; posx < cols; posx++) {
			off = posx + posy * txt->cols;
			fc = tsm_frame_cell_at(frame, posx, posy);
			f = &bb->cell_flags[off];

			/* a trailing half is drawn by the wide cell before it */
			if (!fc->width && posx)
				continue;

			ncells = fc->width == 2 && posx + 1 < cols ? 2 : 1;
			is_cursor = cursor_on && frame->cursor.x == posx && frame->cursor.y == posy;
			cell = is_cursor ? &frame->cursor.cell : fc;
			hidden = !is_cursor && (fc->flags & TSM_FRAME_BLINK) && txt->blinking;

			changed = (fc->flags & TSM_FRAME_CHANGED) || f->force ||
				  f->cursor != is_cursor || f->hidden != hidden;
			if (ncells == 2)
				changed = changed || f[1].force;

			/* drawn on the last frame: the back buffer still lacks it */
			if (!changed && !f->damaged)
				continue;

			f->damaged = changed;
			f->force = 0;
			f->cursor = is_cursor;
			f->hidden = hidden;
			f->drawn = 1;
			if (ncells == 2) {
				f[1].damaged = changed;
				f[1].force = 0;
				f[1].drawn = 1;
			}

			key = hidden ? 0 : cell->key;
			width = cell->width == 2 ? 2 : 1;
			if (is_cursor && is_vbar(frame->cursor.style)) {
				/* the bar, at the left of the cell; a wide cell's
				 * second half is left empty */
				key = FONT_VBAR;
				width = 1;
				if (ncells == 2)
					bbulk_draw_cell(txt, cell, 0, 1, posx + 1, posy, 1, false);
				ncells = 1;
			}

			/* not drawn: the frame will not mark it again, so we do */
			if (bbulk_draw_cell(txt, cell, key, width, posx, posy, ncells,
					    is_cursor && is_underline(frame->cursor.style))) {
				f->force = 1;
				if (ncells == 2)
					f[1].force = 1;
			}
		}
	}
	return 0;
}

/*
 * When the pointer move over, mark the 4 underlying cells as damaged.
 */
static void mark_damaged(struct kmscon_text *txt, struct bbulk *bb, unsigned int x, unsigned int y)
{
	unsigned int posx = 0;
	unsigned int posy = 0;
	unsigned int fw, fh, off;
	fw = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
	fh = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);

	if (x > fw)
		posx = (x - fw) / FONT_WIDTH(txt);
	if (y > fh)
		posy = (y - fh) / FONT_HEIGHT(txt);

	if (posx >= txt->cols)
		posx = txt->cols - 1;

	if (posy >= txt->rows)
		posy = txt->rows - 1;

	off = posx + posy * txt->cols;

	damage_cell(bb, off);

	if (posx + 1 < txt->cols)
		damage_cell(bb, off + 1);

	if (posy + 1 < txt->rows)
		damage_cell(bb, off + txt->cols);

	if (posx + 1 < txt->cols && posy + 1 < txt->rows)
		damage_cell(bb, off + 1 + txt->cols);
}

static unsigned int clamp(unsigned int val, unsigned int min, unsigned int max)
{
	if (val < min)
		return min;
	if (val > max)
		return max;
	return val;
}

/*
 * pointer_x and pointer_y are the center of the pointer sprite, in the
 * non-rotated screen.
 */
static void set_pointer_coordinate(struct bbulk *bb, struct kmscon_text *txt,
				   struct video_blend_req *req, unsigned int pointer_x,
				   unsigned int pointer_y)
{
	unsigned int hf_w, hf_h, x, y;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		hf_w = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
		hf_h = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);
	} else {
		hf_w = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);
		hf_h = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
	}

	switch (txt->orientation) {
	default:
	case OR_NORMAL:
		x = pointer_x + bb->off_x;
		y = pointer_y + bb->off_y;
		break;
	case OR_UPSIDE_DOWN:
		x = bb->max_x - pointer_x;
		y = bb->max_y - pointer_y;
		break;
	case OR_RIGHT:
		x = bb->max_x - pointer_y;
		y = pointer_x + bb->off_y;
		break;
	case OR_LEFT:
		x = pointer_y + bb->off_x;
		y = bb->max_y - pointer_x;
		break;
	}
	x = clamp(x, hf_w, bb->max_x - hf_w);
	y = clamp(y, hf_h, bb->max_y - hf_h);

	req->x = x - hf_w;
	req->y = y - hf_h;
}

static int bbulk_draw_pointer(struct kmscon_text *txt, unsigned int pointer_x,
			      unsigned int pointer_y)
{
	struct bbulk *bb = txt->data;
	struct video_blend_req req;
	struct kmscon_glyph *bb_glyph;
	unsigned int fw2 = FONT_WIDTH(txt) / 2;
	unsigned int fh2 = FONT_HEIGHT(txt) / 2;

	pointer_x = clamp(pointer_x, fw2, txt->cols * FONT_WIDTH(txt) - fw2);
	pointer_y = clamp(pointer_y, fh2, txt->rows * FONT_HEIGHT(txt) - fh2);

	bb_glyph = find_glyph(txt, 'I', 0, 1);
	if (!bb_glyph)
		return -ENOMEM;

	req.buf = &bb_glyph->buf;
	req.w = bb_glyph->buf.width;
	req.h = bb_glyph->buf.height;
	set_pointer_coordinate(bb, txt, &req, pointer_x, pointer_y);
	mark_damaged(txt, bb, pointer_x, pointer_y);

	req.fr = bb->attr.fr;
	req.fg = bb->attr.fg;
	req.fb = bb->attr.fb;
	req.br = bb->attr.br;
	req.bg = bb->attr.bg;
	req.bb = bb->attr.bb;
	display_blend(txt->disp, &req);
	bb->requests++;
	return 0;
}

static void add_damage(struct bbulk *bb, struct video_rect *r)
{
	struct video_rect *out = &bb->damage_rects[bb->damage_rect_len];

	*out = *r;
	bb->damage_rect_len++;
}

static void merge_damage(struct bbulk *bb, struct video_rect *r)
{
	struct video_rect *out = &bb->damage_rects[bb->damage_rect_len - 1];

	out->x1 = min(out->x1, r->x1);
	out->x2 = max(out->x2, r->x2);
	out->y1 = min(out->y1, r->y1);
	out->y2 = max(out->y2, r->y2);
}

/*
 * Simple merge algorithm, on each line, if two damaged cells are less than
 * DAMAGE_MERGE_LEN away, include the two cells in one damage rectangle. Each
 * frame's drawn cells are reported once, and forgotten.
 */
static void bbulk_compute_damage(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int posx, posy, off;
	struct video_rect r;
	unsigned int x1 = 0, y1 = 0;
	unsigned int fw, fh;
	int prev;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		fw = FONT_WIDTH(txt);
		fh = FONT_HEIGHT(txt);
	} else {
		fw = FONT_HEIGHT(txt);
		fh = FONT_WIDTH(txt);
	}

	for (posy = 0; posy < txt->rows; posy++) {
		prev = 0;
		for (posx = 0; posx < txt->cols; posx++) {
			off = posx + posy * txt->cols;
			if (!bb->cell_flags[off].drawn) {
				if (prev)
					prev--;
				continue;
			}
			bb->cell_flags[off].drawn = 0;
			set_coordinate(txt, &x1, &y1, posx, posy);
			r.x1 = x1;
			r.y1 = y1;
			r.x2 = x1 + fw;
			r.y2 = y1 + fh;
			if (prev)
				merge_damage(bb, &r);
			else
				add_damage(bb, &r);
			prev = DAMAGE_MERGE_LEN;
		}
	}
}
static int bbulk_render(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	unsigned int i;

	// log_debug("bbulk, redraw %d cells", bb->requests);
	bbulk_compute_damage(txt);
	if (txt->measure) {
		txt->frame_damage_px = 0;
		for (i = 0; i < bb->damage_rect_len; i++)
			txt->frame_damage_px +=
				(uint64_t)(bb->damage_rects[i].x2 - bb->damage_rects[i].x1) *
				(bb->damage_rects[i].y2 - bb->damage_rects[i].y1);
	}
	if (!display_supports_damage(txt->disp))
		return 0;
	display_set_damage(txt->disp, bb->damage_rect_len, bb->damage_rects);
	return bb->damage_rect_len ? 0 : KMSCON_TEXT_UNCHANGED;
}

static int bbulk_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	int i;

	bb->requests = 0;
	bb->damage_rect_len = 0;

	/*
	 * if default colors or decoration colors have changed, or we switch
	 * from a dirty screen, redraw completely the next 2 frames.
	 */
	if (memcmp(&bb->attr, attr, sizeof(*attr)) || memcmp(&bb->deco, &txt->deco, sizeof(bb->deco)) ||
	    display_need_redraw(txt->disp))
		bb->redraw = 2;

	bb->attr = *attr;
	bb->deco = txt->deco;

	if (bb->redraw) {
		kmscon_text_redraw_all(txt);
		display_clear(txt->disp, attr->br, attr->bg, attr->bb);
		for (i = 0; i < bb->cell_count; i++)
			damage_cell(bb, i);
		bb->redraw--;
	}

	return 0;
}

struct kmscon_text_ops kmscon_text_bbulk_ops = {
	.name = "bbulk",
	.owner = NULL,
	.init = bbulk_init,
	.destroy = bbulk_destroy,
	.set = bbulk_set,
	.unset = bbulk_unset,
	.resize = bbulk_resize,
	.rotate = bbulk_rotate,
	.prepare = bbulk_prepare,
	.draw = bbulk_draw,
	.draw_pointer = bbulk_draw_pointer,
	.render = bbulk_render,
	.abort = NULL,
};
