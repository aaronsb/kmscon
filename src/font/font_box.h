/*
 * kmscon - Box drawing, block element, Braille, sextant and octant sprites
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
 * Box Drawing (U+2500-U+257F), Block Elements (U+2580-U+259F), Braille
 * Patterns (U+2800-U+28FF), the sextants (U+1FB00-U+1FB3B) and the octants
 * (U+1CD00-U+1CDE5) drawn as A8 sprites exactly the size of the cell, in
 * place of the font's glyphs (#46, #76). A font draws these between its own
 * ascent and descent, so where the cell is taller than that (line spacing),
 * stacked blocks and vertical lines leave a gap between rows, and Braille dots
 * and mosaic blocks miss the cell's grid.
 */

#ifndef KMSCON_FONT_BOX_H
#define KMSCON_FONT_BOX_H

#include <stdbool.h>
#include <stdint.h>
#include "font.h"

/* @cp is drawn by kmscon_font_box_render() rather than taken from a font */
bool kmscon_font_box_has(uint32_t cp);

/*
 * @cp as a @w x @h sprite with light lines @thick pixels wide (heavy lines
 * twice that), or NULL for a codepoint it does not draw or no memory. Free it
 * with free().
 */
struct kmscon_glyph *kmscon_font_box_render(uint32_t cp, unsigned int w, unsigned int h,
					    unsigned int thick);

#endif /* KMSCON_FONT_BOX_H */
