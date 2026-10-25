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
 * every terminal, and the renderers' scaled pictures in system memory (bbulk).
 * The renderers' textures in video memory (gltex) count apart, in a budget of
 * their own under the same limit: they free only one another, so a texture
 * never takes room from an image (ADR-103's Memory section). kmscon is one
 * thread, so this is plain process state. A renderer module (mod-gltex) links its own copy
 * of image_budget.c through the render library; the calls a renderer makes are
 * exported (SHL_EXPORT in image_budget.c), so the module's bind to kmscon's
 * state rather than to its copy's (tests/check_module_symbols.py).
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
 * placement and transient, no placement, placed only in scrollback, and, with
 * a libtsm that has kitty graphics, kitty images placed on a screen's own
 * lines) and within a rank by last_use: least recently used first, and oldest
 * transmission first for the kitty images on a screen. A sixel placed on a
 * screen's own lines is never evicted; a reservation that still does not fit
 * is refused. A renderer's own picture, made within its frame, frees in the
 * same order but spares every image its frame draws and every image placed on
 * a screen's own lines, and frees nothing unless that makes it fit
 * (image_budget_picture_room). One that does not fit is left out until the
 * room in the budget grows (image_budget_gen).
 */

#ifndef KMSCON_IMAGE_BUDGET_H
#define KMSCON_IMAGE_BUDGET_H

#include <libtsm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMAGE_BUDGET_MIN ((size_t)16 << 20)
#define IMAGE_BUDGET_MAX ((size_t)192 << 20)
/* image-storage-limit=auto */
#define IMAGE_BUDGET_AUTO SIZE_MAX
/* the largest set limit, 1 TiB (less where size_t is smaller) */
#define IMAGE_BUDGET_LIMIT_MAX                                                                     \
	(SIZE_MAX - 1 < ((uint64_t)1 << 40) ? SIZE_MAX - 1 : (size_t)((uint64_t)1 << 40))

/* image-storage-limit: IMAGE_BUDGET_AUTO, or bytes up to IMAGE_BUDGET_LIMIT_MAX; 0 holds nothing */
void image_budget_set_limit(size_t limit);
/* the limit in bytes now */
size_t image_budget_limit(void);
/* the bytes held: libtsm's images and the renderers' pictures, not their textures */
size_t image_budget_used(void);
/* the renderers' textures' bytes, held apart under the same limit */
size_t image_budget_textures(void);
/* moves whenever the limit or the room under it grows, the textures' included */
uint64_t image_budget_gen(void);

/*
 * A display counted in the auto limit, @px pixels, once however many
 * renderers count it; each count is undone by one uncount.
 */
int image_budget_count_display(const void *disp, uint64_t px);
void image_budget_uncount_display(const void *disp);
/* the pixels of every display counted */
uint64_t image_budget_displays_px(void);
/* a display of @px pixels' share of the limit by size: limit x px / all */
uint64_t image_budget_display_share(uint64_t px);
/*
 * What a renderer drawing a display of @px pixels keeps between frames, its
 * scaled pictures or textures: twice its screen in ARGB, or its share of the
 * budget by size if that is less, so the displays' caches together stay
 * within it and leave a frame room to make more
 */
size_t image_budget_cache_limit(uint64_t px);

/*
 * A renderer's cache of pictures. @trim frees pictures its last frame did not
 * draw, least recently used first, until @want bytes are freed or none such is
 * left; @trimmable is the bytes it would free at most. Both are called between
 * the renderer's frames, from a libtsm reservation, a limit that shrank, or
 * another renderer's picture; a renderer's own picture calls them within its
 * frame, after it trimmed its cache itself, when they find nothing more.
 */
struct image_budget_cache {
	struct image_budget_cache *next;
	void (*trim)(struct image_budget_cache *cache, size_t want);
	size_t (*trimmable)(struct image_budget_cache *cache);
};

void image_budget_add_cache(struct image_budget_cache *cache);
void image_budget_remove_cache(struct image_budget_cache *cache);

