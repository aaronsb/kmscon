/*
 * ADR-103's synthetic scenarios S1 to S6, drawn through the real bbulk
 * renderer, freetype backend and drm2d blend into an in-memory XRGB8888
 * display, as tier 2 draws: the CPU time kmscon spends per frame from
 * preparing it to the end of the render pass, the figure the render timer
 * reports, without a KMS device or a VM.
 *
 * Usage: bench_compose FONT_DIR WIDTH HEIGHT [FRAMES [SCENARIO...]]
 *
 * One JSON object per scenario on stdout. What a scenario changes between
 * frames (the bytes parsed, the pictures made through libtsm's test calls) is
 * done outside the timed part, as kmscon parses before it renders. Run it on
 * one CPU (make bench-compose pins it) to stand in for ADR-103's small machine.
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
#include <time.h>

#include "../src/font/font.c"
#include "../src/font/font_box.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_freetype.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_colour.c"
#undef LOG_SUBSYSTEM
#include "../src/render/text.c"
#undef LOG_SUBSYSTEM
#if __has_include("../src/render/picture.c")
#include "../src/render/picture.c"
#endif
#include "../src/render/bbulk.c"
#undef LOG_SUBSYSTEM
#include "../src/render/deco.c"
#undef LOG_SUBSYSTEM
#include "../src/video/drm2d_render.c"

void shl_module_ref(struct shl_module *module) {}
void shl_module_unref(struct shl_module *module) {}

/* ---- in-memory display, damage clips on ---- */

static uint32_t *fb[2];
static unsigned int fb_w, fb_h, front;
static struct drm2d_display fake_d2d;
static struct display fake_disp;

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
bool display_supports_damage(struct display *disp)
{
	return true;
}
void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages) {}
int display_swap(struct display *disp)
{
	return 0;
}
bool display_need_commit(struct display *disp)
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

/* ---- scenarios ---- */

#define ESC "\x1b"
#define Z_BELOW_TEXT (-1)

struct bench {
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct kmscon_text *txt;
	unsigned int cols, rows, cw, ch;
	uint32_t *pic[2]; /* two full-screen pictures to alternate */
	uint32_t *small[2];
};

static void vte_write(struct tsm_vte *vte, const char *u8, size_t len, void *data) {}

static void input(struct bench *b, const char *s)
{
	tsm_vte_input(b->vte, s, strlen(s));
}

/* an opaque gradient, its blue from @seed */
static uint32_t *gradient(unsigned int w, unsigned int h, unsigned int seed)
{
	uint32_t *px = malloc((size_t)w * h * 4);
	unsigned int x, y;

	assert(px);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			px[(size_t)y * w + x] = 0xff000000 | (x * 255 / w) << 16 |
						(y * 255 / h) << 8 | ((seed * 97 + 64) & 0xff);
	return px;
}

static uint64_t image(struct bench *b, const uint32_t *px, unsigned int w, unsigned int h,
		      uint32_t id)
{
	struct tsm_test_image img = {
		.width = w, .height = h, .pixels = px, .kind = TSM_IMAGE_KITTY, .id = id};
	uint64_t key;

	assert(tsm_screen_test_image(b->con, &img, &key) == 0);
	return key;
}

static void place(struct bench *b, uint64_t image, uint32_t id, unsigned int col, unsigned int row,
		  int32_t z)
{
	struct tsm_test_placement pl = {.image = image, .id = id, .col = col, .row = row, .z = z};
	uint64_t key;

	assert(tsm_screen_test_place(b->con, &pl, &key) == 0);
}

/* a line of text a screen wide, different on each frame */
static void text_line(struct bench *b, unsigned int frame, char *line, size_t size)
{
	static const char words[] = "the quick brown fox jumps over the lazy dog 0123456789 ";
	unsigned int i, n = b->cols - 1 < size - 1 ? b->cols - 1 : size - 1;

	for (i = 0; i < n; i++)
		line[i] = words[(i + frame * 7) % (sizeof(words) - 1)];
	line[n] = 0;
}

/* fills the screen with text, the cursor on the last row */
static void fill(struct bench *b)
{
	char line[1024];
	unsigned int i;

	for (i = 0; i < b->rows; i++) {
		text_line(b, i, line, sizeof(line));
		input(b, i ? "\r\n" : "");
		input(b, line);
	}
}

/* S1: text scrolling, no image */
static void s1(struct bench *b, unsigned int frame)
{
	char line[1024];

	if (frame == 0)
		fill(b);
	text_line(b, frame, line, sizeof(line));
	input(b, "\r\n");
	input(b, line);
}

