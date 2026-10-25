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
 *
 * The last part replays terminal.c's present rules on a fake clock, with
 * coalescing without mode 2026 (ADR-201 step 6) and without it: a bulk
 * write, frames in 16 KB and 4 KB write() calls, a write ending on a read's
 * boundary, frames in mode 2026, and a key's echo, on a display with flips
 * and one without. It prints presents per second and mixed cells per
 * present (cells showing neither the write's end nor what came before it
 * where the write changes them), and checks the rule's bounds.
 */

#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <inttypes.h>
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
#include "pty.h"
#include "terminal_sync.h"

void shl_module_ref(struct shl_module *module) {}
void shl_module_unref(struct shl_module *module) {}

#define COLS 120
#define ROWS 30
#define FONT_PX 16
#define FRAMES 48
#define NREAD KMSCON_NREAD
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
bool display_need_commit(struct display *disp)
{
	return false;
}
int display_show_cursor(struct display *disp, int32_t x, int32_t y)
{
	return 0;
}
int display_hide_cursor(struct display *disp)
{
	return 0;
}

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
	struct display disp;
	struct drm2d_display d2d;
	struct kmscon_text *txt;
	bool damage;			      /* reports damage, as with FB_DAMAGE_CLIPS */
	struct video_rect rects[COLS * ROWS]; /* the damage of the last frame */
	size_t n_rects;
};

static struct output *output_of(struct display *disp)
{
	return (struct output *)((char *)disp - offsetof(struct output, disp));
}

bool display_supports_damage(struct display *disp)
{
	return output_of(disp)->damage;
}

/* what display_swap returns; a failed swap leaves the buffers as they were */
static int swap_ret;

int display_swap(struct display *disp)
{
	if (swap_ret)
		return swap_ret;
	output_of(disp)->d2d.current_rb ^= 1;
	return 0;
}

void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages)
{
	struct output *o = output_of(disp);

	assert(n_rect <= sizeof(o->rects) / sizeof(o->rects[0]));
	memcpy(o->rects, damages, n_rect * sizeof(*damages));
	o->n_rects = n_rect;
}

static struct kmscon_font *font;
static const char *font_dir;
static int failures;
static unsigned int compared;  /* presented frames compared */
static unsigned int mid_write; /* of those, presented before their write ended */

static void output_init(struct output *o, bool damage)
{
	int ret;

	memset(o, 0, sizeof(*o));
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

/*
 * Draws @con on @o and presents it, as terminal.c does; returns whether it
 * swapped. A frame that changed nothing stays in the back buffer.
 */
static bool output_frame(struct output *o, struct tsm_screen *con, struct tsm_vte *vte)
{
	struct tsm_screen_attr attr;
	int ret;

	tsm_vte_get_def_attr(vte, &attr);
	ret = kmscon_text_prepare(o->txt, &attr, false);
	assert(ret == 0);
	ret = kmscon_text_draw(o->txt, con, false);
	assert(ret == 0);
	ret = kmscon_text_present(o->txt);
	assert(ret == 0);
	return o->txt->frame_queued;
}

static const uint32_t *on_screen(const struct output *o)
{
	return o->d2d.rb[o->d2d.current_rb].map;
}

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

/* ---- the program's output ---- */

static uint32_t rnd_state;

static uint32_t rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 8;
}

/* what the program last drew in each cell: a codepoint (0: a space) and its colours */
struct cell {
	uint32_t cp;
	uint32_t fg, bg; /* 0xRRGGBB, or ~0u: the default */
};

static struct cell grid[ROWS][COLS];

#define DEFAULT_COLOUR (~0u)

/* a cell the way the showcase's effects fill one: mostly blank, else a dot pattern or a block */
static struct cell new_cell(unsigned int frame, unsigned int row, unsigned int col)
{
	static const uint32_t blocks[] = {0x2588, 0x2580, 0x2584, 0x258c, 0x2590, 'o'};
	static const uint32_t palette[] = {0x1f6f3f, 0x448daa, 0x4ab99c,
					   0xff8020, 0xa02090, 0xffffff};
	struct cell c = {0, DEFAULT_COLOUR, DEFAULT_COLOUR};
	uint32_t r = rnd();

