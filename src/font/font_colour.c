/*
 * kmscon - Colour glyphs fitted to their cells
 *
 * Copyright (c) 2026 the kmscon fork authors
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
 * The fit follows Ghostty's emoji constraint (MIT, Copyright (c) 2024
 * Mitchell Hashimoto, Ghostty contributors), src/font/SharedGrid.zig
 * renderGlyph(): the emoji is scaled with its aspect kept to the largest size
 * the cells allow, centred both ways, with a little padding so it does not
 * touch its neighbours. Ghostty pads 2.5% of a cell at the left and right;
 * here the padding is one pixel on every side, so no ink reaches the edge of
 * the box at any cell size. The scaling is a box filter over the strike
 * (ADR-102), not Ghostty's.
 */

#include <errno.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H
#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>
#include <stdlib.h>
#include <string.h>
#include "font_colour.h"
#include "shl/log.h"

#if defined(KMSCON_HB_RASTER) && HB_VERSION_ATLEAST(14, 2, 0)
#include <hb-raster.h>
#define KMSCON_COLRV1 1
#endif

#define LOG_SUBSYSTEM "font_colour"

/* at most this many shaped glyphs are drawn for one cluster */
#define COLOUR_MAX_PIECES 32

struct kmscon_colour_face {
	struct kmscon_colour_face *next;
	char *file;
	int index;
	FT_Face face; /* NULL when the file could not be opened */
};

void kmscon_colour_faces_init(struct kmscon_colour_faces *cf, FT_Library ft)
{
	memset(cf, 0, sizeof(*cf));
	cf->ft = ft;
}

void kmscon_colour_faces_free(struct kmscon_colour_faces *cf)
{
	struct kmscon_colour_face *f, *next;

	for (f = cf->open; f; f = next) {
		next = f->next;
		if (f->face)
			FT_Done_Face(f->face);
		free(f->file);
		free(f);
	}
	if (cf->set)
		FcFontSetDestroy(cf->set);
	memset(cf, 0, sizeof(*cf));
}

FT_Face kmscon_colour_face_open(struct kmscon_colour_faces *cf, const FcPattern *pattern)
{
	struct kmscon_colour_face *f;
	FcChar8 *file;
	int index = 0;

	if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch)
		return NULL;
	FcPatternGetInteger(pattern, FC_INDEX, 0, &index);

	for (f = cf->open; f; f = f->next)
		if (f->index == index && !strcmp(f->file, (const char *)file))
			return f->face;

	f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	f->file = strdup((const char *)file);
	f->index = index;
	if (!f->file) {
		free(f);
		return NULL;
	}
	if (FT_New_Face(cf->ft, f->file, index, &f->face)) {
		log_warning("cannot open colour font %s", f->file);
		f->face = NULL;
	} else {
		log_debug("colour font %s", f->file);
	}
	f->next = cf->open;
	cf->open = f;
	return f->face;
}

FT_Face kmscon_colour_face_for(struct kmscon_colour_faces *cf, uint32_t ch)
{
	FcPattern *pat;
	FcResult res;
	FcCharSet *cs;
	FcBool colour;
	int i;

	if (!cf->searched) {
		cf->searched = true;
		pat = FcPatternCreate();
		if (!pat)
			return NULL;
		FcPatternAddBool(pat, FC_COLOR, FcTrue);
		FcConfigSubstitute(NULL, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);
		cf->set = FcFontSort(NULL, pat, FcFalse, NULL, &res);
		FcPatternDestroy(pat);
	}
	if (!cf->set)
		return NULL;

	for (i = 0; i < cf->set->nfont; i++) {
		const FcPattern *f = cf->set->fonts[i];

		if (FcPatternGetBool(f, FC_COLOR, 0, &colour) != FcResultMatch || !colour)
			continue;
		if (FcPatternGetCharSet(f, FC_CHARSET, 0, &cs) != FcResultMatch ||
		    !FcCharSetHasChar(cs, ch))
			continue;
		return kmscon_colour_face_open(cf, f);
	}
	return NULL;
}

