/*
 * kmscon - The process's image budget
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
 * ADR-103's image budget, image-storage-limit: one per kmscon process. It
 * covers the images in every libtsm screen's stores, main and alternate, of
 * every terminal, and the renderers' scaled pictures. kmscon is one thread, so
 * this is plain process state.
 *
 * The limit is set, or `auto`: four frames of the process's displays in ARGB,
 * at least IMAGE_BUDGET_MIN and at most IMAGE_BUDGET_MAX, recomputed when a
 * display is counted or uncounted. A set limit below what is held evicts down
 * to fit at once, as far as eviction can go; an auto limit that shrank with
 * the displays does so at the next reservation.
 *
 * A libtsm reservation that would pass the limit frees, in this order, across
 * every screen and renderer: the renderers' pictures that their last frame did
 * not draw; then libtsm's images, by tsm_screen_image_victim's rank (no
 * placement and transient, no placement, placed only in scrollback) and least
 * recently used first. An image placed on a screen's own lines is never
 * evicted; a reservation that still does not fit is refused. A renderer's own
 * picture evicts nothing outside its renderer: one that does not fit is left
 * out until the room in the budget grows (image_budget_gen).
 */

#ifndef KMSCON_IMAGE_BUDGET_H
#define KMSCON_IMAGE_BUDGET_H

#include <libtsm.h>
#include <stddef.h>
#include <stdint.h>

#define IMAGE_BUDGET_MIN ((size_t)16 << 20)
#define IMAGE_BUDGET_MAX ((size_t)192 << 20)
/* image-storage-limit=auto */
#define IMAGE_BUDGET_AUTO SIZE_MAX

/* image-storage-limit: IMAGE_BUDGET_AUTO, or bytes; 0 holds nothing */
void image_budget_set_limit(size_t limit);
/* the limit in bytes now */
size_t image_budget_limit(void);
/* the bytes held: libtsm's images and the renderers' pictures */
size_t image_budget_used(void);
/* moves whenever the limit or the room under it grows */
uint64_t image_budget_gen(void);

/*
 * A display counted in the auto limit, @px pixels, once however many
 * renderers count it; each count is undone by one uncount.
 */
int image_budget_count_display(const void *disp, uint64_t px);
void image_budget_uncount_display(const void *disp);
/* the pixels of every display counted */
uint64_t image_budget_displays_px(void);

/*
 * A renderer's cache of pictures. @trim frees pictures its last frame did not
 * draw, least recently used first, until @want bytes are freed or none such is
 * left. It is called between frames only, from a libtsm reservation or a
 * limit that shrank.
 */
struct image_budget_cache {
	struct image_budget_cache *next;
	void (*trim)(struct image_budget_cache *cache, size_t want);
};

void image_budget_add_cache(struct image_budget_cache *cache);
void image_budget_remove_cache(struct image_budget_cache *cache);

/* a renderer's picture of @bytes, made or freed; made only once it fits */
void image_budget_add_picture(size_t bytes);
void image_budget_remove_picture(size_t bytes);

/*
 * @con's images reserve from the budget, and are evicted for other screens'
 * images. Removing it gives back what it holds; a screen is removed before it
 * is freed. Returns 0, -ENOMEM, or -ENOSPC when what @con already holds does
 * not fit.
 */
int image_budget_add_screen(struct tsm_screen *con);
void image_budget_remove_screen(struct tsm_screen *con);

#endif /* KMSCON_IMAGE_BUDGET_H */
