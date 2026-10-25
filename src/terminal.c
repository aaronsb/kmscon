/*
 * kmscon - Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
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
 * Terminal
 * A terminal gets assigned an input stream and several output objects and then
 * runs a fully functional terminal emulation on it.
 */

#include <errno.h>
#include <inttypes.h>
#include <libtsm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "asciinema.h"
#include "conf.h"
#include "config.h"
#include "font/font.h"
#include "input/input.h"
#include "issue.h"
#include "pty.h"
#include "render/image_budget.h"
#include "render/text.h"
#include "render_timer.h"
#include "seat.h"
#include "shl/dlist.h"
#include "shl/eloop.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "terminal.h"
#include "terminal_image.h"
#include "terminal_sync.h"
#include "terminal_vte.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "terminal"

#define IMAGE_HELPER BUILD_MODULE_DIR "/kmscon-image-helper"

struct screen {
	struct dlist list;
	struct kmscon_terminal *term;
	struct display *disp;
	struct kmscon_text *txt;
	struct render_timer *timer; /* NULL while render-timer is none */

	bool config_enabled;
	bool pending;
	/* the pending draw is for a display that lost its picture (terminal_sync.h) */
	bool lost;
	/* a draw of the child's output waiting for its write to end (terminal_sync.h) */
	struct terminal_coalesce_display coalesce;
	/* its failed font set, for the timer that asks for a frame to try it again */
	struct terminal_retry retry;
	bool hw_cursor;
	bool enabled;
	bool scaled;
};

struct kmscon_pointer {
	bool visible;
	bool select;
	int32_t x;
	int32_t y;
	unsigned int posx;
	unsigned int posy;
	char *copy;
	int copy_len;
};

struct kmscon_terminal {
	unsigned long ref;
	struct ev_eloop *eloop;
	struct input *input;
	bool opened;
	bool awake;

	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	struct kmscon_session *session;

	struct dlist screens;
	unsigned int cols;
	unsigned int rows;

	struct tsm_screen *console;
	struct tsm_vte *vte;
	struct kmscon_pty *pty;
	struct ev_fd *ptyfd;
	/* the child's output was parsed this loop turn, and is drawn at its end */
	bool redraw_owed;
	/* synchronized output (mode 2026): when to present, and the hold's timer */
	struct terminal_sync sync;
	struct ev_timer *sync_timer;
	/* the pty holds output the last input call left */
	bool unparsed;
	/* the child's output parsed this loop turn, for the waits of its draws */
	struct terminal_coalesce coalesce;
	/* ends the earliest wait for a write's end; armed for coalesce_due, 0: disarmed */
	struct ev_timer *coalesce_timer;
	uint64_t coalesce_due;
	/* asks for a frame when a screen whose font set failed may try it again */
	struct ev_timer *set_retry_timer;
	/* the screen in the image budget, and sixel while every display draws images */
	struct image_budget_terminal images;
	/* the cell size libtsm and TIOCSWINSZ were last given */
	unsigned int cell_w;
	unsigned int cell_h;
	/*
	 * Reads kitty graphics files and shared memory and decodes PNG and zlib
	 * as the session's user; while it has a request out, the pty and
	 * asciicast playback are paused (ADR-103, Notes of 2026-10-24).
	 */
	struct terminal_image image;

	struct kmscon_font *font;
	/* Automatically adjust font size to fit terminal size
	 * set to false if font-size is set in kmscon.conf, or
	 * after zoom in/out
	 */
	bool auto_font_size;
	unsigned int font_size;

	struct kmscon_pointer pointer;

	struct ev_timer *blink_timer;
	struct ev_timer *blink_cursor;
	bool blinking;
	bool cursor_blinking;

	/* closes each screen's render timer interval, logs it and shows it */
	struct ev_timer *render_timer;

	struct kmscon_asciinema *asciinema;
};

#define BLINK_TIMER_NS (500 * 1000 * 1000) // Blinking interval 500ms
#define BLINK_CURSOR_TYPING 1		   // After keypress, wait 1s before blinking the cursor
/*
 * The render timer's interval, one second as ADR-103 sets: its counts read
 * as rates per second, and at video rates (24 to 60 frames per second) one
 * interval holds enough frames for a median and a 95th percentile.
 */
#define RENDER_TIMER_NS (1000 * 1000 * 1000)

static void coord_to_cell(struct kmscon_terminal *term, int32_t x, int32_t y, unsigned int *posx,
			  unsigned int *posy)
{
	int w = term->cols;
	int h = term->rows;
	int fw, fh;

	if (!term->font || !w || !h) {
		*posx = 0;
		*posy = 0;
		return;
	}
	fw = term->font->width;
	fh = term->font->height;

	*posx = min(x / fw, w - 1);
	*posy = min(y / fh, h - 1);
}

static void draw_pointer(struct screen *scr)
{
	if (!scr->term->pointer.visible || scr->hw_cursor || !scr->term->font)
		return;

	if (scr->scaled) {
		int32_t x, y;

		x = (scr->term->pointer.x * (int32_t)FONT_WIDTH(scr->txt)) / scr->term->font->width;
		y = (scr->term->pointer.y * (int32_t)FONT_HEIGHT(scr->txt)) /
		    scr->term->font->height;

		kmscon_text_draw_pointer(scr->txt, x, y);
	} else
		kmscon_text_draw_pointer(scr->txt, scr->term->pointer.x, scr->term->pointer.y);
}

static inline uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * Thin I-beam: 1px stem, proportional top/bottom serifs, 1px outline halo.
 * The shape is built into a boolean mask, then rendered in a single pass
 * that fills white on the shape and a dark outline on any pixel adjacent
 * to it (8-connected).
 */
static uint32_t *generate_ibeam_cursor(unsigned int font_height, unsigned int *width,
				       unsigned int *height, bool rotate)
{
	unsigned int h, w;
	int i, x, y, ny, nx, thk;
	bool *shape, near;
	uint32_t white, outline;
	uint32_t *pixels;

	h = font_height > 8 ? font_height : 8;
	h = min(h, VIDEO_CURSOR_MAX_SIZE);
	thk = 1 + (h / 16);
	w = 2 * (h / 6) + 3 * thk;

	white = argb(255, 255, 255, 255);
	outline = argb(220, 0, 0, 0);

	shape = calloc(w, h * sizeof(*shape));
	if (!shape)
		return NULL;

	pixels = calloc(w, h * sizeof(*pixels));
	if (!pixels) {
		free(shape);
		return NULL;
	}

	if (rotate) {
		unsigned tmp = w;
		w = h;
		h = tmp;
		/* vertical stem */
		for (x = thk; x < w - thk; x++)
			for (i = 0; i < thk; i++)
				shape[x + ((h - thk) / 2) * w + i * w] = true;

		/* Top and bottom serifs */
		for (y = thk; y < h - thk; y++) {
			for (i = 0; i < thk; i++) {
				shape[i + thk + y * w] = true;
				shape[w - i - 1 - thk + y * w] = true;
			}
		}
	} else {
		/* vertical stem */
		for (y = thk; y < h - thk; y++)
			for (i = 0; i < thk; i++)
				shape[(w - thk) / 2 + y * w + i] = true;

		/* Top and bottom serifs */
		for (x = thk; x < w - thk; x++) {
			for (i = 0; i < thk; i++) {
				shape[w * (i + thk) + x] = true;
				shape[(h - i - 1 - thk) * w + x] = true;
			}
		}
	}

	/* White fill on the shape, dark halo on 8-connected neighbors */
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			if (shape[y * w + x]) {
				pixels[y * w + x] = white;
				continue;
			}
			near = false;
			for (ny = y - thk; ny <= y + thk && !near; ny++) {
				for (nx = x - thk; nx <= x + thk && !near; nx++) {
					if (ny >= 0 && ny < (int)h && nx >= 0 && nx < (int)w &&
					    shape[ny * w + nx])
						near = true;
				}
			}
			if (near)
				pixels[y * w + x] = outline;
		}
	}
	free(shape);
	*width = w;
	*height = h;
	return pixels;
}

