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
 *
 * kmscon fork (ADR-102 step 6): an atlas holds alpha masks or premultiplied
 * RGBA colour glyphs, and the shader tints the first and draws the second as
 * it is. Underlines and strike-through are deco.c's sprites, cells of an alpha
 * atlas, which the shader draws under and over the glyph in their own colours.
 * Upstream has alpha atlases only and no decorations; this departure is
 * recorded in ADR-102's corrections from step 6 (ADR-303).
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
	GLenum format; /* GL_ALPHA: masks; GL_RGBA: colour glyphs */
	unsigned int height;
	unsigned int width;
	unsigned int count;
	unsigned int fill;

	unsigned int cache_num; /* quads queued this frame */
	unsigned int cache_off; /* their first slot in the vertex arrays */

	GLfloat advance_htex;
	GLfloat advance_vtex;
};

struct gl_glyph {
	bool double_width;
	struct kmscon_glyph_key key;
	struct atlas *atlas;
	unsigned int texoff;
};

/*
 * A quad queued for the frame, a cell or the pointer: @atlas's cells @texoff
 * to @texoff + @texw drawn over @x1, @y1 to @x2, @y2, with the decoration
 * sprites at cells @ulcell and @strikecell of the decoration atlas tinted
 * @ul and @strike (the blank sprite for none).
 */
struct gltex_quad {
	struct atlas *atlas;
	GLfloat x1, y1, x2, y2;
	GLfloat texoff, texw;
	GLfloat ulcell, strikecell;
	struct tsm_screen_color fg, bg, ul, strike;
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

/* the shader's vertex attributes, gltex_set()'s attr */
#define GLTEX_ATTRS 7

struct gltex {
	struct shl_hashtable *glyphs;
	unsigned int max_tex_size;
	unsigned int cells;	/* atlas cells handed out since the last flush */
	unsigned int max_cells; /* flush at the next frame once cells passes this */

	struct dlist atlases;

	/*
	 * The frame's quads in the order they were queued, at most one a cell
	 * and one for the pointer, and the vertex arrays gltex_render() fills
	 * from them, each atlas's quads together. All atlases share them, so
	 * their size follows the cell count, not the number of atlases (#43).
	 * Upstream gives each atlas its own arrays: an ADR-303 departure users
	 * cannot see, recorded in PR #180.
	 */
	struct gltex_quad *quads;
	unsigned int quads_num;
	unsigned int quads_size;
	GLfloat *cache_pos;
	GLfloat *cache_texpos;
	GLfloat *cache_fgcol;
	GLfloat *cache_bgcol;
	GLfloat *cache_ulcol;
	GLfloat *cache_strikecol;
	GLfloat *cache_deco;

