/*
 * kmscon - OpenGL Textures Text Renderer Backend
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

/**
 * SECTION:text_gltex.c
 * @short_description: OpenGL Textures Text Renderer Backend
 * @include: text.h
 *
 * Uses OpenGL textures to store glyph information and draws these textures with
 * a custom fragment shader.
 * Glyphs are stored in texture-atlases. OpenGL has heavy restrictions on
 * texture sizes so we need to use multiple atlases. As there is no way to pass
 * a varying amount of textures to a shader, we need to render the screen for
 * each atlas we have.
 */

#define GL_GLEXT_PROTOTYPES

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "deco.h"
#include "font/font.h"
#include "gltex_atlas.frag.bin.h"
#include "gltex_atlas.vert.bin.h"
#include "shl/dlist.h"
#include "shl/gl.h"
#include "shl/hashtable.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "text.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "text_gltex"

struct atlas {
	struct dlist list;

	GLuint tex;
	unsigned int height;
	unsigned int width;
	unsigned int count;
	unsigned int fill;

	unsigned int cache_size;
	unsigned int cache_num;
	GLfloat *cache_pos;
	GLfloat *cache_texpos;
	GLfloat *cache_fgcol;
	GLfloat *cache_bgcol;

	GLfloat advance_htex;
	GLfloat advance_vtex;
};

struct gl_glyph {
	bool double_width;
	struct kmscon_glyph_key key;
	struct atlas *atlas;
	unsigned int texoff;
};

#define GLYPH_WIDTH(gly) ((gly)->buf.width)
#define GLYPH_HEIGHT(gly) ((gly)->buf.height)
#define GLYPH_STRIDE(gly) ((gly)->buf.stride)
#define GLYPH_DATA(gly) ((gly)->buf.data)

/*
 * The glyph cache holds at most this many screens of atlas cells between
 * flushes (see gltex_flush()).
 */
#define GLTEX_CACHE_SCREENS 4

struct gltex {
	struct shl_hashtable *glyphs;
	unsigned int max_tex_size;
	unsigned int cells;	/* atlas cells handed out since the last flush */
	unsigned int max_cells; /* flush at the next frame once cells passes this */

	struct dlist atlases;

	GLfloat advance_x;
	GLfloat advance_y;
	GLfloat off_x;
	GLfloat off_y;

	struct gl_shader *shader;
	GLuint uni_cos;
	GLuint uni_sin;
	GLuint uni_proj;
	GLuint uni_atlas;
	GLuint uni_advance_htex;
	GLuint uni_advance_vtex;

	unsigned int sw;
	unsigned int sh;

	GLfloat cos;
	GLfloat sin;

	struct tsm_screen_attr attr;
};

static int gltex_init(struct kmscon_text *txt)
{
	struct gltex *gt;

	gt = malloc(sizeof(*gt));
	if (!gt)
		return -ENOMEM;

	txt->data = gt;
	return 0;
}

static void gltex_destroy(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;

	free(gt);
}

static void free_glyph(void *data)
{
	struct gl_glyph *glyph = data;

	free(glyph);
}

static void gltex_set_cos(struct gltex *gt, enum Orientation orientation)
{
	float sin_table[5] = {0.0, 1.0, 0.0, -1.0, 0.0};

	gt->cos = sin_table[orientation + 1];
	gt->sin = sin_table[orientation];
}

static void compute_advance_and_offset(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	unsigned int off_x, off_y;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt);
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt);
		off_x = (gt->sw - txt->cols * FONT_WIDTH(txt)) / 2;
		off_y = (gt->sh - txt->rows * FONT_HEIGHT(txt)) / 2;
		gt->off_x = (float)2.0 * off_x / gt->sw;
		gt->off_y = (float)2.0 * off_y / gt->sh;
	} else {
		gt->advance_x = 2.0 / gt->sh * FONT_WIDTH(txt);
		gt->advance_y = 2.0 / gt->sw * FONT_HEIGHT(txt);
		off_x = (gt->sw - txt->rows * FONT_HEIGHT(txt)) / 2;
		off_y = (gt->sh - txt->cols * FONT_WIDTH(txt)) / 2;
		gt->off_x = (float)2.0 * off_y / gt->sh;
		gt->off_y = (float)2.0 * off_x / gt->sw;
	}
	display_set_cursor_offset(txt->disp, off_x, off_y);
}