static void setup_hw_cursor(struct screen *scr)
{
	bool rotate = scr->txt->orientation == OR_LEFT || scr->txt->orientation == OR_RIGHT;
	unsigned int beam_h;
	unsigned int beam_w;
	uint32_t *pixels;
	int ret;

	if (!scr->txt->font)
		return;

	pixels = generate_ibeam_cursor(scr->txt->font->height, &beam_w, &beam_h, rotate);
	if (!pixels)
		return;

	ret = display_setup_cursor(scr->disp, pixels, beam_w, beam_h, beam_w / 2, beam_h / 2);
	free(pixels);

	if (ret) {
		log_debug("HW cursor not available for display %s, using software",
			  display_name(scr->disp));
		scr->hw_cursor = false;
	} else {
		log_debug("HW cursor enabled for display %s", display_name(scr->disp));
		scr->hw_cursor = true;
	}
}

static void refresh_hw_cursor(struct screen *scr)
{
	if (!scr->hw_cursor)
		return;

	display_destroy_cursor(scr->disp);
	setup_hw_cursor(scr);
}

static void disable_screen(struct screen *scr)
{
	int ret;

	log_debug("Disabling screen %s", display_name(scr->disp));
	if (display_is_swapping(scr->disp))
		scr->pending = true;
	else {
		log_info("Disabling screen %s", display_name(scr->disp));
		scr->pending = false;
		display_clear(scr->disp, 0, 0, 0);
		ret = display_swap(scr->disp);
		if (ret) {
			if (ret != -EBUSY)
				log_warning("cannot swap display [%s] %d", display_name(scr->disp),
					    ret);
			else {
				log_debug("display [%s] is swapping", display_name(scr->disp));
				scr->pending = true;
			}
		}
	}
	scr->enabled = false;
}

static uint64_t clock_ns(clockid_t id)
{
	struct timespec ts;

	clock_gettime(id, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/*
 * Arms the timer for the earliest time a screen whose kmscon_text_set()
 * failed may try it again, or disarms it (terminal_retry_earliest()). The set
 * is tried again in a frame's kmscon_text_prepare(), and a screen with no
 * output and no blink draws no frame, so without the timer it stayed frozen
 * until something else redrew it (#206). Nothing draws in the background, so
 * there the timer is disarmed.
 */
static void set_retry_arm(struct kmscon_terminal *term)
{
	struct itimerspec spec = {0};
	struct screen *scr;
	uint64_t now = clock_ns(CLOCK_MONOTONIC), due = 0;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		scr->retry.due = scr->enabled ? kmscon_text_retry_due(scr->txt) : 0;
		due = terminal_retry_earliest(due, &scr->retry, term->awake, now);
	}
	/* 0 disarms a timer */
	if (due) {
		spec.it_value.tv_sec = (due - now) / 1000000000ULL;
		spec.it_value.tv_nsec = (due - now) % 1000000000ULL;
	}
	if (ev_timer_update(term->set_retry_timer, &spec))
		log_warning("cannot arm the timer that tries a failed font set again; the next "
			    "redraw tries it");
}

static void do_redraw_screen(struct screen *scr)
{
	struct tsm_screen_attr attr;
	struct kmscon_text_deco deco;
	uint64_t start;
	bool presented, retrying;
	int ret;

	if (!scr->term->awake || !kmscon_session_get_foreground(scr->term->session))
		return;

	if (!scr->enabled) {
		/* make sure to clear unused screen */
		if (scr->pending)
			disable_screen(scr);
		return;
	}

	scr->pending = false;
	scr->lost = false;
	retrying = kmscon_text_retry_due(scr->txt);

	start = clock_ns(CLOCK_MONOTONIC);
	render_timer_frame_begin(scr->timer);
	kmscon_conf_resolve_deco(scr->term->conf, scr->term->vte, &deco);
	kmscon_text_set_decorations(scr->txt, &deco);
	tsm_vte_get_def_attr(scr->term->vte, &attr);
	/*
	 * a renderer that cannot draw this frame, as gltex until its set
	 * succeeds, or a text whose kmscon_text_set() failed until the set
	 * kmscon_text_prepare() tries again succeeds (#206), draws and swaps
	 * nothing: the display keeps its picture, and the frame is no draw for
	 * coalescing (terminal_coalesce_drawn())
	 */
	if (kmscon_text_prepare(scr->txt, &attr, scr->term->blinking)) {
		render_timer_frame_end(scr->timer, false, 0);
		set_retry_arm(scr->term);
		return;
	}
	/* the set succeeded again: the timer no longer waits for this screen */
	if (retrying)
		set_retry_arm(scr->term);
	kmscon_text_draw(scr->txt, scr->term->console, scr->term->cursor_blinking);
	draw_pointer(scr);

	ret = kmscon_text_present(scr->txt);
	if (ret && ret != -EBUSY)
		log_warning("cannot swap display [%s] %d", display_name(scr->disp), ret);
	render_timer_misses(scr->timer, scr->txt->frame_misses);
	presented = !ret && scr->txt->frame_queued;
	terminal_coalesce_drawn(&scr->term->coalesce, &scr->coalesce, start,
				clock_ns(CLOCK_MONOTONIC), presented);
	render_timer_frame_end(scr->timer, presented, scr->txt->frame_damage_px);
}

/*
 * A redraw request. @lost: the display no longer shows the last presented
 * picture, so it draws the state as it is, through a hold of mode 2026.
 */
static void redraw_screen(struct screen *scr, bool lost)
{
	if (!scr->term->awake || !scr->enabled)
		return;

	switch (terminal_sync_draw(&scr->term->sync, display_is_swapping(scr->disp), lost)) {
	case TERMINAL_SYNC_DRAW:
		do_redraw_screen(scr);
		break;
	case TERMINAL_SYNC_FLIP:
		scr->pending = true;
		scr->lost |= lost;
		break;
	case TERMINAL_SYNC_OWED:
		render_timer_held(scr->timer);
		break;
	case TERMINAL_SYNC_IDLE:
		break;
	}
}

static void redraw_screens(struct kmscon_terminal *term, bool lost)
{
	struct screen *scr;

	if (!term->awake)
		return;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		redraw_screen(scr, lost);
	}
}

static void redraw_all(struct kmscon_terminal *term)
{
	redraw_screens(term, false);
}

/* a display's wait ends: its draw is asked for, and a flip or a hold may defer it */
static void coalesce_end(struct screen *scr)
{
	terminal_coalesce_end(&scr->coalesce);
	redraw_screen(scr, false);
}

