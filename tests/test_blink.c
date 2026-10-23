/*
 * Blinking text (SGR 5) through bbulk on a double-buffered display.
 *
 * Usage: test_blink FONT_DIR
 *
 * The pipeline is tier 2's (test_pixels.c): libtsm builds the screen,
 * kmscon_text_draw() hands bbulk the frame since this display's own frame age,
 * and the real drm2d blend draws into a fake display. Here the display has two
 * buffers that swap after every frame, as a DRM display does, so each frame
 * is drawn into a buffer that holds the frame before last. bbulk redraws only
 * the cells it thinks changed, and every presented frame must equal a fresh
 * render of the same screen in the same blink phase. A second display draws
 * the same console on some frames only, skipping frames that change the
 * screen, so its own frame age spans those changes. Each run is drawn again
 * with damage reporting on, as on a plane with FB_DAMAGE_CLIPS: every pixel
 * that differs from the frame on screen must lie in a reported rect, and a
 * display's frame that follows one of its frames with no change, and brings
 * none itself, must report no damage. A scripted run of single-cell writes and
 * blink flips then holds each frame's damage to exactly the cells changed on
 * that frame or the one before, the two frames its buffer is behind: damage
 * that outlives them (#67) fails, and so does a change left out. The run
 * prints how many cells it damaged.
 *
 * What a phase looks like is judged against plain screens: in the visible
 * phase the blinking screen equals the same text without SGR 5, and in the
 * hidden phase it equals the same attributes over spaces, so a blinking cell
 * keeps its background and its underline and strike and loses its glyph.
 * That follows foot, whose render_cell draws a cell's underline and strike
 * whatever its blink phase (it dims the glyph where kmscon hides it), and
 * upstream kmscon, which draws a hidden cell as a space in its attributes.
 */

#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <libtsm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/font/font.c"
#include "../src/font/font_box.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_freetype.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_colour.c"
#undef LOG_SUBSYSTEM
#include "../src/render/text.c"
#undef LOG_SUBSYSTEM
#include "../src/render/bbulk.c"
#include "../src/render/image_budget.c"
#include "../src/render/picture.c"
#undef LOG_SUBSYSTEM
#include "../src/render/deco.c"
#undef LOG_SUBSYSTEM
#include "../src/video/drm2d_render.c"
#include "config.h"

void shl_module_ref(struct shl_module *module) {}
void shl_module_unref(struct shl_module *module) {}

#define COLS 40
#define ROWS 10
#define FONT_PX 16
#define ESC "\x1b"

static unsigned int fb_w, fb_h;
static size_t fb_bytes;

/* ---- double-buffered in-memory display ---- */

unsigned int display_get_width(struct display *disp)
{
	return fb_w;
}
unsigned int display_get_height(struct display *disp)
{
	return fb_h;
}
void display_ref(struct display *disp) {}
void display_unref(struct display *disp) {}
bool display_need_redraw(struct display *disp)
{
	return false;
}
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y) {}

int display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	return drm2d_display_clear(disp, r, g, b);
}

int display_blend(struct display *disp, const struct video_blend_req *req)
{
	return drm2d_display_blend(disp, req);
}

/*
 * One display: drm2d draws into rb[current_rb ^ 1], and a swap flips
 * current_rb, so the next frame lands in the buffer drawn two frames ago.
 */
struct output {
	const char *name;
	struct display disp;
	struct drm2d_display d2d;
	struct kmscon_text *txt;
	unsigned int frames;
	unsigned int swaps;
	bool damage;			      /* reports damage, as with FB_DAMAGE_CLIPS */
	struct video_rect rects[COLS * ROWS]; /* the damage of the last frame */
	size_t n_rects;
	const unsigned int *pointer; /* the next frame draws the pointer here, if set */
	/* the hardware cursor, which only a swap puts on screen, as on drm */
	bool cursor_visible;
	int32_t cursor_x, cursor_y;
	bool need_commit;
};

static struct output *output_of(struct display *disp)
{
	return (struct output *)((char *)disp - offsetof(struct output, disp));
}

bool display_supports_damage(struct display *disp)
{
	return output_of(disp)->damage;
}

int display_swap(struct display *disp)
{
	struct output *o = output_of(disp);

	o->d2d.current_rb ^= 1;
	o->swaps++;
	o->need_commit = false;
	return 0;
}

bool display_need_commit(struct display *disp)
{
	return output_of(disp)->need_commit;
}

