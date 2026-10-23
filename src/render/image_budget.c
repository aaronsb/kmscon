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

size_t image_budget_limit(void)
{
	uint64_t b = budget.displays_px * 4 * 4;

	if (budget.setting != IMAGE_BUDGET_AUTO)
		return budget.setting;
	return b < IMAGE_BUDGET_MIN   ? IMAGE_BUDGET_MIN
	       : b > IMAGE_BUDGET_MAX ? IMAGE_BUDGET_MAX
				      : b;
}

size_t image_budget_used(void)
{
	return budget.images + budget.pictures;
}

uint64_t image_budget_gen(void)
{
	return budget.gen;
}

uint64_t image_budget_displays_px(void)
{
	return budget.displays_px;
}

/* the screen whose image tsm_screen_image_victim names first across all */
static struct tsm_screen *next_victim(void)
{
	struct tsm_image_victim v, best;
	struct tsm_screen *con = NULL;
	unsigned int i;

	for (i = 0; i < budget.n_screens; i++) {
		if (!tsm_screen_image_victim(budget.screens[i], &v))
			continue;
		if (!con || v.rank < best.rank ||
		    (v.rank == best.rank && v.last_use < best.last_use)) {
			con = budget.screens[i];
			best = v;
		}
	}
	return con;
}

/*
 * Frees, in ADR-103's order, until @bytes more fit under the limit: pictures
 * no renderer drew on its last frame, then libtsm's images by rank and least
 * recently used. Returns 0, or -ENOSPC when they still do not fit.
 */
static int make_room(size_t bytes)
{
	size_t limit = image_budget_limit();
	struct image_budget_cache *c;
	struct tsm_screen *con;

	if (bytes > limit)
		return -ENOSPC;
	for (c = budget.caches; c && image_budget_used() > limit - bytes; c = c->next)
		c->trim(c, image_budget_used() - (limit - bytes));
	while (image_budget_used() > limit - bytes) {
		con = next_victim();
		if (!con || !tsm_screen_image_evict(con))
			return -ENOSPC;
	}
	return 0;
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

	budget.setting = limit;
	if (image_budget_limit() == old)
		return;
	budget.gen++;
	if (image_budget_limit() < old)
		make_room(0);
}

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

void image_budget_add_cache(struct image_budget_cache *cache)
{
	cache->next = budget.caches;
	budget.caches = cache;
}

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

void image_budget_add_picture(size_t bytes)
{
	budget.pictures += bytes;
	if (budget.pictures > budget.pictures_peak)
		budget.pictures_peak = budget.pictures;
}

void image_budget_remove_picture(size_t bytes)
{
	budget.pictures -= bytes;
	budget.gen++;
}

static int screen_reserve(struct tsm_screen *con, size_t bytes, void *data)
{
	(void)con;
	(void)data;
	if (make_room(bytes))
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