/* arms the timer for the earliest wait's end, or disarms it when nothing waits */
static void coalesce_arm(struct kmscon_terminal *term)
{
	struct itimerspec spec = {0};
	struct screen *scr;
	uint64_t now, due = 0;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (scr->coalesce.due_ns && (!due || scr->coalesce.due_ns < due))
			due = scr->coalesce.due_ns;
	}
	now = clock_ns(CLOCK_MONOTONIC);
	/* armed for a time already past, the timer has fired or will not */
	if (due == term->coalesce_due && due > now)
		return;
	if (!due) {
		if (term->coalesce_due)
			ev_timer_update(term->coalesce_timer, NULL);
		term->coalesce_due = 0;
		return;
	}
	/* 0 disarms a timer */
	due = due > now ? due : now + 1;
	spec.it_value.tv_sec = (due - now) / 1000000000ULL;
	spec.it_value.tv_nsec = (due - now) % 1000000000ULL;
	if (!ev_timer_update(term->coalesce_timer, &spec)) {
		term->coalesce_due = due;
		return;
	}
	/* nothing waits without the timer that ends the wait */
	term->coalesce_due = 0;
	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (scr->coalesce.due_ns)
			coalesce_end(scr);
	}
}

static void coalesce_timer_event(struct ev_timer *timer, uint64_t count, void *data)
{
	struct kmscon_terminal *term = data;
	struct screen *scr;
	uint64_t now = clock_ns(CLOCK_MONOTONIC);

	term->coalesce_due = 0;
	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (scr->coalesce.due_ns && scr->coalesce.due_ns <= now)
			coalesce_end(scr);
	}
	coalesce_arm(term);
}

/*
 * After each turn of the event loop: draws the child's output parsed in the
 * turn once, however many input calls it took, on each display whose screen
 * it changed. A draw walks the whole frame, and output arrives a pty read of
 * about 4 KB at a time, up to 50 reads a turn, so drawing after each one
 * walked a 7680x2160 screen hundreds of times a second while a sixel picture
 * streamed in and nothing on it changed (#139). A keystroke's echo is read and
 * drawn in the same turn. Part way through a write, the draw waits for the
 * write's end, at most one refresh of the display (terminal_sync.h).
 */
static void terminal_turn_end(struct ev_eloop *eloop, void *unused, void *data)
{
	struct kmscon_terminal *term = data;
	struct screen *scr;

	if (term->redraw_owed && term->awake) {
		dlist_for_each_entry(scr, &term->screens, list)
		{
			if (!kmscon_text_changed(scr->txt, term->console))
				continue;
			/* the time is read for each display, after the draws of those before it */
			if (!terminal_coalesce_decide(&term->sync, &term->coalesce, &scr->coalesce,
						      term->unparsed, clock_ns(CLOCK_MONOTONIC),
						      display_get_refresh_ns(scr->disp)))
				coalesce_end(scr);
		}
		coalesce_arm(term);
	}
	term->redraw_owed = false;
	terminal_coalesce_turn_end(&term->coalesce);
}

static bool has_kms_display(struct kmscon_terminal *term)
{
	struct screen *scr;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (display_is_drm(scr->disp))
			return true;
	}
	return false;
}

static unsigned int term_min_width(struct kmscon_terminal *term)
{
	unsigned int width = UINT_MAX;
	struct screen *scr;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		unsigned int w = kmscon_text_get_width(scr->txt);

		if (!scr->enabled)
			continue;
		if (w < width)
			width = w;
	}
	return width;
}

static unsigned int term_min_height(struct kmscon_terminal *term)
{
	unsigned int height = UINT_MAX;
	struct screen *scr;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		unsigned int h = kmscon_text_get_height(scr->txt);
		if (!scr->enabled)
			continue;
		if (h < height)
			height = h;
	}
	return height;
}

/*
 * Returns true if all available screen are configured to be disabled
 */
static bool term_force_enable(struct kmscon_terminal *term)
{
	struct screen *scr;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (scr->config_enabled)
			return false;
	}
	return true;
}

/*
 * Align the pointer maximum to the minimum width and height of all screens
 * according to their orientation, as kmscon only support mirroring.
 */
static void update_pointer_max_all(struct kmscon_terminal *term)
{
	unsigned int width = term_min_width(term);
	unsigned int height = term_min_height(term);

	if (!term->awake)
		return;

	if (width < INT_MAX && height < INT_MAX)
		input_set_pointer_max(term->input, width, height);
}

static void display_pageflip(void *unused, void *unused2, void *data)
{
	struct screen *scr = data;

	if (scr->term->awake)
		render_timer_flip(scr->timer);
	/*
	 * A hold begun since the draw was deferred waits for its own end; a
	 * disabled screen's pending clear is drawn as for a lost picture.
	 */
	switch (terminal_sync_flipped(&scr->term->sync, scr->pending, scr->lost || !scr->enabled)) {
	case TERMINAL_SYNC_DRAW:
	case TERMINAL_SYNC_FLIP:
		do_redraw_screen(scr);
		break;
	case TERMINAL_SYNC_OWED:
		render_timer_held(scr->timer);
		break;
	case TERMINAL_SYNC_IDLE:
		break;
	}
}

/*
 * Each screen whose failed font set may be tried again, and that the timer
 * has not asked a frame of for that time already (terminal_retry_ask()), is
 * redrawn, through the same requests as a blink's: a flip in flight or a hold
 * of mode 2026 defers the frame, and a frame whose set still fails is no draw
 * for coalescing (do_redraw_screen()). A screen whose set succeeded has
 * nothing to try and is not redrawn.
 */
static void set_retry_event(struct ev_timer *timer, uint64_t count, void *data)
{
	struct kmscon_terminal *term = data;
	struct screen *scr;
	uint64_t now = clock_ns(CLOCK_MONOTONIC);

	dlist_for_each_entry(scr, &term->screens, list)
	{
		scr->retry.due = scr->enabled ? kmscon_text_retry_due(scr->txt) : 0;
		if (terminal_retry_ask(&scr->retry, now))
			redraw_screen(scr, false);
	}
	set_retry_arm(term);
}

static void blink_event(struct ev_timer *timer, uint64_t count, void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->awake)
		return;

	term->blinking = !term->blinking;
	redraw_all(term);
}

static uint64_t render_timer_now(void *data)
{
	return clock_ns(CLOCK_MONOTONIC);
}

static uint64_t render_timer_cpu(void *data)
{
	return clock_ns(CLOCK_THREAD_CPUTIME_ID);
}

static const struct render_timer_clock render_timer_clock = {
	.now_ns = render_timer_now,
	.cpu_ns = render_timer_cpu,
};

/*
 * What the hold's rules ask for after an input call or at the timer: arm the
 * timer for the hold's deadline, or disarm it and present at once. A display
 * with a flip in flight presents when the flip completes.
 */
static bool sync_act(struct kmscon_terminal *term, enum terminal_sync_action action)
{
	struct itimerspec spec = {0};
	struct screen *scr;
	uint64_t ns;

	switch (action) {
	case TERMINAL_SYNC_NONE:
		return false;
	case TERMINAL_SYNC_ARM:
		ns = terminal_sync_arm_ns(&term->sync, clock_ns(CLOCK_MONOTONIC));
		spec.it_value.tv_sec = ns / 1000000000ULL;
		spec.it_value.tv_nsec = ns % 1000000000ULL;
		if (!ev_timer_update(term->sync_timer, &spec))
			return false;
		/* no hold without its timer */
		log_warning("cannot arm the timer of a synchronized frame; presenting it as it is");
		return sync_act(term, terminal_sync_cancel(&term->sync));
	case TERMINAL_SYNC_EXPIRED:
		log_debug("sync-timeout ended a synchronized frame; presenting it as it is");
		dlist_for_each_entry(scr, &term->screens, list) render_timer_expired(scr->timer);
		/* fall through */
	case TERMINAL_SYNC_PRESENT:
		ev_timer_update(term->sync_timer, NULL);
		redraw_all(term);
		return true;
	}
	return false;
}