/* @con's image @key is drawn by the frame being drawn */
typedef bool (*image_budget_drawn_cb)(struct tsm_screen *con, uint64_t key, void *data);

/*
 * Room for a picture of @bytes that a renderer makes within a frame, once its
 * own pictures gave what they can: the renderers' pictures their last frame
 * did not draw, then libtsm's images in ADR-103's order, each screen's as far
 * as the first that @drawn says the frame draws. An image placed on a screen's
 * own lines is never evicted for a picture, kitty or sixel. Nothing is freed
 * unless what can be makes the picture fit. Returns 0, or -ENOSPC when it does
 * not fit.
 */
int image_budget_picture_room(size_t bytes, image_budget_drawn_cb drawn, void *data);

/* a renderer's picture of @bytes, made or freed; made only once it fits */
void image_budget_add_picture(size_t bytes);
void image_budget_remove_picture(size_t bytes);

/*
 * A renderer's cache of textures, in the textures' budget: its @trim and
 * @trimmable are those of a cache of pictures, and are called the same way,
 * for room for another renderer's texture or for a limit that shrank.
 */
void image_budget_add_texture_cache(struct image_budget_cache *cache);
void image_budget_remove_texture_cache(struct image_budget_cache *cache);

/*
 * Room for a texture of @bytes that a renderer makes within a frame, once its
 * own textures gave what they can: the other renderers' textures their last
 * frame did not draw, least recently used first. Nothing is freed unless that
 * makes the texture fit, and no image or picture is freed for it. Returns 0,
 * or -ENOSPC when it does not fit.
 */
int image_budget_texture_room(size_t bytes);

/* a renderer's texture of @bytes, made or freed; made only once it fits */
void image_budget_add_texture(size_t bytes);
void image_budget_remove_texture(size_t bytes);

/*
 * @con's images reserve from the budget, and are evicted for other screens'
 * images. Removing it gives back what it holds; a screen is removed before it
 * is freed. Returns 0, -ENOMEM, or -ENOSPC when what @con already holds does
 * not fit.
 */
int image_budget_add_screen(struct tsm_screen *con);
void image_budget_remove_screen(struct tsm_screen *con);

/*
 * A terminal's images: its screen in the budget, and sixel and the kitty
 * graphics protocol on its vte, each while the configuration asks for it, the
 * limit is not 0, and every display the terminal draws on has a renderer that
 * draws image placements. Otherwise each is off, as with no-sixel or
 * no-kitty-graphics: for sixel DA1 omits attribute 4, a kitty command gets no
 * reply, so a client's query finds no support, and no new picture is stored;
 * pictures stored before stay until cleared or evicted. Turning kitty
 * graphics off cancels a request the vte has out with the host. The limit is
 * set before terminals are made.
 *
 * The budget holds a reference on the screen from add to remove, so a screen
 * whose terminal never removed it leaks rather than being read once freed.
 */
struct image_budget_terminal {
	struct tsm_screen *con;
	struct tsm_vte *vte;
	bool sixel;	    /* sixel in the configuration */
	bool kitty;	    /* kitty-graphics in the configuration */
	unsigned int blind; /* displays drawn by a renderer without placements */
};

int image_budget_add_terminal(struct image_budget_terminal *t, struct tsm_screen *con,
			      struct tsm_vte *vte, bool sixel, bool kitty);
void image_budget_remove_terminal(struct image_budget_terminal *t);
/*
 * The teardown of a terminal whose image_budget_add_terminal succeeded, in
 * its order: its screen leaves the budget, then the terminal's own references
 * on its vte and screen go.
 */
void image_budget_free_terminal(struct image_budget_terminal *t);
/* screens still in the budget, which is none once every terminal is freed */
unsigned int image_budget_screens(void);
/* a display the terminal draws on came or went; @draws_images: its renderer's */
void image_budget_terminal_display(struct image_budget_terminal *t, bool draws_images, bool added);

#endif /* KMSCON_IMAGE_BUDGET_H */
