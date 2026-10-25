/*
 * kmscon - Text Renderer
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

/**
 * SECTION:text
 * @short_description: Text Renderer
 * @include: text.h
 *
 * TODO
 */

#include <errno.h>
#include <libtsm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "font/font.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "shl/register.h"
#include "text.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "text"

/* a failed kmscon_text_set() is tried again at most this often (#206) */
#ifndef KMSCON_TEXT_SET_RETRY_NS
#define KMSCON_TEXT_SET_RETRY_NS 1000000000ULL
#endif

/* the clock the retry reads; a test that includes text.c may stand in for it */
#ifndef KMSCON_TEXT_NOW_NS
static uint64_t text_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#define KMSCON_TEXT_NOW_NS() text_now_ns()
#endif

static struct shl_register text_reg = SHL_REGISTER_INIT(text_reg);

static inline void kmscon_text_destroy(void *data)
{
	const struct kmscon_text_ops *ops = data;

	shl_module_unref(ops->owner);
}

/**
 * kmscon_text_register:
 * @ops: Text operations and name for new backend
 *
 * This register a new text backend with operations set to @ops. The name
 * @ops->name must be valid.
 *
 * The first font that is registered automatically becomes the default and
 * fallback. So make sure you register a safe fallback as first backend.
 * If this is unregistered, the next in the list becomes the default
 * and fallback.
 *
 * Returns: 0 on success, negative error code on failure
 */
SHL_EXPORT
int kmscon_text_register(const struct kmscon_text_ops *ops)
{
	int ret;

	if (!ops)
		return -EINVAL;

	log_debug("register text backend %s", ops->name);

	ret = shl_register_add_cb(&text_reg, ops->name, (void *)ops, kmscon_text_destroy);
	if (ret) {
		log_error("cannot register text backend %s: %d", ops->name, ret);
		return ret;
	}

	shl_module_ref(ops->owner);
	return 0;
}

/**
 * kmscon_text_unregister:
 * @name: Name of backend
 *
 * This unregisters the text-backend that is registered with name @name. If
 * @name is not found, nothing is done.
 */
SHL_EXPORT
void kmscon_text_unregister(const char *name)
{
	log_debug("unregister backend %s", name);
	shl_register_remove(&text_reg, name);
}

static int new_text(struct kmscon_text *text, const char *backend)
{
	struct shl_register_record *record;
	const char *name = backend ? backend : "<default>";

	text->ref = 1;

	if (backend)
		record = shl_register_find(&text_reg, backend);
	else
		record = shl_register_first(&text_reg);

	if (!record) {
		log_error("requested backend '%s' not found", name);
		return -ENOENT;
	}

	text->record = record;
	text->ops = record->data;

	if (text->ops->init) {
		int ret;

		ret = text->ops->init(text);
		if (ret) {
			log_warning("backend %s cannot create renderer %d", name, ret);
			shl_register_record_unref(record);
			return ret;
		}
	}
	return 0;
}

/**
 * kmscon_text_new:
 * @out: A pointer to the new text-renderer is stored here
 * @backend: Backend to use or NULL for default backend
 * @orientation: Orientation to use for output
 *
 * Returns: 0 on success, error code on failure
 */
int kmscon_text_new(struct kmscon_text **out, const char *backend, enum Orientation orientation,
		    struct display *disp)
{
	struct kmscon_text *text;
	int ret;

	if (!out || !disp)
		return -EINVAL;

	text = calloc(1, sizeof(*text));
	if (!text) {
		log_error("cannot allocate memory for new text-renderer");
		return -ENOMEM;
	}

	text->orientation = orientation;
	text->deco.ul_fg = true;
	text->deco.strike_fg = true;

	ret = new_text(text, backend);
	if (ret) {
		if (backend)
			ret = new_text(text, NULL);
		if (ret)
			goto err_free;
	}
	text->disp = disp;
	display_ref(disp);

	log_debug("using: be: %s", text->ops->name);
	*out = text;
	return 0;

err_free:
	free(text);
	return ret;
}

/**
 * kmscon_text_ref:
 * @text: Valid text-renderer object
 *
 * This increases the reference count of @text by one.
 */
void kmscon_text_ref(struct kmscon_text *text)
{
	if (!text || !text->ref)
		return;

	++text->ref;
}

/**
 * kmscon_text_unref:
 * @text: Valid text-renderer object
 *
 * This decreases the reference count of @text by one. If it drops to zero, the
 * object is freed.
 */