/* the vte took input or was reset outside the pty's feed; true if it presented */
static bool sync_check(struct kmscon_terminal *term)
{
	return sync_act(
		term, terminal_sync_input(&term->sync, clock_ns(CLOCK_MONOTONIC), term->unparsed));
}

static void sync_timer_event(struct ev_timer *timer, uint64_t count, void *data)
{
	struct kmscon_terminal *term = data;

	sync_act(term, terminal_sync_timer(&term->sync, clock_ns(CLOCK_MONOTONIC), term->unparsed));
}

static void render_timer_event(struct ev_timer *timer, uint64_t count, void *data)
{
	struct kmscon_terminal *term = data;
	struct render_timer_stats st;
	struct screen *scr;
	char name[256], line[512];

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (!render_timer_report(scr->timer,
					 (uint64_t)display_get_width(scr->disp) *
						 display_get_height(scr->disp),
					 &st))
			continue;
		if (term->conf->render_timer & KMSCON_RENDER_TIMER_LOG) {
			snprintf(name, sizeof(name), "%s[%s]", video_name(display_video(scr->disp)),
				 display_name(scr->disp));
			render_timer_format(&st, name, line, sizeof(line));
			/* asked for in the config, so at the level kmscon logs by default */
			log_notice("%s", line);
		}
		/* the overlay's own redraw, once a second, is part of what it shows */
		if (term->conf->render_timer & KMSCON_RENDER_TIMER_OSD) {
			render_timer_format_osd(&st, line, sizeof(line));
			if (kmscon_text_set_osd(scr->txt, line))
				log_warning("render-timer: cannot draw the on-screen display");
			redraw_screen(scr, false);
		}
	}
}

static void cursor_blink_event(struct ev_timer *timer, uint64_t count, void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->awake)
		return;

	term->cursor_blinking = !term->cursor_blinking;
	redraw_all(term);
}

static void asciinema_write(const char *u8, size_t len, void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->opened || !term->awake || !kmscon_session_get_foreground(term->session))
		return;

	/* a frame end has presented already */
	if (terminal_image_input(&term->image, u8, len) && !sync_check(term))
		redraw_all(term);
}

static void asciinema_start(struct kmscon_terminal *term)
{
	if (!term->asciinema)
		return;
	kmscon_asciinema_start(term->asciinema);
}

/* a kitty graphics reply was stored or answered: draw at the turn's end */
static void image_replied(void *data)
{
	struct kmscon_terminal *term = data;

	term->redraw_owed = true;
}

static void osc_event(struct tsm_vte *vte, const char *osc_string, size_t osc_len, void *data)
{
	struct kmscon_terminal *term = data;

	if (strcmp(osc_string, "setBackground") == 0) {
		log_info("Got OSC setBackground");
		kmscon_session_set_background(term->session);
	} else if (strcmp(osc_string, "setForeground") == 0) {
		log_info("Got OSC setForeground");
		kmscon_session_set_foreground(term->session);
	}
}

static void bell_event(struct tsm_vte *vte, void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->conf->bell)
		return;

	kmscon_session_bell(term->session);
}

static void led_event(struct tsm_vte *vte, unsigned int leds, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_session_set_leds(term->session, leds & TSM_VTE_LED_SCROLL_LOCK,
				leds & TSM_VTE_LED_NUM_LOCK, leds & TSM_VTE_LED_CAPS_LOCK);
}

static void mouse_event(struct tsm_vte *vte, enum tsm_mouse_track_mode track_mode,
			bool track_pixels, void *data)
{
	struct kmscon_terminal *term = data;

	term->pointer.select = false;
	tsm_screen_selection_reset(term->console);
}

static unsigned int terminal_get_auto_font_size(struct kmscon_terminal *term)
{
	unsigned int width, height, font_height;

	if (!term->auto_font_size)
		return term->font_size;

	width = term_min_width(term);
	height = term_min_height(term);

	if (!width || !height || width == UINT_MAX || height == UINT_MAX)
		return 16;

	// Max 120 characters per line, assuming font width is half the font height
	font_height = (2 * width) / 120;
	// at least 40 lines
	if (font_height > height / 40)
		font_height = height / 40;

	return font_height < 16 ? 16 : font_height;
}

static int font_set(struct kmscon_terminal *term, unsigned int new_size)
{
	int ret;
	struct kmscon_font *font;

retry:
	ret = kmscon_font_find(&font, term->conf->font_name, new_size, term->conf->font_engine);
	if (ret) {
		/* if it fails due to incorrect size, retry with 16px */
		if (!term->font) {
			new_size = 16;
			goto retry;
		}
		return ret;
	}
	font->emoji_color = term->conf->emoji_color;
	kmscon_font_unref(term->font);
	term->font = font;
	term->font_size = new_size;
	return 0;
}

static void font_update_all(struct kmscon_terminal *term)
{
	struct screen *scr;
	int ret;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		ret = kmscon_text_set(scr->txt, term->font);
		if (ret)
			log_warning("cannot change text-renderer font: %d", ret);
		refresh_hw_cursor(scr);
	}
}

/*
 * We support multiple monitors per terminal. In clone mode, we use the smallest cols/rows that are
 * provided so wider monitors will have black margins.
 */
static void terminal_update_size_clone(struct kmscon_terminal *term)
{
	struct screen *scr;
	unsigned int min_cols = UINT_MAX;
	unsigned int min_rows = UINT_MAX;
	bool force_enable = term_force_enable(term);

	dlist_for_each_entry(scr, &term->screens, list)
	{
		unsigned int cols, rows;

		if (!force_enable && !scr->config_enabled)
			continue;

		cols = kmscon_text_get_cols(scr->txt, term->font->width);
		if (cols && cols < min_cols)
			min_cols = cols;

		rows = kmscon_text_get_rows(scr->txt, term->font->height);
		if (rows && rows < min_rows)
			min_rows = rows;
	}
	if (min_cols == UINT_MAX || min_rows == UINT_MAX)
		return;

	term->cols = min_cols;
	term->rows = min_rows;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (!force_enable && !scr->config_enabled)
			disable_screen(scr);
		else if (!scr->enabled) {
			log_info("Enabling screen %s", display_name(scr->disp));
			scr->enabled = true;
		}
	}
}

/*
 * In largest mode, we use the largest cols/rows that are
 * provided so smaller monitors will be disabled.
 */
static void terminal_update_size_largest(struct kmscon_terminal *term)
{
	struct screen *scr;
	unsigned int rows, cols, cells;
	unsigned int max_cells = 0;
	bool force_enable = term_force_enable(term);

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (!force_enable && !scr->config_enabled)
			continue;

		rows = kmscon_text_get_rows(scr->txt, term->font->height);
		cols = kmscon_text_get_cols(scr->txt, term->font->width);

		cells = rows * cols;
		if (cells > max_cells) {
			max_cells = cells;
			term->cols = cols;
			term->rows = rows;
		}
	}
	dlist_for_each_entry(scr, &term->screens, list)
	{
		rows = kmscon_text_get_rows(scr->txt, term->font->height);
		cols = kmscon_text_get_cols(scr->txt, term->font->width);
		if (rows != term->rows || cols != term->cols ||
		    (!force_enable && !scr->config_enabled))
			disable_screen(scr);
		else if (!scr->enabled) {
			log_info("Enabling screen %s", display_name(scr->disp));
			scr->enabled = true;
		}
	}
}

