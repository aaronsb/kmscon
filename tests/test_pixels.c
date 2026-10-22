/*
 * Tier 2 conformance test (ADR-301): render fixed screens through the real
 * text renderer and font backend into an in-memory XRGB8888 framebuffer and
 * write each one as a PPM (P6) for pixcmp to compare with its golden.
 *
 * Usage: test_pixels FONT_DIR OUT_DIR
 *
 * Fonts are loaded from FONT_DIR through a private fontconfig configuration
 * that holds nothing else, so the system's fonts and fontconfig rules cannot
 * change the output.
 *
 * The pipeline is the production one: libtsm parses bytes into a screen,
 * kmscon_text_draw() reads it, the bbulk renderer asks the freetype backend
 * for glyph masks and hands them to display_blend(). Only the display is
 * replaced, by a fake struct display that the real drm2d blend and clear
 * draw into.
 */

#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libtsm.h>
#include <limits.h>

#include "../src/font/font.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_freetype.c"
#undef LOG_SUBSYSTEM
#include "../src/render/text.c"
#undef LOG_SUBSYSTEM
#include "../src/render/bbulk.c"
#undef LOG_SUBSYSTEM
#include "../src/render/deco.c"
#undef LOG_SUBSYSTEM
#include "../src/video/drm2d_render.c"
#include "config.h"

void shl_module_ref(struct shl_module *module) {}
void shl_module_unref(struct shl_module *module) {}

/* ---- in-memory display ---- */

static uint32_t *fb, *fb_back;
static unsigned int fb_w, fb_h;

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
bool display_has_damage(struct display *disp)
{
	return false;
}
bool display_supports_damage(struct display *disp)
{
	return false;
}
void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages) {}
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y) {}

/*
 * The real drm2d blend and clear run against a fake display whose back
 * buffer is fb: display_blend() and display_clear() forward to
 * drm2d_display_blend() and drm2d_display_clear() from
 * src/video/drm2d_render.c, which is compiled into this test.
 */
static struct drm2d_display fake_d2d;
static struct display fake_disp;

static void fake_display_init(void)
{
	fake_disp.width = fb_w;
	fake_disp.height = fb_h;
	fake_disp.data = &fake_d2d;
	fake_d2d.current_rb = 0;
	fake_d2d.rb[1].map = fb;
	fake_d2d.rb[1].stride = fb_w * 4;
}

int display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	return drm2d_display_clear(disp, r, g, b);
}

int display_blend(struct display *disp, const struct video_blend_req *req)
{
	return drm2d_display_blend(disp, req);
}

/* ---- screens ---- */

#define COLS 48
#define ROWS 10
#define FONT_PX 16
#define ESC "\x1b"

struct screen {
	const char *name;
	/* what the screen shows and what correct looks like: the caption on
	 * the conformance run page, written beside the image as NAME.txt */
	const char *desc;
	const char *bytes;
	/* optional: the bytes of a second frame; frames 2 and 4 repeat the
	 * one before, so the result is the 4th frame's buffer */
	const char *next;
	/* optional: the decoration settings of kmscon.conf */
	const struct kmscon_conf_t *conf;
};

/* the ADR-102 decoration settings the deco-* screens configure */
static const struct kmscon_conf_t conf_deco = {
	.underline_color = {.kind = KMSCON_COLOR_INDEX, .index = 196},
	.strike_color = {.kind = KMSCON_COLOR_RGB, .rgb = {0x00, 0xc0, 0xff}},
};
static const struct kmscon_conf_t conf_deco_force = {
	.underline_color = {.kind = KMSCON_COLOR_INDEX, .index = 196},
	.strike_color = {.kind = KMSCON_COLOR_RGB, .rgb = {0x00, 0xc0, 0xff}},
	.force_decoration_colors = true,
};

