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
 *
 * Images (ADR-103 steps 3f and 4e) are the frame's placements, in three bands,
 * drawn in layers: the clear to the default background, the lowest band, the
 * cell backgrounds that are not the default, the middle band, the text (the
 * glyphs and decorations, premultiplied and blended over what is under them),
 * the top band, and the mouse pointer last (#181). A frame without placements
 * draws each cell's background and text in one opaque pass, then the pointer.
 * Each image is a texture of its own, uploaded when a placement of it is first
 * drawn and scaled down to fit GL_MAX_TEXTURE_SIZE when it is larger, or, when
 * the whole image does not fit the budget, at the size the placement draws
 * it; a placement draws its source rectangle, clipped at its box and the
 * screen, as one quad, scaled and rotated on the GPU. An upload GL refuses is
 * not tried again while the image is shown. The textures count in ADR-103's
 * image budget (image_budget.c) as bbulk's scaled pictures do: one that does
 * not fit is left out until the room in the budget grows, and those no frame
 * has drawn since go, least recently used first, once the textures pass
 * image_budget_cache_limit, or when the budget asks for room. Upstream draws
 * each cell opaque in one pass per atlas; this departure is recorded in
 * ADR-103's gltex section (ADR-303).
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
#include "gltex_image.frag.bin.h"
#include "gltex_image.vert.bin.h"
#include "image_budget.h"
#include "picture.h"
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

/* an image's texture (ADR-103), kept between frames */
struct gltex_image {
	struct gltex_image *next;
	bool overlay; /* the key is the overlay's, not the screen's */
	uint64_t key;
	GLuint tex;
	unsigned int iw, ih; /* the image's size */
	unsigned int w, h;   /* the texture's: the image's, unless scaled */
	bool scaled;	     /* smaller than the image */
	/*
	 * the image at the size a placement draws it, made where the whole
	 * image does not fit the budget; made again when a placement draws it
	 * larger
	 */
	bool drawn_size;
	bool grow_failed;  /* GL refused it larger: not tried again while shown */
	bool failed;	   /* GL refused it: not tried again, no texture */
	GLint filter;	   /* its GL_TEXTURE_MIN_FILTER and MAG_FILTER */
	uint64_t last_use; /* the frame that last drew it */
	size_t bytes;
};

/*
 * A placement queued for the frame: @image's texture coordinates @u1, @v1 to
 * @u2, @v2 drawn over @x1, @y1 to @x2, @y2, in @band, sampled within @clip
 * (its source rectangle, half a texel in from each edge, so bilinear
 * filtering takes nothing from beside it); @exact when it draws one texel a
 * pixel, so it is sampled as it is.
 */
struct gltex_pic {
	struct gltex_image *image;
	GLfloat x1, y1, x2, y2;
	GLfloat u1, v1, u2, v2;
	GLfloat clip[4];
	uint8_t band;
	bool exact;
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
	/* first, so gltex_trim finds gltex by it; in the budget while counted is set */
	struct image_budget_cache cache;
	struct shl_hashtable *glyphs;
	unsigned int max_tex_size;
	unsigned int cells;	/* atlas cells handed out since the last flush */
	unsigned int max_cells; /* flush at the next frame once cells passes this */

	struct dlist atlases;

	/*
	 * The frame's cell quads in the order they were queued, at most one a
	 * cell, and the vertex arrays gltex_render() fills from them, each
	 * atlas's quads together, the pointer's in the slot after them. All
	 * atlases share them, so their size follows the cell count, not the
	 * number of atlases (#43). Upstream gives each atlas its own arrays: an
	 * ADR-303 departure users cannot see, recorded in PR #180.
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

	/* the mouse pointer's quad, drawn after everything else (#181) */
	struct gltex_quad pointer;
	bool has_pointer;

	/*
	 * The image layer (ADR-103): the frame's placements in draw order and
	 * their vertex arrays; the textures, most recently made first, and
	 * their bytes; the names of textures given up between frames, deleted
	 * once the GL context is ours again (room for one per texture, so
	 * giving one up never fails); and the frame's placements while it is
	 * drawn, for the budget's question of which images it draws.
	 */
	struct gltex_pic *pics;
	unsigned int pics_num;
	unsigned int pics_size;
	GLfloat *pic_pos;
	GLfloat *pic_texpos;
	struct gltex_image *images;
	unsigned int images_num;
	size_t image_bytes;
	GLuint *doomed;
	unsigned int doomed_num;
	unsigned int doomed_size;
	const struct tsm_frame *frame;
	uint64_t frame_count;
	unsigned int max_image_size; /* GL_MAX_TEXTURE_SIZE */
	struct display *counted;     /* the display counted in the image budget, while set */
	bool starved;		     /* a placement was left out for the budget */
	uint64_t starved_gen;	     /* image_budget_gen() when it was */
	bool upload_warned;

	struct gl_shader *img_shader;
	GLuint uni_img_cos;
	GLuint uni_img_sin;
	GLuint uni_img_proj;
	GLuint uni_img_picture;
	GLuint uni_img_clip;

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
	GLuint uni_layer;

	unsigned int sw;
	unsigned int sh;

	GLfloat cos;
	GLfloat sin;

	struct tsm_screen_attr attr;
};

static int gltex_init(struct kmscon_text *txt)
{
	struct gltex *gt;

	gt = calloc(1, sizeof(*gt));
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

/* room for @n cell quads in the queue, and for them and the pointer in the vertex arrays */
static int alloc_quads(struct gltex *gt, unsigned int n)
{
	gt->quads = malloc(sizeof(*gt->quads) * n++);
	gt->cache_pos = malloc(sizeof(GLfloat) * n * 2 * 6);
	gt->cache_texpos = malloc(sizeof(GLfloat) * n * 2 * 6);
	gt->cache_fgcol = malloc(sizeof(GLfloat) * n * 3 * 6);
	gt->cache_bgcol = malloc(sizeof(GLfloat) * n * 4 * 6);
	gt->cache_ulcol = malloc(sizeof(GLfloat) * n * 3 * 6);
	gt->cache_strikecol = malloc(sizeof(GLfloat) * n * 3 * 6);
	gt->cache_deco = malloc(sizeof(GLfloat) * n * 3 * 6);
	if (!gt->quads || !gt->cache_pos || !gt->cache_texpos || !gt->cache_fgcol ||
	    !gt->cache_bgcol || !gt->cache_ulcol || !gt->cache_strikecol || !gt->cache_deco) {
		free_quads(gt);
		return -ENOMEM;
	}
	gt->quads_size = n - 1;
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

/*
 * The image shader. Its fragment shader asks for highp where the GPU has it:
 * a texture coordinate in a picture thousands of texels wide needs more than
 * mediump's 10 bits. The embedded source has no newlines, which a
 * preprocessor line needs, so the line is put before it here.
 */
static int image_shader_new(struct gltex *gt)
{
	static const char head[] = "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
				   "precision highp float;\n"
				   "#else\n"
				   "precision mediump float;\n"
				   "#endif\n";
	static char *attr[] = {"position", "texture_position"};
	int flen = sizeof(head) - 1 + _binary_gltex_image_frag_size;
	char *frag;
	int ret;

	frag = malloc(flen);
	if (!frag)
		return -ENOMEM;
	memcpy(frag, head, sizeof(head) - 1);
	memcpy(frag + sizeof(head) - 1, _binary_gltex_image_frag_start,
	       _binary_gltex_image_frag_size);
	ret = gl_shader_new(&gt->img_shader, _binary_gltex_image_vert_start,
			    _binary_gltex_image_vert_size, frag, flen, attr, 2);
	free(frag);
	if (ret)
		return ret;

	gt->uni_img_cos = gl_shader_get_uniform(gt->img_shader, "cos");
	gt->uni_img_sin = gl_shader_get_uniform(gt->img_shader, "sin");
	gt->uni_img_proj = gl_shader_get_uniform(gt->img_shader, "projection");
	gt->uni_img_picture = gl_shader_get_uniform(gt->img_shader, "picture");
	gt->uni_img_clip = gl_shader_get_uniform(gt->img_shader, "clip");
	return 0;
}

static void gltex_trim(struct image_budget_cache *cache, size_t want);
static size_t gltex_trimmable(struct image_budget_cache *cache);

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
	gt->uni_layer = gl_shader_get_uniform(gt->shader, "layer");

	if (gl_has_error(gt->shader)) {
		log_warning("cannot create shader");
		goto err_shader;
	}

	ret = image_shader_new(gt);
	if (ret)
		goto err_shader;
	if (gl_has_error(gt->img_shader)) {
		log_warning("cannot create image shader");
		ret = -EFAULT;
		goto err_img_shader;
	}

	gt->sw = display_get_width(txt->disp);
	gt->sh = display_get_height(txt->disp);

	txt->max_cols = kmscon_text_get_cols(txt, FONT_WIDTH(txt));
	txt->max_rows = kmscon_text_get_rows(txt, FONT_HEIGHT(txt));
	txt->cols = txt->max_cols;
	txt->rows = txt->max_rows;
	gt->max_cells = GLTEX_CACHE_SCREENS * (txt->max_cols * txt->max_rows + 1);
	ret = alloc_quads(gt, txt->max_cols * txt->max_rows);
	if (ret)
		goto err_img_shader;
	compute_advance_and_offset(txt);
	gltex_set_cos(gt, txt->orientation);

	/* an image's texture may take the GPU's largest; an atlas, 2048 */
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s);
	if (s <= 0)
		s = 64;
	gt->max_image_size = s;
	gt->max_tex_size = s > 2048 ? 2048 : s;

	ret = image_budget_count_display(txt->disp, (uint64_t)gt->sw * gt->sh);
	if (ret)
		goto err_quads;
	gt->counted = txt->disp;
	gt->cache.trim = gltex_trim;
	gt->cache.trimmable = gltex_trimmable;
	image_budget_add_cache(&gt->cache);

	gl_clear_error();
	return 0;

err_quads:
	free_quads(gt);
err_img_shader:
	gl_shader_unref(gt->img_shader);
err_shader:
	gl_shader_unref(gt->shader);
err_htable:
	shl_hashtable_free(gt->glyphs);
	return ret;
}

static void images_free(struct gltex *gt, bool gl);
static void doomed_flush(struct gltex *gt);

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
	images_free(gt, gl);
	if (gt->counted) {
		image_budget_remove_cache(&gt->cache);
		image_budget_uncount_display(gt->counted);
		gt->counted = NULL;
	}

	dlist_for_each_entry_safe(atlas, tmp, &gt->atlases, list)
	{
		dlist_unlink(&atlas->list);

		if (gl)
			gl_tex_free(&atlas->tex, 1);
		free(atlas);
	}
	free_quads(gt);

	if (gl) {
		gl_shader_unref(gt->img_shader);
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
	doomed_flush(gt);

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		atlas->cache_num = 0;
	}
	gt->quads_num = 0;
	gt->has_pointer = false;
	gt->pics_num = 0;
	gt->frame_count++;
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
	/* the background's alpha: 1 unless it is the default */
	GLfloat bga = q->bg.r != gt->attr.br || q->bg.g != gt->attr.bg || q->bg.b != gt->attr.bb;
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
		gt->cache_bgcol[i * 4 * 6 + j * 4 + 0] = q->bg.r / 255.0;
		gt->cache_bgcol[i * 4 * 6 + j * 4 + 1] = q->bg.g / 255.0;
		gt->cache_bgcol[i * 4 * 6 + j * 4 + 2] = q->bg.b / 255.0;
		gt->cache_bgcol[i * 4 * 6 + j * 4 + 3] = bga;
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

/* ---- the image layer (ADR-103) ---- */

/*
 * @pp's texture goes: out of the budget now, and out of GL once the context
 * is this display's again (doomed_flush), since the budget may ask for room
 * while another display's frame is being drawn
 */
static void image_free(struct gltex *gt, struct gltex_image **pp)
{
	struct gltex_image *gi = *pp;

	*pp = gi->next;
	if (gi->tex)
		gt->doomed[gt->doomed_num++] = gi->tex;
	gt->image_bytes -= gi->bytes;
	gt->images_num--;
	image_budget_remove_picture(gi->bytes);
	free(gi);
}

/* delete the textures given up; the GL context is this display's */
static void doomed_flush(struct gltex *gt)
{
	if (gt->doomed_num)
		gl_tex_free(gt->doomed, gt->doomed_num);
	gt->doomed_num = 0;
}

/* every texture and the image layer's arrays; @gl: the GL context is ours */
static void images_free(struct gltex *gt, bool gl)
{
	while (gt->images)
		image_free(gt, &gt->images);
	if (gl)
		doomed_flush(gt);
	free(gt->doomed);
	free(gt->pics);
	free(gt->pic_pos);
	free(gt->pic_texpos);
	gt->doomed = NULL;
	gt->pics = NULL;
	gt->pic_pos = gt->pic_texpos = NULL;
	gt->doomed_num = gt->doomed_size = gt->pics_num = gt->pics_size = 0;
	gt->starved = false;
}

/* @gi was drawn on this frame */
static bool image_in_use(struct gltex *gt, const struct gltex_image *gi)
{
	return gi->last_use == gt->frame_count;
}

/*
 * Free the least recently drawn textures this frame does not draw until @want
 * more bytes fit in @limit, or none such is left.
 */
static void images_trim(struct gltex *gt, size_t want, size_t limit)
{
	struct gltex_image **pp, **lru;

	while (gt->image_bytes + want > limit) {
		lru = NULL;
		for (pp = &gt->images; *pp; pp = &(*pp)->next) {
			if (image_in_use(gt, *pp) || (*pp)->failed)
				continue;
			if (!lru || (*pp)->last_use < (*lru)->last_use)
				lru = pp;
		}
		if (!lru)
			return;
		image_free(gt, lru);
	}
}

/* forget the uploads GL refused, whole or larger, that this frame no longer shows */
static void failed_free(struct gltex *gt)
{
	struct gltex_image **pp = &gt->images;

	while (*pp) {
		if (!image_in_use(gt, *pp))
			(*pp)->grow_failed = false;
		if ((*pp)->failed && !image_in_use(gt, *pp))
			image_free(gt, pp);
		else
			pp = &(*pp)->next;
	}
}

/*
 * The budget asks for @want bytes, between frames or within another
 * renderer's: the textures the last frame did not draw go, least recently
 * used first (ADR-103's first step of eviction).
 */
static void gltex_trim(struct image_budget_cache *cache, size_t want)
{
	struct gltex *gt = (struct gltex *)cache;

	images_trim(gt, 0, want < gt->image_bytes ? gt->image_bytes - want : 0);
}

/* what gltex_trim frees at most */
static size_t gltex_trimmable(struct image_budget_cache *cache)
{
	struct gltex *gt = (struct gltex *)cache;
	struct gltex_image *gi;
	size_t bytes = 0;

	for (gi = gt->images; gi; gi = gi->next)
		if (!image_in_use(gt, gi))
			bytes += gi->bytes;
	return bytes;
}

/* a placement left out here may fit now: the budget has grown since */
static bool gltex_starved(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;

	return gt && gt->starved && gt->starved_gen != image_budget_gen();
}

/* a session left the foreground: its textures give their room back */
static void gltex_release(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;

	while (gt->images)
		image_free(gt, &gt->images);
	gt->starved = false;
	if (!display_use(txt->disp))
		doomed_flush(gt);
}

/*
 * @con's image @key is shown by a placement of the frame that renderer @data
 * draws; without the frame's screen, every image counts as drawn
 */
static bool frame_draws(struct tsm_screen *con, uint64_t key, void *data)
{
	struct kmscon_text *txt = data;
	struct gltex *gt = txt->data;
	unsigned int i;

	if (con != txt->screen)
		return !txt->screen;
	for (i = 0; gt->frame && gt->frame->placements && i < gt->frame->placement_count; i++)
		if (tsm_frame_placement_at(gt->frame, i)->image_key == key)
			return true;
	return false;
}

static struct gltex_image *image_find(struct gltex *gt, bool overlay, uint64_t key)
{
	struct gltex_image *gi;

	for (gi = gt->images; gi; gi = gi->next)
		if (gi->overlay == overlay && gi->key == key)
			return gi;
	return NULL;
}

/* the texture's size for a @w x @h image: as it is, or scaled to fit max_image_size */
static void image_fit(struct gltex *gt, unsigned int w, unsigned int h, unsigned int *tw,
		      unsigned int *th)
{
	unsigned int m = gt->max_image_size;

	*tw = w;
	*th = h;
	if (w <= m && h <= m)
		return;
	if (w >= h) {
		*tw = m;
		*th = (uint64_t)h * m / w;
	} else {
		*th = m;
		*tw = (uint64_t)w * m / h;
	}
	*tw = *tw ? *tw : 1;
	*th = *th ? *th : 1;
}

/*
 * Upload @img's pixels to @gi's texture, @gi->w x @gi->h, scaled with
 * picture_scale() when that is smaller than the image. The bytes go as they
 * are in memory, read as GL_RGBA: B, G, R, A on a little-endian machine, which
 * the shader swizzles; on a big-endian one each pixel is swapped to that order
 * first. Returns 0, -ENOMEM or -EFAULT.
 */
static int image_upload(struct gltex *gt, struct gltex_image *gi, const struct tsm_frame_image *img)
{
	const uint32_t *src = img->pixels;
	size_t stride = img->stride / 4;
	uint32_t *tmp = NULL;
	unsigned int y;
	GLenum err;

	if (gi->scaled) {
		tmp = malloc((size_t)gi->w * gi->h * 4);
		if (!tmp)
			return -ENOMEM;
		if (picture_scale(src, stride, img->width, img->height, gi->w, gi->h, 0, 0, gi->w,
				  gi->h, tmp)) {
			free(tmp);
			return -ENOMEM;
		}
		src = tmp;
		stride = gi->w;
	}
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	{
		uint32_t *sw = tmp ? tmp : malloc((size_t)gi->w * gi->h * 4);
		unsigned int x;

		if (!sw)
			return -ENOMEM;
		for (y = 0; y < gi->h; y++)
			for (x = 0; x < gi->w; x++)
				sw[(size_t)y * gi->w + x] =
					__builtin_bswap32(src[(size_t)y * stride + x]);
		src = tmp = sw;
		stride = gi->w;
	}
#endif

	gl_clear_error();
	gl_tex_new(&gi->tex, 1);
	glBindTexture(GL_TEXTURE_2D, gi->tex);
	if (stride == gi->w) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gi->w, gi->h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			     src);
	} else {
		/* GLES 2 has no row length: a row at a time */
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gi->w, gi->h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			     NULL);
		for (y = 0; y < gi->h; y++)
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, gi->w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
					&src[(size_t)y * stride]);
	}
	free(tmp);
	gi->filter = GL_LINEAR;

	err = glGetError();
	if (err != GL_NO_ERROR || !gi->tex) {
		gl_clear_error();
		if (gi->tex)
			gl_tex_free(&gi->tex, 1);
		gi->tex = 0;
		if (!gt->upload_warned)
			log_warning("cannot load a %ux%u image into an OpenGL texture (%d: %s); "
				    "it is not drawn",
				    gi->w, gi->h, err, gl_err_to_str(err));
		gt->upload_warned = true;
		return -EFAULT;
	}
	return 0;
}

