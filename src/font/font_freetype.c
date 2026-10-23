/*
 * kmscon - Freetype font backend
 *
 * Copyright (c) 2026 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
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

#include <fontconfig/fontconfig.h>
#include <freetype2/freetype/freetype.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H
#include FT_TRUETYPE_TABLES_H
#include <hb-ft.h>
#include <hb.h>
#include <libtsm.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "font_colour.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "font_freetype"

struct ft_font {
	FT_Face face;
	char *name;
	/* FontSet and Pattern are used for fallback glyphs */
	FcFontSet *fc;
	FcPattern *pattern;
	FT_Face fallback;
	int fallback_index;
};

struct ft_data {
	FT_Library ft;
	struct ft_font regular;
	struct ft_font bold;
	/* faces with colour, for emoji presentation (ADR-102) */
	struct kmscon_colour_faces colour;
};

static void free_ft_font(struct ft_font *ftfont)
{
	if (ftfont->face)
		FT_Done_Face(ftfont->face);
	ftfont->face = NULL;
	free(ftfont->name);
	ftfont->name = NULL;
	if (ftfont->fc)
		FcFontSetDestroy(ftfont->fc);
	ftfont->fc = NULL;
	if (ftfont->pattern)
		FcPatternDestroy(ftfont->pattern);
	ftfont->pattern = NULL;
	if (ftfont->fallback)
		FT_Done_Face(ftfont->fallback);
	ftfont->fallback = NULL;
	ftfont->fallback_index = -1;
}

static int prepare_face(FT_Library ft, struct ft_font *ftfont)
{
	FcChar8 *path;
	FT_Error err;
	int index = 0;
	FcPattern *pattern;
	int ret = -EINVAL;
	FcChar8 *full_name = NULL;

	pattern = FcFontRenderPrepare(NULL, ftfont->pattern, ftfont->fc->fonts[0]);
	if (!pattern)
		return -EINVAL;

	if (FcPatternGetString(pattern, FC_FULLNAME, 0, &full_name) == FcResultMatch)
		ftfont->name = strdup((char *)full_name);
	else
		ftfont->name = strdup("Unknown");

	if (FcPatternGetString(pattern, FC_FILE, 0, &path) != FcResultMatch)
		goto err_pattern;

	if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) != FcResultMatch)
		log_warn("%s: failed to get face index", path);

	log_debug("Loading font %s", (char *)path);

	err = FT_New_Face(ft, (char *)path, index, &ftfont->face);
	ret = err ? -EINVAL : 0;

err_pattern:
	FcPatternDestroy(pattern);
	return ret;
}

static int load_and_render(FT_Face face, FT_UInt index)
{
	if (FT_Load_Glyph(face, index, FT_LOAD_TARGET_LIGHT)) {
		log_err("Failed to load glyph index %d", index);
		return -EINVAL;
	}
	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
		log_err("Failed to render glyph index %d", index);
		return -EINVAL;
	}
	return 0;
}

static unsigned int font_get_width(FT_Face face)
{
	FT_UInt index = FT_Get_Char_Index(face, 'W');

	if (load_and_render(face, index))
		return 0;

	return max(face->glyph->advance.x >> 6, face->glyph->bitmap.width);
}

static int bitmap_font_select_size(FT_Face face, int height)
{
	int i, diff, best = 0;
	int min = INT_MAX;

	for (i = 0; i < face->num_fixed_sizes; i++) {
		diff = abs(face->available_sizes[i].height - height);
		if (diff < min) {
			min = diff;
			best = i;
		}
	}
	log_debug("Select bitmap, asked height %d found height %d within %d choices\n", height,
		  face->available_sizes[best].height, face->num_fixed_sizes);
	return best;
}

static FT_Error request_size(FT_Face face, unsigned int height)
{
	// Request a size of half a pixel less, as freetype draws some characters with 1px more.
	FT_Size_RequestRec request = {FT_SIZE_REQUEST_TYPE_CELL, 0, (height << 6) - 32, 0, 0};
	FT_Error ret;

	ret = FT_Request_Size(face, &request);
	if (ret)
		log_warn("Freetype failed to set size to %d, error %d", height, ret);
	return ret;
}

