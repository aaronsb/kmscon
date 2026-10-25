/*
 * Lightweight test for kmscon_text_set / kmscon_text_unset, for one console
 * drawn on two displays: each display's frame marks the cells changed since
 * that display's own last frame, and for a set that fails: the text's frames
 * try it again, once a second on text.c's clock, which the test stands in for.
 * We avoid linking the whole tree by stubbing external deps.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

static uint64_t fake_now_ns;
#define KMSCON_TEXT_NOW_NS() fake_now_ns

#include "../src/render/text.c" /* pull in kmscon_text_set without changing meson */
#include "video/video.h"

/* --- Stubs for external functions used by kmscon_text_set --- */
static int font_refs; /* the references the text holds */
void kmscon_font_ref(struct kmscon_font *font)
{
	font_refs++;
}
void kmscon_font_unref(struct kmscon_font *font)
{
	font_refs--;
}
struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, struct kmscon_font_attr *attr,
					const uint32_t *cps, size_t len, unsigned int cwidth)
{
	return NULL;
}
void display_ref(struct display *disp) {}
void display_unref(struct display *disp) {}
int display_swap(struct display *disp)
{
	return 0;
}
bool display_need_commit(struct display *disp)
{
	return false;
}
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

/*
 * A renderer whose next @fail_sets sets fail after giving the text its
 * largest size, as gltex_set() does, and that keeps the size it is given;
 * its resize and its rotation read the font's cell width, as bbulk_resize()
 * and gltex_rotate()'s set do, so neither may run on a text with no font.
 * @set_orientation: the orientation its last set was given.
 */
static int fail_sets;
static int failing_set_calls;
static enum Orientation set_orientation;
static int failing_set(struct kmscon_text *txt)
{
	failing_set_calls++;
	set_orientation = txt->orientation;
	txt->max_cols = COLS;
	txt->max_rows = ROWS;
	txt->cols = COLS;
	txt->rows = ROWS;
	if (fail_sets) {
		fail_sets--;
		return -EIO;
	}
	return 0;
}
static void keep_size(struct kmscon_text *txt, unsigned int cols, unsigned int rows)
{
	assert(txt->font && FONT_WIDTH(txt));
	txt->cols = cols;
	txt->rows = rows;
}
static int failing_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	assert(txt->font && FONT_WIDTH(txt));
	txt->orientation = orientation;
	return failing_set(txt);
}

static struct kmscon_text_ops failing_ops = {
	.name = "failingtest",
	.set = failing_set,
	.resize = keep_size,
	.rotate = failing_rotate,
	.draw = record_draw,
};

/*
 * A set that fails leaves a text that draws nothing (#206): its frames try
 * the set again with the font it was given, not within a second of the last
 * try, until one succeeds; that frame draws every cell at the size the
 * terminal gave it since, in the orientation given since. A resize or a
 * rotation meanwhile only records what it was given: the renderer has no font
 * to compute with. An unset, or another set, gives the retry up, and the text
 * holds no reference to the font it no longer tries.
 */
static void test_set_again(void)
{
	struct kmscon_text txt;
	struct kmscon_font font = {.width = 8}, other = {.width = 8};
	struct tsm_screen_attr attr = {0};
	struct tsm_screen *con;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, COLS, ROWS) == 0);
	memset(&txt, 0, sizeof(txt));
	txt.ops = &failing_ops;
	txt.disp = (struct display *)0x1;
	failing_set_calls = 0;
	font_refs = 0;
	fake_now_ns = 5000000000ULL;

	fail_sets = 2;
	assert(kmscon_text_set(&txt, &font) == -EIO);
	assert(!txt.font && failing_set_calls == 1);
	kmscon_text_resize(&txt, COLS - 2, ROWS - 1);
	assert(kmscon_text_rotate(&txt, OR_RIGHT) == 0);
	assert(failing_set_calls == 1 && txt.orientation == OR_RIGHT);

	/* within a second of the failure: no frame, no try */
	fake_now_ns += KMSCON_TEXT_SET_RETRY_NS - 1;
	assert(kmscon_text_prepare(&txt, &attr, false) == -EINVAL);
	assert(failing_set_calls == 1 && !txt.rendering);

	/* a second on, a try, which fails, and none within a second of it */
	fake_now_ns += 1;
	assert(kmscon_text_prepare(&txt, &attr, false) == -EINVAL);
	assert(failing_set_calls == 2 && !txt.font);
	fake_now_ns += KMSCON_TEXT_SET_RETRY_NS / 2;
	assert(kmscon_text_prepare(&txt, &attr, false) == -EINVAL);
	assert(failing_set_calls == 2);

	/* the next try succeeds, and its frame draws every cell at the terminal's size */
	fake_now_ns += KMSCON_TEXT_SET_RETRY_NS / 2;
	assert(kmscon_text_prepare(&txt, &attr, false) == 0);
	assert(failing_set_calls == 3 && txt.font == &font && font_refs == 1);
	assert(txt.cols == COLS - 2 && txt.rows == ROWS - 1 && set_orientation == OR_RIGHT);
	draw_full(&txt, con);
	kmscon_text_unset(&txt);
	assert(font_refs == 0);

	/* an unset gives the retry up */
	fail_sets = 1;
	assert(kmscon_text_set(&txt, &font) == -EIO);
	kmscon_text_unset(&txt);
	assert(font_refs == 0);
	fake_now_ns += 10 * KMSCON_TEXT_SET_RETRY_NS;
	assert(kmscon_text_prepare(&txt, &attr, false) == -EINVAL);
	assert(failing_set_calls == 4);

	/* so does another set, which, failing too, is tried in its place */
	fail_sets = 2;
	assert(kmscon_text_set(&txt, &font) == -EIO);
	assert(kmscon_text_set(&txt, &other) == -EIO);
	assert(font_refs == 1);
	fake_now_ns += KMSCON_TEXT_SET_RETRY_NS;
	assert(kmscon_text_prepare(&txt, &attr, false) == 0);
	assert(failing_set_calls == 7 && txt.font == &other && font_refs == 1);
	kmscon_text_unset(&txt);
	assert(font_refs == 0);

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
	test_set_again();
	return 0;
}