/* drm_display_show_cursor and drm_display_hide_cursor: the state waits for a swap */
int display_show_cursor(struct display *disp, int32_t x, int32_t y)
{
	struct output *o = output_of(disp);

	if (!o->cursor_visible || o->cursor_x != x || o->cursor_y != y)
		o->need_commit = true;
	o->cursor_visible = true;
	o->cursor_x = x;
	o->cursor_y = y;
	return 0;
}

int display_hide_cursor(struct display *disp)
{
	struct output *o = output_of(disp);

	if (o->cursor_visible)
		o->need_commit = true;
	o->cursor_visible = false;
	return 0;
}

void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages)
{
	struct output *o = output_of(disp);

	assert(n_rect <= sizeof(o->rects) / sizeof(o->rects[0]));
	memcpy(o->rects, damages, n_rect * sizeof(*damages));
	o->n_rects = n_rect;
}

static int failures;
static unsigned int compared; /* screens compared */

static void output_init(struct output *o, const char *name, struct kmscon_font *font, bool damage)
{
	int ret;

	memset(o, 0, sizeof(*o));
	o->name = name;
	o->damage = damage;
	o->disp.width = fb_w;
	o->disp.height = fb_h;
	o->disp.data = &o->d2d;
	for (int i = 0; i < 2; i++) {
		o->d2d.rb[i].map = calloc(1, fb_bytes);
		o->d2d.rb[i].stride = fb_w * 4;
		assert(o->d2d.rb[i].map);
	}
	ret = kmscon_text_new(&o->txt, "bbulk", OR_NORMAL, &o->disp);
	assert(ret == 0);
	ret = kmscon_text_set(o->txt, font);
	assert(ret == 0);
	assert(o->txt->cols == COLS && o->txt->rows == ROWS);
}

static void output_destroy(struct output *o)
{
	kmscon_text_unref(o->txt);
	free(o->d2d.rb[0].map);
	free(o->d2d.rb[1].map);
}

/* every pixel of @drawn that differs from @shown lies in @o's damage */
static void check_damage(struct output *o, const uint32_t *drawn, const uint32_t *shown,
			 const char *what)
{
	compared++;
	for (unsigned int y = 0; y < fb_h; y++) {
		for (unsigned int x = 0; x < fb_w; x++) {
			size_t i;

			if (drawn[y * fb_w + x] == shown[y * fb_w + x])
				continue;
			for (i = 0; i < o->n_rects; i++) {
				const struct video_rect *r = &o->rects[i];

				if (x >= r->x1 && x < r->x2 && y >= r->y1 && y < r->y2)
					break;
			}
			if (i < o->n_rects)
				continue;
			fprintf(stderr,
				"test_blink: %s: cell %u,%u changed outside the reported damage\n",
				what, x / FONT_WIDTH(o->txt), y / FONT_HEIGHT(o->txt));
			failures++;
			return;
		}
	}
}

/* the cells @o's damage covers */
static void damage_cells(const struct output *o, bool cells[ROWS][COLS])
{
	unsigned int fw = FONT_WIDTH(o->txt), fh = FONT_HEIGHT(o->txt);

	memset(cells, 0, sizeof(bool) * ROWS * COLS);
	for (size_t i = 0; i < o->n_rects; i++) {
		const struct video_rect *r = &o->rects[i];

		for (unsigned int row = r->y1 / fh; row * fh < (unsigned int)r->y2; row++)
			for (unsigned int col = r->x1 / fw; col * fw < (unsigned int)r->x2; col++)
				cells[row][col] = true;
	}
}

/*
 * Draw and present one frame of @con in phase @hidden, as terminal.c does;
 * returns the buffer on screen after it. With damage on, checks the damage
 * against the frame on screen before it.
 */
static const uint32_t *output_frame(struct output *o, struct tsm_screen *con, struct tsm_vte *vte,
				    bool hidden, const char *what)
{
	struct tsm_screen_attr attr;
	const uint32_t *drawn, *shown;
	int ret;

	drawn = o->d2d.rb[o->d2d.current_rb ^ 1].map;
	shown = o->d2d.rb[o->d2d.current_rb].map;
	tsm_vte_get_def_attr(vte, &attr);
	ret = kmscon_text_prepare(o->txt, &attr, hidden);
	assert(ret == 0);
	ret = kmscon_text_draw(o->txt, con, false);
	assert(ret == 0);
	if (o->pointer) {
		ret = kmscon_text_draw_pointer(o->txt, o->pointer[0], o->pointer[1]);
		assert(ret == 0);
	}
	ret = kmscon_text_present(o->txt);
	assert(ret == 0);
	if (o->damage && o->frames)
		check_damage(o, drawn, shown, what);
	o->frames++;
	return o->d2d.rb[o->d2d.current_rb].map;
}