void kmscon_text_unref(struct kmscon_text *text)
{
	if (!text || !text->ref || --text->ref)
		return;

	log_debug("freeing text renderer");
	kmscon_text_unset(text);
	free(text->overlay);
	display_unref(text->disp);

	if (text->ops->destroy)
		text->ops->destroy(text);
	shl_register_record_unref(text->record);
	free(text);
}

/* @txt, which has no font, takes @font: the renderer is set up for it */
static int text_set(struct kmscon_text *txt, struct kmscon_font *font)
{
	int ret;

	txt->font = font;
	if (txt->ops->set) {
		ret = txt->ops->set(txt);
		if (ret) {
			txt->font = NULL;
			return ret;
		}
	}
	kmscon_font_ref(txt->font);
	return 0;
}

/**
 * kmscon_text_set:
 * @txt: Valid text-renderer object
 * @font: font object
 *
 * This makes the text-renderer @txt use the font @font. You
 * can drop your reference to the font after calling this.
 * This calls kmscon_text_unset() first to remove all previous associations.
 * None of the arguments can be NULL!
 * If this function fails then no font is set and the text draws nothing, but
 * it keeps @font, and kmscon_text_prepare() tries the set again with it,
 * until a set succeeds or kmscon_text_unset() is called (#206).
 *
 * Returns: 0 on success, negative error code on failure.
 */
int kmscon_text_set(struct kmscon_text *txt, struct kmscon_font *font)
{
	int ret;

	if (!txt || !font)
		return -EINVAL;

	if (txt->font == font)
		return 0;

	kmscon_text_unset(txt);
	ret = text_set(txt, font);
	if (ret) {
		kmscon_font_ref(font);
		txt->set_font = font;
		txt->set_tried_ns = KMSCON_TEXT_NOW_NS();
	}
	return ret;
}

/**
 * kmscon_text_unset():
 * @txt: text renderer
 *
 * This redos kmscon_text_set() by dropping the internal references to the font
 * and invalidating the object. You need to call kmscon_text_set()
 * again to make use of this text renderer. A failed set's font is dropped
 * too, and no longer tried.
 * This is automatically called when the text renderer is destroyed.
 */
void kmscon_text_unset(struct kmscon_text *txt)
{
	if (!txt)
		return;

	if (txt->set_font) {
		kmscon_font_unref(txt->set_font);
		txt->set_font = NULL;
		txt->set_failed_logged = false;
	}
	if (!txt->font)
		return;

	if (txt->ops->unset)
		txt->ops->unset(txt);

	kmscon_font_unref(txt->font);
	txt->font = NULL;
	txt->cols = 0;
	txt->rows = 0;
	txt->rendering = false;
	kmscon_text_redraw_all(txt);
}

/**
 * kmscon_text_resize:
 * @txt: valid text renderer
 * @cols: number of columns
 * @rows: number of rows
 *
 * After setting the arguments with kmscon_text_set(), the renderer will compute
 * the number of columns/rows of the console that it can display on the screen.
 * But in case of multi-screen setup the actual size of the terminal might be
 * smaller, so call this function to set the current terminal size.
 * A text whose set failed only records the size: the renderer has no font to
 * compute with, and the set tried again puts the size back (set_again()).
 */
void kmscon_text_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows)
{
	if (!txt || !cols || !rows)
		return;
	if (!txt->font) {
		txt->cols = cols;
		txt->rows = rows;
	} else {
		if (cols > txt->max_cols || rows > txt->max_rows)
			return;
		if (txt->ops->resize)
			txt->ops->resize(txt, cols, rows);
	}
	kmscon_text_redraw_all(txt);
}

unsigned int kmscon_text_get_width(struct kmscon_text *txt)
{
	if (!txt)
		return 0;
	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN)
		return display_get_width(txt->disp);
	else
		return display_get_height(txt->disp);
}

unsigned int kmscon_text_get_height(struct kmscon_text *txt)
{
	if (!txt)
		return 0;
	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN)
		return display_get_height(txt->disp);
	else
		return display_get_width(txt->disp);
}

unsigned int kmscon_text_get_cols(struct kmscon_text *txt, unsigned int font_width)
{
	if (!txt || !font_width)
		return 0;

	return kmscon_text_get_width(txt) / font_width;
}

