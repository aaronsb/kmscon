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
 * Colour glyphs (ADR-102 step 4c), shared by the freetype and pango backends:
 * fontconfig finds a face with colour, HarfBuzz shapes the cluster on it,
 * FreeType draws colour bitmaps and COLRv0 layers, HarfBuzz's rasteriser
 * paints COLRv1, and the result is box-filtered into the glyph's cells.
 */

#ifndef KMSCON_FONT_COLOUR_H
#define KMSCON_FONT_COLOUR_H

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdbool.h>
#include <stdint.h>
#include "font.h"

struct kmscon_colour_face;

/* The faces with colour a backend has opened, and fontconfig's list of them. */
struct kmscon_colour_faces {
	FT_Library ft;
	FcFontSet *set;
	bool searched;
	struct kmscon_colour_face *open;
};

void kmscon_colour_faces_init(struct kmscon_colour_faces *cf, FT_Library ft);
void kmscon_colour_faces_free(struct kmscon_colour_faces *cf);

/* The best face with colour that covers @ch, or NULL. Owned by @cf. */
FT_Face kmscon_colour_face_for(struct kmscon_colour_faces *cf, uint32_t ch);

/*
 * The face fontconfig's @pattern names (FC_FILE, FC_INDEX), or NULL. Owned by
 * @cf, which opens it once and sizes it for each glyph it draws, so it is
 * never the face a backend draws its text with.
 */
FT_Face kmscon_colour_face_open(struct kmscon_colour_faces *cf, const FcPattern *pattern);

/* @face has colour glyphs: bitmaps, or a COLR table */
bool kmscon_colour_face_has_colour(FT_Face face);

/*
 * Draw the cluster @cps from @face, shaped by HarfBuzz, fitted into a box
 * @cwidth cells wide: scaled with its aspect kept to the largest size that
 * leaves a pixel clear on every side of the box, and centred in it. The glyph
 * is premultiplied ARGB, or an A8 mask of its alpha when @mask is set or
 * nothing in it has colour. NULL when @face draws nothing for @cps.
 */
struct kmscon_glyph *kmscon_colour_render(struct kmscon_font *font, FT_Face face,
					  const uint32_t *cps, size_t len, unsigned int cwidth,
					  bool mask);

/*
 * @face's missing glyph (glyph 0) as a mask, fitted into a box @cwidth cells
 * wide as kmscon_colour_render fits a cluster. @face is sized for the box, so
 * it must be one @cf opened, not a face that draws text. NULL when the
 * missing glyph is empty.
 */
struct kmscon_glyph *kmscon_colour_render_missing(struct kmscon_font *font, FT_Face face,
						  unsigned int cwidth);

#endif /* KMSCON_FONT_COLOUR_H */