bool kmscon_colour_face_has_colour(FT_Face face)
{
	return face && FT_HAS_COLOR(face);
}

/*
 * One shaped glyph: premultiplied 0xAARRGGBB pixels, its top-left corner at
 * @x, @y from the pen origin (y down), and whether it has colour.
 */
struct colour_piece {
	int x, y;
	unsigned int w, h;
	uint32_t *px;
	bool colour;
};

/*
 * Size @face for a box @h pixels tall: a bitmap face takes its smallest strike
 * at least that tall, else its largest (ADR-102), and a scalable one is set to
 * @h pixels.
 */
static void colour_size_face(FT_Face face, unsigned int h)
{
	int i, best = -1, largest = 0;

	if (FT_IS_SCALABLE(face) || !face->num_fixed_sizes) {
		if (FT_Set_Pixel_Sizes(face, 0, h))
			log_warning("cannot size colour font to %u pixels", h);
		return;
	}
	for (i = 0; i < face->num_fixed_sizes; i++) {
		int sh = face->available_sizes[i].height;

		if (sh > face->available_sizes[largest].height)
			largest = i;
		if (sh >= (int)h && (best < 0 || sh < face->available_sizes[best].height))
			best = i;
	}
	if (FT_Select_Size(face, best < 0 ? largest : best))
		log_warning("cannot select a strike of the colour font");
}

/*
 * @slot's bitmap as a piece: BGRA as it is, any other mode as white coverage.
 * A negative pitch is an up-flow bitmap, whose buffer starts at its bottom
 * row, so the top row is found from the other end.
 */
static int colour_from_slot(FT_GlyphSlot slot, struct colour_piece *p)
{
	const FT_Bitmap *map = &slot->bitmap;
	const uint8_t *row0;
	FT_Bitmap tmp;
	unsigned int x, y;

	p->w = map->width;
	p->h = map->rows;
	p->x = slot->bitmap_left;
	p->y = -slot->bitmap_top;
	p->colour = map->pixel_mode == FT_PIXEL_MODE_BGRA;
	if (!p->w || !p->h)
		return -ENOENT;

	p->px = malloc((size_t)p->w * p->h * 4);
	if (!p->px)
		return -ENOMEM;

	if (p->colour) {
		row0 = map->buffer;
		if (map->pitch < 0)
			row0 -= (long)(p->h - 1) * map->pitch;
		for (y = 0; y < p->h; y++) {
			const uint8_t *s = row0 + (long)y * map->pitch;

			for (x = 0; x < p->w; x++, s += 4)
				p->px[y * p->w + x] = (uint32_t)s[3] << 24 | (uint32_t)s[2] << 16 |
						      (uint32_t)s[1] << 8 | s[0];
		}
		return 0;
	}

	FT_Bitmap_Init(&tmp);
	if (FT_Bitmap_Convert(slot->library, map, &tmp, 1)) {
		free(p->px);
		p->px = NULL;
		return -EINVAL;
	}
	for (y = 0; y < p->h; y++) {
		for (x = 0; x < p->w; x++) {
			uint32_t a = tmp.buffer[y * tmp.pitch + x];

			if (tmp.num_grays > 1 && tmp.num_grays < 256)
				a = a * 255 / (tmp.num_grays - 1);
			p->px[y * p->w + x] = a << 24 | a << 16 | a << 8 | a;
		}
	}
	FT_Bitmap_Done(slot->library, &tmp);
	return 0;
}

#ifdef KMSCON_COLRV1
/*
 * A COLRv1 glyph, painted by HarfBuzz's rasteriser through HarfBuzz's own font
 * functions (ADR-102, corrections from the prototype). Its image runs
 * bottom-up from y_origin, in the y-up space of the font. Fails for a glyph
 * without colour paint, which goes to the mask path.
 */
