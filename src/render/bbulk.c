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
 *
 * Images (ADR-103) are the frame's placements, in three bands: below cell
 * backgrounds, showing through cells on the default background only; below
 * text; and above text. A cell no placement covers draws as above. A cell a
 * placement covers is composed in system memory, from the highest layer that
 * hides everything under it upwards: placements of the lowest band, the
 * cell's background unless it is the default, placements of the middle band,
 * the cell's decorations and glyph, placements of the top band. It is sent as
 * one ARGB request over the default background. A cell wholly under an opaque
 * placement that nothing draws over (no glyph or decoration, no later
 * placement, and in the lowest band the default background) takes the
 * picture's pixels as they are, and runs of such cells along a row go as one
 * request: a full-screen picture is one request per row.
 *
 * A picture drawn at another size than its source, or on a rotated display, is
 * scaled and rotated once and kept, per image and drawn size, up to twice the
 * screen's size in ARGB; the least recently used goes first. The pictures draw
 * on ADR-103's image budget (image_budget.c), which libtsm's images share, and
 * with them never pass it, not even within a frame. A picture that needs room
 * takes it from this display's pictures the frame does not place, then, in
 * ADR-103's order, from other displays' pictures their last frame did not
 * draw and from libtsm's images that are unplaced or placed only in
 * scrollback, never one this frame draws (operator's answer, 2026-10-23):
 * viewing a new picture can push older ones out of scrollback. Nothing is
 * freed for it unless that makes it fit; one that does not fit is left out
 * until the room in the budget grows (pictures or images freed, a display
 * added or removed, a larger limit) or this display's placements change, and
 * idle frames do not compose it again. A libtsm image that needs room takes it
 * first from the pictures the last frame did not draw. While one is left out
 * on any display, each display frees the pictures no placement of its shows
 * any more as its frame starts, and that frame tries its own left-out
 * placements again. A terminal whose session leaves the foreground frees its
 * pictures. The scaler's working memory (at most 16 MiB) and the buffer a
 * picture is rotated in (at most one screen) live only while one picture is
 * made and are not counted.
 *
 * Placement damage: this display keeps the placements it drew last. One added,
 * removed, or changed in any way that changes what it draws (moved, resized,
 * clipped, its band or z, its image) forces the cells of its old and new boxes,
 * which are then drawn on this frame and the next, as changed cells are. A
 * placement whose libtsm frame flag says changed but which draws the same is
 * not redrawn: libtsm flags every placement when anything scrolls, a region
 * beside a picture included.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "deco.h"
#include "font/font.h"
#include "image_budget.h"
#include "picture.h"
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

/* a placement as this display draws it (ADR-103) */
struct place {
	struct tsm_frame_placement fp; /* the frame's, without TSM_FRAME_CHANGED */
	bool overlay;		       /* kmscon's own, after the frame's */
	bool starved;		       /* left out for the budget: drawn again once
					  the budget or the placements change */
	/* its box's cells on the screen: [c0, c1) x [r0, r1) */
	unsigned int c0, c1, r0, r1;
	/*
	 * 0 until a cell first needs it this frame; then 1 when it draws, -1
	 * when it draws nothing, or -ENOMEM to try again on a later frame
	 */
	int state;
	/* its picture's visible part in grid pixels, [x0, x1) x [y0, y1) */
	unsigned int x0, x1, y0, y1;
	/* those pixels on the screen, rotated: @stride pixels a row */
	struct video_rect screen;
	const uint32_t *pix;
	size_t stride;
};

/* a picture scaled or rotated for drawing, kept between frames */
struct picture {
	struct picture *next;
	bool overlay;
	uint64_t image_key;
	unsigned int src_x, src_y, src_w, src_h; /* of the image */
	unsigned int dw, dh;			 /* the size it is drawn at */
	unsigned int u0, v0, u1, v1;		 /* the part of that kept */
	uint64_t last_use;
	size_t bytes;
	uint32_t pixels[];
};

struct bbulk {
	/* first, so bbulk_trim finds bbulk by it; in the budget while counted is set */
	struct image_budget_cache cache;
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

	/* the image layer (ADR-103) */
	struct place *places; /* this frame's, in draw order */
	unsigned int n_places;
	struct place *prev; /* the last frame's, by key */
	unsigned int n_prev;
	unsigned int places_cap;  /* of both */
	unsigned int *row_first;  /* per row, its first entry in row_places */
	unsigned int *row_places; /* placements by row, in draw order */
	unsigned int row_places_cap;
	unsigned int *cover;	     /* the placements over one cell */
	unsigned int grid_w, grid_h; /* the frame's drawn cells, in pixels */
	struct picture *pictures;
	size_t picture_bytes;
	struct display *counted; /* the display counted in the image budget, while set */
	uint64_t starved_gen;	 /* image_budget_gen() when this display last gave up */
	bool starving;		 /* a placement left out: counted in starving */
	uint64_t frame_count;
	struct video_buffer *strip; /* a run of cells under an opaque picture */
	struct {
		unsigned int place, posx, posy, n;
	} run;
};

static void bbulk_trim(struct image_budget_cache *cache, size_t want);
static size_t bbulk_trimmable(struct image_budget_cache *cache);

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
				memcpy(&dst[(j * width + (width - i - 1)) * bpp], &src[j * bpp],
				       bpp);
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
				memcpy(&dst[((height - j - 1) * width + i) * bpp], &src[j * bpp],
				       bpp);
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
	free(bb->strip);
	bb->strip = NULL;
}

