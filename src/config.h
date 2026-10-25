/*
 * Main App
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * This includes global data for the whole kmscon application. For instance,
 * global parameters can be accessed via this header.
 */

#ifndef KMSCON_CONFIG_H
#define KMSCON_CONFIG_H

#include <libtsm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "conf.h"
#include "render/image_budget.h"
#include "render/text.h"

enum kmscon_conf_gpu_selection {
	KMSCON_GPU_ALL,
	KMSCON_GPU_AUX,
	KMSCON_GPU_PRIMARY,
};

typedef uint8_t palette_t[TSM_COLOR_NUM][3];

#define MAX_DISPLAY_NAME_LEN 64
struct kmscon_conf_mode {
	char gpu[MAX_DISPLAY_NAME_LEN];
	char connector[MAX_DISPLAY_NAME_LEN];
	uint32_t width;
	uint32_t height;
	uint32_t refresh_rate;
};

struct kmscon_conf_screen {
	char gpu[MAX_DISPLAY_NAME_LEN];
	char connector[MAX_DISPLAY_NAME_LEN];
	bool enabled;
	enum Orientation orientation;
};

/* max-graphic-size, in pixels */
struct kmscon_conf_graphic_size {
	unsigned int width;
	unsigned int height;
};

/* render-timer: what the render timer does with its figures (ADR-103) */
enum kmscon_conf_render_timer {
	KMSCON_RENDER_TIMER_NONE = 0,
	KMSCON_RENDER_TIMER_OSD = 1,
	KMSCON_RENDER_TIMER_LOG = 2,
	KMSCON_RENDER_TIMER_BOTH = KMSCON_RENDER_TIMER_OSD | KMSCON_RENDER_TIMER_LOG,
};

/*
 * A decoration colour (ADR-102): `fg` follows the cell's foreground, a palette
 * index 0-255 follows the palette in use, `#rrggbb` is fixed.
 */
enum kmscon_conf_color_kind {
	KMSCON_COLOR_FG,
	KMSCON_COLOR_INDEX,
	KMSCON_COLOR_RGB,
};

struct kmscon_conf_color {
	enum kmscon_conf_color_kind kind;
	uint8_t index;
	uint8_t rgb[3];
};

struct kmscon_conf_t {
	/* header information */
	bool seat_config;

	/* General Options */
	/* show help/usage information */
	bool help;
	/* show version information */
	bool version;
	/* exit application after parsing options */
	bool exit;
	/* enable verbose info messages */
	bool verbose;
	/* enable debug messages */
	bool debug;
	/* disable notices and warnings */
	bool silent;
	/* config directory name */
	char *configdir;

	/* Seat Options */
	/* VT number to run on */
	char *vt;
	/* enter new VT directly */
	bool switchvt;
	/* use libseat */
	bool libseat;

	/* Session Options */
	/* sessions */
	unsigned int session_max;
	/* allow keyboard session control */
	bool session_control;

	/* Terminal Options */
	/* display /etc/issue before login prompt */
	bool issue;
	/* colon-separated issue search path */
	char *issue_path;
	/* asciicast animation to play after opening the terminal session */
	char *asciicast;
	/* loop asciicast animation */
	bool asciicast_loop;
	/* custom login process */
	bool login;
	/* argv for login process */
	char **argv;
	/* when the login process exits, exit kmscon */
	bool oneshot;
	/* TERM value */
	char *term;
	/* reset environment */
	bool reset_env;
	/* Send delete when backspace is pressed */
	bool backspace_delete;
	/* terminal scroll-back buffer size */
	unsigned int sb_size;
	/* enable bell forwarding */
	bool bell;
	/* enable cursor and text blinking */
	bool blink;
	/* the longest a synchronized frame (mode 2026) is held, in ms; 0: not offered */
	unsigned int sync_timeout;

	/* Input Options */
	/* input KBD model */
	char *xkb_model;
	/* input KBD layout */
	char *xkb_layout;
	/* input KBD variant */
	char *xkb_variant;
	/* input KBD options */
	char *xkb_options;
	/* input predefined KBD keymap */
	char *xkb_keymap;
	/* input predefined KBD compose file */
	char *xkb_compose_file;
	/* keyboard key-repeat delay */
	unsigned int xkb_repeat_delay;
	/* keyboard key-repeat rate */
	unsigned int xkb_repeat_rate;
	/* Enable mouse support */
	bool mouse;
	/* Natural scrolling (invert mouse wheel direction) */
	bool natural_scrolling;
	/* Force software cursor */
	bool soft_cursor;
	/* DPMS screen timeout in seconds (0 = disabled) */
	unsigned int dpms_timeout;