static void scale_screen(struct screen *scr, unsigned int scaled_height)
{
	struct kmscon_font *font;

retry:
	log_debug("scaling display %s %d -> %d", display_name(scr->disp), scr->term->font->height,
		  scaled_height);
	if (kmscon_font_find(&font, scr->term->conf->font_name, scaled_height,
			     scr->term->conf->font_engine))
		return;
	font->emoji_color = scr->term->conf->emoji_color;

	if (kmscon_text_get_cols(scr->txt, font->width) < scr->term->cols ||
	    kmscon_text_get_rows(scr->txt, font->height) < scr->term->rows) {
		log_debug("scaling would truncate, reducing height to %d", scaled_height);
		kmscon_font_unref(font);
		if (scaled_height <= scr->term->font->increase_step)
			return;
		scaled_height -= scr->term->font->increase_step;
		if (scaled_height <= scr->term->font->height)
			return;
		goto retry;
	}

	if (font->height == scr->term->font->height) {
		kmscon_font_unref(font);
		return;
	}
	if (kmscon_text_set(scr->txt, font)) {
		kmscon_font_unref(font);
		return;
	}
	scr->scaled = true;
	kmscon_font_unref(font);
}

/*
 * In scaled mode, we find the minimum cols/rows among all screens
 * and directly scale up the font height on larger screens to match.
 */
static void terminal_update_size_scaled(struct kmscon_terminal *term)
{
	struct screen *scr;
	unsigned int height = term->font->height;

	terminal_update_size_clone(term);

	dlist_for_each_entry(scr, &term->screens, list)
	{
		unsigned int scaled_height;
		unsigned int h, w;

		scr->scaled = false;

		h = (kmscon_text_get_cols(scr->txt, term->font->width) * height) / term->cols;
		w = (kmscon_text_get_rows(scr->txt, term->font->height) * height) / term->rows;
		scaled_height = min(h, w);

		if (scaled_height > height)
			scale_screen(scr, scaled_height);
		if (!scr->scaled)
			kmscon_text_set(scr->txt, term->font);
		refresh_hw_cursor(scr);
	}
}

/* Returns true if the terminal size has changed */
static bool terminal_update_size(struct kmscon_terminal *term)
{
	struct screen *scr;
	unsigned int cols = term->cols;
	unsigned int rows = term->rows;

	if (term->auto_font_size) {
		unsigned int new_font_size = terminal_get_auto_font_size(term);

		if (new_font_size != term->font_size)
			font_set(term, new_font_size);
	} else if (!term->font)
		font_set(term, term->font_size);

	font_update_all(term);

	if (term->conf->multi_monitor && !strcmp(term->conf->multi_monitor, "largest")) {
		terminal_update_size_largest(term);
	} else if (term->conf->multi_monitor && !strcmp(term->conf->multi_monitor, "scaled")) {
		terminal_update_size_scaled(term);
	} else {
		terminal_update_size_clone(term);
	}

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (scr->enabled)
			kmscon_text_resize(scr->txt, term->cols, term->rows);
	}
	/* every kmscon_text_set() is above: one that failed is tried again on time */
	set_retry_arm(term);

	if (cols == term->cols && rows == term->rows)
		return false;

	log_notice("Resized terminal to %ux%u\n", term->cols, term->rows);
	return true;
}

/*
 * A new size in cells, or a new font, reaches libtsm and the pty: the cell
 * size in pixels goes to libtsm for sixel placement and XTWINOPS replies, and
 * the text area in pixels to TIOCSWINSZ, the same cell size (ADR-103).
 */
static void terminal_update_size_notify(struct kmscon_terminal *term)
{
	bool resized = terminal_update_size(term);

	if (term->font &&
	    (term->font->width != term->cell_w || term->font->height != term->cell_h)) {
		term->cell_w = term->font->width;
		term->cell_h = term->font->height;
		if (tsm_screen_set_cell_size(term->console, term->cell_w, term->cell_h))
			log_warning("cannot give libtsm the cell size %ux%u", term->cell_w,
				    term->cell_h);
	}
	/* the pty sets TIOCSWINSZ when the size or the cell size changed */
	kmscon_pty_resize(term->pty, term->cols, term->rows, term->cell_w, term->cell_h);
	if (resized) {
		tsm_screen_resize(term->console, term->cols, term->rows);
		redraw_screens(term, true);
	}
	update_pointer_max_all(term);
}

static void rotate_cw_screen(struct screen *scr)
{
	unsigned int orientation = kmscon_text_get_orientation(scr->txt);
	orientation = (orientation + 1) % (OR_LEFT + 1);
	kmscon_text_rotate(scr->txt, orientation);
	refresh_hw_cursor(scr);
}

static void rotate_cw_all(struct kmscon_terminal *term)
{
	struct screen *scr;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		rotate_cw_screen(scr);
	}
	terminal_update_size_notify(term);
}

static void rotate_ccw_screen(struct screen *scr)
{
	unsigned int orientation = kmscon_text_get_orientation(scr->txt);
	if (orientation == OR_NORMAL)
		orientation = OR_LEFT;
	else
		orientation -= 1;
	kmscon_text_rotate(scr->txt, orientation);
	refresh_hw_cursor(scr);
}

static void rotate_ccw_all(struct kmscon_terminal *term)
{
	struct screen *scr;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		rotate_ccw_screen(scr);
	}
	terminal_update_size_notify(term);
}

int terminal_add_display(struct kmscon_terminal *term, struct display *disp)
{
	struct screen *scr;
	const char *be, *gpu, *connector;
	enum Orientation orientation;
	int ret;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (scr->disp == disp)
			return 0;
	}

	scr = calloc(1, sizeof(*scr));
	if (!scr) {
		log_error("cannot allocate memory for display %s", display_name(disp));
		return -ENOMEM;
	}
	scr->term = term;
	scr->disp = disp;
	scr->enabled = true;

	gpu = video_name(display_video(disp));
	connector = display_name(disp);

	config_get_screen(term->conf, gpu, connector, &orientation, &scr->config_enabled);

	if (term->conf->render_timer != KMSCON_RENDER_TIMER_NONE) {
		ret = render_timer_new(&scr->timer, &render_timer_clock);
		if (ret)
			log_warning("no render timer for display %s: %d", connector, ret);
	}

	ret = display_register_pageflip(scr->disp, display_pageflip, scr);
	if (ret) {
		log_error("cannot register display callback: %d", ret);
		goto err_free;
	}

	if (display_has_opengl(scr->disp))
		be = "gltex";
	else
		be = "bbulk";

	ret = kmscon_text_new(&scr->txt, be, orientation, scr->disp);
	if (ret) {
		log_error("cannot create text-renderer");
		goto err_cb;
	}
	image_budget_terminal_display(&term->images, kmscon_text_draws_images(scr->txt), true);

	scr->txt->measure = scr->timer != NULL;
	dlist_link(&term->screens, &scr->list);

	terminal_update_size_notify(term);

	if (term->conf->mouse && !term->conf->soft_cursor)
		setup_hw_cursor(scr);

	log_notice("Display %s[%s] with backend [%s] text renderer [%s] font engine [%s] Cursor "
		   "[%s]\n",
		   gpu, connector, display_backend_name(disp), scr->txt->ops->name,
		   term->font->ops->name, scr->hw_cursor ? "HW" : "SW");

	display_ref(scr->disp);
	do_redraw_screen(scr);
	return 0;

err_cb:
	display_unregister_pageflip(scr->disp, display_pageflip, scr);
err_free:
	render_timer_free(scr->timer);
	free(scr);
	return ret;
}

