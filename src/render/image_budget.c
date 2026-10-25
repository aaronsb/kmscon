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

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include "image_budget.h"
#include "shl/misc.h"

static struct {
	size_t setting;	      /* image-storage-limit: IMAGE_BUDGET_AUTO or bytes */
	size_t images;	      /* libtsm's, reserved through the screens */
	size_t pictures;      /* the renderers' */
	size_t pictures_peak; /* the most pictures has held */
	uint64_t gen;
	uint64_t displays_px;

	/*
	 * Each display counts once, however many renderers count it: every
	 * session has a renderer on every display.
	 */
	struct {
		const void *disp;
		unsigned int users;
		uint64_t px;
	} *displays;
	unsigned int n_displays;

	struct tsm_screen **screens;
	unsigned int n_screens;
	struct image_budget_cache *caches;
} budget = {.setting = IMAGE_BUDGET_AUTO};

SHL_EXPORT
size_t image_budget_limit(void)
{
	uint64_t b = budget.displays_px * 4 * 4;

	if (budget.setting != IMAGE_BUDGET_AUTO)
		return budget.setting;
	return b < IMAGE_BUDGET_MIN   ? IMAGE_BUDGET_MIN
	       : b > IMAGE_BUDGET_MAX ? IMAGE_BUDGET_MAX
				      : b;
}

SHL_EXPORT
size_t image_budget_used(void)
{
	return budget.images + budget.pictures;
}

SHL_EXPORT
uint64_t image_budget_gen(void)
{
	return budget.gen;
}

uint64_t image_budget_displays_px(void)
{
	return budget.displays_px;
}

uint64_t image_budget_display_share(uint64_t px)
{
	uint64_t all = budget.displays_px, limit = image_budget_limit();

	/* divided before it is multiplied, so a set limit of up to
	 * IMAGE_BUDGET_LIMIT_MAX on the largest displays does not wrap */
	if (!all)
		return limit;
	return limit / all * px + limit % all * px / all;
}

SHL_EXPORT
size_t image_budget_cache_limit(uint64_t px)
{
	uint64_t screens = px * 4 * 2, share = image_budget_display_share(px);

	return screens < share ? screens : share;
}

/*
 * The index of the screen whose image tsm_screen_image_victim names first
 * across the screens @left allows (all when NULL), and that image in @best;
 * -1 when none has one
 */
static int next_victim(struct tsm_image_victim *best, const unsigned int *left)
{
	struct tsm_image_victim v;
	unsigned int i;
	int con = -1;

	for (i = 0; i < budget.n_screens; i++) {
		if ((left && !left[i]) || !tsm_screen_image_victim(budget.screens[i], &v))
			continue;
		if (con < 0 || v.rank < best->rank ||
		    (v.rank == best->rank && v.last_use < best->last_use)) {
			con = i;
			*best = v;
		}
	}
	return con;
}

/*
 * Frees, in ADR-103's order, until @bytes more fit under the limit: pictures
 * no renderer drew on its last frame, then libtsm's images by rank and, within
 * a rank, by last_use, at most @left[i] of screen i's when @left is given:
 * least recently used for unplaced and scrollback images, oldest transmission
 * for kitty images placed on a screen's own lines. Returns 0, or -ENOSPC when
 * they still do not fit.
 */
static int make_room(size_t bytes, unsigned int *left)
{
	size_t limit = image_budget_limit();
	struct image_budget_cache *c;
	struct tsm_image_victim v = {0};
	int i;

	if (bytes > limit)
		return -ENOSPC;
	for (c = budget.caches; c && image_budget_used() > limit - bytes; c = c->next)
		c->trim(c, image_budget_used() - (limit - bytes));
	while (image_budget_used() > limit - bytes) {
		i = next_victim(&v, left);
		if (i < 0 || !tsm_screen_image_evict(budget.screens[i]))
			return -ENOSPC;
		if (left)
			left[i]--;
	}
	return 0;
}

/*
 * What one screen's victims can give a picture: those before the first its
 * frame draws, and before the first kitty image placed on the screen's own
 * lines, which a picture never evicts whether or not its frame draws it
 * (ADR-103, Notes of 2026-10-23, Q1)
 */