	if (r % 100 < 60)
		c.cp = 0;
	else if (r % 100 < 90)
		c.cp = r % 7 ? 0x2800 + (rnd() & 0xff) : 0x28ff;
	else
		c.cp = blocks[rnd() % (sizeof(blocks) / sizeof(blocks[0]))];
	/* runs of one colour, as a line drawn across cells has */
	c.fg = palette[(frame + col / 9 + row / 4) % (sizeof(palette) / sizeof(palette[0]))];
	if (rnd() % 8 == 0)
		c.bg = palette[rnd() % (sizeof(palette) / sizeof(palette[0]))];
	return c;
}

static size_t put_cp(char *out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = cp;
		return 1;
	}
	out[0] = 0xe0 | cp >> 12;
	out[1] = 0x80 | (cp >> 6 & 0x3f);
	out[2] = 0x80 | (cp & 0x3f);
	return 3;
}

/*
 * Frame @frame as one write into @out: a share of the cells change, and each
 * row with a change is drawn whole from its first column, with SGR only where
 * a colour changes; the rest are left alone. Returns its length.
 */
static size_t frame_bytes(char *out, size_t size, unsigned int frame)
{
	size_t n = 0;

	for (unsigned int row = 0; row < ROWS; row++) {
		bool changed = frame == 0;
		uint32_t fg = DEFAULT_COLOUR - 1, bg = DEFAULT_COLOUR - 1;

		for (unsigned int col = 0; col < COLS; col++) {
			if (frame == 0 || rnd() % 5 == 0) {
				grid[row][col] = new_cell(frame, row, col);
				changed = true;
			}
		}
		/* some rows stay as they are, as rows the picture does not reach */
		if (!changed || (frame && rnd() % 6 == 0))
			continue;
		n += snprintf(out + n, size - n, ESC "[%u;1H", row + 1);
		for (unsigned int col = 0; col < COLS; col++) {
			const struct cell *c = &grid[row][col];

			if (c->bg != bg) {
				bg = c->bg;
				n += bg == DEFAULT_COLOUR
					     ? snprintf(out + n, size - n, ESC "[49m")
					     : snprintf(out + n, size - n, ESC "[48;2;%u;%u;%um",
							bg >> 16, bg >> 8 & 0xff, bg & 0xff);
			}
			if (c->cp && c->fg != fg) {
				fg = c->fg;
				n += snprintf(out + n, size - n, ESC "[38;2;%u;%u;%um", fg >> 16,
					      fg >> 8 & 0xff, fg & 0xff);
			}
			n += put_cp(out + n, c->cp ? c->cp : ' ');
			assert(n + 64 < size);
		}
	}
	n += snprintf(out + n, size - n, ESC "[0m");
	return n;
}

/* ---- the runs ---- */

enum when {
	EVERY_READ, /* after every read, as when no flip is ever pending */
	WRITE_END,  /* once each write has been read whole */
	RANDOM,	    /* after some reads, as flips happen to complete */
};

static const char *const when_names[] = {"every read", "write end", "random reads"};

static void vte_write(struct tsm_vte *vte, const char *u8, size_t len, void *data) {}

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

	output_destroy(&o);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

/* ---- coalescing without mode 2026 (ADR-201 step 6), on a fake clock ---- */

/*
 * terminal.c's present rules replayed on a fake clock, with the real vte,
 * terminal_sync and renderer: the program's writes arrive as write() calls
 * of a set size at a set pace; each loop turn takes what an earlier input
 * call left, or reads NREAD bytes at a time until the pty is empty or 50
 * reads are done, as src/misc/pty.c does; parsing and drawing cost fixed
 * times. At each turn's end the screen is drawn as terminal_turn_end does,
 * by the rule (coalesced) or at once (as before the rule). A display with
 * flips shows a present at the next refresh and defers a draw asked for
 * meanwhile to that flip, as on DRM; one without shows it at once.
 */