static void compute_font_size(struct ft_font *ftfont, unsigned int query_height,
			      unsigned int *width, unsigned int *height)
{
	FT_Face face = ftfont->face;

	/* Special case for bitmap fonts, which can't be scaled */
	if (face->num_fixed_sizes) {
		int bitmap_index = bitmap_font_select_size(face, query_height);

		FT_Select_Size(face, bitmap_index);
		*width = font_get_width(face);
		*height = face->available_sizes[bitmap_index].height;
		log_debug("bitmap font size %dx%d", *width, *height);
	} else {
		if (request_size(face, query_height)) {
			*width = 0;
			*height = 0;
			return;
		}
		*width = font_get_width(face);
		*height = (face->size->metrics.height >> 6);
	}
}

static int prepare_font(FT_Library ft, struct ft_font *ftfont, const char *name,
			unsigned int height, bool bold, struct kmscon_font *font)
{
	FcResult result;

	ftfont->pattern = FcNameParse((const FcChar8 *)name);
	if (!ftfont->pattern)
		return -EINVAL;

	FcPatternAddInteger(ftfont->pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_NORMAL);
	FcPatternAddDouble(ftfont->pattern, FC_PIXEL_SIZE, (double)height);

	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_CHARCELL);
	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_MONO);
	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_DUAL);

	if (!FcConfigSubstitute(NULL, ftfont->pattern, FcMatchPattern)) {
		log_err("%s: failed to do config substitution", name);
		goto err;
	}

	FcDefaultSubstitute(ftfont->pattern);

	ftfont->fc = FcFontSort(NULL, ftfont->pattern, FcTrue, NULL, &result);
	if (result != FcResultMatch) {
		log_err("%s: failed to match font", name);
		goto err;
	}

	if (prepare_face(ft, ftfont))
		goto err;

	ftfont->fallback_index = -1;
	ftfont->fallback = NULL;
	return 0;
err:
	free_ft_font(ftfont);
	return -EINVAL;
}

static FcBool _fc_monospace_font_filter_func(const FcPattern *pat, void *user_data)
{
	int spacing;

	if (FcPatternGetInteger(pat, FC_SPACING, 0, &spacing) == FcResultMatch) {
		if (spacing == FC_CHARCELL || spacing == FC_MONO || spacing == FC_DUAL)
			return FcTrue;
	}
	/* non-monospace font doesn't have FC_SPACING property */
	return FcFalse;
}

static int kmscon_font_freetype_init(struct kmscon_font *out, const char *name, unsigned int height)
{
	struct ft_data *ftf;
	FT_Error err;
	unsigned int bold_width, bold_height;

	ftf = calloc(1, sizeof(*ftf));
	if (!ftf)
		return -ENOMEM;

	err = FT_Init_FreeType(&ftf->ft);
	if (err != 0) {
		log_err("Failed to initialize FreeType\n");
		goto err_free;
	}
	FcConfigSetFontSetFilter(NULL, _fc_monospace_font_filter_func, NULL, NULL);
	if (prepare_font(ftf->ft, &ftf->regular, name, height, false, out))
		goto err_done;

	compute_font_size(&ftf->regular, height, &out->width, &out->height);
	if (out->width == 0 || out->height == 0)
		goto err_free_reg;

	if (prepare_font(ftf->ft, &ftf->bold, name, height, true, out))
		goto err_free_reg;

	compute_font_size(&ftf->bold, height, &bold_width, &bold_height);
	kmscon_colour_faces_init(&ftf->colour, ftf->ft);

	if (out->width != bold_width || out->height != bold_height)
		log_warn("Bold and regular font don't have the same dimension");

	out->width = max(out->width, bold_width);
	out->height = max(out->height, bold_height);

	out->increase_step = 1;
	out->data = ftf;

	log_notice("Using [%s] / [%s] size %d -> cell size %dx%d\n", ftf->regular.name,
		   ftf->bold.name, height, out->width, out->height);
	return 0;

err_free_reg:
	free_ft_font(&ftf->regular);
err_done:
	FT_Done_FreeType(ftf->ft);
err_free:
	free(ftf);
	return -EINVAL;
}

static void kmscon_font_freetype_destroy(struct kmscon_font *font)
{
	struct ft_data *ftf = font->data;

	log_debug("unloading freetype font");
	free_ft_font(&ftf->regular);
	free_ft_font(&ftf->bold);
	kmscon_colour_faces_free(&ftf->colour);
	FT_Done_FreeType(ftf->ft);
	free(ftf);
	font->data = NULL;
}