/*
 * A texture of @tw x @th for @img, the overlay's when @overlay, made within
 * the frame: room from this display's textures the frame does not draw, then
 * from the budget in ADR-103's order (image_budget_picture_room), as bbulk
 * makes a scaled picture. Returns 0, -ENOSPC when it does not fit, -ENOMEM,
 * or -EFAULT when GL refused it, which is remembered for the image's key, so
 * it is not tried, and room not made for it, again on every frame.
 */
static int image_new(struct kmscon_text *txt, bool overlay, uint64_t key,
		     const struct tsm_frame_image *img, unsigned int tw, unsigned int th,
		     struct gltex_image **out)
{
	struct gltex *gt = txt->data;
	struct gltex_image *gi;
	size_t bytes = (size_t)tw * th * 4;
	unsigned int n;
	GLuint *doomed;
	int ret;

	/*
	 * room for its name, and one more, among the textures given up, so
	 * giving one up never fails, even a texture replaced by a larger one
	 */
	n = gt->images_num + gt->doomed_num + 2;
	if (n > gt->doomed_size) {
		doomed = realloc(gt->doomed, (n + 16) * sizeof(*doomed));
		if (!doomed)
			return -ENOMEM;
		gt->doomed = doomed;
		gt->doomed_size = n + 16;
	}

