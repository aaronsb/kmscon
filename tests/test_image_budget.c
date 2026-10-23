/*
 * ADR-103's image budget (src/render/image_budget.c) against real libtsm
 * screens: the auto limit from the displays, a set limit, eviction across
 * screens in ADR-103's order with the renderers' undrawn pictures first, a
 * placed image never evicted, a shrinking limit evicting down, a removed
 * screen giving its bytes back, and a sixel drawn through the vte reserving
 * from the budget. Images are made through libtsm's test calls.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/render/image_budget.c"

#define W 64
#define H 64
/* what one W x H image holds: its pixels and libtsm's overhead */
#define IMG ((size_t)W * H * 4 + TSM_SCREEN_IMAGE_OVERHEAD)

static uint32_t pixels[W * H];

static struct tsm_screen *new_screen(void)
{
	struct tsm_screen *con;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	tsm_screen_set_max_sb(con, 100);
	assert(image_budget_add_screen(con) == 0);
	return con;
}

static void free_screen(struct tsm_screen *con)
{
	image_budget_remove_screen(con);
	tsm_screen_unref(con);
}

/* a kitty image with no placement, id @id; 0 or a negative errno */
static int unplaced(struct tsm_screen *con, uint32_t id, bool transient, uint64_t *key)
{
	struct tsm_test_image img = {.width = W,
				     .height = H,
				     .pixels = pixels,
				     .kind = TSM_IMAGE_KITTY,
				     .id = id,
				     .transient = transient};

	return tsm_screen_test_image(con, &img, key);
}

/* a kitty image placed at the top left of the screen */
static uint64_t placed(struct tsm_screen *con, uint32_t id)
{
	struct tsm_test_placement pl = {0};
	uint64_t key, pkey;

	assert(unplaced(con, id, false, &key) == 0);
	pl.image = key;
	assert(tsm_screen_test_place(con, &pl, &pkey) == 0);
	return key;
}

/* @con still holds the image @key */
static bool holds(struct tsm_screen *con, uint64_t key)
{
	struct tsm_frame_image img;

	return tsm_screen_frame_image(con, key, &img) == 0;
}

/* a renderer's cache whose pictures the last frame did not draw */
static struct {
	struct image_budget_cache cache;
	size_t undrawn;
	unsigned int trims;
} fake;

static void fake_trim(struct image_budget_cache *cache, size_t want)
{
	size_t n = want < fake.undrawn ? want : fake.undrawn;

	(void)cache;
	fake.trims++;
	fake.undrawn -= n;
	image_budget_remove_picture(n);
}

static void test_auto_limit(void)
{
	static int a, b; /* two displays, by address */

	assert(image_budget_limit() == IMAGE_BUDGET_MIN);
	assert(image_budget_count_display(&a, 1920 * 1080) == 0);
	assert(image_budget_limit() == (size_t)1920 * 1080 * 4 * 4);
	/* a second renderer on the same display counts it once */
	assert(image_budget_count_display(&a, 1920 * 1080) == 0);
	assert(image_budget_displays_px() == 1920 * 1080);
	assert(image_budget_count_display(&b, 7680 * 2160) == 0);
	assert(image_budget_limit() == IMAGE_BUDGET_MAX);
	image_budget_uncount_display(&b);
	image_budget_uncount_display(&a);
	assert(image_budget_limit() == (size_t)1920 * 1080 * 4 * 4);
	image_budget_uncount_display(&a);
	assert(image_budget_displays_px() == 0 && !budget.displays);
	assert(image_budget_limit() == IMAGE_BUDGET_MIN);

	/* a set limit replaces auto, whatever the displays */
	image_budget_set_limit(5 * IMG);
	assert(image_budget_count_display(&a, 7680 * 2160) == 0);
	assert(image_budget_limit() == 5 * IMG);
	image_budget_uncount_display(&a);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
	assert(image_budget_limit() == IMAGE_BUDGET_MIN);
}

/*
 * Room for one more image is taken in ADR-103's order, across both screens:
 * undrawn pictures, then the transient unplaced image, then unplaced ones
 * least recently used first, then images placed only in scrollback. An image
 * placed on a screen's lines stays, and a new one that cannot fit is refused.
 */
