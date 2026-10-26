/*
 * Frames presented while a program's write is still arriving.
 *
 * Usage: test_present FONT_DIR
 *
 * kmscon reads pty output 16 KB at a time and draws after a read whenever no
 * page flip is pending, so a frame larger than one read is presented half
 * written. Whatever is presented must still be the screen as libtsm holds it
 * at that moment: drawing a frame between reads may show old rows beside new
 * ones, but never a cell from neither (a missed or expired damage, a stale
 * sprite). The Braille end card of the showcase (#75) was suspected of that;
 * this test replays output of its shape and rules it out for good.
 *
 * The pipeline is tier 2's (test_pixels.c): libtsm builds the screen,
 * kmscon_text_draw() hands bbulk the frame since this display's own frame age,
 * and the real drm2d blend draws into a fake display with two buffers that
 * swap after every frame that changed, as kmscon_text_present() does on a DRM
 * display. The input is a fixed sequence of frames, each one write drawn the
 * way the showcase's painters draw: a cursor move to the start of each row
 * that changed, then every cell of the row, Braille, blocks and spaces, with
 * a foreground or background SGR only where the colour changes. Each frame is
 * fed through tsm_vte_input_bounded() in 16 KB reads, and frames are presented
 * after every read, at the end of each write only, and after reads chosen at
 * random, with the cursor hidden and shown, and with damage reported (as with
 * FB_DAMAGE_CLIPS) and not. Every presented frame must equal what a new
 * display draws for the same screen, and with damage reported, every pixel
 * that changed must lie in the frame's damage. Every configuration must
 * present frames, and every one that presents between reads must present
 * one mid-write.
 */

#include "present_harness.h"

#define FRAMES 48

static unsigned int compared;  /* presented frames compared */
static unsigned int mid_write; /* of those, presented before their write ended */

/* the first cell where @a and @b differ, as "col,row", or NULL if none does */
static const char *first_diff(const uint32_t *a, const uint32_t *b, char *buf, size_t size)
{
	for (size_t i = 0; i < (size_t)fb_w * fb_h; i++) {
		if (a[i] != b[i]) {
			snprintf(buf, size, "%zu,%zu", i % fb_w / font->width,
				 i / fb_w / font->height);
			return buf;
		}
	}
	return NULL;
}

/* every pixel that differs between @before and @after lies in @o's damage */
static const char *outside_damage(const struct output *o, const uint32_t *before,
				  const uint32_t *after, char *buf, size_t size)
{
	for (unsigned int y = 0; y < fb_h; y++) {
		for (unsigned int x = 0; x < fb_w; x++) {
			size_t i;

			if (before[y * fb_w + x] == after[y * fb_w + x])
				continue;
			for (i = 0; i < o->n_rects; i++) {
				const struct video_rect *r = &o->rects[i];

				if (x >= r->x1 && x < r->x2 && y >= r->y1 && y < r->y2)
					break;
			}
			if (i == o->n_rects) {
				snprintf(buf, size, "%u,%u", x / font->width, y / font->height);
				return buf;
			}
		}
	}
	return NULL;
}

/* ---- the runs ---- */

enum when {
	EVERY_READ, /* after every read, as when no flip is ever pending */
	WRITE_END,  /* once each write has been read whole */
	RANDOM,	    /* after some reads, as flips happen to complete */
};

static const char *const when_names[] = {"every read", "write end", "random reads"};

static void run(enum when when, bool cursor, bool damage)
{
	static char buf[1 << 20];
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct output o;
	uint32_t *fresh_px = malloc(fb_bytes), *before = malloc(fb_bytes);
	char what[160], where[32];
	unsigned int run_compared = 0, run_mid_write = 0;
	int ret;

	assert(fresh_px && before);
	snprintf(what, sizeof(what), "cursor %s, present after %s, damage %s",
		 cursor ? "shown" : "hidden", when_names[when], damage ? "reported" : "unreported");
	ret = tsm_screen_new(&con, NULL, NULL);
	assert(ret == 0);
	ret = tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL);
	assert(ret == 0);
	tsm_vte_set_palette(vte, "default");
	ret = tsm_screen_resize(con, COLS, ROWS);
	assert(ret == 0);
	if (!cursor)
		tsm_vte_input(vte, ESC "[?25l", 6);
	output_init(&o, damage);
	rnd_state = 1;

	for (unsigned int frame = 0; frame < FRAMES && !failures; frame++) {
		size_t len = frame_bytes(buf, sizeof(buf), frame);

		for (size_t off = 0; off < len && !failures;) {
			size_t n = len - off < NREAD ? len - off : NREAD;
			bool last;

			off += tsm_vte_input_bounded(vte, buf + off, n);
			last = off >= len;
			if (when == WRITE_END && !last)
				continue;
			if (when == RANDOM && rnd() % 2)
				continue;
			memcpy(before, on_screen(&o), fb_bytes);
			if (!output_frame(&o, con, vte))
				continue;

			struct output f;

			output_init(&f, false);
			output_frame(&f, con, vte);
			memcpy(fresh_px, on_screen(&f), fb_bytes);
			output_destroy(&f);

			run_compared++;
			run_mid_write += !last;
			if (first_diff(on_screen(&o), fresh_px, where, sizeof(where))) {
				fprintf(stderr,
					"test_present: %s: frame %u, byte %zu of %zu: the "
					"presented "
					"frame differs from a fresh draw first at cell %s\n",
					what, frame, off, len, where);
				failures++;
			} else if (damage && outside_damage(&o, before, on_screen(&o), where,
							    sizeof(where))) {
				fprintf(stderr,
					"test_present: %s: frame %u, byte %zu of %zu: cell %s "
					"changed "
					"outside the reported damage\n",
					what, frame, off, len, where);
				failures++;
			}
		}
	}

	/*
	 * Each configuration judges something or fails: a renderer that stops
	 * presenting, in one configuration only, leaves it nothing to compare,
	 * and a schedule that presents between reads must have done so.
	 */
	if (!failures && !run_compared) {
		fprintf(stderr, "test_present: %s: no frame was presented\n", what);
		failures++;
	} else if (!failures && when != WRITE_END && !run_mid_write) {
		fprintf(stderr, "test_present: %s: no frame was presented mid-write\n", what);
		failures++;
	}
	compared += run_compared;
	mid_write += run_mid_write;

	output_destroy(&o);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	free(fresh_px);
	free(before);
}