static int colour_from_paint(hb_font_t *font, hb_codepoint_t gid, struct colour_piece *p)
{
	hb_raster_paint_t *paint;
	hb_raster_image_t *img;
	hb_raster_extents_t e;
	hb_glyph_extents_t ge;
	const uint8_t *buf;
	unsigned int x, y;
	int ret = -ENOENT;

	if (!hb_font_get_glyph_extents(font, gid, &ge))
		return -ENOENT;
	paint = hb_raster_paint_create_or_fail();
	if (!paint)
		return -ENOMEM;
	/* the font's scale is in 26.6 pixels */
	hb_raster_paint_set_scale_factor(paint, 64, 64);
	if (!hb_raster_paint_set_glyph_extents(paint, &ge) ||
	    !hb_raster_paint_glyph_or_fail(paint, font, gid))
		goto out_paint;
	img = hb_raster_paint_render(paint);
	if (!img)
		goto out_paint;

	hb_raster_image_get_extents(img, &e);
	buf = hb_raster_image_get_buffer(img);
	p->w = e.width;
	p->h = e.height;
	p->x = e.x_origin;
	p->y = -(e.y_origin + (int)e.height);
	p->colour = true;
	if (!buf || !p->w || !p->h)
		goto out_img;
	p->px = malloc((size_t)p->w * p->h * 4);
	if (!p->px) {
		ret = -ENOMEM;
		goto out_img;
	}
	for (y = 0; y < p->h; y++) {
		const uint8_t *s = buf + (size_t)(p->h - 1 - y) * e.stride;

		for (x = 0; x < p->w; x++, s += 4)
			p->px[y * p->w + x] = (uint32_t)s[3] << 24 | (uint32_t)s[2] << 16 |
					      (uint32_t)s[1] << 8 | s[0];
	}
	ret = 0;
out_img:
	hb_raster_image_destroy(img);
out_paint:
	hb_raster_paint_destroy(paint);
	return ret;
}
#endif

static int colour_load(FT_Face face, hb_font_t *paint_font, hb_codepoint_t gid,
		       struct colour_piece *p)
{
#ifdef KMSCON_COLRV1
	if (paint_font && !colour_from_paint(paint_font, gid, p))
		return 0;
#endif
	if (FT_Load_Glyph(face, gid, FT_LOAD_COLOR))
		return -EINVAL;
	if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP &&
	    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
		return -EINVAL;
	return colour_from_slot(face->glyph, p);
}

static inline unsigned int colour_chan(uint32_t px, unsigned int c)
{
	return (px >> (8 * c)) & 0xff;
}

/* @src over @dst, both premultiplied */
static uint32_t colour_over(uint32_t src, uint32_t dst)
{
	unsigned int na = 255 - colour_chan(src, 3), c;
	uint32_t out = 0;

	for (c = 0; c < 4; c++)
		out |= (colour_chan(src, c) + (colour_chan(dst, c) * na + 127) / 255) << (8 * c);
	return out;
}

/*
 * Box-filter @src, @sw x @sh premultiplied pixels, to @dw x @dh: each output
 * pixel is the mean of the source area it covers, weighted by how much of
 * each source pixel falls inside it. Integer arithmetic, so the result is the
 * same on every machine. Scaling up works too, as a blend of the one or two
 * source pixels under each output pixel.
 */