/* ---- the screen ---- */

/*
 * A segment of the screen: text drawn with SGR @sgr, blinking when @blink;
 * @cells is the text's width in cells. A segment with no text ends the row.
 */
struct seg {
	const char *sgr;
	const char *text;
	unsigned int cells;
	bool blink;
};

#define ROW_END {NULL, NULL, 0, false}
#define COMB_ACUTE "\xcc\x81"
#define COMB_TILDE "\xcc\x83"
#define COMB_GRAVE "\xcc\x80"

static const struct seg segs[] = {
	{"", "plain ", 6, false},
	{"", "blink", 5, true},
	{"", " steady", 7, false},
	ROW_END,
	{"31", "red ", 4, false},
	{"31", "red", 3, true},
	{"", " ", 1, false},
	{"33;44", "on blue", 7, true},
	{"33;44", " not", 4, false},
	ROW_END,
	{"7", "rev ", 4, false},
	{"7", "reversed", 8, true},
	{"", " ", 1, false},
	{"1", "bold", 4, true},
	ROW_END,
	{"4", "under ", 6, false},
	{"4", "under", 5, true},
	{"", " ", 1, false},
	{"4:3;58:5:196", "curly", 5, true},
	{"4:2", " dbl", 4, false},
	ROW_END,
	{"9", "struck ", 7, false},
	{"9", "struck", 6, true},
	{"", " ", 1, false},
	{"9;4;32", "both", 4, true},
	ROW_END,
	{"", "wide ", 5, false},
	{"", "日本", 4, true},
	{"", " 日本 ", 6, false},
	{"7;4", "日本", 4, true},
	ROW_END,
	{"", "comb ", 5, false},
	{"", "e" COMB_ACUTE " a" COMB_TILDE COMB_GRAVE, 3, true},
	{"", " e" COMB_ACUTE " a" COMB_TILDE COMB_GRAVE, 4, false},
	ROW_END,
	{"", "ab", 2, true},
	{"5;25", "cd", 2, false},
	{"", " SGR 25 ends it", 15, false},
	ROW_END,
};

#define N_SEGS (sizeof(segs) / sizeof(segs[0]))
#define ROWS_USED 8 /* the rows segs fills */

enum variant {
	BLINKING, /* the screen as it is: the blinking segments with SGR 5 */
	VISIBLE,  /* without SGR 5: the visible phase */
	HIDDEN,	  /* without SGR 5, the blinking segments as spaces: the hidden phase */
};

/* the bytes of the screen, as @v */
static void screen_bytes(char *out, size_t size, enum variant v)
{
	size_t n = 0;

	out[0] = 0;
	for (size_t i = 0; i < N_SEGS; i++) {
		const struct seg *s = &segs[i];

		if (!s->text) {
			n += snprintf(out + n, size - n, ESC "[0m\r\n");
			continue;
		}
		n += snprintf(out + n, size - n, ESC "[0%s%s%s", *s->sgr ? ";" : "", s->sgr,
			      v == BLINKING && s->blink ? ";5m" : "m");
		if (v == HIDDEN && s->blink)
			for (unsigned int c = 0; c < s->cells; c++)
				n += snprintf(out + n, size - n, " ");
		else
			n += snprintf(out + n, size - n, "%s", s->text);
		assert(n < size);
	}
}

/* the cells of the blinking segments, row by row */
static bool blink_cell[ROWS][COLS];

static void map_blink_cells(void)
{
	unsigned int row = 0, col = 0;

	for (size_t i = 0; i < N_SEGS; i++) {
		if (!segs[i].text) {
			row++;
			col = 0;
			continue;
		}
		for (unsigned int c = 0; c < segs[i].cells; c++)
			blink_cell[row][col + c] = segs[i].blink;
		col += segs[i].cells;
		assert(col <= COLS && row < ROWS_USED);
	}
	assert(row == ROWS_USED);
}

/* shows the cursor on the "b" of "blink" in the first row */
#define CURSOR_BYTES ESC "[?25h" ESC "[1;7H"

/* rewrites the last row's "ab" with SGR 5 then 25, so it stops blinking */
#define SGR25_BYTES ESC "[8;1H" ESC "[0;5;25mab" ESC "[0m"

/* ---- fresh renders: what a frame must look like ---- */

static struct kmscon_font *font;
static const char *font_dir;

static void vte_write(struct tsm_vte *vte, const char *u8, size_t len, void *data) {}