	/*
	 * The decoration sprites, each kind at cell deco_off + its kind of
	 * deco_atlas, KMSCON_DECO_NONE blank. They are uploaded for the first
	 * cell drawn after a flush, which drops them with the glyphs. An upload
	 * that fails is tried once a frame (deco_tried), and the frame is drawn
	 * without decorations.
	 */
	struct atlas *deco_atlas;
	unsigned int deco_off;
	bool deco_tried;

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
	GLuint uni_colour;
	GLuint uni_deco;
	GLuint uni_deco_htex;
	GLuint uni_deco_vtex;
	GLuint uni_decorated;

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

static void free_quads(struct gltex *gt)
{
	free(gt->quads);
	free(gt->cache_pos);
	free(gt->cache_texpos);
	free(gt->cache_fgcol);
	free(gt->cache_bgcol);
	free(gt->cache_ulcol);
	free(gt->cache_strikecol);
	free(gt->cache_deco);
	gt->quads = NULL;
	gt->cache_pos = gt->cache_texpos = gt->cache_fgcol = gt->cache_bgcol = NULL;
	gt->cache_ulcol = gt->cache_strikecol = gt->cache_deco = NULL;
	gt->quads_num = gt->quads_size = 0;
}

/* room for @n quads, in the queue and in the vertex arrays */
static int alloc_quads(struct gltex *gt, unsigned int n)
{
	gt->quads = malloc(sizeof(*gt->quads) * n);
	gt->cache_pos = malloc(sizeof(GLfloat) * n * 2 * 6);
	gt->cache_texpos = malloc(sizeof(GLfloat) * n * 2 * 6);
	gt->cache_fgcol = malloc(sizeof(GLfloat) * n * 3 * 6);
	gt->cache_bgcol = malloc(sizeof(GLfloat) * n * 3 * 6);
	gt->cache_ulcol = malloc(sizeof(GLfloat) * n * 3 * 6);
	gt->cache_strikecol = malloc(sizeof(GLfloat) * n * 3 * 6);
	gt->cache_deco = malloc(sizeof(GLfloat) * n * 3 * 6);
	if (!gt->quads || !gt->cache_pos || !gt->cache_texpos || !gt->cache_fgcol ||
	    !gt->cache_bgcol || !gt->cache_ulcol || !gt->cache_strikecol || !gt->cache_deco) {
		free_quads(gt);
		return -ENOMEM;
	}
	gt->quads_size = n;
	return 0;
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
	static char *attr[] = {"position", "texture_position", "fgcolor",   "bgcolor",
			       "ulcolor",  "strikecolor",      "decoration"};
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

	ret = gl_shader_new(&gt->shader, vert, vlen, frag, flen, attr, GLTEX_ATTRS);
	if (ret)
		goto err_htable;

	gt->uni_cos = gl_shader_get_uniform(gt->shader, "cos");
	gt->uni_sin = gl_shader_get_uniform(gt->shader, "sin");
	gt->uni_proj = gl_shader_get_uniform(gt->shader, "projection");
	gt->uni_atlas = gl_shader_get_uniform(gt->shader, "atlas");
	gt->uni_advance_htex = gl_shader_get_uniform(gt->shader, "advance_htex");
	gt->uni_advance_vtex = gl_shader_get_uniform(gt->shader, "advance_vtex");
	gt->uni_colour = gl_shader_get_uniform(gt->shader, "colour");
	gt->uni_deco = gl_shader_get_uniform(gt->shader, "deco");
	gt->uni_deco_htex = gl_shader_get_uniform(gt->shader, "deco_htex");
	gt->uni_deco_vtex = gl_shader_get_uniform(gt->shader, "deco_vtex");
	gt->uni_decorated = gl_shader_get_uniform(gt->shader, "decorated");

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
	ret = alloc_quads(gt, txt->max_cols * txt->max_rows + 1); // +1 for the mouse pointer
	if (ret)
		goto err_shader;
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

		if (gl)
			gl_tex_free(&atlas->tex, 1);
		free(atlas);
	}
	free_quads(gt);

	if (gl) {
		gl_shader_unref(gt->shader);

		gl_clear_error();
	}
}

/* returns a @format atlas with @num free glyph positions; NULL on error */
static struct atlas *get_atlas(struct kmscon_text *txt, unsigned int num, GLenum format)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	size_t newsize;
	unsigned int width, height;
	GLenum err;

	/* an atlas with room, which after a flush may be an old one */
	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		if (atlas->format == format && atlas->fill + num <= atlas->count)
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
	glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, NULL);

	err = glGetError();
	if (err != GL_NO_ERROR) {
		if (newsize > num) {
			--newsize;
			goto try_next;
		}
		gl_clear_error();
		log_warning("OpenGL textures too small for %u glyph cells (%d)", num, err);
		goto err_tex;
	}
	if (newsize < num) {
		log_warning("OpenGL textures too small for %u glyph cells", num);
		goto err_tex;
	}

	log_debug("new atlas of size %ux%u for %zu", width, height, newsize);

	atlas->format = format;
	atlas->count = newsize;
	atlas->width = width;
	atlas->height = height;
	atlas->advance_htex = 1.0 / atlas->width * FONT_WIDTH(txt);
	atlas->advance_vtex = 1.0 / atlas->height * FONT_HEIGHT(txt);

	dlist_link(&gt->atlases, &atlas->list);
	return atlas;

