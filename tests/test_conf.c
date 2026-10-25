#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/misc/conf.c"
#include "config.h"
#include "render/text.h"
#undef LOG_SUBSYSTEM
#include "../src/config.c"

static char *config_file = "verbose\n"
			   "mode=pci-0000:0d:00.0[HDMI-1]1280x1024@60\n"
			   "mode=[DP-1]1024x768@120\n"
			   "mode=pci-0000:06:00.0[eDP]3440x1440\n"
			   "mode=[VGA-1]800x600\n"
			   "mode=1024x768@50\n"
			   "mode=640x480\n";

static struct kmscon_conf_mode parsed_modes[] = {
	{
		.gpu = "pci-0000:0d:00.0",
		.connector = "HDMI-1",
		.width = 1280,
		.height = 1024,
		.refresh_rate = 60,
	},
	{
		.gpu = "",
		.connector = "DP-1",
		.width = 1024,
		.height = 768,
		.refresh_rate = 120,
	},
	{
		.gpu = "pci-0000:06:00.0",
		.connector = "eDP",
		.width = 3440,
		.height = 1440,
		.refresh_rate = 0,
	},
	{
		.gpu = "",
		.connector = "VGA-1",
		.width = 800,
		.height = 600,
		.refresh_rate = 0,
	},
	{
		.gpu = "",
		.connector = "",
		.width = 1024,
		.height = 768,
		.refresh_rate = 50,
	},
	{
		.gpu = "",
		.connector = "",
		.width = 640,
		.height = 480,
		.refresh_rate = 0,
	},
};

void test_parse_mode(const char *config, struct kmscon_conf_mode *mode, int num_modes)
{
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;
	int i;
	char *buf;

	kmscon_conf_new(&ctx);
	conf_ctx_reset(ctx);

	buf = malloc(strlen(config) + 1);
	strcpy(buf, config);

	parse_buffer(ctx->opts, ctx->onum, buf, strlen(buf));
	conf = conf_ctx_get_mem(ctx);
	free(buf);

	assert(conf->mode_count == num_modes);

	for (i = 0; i < num_modes; i++) {
		assert(strcmp(conf->modes[i].gpu, mode[i].gpu) == 0);
		assert(strcmp(conf->modes[i].connector, mode[i].connector) == 0);
		assert(conf->modes[i].width == mode[i].width);
		assert(conf->modes[i].height == mode[i].height);
		assert(conf->modes[i].refresh_rate == mode[i].refresh_rate);
	}
	kmscon_conf_free(ctx);
}

static char *screen_conf = "screen=normal\n"
			   "screen=pci-00d.00.0[HDMI-A-1]disabled\n"
			   "screen=[VGA-1]upside-down\n";

static struct kmscon_conf_screen parsed_screens[] = {
	{"", "", true, OR_NORMAL},
	{"pci-00d.00.0", "HDMI-A-1", false, OR_NORMAL},
	{"", "VGA-1", true, OR_UPSIDE_DOWN},
};

void test_parse_screen(const char *config, struct kmscon_conf_screen *screen, int num_screens)
{
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;
	int i;
	char *buf;

	kmscon_conf_new(&ctx);
	conf_ctx_reset(ctx);

	buf = malloc(strlen(config) + 1);
	strcpy(buf, config);

	parse_buffer(ctx->opts, ctx->onum, buf, strlen(buf));
	conf = conf_ctx_get_mem(ctx);
	free(buf);

	assert(conf->screen_count == num_screens);

	for (i = 0; i < num_screens; i++) {
		assert(strcmp(conf->screens[i].gpu, screen[i].gpu) == 0);
		assert(strcmp(conf->screens[i].connector, screen[i].connector) == 0);
		assert(conf->screens[i].enabled == screen[i].enabled);
		assert(conf->screens[i].orientation == screen[i].orientation);
	}
	kmscon_conf_free(ctx);
}

/* the context parse_conf() made last, freed when it makes the next and at the end */
static struct conf_ctx *last_ctx;

static struct kmscon_conf_t *parse_conf(struct conf_ctx **ctx, const char *config)
{
	char *buf;
	int ret;