	/* Grabs / Keyboard-Shortcuts */
	/* scroll-up grab */
	struct conf_grab *grab_scroll_up;
	/* scroll-down grab */
	struct conf_grab *grab_scroll_down;
	/* page-up grab */
	struct conf_grab *grab_page_up;
	/* page-down grab */
	struct conf_grab *grab_page_down;
	/* zoom-in grab */
	struct conf_grab *grab_zoom_in;
	/* zoom-out grab */
	struct conf_grab *grab_zoom_out;
	/* session-next grab */
	struct conf_grab *grab_session_next;
	/* session-prev grab */
	struct conf_grab *grab_session_prev;
	/* session-close grab */
	struct conf_grab *grab_session_close;
	/* terminal-new grab */
	struct conf_grab *grab_terminal_new;
	/* rotate output clock-wise grab */
	struct conf_grab *grab_rotate_cw;
	/* rotate output counter-clock-wise grab */
	struct conf_grab *grab_rotate_ccw;
	/* reboot system grab */
	struct conf_grab *grab_reboot;

	/* Video Options */
	/* use DRM if available */
	bool drm;
	/* use 3D hardware-acceleration if available */
	bool hwaccel;
	/* gpu selection mode */
	unsigned int gpus;
	/* use current KMS video mode to avoid modesetting */
	bool use_original_mode;
	/* Precise configuration of each display */
	unsigned int mode_count;
	struct kmscon_conf_mode *modes;
	/* multiple monitors */
	char *multi_monitor;
	/* screen configuration */
	unsigned int screen_count;
	struct kmscon_conf_screen *screens;

	/* Font Options */
	/* font engine */
	char *font_engine;
	/* font size */
	unsigned int font_size;
	/* font name */
	char *font_name;

	/* Palette Options */
	/* color palette */
	char *palette;
	/* custom palette */
	palette_t custom_palette;

	/* Text Options */
	/* how printed text is divided into cells */
	enum tsm_width_method grapheme_width;
	/* the colours of underline and strike-through */
	struct kmscon_conf_color underline_color;
	struct kmscon_conf_color strike_color;
	/* configured decoration colours win over SGR 58 */
	bool force_decoration_colors;
	/* emoji in colour; off draws them as text, in the foreground */
	bool emoji_color;

	/* Image Options (ADR-103) */
	/* bytes for every image and scaled picture in the process, or
	 * IMAGE_BUDGET_AUTO; 0: none */
	size_t image_storage_limit;
	/* sixel graphics, decoded and advertised */
	bool sixel;
	/* the kitty graphics protocol, decoded and answered */
	bool kitty_graphics;
	/* the largest image in pixels */
	struct kmscon_conf_graphic_size max_graphic_size;
	/* sixel colour registers, and the state mode 1070 starts in */
	unsigned int num_color_registers;
	bool private_color_registers;

	/* Debug Options */
	/* frames presented, CPU time and damage per frame: on screen, logged or both */
	enum kmscon_conf_render_timer render_timer;
};

/*
 * The decoration colours @conf configures, resolved for the renderer: a
 * palette index is looked up in @vte's palette, so it follows the palette in
 * use. Called on every frame.
 */
static inline void kmscon_conf_resolve_deco(const struct kmscon_conf_t *conf, struct tsm_vte *vte,
					    struct kmscon_text_deco *out)
{
	const struct kmscon_conf_color *in[2] = {&conf->underline_color, &conf->strike_color};
	bool *fg[2] = {&out->ul_fg, &out->strike_fg};
	struct tsm_screen_color *rgb[2] = {&out->ul, &out->strike};
	uint8_t c[3];
	int i;

	memset(out, 0, sizeof(*out));
	out->force = conf->force_decoration_colors;
	for (i = 0; i < 2; i++) {
		*fg[i] = true;
		if (in[i]->kind == KMSCON_COLOR_RGB) {
			memcpy(c, in[i]->rgb, 3);
		} else if (in[i]->kind != KMSCON_COLOR_INDEX ||
			   tsm_vte_get_palette_color(vte, in[i]->index, c)) {
			continue;
		}
		*fg[i] = false;
		rgb[i]->r = c[0];
		rgb[i]->g = c[1];
		rgb[i]->b = c[2];
	}
}

int kmscon_conf_new(struct conf_ctx **out);
void kmscon_conf_free(struct conf_ctx *ctx);
int kmscon_conf_load_main(struct conf_ctx *ctx, int argc, char **argv);
int kmscon_conf_load_seat(struct conf_ctx *ctx, const struct conf_ctx *main, const char *seat);

bool config_get_mode(struct kmscon_conf_t *conf, const char *gpu, const char *connector,
		     unsigned int *width, unsigned int *height, unsigned int *rate);
void config_get_screen(struct kmscon_conf_t *conf, const char *gpu, const char *connector,
		       enum Orientation *orientation, bool *enabled);

#endif /* KMSCON_CONFIG_H */
