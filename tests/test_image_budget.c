/*
 * ADR-103's image budget (src/render/image_budget.c) against real libtsm
 * screens: the auto limit from the displays, a set limit, eviction across
 * screens in ADR-103's order with the renderers' undrawn pictures first, a
 * sixel placed on a screen never evicted, and, with a libtsm that has kitty
 * graphics, kitty images placed on a screen evicted last, oldest transmission
 * first, but never for a renderer's picture; a shrinking limit evicting down, a removed
 * screen giving its bytes back, and a sixel drawn through the vte reserving
 * from the budget, and a terminal's vte made into the budget and unwound from
 * it. Images are made through libtsm's test calls.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/render/image_budget.c"
#include "../src/terminal_vte.c"

#ifdef TSM_VTE_KITTY_GRAPHICS
#define HAVE_KITTY true
#else
#define HAVE_KITTY false
#endif

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

/* an image of @kind placed at the top left of the screen */
static uint64_t placed(struct tsm_screen *con, uint32_t id, unsigned int kind)
{
	struct tsm_test_image img = {
		.width = W, .height = H, .pixels = pixels, .kind = kind, .id = id};
	struct tsm_test_placement pl = {0};
	uint64_t key, pkey;

	assert(tsm_screen_test_image(con, &img, &key) == 0);
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

static size_t fake_trimmable(struct image_budget_cache *cache)
{
	(void)cache;
	return fake.undrawn;
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
 * least recently used first, then images placed only in scrollback. A sixel
 * placed on a screen's lines stays, and a new image that cannot fit is
 * refused.
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
	scrolled = placed(a, 2, TSM_IMAGE_KITTY);
	for (i = 0; i < 20; i++)
		tsm_screen_newline(a);
	/* b: a newer unplaced one, a transient one, a sixel placed on screen */
	assert(unplaced(b, 3, false, &newer) == 0);
	assert(unplaced(b, 4, true, &transient) == 0);
	onscreen = placed(b, 0, TSM_IMAGE_SIXEL);
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

#ifdef TSM_SCREEN_IMAGE_PLACED_KITTY
static bool drawn_none(struct tsm_screen *con, uint64_t key, void *data)
{
	return false;
}

/*
 * ADR-103's storage-quota rule: kitty images placed on a screen's lines are
 * evicted for a new image after the unplaced ones, oldest transmission first,
 * and the sixel stays. A renderer's picture never evicts them, so its count
 * of what a screen can give stops at the first of them.
 */
static void test_placed_kitty(void)
{
	struct tsm_screen *a = new_screen();
	uint64_t k1, k2, six, u1, u2, k;

	image_budget_set_limit(3 * IMG);
	k1 = placed(a, 1, TSM_IMAGE_KITTY);
	k2 = placed(a, 2, TSM_IMAGE_KITTY);
	six = placed(a, 0, TSM_IMAGE_SIXEL);
	assert(image_budget_used() == 3 * IMG);

	assert(unplaced(a, 3, false, &u1) == 0);
	assert(!holds(a, k1) && holds(a, k2) && holds(a, six));
	assert(unplaced(a, 4, false, &u2) == 0);
	assert(!holds(a, u1) && holds(a, k2) && holds(a, u2));

	/* a picture takes the unplaced image, then stops at the placed kitty one */
	assert(image_budget_picture_room(IMG, drawn_none, NULL) == 0);
	assert(!holds(a, u2) && holds(a, k2));
	image_budget_add_picture(IMG);
	assert(image_budget_picture_room(IMG, drawn_none, NULL) == -ENOSPC);
	assert(holds(a, k2) && holds(a, six));
	image_budget_remove_picture(IMG);

	/* a new image takes it; then only the sixel is left, which stays */
	image_budget_set_limit(2 * IMG);
	assert(holds(a, k2) && holds(a, six));
	assert(unplaced(a, 5, false, &k) == 0 && !holds(a, k2));
	image_budget_set_limit(IMG);
	assert(holds(a, six) && image_budget_used() == IMG);
	assert(unplaced(a, 6, false, &k) == -ENOSPC);

	free_screen(a);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
}
#endif

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

#ifdef TSM_VTE_KITTY_GRAPHICS
/* a kitty query of one f=32 pixel answers OK: the client's detection */
static bool kitty_answers(struct tsm_vte *vte)
{
	static const char q[] = "\033_Ga=q,i=31,s=1,v=1,f=32;AAAAAA==\033\\";

	reply_len = 0;
	tsm_vte_input(vte, q, sizeof(q) - 1);
	reply[reply_len] = 0;
	return strstr(reply, "\033_Gi=31;OK\033\\");
}
#else
static bool kitty_answers(struct tsm_vte *vte)
{
	(void)vte;
	return false;
}
#endif

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
 * offered (DA1 attribute 4) and pictures stored, and the kitty graphics
 * protocol answers a query, only while the configuration asks for each, the
 * limit is not 0, and every display the terminal draws on draws image
 * placements; a display that does not (gltex, until ADR-103 step 3f) turns
 * both off as if no-sixel and no-kitty-graphics were set.
 */
static void test_terminal(void)
{
	struct image_budget_terminal t = {0};
	struct tsm_screen *con;
	struct tsm_vte *vte;
	size_t used;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);

	assert(image_budget_add_terminal(&t, con, vte, true, true) == 0);
	assert(budget.n_screens == 1 && da1_sixel(vte));
	assert(kitty_answers(vte) == HAVE_KITTY);

	/* a display without placement drawing: off, and nothing stored */
	image_budget_terminal_display(&t, false, true);
	assert(!da1_sixel(vte) && !kitty_answers(vte));
	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() == 0 && tsm_screen_test_image_count(con) == 0);
	/* one that draws them beside it: still off */
	image_budget_terminal_display(&t, true, true);
	assert(!da1_sixel(vte));
	/* the first one gone: on, and a picture is stored */
	image_budget_terminal_display(&t, false, false);
	assert(da1_sixel(vte) && kitty_answers(vte) == HAVE_KITTY);
	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() > 0 && tsm_screen_test_image_count(con) == 1);

	/*
	 * a display without placement drawing arrives: the picture stored
	 * stays, counted, until cleared or evicted; only new sixel is refused
	 */
	used = image_budget_used();
	image_budget_terminal_display(&t, false, true);
	assert(!da1_sixel(vte) && image_budget_used() == used);
	assert(tsm_screen_test_image_count(con) == 1);
	tsm_vte_input(vte, "\r\n\r\n", 4);
	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() == used && tsm_screen_test_image_count(con) == 1);
	image_budget_terminal_display(&t, false, false);
	assert(da1_sixel(vte) && tsm_screen_test_image_count(con) == 1);

	/* leaving gives the bytes back and turns both off */
	image_budget_remove_terminal(&t);
	assert(budget.n_screens == 0 && !budget.screens && image_budget_used() == 0);
	assert(!da1_sixel(vte) && !kitty_answers(vte));
	image_budget_remove_terminal(&t);
	tsm_vte_input(vte, "\033[2J", 4);

	/* no-sixel and no-kitty-graphics, each alone, and image-storage-limit=0 */
	assert(image_budget_add_terminal(&t, con, vte, false, true) == 0);
	assert(!da1_sixel(vte) && kitty_answers(vte) == HAVE_KITTY);
	image_budget_remove_terminal(&t);
	assert(image_budget_add_terminal(&t, con, vte, true, false) == 0);
	assert(da1_sixel(vte) && !kitty_answers(vte));
	image_budget_remove_terminal(&t);
	image_budget_set_limit(0);
	assert(image_budget_add_terminal(&t, con, vte, true, true) == 0);
	assert(!da1_sixel(vte) && !kitty_answers(vte));
	image_budget_remove_terminal(&t);
	assert(budget.n_screens == 0);

	/* a set limit past 1 TiB is held to it */
	image_budget_set_limit((size_t)1 << 41);
	assert(image_budget_limit() == IMAGE_BUDGET_LIMIT_MAX);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);

	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