	images_trim(gt, bytes, image_budget_cache_limit((uint64_t)gt->sw * gt->sh));
	images_trim(gt, bytes + image_budget_used() - gt->image_bytes, image_budget_limit());
	if (image_budget_used() + bytes > image_budget_limit() &&
	    image_budget_picture_room(bytes, frame_draws, txt))
		return -ENOSPC;

	gi = calloc(1, sizeof(*gi));
	if (!gi)
		return -ENOMEM;
	gi->overlay = overlay;
	gi->key = key;
	gi->iw = img->width;
	gi->ih = img->height;
	gi->w = tw;
	gi->h = th;
	gi->scaled = tw != img->width || th != img->height;
	gi->bytes = bytes;
	ret = image_upload(gt, gi, img);
	if (ret == -EFAULT) {
		gi->failed = true;
		gi->bytes = bytes = 0;
	} else if (ret) {
		free(gi);
		return ret;
	}

	gi->next = gt->images;
	gt->images = gi;
	gt->images_num++;
	gt->image_bytes += bytes;
	image_budget_add_picture(bytes);
	*out = gi;
	return ret;
}

/* room for @n placements this frame */
static int pics_reserve(struct gltex *gt, unsigned int n)
{
	struct gltex_pic *pics;
	GLfloat *pos, *texpos;

	if (n <= gt->pics_size)
		return 0;
	pics = realloc(gt->pics, n * sizeof(*pics));
	if (!pics)
		return -ENOMEM;
	gt->pics = pics;
	pos = realloc(gt->pic_pos, n * sizeof(*pos) * 2 * 6);
	if (!pos)
		return -ENOMEM;
	gt->pic_pos = pos;
	texpos = realloc(gt->pic_texpos, n * sizeof(*texpos) * 2 * 6);
	if (!texpos)
		return -ENOMEM;
	gt->pic_texpos = texpos;
	gt->pics_size = n;
	return 0;
}