static void copy_mono(struct video_buffer *buf, FT_GlyphSlot glyph, unsigned int ascender, int dx)
{
	FT_Bitmap *map = &glyph->bitmap;
	int top = ascender - glyph->bitmap_top;
	int left = glyph->bitmap_left + dx;
	uint8_t *src = map->buffer;
	uint8_t *dst = buf->data;
	int i, j, w, h;

	if (top + map->rows > buf->height)
		top = buf->height - map->rows;
	if (top < 0)
		top = 0;
	if (left < 0)
		left = 0;

	w = min((int)buf->width - left, (int)map->width);
	h = min((int)buf->height - top, (int)map->rows);
	if (w <= 0 || h <= 0)
		return;

	for (i = 0; i < h; i++) {
		for (j = 0; j < w; j++)
			dst[j + top * buf->width + left] =
				!!(src[j / 8] & (1 << (7 - (j % 8)))) * 0xff;

		dst += buf->width;
		src += map->pitch;
	}
}

/*
 * Copy a coverage map whose top-left pixel sits at @left, @top in the cell:
 * clipped at the top and bottom, and centred when it is wider than the cell
 * space to its right.
 */
static void copy_map(struct video_buffer *buf, const uint8_t *map, int map_w, int map_h,
		     int pitch, int left, int top)
{
	int width, height;
	int left_src = 0;
	int top_src = 0;
	int i;
	uint8_t *dst;

	if (!map_w || !map_h)
		return;

	if (top < 0) {
		top_src = -top;
		top = 0;
	}
	height = min((int)(buf->height - top), map_h - top_src);

	if (left + map_w > (int)buf->width)
		left -= ((left + map_w) - (int)buf->width) / 2;

	if (left < 0) {
		left_src = -left;
		left = 0;
	}
	width = min((int)(buf->width - left), map_w - left_src);

	if (width <= 0 || height <= 0)
		return;

	dst = buf->data + left + top * buf->width;
	for (i = 0; i < height; i++) {
		memcpy(dst, &map[(i + top_src) * pitch + left_src], width);
		dst += buf->width;
	}
}

/*
 * @slot's bitmap as one byte of coverage per pixel, 0 to 255, or as one bit per
 * pixel for FT_PIXEL_MODE_MONO, which the callers read bit by bit. Any other
 * mode (a strike's 2- or 4-bit grey, LCD, BGRA) is converted into @tmp. @tmp is
 * initialised here; the caller releases it with FT_Bitmap_Done. NULL when the
 * conversion fails.
 */
static const FT_Bitmap *coverage(FT_GlyphSlot slot, FT_Bitmap *tmp)
{
	const FT_Bitmap *map = &slot->bitmap;
	unsigned int i, n, top;

	FT_Bitmap_Init(tmp);
	if (map->pixel_mode == FT_PIXEL_MODE_GRAY || map->pixel_mode == FT_PIXEL_MODE_MONO)
		return map;
	if (FT_Bitmap_Convert(slot->library, map, tmp, 1))
		return NULL;

	/* the converted levels run from 0 to num_grays - 1 */
	top = tmp->num_grays - 1;
	n = tmp->rows * (unsigned int)tmp->pitch;
	if (top && top < 255)
		for (i = 0; i < n; i++)
			tmp->buffer[i] = tmp->buffer[i] * 255 / top;
	return tmp;
}

static struct kmscon_glyph *new_glyph(struct kmscon_font *font, unsigned int cwidth)
{
	struct kmscon_glyph *glyph;

	glyph = calloc(1, sizeof(*glyph) + cwidth * font->width * font->height);
	if (!glyph) {
		log_error("cannot allocate memory for new glyph");
		return NULL;
	}
	glyph->double_width = cwidth == 2;
	glyph->buf.width = font->width * cwidth;
	glyph->buf.height = font->height;
	glyph->buf.format = VIDEO_FORMAT_A8;
	return glyph;
}

/*
 * How far to move a glyph that advances @adv (26.6) right so that it is
 * centred in its box: a narrow glyph in a two-cell box, such as a text-style
 * emoji widened by VS16, sits in the middle of the two cells. A glyph as wide
 * as its box stays where the font puts it. A negative advance, which a shaped
 * cluster can have, counts as none, so the move stays within the box.
 */