/* UTF-8 */
#define E_ACUTE "\xc3\xa9"
#define A_TILDE "\xc3\xa3"
#define N_TILDE "\xc3\xb1"
#define COMB_GRAVE "\xcc\x80"
#define COMB_ACUTE "\xcc\x81"
#define COMB_TILDE "\xcc\x83"
#define COMB_DIAER "\xcc\x88"
#define HEART "\xe2\x9d\xa4"
#define VS15 "\xef\xb8\x8e"
#define VS16 "\xef\xb8\x8f"

#define DECO_CONFIG_BYTES                                                                          \
	ESC "[4mconfigured underline" ESC "[0m " ESC "[9mconfigured strike" ESC "[0m\r\n" ESC        \
	"[4:3;58:5:46mSGR 58 green" ESC "[0m " ESC "[4:4;58:2::255:255:0mSGR 58 yellow" ESC         \
	"[0m\r\n" ESC "[32;4mgreen text" ESC "[0m " ESC "[33;9mstruck yellow text" ESC "[0m\r\n" ESC \
	"[4:2;9mdouble and strike" ESC "[0m\r\n"

static const struct screen screens[] = {
	{"ascii",
	 "Printable ASCII in the regular face: a pangram in both cases, digits and punctuation, "
	 "runs of narrow and wide letters, and a line of code. Correct: every glyph whole and "
	 "inside its own cell, with columns aligned down the screen.",
	 "The quick brown fox jumps over the lazy dog.\r\n"
	 "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG.\r\n"
	 "0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\r\n"
	 "iiiiiiii llllllll WWWWWWWW mmmmmmmm\r\n"
	 "func main() { return 0; }  // 80x24 \r\n"},

	{"bold",
	 "Bold beside regular, a bold pangram over two lines, and bold in red, green and blue. "
	 "Correct: the bold text drawn in the bold face at the same cell width, the coloured words"
	 " in their colours.",
	 "regular " ESC "[1mbold" ESC "[0m regular\r\n" ESC "[1mThe quick brown fox jumps over\r\n"
	 "the lazy dog 0123456789 !@#$%^&*()" ESC "[0m\r\n" ESC "[1;31mbold red" ESC
	 "[0m " ESC "[1;32mbold green" ESC "[0m " ESC "[1;34mbold blue" ESC "[0m\r\n"},

	{"palette",
	 "The 16 ANSI background colours, the 16 foreground colours on letter pairs, a ramp "
	 "through the 256-colour palette, and a 24-bit gradient from red to green. Correct: each "
	 "block filled edge to edge in its own colour, with no gaps between cells.",
	 /* 16 ANSI backgrounds, then foregrounds, then a 256-colour ramp and a
	  * truecolour gradient */
	 ESC "[40m  " ESC "[41m  " ESC "[42m  " ESC "[43m  " ESC "[44m  " ESC "[45m  " ESC
	 "[46m  " ESC "[47m  " ESC "[100m  " ESC "[101m  " ESC "[102m  " ESC "[103m  " ESC
	 "[104m  " ESC "[105m  " ESC "[106m  " ESC "[107m  " ESC "[0m\r\n" ESC "[30mAB" ESC
	 "[31mAB" ESC "[32mAB" ESC "[33mAB" ESC "[34mAB" ESC "[35mAB" ESC "[36mAB" ESC
	 "[37mAB" ESC "[90mAB" ESC "[91mAB" ESC "[92mAB" ESC "[93mAB" ESC "[94mAB" ESC
	 "[95mAB" ESC "[96mAB" ESC "[97mAB" ESC "[0m\r\n" ESC "[48;5;16m  " ESC "[48;5;52m  " ESC
	 "[48;5;88m  " ESC "[48;5;124m  " ESC "[48;5;160m  " ESC "[48;5;196m  " ESC
	 "[48;5;202m  " ESC "[48;5;214m  " ESC "[48;5;226m  " ESC "[48;5;154m  " ESC
	 "[48;5;46m  " ESC "[48;5;48m  " ESC "[48;5;51m  " ESC "[48;5;27m  " ESC
	 "[48;5;21m  " ESC "[48;5;129m  " ESC "[48;5;232m  " ESC "[48;5;244m  " ESC
	 "[48;5;255m  " ESC "[0m\r\n" ESC "[48;2;255;0;0m  " ESC "[48;2;223;32;0m  " ESC
	 "[48;2;191;64;0m  " ESC "[48;2;159;96;0m  " ESC "[48;2;127;128;0m  " ESC
	 "[48;2;95;160;0m  " ESC "[48;2;63;192;0m  " ESC "[48;2;31;224;0m  " ESC
	 "[48;2;0;255;0m  " ESC "[0m\r\n"},

	{"inverse",
	 "Reverse video (SGR 7): one word, a whole line, red on green swapped, and bold with "
	 "reverse. Correct: foreground and background exchanged, so the swapped word is green on "
	 "red.",
	 "normal " ESC "[7minverse" ESC "[0m normal\r\n" ESC "[7mWhole line inverted" ESC
	 "[0m\r\n" ESC "[31;42mred on green " ESC "[7mswapped" ESC "[0m\r\n" ESC
	 "[1;7mbold inverse" ESC "[0m\r\n"},

	{"cjk",
	 "Double-width Japanese, Chinese and Korean, full-width Latin, narrow letters between wide"
	 " characters, and wide text in bold and in reverse. Correct: each wide character takes "
	 "two cells and the narrow letters between them keep their own columns.",
	 "日本語の端末表示 テスト\r\n"
	 "中文字符 漢字 ひらがな カタカナ\r\n"
	 "한글 테스트\r\n"
	 "ＡＢＣＤ０１２３ abcd 0123\r\n"
	 "a日b本c語d\r\n"
	 ESC "[1m日本語 bold" ESC "[0m " ESC "[7m日本語 inverse" ESC "[0m\r\n"},

	{"box",
	 "Box drawing in light and double lines, shades and block elements, eighth blocks, and "
	 "rounded, heavy and diagonal pieces. Correct: lines join from cell to cell with no gaps, "
	 "and blocks fill their cells.",
	 "┌──────┬──────┐ ╔══════╦══════╗\r\n"
	 "│ cell │ cell │ ║ cell ║ cell ║\r\n"
	 "├──────┼──────┤ ╠══════╬══════╣\r\n"
	 "│      │      │ ║      ║      ║\r\n"
	 "└──────┴──────┘ ╚══════╩══════╝\r\n"
	 "░░▒▒▓▓██ ▀▄▌▐ ▁▂▃▄▅▆▇█ ▏▎▍▌▋▊▉█\r\n"
	 "╭─╮ ╰─╯ ┏━┓ ┗━┛ ┃ ╱╲╳\r\n"},

	/* ADR-102: each underline style in the foreground and in an SGR 58
	 * colour, over descenders */
	{"underline",
	 "Each underline style (single, double, curly, dotted, dashed) in the text colour and in "
	 "an SGR 58 colour, over descenders; a red curl ended by SGR 59; underlines on a "
	 "background, in reverse and in bold. Correct: the five styles distinct, the coloured ones"
	 " in their SGR 58 colour, and the line after SGR 59 in the text colour.",
	 ESC "[4mSingle gjpqy" ESC "[0m  " ESC "[4;58:5:196mSingle gjpqy" ESC "[0m\r\n" ESC
	 "[4:2mDouble gjpqy" ESC "[0m  " ESC "[4:2;58:2::0:200:255mDouble gjpqy" ESC "[0m\r\n" ESC
	 "[4:3mCurly  gjpqy" ESC "[0m  " ESC "[4:3;58:5:46mCurly  gjpqy" ESC "[0m\r\n" ESC
	 "[4:4mDotted gjpqy" ESC "[0m  " ESC "[4:4;58:2::255:128:0mDotted gjpqy" ESC "[0m\r\n" ESC
	 "[4:5mDashed gjpqy" ESC "[0m  " ESC "[4:5;58:5:201mDashed gjpqy" ESC "[0m\r\n" ESC
	 "[31;4:3;58:5:226mred, yellow curl" ESC "[59m then fg" ESC "[0m\r\n" ESC
	 "[44;4mon blue" ESC "[0m " ESC "[7;4minverse" ESC "[0m " ESC "[1;4:2mbold double" ESC
	 "[0m\r\n"},

	{"strike",
	 "Strike-through (SGR 9) alone, in colour, on a background, with straight and curly "
	 "underlines, in bold, in reverse, and over spaces. Correct: a line through the middle of "
	 "every struck cell, the spaces included.",
	 ESC "[9mstruck through gjpqy" ESC "[0m plain\r\n" ESC "[9;31mred strike" ESC "[0m " ESC
	 "[9;42mstrike on green" ESC "[0m\r\n" ESC "[9;4mstrike and underline" ESC "[0m " ESC
	 "[9;4:3;58:5:51mcurly too" ESC "[0m\r\n" ESC "[1;9mbold struck" ESC "[0m " ESC
	 "[7;9minverse struck" ESC "[0m\r\n" ESC "[9m      " ESC "[0m struck spaces\r\n"},

	/* underline-color=196, strike-color=#00c0ff */
	{"deco-config",
	 "Underline and strike with the configured colours (underline-color=196, "
	 "strike-color=#00c0ff), beside SGR 58 colours and coloured text. Correct: underlines and "
	 "strikes without an SGR 58 colour take the configured colours; an SGR 58 colour still "
	 "applies.",
	 DECO_CONFIG_BYTES, NULL, &conf_deco},
	/* the same, with force-decoration-colors */
	{"deco-force",
	 "The deco-config screen with force-decoration-colors set. Correct: every underline and "
	 "strike in the configured colour, the SGR 58 ones included.",
	 DECO_CONFIG_BYTES, NULL, &conf_deco_force},

	/* clusters shaped by HarfBuzz: decomposed against precomposed */
	{"combining",
	 "Accented letters precomposed and as a base letter with combining marks, in words, "
	 "underlined, struck and in bold, shaped by HarfBuzz. Correct: each mark sits on its base "
	 "letter in one cell, matching the precomposed form.",
	 "precomposed: " E_ACUTE " " A_TILDE " " N_TILDE "\r\n"
	 "decomposed:  e" COMB_ACUTE " a" COMB_TILDE COMB_GRAVE " n" COMB_DIAER "\r\n"
	 "in words: cafe" COMB_ACUTE " na" COMB_TILDE COMB_GRAVE "o n" COMB_DIAER "ice\r\n" ESC
	 "[4mu" COMB_DIAER "nderlined e" COMB_ACUTE ESC "[0m " ESC "[9mstruck a" COMB_TILDE
	 COMB_GRAVE ESC "[0m\r\n"
	 "bold: " ESC "[1me" COMB_ACUTE " a" COMB_TILDE COMB_GRAVE " n" COMB_DIAER ESC "[0m\r\n"},

	/* a heart with VS16 takes two cells; until colour glyphs (step 4c) it
	 * draws as the text heart, centred in them */
	{"vs16",
	 "A heart with VS16 (two cells), with no selector and with VS15 (one cell each), and the "
	 "VS16 heart underlined. Correct: the b after the VS16 heart one column further right; "
	 "until colour glyphs land, that heart is the text glyph centred in its two cells.",
	 "a" HEART VS16 "b  VS16, wide\r\n"
	 "a" HEART "b  no selector, narrow\r\n"
	 "a" HEART VS15 "b  VS15, narrow\r\n" ESC "[4ma" HEART VS16 "b" ESC "[0m  underlined\r\n"},

	/* decorated wide cells take each sprite twice; the last one is cut at
	 * the margin */
	{"wide-deco",
	 "Wide characters under each underline style and under strike, a decorated run mixing "
	 "narrow and wide, and a decorated wide character placed in the last column. Correct: "
	 "every decoration spans both cells of its wide character; the one in the last column is "
	 "cut at the edge.",
	 ESC "[4m日本語" ESC "[0m " ESC "[4:3m漢字" ESC "[0m " ESC "[4:4;58:5:196mカタカナ" ESC
	 "[0m\r\n" ESC "[4:2m한글" ESC "[0m " ESC "[4:5m中文" ESC "[0m " ESC "[9m日本語" ESC
	 "[0m\r\n" ESC "[4ma日b本c" ESC "[0m\r\n" ESC "[5;48H" ESC "[4;9m語" ESC "[0m"},

	/* frame N changes cells that frame N+1 must draw again into the other
	 * buffer */
	{"redraw",
	 "Two frames into the double-buffered display: the second rewrites line 2 and adds a "
	 "struck line 4, and both buffers must then hold it. Correct: line 2 reads 'line 2 CHANGED"
	 " curly' with nothing left of the first frame's text.",
	 "unchanged line\r\n"
	 "line two, before\r\n"
	 "line three stays\r\n",
	 ESC "[2;1Hline 2 CHANGED " ESC "[4:3mcurly" ESC "[0m" ESC "[4;1H" ESC "[9mnew struck" ESC
	 "[0m"},
};