#define US 1000ULL
#define MS (1000 * US)
#define REFRESH_NS (1000000000ULL / 60)
#define PARSE_NS_PER_KB (10 * US) /* 100 MB/s */
#define DRAW_NS (3 * MS)
#define ECHO_NS (200 * US) /* from a key press to its echo arriving */

struct write {
	uint64_t start_ns;
	size_t off, len;  /* in sim.data */
	size_t wsize;	  /* the write() calls it is made of; 0: one call */
	uint64_t wgap_ns; /* between them */
};

enum why {
	WHY_TURN,  /* the turn's end, the rule finding the write ended or not holding it back */
	WHY_TIMER, /* one refresh after the last present, mid-write */
	WHY_FLIP,  /* a draw deferred to a flip */
	WHY_SYNC,  /* a frame end of mode 2026 */
	WHY_KEY,   /* a key press */
};

struct sim {
	bool coalesce, flips;
	char *data;
	struct write w[512];
	unsigned int nw;
	size_t total;
	uint64_t key_ns; /* 0 or a key press */

	uint64_t t;
	size_t read, parsed;
	char rbuf[NREAD];
	size_t held_off, held_len;
	bool unparsed, read_full, redraw_owed;

	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct terminal_sync sync;
	uint64_t sync_due;
	struct output o;
	bool swapping, pending;
	uint64_t flip_at, coalesce_due, presented_ns, waited_ns;

	/* the screen as it is once the write in progress is parsed whole */
	struct tsm_screen *ref_con;
	struct tsm_vte *ref_vte;
	struct output ref;
	unsigned int ref_writes;

	/* what was presented */
	unsigned int presents, mixed_presents, mid_presents;
	uint64_t mixed_cells, max_mixed;
	uint64_t first_ns, last_ns;
	uint64_t prev_ns, prev_shown_ns;
	bool prev_partial;
	uint64_t max_partial_gap; /* from a present part way through a write to the next */
	uint64_t min_mid_gap;	  /* before a draw the rule made part way through a write */
	uint64_t echo_latency;	  /* from the echo's arrival to its present */
	enum why last_why;	  /* what made the last present */
	uint64_t parsed_ns;	  /* when the last byte was parsed */
	unsigned int failures;
};

static size_t sim_readable(const struct sim *s, uint64_t t)
{
	size_t n = 0;

	for (unsigned int k = 0; k < s->nw && s->w[k].start_ns <= t; k++) {
		const struct write *w = &s->w[k];
		size_t calls, got;

		if (!w->wsize || !w->wgap_ns) {
			got = w->len;
		} else {
			calls = (t - w->start_ns) / w->wgap_ns + 1;
			got = calls * w->wsize < w->len ? calls * w->wsize : w->len;
		}
		n = w->off + got;
	}
	return n;
}

/* when the program next hands the pty bytes after @t; UINT64_MAX when it has no more */
static uint64_t sim_next_arrival(const struct sim *s, uint64_t t)
{
	for (unsigned int k = 0; k < s->nw; k++) {
		const struct write *w = &s->w[k];

		if (w->start_ns > t)
			return w->start_ns;
		if (sim_readable(s, t) < w->off + w->len && w->wsize && w->wgap_ns)
			return w->start_ns + ((t - w->start_ns) / w->wgap_ns + 1) * w->wgap_ns;
	}
	return UINT64_MAX;
}

/* the write @off lies in, or nw past the end */
static unsigned int sim_write_at(const struct sim *s, size_t off)
{
	unsigned int k = 0;

	while (k < s->nw && off >= s->w[k].off + s->w[k].len)
		k++;
	return k;
}

/* cells whose pixels differ between what @a and @b show */
static unsigned int cells_differing(const struct output *a, const struct output *b)
{
	const uint32_t *pa = on_screen(a), *pb = on_screen(b);
	unsigned int n = 0;

	for (unsigned int row = 0; row < ROWS; row++) {
		for (unsigned int col = 0; col < COLS; col++) {
			bool differs = false;

			for (unsigned int y = 0; y < font->height && !differs; y++) {
				size_t i =
					(size_t)(row * font->height + y) * fb_w + col * font->width;

				differs = memcmp(pa + i, pb + i, font->width * 4) != 0;
			}
			n += differs;
		}
	}
	return n;
}

