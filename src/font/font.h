/*
 * kmscon - Font Renderer
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
 * Font Renderer
 */

#ifndef KMSCON_FONT_H
#define KMSCON_FONT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "shl/module.h"
#include "video/video.h"

/* fonts */

struct kmscon_font_attr;
struct kmscon_glyph;
struct kmscon_font;
struct kmscon_font_ops;

#define KMSCON_FONT_MAX_NAME 128
#define KMSCON_FONT_DEFAULT_NAME "monospace"
#define KMSCON_FONT_DEFAULT_PPI 72

#define FONT_REPLACEMENT_CHAR 0x25a1
#define FONT_VBAR 0x258f // Left One Eighth Block

struct kmscon_font_attr {
	bool bold;
	bool italic;
};

/*
 * A glyph is a sprite one or two cells wide (ADR-102). Its width in cells is
 * the one the renderer asked for, from the frame, and its buffer is exactly
 * that many cells. Underline and strike are not part of it: they are
 * decoration sprites the renderer draws.
 */
/*
 * What a renderer caches a glyph under (ADR-102): the frame's 64-bit glyph
 * key, which never names two clusters, plus what changes the drawing. @extra
 * is the renderer's own (gltex bakes decorations into its glyphs).
 */
struct kmscon_glyph_key {
	uint64_t key;
	uint8_t bold;
	uint8_t italic;
	uint8_t width;
	uint8_t extra;
};

/* a 64-bit hash of @k for a hash table that compares the full key */
static inline uint64_t kmscon_glyph_key_hash(const struct kmscon_glyph_key *k)
{
	uint64_t x = k->key;

	x ^= (uint64_t)(k->bold | k->italic << 1 | k->width << 2 | k->extra << 8) *
	     0x9e3779b97f4a7c15ULL;
	/* splitmix64 finaliser */
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}

static inline bool kmscon_glyph_key_equal(const struct kmscon_glyph_key *a,
					  const struct kmscon_glyph_key *b)
{
	return a->key == b->key && a->bold == b->bold && a->italic == b->italic &&
	       a->width == b->width && a->extra == b->extra;
}

struct kmscon_glyph {
	bool double_width;
	struct kmscon_glyph_key key; /* set by the renderer that caches it */
	struct video_buffer buf;     // Must be last
};

static inline unsigned int kmscon_glyph_cwidth(const struct kmscon_glyph *glyph)
{
	return glyph->double_width ? 2 : 1;
}

/*
 * Where a font puts underline and strike-through, in pixel rows from the top
 * of the cell, already clamped inside it.
 */
struct kmscon_font_deco {
	unsigned int ul_pos;	   /* first row of the single underline */
	unsigned int ul_thick;	   /* its thickness, at least 1 */
	unsigned int strike_pos;   /* first row of the strike-through */
	unsigned int strike_thick; /* its thickness, at least 1 */
};

struct kmscon_font {
	unsigned long ref;
	struct shl_register_record *record;
	const struct kmscon_font_ops *ops;
	unsigned int height;
	unsigned int width;
	unsigned increase_step;
	/* draw emoji presentation in colour (kmscon.conf emoji-color, on by
	 * default); off draws every glyph as a mask */
	bool emoji_color;
	void *data;
};

/*
 * Whether the cluster @cps, drawn @cwidth cells wide, asks for emoji
 * presentation (ADR-102, UTS #51): never with VS15; always with VS16; else
 * when the cell is wide. libtsm makes a cell wide from the same tables that
 * define emoji presentation (Emoji_Presentation, East Asian Width and the
 * emoji variation sequences), so the width carries the answer and kmscon needs
 * no table of its own. A wide CJK character asks too, and draws from a text
 * face because no colour face covers it; the few wide codepoints a colour face
 * does cover without Emoji_Presentation (U+3030, U+303D, U+3297, U+3299) draw
 * in colour.
 */
static inline bool kmscon_font_wants_colour(const uint32_t *cps, size_t len, unsigned int cwidth)
{
	bool vs16 = false;
	size_t i;

	for (i = 1; i < len; i++) {
		if (cps[i] == 0xfe0e)
			return false;
		if (cps[i] == 0xfe0f)
			vs16 = true;
	}
	return vs16 || cwidth == 2;
}

struct kmscon_font_ops {
	const char *name;
	struct shl_module *owner;
	int (*init)(struct kmscon_font *out, const char *name, unsigned int height);
	void (*destroy)(struct kmscon_font *font);
	bool (*has_glyph)(struct kmscon_font *font, struct kmscon_font_attr *attr, uint32_t ch);
	/*
	 * Draw the cluster @cps (@len codepoints, at least 1) as a glyph
	 * @cwidth cells wide. A backend that cannot shape draws cps[0].
	 */
	struct kmscon_glyph *(*render)(struct kmscon_font *font, struct kmscon_font_attr *attr,
				       const uint32_t *cps, size_t len, unsigned int cwidth);
	/*
	 * render draws the font's missing glyph for a cluster no face covers
	 * that asks for emoji presentation (any wide one); for other backends
	 * kmscon_font_render draws the replacement character instead
	 */
	bool draws_missing;
	/* optional: the font's own decoration metrics */
	void (*get_deco)(struct kmscon_font *font, struct kmscon_font_deco *out);
};

int kmscon_font_register(const struct kmscon_font_ops *ops);
void kmscon_font_unregister(const char *name);

int kmscon_font_find(struct kmscon_font **out, const char *name, unsigned int height,
		     const char *backend);
void kmscon_font_ref(struct kmscon_font *font);
void kmscon_font_unref(struct kmscon_font *font);

struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, struct kmscon_font_attr *attr,
					const uint32_t *cps, size_t len, unsigned int cwidth);
void kmscon_font_get_deco(struct kmscon_font *font, struct kmscon_font_deco *out);
bool kmscon_font_has_glyph(struct kmscon_font *font, struct kmscon_font_attr *attr, uint32_t ch);

/* modularized backends */

extern struct kmscon_font_ops kmscon_font_8x16_ops;
extern struct kmscon_font_ops kmscon_font_psf_ops;
extern struct kmscon_font_ops kmscon_font_unifont_ops;
extern struct kmscon_font_ops kmscon_font_pango_ops;
extern struct kmscon_font_ops kmscon_font_freetype_ops;

#endif /* KMSCON_FONT_H */