static int centre_dx(struct kmscon_font *font, const struct kmscon_glyph *glyph, long adv)
{
	long px = (adv + 32) >> 6;

	if (!glyph->double_width || px > font->width)
		return 0;
	if (px < 0)
		px = 0;
	return (glyph->buf.width - px) / 2;
}

/* A single codepoint: loaded by glyph index, without shaping. */
static struct kmscon_glyph *render_glyph(struct kmscon_font *font, FT_Face face, FT_UInt index,
					 unsigned int cwidth)
{
	struct kmscon_glyph *glyph;
	const FT_Bitmap *map;
	FT_Bitmap tmp;
	int dx;

	if (load_and_render(face, index))
		return NULL;

	map = coverage(face->glyph, &tmp);
	if (!map)
		return NULL;

	glyph = new_glyph(font, cwidth);
	if (!glyph)
		goto out;

	dx = centre_dx(font, glyph, face->glyph->advance.x);
	if (map->pixel_mode == FT_PIXEL_MODE_MONO)
		copy_mono(&glyph->buf, face->glyph, face->size->metrics.ascender >> 6, dx);
	else
		copy_map(&glyph->buf, map->buffer, map->width, map->rows, map->pitch,
			 face->glyph->bitmap_left + dx,
			 (face->size->metrics.ascender >> 6) - face->glyph->bitmap_top);

out:
	FT_Bitmap_Done(face->glyph->library, &tmp);
	return glyph;
}

/* @ccc is a combining class drawn above the base, or below it */
static bool ccc_above(hb_unicode_combining_class_t ccc)
{
	switch (ccc) {
	case HB_UNICODE_COMBINING_CLASS_ATTACHED_ABOVE:
	case HB_UNICODE_COMBINING_CLASS_ATTACHED_ABOVE_RIGHT:
	case HB_UNICODE_COMBINING_CLASS_ABOVE_LEFT:
	case HB_UNICODE_COMBINING_CLASS_ABOVE:
	case HB_UNICODE_COMBINING_CLASS_ABOVE_RIGHT:
	case HB_UNICODE_COMBINING_CLASS_DOUBLE_ABOVE:
		return true;
	default:
		return false;
	}
}

static bool ccc_below(hb_unicode_combining_class_t ccc)
{
	switch (ccc) {
	case HB_UNICODE_COMBINING_CLASS_ATTACHED_BELOW:
	case HB_UNICODE_COMBINING_CLASS_BELOW_LEFT:
	case HB_UNICODE_COMBINING_CLASS_BELOW:
	case HB_UNICODE_COMBINING_CLASS_BELOW_RIGHT:
	case HB_UNICODE_COMBINING_CLASS_DOUBLE_BELOW:
		return true;
	default:
		return false;
	}
}

/*
 * A cluster of several codepoints (ADR-102): HarfBuzz shapes it on @face, the
 * face that covers its first codepoint, and FreeType draws each shaped glyph
 * into a canvas one glyph box wider on each side. The cluster's pen origin goes
 * where a single glyph's would, and ink past the cell is cut.
 *
 * A codepoint @face lacks shapes to glyph 0, the missing-glyph box; it is left
 * out, with its advance, so the box does not cover the rest of the cluster.
 *
 * A mark above or below that the face does not place (no advance, no offset;
 * HarfBuzz composes a + U+0303 to atilde, and a face may have no anchor for a
 * grave on atilde) would sit at the pen, past the base. It goes centred over
 * the base's ink, above or below all the ink drawn since the base, as in
 * HarfBuzz's fallback mark positioning (hb-ot-shape-fallback.cc,
 * position_around_base; MIT). Left and right classes are centred too. A mark
 * that does not fit is pushed back inside the cell.
 */