/* free the textures of overlays before the one with key @key: there is one at a time */
static void overlays_free(struct gltex *gt, uint64_t key)
{
	struct gltex_image **pp = &gt->images;

	while (*pp) {
		if ((*pp)->overlay && (*pp)->key != key)
			image_free(gt, pp);
		else
			pp = &(*pp)->next;
	}
}

/*
 * The texture placement @fp draws from, the overlay's when @overlay, where @fp
 * draws its source rectangle @dw x @dh pixels: the image's existing texture,
 * or a new one of the whole image, scaled down only to fit
 * GL_MAX_TEXTURE_SIZE. Where that does not fit the budget and @fp draws the
 * image smaller, the texture is the image at the size @fp draws it (ADR-108:
 * hwaccel on draws the images hwaccel off draws, and bbulk draws such a
 * picture at its drawn size); one such is made again, larger, when a
 * placement draws it larger. NULL when there is none: no image, GL refused
 * it, or it does not fit, which marks the frame starved.
 */
static struct gltex_image *place_texture(struct kmscon_text *txt,
					 const struct tsm_frame_placement *fp, bool overlay,
					 unsigned int dw, unsigned int dh)
{
	struct gltex *gt = txt->data;
	struct gltex_image *gi, *old, **pp;
	struct tsm_frame_image img;
	unsigned int fw, fh, tw, th;
	int ret;

	gi = image_find(gt, overlay, fp->image_key);
	if (gi && gi->failed)
		return NULL;
	if (gi && gi->iw && gi->ih) {
		/* the image at the size this placement draws it */
		tw = ((uint64_t)gi->iw * dw + fp->src_width / 2) / fp->src_width;
		th = ((uint64_t)gi->ih * dh + fp->src_height / 2) / fp->src_height;
		if (!gi->drawn_size || (tw <= gi->w && th <= gi->h) || gi->grow_failed)
			return gi;
	}

	if (kmscon_text_get_image(txt, overlay, fp->image_key, &img))
		return gi;
	if (img.stride % 4 || img.stride / 4 < img.width || !img.width || !img.height)
		return gi;
	if (overlay)
		overlays_free(gt, fp->image_key);
	image_fit(gt, img.width, img.height, &fw, &fh);
	tw = ((uint64_t)img.width * dw + fp->src_width / 2) / fp->src_width;
	th = ((uint64_t)img.height * dh + fp->src_height / 2) / fp->src_height;
	image_fit(gt, tw ? tw : 1, th ? th : 1, &tw, &th);

	/* a drawn-size texture drawn larger: its room is the new one's to try */
	old = gi;
	if (old) {
		for (pp = &gt->images; *pp != old; pp = &(*pp)->next)
			;
		*pp = old->next;
		gt->images_num--;
		gt->image_bytes -= old->bytes;
		image_budget_remove_picture(old->bytes);
	}

	ret = image_new(txt, overlay, fp->image_key, &img, fw, fh, &gi);
	if (ret == -ENOSPC && (uint64_t)tw * th < (uint64_t)fw * fh) {
		ret = image_new(txt, overlay, fp->image_key, &img, tw, th, &gi);
		if (!ret)
			gi->drawn_size = true;
	}
	if (ret == -ENOSPC)
		gt->starved = true;
	else if (ret == -EFAULT)
		gi->last_use = gt->frame_count; /* remembered while shown */

	if (old) {
		/* put back: as it was, or among those given up */
		old->next = gt->images;
		gt->images = old;
		gt->images_num++;
		gt->image_bytes += old->bytes;
		image_budget_add_picture(old->bytes);
		if (!ret) {
			image_free(gt, &gt->images);
		} else {
			if (ret == -EFAULT && old->next == gi) {
				/* the refusal is old's to remember, not a key of its own */
				image_free(gt, &old->next);
				old->grow_failed = true;
			}
			return old;
		}
	}
	return ret ? NULL : gi;
}