static void vte_write(struct tsm_vte *vte, const char *u8, size_t len, void *data) {}

static int write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	unsigned int x, y;

	if (!f)
		return -errno;
	fprintf(f, "P6\n%u %u\n255\n", fb_w, fb_h);
	for (y = 0; y < fb_h; y++) {
		for (x = 0; x < fb_w; x++) {
			uint32_t p = fb[y * fb_w + x];
			uint8_t rgb[3] = {p >> 16, p >> 8, p};

			fwrite(rgb, 1, 3, f);
		}
	}
	return fclose(f) ? -errno : 0;
}

static int write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -errno;
	fprintf(f, "%s\n", text);
	return fclose(f) ? -errno : 0;
}

static void draw_frame(struct kmscon_text *txt, struct tsm_screen *con, struct tsm_vte *vte,
		       const struct kmscon_conf_t *conf)
{
	struct tsm_screen_attr attr;
	struct kmscon_text_deco deco;
	uint32_t *tmp;
	int ret;

	if (conf) {
		kmscon_conf_resolve_deco(conf, vte, &deco);
		kmscon_text_set_decorations(txt, &deco);
	}
	tsm_vte_get_def_attr(vte, &attr);
	ret = kmscon_text_prepare(txt, &attr, false);
	assert(ret == 0);
	ret = kmscon_text_draw(txt, con, false);
	assert(ret == 0);
	ret = kmscon_text_render(txt);
	assert(ret == 0);

	/* swap: the next frame draws into the other buffer */
	tmp = fb;
	fb = fb_back;
	fb_back = tmp;
	fake_d2d.rb[1].map = fb;
}