static int gltex_set(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret, vlen, flen;
	const char *vert, *frag;
	static char *attr[] = {"position", "texture_position", "fgcolor", "bgcolor"};
	GLint s;

	if (!display_has_opengl(txt->disp))
		return -EINVAL;

	ret = display_use(txt->disp);
	if (ret < 0)
		return ret;

	memset(gt, 0, sizeof(*gt));
	dlist_init(&gt->atlases);

	ret = shl_hashtable_new(&gt->glyphs, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		return ret;

	vert = _binary_gltex_atlas_vert_start;
	vlen = _binary_gltex_atlas_vert_size;
	frag = _binary_gltex_atlas_frag_start;
	flen = _binary_gltex_atlas_frag_size;
	gl_clear_error();

	ret = gl_shader_new(&gt->shader, vert, vlen, frag, flen, attr, 4);
	if (ret)
		goto err_htable;

	gt->uni_cos = gl_shader_get_uniform(gt->shader, "cos");
	gt->uni_sin = gl_shader_get_uniform(gt->shader, "sin");
	gt->uni_proj = gl_shader_get_uniform(gt->shader, "projection");
	gt->uni_atlas = gl_shader_get_uniform(gt->shader, "atlas");
	gt->uni_advance_htex = gl_shader_get_uniform(gt->shader, "advance_htex");
	gt->uni_advance_vtex = gl_shader_get_uniform(gt->shader, "advance_vtex");

	if (gl_has_error(gt->shader)) {
		log_warning("cannot create shader");
		goto err_shader;
	}

	gt->sw = display_get_width(txt->disp);
	gt->sh = display_get_height(txt->disp);

	txt->max_cols = kmscon_text_get_cols(txt, FONT_WIDTH(txt));
	txt->max_rows = kmscon_text_get_rows(txt, FONT_HEIGHT(txt));
	txt->cols = txt->max_cols;
	txt->rows = txt->max_rows;
	gt->max_cells = GLTEX_CACHE_SCREENS * (txt->max_cols * txt->max_rows + 1);
	compute_advance_and_offset(txt);
	gltex_set_cos(gt, txt->orientation);

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s);
	if (s <= 0)
		s = 64;
	else if (s > 2048)
		s = 2048;
	gt->max_tex_size = s;

	gl_clear_error();
	return 0;

err_shader:
	gl_shader_unref(gt->shader);
err_htable:
	shl_hashtable_free(gt->glyphs);
	return ret;
}

static void gltex_unset(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret;
	struct dlist *tmp;
	struct atlas *atlas;
	bool gl = true;

	ret = display_use(txt->disp);
	if (ret) {
		gl = false;
		log_warning("cannot activate OpenGL-CTX during destruction");
	}

	shl_hashtable_free(gt->glyphs);

	dlist_for_each_entry_safe(atlas, tmp, &gt->atlases, list)
	{
		dlist_unlink(&atlas->list);

		free(atlas->cache_pos);
		free(atlas->cache_texpos);
		free(atlas->cache_fgcol);
		free(atlas->cache_bgcol);

		if (gl)
			gl_tex_free(&atlas->tex, 1);
		free(atlas);
	}

	if (gl) {
		gl_shader_unref(gt->shader);

		gl_clear_error();
	}
}