static void sim_draw(struct sim *s, enum why why, bool mid)
{
	unsigned int mixed;
	uint64_t shown;
	bool partial;

	s->pending = false;
	s->waited_ns = 0;
	s->t += DRAW_NS;
	if (!output_frame(&s->o, s->con, s->vte))
		return;
	s->presented_ns = s->t;
	shown = s->t;
	if (s->flips) {
		s->swapping = true;
		s->flip_at = (s->t / REFRESH_NS + 1) * REFRESH_NS;
		shown = s->flip_at;
	}

	output_frame(&s->ref, s->ref_con, s->ref_vte);
	mixed = cells_differing(&s->o, &s->ref);
	partial = s->parsed < s->total && s->parsed != s->w[sim_write_at(s, s->parsed)].off;
	if (!s->presents)
		s->first_ns = s->t;
	if (s->presents && s->prev_partial && shown - s->prev_shown_ns > s->max_partial_gap)
		s->max_partial_gap = shown - s->prev_shown_ns;
	/* from the last present to this draw's start, which the rule decided */
	if (s->presents && mid && s->t - DRAW_NS - s->prev_ns < s->min_mid_gap)
		s->min_mid_gap = s->t - DRAW_NS - s->prev_ns;
	if (s->key_ns && !s->echo_latency && s->parsed == s->total)
		s->echo_latency = shown - (s->key_ns + ECHO_NS);
	s->presents++;
	s->mid_presents += mid;
	s->mixed_presents += mixed > 0;
	s->mixed_cells += mixed;
	if (mixed > s->max_mixed)
		s->max_mixed = mixed;
	s->last_ns = s->t;
	s->prev_ns = s->t;
	s->prev_shown_ns = shown;
	s->prev_partial = partial;
	s->last_why = why;
}

/* redraw_screen */
static void sim_redraw(struct sim *s, enum why why, bool mid)
{
	switch (terminal_sync_draw(&s->sync, s->swapping, false)) {
	case TERMINAL_SYNC_DRAW:
		sim_draw(s, why, mid);
		break;
	case TERMINAL_SYNC_FLIP:
		s->pending = true;
		break;
	case TERMINAL_SYNC_OWED:
	case TERMINAL_SYNC_IDLE:
		break;
	}
}

/* sync_act */
static void sim_act(struct sim *s, enum terminal_sync_action action)
{
	switch (action) {
	case TERMINAL_SYNC_NONE:
		break;
	case TERMINAL_SYNC_ARM:
		s->sync_due = s->t + terminal_sync_arm_ns(&s->sync, s->t);
		break;
	case TERMINAL_SYNC_EXPIRED:
	case TERMINAL_SYNC_PRESENT:
		s->sync_due = 0;
		sim_redraw(s, WHY_SYNC, false);
		break;
	}
}

/* pty_input, then the pty's feed: true if the call took all of it */
static bool sim_input(struct sim *s, const char *u8, size_t len)
{
	size_t n;

	s->redraw_owed = true;
	if (!s->unparsed)
		s->read_full = len >= NREAD;
	n = tsm_vte_input_bounded(s->vte, u8, len);
	s->t += n * PARSE_NS_PER_KB / 1024;
	s->parsed += n;
	if (s->parsed == s->total)
		s->parsed_ns = s->t;
	while (s->ref_writes < s->nw && s->w[s->ref_writes].off < s->parsed) {
		const struct write *w = &s->w[s->ref_writes++];

		tsm_vte_input(s->ref_vte, s->data + w->off, w->len);
	}
	s->unparsed = n < len;
	sim_act(s, terminal_sync_input(&s->sync, s->t, s->unparsed));
	if (s->unparsed) {
		s->held_off = (u8 - s->rbuf) + n;
		s->held_len = (u8 - s->rbuf) + len;
		return false;
	}
	s->held_len = 0;
	return true;
}