static int render_screen(struct kmscon_font *font, const struct screen *s, const char *out_dir)
{
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct kmscon_text *txt;
	char path[4096];
	int ret;

	ret = tsm_screen_new(&con, NULL, NULL);
	assert(ret == 0);
	ret = tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL);
	assert(ret == 0);
	tsm_vte_set_palette(vte, "default");
	ret = tsm_screen_resize(con, COLS, ROWS);
	assert(ret == 0);

	/* hidden cursor: its blink state is not part of what is compared */
	tsm_vte_input(vte, ESC "[?25l", 6);
	tsm_vte_input(vte, s->bytes, strlen(s->bytes));

	ret = kmscon_text_new(&txt, "bbulk", OR_NORMAL, &fake_disp);
	assert(ret == 0);
	ret = kmscon_text_set(txt, font);
	assert(ret == 0);
	assert(txt->cols == COLS && txt->rows == ROWS);

	draw_frame(txt, con, vte, s->conf);
	if (s->next) {
		draw_frame(txt, con, vte, s->conf);
		tsm_vte_input(vte, s->next, strlen(s->next));
		draw_frame(txt, con, vte, s->conf);
		draw_frame(txt, con, vte, s->conf);
		/* both buffers now hold the last frame */
		if (memcmp(fb, fb_back, (size_t)fb_w * fb_h * sizeof(*fb))) {
			fprintf(stderr, "test_pixels: %s: the two buffers differ after the last frame\n",
				s->name);
			ret = -EINVAL;
		}
	}

	/* the buffer last drawn */
	snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, s->name);
	fb = fb_back;
	if (!ret)
		ret = write_ppm(path);
	if (ret)
		fprintf(stderr, "test_pixels: cannot write %s: %s\n", path, strerror(-ret));
	fb = fake_d2d.rb[1].map;

	snprintf(path, sizeof(path), "%s/%s.txt", out_dir, s->name);
	if (!ret)
		ret = write_text(path, s->desc);
	if (ret)
		fprintf(stderr, "test_pixels: cannot write %s: %s\n", path, strerror(-ret));

	kmscon_text_unref(txt);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	return ret;
}

