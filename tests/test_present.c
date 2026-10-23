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
 * that changed must lie in the frame's damage.
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

#define COLS 120
#define ROWS 30
#define FONT_PX 16
#define FRAMES 48
#define NREAD 16384 /* KMSCON_NREAD, src/misc/pty.c */
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

int display_swap(struct display *disp)
{
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

			compared++;
			mid_write += !last;
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

	output_destroy(&o);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	free(fresh_px);
	free(before);
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

	for (int when = EVERY_READ; when <= RANDOM; when++)
		for (int cursor = 0; cursor < 2; cursor++)
			for (int damage = 0; damage < 2; damage++)
				run(when, cursor, damage);

	kmscon_font_unref(font);
	fprintf(stderr,
		"test_present: %u presented frames compared, %u of them mid-write, %d failures\n",
		compared, mid_write, failures);
	/* a run that never presented a frame mid-write has not tested anything */
	if (!compared || !mid_write)
		failures++;
	return failures ? 1 : 0;
}