/* terminal_turn_end */
static void sim_turn_end(struct sim *s)
{
	bool mid = s->read_full || s->unparsed;
	uint64_t due;

	if (!s->redraw_owed)
		return;
	s->redraw_owed = false;
	if (!kmscon_text_changed(s->o.txt, s->con))
		return;
	due = s->coalesce ? terminal_sync_coalesce(&s->sync, mid, s->t, s->presented_ns,
						   s->waited_ns, REFRESH_NS)
			  : 0;
	if (!due) {
		sim_redraw(s, WHY_TURN, mid);
		return;
	}
	if (!s->waited_ns)
		s->waited_ns = s->t;
	if (!s->coalesce_due || due < s->coalesce_due)
		s->coalesce_due = due;
}

static void sim_run(struct sim *s)
{
	bool key_done = !s->key_ns;

	for (;;) {
		uint64_t next = UINT64_MAX;

		if (s->swapping && s->flip_at <= s->t) {
			s->swapping = false;
			/* display_pageflip */
			switch (terminal_sync_flipped(&s->sync, s->pending, false)) {
			case TERMINAL_SYNC_DRAW:
			case TERMINAL_SYNC_FLIP:
				sim_draw(s, WHY_FLIP, false);
				break;
			default:
				break;
			}
		}
		if (s->coalesce_due && s->coalesce_due <= s->t) {
			s->coalesce_due = 0;
			if (s->waited_ns)
				sim_redraw(s, WHY_TIMER, true);
		}
		if (s->sync_due && s->sync_due <= s->t) {
			s->sync_due = 0;
			sim_act(s, terminal_sync_timer(&s->sync, s->t, s->unparsed));
		}
		if (!key_done && s->key_ns <= s->t) {
			key_done = true;
			sim_redraw(s, WHY_KEY, false);
		}

		if (s->held_len) {
			sim_input(s, s->rbuf + s->held_off, s->held_len - s->held_off);
		} else {
			for (unsigned int num = 50; num; num--) {
				size_t n = sim_readable(s, s->t) - s->read;

				if (!n)
					break;
				if (n > NREAD)
					n = NREAD;
				memcpy(s->rbuf, s->data + s->read, n);
				s->read += n;
				if (!sim_input(s, s->rbuf, n))
					break;
			}
		}
		sim_turn_end(s);

		if (s->held_len || sim_readable(s, s->t) > s->read)
			continue;
		next = sim_next_arrival(s, s->t);
		if (s->swapping && s->flip_at < next)
			next = s->flip_at;
		if (s->coalesce_due && s->coalesce_due < next)
			next = s->coalesce_due;
		if (s->sync_due && s->sync_due < next)
			next = s->sync_due;
		if (!key_done && s->key_ns < next)
			next = s->key_ns;
		if (next == UINT64_MAX)
			break;
		if (next > s->t)
			s->t = next;
	}
}

static void sim_init(struct sim *s, bool coalesce, bool flips)
{
	memset(s, 0, sizeof(*s));
	s->coalesce = coalesce;
	s->flips = flips;
	s->min_mid_gap = UINT64_MAX;
	assert(tsm_screen_new(&s->con, NULL, NULL) == 0);
	assert(tsm_screen_resize(s->con, COLS, ROWS) == 0);
	assert(tsm_vte_new(&s->vte, s->con, vte_write, NULL, NULL, NULL) == 0);
	tsm_vte_set_palette(s->vte, "default");
	tsm_vte_input(s->vte, ESC "[?25l", 6);
	terminal_sync_init(&s->sync, s->vte, 200);
	assert(tsm_screen_new(&s->ref_con, NULL, NULL) == 0);
	assert(tsm_screen_resize(s->ref_con, COLS, ROWS) == 0);
	assert(tsm_vte_new(&s->ref_vte, s->ref_con, vte_write, NULL, NULL, NULL) == 0);
	tsm_vte_set_palette(s->ref_vte, "default");
	tsm_vte_input(s->ref_vte, ESC "[?25l", 6);
	output_init(&s->o, false);
	output_init(&s->ref, false);
	/* both start from the blank screen, presented long ago */
	output_frame(&s->o, s->con, s->vte);
	output_frame(&s->ref, s->ref_con, s->ref_vte);
}