/* S2: text scrolling in a region below a static picture over the top half */
static void s2(struct bench *b, unsigned int frame)
{
	char line[1024];

	if (frame == 0) {
		fill(b);
		place(b, image(b, b->pic[0], b->cols * b->cw, b->rows / 2 * b->ch, 1), 0, 0, 0, 0);
		snprintf(line, sizeof(line), ESC "[%u;%ur" ESC "[%u;1H", b->rows / 2 + 1, b->rows,
			 b->rows);
		input(b, line);
	}
	text_line(b, frame, line, sizeof(line));
	input(b, "\r\n");
	input(b, line);
}

/*
 * S3: one line of scroll with a full-width picture over half the screen, so
 * every cell under it moves; the picture is put back at the top every eighth
 * frame, which moves it too.
 */
static void s3(struct bench *b, unsigned int frame)
{
	static uint64_t key;
	char line[1024];

	if (frame == 0) {
		fill(b);
		key = image(b, b->pic[0], b->cols * b->cw, b->rows / 2 * b->ch, 1);
	}
	if (frame % 8 == 0)
		place(b, key, 1, 0, 0, 0);
	text_line(b, frame, line, sizeof(line));
	input(b, ESC "[999;1H\r\n");
	input(b, line);
}

/* S4: a full-screen opaque picture replaced every frame, in the top band */
static void s4(struct bench *b, unsigned int frame)
{
	place(b, image(b, b->pic[frame % 2], b->cols * b->cw, b->rows * b->ch, 1), 0, 0, 0, 0);
}

/* S5: S4 below text, under a two-row text band that scrolls every frame */
static void s5(struct bench *b, unsigned int frame)
{
	char line[1024], out[1100];

	if (frame == 0) {
		snprintf(line, sizeof(line), ESC "[%u;%ur" ESC "[%u;1H", b->rows - 1, b->rows,
			 b->rows);
		input(b, line);
	}
	place(b, image(b, b->pic[frame % 2], b->cols * b->cw, b->rows * b->ch, 1), 0, 0, 0,
	      Z_BELOW_TEXT);
	text_line(b, frame, line, sizeof(line));
	snprintf(out, sizeof(out), "\r\n" ESC "[44;97m%s" ESC "[0m", line);
	input(b, out);
}

/* S6: fifty small pictures, 6 x 3 cells each, every one replaced every frame */
static void s6(struct bench *b, unsigned int frame)
{
	unsigned int i;

	if (frame == 0)
		fill(b);
	for (i = 0; i < 50; i++)
		place(b, image(b, b->small[(frame + i) % 2], 6 * b->cw, 3 * b->ch, i + 1), 0,
		      (i % 10) * 7 % b->cols, (i / 10) * 4 % b->rows, 0);
}

static const struct {
	const char *name;
	void (*step)(struct bench *b, unsigned int frame);
} scenarios[] = {
	{"S1", s1}, {"S2", s2}, {"S3", s3}, {"S4", s4}, {"S5", s5}, {"S6", s6},
};

static uint64_t cpu_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/* one frame as kmscon draws it: prepare, draw, render; returns its CPU time */
static uint64_t frame(struct bench *b, uint64_t *damage, unsigned int *requests)
{
	struct tsm_screen_attr attr;
	struct bbulk *bb = b->txt->data;
	uint64_t t0, t1;

	tsm_vte_get_def_attr(b->vte, &attr);
	t0 = cpu_ns();
	assert(kmscon_text_prepare(b->txt, &attr, false) == 0);
	assert(kmscon_text_draw(b->txt, b->con, false) == 0);
	kmscon_text_render(b->txt);
	t1 = cpu_ns();
	*damage = b->txt->frame_damage_px;
	*requests = bb->requests;

	/* the next frame draws into the other buffer */
	front ^= 1;
	fake_d2d.rb[1].map = fb[front ^ 1];
	return t1 - t0;
}