err_tex:
	gl_tex_free(&atlas->tex, 1);
err_free:
	free(atlas);
	return NULL;
}

/*
 * Upload @w x @h pixels of @format at @atlas's cell @cell. A GL error is
 * reported and fails it, with a hint to disable the GL renderer.
 */
static int upload(struct kmscon_text *txt, struct atlas *atlas, unsigned int cell, unsigned int w,
		  unsigned int h, GLenum format, const void *data)
{
	GLenum err;

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * cell, 0, w, h, format, GL_UNSIGNED_BYTE,
			data);
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
		return -EFAULT;
	}
	return 0;
}

/*
 * The decoration sprites (deco.c) as cells of an alpha atlas, uploaded once
 * after each flush; see struct gltex. Each is drawn tinted, under the glyph
 * or over it, by the shader.
 */
static int get_deco(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	struct kmscon_font_deco m;
	struct kmscon_glyph *sprite;
	struct atlas *atlas;
	unsigned int fw = FONT_WIDTH(txt), fh = FONT_HEIGHT(txt), w = fw * KMSCON_DECO_COUNT;
	unsigned int kind, y;
	uint8_t *strip;
	int ret = -ENOMEM;

	if (gt->deco_atlas)
		return 0;
	if (gt->deco_tried)
		return -EAGAIN;
	gt->deco_tried = true;

	strip = calloc(w, fh);
	if (!strip)
		return -ENOMEM;
	kmscon_font_get_deco(txt->font, &m);
	for (kind = KMSCON_DECO_NONE + 1; kind < KMSCON_DECO_COUNT; kind++) {
		sprite = kmscon_deco_render(kind, fw, fh, &m);
		if (!sprite)
			goto out;
		for (y = 0; y < fh; y++)
			memcpy(&strip[y * w + kind * fw], &sprite->buf.data[y * fw], fw);
		free(sprite);
	}

	atlas = get_atlas(txt, KMSCON_DECO_COUNT, GL_ALPHA);
	if (!atlas)
		goto out;
	ret = upload(txt, atlas, atlas->fill, w, fh, GL_ALPHA, strip);
	if (ret)
		goto out;

	gt->deco_atlas = atlas;
	gt->deco_off = atlas->fill;
	atlas->fill += KMSCON_DECO_COUNT;
	gt->cells += KMSCON_DECO_COUNT;
out:
	free(strip);
	return ret;
}

/*
 * A colour glyph's premultiplied ARGB32 pixels rewritten in place as the bytes
 * GL_RGBA reads: R, G, B, A.
 */
static void argb_to_rgba(struct kmscon_glyph *glyph)
{
	uint8_t *b = glyph->buf.data;
	size_t i, n = (size_t)glyph->buf.width * glyph->buf.height;

	for (i = 0; i < n; i++, b += 4) {
		uint32_t v = *(const uint32_t *)b;

		b[0] = v >> 16;
		b[1] = v >> 8;
		b[2] = v;
		b[3] = v >> 24;
	}
}