static void console_new(struct tsm_screen **con, struct tsm_vte **vte)
{
	int ret;

	ret = tsm_screen_new(con, NULL, NULL);
	assert(ret == 0);
	ret = tsm_vte_new(vte, *con, vte_write, NULL, NULL, NULL);
	assert(ret == 0);
	tsm_vte_set_palette(*vte, "default");
	ret = tsm_screen_resize(*con, COLS, ROWS);
	assert(ret == 0);
	/* hidden cursor: it would cover a cell, and its blink is not judged here */
	tsm_vte_input(*vte, ESC "[?25l", 6);
}

/*
 * @bytes drawn by a new display in phase @hidden, with the pointer at
 * @pointer if set, into @out: its first frame, every cell drawn over a cleared
 * buffer.
 */
static void fresh(uint32_t *out, const char *bytes, bool hidden, const unsigned int *pointer)
{
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct output o;
	const uint32_t *drawn;

	console_new(&con, &vte);
	tsm_vte_input(vte, bytes, strlen(bytes));
	output_init(&o, "fresh", font, false);
	o.pointer = pointer;
	drawn = output_frame(&o, con, vte, hidden, "fresh");
	memcpy(out, drawn, fb_bytes);
	output_destroy(&o);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

/* whether @a and @b hold the same screen; if not, report the first cell that differs */
static bool same(const uint32_t *a, const uint32_t *b, const char *what)
{
	compared++;
	for (unsigned int y = 0; y < fb_h; y++) {
		for (unsigned int x = 0; x < fb_w; x++) {
			if (a[y * fb_w + x] == b[y * fb_w + x])
				continue;
			fprintf(stderr,
				"test_blink: %s: differs first at cell %u,%u (pixel %u,%u)\n", what,
				x / font->width, y / font->height, x, y);
			failures++;
			return false;
		}
	}
	return true;
}

/* whether cells @col, @row to @col + @cols - 1 of @a and @b are the same */
static bool same_cells(const uint32_t *a, const uint32_t *b, unsigned int col, unsigned int row,
		       unsigned int cols)
{
	for (unsigned int y = row * font->height; y < (row + 1) * font->height; y++) {
		for (unsigned int x = col * font->width; x < (col + cols) * font->width; x++) {
			if (a[y * fb_w + x] != b[y * fb_w + x])
				return false;
		}
	}
	return true;
}

/* ---- what each phase looks like ---- */

static void test_phases(void)
{
	static char blinking[4096], visible[4096], hidden[4096];
	uint32_t *on = malloc(fb_bytes), *off = malloc(fb_bytes), *ref = malloc(fb_bytes);
	static bool reported[ROWS][COLS];
	unsigned int changed = 0;

	screen_bytes(blinking, sizeof(blinking), BLINKING);
	screen_bytes(visible, sizeof(visible), VISIBLE);
	screen_bytes(hidden, sizeof(hidden), HIDDEN);

	fresh(on, blinking, false, NULL);
	fresh(ref, visible, false, NULL);
	same(on, ref, "visible phase against the text without SGR 5");

	fresh(off, blinking, true, NULL);
	fresh(ref, hidden, false, NULL);
	same(off, ref, "hidden phase against the attributes over spaces");

	/* the phase moves the blinking cells and nothing else */
	for (unsigned int y = 0; y < fb_h; y++) {
		for (unsigned int x = 0; x < fb_w; x++) {
			unsigned int col = x / font->width, row = y / font->height;
			bool differs = on[y * fb_w + x] != off[y * fb_w + x];

			if (differs && !blink_cell[row][col] && !reported[row][col]) {
				fprintf(stderr,
					"test_blink: steady cell %u,%u differs between the "
					"phases\n",
					col, row);
				reported[row][col] = true;
				failures++;
			}
			changed += differs;
		}
	}
	if (!changed) {
		fprintf(stderr, "test_blink: the phases draw the same screen\n");
		failures++;
	}

	/* hidden-phase rendering of a screen with no SGR 5 is the visible one */
	fresh(off, visible, true, NULL);
	fresh(ref, visible, false, NULL);
	same(off, ref, "a screen without SGR 5 in the hidden phase");

	/* "ab" rewritten with SGR 25 shows in the hidden phase; "blink" still hides */
	strncat(blinking, SGR25_BYTES, sizeof(blinking) - strlen(blinking) - 1);
	fresh(off, blinking, true, NULL);
	compared++;
	if (!same_cells(off, on, 0, ROWS_USED - 1, 2) || same_cells(off, on, 6, 0, 5)) {
		fprintf(stderr, "test_blink: SGR 25 does not end the blink of \"ab\" alone\n");
		failures++;
	}

	/* the cursor on a blinking cell shows it in the hidden phase too; the
	 * blinking cell beside it hides */
	screen_bytes(blinking, sizeof(blinking), BLINKING);
	strncat(blinking, CURSOR_BYTES, sizeof(blinking) - strlen(blinking) - 1);
	fresh(on, blinking, false, NULL);
	fresh(off, blinking, true, NULL);
	compared++;
	if (!same_cells(off, on, 6, 0, 1) || same_cells(off, on, 7, 0, 1)) {
		fprintf(stderr, "test_blink: the cursor's blinking cell hides, or its "
				"neighbour does not\n");
		failures++;
	}
	free(on);
	free(off);
	free(ref);
}

/* ---- the phases through both buffers ---- */

/*
 * One step of a run: the bytes written before the frame (or NULL) and the
 * phase it is drawn in.
 */
struct step {
	const char *bytes;
	bool hidden;
};

/*
 * Every frame each display draws must equal a fresh render of the screen as
 * it is, in the frame's phase. @mask: which of the two displays draws at each
 * step (bit 0 the first, drawing every step unless @mask says otherwise).
 * @damage: the displays report damage. A display's frame changes nothing when
 * no bytes were written since its last frame and its phase is that frame's;
 * its first two frames redraw every cell.
 */
static void run_steps(const char *what, const struct step *steps, size_t n, const char *initial,
		      const unsigned int *mask, bool damage)
{
	static char bytes[8192];
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct output outs[2];
	uint32_t *ref = malloc(fb_bytes);
	char label[256];
	/* per display: bytes written since its last frame, that frame's phase,
	 * and whether it changed anything */
	bool pending[2] = {false, false}, last_hidden[2] = {false, false};
	bool last_changed[2] = {true, true};

	console_new(&con, &vte);
	tsm_vte_input(vte, initial, strlen(initial));
	snprintf(bytes, sizeof(bytes), "%s", initial);
	output_init(&outs[0], "every frame", font, damage);
	output_init(&outs[1], "some frames", font, damage);

	for (size_t i = 0; i < n; i++) {
		unsigned int draws = mask ? mask[i] : 1;

		if (steps[i].bytes) {
			tsm_vte_input(vte, steps[i].bytes, strlen(steps[i].bytes));
			strncat(bytes, steps[i].bytes, sizeof(bytes) - strlen(bytes) - 1);
			pending[0] = pending[1] = true;
		}
		fresh(ref, bytes, steps[i].hidden, NULL);

		for (int d = 0; d < 2; d++) {
			const uint32_t *drawn;
			unsigned int swaps = outs[d].swaps;
			bool changed;

			if (!(draws & (1u << d)))
				continue;
			changed = pending[d] || steps[i].hidden != last_hidden[d] ||
				  outs[d].frames < 2;
			snprintf(label, sizeof(label),
				 "%s%s: step %zu (%s phase), display '%s' frame %u into buffer %d",
				 what, damage ? " with damage" : "", i,
				 steps[i].hidden ? "hidden" : "visible", outs[d].name,
				 outs[d].frames + 1, outs[d].d2d.current_rb ^ 1);
			drawn = output_frame(&outs[d], con, vte, steps[i].hidden, label);
			same(drawn, ref, label);
			if (damage && !changed && !last_changed[d]) {
				compared++;
				if (outs[d].n_rects || outs[d].swaps != swaps) {
					fprintf(stderr,
						"test_blink: %s: %zu damage rects and %u swaps "
						"after "
						"two frames without change\n",
						label, outs[d].n_rects, outs[d].swaps - swaps);
					failures++;
				}
			}
			pending[d] = false;
			last_hidden[d] = steps[i].hidden;
			last_changed[d] = changed;
		}
	}

	output_destroy(&outs[0]);
	output_destroy(&outs[1]);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	free(ref);
}

static void test_buffers(void)
{
	static char blinking[4096], cursor[4096];
	/*
	 * V V | H H H | V V | H | V | H | V V: a phase held for several frames,
	 * so the frame after a toggle goes into the buffer that still holds the
	 * other phase, and phases of one frame each, so each buffer sees every
	 * other phase. Steps 2 and 6 write to the last row (the second, a new
	 * blinking run), on frames the second display skips.
	 */
	static const struct step phases[] = {
		{NULL, false},
		{NULL, false},
		{ESC "[10;1H" ESC "[0msteady row", true},
		{NULL, true},
		{NULL, true},
		{NULL, false},
		{ESC "[10;14H" ESC "[0;5;31mnew blink" ESC "[0m", false},
		{NULL, true},
		{NULL, false},
		{NULL, true},
		{NULL, false},
		{NULL, false},
	};
	/* the second display misses frames: it draws steps 0, 1, 3, 4, 7, 8, 10 */
	static const unsigned int phases_some[] = {3, 3, 1, 3, 3, 1, 1, 3, 3, 1, 3, 1};
	/* SGR 25 rewrites "ab" in the hidden phase; the frames after it show it */
	static const struct step sgr25[] = {
		{NULL, false},	     {NULL, true},  {NULL, true},
		{SGR25_BYTES, true}, {NULL, true},  {NULL, true},
		{NULL, false},	     {NULL, false}, {ESC "[8;1H" ESC "[0;5mab", true},
		{NULL, true},	     {NULL, false}, {NULL, false},
	};
	/* the second display skips the two rewrites, then draws a phase flip */
	static const unsigned int sgr25_some[] = {3, 3, 3, 1, 3, 3, 3, 3, 1, 3, 3, 3};
	/* no-blink: kmscon never leaves the visible phase */
	static const struct step noblink[] = {
		{NULL, false}, {NULL, false}, {SGR25_BYTES, false}, {NULL, false}, {NULL, false},
	};
	/* the cursor on "blink" through a flip, then off it while hidden, then
	 * onto it again while hidden */
	static const struct step cursor_steps[] = {
		{NULL, false}, {NULL, false}, {NULL, true}, {ESC "[10;40H", true},
		{NULL, true},  {NULL, false}, {NULL, true}, {ESC "[1;9H", true},
		{NULL, true},  {NULL, false},
	};

	screen_bytes(blinking, sizeof(blinking), BLINKING);
	strcpy(cursor, blinking);
	strncat(cursor, CURSOR_BYTES, sizeof(cursor) - strlen(cursor) - 1);
	for (int damage = 0; damage < 2; damage++) {
		run_steps("phases", phases, sizeof(phases) / sizeof(phases[0]), blinking,
			  phases_some, damage);
		run_steps("SGR 25", sgr25, sizeof(sgr25) / sizeof(sgr25[0]), blinking, sgr25_some,
			  damage);
		run_steps("no-blink", noblink, sizeof(noblink) / sizeof(noblink[0]), blinking, NULL,
			  damage);
		run_steps("cursor", cursor_steps, sizeof(cursor_steps) / sizeof(cursor_steps[0]),
			  cursor, NULL, damage);
	}
}

/*
 * Whether @r is one row of cells that starts and ends on a cell of @want,
 * with no run of other cells inside it as long as DAMAGE_MERGE_LEN.
 */
static bool rect_fits(const struct output *o, const struct video_rect *r, bool want[ROWS][COLS])
{
	unsigned int fw = FONT_WIDTH(o->txt), fh = FONT_HEIGHT(o->txt);
	unsigned int row = r->y1 / fh, first = r->x1 / fw, last = (r->x2 - 1) / fw, run = 0;

	if (r->y1 % fh || r->y2 - r->y1 != (int)fh || r->x1 % fw || r->x2 % fw)
		return false;
	if (!want[row][first] || !want[row][last])
		return false;
	for (unsigned int col = first; col <= last; col++) {
		run = want[row][col] ? 0 : run + 1;
		if (run >= DAMAGE_MERGE_LEN)
			return false;
	}
	return true;
}

/*
 * One display with damage draws the blinking screen for EXTENT_FRAMES frames,
 * as a shell prompt would see it. One frame in three writes one cell of the
 * free rows, every fourth of them a wide character over two cells; the blink
 * phase flips every eight frames, which changes the blinking cells; a steady
 * bar cursor shows on a wide cell for a few frames, which changes both of its
 * cells; and the pointer shows for one frame, which changes the four cells
 * around its centre on that frame and the next. Each frame's damage must cover
 * exactly the cells changed on that frame or the one before, give or take
 * bbulk's merging: a rect starts and ends on such a cell, and a run of other
 * cells inside it is shorter than DAMAGE_MERGE_LEN. A frame is presented
 * (swapped) when its damage is not empty, and only then. The first three
 * frames are left out: the first two redraw every cell, so the third draws
 * every cell again into the buffer the second left behind.
 */
#define EXTENT_FRAMES 100
#define CURSOR_ON 50  /* the bar cursor shows on the first wide character */
#define CURSOR_OFF 54 /* and hides */
#define POINTER 70    /* the frame that draws the pointer */

static void test_damage_extent(void)
{
	static char bytes[8192];
	static bool now[ROWS][COLS], before[ROWS][COLS], want[ROWS][COLS], got[ROWS][COLS];
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct output o;
	uint32_t *ref = malloc(fb_bytes);
	unsigned int typed = 0, total = 0, judged = 0, most = 0, none = 0;
	unsigned int row = ROWS_USED, col = 0, wide_row = ROWS, wide_col = 0;
	unsigned int fw = font->width, fh = font->height;
	/* the pointer's centre: its cells are columns 20-21, rows 4-5, all blank */
	const unsigned int pointer[2] = {21 * fw, 5 * fh};
	bool hidden = false;
	char label[128], cell[64];

	screen_bytes(bytes, sizeof(bytes), BLINKING);
	console_new(&con, &vte);
	tsm_vte_input(vte, bytes, strlen(bytes));
	output_init(&o, "extent", font, true);

	for (unsigned int f = 0; f < EXTENT_FRAMES; f++) {
		unsigned int n = 0, swaps = o.swaps;
		bool reported = false, any = false;

		memset(now, 0, sizeof(now));
		cell[0] = 0;
		if (f && f % 8 == 0) {
			hidden = !hidden;
			memcpy(now, blink_cell, sizeof(now));
		}
		if (f >= 4 && f % 3 == 1 && (f < CURSOR_ON || f > CURSOR_OFF)) {
			bool wide = typed % 4 == 3;
			unsigned int w = wide ? 2 : 1;

			if (col + w > COLS - 1) {
				row++;
				col = 0;
			}
			assert(row < ROWS);
			snprintf(cell, sizeof(cell), ESC "[%u;%uH%s", row + 1, col + 1,
				 wide ? "日" : (char[]){'a' + typed % 26, 0});
			if (wide && wide_row == ROWS) {
				wide_row = row;
				wide_col = col;
			}
			for (unsigned int c = 0; c < w; c++)
				now[row][col + c] = true;
			col += w;
			typed++;
		}
		if (f == CURSOR_ON || f == CURSOR_OFF) {
			assert(wide_row < ROWS);
			/* moved first: libtsm marks the cursor's cell changed on a
			 * style change, hidden or not */
			if (f == CURSOR_ON)
				snprintf(cell, sizeof(cell), ESC "[%u;%uH" ESC "[6 q" ESC "[?25h",
					 wide_row + 1, wide_col + 1);
			else
				snprintf(cell, sizeof(cell), ESC "[?25l");
			now[wide_row][wide_col] = now[wide_row][wide_col + 1] = true;
		}
		if (f == POINTER || f == POINTER + 1)
			for (unsigned int c = 20; c < 22; c++)
				now[4][c] = now[5][c] = true;
		o.pointer = f == POINTER ? pointer : NULL;
		if (cell[0]) {
			tsm_vte_input(vte, cell, strlen(cell));
			strncat(bytes, cell, sizeof(bytes) - strlen(bytes) - 1);
		}

		fresh(ref, bytes, hidden, o.pointer);
		snprintf(label, sizeof(label), "damage extent: frame %u (%s phase)", f + 1,
			 hidden ? "hidden" : "visible");
		same(output_frame(&o, con, vte, hidden, label), ref, label);

		damage_cells(&o, got);
		for (unsigned int y = 0; y < ROWS; y++) {
			for (unsigned int x = 0; x < COLS; x++) {
				n += got[y][x];
				want[y][x] = now[y][x] || before[y][x];
				any = any || want[y][x];
				if (f >= 3 && want[y][x] && !got[y][x] && !reported) {
					fprintf(stderr,
						"test_blink: %s: cell %u,%u changed but left out "
						"of "
						"the damage\n",
						label, x, y);
					reported = true;
					failures++;
				}
			}
		}
		if (f >= 3) {
			compared++;
			for (size_t i = 0; i < o.n_rects && !reported; i++) {
				if (!rect_fits(&o, &o.rects[i], want)) {
					fprintf(stderr,
						"test_blink: %s: damage rect %d,%d-%d,%d reaches "
						"past the cells changed in two frames\n",
						label, o.rects[i].x1, o.rects[i].y1, o.rects[i].x2,
						o.rects[i].y2);
					reported = true;
					failures++;
				}
			}
		}
		if (o.swaps - swaps != (f < 3 || any)) {
			fprintf(stderr, "test_blink: %s: %s\n", label,
				f < 3 || any ? "a frame with damage was not presented"
					     : "a frame without change was presented");
			failures++;
		}
		total += n;
		if (f >= 3) {
			judged += n;
			most = n > most ? n : most;
			none += !n;
		}
		memcpy(before, now, sizeof(now));
	}
	fprintf(stderr,
		"test_blink: damage extent: %u frames, %u cells damaged, %u in frames 4-%u "
		"(at most %u in one frame, %u frames with none), %u swaps\n",
		EXTENT_FRAMES, total, judged, EXTENT_FRAMES, most, none, o.swaps);

	output_destroy(&o);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	free(ref);
}

/*
 * The hardware cursor on a display with damage: terminal.c shows, moves and
 * hides it through the display, which puts it on screen only with a swap, and
 * then redraws, so the frame draws nothing. Each such frame must still be
 * swapped, once; a frame after it without change must not be. The steps are
 * those of a pointer that appears, moves, stays put, and hides when idle.
 */
static void test_hw_cursor(void)
{
	static char bytes[4096];
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct output o;
	uint32_t *ref = malloc(fb_bytes);
	static const struct {
		const char *what;
		int show; /* 1: show at x, y; -1: hide; 0: leave */
		int32_t x, y;
		unsigned int swaps;
	} steps[] = {
		{"no change", 0, 0, 0, 0},	  {"shown", 1, 40, 30, 1},
		{"no change", 0, 0, 0, 0},	  {"moved", 1, 90, 50, 1},
		{"shown in place", 1, 90, 50, 0}, {"hidden after the timeout", -1, 0, 0, 1},
		{"hidden again", -1, 0, 0, 0},	  {"no change", 0, 0, 0, 0},
		{"shown again", 1, 10, 10, 1},	  {"no change", 0, 0, 0, 0},
	};
	char label[128];

	screen_bytes(bytes, sizeof(bytes), BLINKING);
	console_new(&con, &vte);
	tsm_vte_input(vte, bytes, strlen(bytes));
	output_init(&o, "hw cursor", font, true);
	fresh(ref, bytes, false, NULL);

	/* the full redraws and their catch-up */
	for (int i = 0; i < 3; i++)
		output_frame(&o, con, vte, false, "hw cursor: first frames");

	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		unsigned int swaps = o.swaps;

		if (steps[i].show > 0)
			display_show_cursor(&o.disp, steps[i].x, steps[i].y);
		else if (steps[i].show < 0)
			display_hide_cursor(&o.disp);
		snprintf(label, sizeof(label), "hw cursor: step %zu (%s)", i, steps[i].what);
		same(output_frame(&o, con, vte, false, label), ref, label);
		compared++;
		if (o.n_rects || o.swaps - swaps != steps[i].swaps) {
			fprintf(stderr, "test_blink: %s: %zu damage rects and %u swaps, not %u\n",
				label, o.n_rects, o.swaps - swaps, steps[i].swaps);
			failures++;
		}
	}

	output_destroy(&o);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	free(ref);
}