/*
 * The terminal's teardown, image_budget_free_terminal, leaves nothing in the
 * budget. A terminal that dropped its screen without leaving the budget
 * leaves a screen the budget still holds a reference on, so a later
 * reservation reads a live screen (ASan would catch a freed one), until it is
 * removed.
 */
static void test_terminal_teardown(void)
{
	struct image_budget_terminal t = {0}, lost = {0};
	struct tsm_screen *con, *other;
	struct tsm_vte *vte;
	uint64_t k;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);
	assert(image_budget_add_terminal(&t, con, vte, true, true) == 0);
	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() > 0 && image_budget_screens() == 1);
	image_budget_free_terminal(&t);
	assert(image_budget_screens() == 0 && image_budget_used() == 0 && !t.con && !t.vte);

	/* a terminal that never left: its screen outlives its own references */
	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);
	assert(image_budget_add_terminal(&lost, con, vte, true, true) == 0);
	assert(unplaced(con, 1, false, &k) == 0);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	image_budget_set_limit(IMG);
	other = new_screen();
	assert(unplaced(other, 2, false, &k) == 0);
	/* the lost screen's image was the victim: the screen was read, live */
	assert(tsm_screen_test_image_count(con) == 0);
	free_screen(other);
	lost.vte = NULL;
	image_budget_remove_screen(lost.con);
	tsm_screen_unref(lost.con);
	assert(image_budget_screens() == 0);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
}