static void sim_free(struct sim *s)
{
	output_destroy(&s->o);
	output_destroy(&s->ref);
	tsm_vte_unref(s->vte);
	tsm_screen_unref(s->con);
	tsm_vte_unref(s->ref_vte);
	tsm_screen_unref(s->ref_con);
}

/* a program's output: @writes writes of @frames frames each, @period_ns apart */
struct sched {
	const char *name;
	unsigned int writes, frames;
	uint64_t period_ns;
	size_t wsize;
	uint64_t wgap_ns;
	bool sync;
};

static char *sched_data(const struct sched *sc, struct write *w, size_t *total)
{
	size_t size = (size_t)sc->writes * sc->frames * 48 * 1024 + 4096;
	char *data = malloc(size);
	size_t n = 0;

	assert(data);
	rnd_state = 1;
	memset(grid, 0, sizeof(grid));
	for (unsigned int k = 0; k < sc->writes; k++) {
		w[k].start_ns = k * sc->period_ns;
		w[k].off = n;
		w[k].wsize = sc->wsize;
		w[k].wgap_ns = sc->wgap_ns;
		if (sc->sync)
			n += snprintf(data + n, size - n, ESC "[?2026h");
		for (unsigned int f = 0; f < sc->frames; f++)
			n += frame_bytes(data + n, size - n, k * sc->frames + f);
		if (sc->sync)
			n += snprintf(data + n, size - n, ESC "[?2026l");
		w[k].len = n - w[k].off;
		/* each write is in the pty before the next begins */
		assert(!sc->wsize || !sc->wgap_ns ||
		       (w[k].len + sc->wsize - 1) / sc->wsize * sc->wgap_ns <= sc->period_ns ||
		       k + 1 == sc->writes);
	}
	*total = n;
	return data;
}

/* presents per second of the program's output, from its first byte to the last present */
static double sim_rate(const struct sim *s)
{
	return s->last_ns ? s->presents * 1e9 / (double)s->last_ns : 0;
}

static void sim_report(const char *name, const char *display, const struct sim *b,
		       const struct sim *a)
{
	fprintf(stderr,
		"test_present: %-24s %-8s before: %4u presents, %6.1f/s, %7.1f mixed cells per "
		"present (max %5" PRIu64 ") | after: %4u presents, %6.1f/s, %7.1f mixed cells per "
		"present (max %5" PRIu64 ")\n",
		name, display, b->presents, sim_rate(b),
		b->presents ? (double)b->mixed_cells / b->presents : 0.0, b->max_mixed, a->presents,
		sim_rate(a), a->presents ? (double)a->mixed_cells / a->presents : 0.0,
		a->max_mixed);
}

static void sim_fail(const char *name, const char *display, const char *what)
{
	fprintf(stderr, "test_present: coalescing, %s, %s: %s\n", name, display, what);
	failures++;
}

/* one schedule before the rule and after it, on @flips' display; leaves the runs in @b and @a */
static void sim_sched(const struct sched *sc, bool flips, struct sim *b, struct sim *a)
{
	struct sim *runs[2] = {b, a};

	for (int i = 0; i < 2; i++) {
		struct sim *s = runs[i];

		sim_init(s, i == 1, flips);
		s->nw = sc->writes;
		assert(s->nw <= sizeof(s->w) / sizeof(s->w[0]));
		s->data = sched_data(sc, s->w, &s->total);
		sim_run(s);
		assert(s->parsed == s->total);
		/* the last present shows the program's last frame whole */
		if (cells_differing(&s->o, &s->ref))
			sim_fail(sc->name, flips ? "flips" : "no flips",
				 "the screen is left part written");
	}
	sim_report(sc->name, flips ? "flips" : "no flips", b, a);
}

static void sim_done(struct sim *s)
{
	free(s->data);
	sim_free(s);
}

/*
 * A bulk write, as cat of a large file: 128 KB write() calls faster than
 * kmscon parses, so every read is full. A present part way through comes at
 * most once a refresh, and never two refreshes after the one before.
 */