/*
 * Queue placement @fp, the overlay's when @overlay, @cols x @rows cells of the
 * frame drawn: its picture's part inside its box and the grid, as bbulk
 * clips it, from its image's texture (place_texture). One that draws nothing,
 * or whose texture cannot be had, is left out.
 */
static void place_queue(struct kmscon_text *txt, const struct tsm_frame_placement *fp, bool overlay,
			unsigned int cols, unsigned int rows)
{
	struct gltex *gt = txt->data;
	long long fw = FONT_WIDTH(txt), fh = FONT_HEIGHT(txt), gx, gy, px0, px1, py0, py1;
	long long x0, x1, y0, y1, gw = cols * fw, gh = rows * fh;
	double sx = gt->advance_x / fw, sy = gt->advance_y / fh, lo, hi;
	struct gltex_image *gi;
	struct gltex_pic *p;
	unsigned int dw, dh, i;

	if (!fp->cell_width || !fp->cell_height || !fp->width || !fp->height || !fp->src_width ||
	    !fp->src_height || gt->pics_num >= gt->pics_size)
		return;

	/* the picture in grid pixels, scaled from the cells it was made in */
	gx = (long long)fp->col * fw;
	gy = (long long)fp->row * fh;
	px0 = gx + (long long)fp->x * fw / fp->cell_width;
	px1 = gx + ((long long)fp->x + fp->width) * fw / fp->cell_width;
	py0 = gy + (long long)fp->y * fh / fp->cell_height;
	py1 = gy + ((long long)fp->y + fp->height) * fh / fp->cell_height;

	/* its part inside its box and the grid */
	x0 = px0 > gx ? px0 : gx;
	y0 = py0 > gy ? py0 : gy;
	x0 = x0 > 0 ? x0 : 0;
	y0 = y0 > 0 ? y0 : 0;
	x1 = px1 < gx + (long long)fp->cols * fw ? px1 : gx + (long long)fp->cols * fw;
	y1 = py1 < gy + (long long)fp->rows * fh ? py1 : gy + (long long)fp->rows * fh;
	x1 = x1 < gw ? x1 : gw;
	y1 = y1 < gh ? y1 : gh;
	if (x0 >= x1 || y0 >= y1)
		return;
	dw = px1 - px0;
	dh = py1 - py0;

	gi = place_texture(txt, fp, overlay, dw, dh);
	if (!gi || fp->src_x >= gi->iw || fp->src_y >= gi->ih ||
	    fp->src_width > gi->iw - fp->src_x || fp->src_height > gi->ih - fp->src_y)
		return;
	gi->last_use = gt->frame_count;

	/* the visible part's texture coordinates, of the image's whole */
	p = &gt->pics[gt->pics_num++];
	p->image = gi;
	p->band = fp->band;
	/* one texel a pixel, from a texel's edge */
	p->exact = (uint64_t)gi->w * fp->src_width == (uint64_t)dw * gi->iw &&
		   (uint64_t)gi->h * fp->src_height == (uint64_t)dh * gi->ih &&
		   (uint64_t)fp->src_x * gi->w % gi->iw == 0 &&
		   (uint64_t)fp->src_y * gi->h % gi->ih == 0;
	p->x1 = gt->off_x + x0 * sx - 1.0;
	p->x2 = gt->off_x + x1 * sx - 1.0;
	p->y1 = 1.0 - gt->off_y - y0 * sy;
	p->y2 = 1.0 - gt->off_y - y1 * sy;
	p->u1 = (fp->src_x + (x0 - px0) * (double)fp->src_width / dw) / gi->iw;
	p->u2 = (fp->src_x + (x1 - px0) * (double)fp->src_width / dw) / gi->iw;
	p->v1 = (fp->src_y + (y0 - py0) * (double)fp->src_height / dh) / gi->ih;
	p->v2 = (fp->src_y + (y1 - py0) * (double)fp->src_height / dh) / gi->ih;

	/* the source rectangle in the texture's texels, half a texel in */
	for (i = 0; i < 2; i++) {
		unsigned int n = i ? gi->h : gi->w, size = i ? gi->ih : gi->iw;
		unsigned int s0 = i ? fp->src_y : fp->src_x;
		unsigned int s1 = s0 + (i ? fp->src_height : fp->src_width);

		lo = (double)s0 * n / size + 0.5;
		hi = (double)s1 * n / size - 0.5;
		if (lo > hi)
			lo = hi = (lo + hi) / 2;
		p->clip[i] = lo / n;
		p->clip[2 + i] = hi / n;
	}
}

