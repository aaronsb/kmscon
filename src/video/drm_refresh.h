/*
 * kmscon - A DRM mode's refresh rate
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

#ifndef KMSCON_DRM_REFRESH_H
#define KMSCON_DRM_REFRESH_H

#include <stdint.h>
#include <xf86drmMode.h>

/*
 * @mode's refresh rate in mHz from its pixel clock (kHz) and totals, as the
 * kernel's drm_mode_vrefresh counts it; the rounded vrefresh when the totals
 * are unset.
 */
static inline unsigned int drm_mode_refresh_mhz(const drmModeModeInfo *mode)
{
	uint64_t num = (uint64_t)mode->clock * 1000 * 1000;
	uint64_t den = (uint64_t)mode->htotal * mode->vtotal;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (mode->vscan > 1)
		den *= mode->vscan;
	if (!den)
		return mode->vrefresh * 1000;
	return num / den;
}

#endif /* KMSCON_DRM_REFRESH_H */