/* draws @con on @o and presents it, as terminal.c does; returns the present's result */
static int draw_present(struct output *o, struct tsm_screen *con, struct tsm_vte *vte)
{
	struct tsm_screen_attr attr;

	tsm_vte_get_def_attr(vte, &attr);
	assert(kmscon_text_prepare(o->txt, &attr, false) == 0);
	assert(kmscon_text_draw(o->txt, con, false) == 0);
	return kmscon_text_present(o->txt);
}

static void feed(struct tsm_vte *vte, const char *s)
{
	tsm_vte_input(vte, s, strlen(s));
}

/*
 * kmscon_text_changed (#139, #152): false right after a draw, and true after
 * each thing that changes what the display would draw from the screen, until
 * the draw that shows it: a cell written, the cursor moved, its style
 * (DECSCUSR), a sixel placed, kmscon_text_redraw_all, another screen, a
 * present whose swap failed, and a picture left out for the image budget once
 * the budget has grown.
 */
static void test_changed(void)
{
	struct tsm_screen *con, *other;
	struct tsm_vte *vte;
	struct output o;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, COLS, ROWS) == 0);
	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);
	/* twice the font's cell, so bbulk scales the sixel into a picture */
	assert(tsm_screen_set_cell_size(con, font->width * 2, font->height * 2) == 0);
	tsm_screen_set_image_limit(con, 64 << 20);
	tsm_vte_set_sixel(vte, true);
	output_init(&o, false);

	assert(kmscon_text_changed(o.txt, con));
	assert(draw_present(&o, con, vte) == 0);
	assert(!kmscon_text_changed(o.txt, con));

	feed(vte, "x");
	assert(kmscon_text_changed(o.txt, con));
	assert(draw_present(&o, con, vte) == 0);
	assert(!kmscon_text_changed(o.txt, con));

	feed(vte, ESC "[5;5H");
	assert(kmscon_text_changed(o.txt, con));
	assert(draw_present(&o, con, vte) == 0);
	assert(!kmscon_text_changed(o.txt, con));

	feed(vte, ESC "[4 q");
	assert(kmscon_text_changed(o.txt, con));
	assert(draw_present(&o, con, vte) == 0);
	assert(!kmscon_text_changed(o.txt, con));

	kmscon_text_redraw_all(o.txt);
	assert(kmscon_text_changed(o.txt, con));
	assert(draw_present(&o, con, vte) == 0);
	assert(!kmscon_text_changed(o.txt, con));

	/* another screen, as after a VT's screen is swapped for another */
	assert(tsm_screen_new(&other, NULL, NULL) == 0);
	assert(tsm_screen_resize(other, COLS, ROWS) == 0);
	assert(kmscon_text_changed(o.txt, other));
	tsm_screen_unref(other);

	/* drawn but not shown: the swap failed, and no flip will show it */
	feed(vte, "y");
	swap_ret = -EBUSY;
	assert(draw_present(&o, con, vte) == -EBUSY);
	swap_ret = 0;
	assert(kmscon_text_changed(o.txt, con));
	assert(draw_present(&o, con, vte) == 0);
	assert(!kmscon_text_changed(o.txt, con));

#ifdef BUILD_ENABLE_IMAGES
	/*
	 * A sixel of 8x8 cells of the screen's size, scaled into a picture of
	 * four times its pixels: with no room in the budget, it is left out;
	 * the budget grows, and the screen has not changed
	 */
	image_budget_set_limit(0);
	feed(vte, ESC "[1;1H" ESC "P0;1q\"1;1;160;160#1;2;100;0;0#1!160~-!160~-!160~-"
		      "!160~-!160~-!160~-!160~-!160~-!160~-!160~-!160~-!160~-!160~-!160~-"
		      "!160~-!160~-!160~-!160~-!160~-!160~-!160~-!160~-!160~-!160~-!160~-"
		      "!160~-!160~~" ESC "\\");
	assert(kmscon_text_changed(o.txt, con));
	assert(draw_present(&o, con, vte) == 0);
	assert(((struct bbulk *)o.txt->data)->starving);
	assert(!kmscon_text_changed(o.txt, con));
	image_budget_set_limit(64 << 20);
	assert(kmscon_text_changed(o.txt, con));
	assert(draw_present(&o, con, vte) == 0);
	assert(!((struct bbulk *)o.txt->data)->starving);
	assert(!kmscon_text_changed(o.txt, con));
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
#endif

	output_destroy(&o);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

int main(int argc, char **argv)
{
	int ret = harness_init(argc, argv, "test_present");

	if (ret)
		return ret;

	test_changed();
	for (int when = EVERY_READ; when <= RANDOM; when++)
		for (int cursor = 0; cursor < 2; cursor++)
			for (int damage = 0; damage < 2; damage++)
				run(when, cursor, damage);

	kmscon_font_unref(font);
	fprintf(stderr,
		"test_present: %u presented frames compared, %u of them mid-write, %d failures\n",
		compared, mid_write, failures);
	return failures ? 1 : 0;
}
