/*
 * kmscon - Pictures for the image layer
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
 * Scaling and rotating pictures of premultiplied ARGB32 pixels (ADR-103):
 * a placement's picture drawn at another cell size than it was made with, or
 * on a rotated display. Integer arithmetic only, so a picture scales to the
 * same pixels on every machine and the tier 2 goldens can judge it.
 */

#ifndef KMSCON_PICTURE_H
#define KMSCON_PICTURE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Scale @src, @sw x @sh pixels with @stride pixels from one row to the next,
 * to @dw x @dh, and write the part [@x0, @x1) x [@y0, @y1) of the result to
 * @dst, packed, (@x1 - @x0) pixels a row. Each axis on its own: a box filter
 * where it shrinks (ADR-102's filter: each output pixel the mean of the source
 * area it covers), bilinear where it grows, a copy where it keeps its size.
 * Returns 0, -EINVAL or -ENOMEM.
 */
int picture_scale(const uint32_t *src, size_t stride, unsigned int sw, unsigned int sh,
		  unsigned int dw, unsigned int dh, unsigned int x0, unsigned int y0,
		  unsigned int x1, unsigned int y1, uint32_t *dst);

/*
 * Rotate @src, @w x @h packed pixels, into @dst as the display's orientation
 * @orientation (enum Orientation) turns the screen, as bbulk rotates glyphs:
 * @dst is @h x @w for a quarter turn, else @w x @h.
 */
void picture_rotate(const uint32_t *src, unsigned int w, unsigned int h, uint32_t *dst,
		    int orientation);

#endif /* KMSCON_PICTURE_H */