struct give {
	struct tsm_screen *con;
	image_budget_drawn_cb drawn;
	void *data;
	size_t bytes;
	unsigned int n;
};

static bool give_victim(const struct tsm_image_victim *v, void *data)
{
	struct give *g = data;

#ifdef TSM_SCREEN_IMAGE_PLACED_KITTY
	if (v->rank >= TSM_IMAGE_PLACED_KITTY)
		return false;
#endif
	if (g->drawn(g->con, v->key, g->data))
		return false;
	g->bytes += v->bytes;
	g->n++;
	return true;
}

/*
 * Counts what the caches and each screen can give before anything is freed,
 * and frees only when that is enough. A screen gives its victims in its own
 * order up to the first its frame draws or the first placed on its own lines;
 * eviction takes no more of each.
 */
SHL_EXPORT
int image_budget_picture_room(size_t bytes, image_budget_drawn_cb drawn, void *data)
{
	size_t limit = image_budget_limit(), room = 0;
	struct image_budget_cache *c;
	unsigned int *left, i;
	struct give g;
	int ret = -ENOSPC;

	if (bytes > limit)
		return -ENOSPC;
	if (image_budget_used() <= limit - bytes)
		return 0;
	left = calloc(budget.n_screens + 1, sizeof(*left));
	if (!left)
		return -ENOSPC;
	for (c = budget.caches; c; c = c->next)
		room += c->trimmable(c);
	for (i = 0; i < budget.n_screens; i++) {
		g = (struct give){.con = budget.screens[i], .drawn = drawn, .data = data};
		if (tsm_screen_image_victims(budget.screens[i], give_victim, &g))
			goto out;
		room += g.bytes;
		left[i] = g.n;
	}
	if (room >= image_budget_used() - (limit - bytes))
		ret = make_room(bytes, left);
out:
	free(left);
	return ret;
}

/*
 * A set limit smaller than the one before evicts down to it at once. The auto
 * limit follows the displays without evicting: a renderer re-set on a font
 * change uncounts its display and counts it back at once, and the dip between
 * must cost no image. A display that really went is enforced by the next
 * reservation, which evicts down to the limit it finds.
 */
void image_budget_set_limit(size_t limit)
{
	size_t old = image_budget_limit();

	budget.setting = limit == IMAGE_BUDGET_AUTO || limit <= IMAGE_BUDGET_LIMIT_MAX
				 ? limit
				 : IMAGE_BUDGET_LIMIT_MAX;
	if (image_budget_limit() == old)
		return;
	budget.gen++;
	if (image_budget_limit() < old)
		make_room(0, NULL);
}

SHL_EXPORT
int image_budget_count_display(const void *disp, uint64_t px)
{
	unsigned int i;
	void *tmp;

	for (i = 0; i < budget.n_displays && budget.displays[i].disp != disp; i++)
		;
	if (i == budget.n_displays) {
		tmp = realloc(budget.displays, (i + 1) * sizeof(*budget.displays));
		if (!tmp)
			return -ENOMEM;
		budget.displays = tmp;
		budget.displays[i].disp = disp;
		budget.displays[i].users = 0;
		budget.displays[i].px = 0;
		budget.n_displays++;
	}
	budget.displays_px += px - budget.displays[i].px;
	budget.displays[i].px = px;
	budget.displays[i].users++;
	budget.gen++;
	return 0;
}

SHL_EXPORT
void image_budget_uncount_display(const void *disp)
{
	unsigned int i;

	for (i = 0; i < budget.n_displays && budget.displays[i].disp != disp; i++)
		;
	if (i == budget.n_displays || --budget.displays[i].users)
		return;
	budget.displays_px -= budget.displays[i].px;
	budget.displays[i] = budget.displays[--budget.n_displays];
	budget.gen++;
	if (!budget.n_displays) {
		free(budget.displays);
		budget.displays = NULL;
	}
}

SHL_EXPORT
void image_budget_add_cache(struct image_budget_cache *cache)
{
	cache->next = budget.caches;
	budget.caches = cache;
}