static void test_eviction_order(void)
{
	struct tsm_screen *a = new_screen(), *b = new_screen();
	uint64_t old, transient, newer, scrolled, onscreen, k;
	uint64_t gen;
	unsigned int i;

	image_budget_set_limit(5 * IMG);
	/* a: an unplaced image, then one placed and scrolled into scrollback */
	assert(unplaced(a, 1, false, &old) == 0);
	scrolled = placed(a, 2);
	for (i = 0; i < 20; i++)
		tsm_screen_newline(a);
	/* b: a newer unplaced one, a transient one, one placed on screen */
	assert(unplaced(b, 3, false, &newer) == 0);
	assert(unplaced(b, 4, true, &transient) == 0);
	onscreen = placed(b, 5);
	assert(image_budget_used() == 5 * IMG);

	/* undrawn pictures go first; images stay while they give room */
	fake.undrawn = 2 * IMG;
	image_budget_add_picture(2 * IMG);
	image_budget_set_limit(7 * IMG);
	assert(unplaced(b, 6, false, &k) == 0);
	assert(fake.trims && fake.undrawn == IMG && holds(b, transient));
	image_budget_remove_picture(fake.undrawn);
	fake.undrawn = 0;
	image_budget_set_limit(6 * IMG);
	assert(image_budget_used() == 6 * IMG);

	gen = image_budget_gen();
	assert(unplaced(a, 7, false, &k) == 0);
	assert(!holds(b, transient) && holds(a, old) && image_budget_gen() != gen);
	assert(unplaced(a, 8, false, &k) == 0);
	assert(!holds(a, old) && holds(b, newer));
	assert(unplaced(a, 9, false, &k) == 0);
	assert(!holds(b, newer) && holds(a, scrolled));

	/* a smaller limit evicts down: the unplaced ones, then scrollback */
	image_budget_set_limit(3 * IMG);
	assert(image_budget_used() == 3 * IMG && holds(a, scrolled) && holds(b, onscreen));
	image_budget_set_limit(IMG);
	assert(!holds(a, scrolled) && holds(b, onscreen) && image_budget_used() == IMG);
	assert(unplaced(a, 10, false, &k) == -ENOSPC);
	assert(image_budget_used() == IMG);

	/* a screen removed gives its bytes back, before it is freed */
	image_budget_remove_screen(b);
	assert(image_budget_used() == 0 && holds(b, onscreen));
	tsm_screen_unref(b);
	free_screen(a);
	assert(!budget.screens && !budget.n_screens);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
}

/*
 * A renderer re-set on a font change uncounts its display and counts it back
 * at once: the auto limit dips to the floor between the two, and that dip
 * evicts nothing. The images still held are evicted down at the next
 * reservation if the limit has really shrunk.
 */
static void test_display_recount(void)
{
	struct tsm_screen *a = new_screen();
	static int disp;
	uint64_t keys[8], k;
	unsigned int i;
	/* about 24 MiB of unplaced images, past the 16 MiB floor */
	static uint32_t big[1024 * 1024];
	struct tsm_test_image img = {
		.width = 1024, .height = 1024, .pixels = big, .kind = TSM_IMAGE_KITTY};

	assert(image_budget_count_display(&disp, 7680 * 2160) == 0);
	for (i = 0; i < 6; i++) {
		img.id = 100 + i;
		assert(tsm_screen_test_image(a, &img, &keys[i]) == 0);
	}
	assert(image_budget_used() > IMAGE_BUDGET_MIN);
	image_budget_uncount_display(&disp);
	assert(image_budget_count_display(&disp, 7680 * 2160) == 0);
	for (i = 0; i < 6; i++)
		assert(holds(a, keys[i]));

	/* the display really goes: the next image evicts down to the floor */
	image_budget_uncount_display(&disp);
	assert(holds(a, keys[0]));
	assert(unplaced(a, 1, false, &k) == 0);
	assert(image_budget_used() <= IMAGE_BUDGET_MIN && !holds(a, keys[0]));
	free_screen(a);
}

/* limit 0 holds nothing: every image is refused */
static void test_zero(void)
{
	struct tsm_screen *a = new_screen();
	uint64_t k;

	image_budget_set_limit(0);
	assert(unplaced(a, 1, false, &k) == -ENOSPC);
	assert(image_budget_used() == 0);
	free_screen(a);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
}