/* the renderers with a placement left out for the budget */
static unsigned int starving;

static void picture_free(struct bbulk *bb, struct picture *p)
{
	bb->picture_bytes -= p->bytes;
	image_budget_remove_picture(p->bytes);
	free(p);
}

/* @bb has a placement left out for the budget, or no longer */
static void starving_set(struct bbulk *bb, bool on)
{
	if (bb->starving == on)
		return;
	bb->starving = on;
	if (on)
		starving++;
	else
		starving--;
}

static void free_images(struct bbulk *bb)
{
	struct picture *p, *next;

	for (p = bb->pictures; p; p = next) {
		next = p->next;
		picture_free(bb, p);
	}
	bb->pictures = NULL;
	starving_set(bb, false);
	free(bb->places);
	free(bb->prev);
	free(bb->row_first);
	free(bb->row_places);
	free(bb->cover);
	bb->places = bb->prev = NULL;
	bb->row_first = bb->row_places = bb->cover = NULL;
	bb->n_places = bb->n_prev = bb->places_cap = bb->row_places_cap = 0;
}

/*
 * The decoration sprites for this font's cell size and metrics, rotated like
 * the glyphs; a blank two-cell glyph for empty cells; the scratch buffer a
 * decorated or covered cell, at most two cells, is composed in; and the strip
 * a row of cells under an opaque picture is copied into.
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

	bb->strip = calloc(1, sizeof(*bb->strip) + (size_t)txt->max_cols * fw * fh * 4);
	if (!bb->strip)
		goto err;
	bb->strip->format = VIDEO_FORMAT_ARGB32;
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

	bb->row_first = calloc(txt->max_rows + 1, sizeof(*bb->row_first));
	if (!bb->row_first)
		goto free_glyphs;

	if (make_sprites(txt))
		goto free_glyphs;
	if (image_budget_count_display(txt->disp, (uint64_t)bb->sw * bb->sh))
		goto free_sprites;
	bb->counted = txt->disp;
	bb->cache.trim = bbulk_trim;
	bb->cache.trimmable = bbulk_trimmable;
	image_budget_add_cache(&bb->cache);
	return 0;

free_sprites:
	free_sprites(bb);
free_glyphs:
	free(bb->row_first);
	bb->row_first = NULL;
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
	free_images(bb);
	if (bb->counted) {
		image_budget_remove_cache(&bb->cache);
		image_budget_uncount_display(bb->counted);
	}
	bb->counted = NULL;
	bb->glyphs = NULL;
	bb->damage_rects = NULL;
	bb->cell_flags = NULL;
}

/* a session left the foreground: its scaled pictures give their room back */
static void bbulk_release(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	struct picture *p, *next;

	for (p = bb->pictures; p; p = next) {
		next = p->next;
		picture_free(bb, p);
	}
	bb->pictures = NULL;
	starving_set(bb, false);
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

/* ---- the image layer (ADR-103) ---- */

/* the screen rectangle of the grid's pixels @x, @y, @w x @h, as rotated */
static struct video_rect grid_to_screen(struct kmscon_text *txt, unsigned int x, unsigned int y,
					unsigned int w, unsigned int h)
{
	struct bbulk *bb = txt->data;
	struct video_rect r;

	switch (txt->orientation) {
	default:
	case OR_NORMAL:
		r.x1 = bb->off_x + x;
		r.y1 = bb->off_y + y;
		r.x2 = r.x1 + w;
		r.y2 = r.y1 + h;
		break;
	case OR_UPSIDE_DOWN:
		r.x2 = bb->max_x - x;
		r.y2 = bb->max_y - y;
		r.x1 = r.x2 - w;
		r.y1 = r.y2 - h;
		break;
	case OR_RIGHT:
		r.x2 = bb->max_x - y;
		r.x1 = r.x2 - h;
		r.y1 = bb->off_y + x;
		r.y2 = r.y1 + w;
		break;
	case OR_LEFT:
		r.x1 = bb->off_x + y;
		r.x2 = r.x1 + h;
		r.y2 = bb->max_y - x;
		r.y1 = r.y2 - w;
		break;
	}
	return r;
}

static bool default_bg(struct bbulk *bb, const struct tsm_frame_cell *cell)
{
	return cell->bg.r == bb->attr.br && cell->bg.g == bb->attr.bg && cell->bg.b == bb->attr.bb;
}

/* @a and @b draw the same pixels */
static bool same_place(const struct tsm_frame_placement *a, const struct tsm_frame_placement *b)
{
	return a->image_key == b->image_key && a->row == b->row && a->col == b->col &&
	       a->cols == b->cols && a->rows == b->rows && a->x == b->x && a->y == b->y &&
	       a->width == b->width && a->height == b->height && a->src_x == b->src_x &&
	       a->src_y == b->src_y && a->src_width == b->src_width &&
	       a->src_height == b->src_height && a->cell_width == b->cell_width &&
	       a->cell_height == b->cell_height && a->z == b->z && a->band == b->band &&
	       a->flags == b->flags;
}

/* by key; the overlay's keys are its own, after libtsm's */
static int cmp_place_key(const void *a, const void *b)
{
	const struct place *p = a, *q = b;

	if (p->overlay != q->overlay)
		return p->overlay - q->overlay;
	return (p->fp.key > q->fp.key) - (p->fp.key < q->fp.key);
}

static struct place *find_prev(struct bbulk *bb, const struct place *p)
{
	return bsearch(p, bb->prev, bb->n_prev, sizeof(*bb->prev), cmp_place_key);
}

static void force_cells(struct kmscon_text *txt, const struct place *p)
{
	struct bbulk *bb = txt->data;
	unsigned int x, y, c1 = min(p->c1, txt->cols), r1 = min(p->r1, txt->rows);

	for (y = p->r0; y < r1; y++)
		for (x = p->c0; x < c1; x++)
			bb->cell_flags[x + y * txt->cols].force = 1;
}

/* a placement of this frame shows @pic */
static bool picture_placed(struct bbulk *bb, const struct picture *pic)
{
	unsigned int i;

	for (i = 0; i < bb->n_places; i++)
		if (bb->places[i].overlay == pic->overlay &&
		    bb->places[i].fp.image_key == pic->image_key)
			return true;
	return false;
}

/* free the scaled pictures no placement of this frame shows */
static void pictures_free_unused(struct bbulk *bb)
{
	struct picture **pp, *pic;

	for (pp = &bb->pictures; *pp;) {
		pic = *pp;
		if (picture_placed(bb, pic)) {
			pp = &pic->next;
			continue;
		}
		*pp = pic->next;
		picture_free(bb, pic);
	}
}

/*
 * Take this frame's placements, @cols x @rows cells of it drawn, and the
 * overlay after them; force the cells of every placement added, changed or
 * removed since the last frame; and list each row's placements. While a
 * placement of any display is left out for the budget, the pictures this frame
 * no longer shows are freed first, so a placement left out here is tried again
 * on this frame when that, or anything else, changed the budget, or when a
 * placement here was added, changed or removed.
 */
static int places_update(struct kmscon_text *txt, const struct tsm_frame *frame, unsigned int cols,
			 unsigned int rows)
{
	struct bbulk *bb = txt->data;
	unsigned int nf = frame->placements ? frame->placement_count : 0, n, i, total, y;
	unsigned int *cover;
	size_t size = min(sizeof(struct tsm_frame_placement), frame->placement_size);
	struct place *p, *old, *tmp;
	bool retry, moved = false;

	n = nf + !!txt->overlay;
	if (n > bb->places_cap) {
		tmp = realloc(bb->prev, n * sizeof(*tmp));
		if (!tmp)
			return -ENOMEM;
		bb->prev = tmp;
		tmp = realloc(bb->places, n * sizeof(*tmp));
		if (!tmp)
			return -ENOMEM;
		bb->places = tmp;
		cover = realloc(bb->cover, n * sizeof(*cover));
		if (!cover)
			return -ENOMEM;
		bb->cover = cover;
		bb->places_cap = n;
	}

	bb->grid_w = cols * FONT_WIDTH(txt);
	bb->grid_h = rows * FONT_HEIGHT(txt);
	total = 0;
	for (i = 0; i < n; i++) {
		long long r0, r1;

		p = &bb->places[i];
		memset(p, 0, sizeof(*p));
		if (i < nf) {
			memcpy(&p->fp, tsm_frame_placement_at(frame, i), size);
		} else {
			/* the overlay, at the right end of the top row */
			p->overlay = true;
			p->fp = txt->overlay->placement;
			p->fp.col = cols > p->fp.cols ? cols - p->fp.cols : 0;
		}
		p->fp.flags &= ~TSM_FRAME_CHANGED;
		r0 = p->fp.row;
		r1 = r0 + p->fp.rows;
		p->c0 = min(p->fp.col, cols);
		p->c1 = p->fp.cols < cols - p->c0 ? p->c0 + p->fp.cols : cols;
		p->r0 = r0 < 0 ? 0 : r0 > rows ? rows : r0;
		p->r1 = r1 < 0 ? 0 : r1 > rows ? rows : r1;
		if (p->c0 == p->c1)
			p->r1 = p->r0;
		total += p->r1 - p->r0;
	}
	bb->n_places = n;

	if (starving)
		pictures_free_unused(bb);
	retry = bb->starved_gen != image_budget_gen();

	/* damage: prev's state marks the placements still here */
	for (i = 0; i < bb->n_prev; i++)
		bb->prev[i].state = 0;
	for (i = 0; i < n; i++) {
		p = &bb->places[i];
		old = find_prev(bb, p);
		if (old)
			old->state = 1;
		if (old && same_place(&old->fp, &p->fp)) {
			p->starved = old->starved;
			continue;
		}
		moved = true;
		if (old)
			force_cells(txt, old);
		force_cells(txt, p);
	}
	for (i = 0; i < bb->n_prev; i++) {
		if (!bb->prev[i].state) {
			moved = true;
			force_cells(txt, &bb->prev[i]);
		}
	}

	/* left out for the budget: again once it, or what this display draws, changed */
	for (i = 0; i < n && (retry || moved); i++) {
		if (bb->places[i].starved) {
			bb->places[i].starved = false;
			force_cells(txt, &bb->places[i]);
		}
	}

	if (n) {
		memcpy(bb->prev, bb->places, n * sizeof(*bb->places));
		qsort(bb->prev, n, sizeof(*bb->prev), cmp_place_key);
	}
	bb->n_prev = n;

	/* each row's placements, in draw order */
	if (total > bb->row_places_cap) {
		cover = realloc(bb->row_places, total * sizeof(*cover));
		if (!cover)
			return -ENOMEM;
		bb->row_places = cover;
		bb->row_places_cap = total;
	}
	memset(bb->row_first, 0, (rows + 1) * sizeof(*bb->row_first));
	for (i = 0; i < n; i++)
		for (y = bb->places[i].r0; y < bb->places[i].r1; y++)
			bb->row_first[y + 1]++;
	for (y = 0; y < rows; y++)
		bb->row_first[y + 1] += bb->row_first[y];
	for (i = 0; i < n; i++)
		for (y = bb->places[i].r0; y < bb->places[i].r1; y++)
			bb->row_places[bb->row_first[y]++] = i;
	/* each row's count was moved onto the next row's start; move it back */
	for (y = rows; y > 0; y--)
		bb->row_first[y] = bb->row_first[y - 1];
	bb->row_first[0] = 0;
	return 0;
}

/*
 * This display's share of the budget by size: limit x px / all, divided
 * before it is multiplied, so a set limit of up to IMAGE_BUDGET_LIMIT_MAX on
 * the largest displays does not wrap 64 bits
 */
static uint64_t picture_share(struct bbulk *bb)
{
	uint64_t px = (uint64_t)bb->sw * bb->sh, all = image_budget_displays_px();
	uint64_t limit = image_budget_limit();

	if (!all)
		return limit;
	return limit / all * px + limit % all * px / all;
}

/*
 * This display's scaled pictures between frames: twice its screen in ARGB, or
 * its share of the budget if that is less, so the displays' pictures together
 * stay within it and leave a frame room to scale more
 */
static size_t picture_limit(struct bbulk *bb)
{
	uint64_t screens = (uint64_t)bb->sw * bb->sh * 4 * 2, share = picture_share(bb);

	return screens < share ? screens : share;
}

/*
 * Free the least recently used pictures until @want more bytes fit in
 * @limit, keeping those this frame places when @keep_drawn is set: one placed
 * but not drawn yet may be drawn later in the frame, or its cells may not be
 * drawn at all, and a picture made from its room would then draw only in the
 * cells this frame draws, and stay left out of the rest of its box.
 */
/* @pic was drawn on this frame, or a placement of it shows it */
static bool picture_in_use(struct bbulk *bb, const struct picture *pic)
{
	return pic->last_use == bb->frame_count || picture_placed(bb, pic);
}

static void pictures_trim(struct bbulk *bb, size_t want, size_t limit, bool keep_drawn)
{
	struct picture **pp, **lru, *victim;

	while (bb->picture_bytes + want > limit) {
		lru = NULL;
		for (pp = &bb->pictures; *pp; pp = &(*pp)->next) {
			if (keep_drawn && picture_in_use(bb, *pp))
				continue;
			if (!lru || (*pp)->last_use < (*lru)->last_use)
				lru = pp;
		}
		if (!lru)
			return;
		victim = *lru;
		*lru = victim->next;
		picture_free(bb, victim);
	}
}

/*
 * The budget asks for @want bytes between frames, for one of libtsm's images
 * or a limit that shrank: the pictures the last frame did not draw go, least
 * recently used first (ADR-103's first step of eviction).
 */
static void bbulk_trim(struct image_budget_cache *cache, size_t want)
{
	struct bbulk *bb = (struct bbulk *)cache;

	pictures_trim(bb, 0, want < bb->picture_bytes ? bb->picture_bytes - want : 0, true);
}

/* what bbulk_trim frees at most */
static size_t bbulk_trimmable(struct image_budget_cache *cache)
{
	struct bbulk *bb = (struct bbulk *)cache;
	struct picture *p;
	size_t bytes = 0;

	for (p = bb->pictures; p; p = p->next)
		if (!picture_in_use(bb, p))
			bytes += p->bytes;
	return bytes;
}

/* a placement left out here may fit now: the budget has grown since */
static bool bbulk_starved(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	return bb && bb->starving && bb->starved_gen != image_budget_gen();
}

/*
 * Between frames: the scaled pictures fit this display's limit. Pictures freed
 * to fit it do not try a placement left out here again, or two pictures over
 * the same cells would take turns being scaled on every frame.
 */
static void pictures_settle(struct bbulk *bb)
{
	bool starved = false;
	unsigned int i;

	for (i = 0; i < bb->n_prev; i++)
		starved |= bb->prev[i].starved;
	starving_set(bb, starved);
	pictures_trim(bb, 0, picture_limit(bb), false);
	bb->starved_gen = image_budget_gen();
}

/*
 * @con's image @key is shown by a placement of the frame that renderer @data
 * draws; without the frame's screen, every image counts as drawn
 */
static bool frame_draws(struct tsm_screen *con, uint64_t key, void *data)
{
	struct kmscon_text *txt = data;
	struct bbulk *bb = txt->data;
	unsigned int i;

	if (con != txt->screen)
		return !txt->screen;
	for (i = 0; i < bb->n_places; i++)
		if (!bb->places[i].overlay && bb->places[i].fp.image_key == key)
			return true;
	return false;
}

/*
 * Where @p's picture draws and its pixels, scaled and rotated for this display
 * when its size or the orientation asks for it: 1, -1 when it draws nothing,
 * -ENOSPC when it does not fit the budget, or -ENOMEM.
 */
static int place_resolve(struct kmscon_text *txt, struct place *p)
{
	struct bbulk *bb = txt->data;
	const struct tsm_frame_placement *fp = &p->fp;
	long long fw = FONT_WIDTH(txt), fh = FONT_HEIGHT(txt), gx, gy, px0, px1, py0, py1;
	long long x0, x1, y0, y1;
	struct tsm_frame_image img;
	const uint32_t *src;
	struct picture *pic;
	unsigned int dw, dh, u0, v0, u1, v1, w, h;
	size_t stride, bytes;
	uint32_t *tmp;
	int ret;

	if (!fp->cell_width || !fp->cell_height || !fp->width || !fp->height || !fp->src_width ||
	    !fp->src_height)
		return -1;

	/* the picture in grid pixels, scaled from the cells it was made in */
	gx = (long long)fp->col * fw;
	gy = (long long)fp->row * fh;
	px0 = gx + (long long)fp->x * fw / fp->cell_width;
	px1 = gx + ((long long)fp->x + fp->width) * fw / fp->cell_width;
	py0 = gy + (long long)fp->y * fh / fp->cell_height;
	py1 = gy + ((long long)fp->y + fp->height) * fh / fp->cell_height;

	/* its part inside its box and the grid */
	x0 = px0 > gx ? px0 : gx;
	y0 = py0 > gy ? py0 : gy;
	x0 = x0 > 0 ? x0 : 0;
	y0 = y0 > 0 ? y0 : 0;
	x1 = px1 < gx + (long long)fp->cols * fw ? px1 : gx + (long long)fp->cols * fw;
	y1 = py1 < gy + (long long)fp->rows * fh ? py1 : gy + (long long)fp->rows * fh;
	x1 = x1 < bb->grid_w ? x1 : bb->grid_w;
	y1 = y1 < bb->grid_h ? y1 : bb->grid_h;
	if (x0 >= x1 || y0 >= y1)
		return -1;

	if (kmscon_text_get_image(txt, p->overlay, fp->image_key, &img))
		return -1;
	stride = img.stride / 4;
	if (img.stride % 4 || stride < img.width || fp->src_x >= img.width ||
	    fp->src_y >= img.height || fp->src_width > img.width - fp->src_x ||
	    fp->src_height > img.height - fp->src_y)
		return -1;
	src = &img.pixels[(size_t)fp->src_y * stride + fp->src_x];

	dw = px1 - px0;
	dh = py1 - py0;
	u0 = x0 - px0;
	v0 = y0 - py0;
	u1 = x1 - px0;
	v1 = y1 - py0;
	w = u1 - u0;
	h = v1 - v0;
	p->x0 = x0;
	p->x1 = x1;
	p->y0 = y0;
	p->y1 = y1;
	p->screen = grid_to_screen(txt, x0, y0, w, h);

	/* drawn as it is */
	if (txt->orientation == OR_NORMAL && dw == fp->src_width && dh == fp->src_height) {
		p->pix = &src[(size_t)v0 * stride + u0];
		p->stride = stride;
		return 1;
	}

	for (pic = bb->pictures; pic; pic = pic->next) {
		if (pic->overlay == p->overlay && pic->image_key == fp->image_key &&
		    pic->src_x == fp->src_x && pic->src_y == fp->src_y &&
		    pic->src_w == fp->src_width && pic->src_h == fp->src_height && pic->dw == dw &&
		    pic->dh == dh && pic->u0 == u0 && pic->v0 == v0 && pic->u1 == u1 &&
		    pic->v1 == v1)
			break;
	}
	if (!pic) {
		bytes = (size_t)w * h * 4;
		pictures_trim(bb, bytes, picture_limit(bb), true);
		pictures_trim(bb, bytes + image_budget_used() - bb->picture_bytes,
			      image_budget_limit(), true);
		/*
		 * a frame may pass this display's limit, never the budget: past
		 * it the cells are drawn without the picture, and drawn again
		 * once the budget has changed
		 */
		if (image_budget_used() + bytes > image_budget_limit() &&
		    image_budget_picture_room(bytes, frame_draws, txt))
			return -ENOSPC;
		pic = malloc(sizeof(*pic) + bytes);
		tmp = txt->orientation == OR_NORMAL ? NULL : malloc(bytes);
		if (!pic || (txt->orientation != OR_NORMAL && !tmp)) {
			free(pic);
			return -ENOMEM;
		}
		ret = picture_scale(src, stride, fp->src_width, fp->src_height, dw, dh, u0, v0, u1,
				    v1, tmp ? tmp : pic->pixels);
		if (!ret && tmp)
			picture_rotate(tmp, w, h, pic->pixels, txt->orientation);
		free(tmp);
		if (ret) {
			free(pic);
			return ret == -ENOMEM ? -ENOMEM : -1;
		}
		pic->overlay = p->overlay;
		pic->image_key = fp->image_key;
		pic->src_x = fp->src_x;
		pic->src_y = fp->src_y;
		pic->src_w = fp->src_width;
		pic->src_h = fp->src_height;
		pic->dw = dw;
		pic->dh = dh;
		pic->u0 = u0;
		pic->v0 = v0;
		pic->u1 = u1;
		pic->v1 = v1;
		pic->bytes = bytes;
		pic->next = bb->pictures;
		bb->pictures = pic;
		bb->picture_bytes += bytes;
		image_budget_add_picture(bytes);
	}
	pic->last_use = bb->frame_count;
	p->pix = pic->pixels;
	p->stride = p->screen.x2 - p->screen.x1;
	return 1;
}

/* @p's picture covers the cells [@posx, @posx + @n) of row @posy */
static bool place_covers(struct kmscon_text *txt, const struct place *p, unsigned int posx,
			 unsigned int posy, unsigned int n)
{
	unsigned int fw = FONT_WIDTH(txt), fh = FONT_HEIGHT(txt);

	return p->x0 <= posx * fw && p->x1 >= (posx + n) * fw && p->y0 <= posy * fh &&
	       p->y1 >= (posy + 1) * fh;
}

/*
 * The placements whose pictures draw on the cells [@posx, @posx + @n) of row
 * @posy, in draw order, into bb->cover; returns how many. @retry is set when
 * one could not be had for lack of memory.
 */
static unsigned int cell_cover(struct kmscon_text *txt, unsigned int posx, unsigned int posy,
			       unsigned int n, bool *retry)
{
	struct bbulk *bb = txt->data;
	unsigned int fw = FONT_WIDTH(txt), fh = FONT_HEIGHT(txt), k, count = 0;
	struct place *p;

	if (!bb->n_places)
		return 0;
	for (k = bb->row_first[posy]; k < bb->row_first[posy + 1]; k++) {
		p = &bb->places[bb->row_places[k]];
		if (p->c0 >= posx + n || p->c1 <= posx)
			continue;
		if (!p->state) {
			p->state = place_resolve(txt, p);
			if (p->state == -ENOSPC) {
				struct place *q = find_prev(bb, p);

				p->starved = true;
				if (q)
					q->starved = true;
			}
		}
		if (p->state == -ENOMEM)
			*retry = true;
		if (p->state != 1 || p->x1 <= posx * fw || p->x0 >= (posx + n) * fw ||
		    p->y1 <= posy * fh || p->y0 >= (posy + 1) * fh)
			continue;
		bb->cover[count++] = bb->row_places[k];
	}
	return count;
}

/* @p's picture over the scratch @dst, which holds the screen's rectangle @req */
static void compose_place(struct video_buffer *dst, const struct video_blend_req *req,
			  const struct place *p)
{
	int x1 = max((int)req->x, p->screen.x1), y1 = max((int)req->y, p->screen.y1);
	int x2 = min((int)(req->x + req->w), p->screen.x2);
	int y2 = min((int)(req->y + req->h), p->screen.y2);
	bool opaque = p->fp.flags & TSM_FRAME_OPAQUE;
	uint32_t *d;
	const uint32_t *s;
	int x, y;

	for (y = y1; y < y2; y++) {
		d = &((uint32_t *)dst->data)[(y - req->y) * dst->width + (x1 - req->x)];
		s = &p->pix[(size_t)(y - p->screen.y1) * p->stride + (x1 - p->screen.x1)];
		if (opaque) {
			memcpy(d, s, (x2 - x1) * sizeof(*d));
			continue;
		}
		for (x = 0; x < x2 - x1; x++) {
			uint32_t a = s[x] >> 24, na = 255 - a, q = d[x];

			if (!a)
				continue;
			d[x] = s[x] + ((over_div255((q >> 24) * na) << 24) |
				       (over_div255(((q >> 16) & 0xff) * na) << 16) |
				       (over_div255(((q >> 8) & 0xff) * na) << 8) |
				       over_div255((q & 0xff) * na));
		}
	}
}

/*
 * The placement whose pixels a cell takes as they are, or -1: the last
 * placement over the cell is opaque and covers it wholly, and nothing draws
 * over it: in the top band it hides the text; below it, the cell has no glyph
 * and no decoration, and in the lowest band its background is the default.
 */
static int cell_fast(struct kmscon_text *txt, const struct tsm_frame_cell *cell, uint64_t key,
		     unsigned int posx, unsigned int posy)
{
	struct bbulk *bb = txt->data;
	const struct place *p;
	unsigned int n;
	bool retry = false;

	n = cell_cover(txt, posx, posy, 1, &retry);
	if (!n || retry)
		return -1;
	p = &bb->places[bb->cover[n - 1]];
	if (!(p->fp.flags & TSM_FRAME_OPAQUE) || !place_covers(txt, p, posx, posy, 1))
		return -1;
	if (p->fp.band != TSM_FRAME_BAND_ABOVE_TEXT &&
	    ((key && key != ' ') || cell->underline != TSM_UNDERLINE_NONE ||
	     (cell->flags & TSM_FRAME_STRIKE)))
		return -1;
	if (p->fp.band == TSM_FRAME_BAND_BELOW_BG && !default_bg(bb, cell))
		return -1;
	return bb->cover[n - 1];
}

/* send the pending run of cells that take one opaque picture as they are */
static void run_flush(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	const struct place *p;
	struct video_blend_req req;
	struct video_rect r;
	unsigned int w, h, y;

	if (!bb->run.n)
		return;
	p = &bb->places[bb->run.place];
	r = grid_to_screen(txt, bb->run.posx * FONT_WIDTH(txt), bb->run.posy * FONT_HEIGHT(txt),
			   bb->run.n * FONT_WIDTH(txt), FONT_HEIGHT(txt));
	w = r.x2 - r.x1;
	h = r.y2 - r.y1;
	for (y = 0; y < h; y++)
		memcpy(&((uint32_t *)bb->strip->data)[y * w],
		       &p->pix[(size_t)(r.y1 - p->screen.y1 + y) * p->stride +
			       (r.x1 - p->screen.x1)],
		       w * 4);
	bb->strip->width = w;
	bb->strip->height = h;

	memset(&req, 0, sizeof(req));
	req.buf = bb->strip;
	req.x = r.x1;
	req.y = r.y1;
	req.w = w;
	req.h = h;
	display_blend(txt->disp, &req);
	bb->requests++;
	bb->run.n = 0;
}

/* cell @posx, @posy takes placement @place's pixels: extend the run, or start one */
static void run_add(struct kmscon_text *txt, unsigned int place, unsigned int posx,
		    unsigned int posy)
{
	struct bbulk *bb = txt->data;

	if (bb->run.n && bb->run.place == place && bb->run.posy == posy &&
	    bb->run.posx + bb->run.n == posx) {
		bb->run.n++;
		return;
	}
	run_flush(txt);
	bb->run.place = place;
	bb->run.posx = posx;
	bb->run.posy = posy;
	bb->run.n = 1;
}

/*
 * Paint @cell's decorations and glyph @glyph (none when @key is 0) over the
 * scratch buffer, @ncells cells: underline, glyph, strike-through.
 */
static void compose_text(struct kmscon_text *txt, const struct tsm_frame_cell *cell,
			 const struct kmscon_glyph *glyph, uint64_t key, unsigned int ncells,
			 bool ul_cursor)
{
	struct bbulk *bb = txt->data;

	if (cell->underline > TSM_UNDERLINE_NONE && cell->underline < KMSCON_DECO_STRIKE)
		compose_sprite(txt, cell->underline, ncells,
			       ul_cursor ? cell->fg : kmscon_text_ul_color(txt, cell));
	if (key)
		compose(bb->scratch, &glyph->buf, 0, 0, cell->fg);
	if (cell->flags & TSM_FRAME_STRIKE)
		compose_sprite(txt, KMSCON_DECO_STRIKE, ncells,
			       kmscon_text_strike_color(txt, cell));
}

/*
 * Compose the cells under @req, with @n placements over them in bb->cover,
 * from the highest layer that hides everything below it upwards (ADR-103):
 * the lowest band, the cell's background unless it is the default, the middle
 * band, the text, the top band.
 */
static void compose_covered(struct kmscon_text *txt, struct video_blend_req *req,
			    const struct tsm_frame_cell *cell, const struct kmscon_glyph *glyph,
			    uint64_t key, unsigned int posx, unsigned int posy, unsigned int ncells,
			    bool ul_cursor, unsigned int n)
{
	struct bbulk *bb = txt->data;
	struct video_buffer *sc = bb->scratch;
	const struct place *p;
	unsigned int i = 0, k, x;
	bool text = false;

	sc->width = req->w;
	sc->height = req->h;
	if (default_bg(bb, cell)) {
		memset(sc->data, 0, sc->width * sc->height * 4);
	} else {
		uint32_t bg = 0xff000000 | (cell->bg.r << 16) | (cell->bg.g << 8) | cell->bg.b;

		for (x = 0; x < sc->width * sc->height; x++)
			((uint32_t *)sc->data)[x] = bg;
		while (i < n && bb->places[bb->cover[i]].fp.band == TSM_FRAME_BAND_BELOW_BG)
			i++;
	}
	/* an opaque picture over every pixel hides what is under it, the
	 * text too when it is in the top band */
	for (k = n; k-- > i;) {
		p = &bb->places[bb->cover[k]];
		if ((p->fp.flags & TSM_FRAME_OPAQUE) && place_covers(txt, p, posx, posy, ncells)) {
			i = k;
			text = p->fp.band == TSM_FRAME_BAND_ABOVE_TEXT;
			break;
		}
	}
	for (; i < n; i++) {
		p = &bb->places[bb->cover[i]];
		if (!text && p->fp.band == TSM_FRAME_BAND_ABOVE_TEXT) {
			compose_text(txt, cell, glyph, key, ncells, ul_cursor);
			text = true;
		}
		compose_place(sc, req, p);
	}
	if (!text)
		compose_text(txt, cell, glyph, key, ncells, ul_cursor);

	req->buf = sc;
	req->br = bb->attr.br;
	req->bg = bb->attr.bg;
	req->bb = bb->attr.bb;
}

/*
 * Draw @cell at @posx, @posy over @ncells cells (2 for a wide cell with room
 * for its second half), with glyph key @key (0: none) drawn @width cells wide.
 * When the glyph or a picture over the cell cannot be had, the cell is drawn
 * without it and -ENOMEM returned, for the caller to draw it again.
 * @ul_cursor: @cell's underline is an underline cursor, which is no
 * decoration and draws in the foreground.
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
	bool retry = false;
	unsigned int n;
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

	n = cell_cover(txt, posx, posy, ncells, &retry);
	if (retry)
		ret = -ENOMEM;
	if (n) {
		compose_covered(txt, &req, cell, glyph, key, posx, posy, ncells, ul_cursor, n);
	} else if (cell->underline == TSM_UNDERLINE_NONE && !strike) {
		req.buf = &glyph->buf;
	} else {
		struct video_buffer *sc = bb->scratch;

		sc->width = min(req.w, glyph->buf.width);
		sc->height = min(req.h, glyph->buf.height);
		memset(sc->data, 0, sc->width * sc->height * 4);
		compose_text(txt, cell, glyph, key, ncells, ul_cursor);
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
	int place;

	cols = min(txt->cols, frame->cols);
	rows = min(txt->rows, frame->rows);

	bb->frame_count++;
	bb->run.n = 0;
	if (places_update(txt, frame, cols, rows)) {
		bb->n_places = 0;
		return -ENOMEM;
	}

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

			/* a run of cells under one opaque picture goes as one request */
			if (!is_cursor && ncells == 1) {
				place = cell_fast(txt, cell, key, posx, posy);
				if (place >= 0) {
					run_add(txt, place, posx, posy);
					continue;
				}
			}
			run_flush(txt);

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
		run_flush(txt);
	}
	pictures_settle(bb);
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
	if (memcmp(&bb->attr, attr, sizeof(*attr)) ||
	    memcmp(&bb->deco, &txt->deco, sizeof(bb->deco)) || display_need_redraw(txt->disp))
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
	.release = bbulk_release,
	.starved = bbulk_starved,
	.draws_images = true,
};