/*
 * The frame's image placements, and the overlay after them, in draw order
 * (ADR-103), @cols x @rows cells of the frame drawn. The textures they name
 * are marked drawn first, so making one for a later placement never frees one
 * an earlier placement drew.
 */
static void gltex_draw_placements(struct kmscon_text *txt, const struct tsm_frame *frame,
				  unsigned int cols, unsigned int rows)
{
	struct gltex *gt = txt->data;
	unsigned int nf = frame->placements ? frame->placement_count : 0, i;
	size_t size = min(sizeof(struct tsm_frame_placement), frame->placement_size);
	struct tsm_frame_placement fp;
	struct gltex_image *gi;

	if (pics_reserve(gt, nf + 1))
		return;
	for (i = 0; i < nf; i++) {
		gi = image_find(gt, false, tsm_frame_placement_at(frame, i)->image_key);
		if (gi)
			gi->last_use = gt->frame_count;
	}
	if (txt->overlay) {
		gi = image_find(gt, true, txt->overlay->placement.image_key);
		if (gi)
			gi->last_use = gt->frame_count;
	}

	for (i = 0; i < nf; i++) {
		/* an older libtsm's entry is smaller: the rest stays 0 */
		memset(&fp, 0, sizeof(fp));
		memcpy(&fp, tsm_frame_placement_at(frame, i), size);
		place_queue(txt, &fp, false, cols, rows);
	}
	if (txt->overlay) {
		/* the overlay, at the right end of the top row */
		struct tsm_frame_placement ov = txt->overlay->placement;

		ov.col = cols > ov.cols ? cols - ov.cols : 0;
		place_queue(txt, &ov, true, cols, rows);
	}
}