	kmscon_conf_free(last_ctx);
	kmscon_conf_new(ctx);
	last_ctx = *ctx;
	conf_ctx_reset(*ctx);

	buf = strdup(config);
	ret = parse_buffer((*ctx)->opts, (*ctx)->onum, buf, strlen(buf));
	free(buf);
	return ret ? NULL : conf_ctx_get_mem(*ctx);
}

/* grapheme-width, underline-color, strike-color, force-decoration-colors, emoji-color */
static void test_parse_text(void)
{
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;

	/* defaults */
	conf = parse_conf(&ctx, "\n");
	assert(conf);
	assert(conf->grapheme_width == TSM_WIDTH_UNICODE);
	assert(conf->underline_color.kind == KMSCON_COLOR_FG);
	assert(conf->strike_color.kind == KMSCON_COLOR_FG);
	assert(!conf->force_decoration_colors);
	assert(conf->emoji_color);

	conf = parse_conf(&ctx, "grapheme-width=wcwidth\n"
				"underline-color=196\n"
				"strike-color=#12ab9F\n"
				"force-decoration-colors\n"
				"no-emoji-color\n");
	assert(conf);
	assert(conf->grapheme_width == TSM_WIDTH_WCWIDTH);
	assert(conf->underline_color.kind == KMSCON_COLOR_INDEX);
	assert(conf->underline_color.index == 196);
	assert(conf->strike_color.kind == KMSCON_COLOR_RGB);
	assert(conf->strike_color.rgb[0] == 0x12);
	assert(conf->strike_color.rgb[1] == 0xab);
	assert(conf->strike_color.rgb[2] == 0x9f);
	assert(conf->force_decoration_colors);
	assert(!conf->emoji_color);

	conf = parse_conf(&ctx, "grapheme-width=unicode\nunderline-color=fg\n"
				"no-force-decoration-colors\nemoji-color\n");
	assert(conf);
	assert(conf->grapheme_width == TSM_WIDTH_UNICODE);
	assert(conf->underline_color.kind == KMSCON_COLOR_FG);
	assert(!conf->force_decoration_colors);
	assert(conf->emoji_color);

	/* rejected values */
	assert(!parse_conf(&ctx, "grapheme-width=both\n"));
	assert(!parse_conf(&ctx, "underline-color=256\n"));
	assert(!parse_conf(&ctx, "underline-color=#12345\n"));
	assert(!parse_conf(&ctx, "underline-color=#12345g\n"));
	assert(!parse_conf(&ctx, "strike-color=red\n"));
	assert(!parse_conf(&ctx, "underline-color=0x10\n"));
	assert(!parse_conf(&ctx, "underline-color=-0\n"));

	/* an index is decimal: a leading 0 is not octal */
	conf = parse_conf(&ctx, "underline-color=010\nstrike-color=08\n");
	assert(conf);
	assert(conf->underline_color.kind == KMSCON_COLOR_INDEX);
	assert(conf->underline_color.index == 10);
	assert(conf->strike_color.kind == KMSCON_COLOR_INDEX);
	assert(conf->strike_color.index == 8);
}

/* a '#' opens a comment, unless it begins a #rrggbb value */
static void test_parse_comment(void)
{
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;

	conf = parse_conf(&ctx, "font-name= # comment\n");
	assert(conf);
	assert(!strcmp(conf->font_name, ""));

	conf = parse_conf(&ctx, "font-name=#comment\n");
	assert(conf);
	assert(!strcmp(conf->font_name, ""));

	conf = parse_conf(&ctx, "font-name=mono # comment\n");
	assert(conf);
	assert(!strcmp(conf->font_name, "mono"));

	conf = parse_conf(&ctx, "font-name= #abcdef0\n");
	assert(conf);
	assert(!strcmp(conf->font_name, ""));

	conf = parse_conf(&ctx, "underline-color= #00ff80 # green\nstrike-color=#0000ff#blue\n");
	assert(conf);
	assert(conf->underline_color.kind == KMSCON_COLOR_RGB);
	assert(conf->underline_color.rgb[0] == 0x00);
	assert(conf->underline_color.rgb[1] == 0xff);
	assert(conf->underline_color.rgb[2] == 0x80);
	assert(conf->strike_color.kind == KMSCON_COLOR_RGB);
	assert(conf->strike_color.rgb[2] == 0xff);
}