static struct gl_glyph *find_glyph(struct kmscon_text *txt, uint64_t key, uint16_t flags,
				   unsigned int width)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct gl_glyph *glglyph;
	GLenum format;
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

	if (!key)
		key = ' ';
	k.key = key;
	k.bold = attr.bold;
	k.italic = attr.italic;
	k.width = width;
	id = kmscon_glyph_key_hash(key, attr.bold, attr.italic, width, 0);

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
	/* a colour glyph goes to an RGBA atlas, drawn untinted */
	format = GL_ALPHA;
	if (glyph->buf.format == VIDEO_FORMAT_ARGB32) {
		argb_to_rgba(glyph);
		format = GL_RGBA;
	}

	glglyph->key = k;
	glglyph->double_width = glyph->double_width;

	num = kmscon_glyph_cwidth(glyph);
	atlas = get_atlas(txt, num, format);
	if (!atlas)
		goto err_free;

	if (upload(txt, atlas, atlas->fill, GLYPH_WIDTH(glyph), GLYPH_HEIGHT(glyph), format,
		   GLYPH_DATA(glyph)))
		goto err_free;

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
	gt->deco_atlas = NULL;
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
	gt->deco_tried = false;

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		atlas->cache_num = 0;
	}
	gt->quads_num = 0;
	gt->attr = *attr;

	glClearColor(gt->attr.br / 255.0, gt->attr.bg / 255.0, gt->attr.bb / 255.0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	return 0;
}

/* queue quad @q for the frame; -ERANGE once the frame holds its maximum */
static int queue_quad(struct gltex *gt, const struct gltex_quad *q)
{
	if (gt->quads_num >= gt->quads_size)
		return -ERANGE;

	gt->quads[gt->quads_num++] = *q;
	++q->atlas->cache_num;
	return 0;
}

/* the six vertices of quad @q, two triangles, at slot @i of the vertex arrays */
static void write_quad(struct gltex *gt, unsigned int i, const struct gltex_quad *q)
{
	GLfloat *pos = &gt->cache_pos[i * 2 * 6];
	GLfloat *tex = &gt->cache_texpos[i * 2 * 6];
	GLfloat t1 = q->texoff, t2 = q->texoff + q->texw;
	static const GLfloat right[6] = {0, 0, 1, 0, 1, 1}; /* vertex at x2 */
	int j, idx;

	pos[0] = q->x1;
	pos[1] = q->y1;
	pos[2] = q->x1;
	pos[3] = q->y2;
	pos[4] = q->x2;
	pos[5] = q->y2;

	pos[6] = q->x1;
	pos[7] = q->y1;
	pos[8] = q->x2;
	pos[9] = q->y2;
	pos[10] = q->x2;
	pos[11] = q->y1;

	tex[0] = t1;
	tex[1] = 0.0;
	tex[2] = t1;
	tex[3] = 1.0;
	tex[4] = t2;
	tex[5] = 1.0;

	tex[6] = t1;
	tex[7] = 0.0;
	tex[8] = t2;
	tex[9] = 1.0;
	tex[10] = t2;
	tex[11] = 0.0;

	for (j = 0; j < 6; ++j) {
		idx = i * 3 * 6 + j * 3;
		gt->cache_fgcol[idx + 0] = q->fg.r / 255.0;
		gt->cache_fgcol[idx + 1] = q->fg.g / 255.0;
		gt->cache_fgcol[idx + 2] = q->fg.b / 255.0;
		gt->cache_bgcol[idx + 0] = q->bg.r / 255.0;
		gt->cache_bgcol[idx + 1] = q->bg.g / 255.0;
		gt->cache_bgcol[idx + 2] = q->bg.b / 255.0;
		gt->cache_ulcol[idx + 0] = q->ul.r / 255.0;
		gt->cache_ulcol[idx + 1] = q->ul.g / 255.0;
		gt->cache_ulcol[idx + 2] = q->ul.b / 255.0;
		gt->cache_strikecol[idx + 0] = q->strike.r / 255.0;
		gt->cache_strikecol[idx + 1] = q->strike.g / 255.0;
		gt->cache_strikecol[idx + 2] = q->strike.b / 255.0;
		/* the vertex's x within the quad's cells: 0 or @texw */
		idx = i * 3 * 6 + j * 3;
		gt->cache_deco[idx + 0] = right[j] * q->texw;
		gt->cache_deco[idx + 1] = q->ulcell;
		gt->cache_deco[idx + 2] = q->strikecell;
	}
}