/* returns an atlas with at least 1 free glyph position; NULL on error */
static struct atlas *get_atlas(struct kmscon_text *txt, unsigned int num)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	size_t newsize;
	unsigned int width, height, nsize;
	GLenum err;

	/* an atlas with room, which after a flush may be an old one */
	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		if (atlas->fill + num <= atlas->count)
			return atlas;
	}

	/* all atlases are full so we have to create a new atlas */
	atlas = calloc(1, sizeof(*atlas));
	if (!atlas)
		return NULL;

	gl_clear_error();

	gl_tex_new(&atlas->tex, 1);
	err = glGetError();
	if (err != GL_NO_ERROR || !atlas->tex) {
		gl_clear_error();
		log_warning("cannot create new OpenGL texture: %d", err);
		goto err_free;
	}

	newsize = gt->max_tex_size / FONT_WIDTH(txt);
	if (newsize < 1)
		newsize = 1;

	/* OpenGL texture sizes are heavily restricted so we need to find a
	 * valid texture size that is big enough to hold as many glyphs as
	 * possible but at least 1 */
try_next:
	width = shl_next_pow2(FONT_WIDTH(txt) * newsize);
	height = shl_next_pow2(FONT_HEIGHT(txt));

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
		     NULL);

	err = glGetError();
	if (err != GL_NO_ERROR) {
		if (newsize > 1) {
			--newsize;
			goto try_next;
		}
		gl_clear_error();
		log_warning("OpenGL textures too small for a single glyph (%d)", err);
		goto err_tex;
	}

	log_debug("new atlas of size %ux%u for %zu", width, height, newsize);

	nsize = txt->max_cols * txt->max_rows + 1; // +1 for the mouse pointer

	atlas->cache_pos = malloc(sizeof(GLfloat) * nsize * 2 * 6);
	if (!atlas->cache_pos)
		goto err_mem;

	atlas->cache_texpos = malloc(sizeof(GLfloat) * nsize * 2 * 6);
	if (!atlas->cache_texpos)
		goto err_mem;

	atlas->cache_fgcol = malloc(sizeof(GLfloat) * nsize * 3 * 6);
	if (!atlas->cache_fgcol)
		goto err_mem;

	atlas->cache_bgcol = malloc(sizeof(GLfloat) * nsize * 3 * 6);
	if (!atlas->cache_bgcol)
		goto err_mem;

	atlas->cache_size = nsize;
	atlas->count = newsize;
	atlas->width = width;
	atlas->height = height;
	atlas->advance_htex = 1.0 / atlas->width * FONT_WIDTH(txt);
	atlas->advance_vtex = 1.0 / atlas->height * FONT_HEIGHT(txt);

	dlist_link(&gt->atlases, &atlas->list);
	return atlas;

err_mem:
	free(atlas->cache_pos);
	free(atlas->cache_texpos);
	free(atlas->cache_fgcol);
	free(atlas->cache_bgcol);
err_tex:
	gl_tex_free(&atlas->tex, 1);
err_free:
	free(atlas);
	return NULL;
}

/*
 * Until gltex composes decoration sprites itself (ADR-102, step 6), it bakes
 * them into the glyph mask, drawn in the foreground colour as before: every
 * underline style and strike-through, without decoration colours.
 */
static void bake_deco(struct kmscon_text *txt, struct kmscon_glyph *glyph, enum kmscon_deco kind)
{
	struct kmscon_font_deco m;
	struct kmscon_glyph *sprite;
	unsigned int fw = FONT_WIDTH(txt), x, y;

	kmscon_font_get_deco(txt->font, &m);
	sprite = kmscon_deco_render(kind, fw, FONT_HEIGHT(txt), &m);
	if (!sprite)
		return;
	for (y = 0; y < glyph->buf.height && y < sprite->buf.height; y++)
		for (x = 0; x < glyph->buf.width; x++) {
			uint8_t v = sprite->buf.data[y * fw + x % fw];
			uint8_t *d = &glyph->buf.data[y * glyph->buf.width + x];

			if (v > *d)
				*d = v;
		}
	free(sprite);
}