unsigned int kmscon_text_get_rows(struct kmscon_text *txt, unsigned int font_height)
{
	if (!txt || !font_height)
		return 0;

	return kmscon_text_get_height(txt) / font_height;
}

/**
 * kmscon_text_get_orientation:
 * @txt: valid text renderer
 *
 * With a valid @txt passed it, this returns the currently active orientation
 * of the output/screen. Possible values are:
 *
 *   - OR_NORMAL
 *   - OR_RIGHT
 *   - OR_UPSIDE_DOWN
 *   - OR_LEFT
 *
 * Returns: Current orientation enum or OR_NORMAL if @txt is invalid
 */
enum Orientation kmscon_text_get_orientation(struct kmscon_text *txt)
{
	if (!txt)
		return OR_NORMAL;

	return txt->orientation;
}

/**
 * kmscon_text_rotate:
 * @txt: valid text renderer
 * @orientation: enum value representing the desired output-orientation
 *
 * Update the rotation/orientation of the text. It can be one of:
 *
 *   - OR_NORMAL
 *   - OR_RIGHT
 *   - OR_UPSIDE_DOWN
 *   - OR_LEFT
 *
 * A text whose set failed only records the orientation: the renderer has no
 * font to compute with, and the set tried again (set_again()) sets it up in
 * this orientation.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int kmscon_text_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	kmscon_text_redraw_all(txt);
	if (!txt->ops->rotate)
		return 0;
	if (!txt->font) {
		txt->orientation = orientation;
		return 0;
	}
	return txt->ops->rotate(txt, orientation);
}

/**
 * kmscon_text_set_decorations:
 * @txt: valid text renderer
 * @deco: the colours of underline and strike-through
 *
 * Sets the colours decorations take when no program has chosen one, and
 * whether they win over the program's (ADR-102). Call it before
 * kmscon_text_prepare(); a change redraws the screen.
 */
void kmscon_text_set_decorations(struct kmscon_text *txt, const struct kmscon_text_deco *deco)
{
	if (!txt || !deco)
		return;
	txt->deco = *deco;
}

/**
 * kmscon_text_ul_color:
 * @txt: valid text renderer
 * @cell: a frame cell with an underline
 *
 * The colour @cell's underline draws in: the configured colour when
 * force-decoration-colors is on, else the colour SGR 58 set, else the
 * configured colour, where `fg` is the cell's foreground as drawn.
 */
struct tsm_screen_color kmscon_text_ul_color(struct kmscon_text *txt,
					     const struct tsm_frame_cell *cell)
{
	const struct kmscon_text_deco *d = &txt->deco;

	if (!d->force && (cell->flags & TSM_FRAME_UL_SET))
		return cell->ul;
	return d->ul_fg ? cell->fg : d->ul;
}

/**
 * kmscon_text_strike_color:
 * @txt: valid text renderer
 * @cell: a frame cell with strike-through
 *
 * The colour @cell's strike-through draws in. No escape sequence sets one, so
 * it is always the configured colour.
 */
struct tsm_screen_color kmscon_text_strike_color(struct kmscon_text *txt,
						 const struct tsm_frame_cell *cell)
{
	return txt->deco.strike_fg ? cell->fg : txt->deco.strike;
}

/**
 * kmscon_text_get_cluster:
 * @txt: valid text renderer, drawing
 * @key: a frame cell's glyph key, not 0
 * @one: storage for a single codepoint
 * @len: the number of codepoints returned
 *
 * The codepoints frame key @key names: @key itself for a codepoint, the
 * cluster's codepoints for a cluster, or the replacement character for a
 * cluster the screen no longer holds.
 */
const uint32_t *kmscon_text_get_cluster(struct kmscon_text *txt, uint64_t key, uint32_t *one,
					size_t *len)
{
	const uint32_t *cps = NULL;

	if (key >= TSM_FRAME_CLUSTER_KEY && txt->screen)
		cps = tsm_screen_frame_cluster(txt->screen, key, len);
	if (cps && *len)
		return cps;

	*one = key < TSM_FRAME_CLUSTER_KEY ? (uint32_t)key : FONT_REPLACEMENT_CHAR;
	*len = 1;
	return one;
}

/**
 * kmscon_text_get_image:
 * @txt: valid text renderer, drawing
 * @overlay: the key is @txt's overlay's, not the screen's
 * @key: a frame placement's image key
 * @out: the image's size and premultiplied ARGB32 pixels
 *
 * The image a placement of the frame being drawn shows (ADR-103). The pixels
 * stay valid until the screen next changes, or the overlay is replaced.
 *
 * Returns: 0, or -ENOENT when there is no such image.
 */