/*
 * Draw @cell at @posx, @posy with glyph key @key (0: none), @cwidth cells wide
 * (a wide glyph in the last column is cut at the margin by the atlas), and its
 * underline and strike-through in their colours, as bbulk draws them.
 * @ul_cursor: @cell's underline is an underline cursor, which is no
 * decoration and draws in the foreground.
 * When the glyph cannot be had, the cell is drawn without it and -ENOMEM
 * returned, as bbulk does.
 */
static int gltex_draw_cell(struct kmscon_text *txt, const struct tsm_frame_cell *cell, uint64_t key,
			   unsigned int posx, unsigned int posy, unsigned int cwidth,
			   bool ul_cursor)
{
	struct gltex *gt = txt->data;
	struct gl_glyph *glglyph;
	struct gltex_quad q = {0};
	unsigned int i, n = 1;
	float width;
	int ret = 0, r;

	glglyph = find_glyph(txt, key, cell->flags, cwidth);
	if (!glglyph) {
		/* a blank in each of its cells, which fits where a wide glyph
		 * fits no texture, keeps its background and decorations */
		glglyph = find_glyph(txt, ' ', cell->flags, 1);
		if (!glglyph)
			return -ENOMEM;
		n = cwidth;
		ret = -ENOMEM;
	}

	width = glglyph->double_width ? 2.0 : 1.0;

	q.atlas = glglyph->atlas;
	q.y1 = 1.0 - gt->off_y - gt->advance_y * posy;
	q.y2 = q.y1 - gt->advance_y;
	q.texoff = glglyph->texoff;
	q.texw = width;
	q.fg = cell->fg;
	q.bg = cell->bg;
	/* without the sprites this frame, the cell is drawn undecorated */
	if (!get_deco(txt)) {
		q.ulcell = q.strikecell = gt->deco_off + KMSCON_DECO_NONE;
		if (cell->underline > TSM_UNDERLINE_NONE && cell->underline < KMSCON_DECO_STRIKE) {
			q.ulcell = gt->deco_off + cell->underline;
			q.ul = ul_cursor ? cell->fg : kmscon_text_ul_color(txt, cell);
		}
		if (cell->flags & TSM_FRAME_STRIKE) {
			q.strikecell = gt->deco_off + KMSCON_DECO_STRIKE;
			q.strike = kmscon_text_strike_color(txt, cell);
		}
	}

	for (i = 0; i < n; i++) {
		q.x1 = gt->off_x + gt->advance_x * (posx + i) - 1.0;
		q.x2 = q.x1 + width * gt->advance_x;
		r = queue_quad(gt, &q);
		if (r)
			return r;
	}
	return ret;
}

/*
 * The frame's image placements (ADR-103 step 3f): here gltex will compose the
 * three bands, a texture per image key, on the RGBA path ADR-102 step 6 gave
 * it (#68). Until step 3f it draws none, and kmscon_text_gltex_ops leaves
 * draws_images false, so no terminal gltex draws for offers sixel.
 */
static void gltex_draw_placements(struct kmscon_text *txt, const struct tsm_frame *frame)
{
	(void)txt;
	(void)frame;
}

static int gltex_draw(struct kmscon_text *txt, const struct tsm_frame *frame, bool cursor_on)
{
	unsigned int posx, posy, cols, rows, width;
	const struct tsm_frame_cell *fc, *cell;
	bool is_cursor, ul_cursor;
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
					gltex_draw_cell(txt, cell, 0, posx + 1, posy, 1, false);
				key = FONT_VBAR;
				width = 1;
			}
			ul_cursor = is_cursor &&
				    (frame->cursor.style == TSM_SCREEN_CURSOR_UNDERLINE_BLINK ||
				     frame->cursor.style == TSM_SCREEN_CURSOR_UNDERLINE_STEADY);

			gltex_draw_cell(txt, cell, key, posx, posy, width, ul_cursor);
		}
	}
	gltex_draw_placements(txt, frame);
	return 0;
}