/*
 * Until gltex has an RGBA atlas (ADR-102, step 6), a colour glyph goes into
 * the alpha atlas as its alpha, drawn in the foreground like text: the right
 * shape and size, without its colours.
 */
static void colour_to_mask(struct kmscon_glyph *glyph)
{
	const uint32_t *px = (const uint32_t *)glyph->buf.data;
	size_t i, n = (size_t)glyph->buf.width * glyph->buf.height;

	if (glyph->buf.format != VIDEO_FORMAT_ARGB32)
		return;
	/* byte i is written after pixel i, at or past it, has been read */
	for (i = 0; i < n; i++)
		glyph->buf.data[i] = px[i] >> 24;
	glyph->buf.format = VIDEO_FORMAT_A8;
}

static struct gl_glyph *find_glyph(struct kmscon_text *txt, uint64_t key, uint16_t flags,
				   unsigned int width, unsigned int underline, bool strike)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct gl_glyph *glglyph;
	GLenum err;
	struct kmscon_font *font = txt->font;
	unsigned int num;
	struct kmscon_glyph *glyph;
	struct kmscon_font_attr attr;
	struct kmscon_glyph_key k = {0};
	const uint32_t *cps;
	uint32_t one;
	size_t len;
	uint64_t id;

	attr.italic = !!(flags & TSM_FRAME_ITALIC);
	attr.bold = !!(flags & TSM_FRAME_BOLD);

	k.key = key ? key : ' ';
	k.bold = attr.bold;
	k.italic = attr.italic;
	k.width = width;
	k.extra = (underline & 7) | (strike ? 8 : 0);
	id = kmscon_glyph_key_hash(&k);

	if (shl_hashtable_find(gt->glyphs, (void **)&glglyph, id)) {
		if (kmscon_glyph_key_equal(&glglyph->key, &k))
			return glglyph;
		/* a hash collision: the newer glyph takes the slot */
		shl_hashtable_remove(gt->glyphs, id);
		free(glglyph);
	}

	cps = kmscon_text_get_cluster(txt, k.key, &one, &len);

	glglyph = calloc(1, sizeof(*glglyph));
	if (!glglyph)
		return NULL;

	glyph = kmscon_font_render(font, &attr, cps, len, width);
	if (!glyph) {
		free(glglyph);
		return NULL;
	}
	colour_to_mask(glyph);
	if (underline > KMSCON_DECO_NONE && underline < KMSCON_DECO_STRIKE)
		bake_deco(txt, glyph, underline);
	if (strike)
		bake_deco(txt, glyph, KMSCON_DECO_STRIKE);

	glglyph->key = k;
	glglyph->double_width = glyph->double_width;

	num = kmscon_glyph_cwidth(glyph);
	atlas = get_atlas(txt, num);
	if (!atlas)
		goto err_free;

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * atlas->fill, 0, GLYPH_WIDTH(glyph),
			GLYPH_HEIGHT(glyph), GL_ALPHA, GL_UNSIGNED_BYTE, GLYPH_DATA(glyph));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

	/* Check for GL-errors
	 * As OpenGL is a state-machine, we cannot really tell which call failed
	 * without adding a glGetError() after each call. This is totally
	 * overkill so let us at least catch the error afterwards.
	 * We also add a hint to disable OpenGL if this does not work. This
	 * should _always_ work but OpenGL is kind of a black-box that isn't
	 * verbose at all and many things can go wrong. */

	err = glGetError();
	if (err != GL_NO_ERROR) {
		gl_clear_error();
		log_warning("cannot load glyph data into OpenGL texture (%d: %s); disable the "
			    "GL-renderer if this does not work reliably",
			    err, gl_err_to_str(err));
		goto err_free;
	}

	glglyph->atlas = atlas;
	glglyph->texoff = atlas->fill;

	if (shl_hashtable_insert(gt->glyphs, id, glglyph))
		goto err_free;

	atlas->fill += num;
	gt->cells += num;
	free(glyph);

	return glglyph;