/*
 * blink is on by default and no-blink turns it off; an option on the command
 * line wins over the file
 */
static void test_parse_blink(void)
{
	char *argv[] = {"kmscon", "--blink", NULL};
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;
	char *buf;

	conf = parse_conf(&ctx, "\n");
	assert(conf && conf->blink);
	conf = parse_conf(&ctx, "no-blink\n");
	assert(conf && !conf->blink);

	kmscon_conf_new(&ctx);
	conf_ctx_reset(ctx);
	assert(conf_ctx_parse_argv(ctx, 2, argv) == 0);
	buf = strdup("no-blink\n");
	assert(parse_buffer(ctx->opts, ctx->onum, buf, strlen(buf)) == 0);
	free(buf);
	conf = conf_ctx_get_mem(ctx);
	assert(conf->blink);
	kmscon_conf_free(ctx);
}

/* sync-timeout is 200 ms by default; 0 turns mode 2026 off (ADR-201) */
static void test_parse_sync_timeout(void)
{
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;

	conf = parse_conf(&ctx, "\n");
	assert(conf && conf->sync_timeout == 200);
	conf = parse_conf(&ctx, "sync-timeout=0\n");
	assert(conf && conf->sync_timeout == 0);
	conf = parse_conf(&ctx, "sync-timeout=1000\n");
	assert(conf && conf->sync_timeout == 1000);
}

/* render-timer takes foot's values; none is the default */
static void test_parse_render_timer(void)
{
	char *argv[] = {"kmscon", "--render-timer=both", NULL};
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;
	char *buf;

	conf = parse_conf(&ctx, "\n");
	assert(conf && conf->render_timer == KMSCON_RENDER_TIMER_NONE);
	conf = parse_conf(&ctx, "render-timer=none\n");
	assert(conf && conf->render_timer == KMSCON_RENDER_TIMER_NONE);
	conf = parse_conf(&ctx, "render-timer=osd\n");
	assert(conf && conf->render_timer == KMSCON_RENDER_TIMER_OSD);
	conf = parse_conf(&ctx, "render-timer=log\n");
	assert(conf && conf->render_timer == KMSCON_RENDER_TIMER_LOG);
	conf = parse_conf(&ctx, "render-timer=both\n");
	assert(conf && conf->render_timer == KMSCON_RENDER_TIMER_BOTH);
	assert(conf->render_timer & KMSCON_RENDER_TIMER_OSD);
	assert(conf->render_timer & KMSCON_RENDER_TIMER_LOG);
	conf = parse_conf(&ctx, "render-timer=log # comment\n");
	assert(conf && conf->render_timer == KMSCON_RENDER_TIMER_LOG);

	/* conf.c logs an option without its argument and keeps the default */
	conf = parse_conf(&ctx, "render-timer\n");
	assert(conf && conf->render_timer == KMSCON_RENDER_TIMER_NONE);
	assert(!parse_conf(&ctx, "render-timer=\n"));
	assert(!parse_conf(&ctx, "render-timer=LOG\n"));
	assert(!parse_conf(&ctx, "render-timer=on\n"));
	assert(!parse_conf(&ctx, "render-timer=log,osd\n"));

	/* the command line wins over the file; getopt restarts after test_parse_blink */
	kmscon_conf_new(&ctx);
	conf_ctx_reset(ctx);
	optind = 0;
	assert(conf_ctx_parse_argv(ctx, 2, argv) == 0);
	buf = strdup("render-timer=none\n");
	assert(parse_buffer(ctx->opts, ctx->onum, buf, strlen(buf)) == 0);
	free(buf);
	conf = conf_ctx_get_mem(ctx);
	assert(conf->render_timer == KMSCON_RENDER_TIMER_BOTH);
	kmscon_conf_free(ctx);
}