/* Replace the process's fontconfig with one that knows only the pinned files. */
static void pin_fonts(void)
{
	static const char *const files[] = {"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf",
					    "NotoSansMonoCJKJP-Subset.otf"};
	FcConfig *cfg = FcConfigCreate();
	char path[4096];

	assert(cfg);
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", font_dir, files[i]);
		if (!FcConfigAppFontAddFile(cfg, (const FcChar8 *)path)) {
			fprintf(stderr, "test_blink: cannot load font %s\n", path);
			exit(2);
		}
	}
	if (!FcConfigSetCurrent(cfg)) {
		fprintf(stderr, "test_blink: cannot install the font configuration\n");
		exit(2);
	}
}

int main(int argc, char **argv)
{
	int ret;

	if (argc != 2) {
		fprintf(stderr, "usage: %s FONT_DIR\n", argv[0]);
		return 2;
	}
	font_dir = argv[1];

	ret = kmscon_font_register(&kmscon_font_freetype_ops);
	assert(ret == 0);
	ret = kmscon_text_register(&kmscon_text_bbulk_ops);
	assert(ret == 0);

	pin_fonts();
	ret = kmscon_font_find(&font, "DejaVu Sans Mono", FONT_PX, "freetype");
	if (ret || !font || font->ops != &kmscon_font_freetype_ops) {
		fprintf(stderr, "test_blink: pinned font not found\n");
		return 2;
	}
	fb_w = COLS * font->width;
	fb_h = ROWS * font->height;
	fb_bytes = (size_t)fb_w * fb_h * 4;
	map_blink_cells();

	test_phases();
	test_buffers();
	test_damage_extent();
	test_hw_cursor();

	kmscon_font_unref(font);
	fprintf(stderr, "test_blink: %u screens compared, %d failures\n", compared, failures);
	if (!compared)
		failures++;
	return failures ? 1 : 0;
}