err_free:
	free(glyph);
	free(glglyph);
	return NULL;
}

static void gltex_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows)
{
	txt->cols = cols;
	txt->rows = rows;
	compute_advance_and_offset(txt);
}

static int gltex_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	txt->orientation = orientation;

	gltex_unset(txt);
	gltex_set(txt);
	return 0;
}

/*
 * Empty the glyph cache and refill every atlas from its first cell; the atlases
 * and their textures stay. Atlas cells are handed out in order and not given
 * back one at a time, and a cluster's frame key changes each time the screen
 * re-creates the cluster, so without this the atlases grow without bound. It
 * runs between frames, as no queued vertex may name a cell that is refilled.
 */
static void gltex_flush(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	struct shl_hashtable *glyphs;
	struct atlas *atlas;

	if (shl_hashtable_new(&glyphs, shl_direct_hash, shl_direct_equal, free_glyph))
		return; /* keep the full cache and try again next frame */
	shl_hashtable_free(gt->glyphs);
	gt->glyphs = glyphs;

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		atlas->fill = 0;
	}
	gt->cells = 0;
}

static int gltex_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	int ret;

	ret = display_use(txt->disp);
	if (ret)
		return ret;

	if (gt->cells > gt->max_cells)
		gltex_flush(txt);

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		atlas->cache_num = 0;
	}
	gt->attr = *attr;

	glClearColor(gt->attr.br / 255.0, gt->attr.bg / 255.0, gt->attr.bb / 255.0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	return 0;
}

/*
 * Draw @cell at @posx, @posy with glyph key @key (0: none), @cwidth cells wide
 * (a wide glyph in the last column is cut at the margin by the atlas).
 */
static int gltex_draw_cell(struct kmscon_text *txt, const struct tsm_frame_cell *cell,
			   uint64_t key, unsigned int posx, unsigned int posy, unsigned int cwidth)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct gl_glyph *glglyph;
	float gl_x1, gl_x2, gl_y1, gl_y2;
	float width;
	int i, idx;

	glglyph = find_glyph(txt, key, cell->flags, cwidth, cell->underline,
			     cell->flags & TSM_FRAME_STRIKE);
	if (!glglyph)
		return -ENOMEM;

	atlas = glglyph->atlas;

	if (atlas->cache_num >= atlas->cache_size)
		return -ERANGE;

	width = glglyph->double_width ? 2.0 : 1.0;

	idx = atlas->cache_num * 2 * 6;
	gl_x1 = gt->off_x + gt->advance_x * posx - 1.0;
	gl_x2 = gl_x1 + width * gt->advance_x;
	gl_y1 = 1.0 - gt->off_y - gt->advance_y * posy;
	gl_y2 = gl_y1 - gt->advance_y;

	atlas->cache_pos[idx + 0] = gl_x1;
	atlas->cache_pos[idx + 1] = gl_y1;
	atlas->cache_pos[idx + 2] = gl_x1;
	atlas->cache_pos[idx + 3] = gl_y2;
	atlas->cache_pos[idx + 4] = gl_x2;
	atlas->cache_pos[idx + 5] = gl_y2;

	atlas->cache_pos[idx + 6] = gl_x1;
	atlas->cache_pos[idx + 7] = gl_y1;
	atlas->cache_pos[idx + 8] = gl_x2;
	atlas->cache_pos[idx + 9] = gl_y2;
	atlas->cache_pos[idx + 10] = gl_x2;
	atlas->cache_pos[idx + 11] = gl_y1;

	atlas->cache_texpos[idx + 0] = glglyph->texoff;
	atlas->cache_texpos[idx + 1] = 0.0;
	atlas->cache_texpos[idx + 2] = glglyph->texoff;
	atlas->cache_texpos[idx + 3] = 1.0;
	atlas->cache_texpos[idx + 4] = glglyph->texoff + width;
	atlas->cache_texpos[idx + 5] = 1.0;

	atlas->cache_texpos[idx + 6] = glglyph->texoff;
	atlas->cache_texpos[idx + 7] = 0.0;
	atlas->cache_texpos[idx + 8] = glglyph->texoff + width;
	atlas->cache_texpos[idx + 9] = 1.0;
	atlas->cache_texpos[idx + 10] = glglyph->texoff + width;
	atlas->cache_texpos[idx + 11] = 0.0;

	for (i = 0; i < 6; ++i) {
		idx = atlas->cache_num * 3 * 6 + i * 3;
		atlas->cache_fgcol[idx + 0] = cell->fg.r / 255.0;
		atlas->cache_fgcol[idx + 1] = cell->fg.g / 255.0;
		atlas->cache_fgcol[idx + 2] = cell->fg.b / 255.0;
		atlas->cache_bgcol[idx + 0] = cell->bg.r / 255.0;
		atlas->cache_bgcol[idx + 1] = cell->bg.g / 255.0;
		atlas->cache_bgcol[idx + 2] = cell->bg.b / 255.0;
	}

	++atlas->cache_num;

	return 0;
}