static int gltex_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y)
{
	struct gltex *gt = txt->data;
	struct gl_glyph *glyph;
	struct gltex_quad q = {0};
	unsigned int sw, sh;

	glyph = find_glyph(txt, 'I', 0, 1);
	if (!glyph)
		return -ENOMEM;

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

	q.atlas = glyph->atlas;
	q.x1 = gt->off_x + x * 2.0 / sw - 1.0 - gt->advance_x / 2.0;
	q.y1 = 1.0 - gt->off_y - y * 2.0 / sh + gt->advance_y / 2.0;
	q.x2 = q.x1 + gt->advance_x;
	q.y2 = q.y1 - gt->advance_y;
	q.texoff = glyph->texoff;
	q.texw = 1.0;
	q.fg = (struct tsm_screen_color){gt->attr.fr, gt->attr.fg, gt->attr.fb};
	q.bg = (struct tsm_screen_color){gt->attr.br, gt->attr.bg, gt->attr.bb};
	if (!get_deco(txt))
		q.ulcell = q.strikecell = gt->deco_off + KMSCON_DECO_NONE;
	return queue_quad(gt, &q);
}

static int gltex_render(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas, *deco;
	struct gltex_quad *q;
	unsigned int i, off = 0;
	float mat[16];

	/* each atlas's quads together, in the order they were queued */
	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		atlas->cache_off = off;
		off += atlas->cache_num;
		atlas->cache_num = 0;
	}
	for (i = 0; i < gt->quads_num; i++) {
		q = &gt->quads[i];
		write_quad(gt, q->atlas->cache_off + q->atlas->cache_num++, q);
	}

	gl_clear_error();

	gl_shader_use(gt->shader);

	glViewport(0, 0, gt->sw, gt->sh);
	glDisable(GL_BLEND);

	gl_m4_identity(mat);
	glUniformMatrix4fv(gt->uni_proj, 1, GL_FALSE, mat);
	glUniform1f(gt->uni_cos, gt->cos);
	glUniform1f(gt->uni_sin, gt->sin);

	for (i = 0; i < GLTEX_ATTRS; i++)
		glEnableVertexAttribArray(i);

	/*
	 * every quad's decoration sprites come from the one atlas on unit 1.
	 * A frame without them (see get_deco) is drawn undecorated, and unit 1
	 * still holds an atlas: an incomplete texture samples as alpha 1.
	 */
	deco = gt->deco_atlas;
	if (!deco && !dlist_empty(&gt->atlases))
		deco = dlist_first(&gt->atlases, struct atlas, list);
	if (deco) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, deco->tex);
		glUniform1i(gt->uni_deco, 1);
		glUniform1f(gt->uni_deco_htex, deco->advance_htex);
		glUniform1f(gt->uni_deco_vtex, deco->advance_vtex);
	}
	glUniform1f(gt->uni_decorated, gt->deco_atlas ? 1.0 : 0.0);

	glActiveTexture(GL_TEXTURE0);
	glUniform1i(gt->uni_atlas, 0);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, gt->cache_pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, gt->cache_texpos);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, gt->cache_fgcol);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 0, gt->cache_bgcol);
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, 0, gt->cache_ulcol);
	glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, 0, gt->cache_strikecol);
	glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, 0, gt->cache_deco);

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		if (!atlas->cache_num)
			continue;

		glBindTexture(GL_TEXTURE_2D, atlas->tex);
		glUniform1i(gt->uni_colour, atlas->format == GL_RGBA);
		glUniform1f(gt->uni_advance_htex, atlas->advance_htex);
		glUniform1f(gt->uni_advance_vtex, atlas->advance_vtex);
		glDrawArrays(GL_TRIANGLES, 6 * atlas->cache_off, 6 * atlas->cache_num);
	}

	for (i = 0; i < GLTEX_ATTRS; i++)
		glDisableVertexAttribArray(i);

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
	.draws_images = false, /* until ADR-103 step 3f: gltex_draw_placements */
};
