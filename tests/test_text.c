/*
 * Lightweight test for kmscon_text_set / kmscon_text_unset, and for one
 * console drawn on two displays: each display's frame marks the cells changed
 * since that display's own last frame.
 * We avoid linking the whole tree by stubbing external deps.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "../src/render/text.c" /* pull in kmscon_text_set without changing meson */
#include "video/video.h"

/* --- Stubs for external functions used by kmscon_text_set --- */
void kmscon_font_ref(struct kmscon_font *font) {}
void kmscon_font_unref(struct kmscon_font *font) {}
void display_ref(struct display *disp) {}
void display_unref(struct display *disp) {}
unsigned int display_get_height(struct display *disp)
{
	return 0;
}
unsigned int display_get_width(struct display *disp)
{
	return 0;
}

static int dummy_set_calls;
static int dummy_unset_calls;
static int dummy_set(struct kmscon_text *txt)
{
	dummy_set_calls++;
	return 0;
}
static void dummy_unset(struct kmscon_text *txt)
{
	dummy_unset_calls++;
}

static struct kmscon_text_ops dummy_ops = {
	.name = "dummytest",
	.owner = NULL,
	.init = NULL,
	.destroy = NULL,
	.set = dummy_set,
	.unset = dummy_unset,
};

#define COLS 10
#define ROWS 4

/* the cells the last frame drawn marked changed */
static bool changed[ROWS][COLS];
static unsigned int n_changed;
static bool fail_draw;

static int record_draw(struct kmscon_text *txt, const struct tsm_frame *frame, bool cursor_on)
{
	unsigned int x, y;

	n_changed = 0;
	for (y = 0; y < ROWS; y++) {
		for (x = 0; x < COLS; x++) {
			changed[y][x] = tsm_frame_cell_at(frame, x, y)->flags & TSM_FRAME_CHANGED;
			n_changed += changed[y][x];
		}
	}
	return fail_draw ? -EIO : 0;
}

static struct kmscon_text_ops record_ops = {
	.name = "recordtest",
	.draw = record_draw,
};

static void init_display(struct kmscon_text *txt)
{
	memset(txt, 0, sizeof(*txt));
	txt->ops = &record_ops;
	txt->max_cols = COLS;
	txt->max_rows = ROWS;
}

static void put(struct tsm_screen *con, unsigned int x, unsigned int y, char c)
{
	struct tsm_screen_attr attr = {0};

	tsm_screen_move_to(con, x, y);
	tsm_screen_write(con, c, &attr);
}

/* @txt's frame of @con marks exactly the cells given as x, y pairs */
static void draw_expect(struct kmscon_text *txt, struct tsm_screen *con, unsigned int n, ...)
{
	va_list ap;
	unsigned int i, x, y;

	assert(kmscon_text_draw(txt, con, false) == 0);
	assert(n_changed == n);
	va_start(ap, n);
	for (i = 0; i < n; i++) {
		x = va_arg(ap, unsigned int);
		y = va_arg(ap, unsigned int);
		assert(changed[y][x]);
	}
	va_end(ap);
}

static void draw_full(struct kmscon_text *txt, struct tsm_screen *con)
{
	assert(kmscon_text_draw(txt, con, false) == 0);
	assert(n_changed == COLS * ROWS);
}

static void test_two_displays(void)
{
	struct kmscon_text a, b;
	struct tsm_screen *con, *other;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, COLS, ROWS) == 0);
	tsm_screen_set_flags(con, TSM_SCREEN_HIDE_CURSOR);
	assert(tsm_screen_new(&other, NULL, NULL) == 0);
	assert(tsm_screen_resize(other, COLS, ROWS) == 0);
	init_display(&a);
	init_display(&b);

	/* each display's first frame is full, then nothing has changed */
	draw_full(&a, con);
	draw_full(&b, con);
	draw_expect(&a, con, 0);
	draw_expect(&b, con, 0);

	/* a change reaches both displays, whichever draws first */
	put(con, 2, 1, 'x');
	draw_expect(&a, con, 1, 2, 1);
	draw_expect(&b, con, 1, 2, 1);
	draw_expect(&a, con, 0);
	draw_expect(&b, con, 0);

	/* changes between the two displays' frames */
	put(con, 4, 2, 'y');
	draw_expect(&a, con, 1, 4, 2);
	put(con, 5, 2, 'z');
	draw_expect(&b, con, 2, 4, 2, 5, 2);
	draw_expect(&a, con, 1, 5, 2);
	draw_expect(&b, con, 0);

	/* a resize, a rotation and a failed draw redraw that display only */
	kmscon_text_resize(&a, COLS, ROWS);
	draw_full(&a, con);
	draw_expect(&b, con, 0);
	kmscon_text_rotate(&b, OR_NORMAL);
	draw_full(&b, con);
	draw_expect(&a, con, 0);
	fail_draw = true;
	assert(kmscon_text_draw(&a, con, false) == -EIO);
	fail_draw = false;
	draw_full(&a, con);
	draw_expect(&b, con, 0);

	/* another console, and back, is drawn whole */
	draw_full(&a, other);
	draw_full(&a, con);
	draw_expect(&b, con, 0);

	tsm_screen_unref(other);
	tsm_screen_unref(con);
}

int main(void)
{
	struct kmscon_text txt = {0};
	struct kmscon_font fake_font = {0};
	int ret;

	txt.ops = &dummy_ops;

	/* set calls backend set */
	ret = kmscon_text_set(&txt, &fake_font);
	assert(ret == 0);
	assert(dummy_set_calls == 1);
	assert(txt.font == &fake_font);

	/* unset calls backend unset and clears pointers */
	kmscon_text_unset(&txt);
	assert(dummy_unset_calls == 1);
	assert(txt.font == NULL);
	assert(txt.disp == NULL);

	/* NULL font must return -EINVAL */
	ret = kmscon_text_set(&txt, NULL);
	assert(ret == -EINVAL);
	assert(dummy_set_calls == 1); /* not called again */

	test_two_displays();
	return 0;
}