/*
 * The image options (ADR-103): image-storage-limit takes auto, bytes, or a
 * size in K, M or G; sixel and kitty-graphics are on unless turned off;
 * max-graphic-size and num-color-registers take xterm's forms within libtsm's
 * bounds. A bad value keeps nothing of it.
 */
static void test_parse_images(void)
{
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;

	conf = parse_conf(&ctx, "\n");
	assert(conf && conf->image_storage_limit == IMAGE_BUDGET_AUTO);
	assert(conf->sixel && conf->kitty_graphics && conf->private_color_registers);
	assert(conf->max_graphic_size.width == 10000 && conf->max_graphic_size.height == 10000);
	assert(conf->num_color_registers == 1024);

	conf = parse_conf(&ctx, "image-storage-limit=0\n");
	assert(conf && conf->image_storage_limit == 0);
	conf = parse_conf(&ctx, "image-storage-limit=123456\n");
	assert(conf && conf->image_storage_limit == 123456);
	conf = parse_conf(&ctx, "image-storage-limit=64K\n");
	assert(conf && conf->image_storage_limit == (size_t)64 << 10);
	conf = parse_conf(&ctx, "image-storage-limit=192M\n");
	assert(conf && conf->image_storage_limit == (size_t)192 << 20);
	conf = parse_conf(&ctx, "image-storage-limit=2G\n");
	assert(conf && conf->image_storage_limit == (size_t)2 << 30);
	conf = parse_conf(&ctx, "image-storage-limit=auto\n");
	assert(conf && conf->image_storage_limit == IMAGE_BUDGET_AUTO);
	assert(!parse_conf(&ctx, "image-storage-limit=\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=-1\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=12k\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=12MB\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=M\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=AUTO\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=99999999999999999999\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=17179869184G\n"));
	/* at most 1 TiB: a display's share is the limit times its pixels */
	conf = parse_conf(&ctx, "image-storage-limit=1024G\n");
	assert(conf && conf->image_storage_limit == (size_t)1 << 40);
	conf = parse_conf(&ctx, "image-storage-limit=1099511627776\n");
	assert(conf && conf->image_storage_limit == (size_t)1 << 40);
	assert(!parse_conf(&ctx, "image-storage-limit=1025G\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=2048G\n"));
	assert(!parse_conf(&ctx, "image-storage-limit=1099511627777\n"));

	conf = parse_conf(&ctx, "no-sixel\nno-kitty-graphics\nno-private-color-registers\n");
	assert(conf && !conf->sixel && !conf->kitty_graphics && !conf->private_color_registers);

	conf = parse_conf(&ctx, "max-graphic-size=1000x600\n");
	assert(conf && conf->max_graphic_size.width == 1000 &&
	       conf->max_graphic_size.height == 600);
	conf = parse_conf(&ctx, "max-graphic-size=65535x1\n");
	assert(conf && conf->max_graphic_size.width == 65535 && conf->max_graphic_size.height == 1);
	assert(!parse_conf(&ctx, "max-graphic-size=1000\n"));
	assert(!parse_conf(&ctx, "max-graphic-size=0x600\n"));
	assert(!parse_conf(&ctx, "max-graphic-size=65536x600\n"));
	assert(!parse_conf(&ctx, "max-graphic-size=1000x600x2\n"));
	assert(!parse_conf(&ctx, "max-graphic-size=-1x600\n"));

	conf = parse_conf(&ctx, "num-color-registers=2\n");
	assert(conf && conf->num_color_registers == 2);
	conf = parse_conf(&ctx, "num-color-registers=65535\n");
	assert(conf && conf->num_color_registers == 65535);
	assert(!parse_conf(&ctx, "num-color-registers=1\n"));
	assert(!parse_conf(&ctx, "num-color-registers=65536\n"));
	assert(!parse_conf(&ctx, "num-color-registers=16 colours\n"));
}

int main()
{
	test_parse_mode(config_file, parsed_modes, 6);
	test_parse_screen(screen_conf, parsed_screens, 3);
	test_parse_text();
	test_parse_comment();
	test_parse_blink();
	test_parse_sync_timeout();
	test_parse_render_timer();
	test_parse_images();
	kmscon_conf_free(last_ctx);

	return 0;
}