static struct kmscon_glyph *render_cluster(struct kmscon_font *font, FT_Face face,
					   const uint32_t *cps, size_t len, unsigned int cwidth)
{
	struct kmscon_glyph *glyph = NULL;
	hb_font_t *hb;
	hb_buffer_t *hbuf;
	hb_glyph_info_t *info;
	hb_glyph_position_t *pos;
	hb_unicode_funcs_t *ucd;
	hb_unicode_combining_class_t ccc;
	unsigned int n, i, gw, cw, ch;
	int baseline = face->size->metrics.ascender >> 6;
	int pen_x = 0, pen_y = 0, ox, x, y, gx, gy;
	int base_x0 = 0, base_x1 = -1, ink_top = 0, ink_bottom = 0;
	uint8_t *canvas;

	glyph = new_glyph(font, cwidth);
	if (!glyph)
		return NULL;
	gw = glyph->buf.width;
	cw = 3 * gw;
	ch = glyph->buf.height;
	ox = gw;
	canvas = calloc(cw, ch);
	if (!canvas)
		goto err_glyph;

	hb = hb_ft_font_create_referenced(face);
	hb_ft_font_set_load_flags(hb, FT_LOAD_TARGET_LIGHT);
	hbuf = hb_buffer_create();
	/* each glyph's cluster is the index of its codepoint in @cps */
	hb_buffer_set_cluster_level(hbuf, HB_BUFFER_CLUSTER_LEVEL_CHARACTERS);
	hb_buffer_add_utf32(hbuf, cps, len, 0, len);
	hb_buffer_guess_segment_properties(hbuf);
	hb_shape(hb, hbuf, NULL, 0);
	info = hb_buffer_get_glyph_infos(hbuf, &n);
	pos = hb_buffer_get_glyph_positions(hbuf, &n);
	ucd = hb_buffer_get_unicode_funcs(hbuf);

	for (i = 0; i < n; i++) {
		FT_GlyphSlot slot = face->glyph;
		const FT_Bitmap *map;
		FT_Bitmap tmp;

		FT_Bitmap_Init(&tmp);
		if (!info[i].codepoint)
			continue;
		if (load_and_render(face, info[i].codepoint))
			goto next;
		map = coverage(slot, &tmp);
		if (!map)
			goto next;
		gx = ox + ((pen_x + pos[i].x_offset + 32) >> 6) + slot->bitmap_left;
		gy = baseline - ((pen_y + pos[i].y_offset + 32) >> 6) - slot->bitmap_top;

		ccc = info[i].cluster < len ? hb_unicode_combining_class(ucd, cps[info[i].cluster])
					    : HB_UNICODE_COMBINING_CLASS_NOT_REORDERED;
		if (base_x1 >= base_x0 && !pos[i].x_advance && !pos[i].x_offset &&
		    !pos[i].y_offset && (ccc_above(ccc) || ccc_below(ccc))) {
			gx = (base_x0 + base_x1 + 1 - (int)map->width) / 2;
			if (ccc_above(ccc))
				gy = max(ink_top - 1 - (int)map->rows, 0);
			else
				gy = min(ink_bottom + 1, (int)ch - (int)map->rows);
		}
		if (map->width && map->rows) {
			if (ccc == HB_UNICODE_COMBINING_CLASS_NOT_REORDERED) {
				base_x0 = gx;
				base_x1 = gx + (int)map->width - 1;
				ink_top = gy;
				ink_bottom = gy + (int)map->rows - 1;
			} else {
				ink_top = min(ink_top, gy);
				ink_bottom = max(ink_bottom, gy + (int)map->rows - 1);
			}
		}

		for (y = 0; y < (int)map->rows; y++) {
			if (gy + y < 0 || gy + y >= (int)ch)
				continue;
			for (x = 0; x < (int)map->width; x++) {
				uint8_t v, *d;

				if (gx + x < 0 || gx + x >= (int)cw)
					continue;
				if (map->pixel_mode == FT_PIXEL_MODE_MONO)
					v = !!(map->buffer[y * map->pitch + x / 8] &
					       (1 << (7 - (x % 8)))) * 0xff;
				else
					v = map->buffer[y * map->pitch + x];
				d = &canvas[(gy + y) * cw + gx + x];
				if (v > *d)
					*d = v;
			}
		}
	next:
		FT_Bitmap_Done(slot->library, &tmp);
		pen_x += pos[i].x_advance;
		pen_y += pos[i].y_advance;
	}

	hb_buffer_destroy(hbuf);
	hb_font_destroy(hb);

	copy_map(&glyph->buf, canvas + ox - centre_dx(font, glyph, pen_x), gw, ch, cw, 0, 0);

	free(canvas);
	return glyph;

err_glyph:
	free(glyph);
	return NULL;
}