int kmscon_text_get_image(struct kmscon_text *txt, bool overlay, uint64_t key,
			  struct tsm_frame_image *out)
{
	if (overlay) {
		if (!txt->overlay || txt->overlay->placement.image_key != key)
			return -ENOENT;
		*out = txt->overlay->image;
		return 0;
	}
	if (!txt->screen)
		return -ENOENT;
	return tsm_screen_frame_image(txt->screen, key, out);
}

/* the on-screen display's colours: light grey on a dark grey, opaque */
#define OSD_FG 0xe0
#define OSD_BG 0x20

/**
 * kmscon_text_set_osd:
 * @txt: valid text renderer with a font
 * @text: printable ASCII, or NULL or "" to remove it
 *
 * Draws @text with @txt's font into the overlay picture at the right end of
 * the top row (ADR-103's render timer display), replacing the one before. It
 * shows from the next frame.
 *
 * Returns: 0, -EINVAL or -ENOMEM; on failure the overlay is removed.
 */
int kmscon_text_set_osd(struct kmscon_text *txt, const char *text)
{
	struct kmscon_text_overlay *ov;
	struct kmscon_font_attr attr = {0};
	struct kmscon_glyph *glyph;
	unsigned int fw, fh, len, i, x, y, w;
	uint32_t cp;

	if (!txt)
		return -EINVAL;
	free(txt->overlay);
	txt->overlay = NULL;
	if (!text || !*text)
		return 0;
	if (!txt->font)
		return -EINVAL;

	fw = txt->font->width;
	fh = txt->font->height;
	len = strlen(text);
	w = len * fw;
	ov = calloc(1, sizeof(*ov) + (size_t)w * fh * 4);
	if (!ov)
		return -ENOMEM;
	for (i = 0; i < w * fh; i++)
		ov->pixels[i] = 0xff000000 | OSD_BG << 16 | OSD_BG << 8 | OSD_BG;

	for (i = 0; i < len; i++) {
		cp = (unsigned char)text[i];
		if (cp <= ' ' || cp > '~')
			continue;
		glyph = kmscon_font_render(txt->font, &attr, &cp, 1, 1);
		if (!glyph) {
			free(ov);
			return -ENOMEM;
		}
		/* a mask's alpha mixes the two greys; a colour glyph is not ASCII */
		for (y = 0; y < fh && y < glyph->buf.height; y++) {
			for (x = 0; x < fw && x < glyph->buf.width; x++) {
				uint32_t a = glyph->buf.format == VIDEO_FORMAT_A8
						     ? glyph->buf.data[y * glyph->buf.width + x]
						     : 0;
				uint32_t c = (OSD_FG * a + OSD_BG * (255 - a) + 127) / 255;

				ov->pixels[y * w + i * fw + x] = 0xff000000 | c << 16 | c << 8 | c;
			}
		}
		free(glyph);
	}

	ov->image.width = w;
	ov->image.height = fh;
	ov->image.stride = (size_t)w * 4;
	ov->image.pixels = ov->pixels;
	ov->image.opaque = true;
	ov->placement.key = 1;
	ov->placement.image_key = ++txt->overlay_count;
	ov->placement.cols = len;
	ov->placement.rows = 1;
	ov->placement.width = w;
	ov->placement.height = fh;
	ov->placement.src_width = w;
	ov->placement.src_height = fh;
	ov->placement.cell_width = fw;
	ov->placement.cell_height = fh;
	ov->placement.z = INT32_MAX;
	ov->placement.band = TSM_FRAME_BAND_ABOVE_TEXT;
	ov->placement.flags = TSM_FRAME_OPAQUE;
	txt->overlay = ov;
	return 0;
}

/*
 * @txt, whose last set failed, tries it again with the font it was given, at
 * most once in KMSCON_TEXT_SET_RETRY_NS, and says once that it keeps trying.
 * A set, failed or not, can give the text its largest size in cells, so the
 * size the terminal gave it since is put back, as gltex_prepare() does when
 * it tries its own set again (which it does every frame).
 */
