/*
 * kmscon - Text Renderer
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * Text Renderer
 * The Text-Renderer subsystem provides a simple way to draw text into a
 * framebuffer. The system is modular and several different backends are
 * available that can be used.
 */

#ifndef KMSCON_TEXT_H
#define KMSCON_TEXT_H

#include <errno.h>
#include <libtsm.h>
#include <stdlib.h>
#include <string.h>
#include "font/font.h"
#include "shl/module.h"
#include "video/video.h"

/* text renderer */

enum Orientation {
	OR_NORMAL = 0,	// 0 Degree
	OR_RIGHT,	// 90 Degree
	OR_UPSIDE_DOWN, // 180 Degree
	OR_LEFT,	// 270 Degree
};

struct kmscon_text;
struct kmscon_text_ops;

/*
 * The colours underline and strike-through take (ADR-102), resolved by the
 * caller from kmscon.conf: palette indices are looked up in the palette in
 * use before they reach the renderer.
 */
struct kmscon_text_deco {
	bool ul_fg;			/* underline-color=fg: the cell's foreground */
	struct tsm_screen_color ul;	/* else this */
	bool strike_fg;			/* strike-color=fg */
	struct tsm_screen_color strike; /* else this */
	bool force;			/* force-decoration-colors: win over SGR 58 */
};

/*
 * A picture kmscon itself draws over the screen, after libtsm's placements and
 * in the top band: the render timer's on-screen display (ADR-103). It belongs
 * to no libtsm store, so no program can erase, delete or replace it. It sits
 * at the right end of the top row; its placement's col is set as it is drawn.
 * Every new picture takes a new image_key, so a renderer damages it as it
 * damages any placement whose image changed.
 */
struct kmscon_text_overlay {
	struct tsm_frame_placement placement;
	struct tsm_frame_image image;
	uint32_t pixels[];
};

struct kmscon_text {
	unsigned long ref;
	struct shl_register_record *record;
	const struct kmscon_text_ops *ops;
	void *data;

	struct kmscon_font *font;
	struct display *disp;
	unsigned int cols;
	unsigned int rows;
	unsigned int max_cols;
	unsigned int max_rows;
	bool rendering;
	enum Orientation orientation;
	bool blinking;
	struct kmscon_text_deco deco;
	struct tsm_screen *screen; /* the screen being drawn, for cluster lookups */

	/*
	 * This display's place in the history of the screen it draws, @drawn.
	 * Each display keeps its own (libtsm's tsm_screen_frame_since), so one
	 * console shown on several displays marks each display's changes. It
	 * is zeroed whenever this display needs every cell redrawn.
	 */
	struct tsm_frame_age age;
	struct tsm_screen *drawn;

	/*
	 * The last frame kmscon_text_present() handled, for the render timer:
	 * whether it queued a flip, and, while @measure is set, the pixels in
	 * its damage rectangles. kmscon_text_render() sets the whole display,
	 * and a renderer that knows its damage replaces it. With @measure
	 * unset, as when the render timer is off, the area is not computed.
	 */
	bool measure;
	bool frame_queued;
	uint64_t frame_damage_px;

	struct kmscon_text_overlay *overlay; /* NULL: none */
	uint64_t overlay_count;		     /* the overlays made, for their keys */
};


struct kmscon_text_ops {
	const char *name;
	struct shl_module *owner;
	int (*init)(struct kmscon_text *txt);
	void (*destroy)(struct kmscon_text *txt);
	int (*set)(struct kmscon_text *txt);
	void (*unset)(struct kmscon_text *txt);
	void (*resize)(struct kmscon_text *txt, unsigned int cols, unsigned int rows);
	int (*rotate)(struct kmscon_text *txt, enum Orientation orientation);
	int (*prepare)(struct kmscon_text *txt, struct tsm_screen_attr *attr);
	/*
	 * Draw @frame. @cursor_on: the frame's cursor is visible and in the
	 * visible phase of its blink.
	 */
	int (*draw)(struct kmscon_text *txt, const struct tsm_frame *frame, bool cursor_on);
	int (*draw_pointer)(struct kmscon_text *txt, unsigned int x, unsigned int y);
	int (*render)(struct kmscon_text *txt);
	void (*abort)(struct kmscon_text *txt);
};

#define FONT_WIDTH(txt) ((txt)->font->width)
#define FONT_HEIGHT(txt) ((txt)->font->height)

int kmscon_text_register(const struct kmscon_text_ops *ops);
void kmscon_text_unregister(const char *name);

int kmscon_text_new(struct kmscon_text **out, const char *backend, enum Orientation orientation,
		    struct display *disp);
void kmscon_text_ref(struct kmscon_text *txt);
void kmscon_text_unref(struct kmscon_text *txt);

int kmscon_text_set(struct kmscon_text *txt, struct kmscon_font *font);
void kmscon_text_unset(struct kmscon_text *txt);

unsigned int kmscon_text_get_width(struct kmscon_text *txt);
unsigned int kmscon_text_get_height(struct kmscon_text *txt);
unsigned int kmscon_text_get_cols(struct kmscon_text *txt, unsigned int font_width);
unsigned int kmscon_text_get_rows(struct kmscon_text *txt, unsigned int font_height);

enum Orientation kmscon_text_get_orientation(struct kmscon_text *txt);
void kmscon_text_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows);
int kmscon_text_rotate(struct kmscon_text *txt, enum Orientation orientation);

void kmscon_text_set_decorations(struct kmscon_text *txt, const struct kmscon_text_deco *deco);
int kmscon_text_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr, bool blinking);
int kmscon_text_draw(struct kmscon_text *txt, struct tsm_screen *con, bool cursor_blink);
int kmscon_text_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y);
int kmscon_text_render(struct kmscon_text *txt);
int kmscon_text_present(struct kmscon_text *txt);
void kmscon_text_abort(struct kmscon_text *txt);

/* kmscon_text_render(): the frame drew nothing, so the display needs no swap */
#define KMSCON_TEXT_UNCHANGED 1

/* helpers for backends */

/*
 * The next kmscon_text_draw() marks every cell of its frame changed. Call it
 * when this display's buffers no longer hold what it last drew.
 */
static inline void kmscon_text_redraw_all(struct kmscon_text *txt)
{
	memset(&txt->age, 0, sizeof(txt->age));
}

const uint32_t *kmscon_text_get_cluster(struct kmscon_text *txt, uint64_t key, uint32_t *one,
					size_t *len);
struct tsm_screen_color kmscon_text_ul_color(struct kmscon_text *txt,
					     const struct tsm_frame_cell *cell);
struct tsm_screen_color kmscon_text_strike_color(struct kmscon_text *txt,
						 const struct tsm_frame_cell *cell);
int kmscon_text_set_osd(struct kmscon_text *txt, const char *text);
int kmscon_text_get_image(struct kmscon_text *txt, bool overlay, uint64_t key,
			  struct tsm_frame_image *out);

/* modularized backends */

extern struct kmscon_text_ops kmscon_text_bbulk_ops;
extern struct kmscon_text_ops kmscon_text_gltex_ops;

#endif /* KMSCON_TEXT_H */