static int colour_box_filter(const uint32_t *src, unsigned int sw, unsigned int sh, uint32_t *dst,
			     unsigned int dw, unsigned int dh)
{
	uint64_t *rows, *acc, sum[4], div = (uint64_t)sw * sh;
	unsigned int x, y, i, c;

	/* each row's horizontal sums, in units where an output pixel spans
	 * @sw and a source pixel @dw */
	rows = calloc((size_t)sh * dw * 4, sizeof(*rows));
	if (!rows)
		return -ENOMEM;

	for (y = 0; y < sh; y++) {
		for (x = 0; x < dw; x++) {
			uint64_t lo = (uint64_t)x * sw, hi = lo + sw;

			acc = &rows[((size_t)y * dw + x) * 4];
			for (i = lo / dw; i < sw && (uint64_t)i * dw < hi; i++) {
				uint64_t a = (uint64_t)i * dw, b = a + dw;
				uint64_t ov = (hi < b ? hi : b) - (lo > a ? lo : a);
				uint32_t p = src[(size_t)y * sw + i];

				for (c = 0; c < 4; c++)
					acc[c] += colour_chan(p, c) * ov;
			}
		}
	}

	for (y = 0; y < dh; y++) {
		uint64_t lo = (uint64_t)y * sh, hi = lo + sh;

		for (x = 0; x < dw; x++) {
			uint32_t out = 0;

			memset(sum, 0, sizeof(sum));
			for (i = lo / dh; i < sh && (uint64_t)i * dh < hi; i++) {
				uint64_t a = (uint64_t)i * dh, b = a + dh;
				uint64_t ov = (hi < b ? hi : b) - (lo > a ? lo : a);

				acc = &rows[((size_t)i * dw + x) * 4];
				for (c = 0; c < 4; c++)
					sum[c] += acc[c] * ov;
			}
			for (c = 0; c < 4; c++) {
				uint64_t v = (sum[c] + div / 2) / div;

				out |= (uint32_t)(v > 255 ? 255 : v) << (8 * c);
			}
			dst[(size_t)y * dw + x] = out;
		}
	}

	free(rows);
	return 0;
}

/*
 * Fit @canvas, @cw x @ch, into a new glyph @bw x @bh: the largest size with
 * its aspect kept that leaves one pixel clear on every side, centred.
 */
static struct kmscon_glyph *colour_fit(const uint32_t *canvas, unsigned int cw, unsigned int ch,
				       unsigned int bw, unsigned int bh, unsigned int cwidth,
				       bool argb)
{
	struct kmscon_glyph *glyph;
	unsigned int iw = bw > 2 ? bw - 2 : 1, ih = bh > 2 ? bh - 2 : 1;
	unsigned int dw, dh, ox, oy, x, y;
	uint32_t *scaled;

	if ((uint64_t)iw * ch <= (uint64_t)ih * cw) {
		dw = iw;
		dh = ((uint64_t)ch * iw + cw / 2) / cw;
	} else {
		dh = ih;
		dw = ((uint64_t)cw * ih + ch / 2) / ch;
	}
	dw = dw ? dw : 1;
	dh = dh ? dh : 1;
	ox = (bw - dw) / 2;
	oy = (bh - dh) / 2;

	scaled = malloc((size_t)dw * dh * 4);
	if (!scaled)
		return NULL;
	if (colour_box_filter(canvas, cw, ch, scaled, dw, dh)) {
		free(scaled);
		return NULL;
	}

	glyph = calloc(1, sizeof(*glyph) + (size_t)bw * bh * (argb ? 4 : 1));
	if (!glyph) {
		free(scaled);
		return NULL;
	}
	glyph->double_width = cwidth == 2;
	glyph->buf.width = bw;
	glyph->buf.height = bh;
	glyph->buf.format = argb ? VIDEO_FORMAT_ARGB32 : VIDEO_FORMAT_A8;

	for (y = 0; y < dh; y++) {
		for (x = 0; x < dw; x++) {
			uint32_t p = scaled[y * dw + x];
			size_t at = (size_t)(oy + y) * bw + ox + x;

			if (argb)
				((uint32_t *)glyph->buf.data)[at] = p;
			else
				glyph->buf.data[at] = colour_chan(p, 3);
		}
	}
	free(scaled);
	return glyph;
}

/*
 * Compose the @np pieces into one picture, the box around all their ink, and
 * fit it into a glyph @cwidth cells wide; NULL when @need_colour is set and
 * no piece has colour. Frees the pieces' pixels.
 */