static void select_font_size(struct kmscon_font *font, FT_Face face)
{
	int height = font->height;

	/* Special case for bitmap fonts, which can't be scaled */
	if (face->num_fixed_sizes) {
		int bitmap_index = bitmap_font_select_size(face, height);

		FT_Select_Size(face, bitmap_index);
		return;
	}
	/* scale down to a size that fits the glyph height */
	do {
		if (request_size(face, height)) {
			log_warn("failed to set size to %d", height);
			return;
		}
		height--;
	} while (face->size->metrics.height >> 6 > font->height && height > 10);
}

static FT_Face prepare_tmp_face(FT_Library ft, struct ft_font *font, int fallback)
{
	FcChar8 *path;
	FT_Error err;
	int index = 0;
	FcPattern *pattern;
	FT_Face face = NULL;
	FcChar8 *full_name = NULL;

	pattern = FcFontRenderPrepare(NULL, font->pattern, font->fc->fonts[fallback]);
	if (!pattern)
		return NULL;

	FcPatternGetString(pattern, FC_FULLNAME, 0, &full_name);

	if (FcPatternGetString(pattern, FC_FILE, 0, &path) != FcResultMatch)
		goto err_pattern;

	if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) != FcResultMatch)
		log_warn("%s: failed to get face index", path);

	log_debug("Loading fallback font %s %s", (char *)full_name, (char *)path);

	err = FT_New_Face(ft, (char *)path, index, &face);
	if (err)
		face = NULL;

err_pattern:
	FcPatternDestroy(pattern);
	return face;
}

/*
 * The first font of @ftf's list that covers @ch, among the faces without
 * colour first: a colour face draws only what no other face covers (ADR-102).
 */
static int get_fallback(uint32_t ch, struct ft_font *ftf)
{
	int i, pass;

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < ftf->fc->nfont; i++) {
			FcCharSet *cs;
			FcBool colour = FcFalse;

			if (FcPatternGetCharSet(ftf->fc->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch ||
			    !FcCharSetHasChar(cs, ch))
				continue;
			FcPatternGetBool(ftf->fc->fonts[i], FC_COLOR, 0, &colour);
			if (!pass && colour)
				continue;
			return i;
		}
	}
	return -ENOENT;
}

static bool kmscon_font_freetype_has_glyph(struct kmscon_font *font, struct kmscon_font_attr *attr,
					   uint32_t ch)
{
	struct ft_data *ftd = font->data;
	struct ft_font *ftfont = attr->bold ? &ftd->bold : &ftd->regular;
	FT_UInt glyph_index = FT_Get_Char_Index(ftfont->face, ch);

	if (glyph_index)
		return true;

	return get_fallback(ch, ftfont) >= 0;
}

/* The face that draws @ch for @ftfont, and its glyph index, or NULL. */
static FT_Face face_for(struct kmscon_font *font, struct ft_font *ftfont, uint32_t ch,
			FT_UInt *index)
{
	struct ft_data *ftd = font->data;
	int fallback_index;

	*index = FT_Get_Char_Index(ftfont->face, ch);
	if (*index)
		return ftfont->face;

	/* Fallback, if the glyph is not found in the regular font */
	fallback_index = get_fallback(ch, ftfont);
	if (fallback_index < 0)
		return NULL;

	if (ftfont->fallback_index != fallback_index) {
		if (ftfont->fallback)
			FT_Done_Face(ftfont->fallback);
		ftfont->fallback = NULL;
		ftfont->fallback_index = fallback_index;
	}

	if (!ftfont->fallback) {
		ftfont->fallback = prepare_tmp_face(ftd->ft, ftfont, fallback_index);
		if (!ftfont->fallback)
			return NULL;
		select_font_size(font, ftfont->fallback);
	}

	*index = FT_Get_Char_Index(ftfont->fallback, ch);
	return *index ? ftfont->fallback : NULL;
}

/*
 * ADR-102: an emoji cluster draws from the best face with colour that covers
 * its first codepoint, fitted to its cells, unless emoji-color is off. Any
 * other cluster, or an emoji no colour face draws, goes to the faces in the
 * font's list, those without colour first; one drawn from a colour face
 * anyway is the mask of its colour glyph, fitted the same way. An emoji no
 * face covers is the text face's missing glyph, fitted the same way.
 */
