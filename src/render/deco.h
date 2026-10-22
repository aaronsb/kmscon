/*
 * kmscon - Decoration sprites
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
 * Underlines and strike-through as one-cell A8 sprites (ADR-102), tinted by
 * the renderer at draw time. Every sprite tiles: the curly wave is one period
 * per cell and starts and ends at the same height, and dots and dashes divide
 * the cell so neighbours join. A wide cell takes the sprite twice.
 */

#ifndef KMSCON_DECO_H
#define KMSCON_DECO_H

#include "font/font.h"

/* The underline kinds share their values with enum tsm_underline_style. */
enum kmscon_deco {
	KMSCON_DECO_NONE = 0,
	KMSCON_DECO_SINGLE = 1,
	KMSCON_DECO_DOUBLE = 2,
	KMSCON_DECO_CURLY = 3,
	KMSCON_DECO_DOTTED = 4,
	KMSCON_DECO_DASHED = 5,
	KMSCON_DECO_STRIKE = 6,
	KMSCON_DECO_COUNT = 7,
};

/* A @w x @h sprite of @kind from metrics @m, or NULL. Free it with free(). */
struct kmscon_glyph *kmscon_deco_render(enum kmscon_deco kind, unsigned int w, unsigned int h,
					const struct kmscon_font_deco *m);

#endif /* KMSCON_DECO_H */