static int gltex_draw(struct kmscon_text *txt, const struct tsm_frame *frame, bool cursor_on)
{
	struct gltex *gt = txt->data;
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

	gt->frame = frame;
	gt->starved = false;
	gltex_draw_placements(txt, frame, cols, rows);
	gt->frame = NULL;
	gt->starved_gen = image_budget_gen();
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
	gt->pointer = q;
	gt->has_pointer = true;
	return 0;
}

/* the cell shader's layers: a cell whole and opaque, its background, its text */
enum gltex_layer {
	LAYER_CELL = 0,
	LAYER_BACKGROUND = 1,
	LAYER_TEXT = 2,
};

/* the cell shader and its vertex arrays, for a pass drawing @layer */
static void cells_begin(struct gltex *gt, const float *mat, enum gltex_layer layer)
{
	struct atlas *deco;
	unsigned int i;

	gl_shader_use(gt->shader);
	glUniformMatrix4fv(gt->uni_proj, 1, GL_FALSE, mat);
	glUniform1f(gt->uni_cos, gt->cos);
	glUniform1f(gt->uni_sin, gt->sin);
	glUniform1i(gt->uni_layer, layer);

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
	glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 0, gt->cache_bgcol);
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, 0, gt->cache_ulcol);
	glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, 0, gt->cache_strikecol);
	glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, 0, gt->cache_deco);
}

static void cells_end(void)
{
	unsigned int i;

	for (i = 0; i < GLTEX_ATTRS; i++)
		glDisableVertexAttribArray(i);
}

/* @atlas's @n quads from slot @off of the vertex arrays */
static void cells_draw(struct gltex *gt, struct atlas *atlas, unsigned int off, unsigned int n)
{
	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glUniform1i(gt->uni_colour, atlas->format == GL_RGBA);
	glUniform1f(gt->uni_advance_htex, atlas->advance_htex);
	glUniform1f(gt->uni_advance_vtex, atlas->advance_vtex);
	glDrawArrays(GL_TRIANGLES, 6 * off, 6 * n);
}