static struct kmscon_glyph *kmscon_font_freetype_render(struct kmscon_font *font,
							struct kmscon_font_attr *attr,
							const uint32_t *cps, size_t len,
							unsigned int cwidth)
{
	struct ft_data *ftd = font->data;
	struct ft_font *ftfont = attr->bold ? &ftd->bold : &ftd->regular;
	bool emoji = kmscon_font_wants_colour(cps, len, cwidth);
	struct kmscon_glyph *glyph;
	FT_UInt glyph_index;
	FT_Face face;

	if (emoji && font->emoji_color) {
		face = kmscon_colour_face_for(&ftd->colour, cps[0]);
		glyph = face ? kmscon_colour_render(font, face, cps, len, cwidth, false) : NULL;
		if (glyph)
			return glyph;
	}

	face = face_for(font, ftfont, cps[0], &glyph_index);
	if (!face && emoji) {
		face = kmscon_colour_face_open(&ftd->colour, ftfont->fc->fonts[0]);
		return kmscon_colour_render_missing(font, face, cwidth);
	}
	if (!face)
		return NULL;
	if (kmscon_colour_face_has_colour(face))
		return kmscon_colour_render(font, face, cps, len, cwidth, true);
	if (len == 1)
		return render_glyph(font, face, glyph_index, cwidth);
	return render_cluster(font, face, cps, len, cwidth);
}

/*
 * Underline from the post table (position is the centre of the stroke, below
 * the baseline when negative); a zero thickness is estimated from the cell
 * height. Strike-through from the OS/2 strikeout (position is the top of the
 * stroke above the baseline), else centred on half the x-height.
 * kmscon_font_get_deco() clamps the result inside the cell.
 */
static void kmscon_font_freetype_get_deco(struct kmscon_font *font, struct kmscon_font_deco *out)
{
	struct ft_data *ftd = font->data;
	FT_Face face = ftd->regular.face;
	TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
	FT_Fixed ys = face->size->metrics.y_scale;
	long baseline = face->size->metrics.ascender; /* 26.6, from the cell top */
	long h = font->height, thick, centre, xh;
	FT_UInt x;

	thick = (FT_MulFix(face->underline_thickness, ys) + 32) >> 6;
	if (!FT_IS_SCALABLE(face) || thick <= 0) {
		thick = h / 14;
		if (thick < 1)
			thick = 1;
	}
	if (FT_IS_SCALABLE(face) && face->underline_position)
		centre = baseline - FT_MulFix(face->underline_position, ys);
	else
		centre = (h << 6) - (thick << 5) - (thick << 6);
	out->ul_thick = thick;
	out->ul_pos = max(0L, (centre - (thick << 5) + 32) >> 6);

	if (os2 && os2->version != 0xFFFF && os2->yStrikeoutSize > 0 && FT_IS_SCALABLE(face)) {
		thick = (FT_MulFix(os2->yStrikeoutSize, ys) + 32) >> 6;
		out->strike_thick = thick > 0 ? thick : 1;
		out->strike_pos =
			max(0L, (baseline - FT_MulFix(os2->yStrikeoutPosition, ys) + 32) >> 6);
		return;
	}

	out->strike_thick = out->ul_thick;
	xh = 0;
	if (os2 && os2->version != 0xFFFF && os2->version >= 2 && os2->sxHeight)
		xh = FT_MulFix(os2->sxHeight, ys);
	x = FT_Get_Char_Index(face, 'x');
	if (!xh && x && !FT_Load_Glyph(face, x, FT_LOAD_TARGET_LIGHT))
		xh = face->glyph->metrics.horiBearingY;
	if (!xh)
		xh = baseline / 2;
	centre = baseline - xh / 2;
	out->strike_pos = max(0L, (centre - ((long)out->strike_thick << 5) + 32) >> 6);
}

struct kmscon_font_ops kmscon_font_freetype_ops = {
	.name = "freetype",
	.owner = NULL,
	.init = kmscon_font_freetype_init,
	.destroy = kmscon_font_freetype_destroy,
	.has_glyph = kmscon_font_freetype_has_glyph,
	.render = kmscon_font_freetype_render,
	.draws_missing = true,
	.get_deco = kmscon_font_freetype_get_deco,
};