static void test_coalesce_bulk(bool flips)
{
	static const struct sched sc = {"bulk write", 1, 300, 0, 128 * 1024, 30 * US, false};
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(&sc, flips, &b, &a);
	if (a.mid_presents < 2)
		sim_fail(sc.name, display, "the write never presented part way through");
	if (a.min_mid_gap < REFRESH_NS)
		sim_fail(sc.name, display, "two presents part way through came within a refresh");
	if (a.max_partial_gap > 2 * REFRESH_NS)
		sim_fail(sc.name, display, "two refreshes in a row were skipped mid-write");
	if (!flips && b.min_mid_gap >= REFRESH_NS)
		sim_fail(sc.name, display,
			 "control: before the rule, presents came a refresh apart");
	sim_done(&b);
	sim_done(&a);
}

/*
 * Frames of three screens, written as 16 KB write() calls slower than kmscon
 * parses, 50 ms apart: each read is full and leaves the pty empty. Each
 * write begins after an idle spell, so its wait counts from its first read,
 * and the rule draws at its last, short read: no frame is presented part
 * written.
 */
static void test_coalesce_frames(bool flips)
{
	static const struct sched sc = {
		"frames, 16 KB writes", 24, 3, 50 * MS, NREAD, 300 * US, false};
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(&sc, flips, &b, &a);
	if (!b.mixed_cells)
		sim_fail(sc.name, display, "control: before the rule, no cell was presented mixed");
	if (a.mixed_cells)
		sim_fail(sc.name, display, "a frame was presented part written");
	if (a.max_partial_gap > 2 * REFRESH_NS)
		sim_fail(sc.name, display, "two refreshes in a row were skipped mid-write");
	sim_done(&b);
	sim_done(&a);
}

/*
 * The same frames as 4 KB write() calls: every read is short, so the rule
 * holds nothing back, and presents are as before (ADR-201's cost: a write
 * slower than kmscon reads still tears).
 */
static void test_coalesce_short(bool flips)
{
	static const struct sched sc = {
		"frames, 4 KB writes", 24, 3, 50 * MS, 4096, 80 * US, false};
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(&sc, flips, &b, &a);
	if (a.presents != b.presents || a.mixed_cells != b.mixed_cells)
		sim_fail(sc.name, display, "short reads were held back");
	sim_done(&b);
	sim_done(&a);
}

/*
 * A write that ends exactly on a read's boundary: every read is full, the
 * last one too, and no more output comes to end the wait. The timer
 * presents the write whole within one refresh of the last present.
 */
static void test_coalesce_boundary(bool flips)
{
	static const struct sched sc = {
		"write on a read boundary", 1, 2, 0, NREAD, 300 * US, false};
	const char *display = flips ? "flips" : "no flips";
	struct sim *runs[2], b, a;

	runs[0] = &b;
	runs[1] = &a;
	for (int i = 0; i < 2; i++) {
		struct sim *s = runs[i];
		size_t len, whole;

		sim_init(s, i == 1, flips);
		s->nw = 1;
		s->data = sched_data(&sc, s->w, &len);
		/* padded with NUL, which the vte ignores, to whole reads */
		whole = (len + NREAD - 1) / NREAD * NREAD;
		assert(whole >= 2 * NREAD && whole <= 2 * sc.frames * 48 * 1024);
		memset(s->data + len, 0, whole - len);
		s->w[0].len = s->total = whole;
		sim_run(s);
		assert(s->parsed == s->total);
		if (cells_differing(&s->o, &s->ref))
			sim_fail(sc.name, display, "the screen is left part written");
	}
	sim_report(sc.name, display, &b, &a);
	if (a.last_why != WHY_TIMER)
		sim_fail(sc.name, display, "the write's end was not presented by the timer");
	else if (a.last_ns - a.parsed_ns > REFRESH_NS + DRAW_NS)
		sim_fail(sc.name, display, "the timer presented more than a refresh late");
	sim_done(&b);
	sim_done(&a);
}

/*
 * Frames bracketed in mode 2026, written back to back as 16 KB write()
 * calls faster than kmscon parses, so frame ends fall inside full reads with
 * the next frame behind them: the hold decides. Nothing is presented part
 * written, and every present is as it was without the rule.
 */