static void set_again(struct kmscon_text *txt)
{
	struct kmscon_font *font = txt->set_font;
	unsigned int cols = txt->cols, rows = txt->rows;
	uint64_t now = KMSCON_TEXT_NOW_NS();
	int ret;

	if (now - txt->set_tried_ns < KMSCON_TEXT_SET_RETRY_NS)
		return;
	txt->set_tried_ns = now;
	ret = text_set(txt, font);
	if (ret) {
		txt->cols = cols;
		txt->rows = rows;
		if (!txt->set_failed_logged)
			log_warning("cannot set the text renderer's font (%d); later frames try "
				    "again, at most once a second",
				    ret);
		txt->set_failed_logged = true;
		return;
	}
	log_info("text renderer's font set again");
	txt->set_font = NULL;
	txt->set_failed_logged = false;
	kmscon_font_unref(font);
	kmscon_text_resize(txt, cols, rows);
}

/**
 * kmscon_text_retry_due:
 * @txt: valid text renderer
 *
 * A text whose set failed tries it again in a frame's kmscon_text_prepare(),
 * at most once in KMSCON_TEXT_SET_RETRY_NS. A screen with no output draws no
 * frame, so its caller asks for one at this time (#206).
 *
 * Returns: the time, on CLOCK_MONOTONIC, from which kmscon_text_prepare() tries
 * the failed set again, or 0 when no set is to be tried again.
 */
uint64_t kmscon_text_retry_due(struct kmscon_text *txt)
{
	if (!txt || !txt->set_font)
		return 0;
	return txt->set_tried_ns + KMSCON_TEXT_SET_RETRY_NS;
}

/**
 * kmscon_text_prepare:
 * @txt: valid text renderer
 * @attr: glyph attributes
 *
 * This starts a rendering-round. When rendering a console via a text renderer,
 * you have to call this first, then render all your glyphs via
 * kmscon_text_draw() and finally use kmscon_text_render(). If you modify this
 * renderer during rendering or if you activate different OpenGL contexts in
 * between, you need to restart rendering by calling kmscon_text_prepare() again
 * and redoing everything from the beginning.
 * A text whose last set failed tries it again here first (set_again()).
 *
 * Returns: 0 on success, negative error code on failure.
 */
int kmscon_text_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr, bool blinking)
{
	int ret = 0;

	if (!txt || !txt->disp)
		return -EINVAL;
	if (!txt->font && txt->set_font)
		set_again(txt);
	if (!txt->font)
		return -EINVAL;

	txt->rendering = true;
	txt->blinking = blinking;
	txt->frame_misses = 0;
	if (txt->ops->prepare)
		ret = txt->ops->prepare(txt, attr);
	if (ret)
		txt->rendering = false;

	return ret;
}

static bool is_cursor_blinking(enum tsm_screen_cursor_style style)
{
	return (!style || style & 1);
}

/**
 * kmscon_text_draw:
 * @txt: valid text renderer
 * @con: valid tsm screen
 * @cursor_blink: the cursor's blink phase hides it
 *
 * This draws the screen's frame (ADR-102). The frame carries the cursor cell;
 * the renderer only decides whether the blink phase shows it.
 *
 * The frame marks the cells changed since this display's last frame, from
 * @txt's own age, so every display drawing @con sees each change. That covers
 * one frame back; a double-buffered backend also keeps the previous frame's
 * changes, as bbulk does, because its back buffer is two frames old.
 *
 * Returns: 0 on success or negative error code if this glyph couldn't be drawn.
 */
int kmscon_text_draw(struct kmscon_text *txt, struct tsm_screen *con, bool cursor_blink)
{
	const struct tsm_frame *frame;
	bool cursor_on;
	int ret;

	if (!txt || !con)
		return -EINVAL;

	if (txt->drawn != con) {
		kmscon_text_redraw_all(txt);
		txt->drawn = con;
	}

	frame = tsm_screen_frame_since(con, &txt->age);
	if (!frame)
		return -ENOMEM;

	cursor_on =
		frame->cursor.visible && !(is_cursor_blinking(frame->cursor.style) && cursor_blink);

	txt->screen = con;
	ret = txt->ops->draw(txt, frame, cursor_on);
	txt->screen = NULL;
	if (ret)
		kmscon_text_redraw_all(txt);
	return ret;
}

/**
 * kmscon_text_changed:
 * @txt: valid text renderer
 * @con: valid tsm screen
 *
 * Whether kmscon_text_draw() would find anything of @con to draw: a cell, the
 * cursor or a placement changed since this display's last frame, the display
 * has not drawn @con yet or needs every cell, or a picture it left out for the
 * image budget may fit now. It builds no frame (libtsm's
 * tsm_screen_frame_changed_since). It knows nothing of what kmscon draws
 * besides the screen: the blink phases, the pointer, the overlay.
 */