/*
 * terminal_new's vte and its place in the budget, terminal_vte_new. A step
 * that fails after the terminal joined the budget leaves nothing: the
 * terminal leaves the budget before its vte goes, since leaving turns sixel
 * off on the vte (ASan reports the other order as a use-after-free). The
 * unwind of a step after terminal_vte_new is image_budget_free_terminal.
 */
static void test_terminal_vte(void)
{
	struct kmscon_conf_t conf = {
		.sixel = true,
		.kitty_graphics = true,
		.max_graphic_size = {1000, 1000},
		.num_color_registers = 256,
		.grapheme_width = TSM_WIDTH_UNICODE,
	};
	struct image_budget_terminal t = {0};
	struct tsm_screen *con;
	struct tsm_vte *vte = NULL;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);

	/* the width method, the last step, refused */
	conf.grapheme_width = (enum tsm_width_method)99;
	assert(terminal_vte_new(&vte, &t, con, &conf, vte_write, NULL) == -EINVAL);
	assert(!vte && !t.con && image_budget_screens() == 0);

	/* made with sixel and kitty graphics on; a later step's unwind leaves nothing in the budget
	 */
	conf.grapheme_width = TSM_WIDTH_UNICODE;
	assert(terminal_vte_new(&vte, &t, con, &conf, vte_write, NULL) == 0);
	assert(t.con == con && t.vte == vte && image_budget_screens() == 1);
	assert(kitty_answers(vte) == HAVE_KITTY);
	tsm_vte_input(vte, six, sizeof(six) - 1);
	assert(image_budget_used() > 0);
	image_budget_free_terminal(&t);
	assert(image_budget_screens() == 0 && image_budget_used() == 0);
}

int main(void)
{
	fake.cache.trim = fake_trim;
	fake.cache.trimmable = fake_trimmable;
	image_budget_add_cache(&fake.cache);

	test_auto_limit();
	test_eviction_order();
#ifdef TSM_SCREEN_IMAGE_PLACED_KITTY
	test_placed_kitty();
#endif
	test_display_recount();
	test_zero();
	test_sixel();
	test_terminal();
	test_terminal_teardown();
	test_terminal_vte();

	image_budget_remove_cache(&fake.cache);
	assert(!budget.caches && image_budget_used() == 0);
	printf("test_image_budget: ok\n");
	return 0;
}