static void run(struct kmscon_font *font, unsigned int si, unsigned int frames)
{
	struct bench b = {0};
	uint64_t *cpu = calloc(frames, sizeof(*cpu)), sum = 0, damage, damage_sum = 0;
	unsigned int i, requests, requests_sum = 0, warm = 4;

	assert(cpu);
	b.cw = font->width;
	b.ch = font->height;
	b.cols = fb_w / b.cw;
	b.rows = fb_h / b.ch;
	b.pic[0] = gradient(b.cols * b.cw, b.rows * b.ch, 1);
	b.pic[1] = gradient(b.cols * b.cw, b.rows * b.ch, 2);
	b.small[0] = gradient(6 * b.cw, 3 * b.ch, 3);
	b.small[1] = gradient(6 * b.cw, 3 * b.ch, 4);

	assert(tsm_screen_new(&b.con, NULL, NULL) == 0);
	assert(tsm_vte_new(&b.vte, b.con, vte_write, NULL, NULL, NULL) == 0);
	tsm_vte_set_palette(b.vte, "default");
	assert(tsm_screen_resize(b.con, b.cols, b.rows) == 0);
	assert(tsm_screen_set_cell_size(b.con, b.cw, b.ch) == 0);
	tsm_screen_set_image_limit(b.con, (size_t)1 << 30);
	input(&b, ESC "[?25l");
	assert(kmscon_text_new(&b.txt, "bbulk", OR_NORMAL, &fake_disp) == 0);
	assert(kmscon_text_set(b.txt, font) == 0);
	b.txt->measure = true;

	for (i = 0; i < warm + frames; i++) {
		scenarios[si].step(&b, i);
		uint64_t t = frame(&b, &damage, &requests);

		if (i < warm)
			continue;
		cpu[i - warm] = t;
		sum += t;
		damage_sum += damage;
		requests_sum += requests;
	}
	qsort(cpu, frames, sizeof(*cpu), cmp_u64);
	printf("{\"scenario\": \"%s\", \"width\": %u, \"height\": %u, \"cols\": %u, \"rows\": %u, "
	       "\"cell\": \"%ux%u\", \"frames\": %u, \"cpu_ms_mean\": %.3f, \"cpu_ms_median\": %.3f, "
	       "\"cpu_ms_p95\": %.3f, \"cpu_ms_max\": %.3f, \"damage_pct\": %.1f, "
	       "\"requests_mean\": %u}\n",
	       scenarios[si].name, fb_w, fb_h, b.cols, b.rows, b.cw, b.ch, frames,
	       sum / 1e6 / frames, cpu[frames / 2] / 1e6, cpu[(frames * 95 + 99) / 100 - 1] / 1e6,
	       cpu[frames - 1] / 1e6, 100.0 * damage_sum / frames / ((double)fb_w * fb_h),
	       requests_sum / frames);
	fflush(stdout);

	kmscon_text_unref(b.txt);
	tsm_vte_unref(b.vte);
	tsm_screen_unref(b.con);
	free(b.pic[0]);
	free(b.pic[1]);
	free(b.small[0]);
	free(b.small[1]);
	free(cpu);
}

int main(int argc, char **argv)
{
	struct kmscon_font *font = NULL;
	unsigned int frames = 60, i, k;
	char path[4096];
	FcConfig *cfg;

	if (argc < 4) {
		fprintf(stderr, "usage: %s FONT_DIR WIDTH HEIGHT [FRAMES [SCENARIO...]]\n", argv[0]);
		return 2;
	}
	fb_w = atoi(argv[2]);
	fb_h = atoi(argv[3]);
	if (argc > 4)
		frames = atoi(argv[4]);
	assert(fb_w && fb_h && frames);

	/* the pinned fonts only, as tier 2 */
	cfg = FcConfigCreate();
	snprintf(path, sizeof(path), "%s/DejaVuSansMono.ttf", argv[1]);
	assert(FcConfigAppFontAddFile(cfg, (const FcChar8 *)path));
	snprintf(path, sizeof(path), "%s/DejaVuSansMono-Bold.ttf", argv[1]);
	assert(FcConfigAppFontAddFile(cfg, (const FcChar8 *)path));
	assert(FcConfigSetCurrent(cfg));
	assert(kmscon_font_register(&kmscon_font_freetype_ops) == 0);
	assert(kmscon_text_register(&kmscon_text_bbulk_ops) == 0);
	assert(kmscon_font_find(&font, "DejaVu Sans Mono", 16, "freetype") == 0 && font);

	fb[0] = calloc((size_t)fb_w * fb_h, 4);
	fb[1] = calloc((size_t)fb_w * fb_h, 4);
	assert(fb[0] && fb[1]);
	fake_disp.width = fb_w;
	fake_disp.height = fb_h;
	fake_disp.data = &fake_d2d;
	fake_d2d.rb[1].map = fb[1];
	fake_d2d.rb[1].stride = fb_w * 4;

	for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		bool want = argc <= 5;

		for (k = 5; k < (unsigned int)argc; k++)
			want |= !strcmp(argv[k], scenarios[i].name);
		if (want)
			run(font, i, frames);
	}
	kmscon_font_unref(font);
	free(fb[0]);
	free(fb[1]);
	return 0;
}