static int gltex_draw(struct kmscon_text *txt, const struct tsm_frame *frame, bool cursor_on)
{
	unsigned int posx, posy, cols, rows, width;
	const struct tsm_frame_cell *fc, *cell;
	bool is_cursor;
	uint64_t key;

	cols = min(txt->cols, frame->cols);
	rows = min(txt->rows, frame->rows);

	for (posy = 0; posy < rows; posy++) {
		for (posx = 0; posx < cols; posx++) {
			fc = tsm_frame_cell_at(frame, posx, posy);

			/* a trailing half is drawn by the wide cell before it */
			if (!fc->width && posx)
				continue;

			width = fc->width == 2 ? 2 : 1;
			is_cursor = cursor_on && frame->cursor.x == posx && frame->cursor.y == posy;
			cell = is_cursor ? &frame->cursor.cell : fc;
			key = cell->key;
			if (!is_cursor && (fc->flags & TSM_FRAME_BLINK) && txt->blinking)
				key = 0;
			if (is_cursor && (frame->cursor.style == TSM_SCREEN_CURSOR_VBAR_BLINK ||
					  frame->cursor.style == TSM_SCREEN_CURSOR_VBAR_STEADY)) {
				/* the bar, at the left of the cell; a wide cell's
				 * second half is left empty */
				if (width == 2 && posx + 1 < cols)
					gltex_draw_cell(txt, cell, 0, posx + 1, posy, 1);
				key = FONT_VBAR;
				width = 1;
			}

			gltex_draw_cell(txt, cell, key, posx, posy, width);
		}
	}
	return 0;
}