bool kmscon_text_changed(struct kmscon_text *txt, struct tsm_screen *con)
{
	return !txt || txt->drawn != con || tsm_screen_frame_changed_since(con, &txt->age) ||
	       (txt->ops->starved && txt->ops->starved(txt));
}

/**
 * kmscon_text_draw_pointer:
 * @txt: valid text renderer
 * @x: X-position of the center of the pointer in pixel
 * @y: Y-position of the center of the pointer in pixel
 *
 * This draws a single I glyph at the requested position. The position is a
 * a pixel position! You must precede this call with kmscon_text_prepare().
 * Use this function to feed the mouse pointer into the rendering pipeline
 * and finally call kmscon_text_render().
 *
 * Returns: 0 on success or negative error code if it couldn't be drawn.
 */
int kmscon_text_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y)
{
	if (!txt || !txt->rendering || !txt->ops->draw_pointer)
		return -EINVAL;

	return txt->ops->draw_pointer(txt, x, y);
}

/**
 * kmscon_text_render:
 * @txt: valid text renderer
 *
 * This does the final rendering round after kmscon_text_prepare() has been
 * called and all glyphs were sent to the renderer via kmscon_text_draw().
 *
 * Returns: 0 on success, KMSCON_TEXT_UNCHANGED when the frame drew nothing on
 * a display with damage clips, negative error on failure.
 */
int kmscon_text_render(struct kmscon_text *txt)
{
	int ret = 0;

	if (!txt || !txt->rendering)
		return -EINVAL;

	if (txt->measure)
		txt->frame_damage_px =
			(uint64_t)display_get_width(txt->disp) * display_get_height(txt->disp);
	if (txt->ops->render)
		ret = txt->ops->render(txt);
	txt->rendering = false;

	return ret;
}

/**
 * kmscon_text_present:
 * @txt: valid text renderer
 *
 * This renders the frame with kmscon_text_render() and swaps it onto the
 * display, unless the renderer reports it unchanged and the display has no
 * other change waiting for a swap (display_need_commit(), such as a moved
 * hardware cursor). A frame that drew nothing is in both buffers already, and
 * the back buffer stays the back buffer. On a display with damage clips, a
 * commit without clips updates the whole plane.
 *
 * txt->frame_queued records whether it swapped. A frame whose swap failed was
 * drawn but is not shown, and no flip will show it, so the next
 * kmscon_text_draw() redraws every cell and kmscon_text_changed() is true
 * until then.
 *
 * Returns: 0 on success, negative error code if the swap failed.
 */
int kmscon_text_present(struct kmscon_text *txt)
{
	int ret;

	txt->frame_queued = false;
	if (kmscon_text_render(txt) == KMSCON_TEXT_UNCHANGED && !display_need_commit(txt->disp))
		return 0;
	txt->frame_queued = true;
	ret = display_swap(txt->disp);
	if (ret)
		kmscon_text_redraw_all(txt);
	return ret;
}

/**
 * kmscon_text_abort:
 * @txt: valid text renderer
 *
 * If you called kmscon_text_prepare() but you want to abort rendering instead
 * of finishing it with kmscon_text_render(), you can safely call this to reset
 * internal state. It is optional to call this or simply restart rendering.
 * Especially if the other renderers return an error, then they probably already
 * aborted rendering and it is not required to call this.
 */
void kmscon_text_abort(struct kmscon_text *txt)
{
	if (!txt || !txt->rendering)
		return;

	if (txt->ops->abort)
		txt->ops->abort(txt);
	txt->rendering = false;
}

/**
 * kmscon_text_release:
 * @txt: valid text renderer
 *
 * Frees what the renderer keeps between frames and can make again, such as
 * bbulk's scaled pictures, so a terminal in the background gives back its
 * share of the process's image budget (ADR-103). The next frame makes what it
 * needs again.
 */
void kmscon_text_release(struct kmscon_text *txt)
{
	if (txt && txt->ops->release)
		txt->ops->release(txt);
}

/**
 * kmscon_text_draws_images:
 * @txt: valid text renderer
 *
 * Returns true when the renderer draws the frame's image placements (ADR-103).
 */
bool kmscon_text_draws_images(struct kmscon_text *txt)
{
	return txt && txt->ops->draws_images;
}