static void test_coalesce_sync(bool flips)
{
	static const struct sched sc = {"frames in 2026", 24, 3, 2 * MS, NREAD, 100 * US, true};
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(&sc, flips, &b, &a);
	if (!a.presents)
		sim_fail(sc.name, display, "no frame was presented");
	if (a.mixed_cells || b.mixed_cells)
		sim_fail(sc.name, display, "a frame was presented part written");
	if (a.presents != b.presents || a.last_ns != b.last_ns)
		sim_fail(sc.name, display, "the rule changed a present the hold decides");
	sim_done(&b);
	sim_done(&a);
}

/*
 * A key press 2 ms after a present, and its echo, one byte, 0.2 ms later:
 * the echo is a short read, and presents as it did before the rule, at once
 * after the key's own draw, or at the flip in flight.
 */
static uint64_t echo_latency(bool coalesce, bool flips)
{
	static const char echo[] = "x";
	struct sim s;
	uint64_t latency;
	size_t n;

	sim_init(&s, coalesce, flips);
	s.data = malloc(64 * 1024);
	assert(s.data);
	rnd_state = 1;
	memset(grid, 0, sizeof(grid));
	n = frame_bytes(s.data, 64 * 1024, 0);
	s.w[0] = (struct write){0, 0, n, 0, 0};
	memcpy(s.data + n, echo, 1);
	/* the frame's present ends at 3.6 ms */
	s.key_ns = n * PARSE_NS_PER_KB / 1024 + DRAW_NS + 2 * MS;
	s.w[1] = (struct write){s.key_ns + ECHO_NS, n, 1, 0, 0};
	s.nw = 2;
	s.total = n + 1;
	sim_run(&s);
	latency = s.echo_latency;
	free(s.data);
	sim_free(&s);
	return latency;
}

static void test_coalesce_echo(bool flips)
{
	const char *display = flips ? "flips" : "no flips";
	uint64_t before = echo_latency(false, flips), after = echo_latency(true, flips);

	fprintf(stderr,
		"test_present: %-24s %-8s echo shown %.2f ms after it arrived before, %.2f ms "
		"after\n",
		"key echo", display, before / 1e6, after / 1e6);
	if (!after)
		sim_fail("key echo", display, "the echo was not presented");
	else if (after != before)
		sim_fail("key echo", display, "the echo waited for the rule");
	/* the key's own draw, then the echo's, then the flips in flight */
	else if (after > 2 * DRAW_NS + (flips ? 2 * REFRESH_NS : 0))
		sim_fail("key echo", display, "the echo waited");
}

static void test_coalesce(void)
{
	for (int flips = 0; flips < 2; flips++) {
		test_coalesce_bulk(flips);
		test_coalesce_frames(flips);
		test_coalesce_short(flips);
		test_coalesce_boundary(flips);
		test_coalesce_sync(flips);
		test_coalesce_echo(flips);
	}
}

/* Replace the process's fontconfig with one that knows only the pinned files. */
static void pin_fonts(void)
{
	static const char *const files[] = {"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf"};
	FcConfig *cfg = FcConfigCreate();
	char path[4096];

	assert(cfg);
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", font_dir, files[i]);
		if (!FcConfigAppFontAddFile(cfg, (const FcChar8 *)path)) {
			fprintf(stderr, "test_present: cannot load font %s\n", path);
			exit(2);
		}
	}
	if (!FcConfigSetCurrent(cfg)) {
		fprintf(stderr, "test_present: cannot install the font configuration\n");
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
		fprintf(stderr, "test_present: pinned font not found\n");
		return 2;
	}
	fb_w = COLS * font->width;
	fb_h = ROWS * font->height;
	fb_bytes = (size_t)fb_w * fb_h * 4;

	test_changed();
	for (int when = EVERY_READ; when <= RANDOM; when++)
		for (int cursor = 0; cursor < 2; cursor++)
			for (int damage = 0; damage < 2; damage++)
				run(when, cursor, damage);
	test_coalesce();

	kmscon_font_unref(font);
	fprintf(stderr,
		"test_present: %u presented frames compared, %u of them mid-write, %d failures\n",
		compared, mid_write, failures);
	return failures ? 1 : 0;
}