static void free_screen(struct screen *scr, bool update)
{
	struct kmscon_terminal *term = scr->term;

	log_debug("destroying terminal screen %p", scr);
	if (scr->hw_cursor)
		display_destroy_cursor(scr->disp);
	dlist_unlink(&scr->list);
	image_budget_terminal_display(&term->images, kmscon_text_draws_images(scr->txt), false);
	kmscon_text_unref(scr->txt);
	display_unregister_pageflip(scr->disp, display_pageflip, scr);
	display_unref(scr->disp);
	render_timer_free(scr->timer);
	free(scr);

	if (!update || dlist_empty(&term->screens))
		return;

	terminal_update_size_notify(term);
}

void terminal_rm_display(struct kmscon_terminal *term, struct display *disp)
{
	struct screen *scr;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (scr->disp == disp) {
			log_debug("removed display %s from terminal %p", display_name(disp), term);
			free_screen(scr, true);
			return;
		}
	}
}

static void zoom_in(struct kmscon_terminal *term)
{
	unsigned int new_size;

	term->auto_font_size = false; // don't auto-adjust font size anymore

	if (term->font_size > 150 || !term->font_size) // don't allow zoom in beyond 150
		return;

	new_size = term->font_size + term->font->increase_step;
	if (font_set(term, new_size))
		return;
	font_update_all(term);
	terminal_update_size_notify(term);
}

static void zoom_out(struct kmscon_terminal *term)
{
	unsigned int new_size;

	term->auto_font_size = false; // don't auto-adjust font size anymore

	if (term->font_size <= term->font->increase_step)
		return;
	if (term->font_size - term->font->increase_step < 10)
		return;
	new_size = term->font_size - term->font->increase_step;
	if (font_set(term, new_size))
		return;
	font_update_all(term);
	terminal_update_size_notify(term);
}