static struct kmscon_glyph *colour_compose(struct kmscon_font *font, struct colour_piece *pieces,
					   unsigned int np, unsigned int cwidth, bool mask,
					   bool need_colour)
{
	struct kmscon_glyph *glyph = NULL;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	unsigned int i, cw, ch;
	bool colour = false;
	uint32_t *canvas;

	if (!np)
		return NULL;

	for (i = 0; i < np; i++) {
		struct colour_piece *p = &pieces[i];

		if (!i || p->x < x0)
			x0 = p->x;
		if (!i || p->y < y0)
			y0 = p->y;
		if (!i || p->x + (int)p->w > x1)
			x1 = p->x + p->w;
		if (!i || p->y + (int)p->h > y1)
			y1 = p->y + p->h;
		colour |= p->colour;
	}

	cw = x1 - x0;
	ch = y1 - y0;
	canvas = need_colour && !colour ? NULL : calloc((size_t)cw * ch, 4);
	if (canvas) {
		for (i = 0; i < np; i++) {
			struct colour_piece *p = &pieces[i];
			unsigned int x, y;

			for (y = 0; y < p->h; y++) {
				uint32_t *d = &canvas[(size_t)(p->y - y0 + y) * cw + p->x - x0];

				for (x = 0; x < p->w; x++)
					d[x] = colour_over(p->px[y * p->w + x], d[x]);
			}
		}
		glyph = colour_fit(canvas, cw, ch, cwidth * font->width, font->height, cwidth,
				   colour && !mask);
		free(canvas);
	}

	for (i = 0; i < np; i++)
		free(pieces[i].px);
	return glyph;
}

struct kmscon_glyph *kmscon_colour_render(struct kmscon_font *font, FT_Face face,
					  const uint32_t *cps, size_t len, unsigned int cwidth,
					  bool mask)
{
	struct colour_piece pieces[COLOUR_MAX_PIECES];
	unsigned int n, i, np = 0;
	int pen_x = 0, pen_y = 0;
	hb_font_t *hb, *paint_font = NULL;
	hb_buffer_t *hbuf;
	hb_glyph_info_t *info;
	hb_glyph_position_t *pos;

	if (!face || !font->width || !font->height)
		return NULL;

	colour_size_face(face, font->height > 2 ? font->height - 2 : font->height);
	hb = hb_ft_font_create_referenced(face);
	hb_ft_font_set_load_flags(hb, FT_LOAD_COLOR);
#ifdef KMSCON_COLRV1
	if (hb_ot_color_has_paint(hb_font_get_face(hb))) {
		int xs, ys;

		/* HarfBuzz's own font functions, at hb-ft's scale */
		paint_font = hb_font_create(hb_font_get_face(hb));
		hb_font_get_scale(hb, &xs, &ys);
		hb_font_set_scale(paint_font, xs, ys);
	}
#endif
	hbuf = hb_buffer_create();
	hb_buffer_add_utf32(hbuf, cps, len, 0, len);
	hb_buffer_guess_segment_properties(hbuf);
	hb_shape(hb, hbuf, NULL, 0);
	info = hb_buffer_get_glyph_infos(hbuf, &n);
	pos = hb_buffer_get_glyph_positions(hbuf, &n);

	for (i = 0; i < n && np < COLOUR_MAX_PIECES; i++) {
		struct colour_piece *p = &pieces[np];

		memset(p, 0, sizeof(*p));
		/* glyph 0, the face's missing glyph, is left out, so it does not
		 * cover what the rest of the cluster draws */
		if (info[i].codepoint && !colour_load(face, paint_font, info[i].codepoint, p)) {
			p->x += (pen_x + pos[i].x_offset + 32) >> 6;
			p->y -= (pen_y + pos[i].y_offset + 32) >> 6;
			np++;
		}
		pen_x += pos[i].x_advance;
		pen_y += pos[i].y_advance;
	}
	hb_buffer_destroy(hbuf);
	if (paint_font)
		hb_font_destroy(paint_font);
	hb_font_destroy(hb);

	return colour_compose(font, pieces, np, cwidth, mask, true);
}

struct kmscon_glyph *kmscon_colour_render_missing(struct kmscon_font *font, FT_Face face,
						  unsigned int cwidth)
{
	struct colour_piece piece;

	if (!face || !font->width || !font->height)
		return NULL;
	colour_size_face(face, font->height > 2 ? font->height - 2 : font->height);
	memset(&piece, 0, sizeof(piece));
	if (colour_load(face, NULL, 0, &piece))
		return NULL;
	return colour_compose(font, &piece, 1, cwidth, true, false);
}