static char reply[256];
static size_t reply_len;

static void vte_write(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	(void)vte;
	(void)data;
	if (reply_len + len < sizeof(reply)) {
		memcpy(reply + reply_len, u8, len);
		reply_len += len;
	}
}

/* DA1 advertises sixel, attribute 4 */
static bool da1_sixel(struct tsm_vte *vte)
{
	reply_len = 0;
	tsm_vte_input(vte, "\033[c", 3);
	reply[reply_len] = 0;
	return strstr(reply, ";4;") || strstr(reply, ";4c");
}

/* 8 x 12 pixels of register 1 */
static const char six[] = "\033Pq\"1;1;8;12#1;2;100;0;0#1!8~-#1!8~\033\\";

/*
 * A sixel through the vte reserves its picture from the budget, and gives it
 * back when cleared; a sixel past the budget is dropped.
 */
static void test_sixel(void)
{
	struct tsm_screen *con = new_screen();
	struct tsm_vte *vte;

	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);
	assert(!da1_sixel(vte));
	tsm_vte_set_sixel(vte, true);
	assert(da1_sixel(vte));

	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() == 8 * 12 * 4 + TSM_SCREEN_IMAGE_OVERHEAD);
	tsm_vte_input(vte, "\033[2J", 4);
	assert(image_budget_used() == 0);

	image_budget_set_limit(8 * 12 * 4);
	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() == 0 && tsm_screen_test_image_count(con) == 0);

	tsm_vte_unref(vte);
	free_screen(con);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
}

/*
 * A terminal's images: its screen joins the budget and leaves it; sixel is
 * offered (DA1 attribute 4) and pictures stored only while the configuration
 * asks for it, the limit is not 0, and every display the terminal draws on
 * draws image placements; a display that does not (gltex, until ADR-103 step
 * 3f) turns sixel off as if no-sixel were set.
 */
static void test_terminal(void)
{
	struct image_budget_terminal t = {0};
	struct tsm_screen *con;
	struct tsm_vte *vte;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);

	assert(image_budget_add_terminal(&t, con, vte, true) == 0);
	assert(budget.n_screens == 1 && da1_sixel(vte));

	/* a display without placement drawing: off, and nothing stored */
	image_budget_terminal_display(&t, false, true);
	assert(!da1_sixel(vte));
	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() == 0 && tsm_screen_test_image_count(con) == 0);
	/* one that draws them beside it: still off */
	image_budget_terminal_display(&t, true, true);
	assert(!da1_sixel(vte));
	/* the first one gone: on, and a picture is stored */
	image_budget_terminal_display(&t, false, false);
	assert(da1_sixel(vte));
	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() > 0 && tsm_screen_test_image_count(con) == 1);

	/* leaving gives the bytes back and turns sixel off */
	image_budget_remove_terminal(&t);
	assert(budget.n_screens == 0 && !budget.screens && image_budget_used() == 0);
	assert(!da1_sixel(vte));
	image_budget_remove_terminal(&t);
	tsm_vte_input(vte, "\033[2J", 4);

	/* no-sixel, and image-storage-limit=0 */
	assert(image_budget_add_terminal(&t, con, vte, false) == 0);
	assert(!da1_sixel(vte));
	image_budget_remove_terminal(&t);
	image_budget_set_limit(0);
	assert(image_budget_add_terminal(&t, con, vte, true) == 0);
	assert(!da1_sixel(vte));
	image_budget_remove_terminal(&t);
	assert(budget.n_screens == 0);

	/* a set limit past 1 TiB is held to it */
	image_budget_set_limit((size_t)1 << 41);
	assert(image_budget_limit() == IMAGE_BUDGET_LIMIT_MAX);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);

	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

int main(void)
{
	fake.cache.trim = fake_trim;
	image_budget_add_cache(&fake.cache);

	test_auto_limit();
	test_eviction_order();
	test_display_recount();
	test_zero();
	test_sixel();
	test_terminal();

	image_budget_remove_cache(&fake.cache);
	assert(!budget.caches && image_budget_used() == 0);
	printf("test_image_budget: ok\n");
	return 0;
}