static void input_event(struct input *input, struct input_key_event *ev, void *data)
{
	struct kmscon_terminal *term = data;
	uint32_t ms = tsm_screen_get_cursor_blinking_rate_ms(term->console);
	struct itimerspec blink_interval = {
		.it_interval = {ms / 1000, (ms % 1000) * 1000000L},
		.it_value = {BLINK_CURSOR_TYPING, 0},
	};

	if (!term->opened || !term->awake || ev->handled ||
	    !kmscon_session_get_foreground(term->session))
		return;

	// reset mouse selection on keypress
	tsm_screen_selection_reset(term->console);
	kmscon_asciinema_stop(term->asciinema);
	term->cursor_blinking = false;
	ev_timer_update(term->blink_cursor, &blink_interval);

	if (conf_grab_matches(term->conf->grab_scroll_up, ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_up(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_scroll_down, ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_down(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_page_up, ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_page_up(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_page_down, ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_page_down(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_in, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		zoom_in(term);
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_out, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		zoom_out(term);
		return;
	}
	if (conf_grab_matches(term->conf->grab_rotate_cw, ev->mods, ev->num_syms, ev->keysyms)) {
		rotate_cw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_rotate_ccw, ev->mods, ev->num_syms, ev->keysyms)) {
		rotate_ccw_all(term);
		ev->handled = true;
		return;
	}

	/* TODO: xkbcommon supports multiple keysyms, but it is currently
	 * unclear how this feature will be used. There is no keymap, which
	 * uses this, yet. */
	if (ev->num_syms > 1)
		return;

	if (tsm_vte_handle_keyboard(term->vte, ev->keysyms[0], ev->ascii, ev->mods,
				    ev->codepoints[0])) {
		tsm_screen_sb_reset(term->console);
		redraw_all(term);
		ev->handled = true;
	}
}

static void start_selection(struct tsm_screen *console, unsigned int x, unsigned int y)
{
	tsm_screen_selection_reset(console);
	tsm_screen_selection_start(console, x, y);
}

static void update_selection(struct tsm_screen *console, unsigned int x, unsigned int y)
{
	tsm_screen_selection_target(console, x, y);
}

static void free_selection(struct kmscon_terminal *term)
{
	if (!term->pointer.copy)
		return;
	free(term->pointer.copy);
	term->pointer.copy = NULL;
	term->pointer.copy_len = 0;
}

static void copy_selection(struct kmscon_terminal *term)
{
	free_selection(term);
	term->pointer.copy_len = tsm_screen_selection_copy(term->console, &term->pointer.copy);
}

static void forward_pointer_event(struct kmscon_terminal *term, struct input_pointer_event *ev)
{
	unsigned int event;
	unsigned int button;
	int32_t wheel;

	button = ev->button;
	wheel = ev->wheel;
	if (term->conf->natural_scrolling)
		wheel = -wheel;

	switch (ev->event) {
	case POINTER_MOVED:
		event = TSM_MOUSE_EVENT_MOVED;
		/* In mouse tracking protocol, motion with button pressed uses button+32 */
		if (ev->pressed && button <= 2) {
			button += 32;
		}
		break;
	case POINTER_BUTTON:
		if (ev->pressed)
			event = TSM_MOUSE_EVENT_PRESSED;
		else
			event = TSM_MOUSE_EVENT_RELEASED;
		break;
	case POINTER_WHEEL:
		/* Convert wheel events to button 4 (scroll up) or 5 (scroll down) */
		event = TSM_MOUSE_EVENT_PRESSED;
		if (wheel > 0)
			button = 4; /* Scroll up */
		else
			button = 5; /* Scroll down */
		break;
	default:
		return;
	}
	tsm_vte_handle_mouse(term->vte, term->pointer.posx, term->pointer.posy, term->pointer.x,
			     term->pointer.y, button, event, 0);
}

static void handle_pointer_button(struct kmscon_terminal *term, struct input_pointer_event *ev)
{
	switch (ev->button) {
	case 0:
		if (ev->pressed) {
			if (ev->double_click) {
				tsm_screen_selection_word(term->console, term->pointer.posx,
							  term->pointer.posy);
				copy_selection(term);
				term->pointer.select = false;
			} else {
				term->pointer.select = true;
				start_selection(term->console, term->pointer.posx,
						term->pointer.posy);
			}
		} else {
			if (term->pointer.select)
				copy_selection(term);
			term->pointer.select = false;
		}
		break;
	case 1:
		if (ev->pressed) {
			if (term->pointer.copy && term->pointer.copy_len)
				tsm_vte_paste(term->vte, term->pointer.copy);
			tsm_screen_selection_reset(term->console);
		}
		break;
	case 2:
		term->pointer.select = false;
		tsm_screen_selection_reset(term->console);
		break;
	}
}

static void text_show_cursor(struct kmscon_text *txt, int32_t x, int32_t y)
{
	unsigned int sw = txt->cols * FONT_WIDTH(txt);
	unsigned int sh = txt->rows * FONT_HEIGHT(txt);

	switch (txt->orientation) {
	default:
	case OR_NORMAL:
		display_show_cursor(txt->disp, x, y);
		break;
	case OR_UPSIDE_DOWN:
		display_show_cursor(txt->disp, sw - x, sh - y);
		break;
	case OR_RIGHT:
		display_show_cursor(txt->disp, sh - y, x);
		break;
	case OR_LEFT:
		display_show_cursor(txt->disp, y, sw - x);
		break;
	}
}

static void hw_cursor_show(struct kmscon_terminal *term, int32_t x, int32_t y)
{
	struct screen *scr;
	int fw = term->font->width ? term->font->width : 1;
	int fh = term->font->height ? term->font->height : 1;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		int32_t sx, sy;

		/* a text whose set failed has no font to place the pointer by (#211) */
		if (!scr->txt->font)
			continue;
		if (scr->hw_cursor) {
			if (!scr->scaled)
				text_show_cursor(scr->txt, x, y);
			else {
				sx = (x * (int32_t)FONT_WIDTH(scr->txt)) / fw;
				sy = (y * (int32_t)FONT_HEIGHT(scr->txt)) / fh;
				if (sx >= FONT_WIDTH(scr->txt) * scr->txt->cols)
					sx = FONT_WIDTH(scr->txt) * scr->txt->cols - 1;
				if (sy >= FONT_HEIGHT(scr->txt) * scr->txt->rows)
					sy = FONT_HEIGHT(scr->txt) * scr->txt->rows - 1;
				text_show_cursor(scr->txt, sx, sy);
			}
		}
	}
}

static void hw_cursor_hide(struct kmscon_terminal *term)
{
	struct screen *scr;

	dlist_for_each_entry(scr, &term->screens, list)
	{
		if (scr->hw_cursor)
			display_hide_cursor(scr->disp);
	}
}

static void pointer_event(struct input *input, struct input_pointer_event *ev, void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->opened || !term->awake || !kmscon_session_get_foreground(term->session))
		return;

	if (ev->event == POINTER_MOVED) {
		term->pointer.x = ev->pointer_x;
		term->pointer.y = ev->pointer_y;

		coord_to_cell(term, term->pointer.x, term->pointer.y, &term->pointer.posx,
			      &term->pointer.posy);
		term->pointer.visible = true;
		hw_cursor_show(term, ev->pointer_x, ev->pointer_y);
	}

	if (tsm_vte_get_mouse_mode(term->vte) != TSM_MOUSE_TRACK_DISABLE &&
	    ev->event != POINTER_SYNC) {
		forward_pointer_event(term, ev);
		return;
	}

	switch (ev->event) {
	default:
		break;
	case POINTER_MOVED:
		if (term->pointer.select)
			update_selection(term->console, term->pointer.posx, term->pointer.posy);
		break;
	case POINTER_BUTTON:
		handle_pointer_button(term, ev);
		break;
	case POINTER_WHEEL:
		if (input_get_mods(term->input) & INPUT_CONTROL_MASK) {
			if (ev->wheel > 0)
				zoom_in(term);
			else
				zoom_out(term);
		} else {
			if (term->conf->natural_scrolling != (ev->wheel > 0))
				tsm_screen_sb_up(term->console, 3);
			else
				tsm_screen_sb_down(term->console, 3);
		}
		break;
	case POINTER_SYNC:
		redraw_all(term);
		break;
	case POINTER_HIDE_TIMEOUT:
		tsm_screen_selection_reset(term->console);
		term->pointer.visible = false;
		hw_cursor_hide(term);
		break;
	}
}

static void rm_all_screens(struct kmscon_terminal *term)
{
	struct dlist *iter;
	struct screen *scr;

	while ((iter = term->screens.next) != &term->screens) {
		scr = dlist_entry(iter, struct screen, list);
		free_screen(scr, false);
	}

	term->cols = 0;
	term->rows = 0;
}

static void get_pty_str(struct kmscon_terminal *term, char *buf, size_t buflen)
{
	char pty_name[128] = {0};
	const char *short_pty_name;
	const char *prefix = "/dev/";

	kmscon_pty_get_slave_name(term->pty, pty_name, sizeof(pty_name));
	short_pty_name = shl_strip_prefix(prefix, pty_name);

	if (term->conf->vt)
		snprintf(buf, buflen, "%s (%s)", short_pty_name,
			 shl_strip_prefix(prefix, term->conf->vt));
	else
		strncpy(buf, short_pty_name, buflen);
}

static void kmscon_issue_write(struct kmscon_terminal *term)
{

	char pty_str[128] = {0};
	char *issue;
	size_t issue_len;

	get_pty_str(term, pty_str, sizeof(pty_str));

	issue = kmscon_issue_get_buffer(term->conf->issue_path, pty_str, &issue_len);
	if (!issue || !issue_len)
		return;

	terminal_image_input(&term->image, issue, issue_len);
	free(issue);
	sync_check(term);
}

static int terminal_open(struct kmscon_terminal *term)
{
	int ret;
	unsigned short width, height;

	if (term->opened)
		return -EALREADY;

	tsm_vte_hard_reset(term->vte);
	sync_check(term);
	width = tsm_screen_get_width(term->console);
	height = tsm_screen_get_height(term->console);
	ret = kmscon_pty_open(term->pty, width, height, term->cell_w, term->cell_h,
			      has_kms_display(term));
	if (ret)
		return ret;

	term->opened = true;
	/* no request survives a close, so none is out to hold the issue back */
	if (term->conf->issue)
		kmscon_issue_write(term);
	asciinema_start(term);

	update_pointer_max_all(term);
	redraw_all(term);
	return 0;
}

/* the helper goes with the session; libtsm's request out, if any, is cancelled */
static void terminal_close(struct kmscon_terminal *term)
{
	terminal_image_close(&term->image);
	kmscon_pty_close(term->pty);
	term->opened = false;
	term->unparsed = false;
	terminal_coalesce_turn_end(&term->coalesce);
	sync_check(term);
}

void terminal_refresh_displays(struct kmscon_terminal *term)
{
	struct screen *scr;

	if (term->pointer.visible)
		hw_cursor_show(term, term->pointer.x, term->pointer.y);

	dlist_for_each_entry(scr, &term->screens, list)
	{
		display_set_need_redraw(scr->disp);
	}
	redraw_screens(term, true);
}

void terminal_activate(struct kmscon_terminal *term)
{
	term->awake = true;
	if (term->conf->blink) {
		ev_timer_enable(term->blink_timer);
		ev_timer_enable(term->blink_cursor);
	}
	if (!term->opened)
		terminal_open(term);
	terminal_image_play(&term->image, true);

	terminal_refresh_displays(term);
	/* a failed set waiting in the background is tried again now, or on time */
	set_retry_arm(term);
}

void terminal_deactivate(struct kmscon_terminal *term)
{
	struct screen *scr;

	term->awake = false;
	/* in the background, its scaled pictures give their room back */
	dlist_for_each_entry(scr, &term->screens, list) kmscon_text_release(scr->txt);
	hw_cursor_hide(term);
	if (term->conf->blink) {
		ev_timer_disable(term->blink_timer);
		ev_timer_disable(term->blink_cursor);
	}
	terminal_image_play(&term->image, false);
	/* not awake: disarms the timer of a failed font set */
	set_retry_arm(term);
}

void terminal_destroy(struct kmscon_terminal *term)
{
	log_debug("free terminal object %p", term);

	terminal_close(term);
	terminal_image_destroy(&term->image);
	rm_all_screens(term);
	ev_eloop_rm_timer(term->blink_timer);
	ev_eloop_rm_timer(term->blink_cursor);
	ev_eloop_rm_timer(term->render_timer);
	ev_eloop_rm_timer(term->sync_timer);
	ev_eloop_rm_timer(term->coalesce_timer);
	ev_eloop_rm_timer(term->set_retry_timer);
	kmscon_asciinema_free(term->asciinema);
	input_unregister_pointer_cb(term->input, pointer_event, term);
	input_unregister_key_cb(term->input, input_event, term);
	ev_eloop_unregister_post_cb(term->eloop, terminal_turn_end, term);
	ev_eloop_rm_fd(term->ptyfd);
	kmscon_pty_unref(term->pty);
	kmscon_font_unref(term->font);
	image_budget_free_terminal(&term->images);
	input_unref(term->input);
	ev_eloop_unref(term->eloop);
	free_selection(term);
	free(term);
}

/*
 * The child's output, parsed a bounded amount of image work at a time: a few
 * bytes of sixel can fill a large picture. What one call leaves is held by the
 * pty, which hands it back on later turns of its loop, so input, drawing and
 * VT switches are handled between the parts. Drawing waits for the end of the
 * turn (terminal_turn_end), except at the end of a synchronized frame: the
 * call stops right after it, and the frame is presented before any byte of
 * the next is parsed.
 */
static size_t pty_input(struct kmscon_pty *pty, const char *u8, size_t len, void *data)
{
	struct kmscon_terminal *term = data;
	uint64_t start;
	size_t n;

	if (!len)
		return 0;
	term->redraw_owed = true;
	start = clock_ns(CLOCK_MONOTONIC);
	n = tsm_vte_input_bounded(term->vte, u8, len);
	terminal_coalesce_input(&term->coalesce, start, clock_ns(CLOCK_MONOTONIC), n);
	term->unparsed = n < len;
	sync_check(term);
	return n;
}

static void pty_exit(struct kmscon_pty *pty, bool restart, void *data)
{
	struct kmscon_terminal *term = data;

	terminal_close(term);

	if (restart) {
		terminal_open(term);
		return;
	}

	ev_eloop_exit(term->eloop);
}

static void pty_event(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_dispatch(term->pty);
}

static void write_event(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_write(term->pty, u8, len);
}

struct kmscon_terminal *terminal_new(struct kmscon_session *session, unsigned int vtnr,
				     struct conf_ctx *conf_ctx, struct ev_eloop *eloop,
				     struct input *input, const char *seat_name)
{
	struct kmscon_terminal *term;
	struct itimerspec blink_interval = {
		.it_interval = {0, BLINK_TIMER_NS},
		.it_value = {0, BLINK_TIMER_NS},
	};
	struct itimerspec render_timer_interval = {
		.it_interval = {RENDER_TIMER_NS / 1000000000, RENDER_TIMER_NS % 1000000000},
		.it_value = {RENDER_TIMER_NS / 1000000000, RENDER_TIMER_NS % 1000000000},
	};
	int ret;

	term = calloc(1, sizeof(*term));
	if (!term)
		return NULL;

	term->ref = 1;
	term->session = session;
	term->eloop = eloop;
	term->input = input;
	dlist_init(&term->screens);

	term->conf_ctx = conf_ctx;
	term->conf = conf_ctx_get_mem(term->conf_ctx);

	ret = tsm_screen_new(&term->console, log_llog, NULL);
	if (ret)
		goto err_free;
	tsm_screen_set_max_sb(term->console, term->conf->sb_size);

	ret = terminal_vte_new(&term->vte, &term->images, term->console, term->conf, write_event,
			       term);
	if (ret)
		goto err_con;

	tsm_vte_set_osc_cb(term->vte, osc_event, (void *)term);
	tsm_vte_set_mouse_cb(term->vte, mouse_event, (void *)term);
	tsm_vte_set_bell_cb(term->vte, bell_event, (void *)term);
	tsm_vte_set_led_cb(term->vte, led_event, (void *)term);

	term->font_size = term->conf->font_size;
	if (term->font_size) {
		ret = font_set(term, term->font_size);
		if (ret)
			goto err_images;
	} else {
		term->auto_font_size = true;
	}

	ret = kmscon_pty_new(&term->pty, pty_input, pty_exit, term);
	if (ret)
		goto err_font;

	ret = kmscon_pty_set_conf(term->pty, term->conf->term, "truecolor", term->conf->argv,
				  seat_name, vtnr, term->conf->reset_env,
				  term->conf->backspace_delete, term->conf->oneshot);
	if (ret)
		goto err_pty;

	ret = terminal_image_init(&term->image, term->vte, term->pty, term->conf, IMAGE_HELPER,
				  image_replied, term);
	if (ret)
		goto err_pty;

	ret = ev_eloop_new_fd(term->eloop, &term->ptyfd, kmscon_pty_get_fd(term->pty), EV_READABLE,
			      pty_event, term);
	if (ret)
		goto err_image;

	ret = ev_eloop_register_post_cb(term->eloop, terminal_turn_end, term);
	if (ret)
		goto err_ptyfd;

	/* armed while a synchronized frame is held */
	ret = ev_eloop_new_timer(term->eloop, &term->sync_timer, NULL, sync_timer_event, term);
	if (ret)
		goto err_turn;
	if (terminal_sync_init(&term->sync, term->vte, term->conf->sync_timeout) !=
	    term->conf->sync_timeout)
		log_warning("sync-timeout=%u is above the limit; holding a synchronized frame for "
			    "%u ms at most",
			    term->conf->sync_timeout, TERMINAL_SYNC_MAX_MS);

	/* armed while a draw waits for the end of the child's write */
	terminal_coalesce_init(&term->coalesce);
	ret = ev_eloop_new_timer(term->eloop, &term->coalesce_timer, NULL, coalesce_timer_event,
				 term);
	if (ret)
		goto err_sync;

	/* armed while a screen whose font set failed waits to try it again */
	ret = ev_eloop_new_timer(term->eloop, &term->set_retry_timer, NULL, set_retry_event, term);
	if (ret)
		goto err_coalesce;

	ret = input_register_key_cb(term->input, input_event, term);
	if (ret)
		goto err_set_retry;

	if (term->conf->mouse) {
		ret = input_register_pointer_cb(term->input, pointer_event, term);
		if (ret)
			goto err_input;
	}
	if (term->conf->blink) {
		ret = ev_eloop_new_timer(term->eloop, &term->blink_timer, &blink_interval,
					 blink_event, term);
		if (ret)
			goto err_pointer;
		ret = ev_eloop_new_timer(term->eloop, &term->blink_cursor, &blink_interval,
					 cursor_blink_event, term);
		if (ret)
			goto err_blink;
	}
	if (term->conf->render_timer != KMSCON_RENDER_TIMER_NONE) {
		ret = ev_eloop_new_timer(term->eloop, &term->render_timer, &render_timer_interval,
					 render_timer_event, term);
		if (ret)
			goto err_cursor;
	}
	if (term->conf->asciicast) {
		ret = kmscon_asciinema_new(&term->asciinema, term->eloop, term->conf->asciicast,
					   term->conf->asciicast_loop, asciinema_write, term);
		if (ret)
			log_warn("cannot load asciicast %s: %d", term->conf->asciicast, ret);
		terminal_image_set_playback(&term->image, term->asciinema);
	}

	ev_eloop_ref(term->eloop);
	input_ref(term->input);
	log_debug("new terminal object %p", term);
	return term;

err_cursor:
	ev_eloop_rm_timer(term->blink_cursor);
err_blink:
	ev_eloop_rm_timer(term->blink_timer);
err_pointer:
	input_unregister_pointer_cb(term->input, pointer_event, term);
err_input:
	input_unregister_key_cb(term->input, input_event, term);
err_set_retry:
	ev_eloop_rm_timer(term->set_retry_timer);
err_coalesce:
	ev_eloop_rm_timer(term->coalesce_timer);
err_sync:
	ev_eloop_rm_timer(term->sync_timer);
err_turn:
	ev_eloop_unregister_post_cb(term->eloop, terminal_turn_end, term);
err_ptyfd:
	ev_eloop_rm_fd(term->ptyfd);
err_image:
	terminal_image_destroy(&term->image);
err_pty:
	kmscon_pty_unref(term->pty);
err_font:
	kmscon_font_unref(term->font);
err_images:
	/* the vte and the screen go with the terminal's place in the budget */
	image_budget_free_terminal(&term->images);
	goto err_free;
err_con:
	tsm_screen_unref(term->console);
err_free:
	free(term);
	return NULL;
}