static int gltex_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct gl_glyph *glyph;
	float gl_x1, gl_x2, gl_y1, gl_y2;
	unsigned int sw, sh;
	int i, idx;
	glyph = find_glyph(txt, 'I', 0, 1, TSM_UNDERLINE_NONE, false);
	if (!glyph)
		return -ENOMEM;

	atlas = glyph->atlas;

	if (atlas->cache_num >= atlas->cache_size)
		return -ERANGE;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		sw = gt->sw;
		sh = gt->sh;
	} else {
		sw = gt->sh;
		sh = gt->sw;
	}

	if (x > sw)
		x = sw;

	if (y > sh)
		y = sh;

	gl_x1 = gt->off_x + x * 2.0 / sw - 1.0 - gt->advance_x / 2.0;
	gl_y1 = 1.0 - gt->off_y - y * 2.0 / sh + gt->advance_y / 2.0;
	gl_x2 = gl_x1 + gt->advance_x;
	gl_y2 = gl_y1 - gt->advance_y;

	idx = atlas->cache_num * 2 * 6;

	atlas->cache_pos[idx + 0] = gl_x1;
	atlas->cache_pos[idx + 1] = gl_y1;
	atlas->cache_pos[idx + 2] = gl_x1;
	atlas->cache_pos[idx + 3] = gl_y2;
	atlas->cache_pos[idx + 4] = gl_x2;
	atlas->cache_pos[idx + 5] = gl_y2;

	atlas->cache_pos[idx + 6] = gl_x1;
	atlas->cache_pos[idx + 7] = gl_y1;
	atlas->cache_pos[idx + 8] = gl_x2;
	atlas->cache_pos[idx + 9] = gl_y2;
	atlas->cache_pos[idx + 10] = gl_x2;
	atlas->cache_pos[idx + 11] = gl_y1;

	atlas->cache_texpos[idx + 0] = glyph->texoff;
	atlas->cache_texpos[idx + 1] = 0.0;
	atlas->cache_texpos[idx + 2] = glyph->texoff;
	atlas->cache_texpos[idx + 3] = 1.0;
	atlas->cache_texpos[idx + 4] = glyph->texoff + 1.0;
	atlas->cache_texpos[idx + 5] = 1.0;

	atlas->cache_texpos[idx + 6] = glyph->texoff;
	atlas->cache_texpos[idx + 7] = 0.0;
	atlas->cache_texpos[idx + 8] = glyph->texoff + 1.0;
	atlas->cache_texpos[idx + 9] = 1.0;
	atlas->cache_texpos[idx + 10] = glyph->texoff + 1.0;
	atlas->cache_texpos[idx + 11] = 0.0;

	for (i = 0; i < 6; ++i) {
		idx = atlas->cache_num * 3 * 6 + i * 3;
		atlas->cache_fgcol[idx + 0] = gt->attr.fr / 255.0;
		atlas->cache_fgcol[idx + 1] = gt->attr.fg / 255.0;
		atlas->cache_fgcol[idx + 2] = gt->attr.fb / 255.0;
		atlas->cache_bgcol[idx + 0] = gt->attr.br / 255.0;
		atlas->cache_bgcol[idx + 1] = gt->attr.bg / 255.0;
		atlas->cache_bgcol[idx + 2] = gt->attr.bb / 255.0;
	}

	++atlas->cache_num;

	return 0;
}

static int gltex_render(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	float mat[16];

	gl_clear_error();

	gl_shader_use(gt->shader);

	glViewport(0, 0, gt->sw, gt->sh);
	glDisable(GL_BLEND);

	gl_m4_identity(mat);
	glUniformMatrix4fv(gt->uni_proj, 1, GL_FALSE, mat);
	glUniform1f(gt->uni_cos, gt->cos);
	glUniform1f(gt->uni_sin, gt->sin);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);

	glActiveTexture(GL_TEXTURE0);
	glUniform1i(gt->uni_atlas, 0);

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		if (!atlas->cache_num)
			continue;

		glBindTexture(GL_TEXTURE_2D, atlas->tex);
		glUniform1f(gt->uni_advance_htex, atlas->advance_htex);
		glUniform1f(gt->uni_advance_vtex, atlas->advance_vtex);

		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, atlas->cache_pos);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, atlas->cache_texpos);
		glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, atlas->cache_fgcol);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 0, atlas->cache_bgcol);
		glDrawArrays(GL_TRIANGLES, 0, 6 * atlas->cache_num);
	}

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(3);

	if (gl_has_error(gt->shader)) {
		log_warning("rendering console caused OpenGL errors");
		return -EFAULT;
	}

	return 0;
}

struct kmscon_text_ops kmscon_text_gltex_ops = {
	.name = "gltex",
	.owner = NULL,
	.init = gltex_init,
	.destroy = gltex_destroy,
	.set = gltex_set,
	.unset = gltex_unset,
	.resize = gltex_resize,
	.rotate = gltex_rotate,
	.prepare = gltex_prepare,
	.draw = gltex_draw,
	.draw_pointer = gltex_draw_pointer,
	.render = gltex_render,
	.abort = NULL,
};