SHL_EXPORT
void image_budget_remove_cache(struct image_budget_cache *cache)
{
	struct image_budget_cache **cp;

	for (cp = &budget.caches; *cp; cp = &(*cp)->next) {
		if (*cp == cache) {
			*cp = cache->next;
			return;
		}
	}
}

SHL_EXPORT
void image_budget_add_picture(size_t bytes)
{
	budget.pictures += bytes;
	if (budget.pictures > budget.pictures_peak)
		budget.pictures_peak = budget.pictures;
}

SHL_EXPORT
void image_budget_remove_picture(size_t bytes)
{
	budget.pictures -= bytes;
	budget.gen++;
}

static int screen_reserve(struct tsm_screen *con, size_t bytes, void *data)
{
	(void)con;
	(void)data;
	if (make_room(bytes, NULL))
		return -ENOSPC;
	budget.images += bytes;
	return 0;
}

static void screen_release(struct tsm_screen *con, size_t bytes, void *data)
{
	(void)con;
	(void)data;
	budget.images -= bytes;
	budget.gen++;
}

int image_budget_add_screen(struct tsm_screen *con)
{
	struct tsm_screen **tmp;
	int ret;

	tmp = realloc(budget.screens, (budget.n_screens + 1) * sizeof(*tmp));
	if (!tmp)
		return -ENOMEM;
	budget.screens = tmp;
	budget.screens[budget.n_screens++] = con;
	ret = tsm_screen_set_image_budget(con, screen_reserve, screen_release, NULL);
	if (ret)
		budget.n_screens--;
	return ret;
}

void image_budget_remove_screen(struct tsm_screen *con)
{
	unsigned int i;

	for (i = 0; i < budget.n_screens && budget.screens[i] != con; i++)
		;
	if (i == budget.n_screens)
		return;
	/* gives back what @con holds, through screen_release */
	tsm_screen_set_image_budget(con, NULL, NULL, NULL);
	budget.screens[i] = budget.screens[--budget.n_screens];
	if (!budget.n_screens) {
		free(budget.screens);
		budget.screens = NULL;
	}
}

/* sixel and kitty graphics on @t's vte, or off, by the rule in image_budget.h */
static void terminal_update(struct image_budget_terminal *t)
{
	bool images = image_budget_limit() && !t->blind;

	tsm_vte_set_sixel(t->vte, t->sixel && images);
#ifdef TSM_VTE_KITTY_GRAPHICS
	tsm_vte_set_kitty_graphics(t->vte, t->kitty && images);
#endif
}

int image_budget_add_terminal(struct image_budget_terminal *t, struct tsm_screen *con,
			      struct tsm_vte *vte, bool sixel, bool kitty)
{
	int ret = image_budget_add_screen(con);

	if (ret)
		return ret;
	tsm_screen_ref(con);
	t->con = con;
	t->vte = vte;
	t->sixel = sixel;
	t->kitty = kitty;
	t->blind = 0;
	terminal_update(t);
	return 0;
}

void image_budget_remove_terminal(struct image_budget_terminal *t)
{
	if (!t->con)
		return;
	tsm_vte_set_sixel(t->vte, false);
#ifdef TSM_VTE_KITTY_GRAPHICS
	tsm_vte_set_kitty_graphics(t->vte, false);
#endif
	image_budget_remove_screen(t->con);
	tsm_screen_unref(t->con);
	t->con = NULL;
}

void image_budget_free_terminal(struct image_budget_terminal *t)
{
	struct tsm_screen *con = t->con;

	image_budget_remove_terminal(t);
	tsm_vte_unref(t->vte);
	t->vte = NULL;
	tsm_screen_unref(con);
}

unsigned int image_budget_screens(void)
{
	return budget.n_screens;
}

void image_budget_terminal_display(struct image_budget_terminal *t, bool draws_images, bool added)
{
	if (!t->con || draws_images)
		return;
	if (added)
		t->blind++;
	else if (t->blind)
		t->blind--;
	terminal_update(t);
}