/* a pass over the cell quads, each atlas's in one draw: they never overlap */
static void cells_pass(struct gltex *gt, const float *mat, enum gltex_layer layer)
{
	struct atlas *atlas;

	cells_begin(gt, mat, layer);
	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		if (atlas->cache_num)
			cells_draw(gt, atlas, atlas->cache_off, atlas->cache_num);
	}
	cells_end();
}

/*
 * The placements of @band, in the frame's order, each from its texture,
 * blended premultiplied over what is drawn: sampled as they are where they
 * draw a texel a pixel, bilinear where they are scaled.
 */
static void pics_pass(struct gltex *gt, const float *mat, uint8_t band)
{
	struct gltex_pic *p;
	unsigned int i;
	bool begun = false;
	GLint filter;

	for (i = 0; i < gt->pics_num; i++) {
		p = &gt->pics[i];
		if (p->band != band)
			continue;
		if (!begun) {
			gl_shader_use(gt->img_shader);
			glUniformMatrix4fv(gt->uni_img_proj, 1, GL_FALSE, mat);
			glUniform1f(gt->uni_img_cos, gt->cos);
			glUniform1f(gt->uni_img_sin, gt->sin);
			glUniform1i(gt->uni_img_picture, 0);
			glActiveTexture(GL_TEXTURE0);
			glEnableVertexAttribArray(0);
			glEnableVertexAttribArray(1);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, gt->pic_pos);
			glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, gt->pic_texpos);
			begun = true;
		}
		glBindTexture(GL_TEXTURE_2D, p->image->tex);
		glUniform4f(gt->uni_img_clip, p->clip[0], p->clip[1], p->clip[2], p->clip[3]);
		filter = p->exact ? GL_NEAREST : GL_LINEAR;
		if (p->image->filter != filter) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
			p->image->filter = filter;
		}
		glDrawArrays(GL_TRIANGLES, 6 * i, 6);
	}
	if (begun) {
		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);
	}
}

/* the six vertices of placement @p, two triangles, at slot @i of its arrays */
static void write_pic(struct gltex *gt, unsigned int i, const struct gltex_pic *p)
{
	GLfloat *pos = &gt->pic_pos[i * 2 * 6], *tex = &gt->pic_texpos[i * 2 * 6];
	const GLfloat v[6][4] = {
		{p->x1, p->y1, p->u1, p->v1}, {p->x1, p->y2, p->u1, p->v2},
		{p->x2, p->y2, p->u2, p->v2}, {p->x1, p->y1, p->u1, p->v1},
		{p->x2, p->y2, p->u2, p->v2}, {p->x2, p->y1, p->u2, p->v1},
	};
	unsigned int j;

	for (j = 0; j < 6; j++) {
		pos[j * 2] = v[j][0];
		pos[j * 2 + 1] = v[j][1];
		tex[j * 2] = v[j][2];
		tex[j * 2 + 1] = v[j][3];
	}
}

/*
 * The frame in ADR-103's layers. Without placements, each cell is drawn
 * whole in one opaque pass. With them: the lowest band over the default
 * background the frame was cleared to, the cell backgrounds that are not the
 * default, the middle band, the cells' text blended over it, the top band.
 * The pointer is drawn last, whole, in a draw of its own (#181). Then the
 * textures this frame did not draw go while they pass the cache's bound.
 */
static int gltex_render(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct gltex_quad *q;
	unsigned int i, off = 0;
	float mat[16];
	int ret = 0;

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
	if (gt->has_pointer)
		write_quad(gt, gt->quads_size, &gt->pointer);
	for (i = 0; i < gt->pics_num; i++)
		write_pic(gt, i, &gt->pics[i]);

	gl_clear_error();

	glViewport(0, 0, gt->sw, gt->sh);
	gl_m4_identity(mat);

	if (!gt->pics_num) {
		glDisable(GL_BLEND);
		cells_pass(gt, mat, LAYER_CELL);
	} else {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		pics_pass(gt, mat, TSM_FRAME_BAND_BELOW_BG);
		glDisable(GL_BLEND);
		cells_pass(gt, mat, LAYER_BACKGROUND);
		glEnable(GL_BLEND);
		pics_pass(gt, mat, TSM_FRAME_BAND_BELOW_TEXT);
		cells_pass(gt, mat, LAYER_TEXT);
		pics_pass(gt, mat, TSM_FRAME_BAND_ABOVE_TEXT);
		glDisable(GL_BLEND);
	}

	if (gt->has_pointer) {
		cells_begin(gt, mat, LAYER_CELL);
		cells_draw(gt, gt->pointer.atlas, gt->quads_size, 1);
		cells_end();
	}

	if (gl_has_error(gt->shader)) {
		log_warning("rendering console caused OpenGL errors");
		ret = -EFAULT;
	}

	images_trim(gt, 0, image_budget_cache_limit((uint64_t)gt->sw * gt->sh));
	failed_free(gt);
	doomed_flush(gt);
	return ret;
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
	.release = gltex_release,
	.starved = gltex_starved,
	.draws_images = true,
};