static void check_font_files(struct kmscon_font *font, const char *font_dir);

/* Replace the process's fontconfig with one that knows only the pinned files. */
static void pin_fonts(const char *font_dir)
{
	static const char *const files[] = {"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf",
					    "NotoSansMonoCJKJP-Subset.otf"};
	FcConfig *cfg = FcConfigCreate();
	char path[4096];

	assert(cfg);
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", font_dir, files[i]);
		if (!FcConfigAppFontAddFile(cfg, (const FcChar8 *)path)) {
			fprintf(stderr, "test_pixels: cannot load font %s\n", path);
			exit(2);
		}
	}
	if (!FcConfigSetCurrent(cfg)) {
		fprintf(stderr, "test_pixels: cannot install the font configuration\n");
		exit(2);
	}
}

/* Every font the backend can draw from, regular and bold, must be a pinned file. */
static void check_font_files(struct kmscon_font *font, const char *font_dir)
{
	struct ft_data *ftd = font->data;
	FcFontSet *sets[2] = {ftd->regular.fc, ftd->bold.fc};
	char dir[PATH_MAX], file[PATH_MAX];

	if (!realpath(font_dir, dir)) {
		fprintf(stderr, "test_pixels: bad font dir %s\n", font_dir);
		exit(2);
	}
	for (int k = 0; k < 2; k++) {
		assert(sets[k] && sets[k]->nfont > 0);
		for (int i = 0; i < sets[k]->nfont; i++) {
			FcChar8 *path = NULL;

			if (FcPatternGetString(sets[k]->fonts[i], FC_FILE, 0, &path) != FcResultMatch ||
			    !realpath((char *)path, file) || strncmp(file, dir, strlen(dir)) != 0 ||
			    file[strlen(dir)] != '/') {
				fprintf(stderr, "test_pixels: font %s is not under %s\n",
					path ? (char *)path : "(none)", dir);
				exit(2);
			}
		}
	}
}

int main(int argc, char **argv)
{
	struct kmscon_font *font = NULL;
	int ret, failed = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: %s FONT_DIR OUT_DIR\n", argv[0]);
		return 2;
	}

	pin_fonts(argv[1]);

	ret = kmscon_font_register(&kmscon_font_freetype_ops);
	assert(ret == 0);
	ret = kmscon_text_register(&kmscon_text_bbulk_ops);
	assert(ret == 0);

	ret = kmscon_font_find(&font, "DejaVu Sans Mono", FONT_PX, "freetype");
	if (ret || !font || font->ops != &kmscon_font_freetype_ops) {
		fprintf(stderr, "test_pixels: pinned font not found\n");
		return 2;
	}

	fb_w = COLS * font->width;
	fb_h = ROWS * font->height;
	fb = calloc((size_t)fb_w * fb_h, sizeof(*fb));
	fb_back = calloc((size_t)fb_w * fb_h, sizeof(*fb));
	assert(fb && fb_back);
	fake_display_init();
	check_font_files(font, argv[1]);

	for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); i++)
		failed |= !!render_screen(font, &screens[i], argv[2]);

	kmscon_font_unref(font);
	free(fb);
	free(fb_back);
	return failed ? 1 : 0;
}
