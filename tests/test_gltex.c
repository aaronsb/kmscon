/*
 * The gltex glyph cache (ADR-102) holds each kind of glyph in its own pool of
 * strips within a bound in glyphs and bytes and reuses the least recently
 * drawn glyph's slot, never one the frame being drawn uses; the tests from
 * "the glyph cache" on are its specification, and a reference LRU in the
 * test checks it frame by frame. A cluster's frame key carries the screen's generation, so a
 * screen that re-creates its clusters asks for new keys every frame; they age
 * out. Its memory stays linear in the cell count at a 4K cell grid. The GL
 * stubs hold gltex to the GLES 2 texture rules a real driver enforces (#17).
 * Blinking cells lose their glyph in the hidden phase. Decorations are the
 * real decoration sprites (deco.c), drawn in their own colours, and colour
 * glyphs go to an RGBA atlas drawn untinted (ADR-102 step 6). Each pixel of a
 * cell samples its own texel's centre, GL_NEAREST, at the precision the cell
 * shader asks for (#197). The GL and display calls are stubs: the test counts
 * the textures gltex creates, keeps the pixels it uploads, records each
 * draw's textures and uniforms, and checks where its glyphs land in them and
 * in the frame. We include the implementation to reach its statics.
 *
 * Image placements (ADR-103 steps 3f and 4e): one texture per image, uploaded
 * on its first draw as the image's bytes, scaled down to fit
 * GL_MAX_TEXTURE_SIZE; the textures count in the image budget's own budget
 * for textures, apart from libtsm's images, and those no frame draws go past
 * the cache's bound; the frame is drawn in ADR-103's
 * layers, each with its blend state, and the pointer last (#181). The image
 * budget and the scaler are the real ones.
 */

#include <GLES2/gl2.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/font/font.h"
#include "../src/render/deco.h"
#include "../src/render/image_budget.h"
#include "../src/render/picture.h"
#include "../src/render/text.h"
#include "../src/shl/gl.h"
#include "../src/video/video.h"

/* mallinfo2() is glibc's, from 2.33; elsewhere the heap is not measured */
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#define HAVE_MALLINFO2 1
#else
#define HAVE_MALLINFO2 0
#endif

#define FAKE_CELL_W 8
#define FAKE_CELL_H 16
#define FAKE_COLS (640 / FAKE_CELL_W)
#define FAKE_ROWS (480 / FAKE_CELL_H)

static unsigned int disp_w = 640, disp_h = 480;
static unsigned int n_textures; /* textures created */
static unsigned int unit;	/* the texture unit glActiveTexture made active */
static GLuint bound[2];		/* the texture glBindTexture last bound on each unit */
#define bound_tex bound[unit]

/* the uniforms gltex asks for, each at its index here, and their values */
static const char *const uniform_names[] = {
	"cos",	"sin",	     "projection", "atlas",	"advance_htex", "advance_vtex", "colour",
	"deco", "deco_htex", "deco_vtex",  "decorated", "layer",	"picture",	"clip",
};
#define N_UNIFORMS (sizeof(uniform_names) / sizeof(*uniform_names))
static GLint uniform_i[N_UNIFORMS];
static GLfloat uniform_f[N_UNIFORMS];
enum {
	U_COS = 0,
	U_SIN = 1,
	U_ATLAS = 3,
	U_ADVANCE_H = 4,
	U_ADVANCE_V = 5,
	U_COLOUR = 6,
	U_DECO = 7,
	U_DECO_H = 8,
	U_DECO_V = 9,
	U_DECORATED = 10,
	U_LAYER = 11,
	U_PICTURE = 12,
	U_CLIP = 13,
};
static GLfloat uniform_clip[4]; /* the image shader's clip, glUniform4f's */

/* the shader gl_shader_use made current, and the blend state */
static struct gl_shader *program;
static bool blend_on;
static GLenum blend_src, blend_dst;

/*
 * a draw: the textures on units 0 and 1, the uniforms it was drawn with, its
 * shader, the blend state, and the filter of the texture on unit 0
 */
static struct draw_call {
	GLuint tex, deco_tex;
	GLint first;
	GLsizei count;
	GLint colour, atlas_unit, deco_unit, layer, picture_unit;
	GLfloat advance_h, advance_v, deco_h, deco_v, decorated, cos, sin;
	struct gl_shader *program;
	bool blend;
	GLenum src, dst;
	GLint filter, mag;
	GLfloat clip[4];
} draws[4096];
static unsigned int n_draws;	/* glDrawArrays calls since the last reset */
static unsigned int n_rendered; /* glyphs rasterised */

/*
 * What GL knows of each texture, so the stubs fail where a driver raises
 * GL_INVALID_OPERATION or GL_INVALID_VALUE (#17). As in GLES 2, binding a name
 * gltex never generated, or one it deleted, makes a texture without storage
 * (a stray); a glyph uploaded into a texture glTexImage2D gave no storage, or
 * outside it, is the error, where #17's driver failed.
 */
static struct fake_tex {
	GLuint name;
	bool live;     /* generated and not deleted, or bound as a stray */
	GLsizei w, h;  /* its storage; 0 before glTexImage2D */
	GLenum format; /* its storage's format */
	GLint filter;  /* GL_TEXTURE_MIN_FILTER, which MAG_FILTER must match */
	GLint mag;
	uint8_t *data; /* the pixels glTexImage2D gave it, or NULL */
} texs[16384], strays[16];
static unsigned int n_tex_images; /* glTexImage2D calls with pixels */
static unsigned int n_tex_rgba;	  /* glTexImage2D calls for RGBA storage */
static bool fail_teximage;	  /* glTexImage2D of RGBA storage raises GL_OUT_OF_MEMORY */
static unsigned int n_strays;

/* the texture GL knows by @name, a new stray when it knows none */
static struct fake_tex *tex_of(GLuint name)
{
	unsigned int i;

	if (name < sizeof(texs) / sizeof(*texs) && texs[name].live)
		return &texs[name];
	for (i = 0; i < n_strays; i++)
		if (strays[i].name == name)
			return &strays[i];
	assert(n_strays < sizeof(strays) / sizeof(*strays));
	strays[n_strays] = (struct fake_tex){.name = name, .live = true};
	return &strays[n_strays++];
}

/* the pixels uploaded while keep_uploads is set */
static bool keep_uploads;
static struct upload {
	GLuint tex;
	GLint x;
	GLsizei w, h;
	GLenum format;
	uint8_t *data; /* w * h pixels, a byte each for GL_ALPHA, four for GL_RGBA */
} uploads[64];
static unsigned int n_uploads;

/* the decoration sprites' strip: its uploads, and whether they fail */
#define STRIP_W (KMSCON_DECO_COUNT * FAKE_CELL_W)
static unsigned int strip_uploads;
static bool fail_strip;
/* the next uploads that fail, any of them */
static unsigned int fail_uploads;

static unsigned int bytes_per_pixel(GLenum format)
{
	assert(format == GL_ALPHA || format == GL_RGBA);
	return format == GL_RGBA ? 4 : 1;
}

/* ---- GL ---- */
void glActiveTexture(GLenum texture)
{
	assert(texture == GL_TEXTURE0 || texture == GL_TEXTURE1);
	unit = texture - GL_TEXTURE0;
}
void glBindTexture(GLenum target, GLuint texture)
{
	tex_of(texture);
	bound_tex = texture;
}
void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	blend_src = sfactor;
	blend_dst = dfactor;
}
void glClear(GLbitfield mask) {}
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {}
void glDisable(GLenum cap)
{
	assert(cap == GL_BLEND);
	blend_on = false;
}
void glEnable(GLenum cap)
{
	assert(cap == GL_BLEND);
	blend_on = true;
}
void glDisableVertexAttribArray(GLuint index) {}
void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	assert(n_draws < sizeof(draws) / sizeof(*draws));
	draws[n_draws++] = (struct draw_call){
		.layer = uniform_i[U_LAYER],
		.picture_unit = uniform_i[U_PICTURE],
		.cos = uniform_f[U_COS],
		.sin = uniform_f[U_SIN],
		.program = program,
		.blend = blend_on,
		.src = blend_src,
		.dst = blend_dst,
		.filter = tex_of(bound[0])->filter,
		.mag = tex_of(bound[0])->mag,
		.clip = {uniform_clip[0], uniform_clip[1], uniform_clip[2], uniform_clip[3]},
		.tex = bound[0],
		.deco_tex = bound[1],
		.first = first,
		.count = count,
		.colour = uniform_i[U_COLOUR],
		.atlas_unit = uniform_i[U_ATLAS],
		.deco_unit = uniform_i[U_DECO],
		.advance_h = uniform_f[U_ADVANCE_H],
		.advance_v = uniform_f[U_ADVANCE_V],
		.deco_h = uniform_f[U_DECO_H],
		.deco_v = uniform_f[U_DECO_V],
		.decorated = uniform_f[U_DECORATED],
	};
}
void glEnableVertexAttribArray(GLuint index) {}
/* the error the last failed call raised, until glGetError reads it */
static GLenum gl_error = GL_NO_ERROR;
GLenum glGetError(void)
{
	GLenum err = gl_error;

	gl_error = GL_NO_ERROR;
	return err;
}
/*
 * GL_MAX_TEXTURE_SIZE, which a test may lower, and the largest texture
 * glTexImage2D takes: the same, but for test_glyph_fallback, whose texture one
 * cell wide is still a cell high
 */
static GLint max_texture_size = 2048;
static GLint texture_limit = 2048;
void glGetIntegerv(GLenum pname, GLint *data)
{
	*data = max_texture_size;
}
void glPixelStorei(GLenum pname, GLint param) {}
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
		  GLint border, GLenum format, GLenum type, const void *pixels)
{
	struct fake_tex *t = tex_of(bound_tex);

	assert(level == 0);
	/* GLES 2 has no conversion: the storage takes the format of the pixels */
	assert(internalformat == (GLint)format && type == GL_UNSIGNED_BYTE);
	bytes_per_pixel(format);
	assert(width > 0 && width <= texture_limit && height > 0 && height <= texture_limit);
	if (format == GL_RGBA) {
		n_tex_rgba++;
		if (fail_teximage) {
			gl_error = GL_OUT_OF_MEMORY;
			return;
		}
	}
	t->w = width;
	t->h = height;
	t->format = format;
	free(t->data);
	t->data = NULL;
	if (pixels) {
		size_t n = (size_t)width * height * bytes_per_pixel(format);

		t->data = malloc(n);
		memcpy(t->data, pixels, n);
		n_tex_images++;
	} else {
		/* filled by glTexSubImage2D: an atlas, or a picture with padded rows */
		t->data = calloc((size_t)width * height, bytes_per_pixel(format));
	}
}
void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	struct fake_tex *t = tex_of(bound_tex);

	assert(target == GL_TEXTURE_2D && (param == GL_NEAREST || param == GL_LINEAR));
	if (pname == GL_TEXTURE_MIN_FILTER)
		t->filter = param;
	else if (pname == GL_TEXTURE_MAG_FILTER)
		t->mag = param;
	else
		assert(!"a parameter gltex does not set");
}
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
		     GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	const struct fake_tex *t = tex_of(bound_tex);
	struct upload *u;

	/* pixels of another format than the storage's: GL_INVALID_OPERATION */
	assert(t->live && t->w && level == 0 && format == t->format && type == GL_UNSIGNED_BYTE);
	assert(xoffset >= 0 && yoffset >= 0 && width > 0 && height > 0);
	assert(xoffset + width <= t->w && yoffset + height <= t->h);
	if (t->data) {
		unsigned int bpp = bytes_per_pixel(format);

		for (GLsizei y = 0; y < height; y++)
			memcpy(&t->data[((size_t)(yoffset + y) * t->w + xoffset) * bpp],
			       (const uint8_t *)pixels + (size_t)y * width * bpp,
			       (size_t)width * bpp);
	}
	if (fail_uploads) {
		fail_uploads--;
		gl_error = GL_INVALID_OPERATION;
		return;
	}
	if (width == STRIP_W) {
		strip_uploads++;
		if (fail_strip) {
			gl_error = GL_INVALID_OPERATION; /* as under #17's driver */
			return;
		}
	}
	if (!keep_uploads)
		return;
	assert(yoffset == 0);
	assert(n_uploads < sizeof(uploads) / sizeof(*uploads));
	u = &uploads[n_uploads++];
	*u = (struct upload){bound_tex, xoffset, width,
			     height,	format,	 malloc(width * height * bytes_per_pixel(format))};
	memcpy(u->data, pixels, width * height * bytes_per_pixel(format));
}
void glUniform1f(GLint location, GLfloat v0)
{
	assert(location >= 0 && location < (GLint)N_UNIFORMS);
	uniform_f[location] = v0;
}
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
{
	assert(location == U_CLIP);
	uniform_clip[0] = v0;
	uniform_clip[1] = v1;
	uniform_clip[2] = v2;
	uniform_clip[3] = v3;
}
void glUniform1i(GLint location, GLint v0)
{
	assert(location >= 0 && location < (GLint)N_UNIFORMS);
	uniform_i[location] = v0;
}
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
			   GLsizei stride, const void *pointer)
{
}
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {}

/* ---- shl gl ---- */
void gl_clear_error() {}
const char *gl_err_to_str(GLenum err)
{
	return "stub";
}
bool gl_has_error(struct gl_shader *shader)
{
	return false;
}
static unsigned int n_shaders;
static char cell_frag[16384]; /* the cell shader's fragment source, as it was last given */
int gl_shader_new(struct gl_shader **out, const char *vert, int vert_len, const char *frag,
		  int frag_len, char **attr, size_t attr_count)
{
	if (!strcmp(attr[attr_count - 1], "decoration")) {
		assert(frag_len >= 0 && (size_t)frag_len < sizeof(cell_frag));
		memcpy(cell_frag, frag, frag_len);
		cell_frag[frag_len] = 0;
	}
	*out = (struct gl_shader *)(uintptr_t)++n_shaders;
	return 0;
}
void gl_shader_unref(struct gl_shader *shader) {}
GLuint gl_shader_get_uniform(struct gl_shader *shader, const char *name)
{
	for (unsigned int i = 0; i < N_UNIFORMS; i++)
		if (!strcmp(name, uniform_names[i]))
			return i;
	assert(!"a uniform the shader does not have");
	return 0;
}
void gl_shader_use(struct gl_shader *shader)
{
	program = shader;
}
/*
 * as shl's: bound, and filtered GL_LINEAR; named in order, and once the names
 * run out, by a name GL has deleted, as a driver reuses them
 */
void gl_tex_new(GLuint *tex, size_t num)
{
	for (size_t i = 0; i < num; i++) {
		tex[i] = ++n_textures;
		if (tex[i] >= sizeof(texs) / sizeof(*texs))
			for (tex[i] = 1; texs[tex[i]].live; tex[i]++)
				assert(tex[i] + 1 < sizeof(texs) / sizeof(*texs));
		texs[tex[i]] = (struct fake_tex){
			.name = tex[i], .live = true, .filter = GL_LINEAR, .mag = GL_LINEAR};
		bound_tex = tex[i];
	}
}
void gl_tex_free(GLuint *tex, size_t num)
{
	for (size_t i = 0; i < num; i++) {
		assert(texs[tex[i]].live);
		texs[tex[i]].live = false;
		free(texs[tex[i]].data);
		texs[tex[i]].data = NULL;
	}
}

/* ---- display ---- */
unsigned int display_get_width(struct display *disp)
{
	return disp_w;
}
unsigned int display_get_height(struct display *disp)
{
	return disp_h;
}
bool display_has_opengl(struct display *disp)
{
	return true;
}
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y) {}
/* while set, display_use() fails, as with no GL context; display_uses counts its calls */
static bool fail_display_use;
static unsigned int display_uses;
int display_use(struct display *disp)
{
	display_uses++;
	return fail_display_use ? -EFAULT : 0;
}

/* ---- font and text ---- */
unsigned int kmscon_text_get_cols(struct kmscon_text *txt, unsigned int font_width)
{
	return disp_w / font_width;
}
unsigned int kmscon_text_get_rows(struct kmscon_text *txt, unsigned int font_height)
{
	return disp_h / font_height;
}
/*
 * The colour glyph's pixel at @x, @y, ARGB: its alpha varies,
 * and its red, green and blue differ so that a swap of any two shows.
 */
#define COLOUR_CP 0x1f600
/* a wide codepoint the font cannot draw */
#define FAIL_CP 0x4e00
#define COLOUR_PX(x, y) ((uint32_t)((x) * 16 + (y)) << 24 | 0x804000)

/*
 * While set, the font draws each glyph as a pattern of its codepoint, style
 * and pixel (patterned_px()), so a cell drawn from another glyph's slot, or
 * from what a slot's last glyph left, shows in the texels; and codepoints from
 * COLOUR_FIRST up to the next plane are colour glyphs, as clusters are while
 * colour_clusters is set too.
 */
static bool patterned;
static bool colour_clusters;
#define COLOUR_FIRST 0x1f300
/* a glyph the font draws three pixels narrower and two shorter than its cells */
#define SHORT_CP 0xe000

static uint32_t patterned_px(uint32_t cp, const struct kmscon_font_attr *attr, unsigned int x,
			     unsigned int y)
{
	uint32_t v = cp * 2654435761u ^ (attr->bold << 7 | attr->italic << 6);

	return (v + x * 0x9e3779b9u + y * 0x85ebca6bu) | 0x01010101u;
}

/*
 * A blank mask, or for COLOUR_CP an ARGB glyph, as the font gives a colour
 * emoji (ADR-102 step 4c); patterned, see above. A glyph is its font's cells,
 * @cwidth of them.
 */
struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, struct kmscon_font_attr *attr,
					const uint32_t *cps, size_t len, unsigned int cwidth)
{
	unsigned int w = cwidth * font->width, h = font->height, x, y;
	bool colour =
		cps[0] == COLOUR_CP ||
		(patterned && ((cps[0] >= COLOUR_FIRST && cps[0] < 0x20000) || cps[0] >= 0x40000));
	struct kmscon_glyph *g;

	n_rendered++;
	if (len == 1 && cps[0] == FAIL_CP)
		return NULL;
	if (cps[0] == SHORT_CP) {
		w -= 3;
		h -= 2;
	}
	if (colour) {
		uint32_t *px;

		g = calloc(1, sizeof(*g) + w * h * 4);
		g->buf.format = VIDEO_FORMAT_ARGB32;
		px = (uint32_t *)g->buf.data;
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				px[y * w + x] = patterned ? patterned_px(cps[0], attr, x, y)
							  : COLOUR_PX(x, y);
	} else {
		g = calloc(1, sizeof(*g) + w * h);
		for (y = 0; patterned && y < h; y++)
			for (x = 0; x < w; x++)
				g->buf.data[y * w + x] = patterned_px(cps[0], attr, x, y);
	}
	g->buf.width = w;
	g->buf.height = h;
	g->double_width = cwidth == 2;
	return g;
}
bool kmscon_font_has_glyph(struct kmscon_font *font, struct kmscon_font_attr *attr,
			   const uint32_t ch)
{
	return true;
}
/* an 8x16 cell's underline at row 13 and strike-through at row 7, a pixel thick */
static const struct kmscon_font_deco fake_deco = {
	.ul_pos = 13, .ul_thick = 1, .strike_pos = 7, .strike_thick = 1};

void kmscon_font_get_deco(struct kmscon_font *font, struct kmscon_font_deco *out)
{
	*out = fake_deco;
}
/*
 * A codepoint names itself; every cluster is 'c', or, patterned, a codepoint
 * of its own past the BMP, a colour one while colour_clusters is set.
 */
const uint32_t *kmscon_text_get_cluster(struct kmscon_text *txt, uint64_t key, uint32_t *one,
					size_t *len)
{
	*one = key < TSM_FRAME_CLUSTER_KEY ? key : 'c';
	if (key >= TSM_FRAME_CLUSTER_KEY && patterned)
		*one = (colour_clusters ? 0x40000 : 0x30000) + (uint32_t)(key & 0xffff);
	*len = 1;
	return one;
}
/* text.c's rules for the decoration colours (ADR-102), as bbulk reads them */
struct tsm_screen_color kmscon_text_ul_color(struct kmscon_text *txt,
					     const struct tsm_frame_cell *cell)
{
	if (!txt->deco.force && (cell->flags & TSM_FRAME_UL_SET))
		return cell->ul;
	return txt->deco.ul_fg ? cell->fg : txt->deco.ul;
}
struct tsm_screen_color kmscon_text_strike_color(struct kmscon_text *txt,
						 const struct tsm_frame_cell *cell)
{
	return txt->deco.strike_fg ? cell->fg : txt->deco.strike;
}
/*
 * the images test placements show, by key, and the overlay's and those of the
 * screen being drawn as text.c has them
 */
static struct tsm_frame_image test_images[64];
int kmscon_text_get_image(struct kmscon_text *txt, bool overlay, uint64_t key,
			  struct tsm_frame_image *out)
{
	if (overlay) {
		if (!txt->overlay || txt->overlay->placement.image_key != key)
			return -ENOENT;
		*out = txt->overlay->image;
		return 0;
	}
	if (txt->screen)
		return tsm_screen_frame_image(txt->screen, key, out);
	if (key >= 64 || !test_images[key].pixels)
		return -ENOENT;
	*out = test_images[key];
	return 0;
}

#include "../src/render/gltex.c"

static void vte_write(struct tsm_vte *vte, const char *u8, size_t len, void *data) {}

static struct kmscon_font fake_font = {.width = FAKE_CELL_W, .height = FAKE_CELL_H};

/* bytes in use on the heap, the blocks malloc maps on its own included */
static size_t heap_in_use(void)
{
#if HAVE_MALLINFO2
	struct mallinfo2 mi = mallinfo2();

	return mi.uordblks + mi.hblkhd;
#else
	return 0;
#endif
}

/*
 * gltex_render() drew a frame without placements: every queued quad once, from
 * its own atlas, whole and opaque: each atlas with quads in one draw over its
 * slots of the shared vertex arrays, which hold its quads in the order they
 * were queued. Each draw has its atlas on unit 0, sampled untinted when it is
 * an RGBA atlas, and the decoration sprites' atlas on unit 1. The pointer, if
 * any, is the last draw, from the slot after the cells' (#181).
 */
static void check_render(struct gltex *gt)
{
	struct atlas *atlas;
	unsigned int i, n = 0, total = 0;

	assert(!gt->pics_num);
	for (i = 0; i < n_draws; i++)
		assert(draws[i].program == gt->shader && draws[i].layer == LAYER_CELL &&
		       !draws[i].blend);
	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		if (!atlas->cache_num)
			continue;
		assert(n < n_draws);
		assert(draws[n].tex == atlas->tex);
		assert(draws[n].first == (GLint)(6 * atlas->cache_off));
		assert(draws[n].count == (GLsizei)(6 * atlas->cache_num));
		assert(draws[n].colour == (atlas->format == GL_RGBA));
		assert(draws[n].advance_h == atlas->advance_htex);
		assert(draws[n].advance_v == atlas->advance_vtex);
		assert(draws[n].atlas_unit == 0 && draws[n].deco_unit == 1);
		/* unit 1 holds a texture with storage, sprites or not */
		assert(texs[draws[n].deco_tex].live && texs[draws[n].deco_tex].w);
		if (gt->deco_atlas) {
			assert(draws[n].deco_tex == gt->deco_atlas->tex);
			assert(draws[n].deco_h == gt->deco_atlas->advance_htex);
			assert(draws[n].deco_v == gt->deco_atlas->advance_vtex);
			assert(draws[n].decorated == 1.0);
		} else {
			assert(draws[n].decorated == 0.0);
		}
		total += atlas->cache_num;
		atlas->cache_num = 0; /* counted again below */
		n++;
	}
	assert(n + gt->has_pointer == n_draws);
	assert(total == gt->quads_num);
	if (gt->has_pointer) {
		assert(draws[n].tex == gt->pointer.atlas->tex);
		assert(draws[n].first == (GLint)(6 * gt->quads_size) && draws[n].count == 6);
		assert(gt->cache_pos[gt->quads_size * 12] == gt->pointer.x1);
	}

	for (i = 0; i < gt->quads_num; i++) {
		const struct gltex_quad *q = &gt->quads[i];
		unsigned int slot = q->atlas->cache_off + q->atlas->cache_num++;

		assert(gt->cache_pos[slot * 12] == q->x1 && gt->cache_pos[slot * 12 + 3] == q->y2);
		assert(gt->cache_texpos[slot * 12] == q->texoff);
		assert(gt->cache_texpos[slot * 12 + 4] == q->texoff + q->texw);
		assert(gt->cache_fgcol[slot * 18] == (GLfloat)(q->fg.r / 255.0));
		assert(gt->cache_bgcol[slot * 24 + 22] == (GLfloat)(q->bg.b / 255.0));
		assert(gt->cache_ulcol[slot * 18 + 1] == (GLfloat)(q->ul.g / 255.0));
		assert(gt->cache_strikecol[slot * 18 + 15] == (GLfloat)(q->strike.r / 255.0));
		/* each vertex's x within the quad's cells: 0 on the left, texw on the right */
		assert(gt->cache_deco[slot * 18] == 0.0 && gt->cache_deco[slot * 18 + 3] == 0.0);
		assert(gt->cache_deco[slot * 18 + 6] == q->texw);
		assert(gt->cache_deco[slot * 18 + 15] == q->texw);
		assert(gt->cache_deco[slot * 18 + 1] == q->ulcell);
		assert(gt->cache_deco[slot * 18 + 17] == q->strikecell);
	}
}

/*
 * #43: at a 4K cell grid, 3840x2160 in 8x16 cells (480x135), what gltex holds
 * stays linear in the cell count: at most 1 KiB per cell, its vertex arrays,
 * glyph cache and atlases together. Each frame draws every cell with a glyph
 * of its own, so a frame fills about 254 atlases of 256 cells, and six frames
 * pass the flush bound. Vertex arrays sized for a full screen in every atlas
 * came to 15.6 MB an atlas, about 4 GB a frame here.
 */
static void test_vertex_memory(void)
{
	struct kmscon_text txt = {0};
	struct tsm_frame_cell cell = {.fg = {200, 200, 200}, .bg = {0, 0, 40}, .width = 1};
	uint64_t next = TSM_FRAME_CLUSTER_KEY;
	struct tsm_screen_attr attr = {0};
	size_t base, used, peak = 0, cells;
	unsigned int f, x, y;

	disp_w = 3840;
	disp_h = 2160;
	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	base = heap_in_use();
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	assert(txt.max_cols == 480 && txt.max_rows == 135);
	cells = txt.max_cols * txt.max_rows;

	for (f = 0; f < 6; f++) {
		assert(gltex_prepare(&txt, &attr) == 0);
		for (y = 0; y < txt.rows; y++)
			for (x = 0; x < txt.cols; x++)
				assert(gltex_draw_cell(&txt, &cell, next++, x, y, 1, false) == 0);
		assert(gltex_draw_pointer(&txt, 100, 100) == 0);
		n_draws = 0;
		assert(gltex_render(&txt) == 0);
		check_render(txt.data);
		used = heap_in_use() - base;
		if (used > peak)
			peak = used;
	}

	if (!HAVE_MALLINFO2) {
		fprintf(stderr, "4K grid: no mallinfo2(), so the memory bound is not checked\n");
	} else {
		fprintf(stderr, "4K grid: %zu cells, at most %zu bytes held, %zu per cell\n", cells,
			peak, peak / cells);
		assert(peak <= cells * 1024);
	}

	gltex_unset(&txt);
	gltex_destroy(&txt);
	disp_w = 640;
	disp_h = 480;
}

/*
 * The glyph drawn at cell @col, @row of the last frame: its atlas and texture
 * offset, from the quads gltex queued. 0 if no quad starts there.
 */
static int drawn_at(struct gltex *gt, unsigned int col, unsigned int row, struct atlas **out,
		    float *texoff)
{
	float x1 = gt->off_x + gt->advance_x * col - 1.0;
	float y1 = 1.0 - gt->off_y - gt->advance_y * row;
	int found = 0;

	for (unsigned int i = 0; i < gt->quads_num; i++) {
		if (gt->quads[i].x1 != x1 || gt->quads[i].y1 != y1)
			continue;
		*out = gt->quads[i].atlas;
		*texoff = gt->quads[i].texoff;
		found++;
	}
	return found;
}

/* cell @col of row 0 was drawn with the glyph for @key and the cell's attributes */
static void assert_drawn(struct kmscon_text *txt, const struct tsm_frame *frame, unsigned int col,
			 uint64_t key)
{
	const struct tsm_frame_cell *fc = tsm_frame_cell_at(frame, col, 0);
	struct gl_glyph *want;
	struct atlas *atlas;
	float texoff;

	want = find_glyph(txt, key, fc->flags, fc->width == 2 ? 2 : 1);
	assert(want);
	assert(drawn_at(txt->data, col, 0, &atlas, &texoff) == 1);
	assert(atlas == want->atlas && texoff == want->texoff);
}

/*
 * #17: the first glyph of a fresh gltex goes into a new atlas, with a texture
 * gltex generated and gave storage. gltex before 9e12309 took the first entry
 * of its atlas list without checking the list was empty; on an empty list that
 * is the list head inside struct gltex, read as an atlas, so the glyph went to
 * a texture name made of a float's bits whenever the bytes after the head
 * passed for room. Under virgl every upload then failed with
 * GL_INVALID_OPERATION and the screen stayed black. The test checks the
 * outcome. The display leaves a margin so off_x and off_y are not zero: with
 * c5b2fa5's fields after the list head, that is what let its lookup pass the
 * room check, and that plant fails here (#180). Today an atlas is only ever
 * made for the glyph cache's pools or the sprites, never looked up.
 */
static void test_first_glyph(void)
{
	struct kmscon_text txt = {0};
	struct gl_glyph *g;
	struct gltex *gt;
	unsigned int before = n_textures;

	disp_w = 645;
	disp_h = 490;
	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;
	assert(dlist_empty(&gt->atlases));
	assert(gt->off_x != 0 && gt->off_y != 0);

	g = find_glyph(&txt, 'A', 0, 1);
	assert(g);
	assert(n_textures == before + 1);
	assert(!dlist_empty(&gt->atlases));
	assert(g->atlas == dlist_first(&gt->atlases, struct atlas, list));
	assert(g->atlas->tex == n_textures && texs[g->atlas->tex].w);
	assert(g->texoff == 0 && g->held && g->atlas->pool == pool_of(gt, GL_ALPHA, 1));

	gltex_unset(&txt);
	gltex_destroy(&txt);
	disp_w = 640;
	disp_h = 480;
}

/*
 * Blinking text (SGR 5): in the hidden phase a blinking cell is drawn with no
 * glyph and keeps its colours, underline and strike; steady cells, and a cell
 * whose blink SGR 25 ended, keep their glyphs. gltex clears and draws every
 * cell each frame, so no buffer can keep a phase, unlike bbulk (test_blink.c).
 */
static void test_blink(void)
{
	static const char bytes[] = "\x1b[?25l\x1b[5;4;9mA\x1b[0mB\x1b[5;7m\xe6\x97\xa5\x1b[0m"
				    "\x1b[5;25mC";
	struct kmscon_text txt = {0};
	struct tsm_frame_age age = {0};
	struct tsm_screen_attr attr = {0};
	const struct tsm_frame *frame;
	struct tsm_screen *con;
	struct tsm_vte *vte;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, FAKE_COLS, FAKE_ROWS) == 0);
	tsm_vte_input(vte, bytes, sizeof(bytes) - 1);

	/* visible, hidden, visible, hidden */
	for (int i = 0; i < 4; i++) {
		bool hidden = i & 1;

		txt.blinking = hidden;
		frame = tsm_screen_frame_since(con, &age);
		assert(frame);
		assert(tsm_frame_cell_at(frame, 0, 0)->flags & TSM_FRAME_BLINK);
		assert(!(tsm_frame_cell_at(frame, 4, 0)->flags & TSM_FRAME_BLINK));
		assert(gltex_prepare(&txt, &attr) == 0);
		assert(gltex_draw(&txt, frame, false) == 0);

		assert_drawn(&txt, frame, 0, hidden ? 0 : 'A');
		assert_drawn(&txt, frame, 1, 'B');
		assert_drawn(&txt, frame, 2, hidden ? 0 : 0x65e5);
		assert(drawn_at(txt.data, 3, 0, &(struct atlas *){0}, &(float){0}) == 0);
		assert_drawn(&txt, frame, 4, 'C');
	}

	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	gltex_unset(&txt);
	gltex_destroy(&txt);
}

/* the quad queued for cell @col of row 0 */
static const struct gltex_quad *quad_at(struct gltex *gt, unsigned int col)
{
	float x1 = gt->off_x + gt->advance_x * col - 1.0;
	float y1 = 1.0 - gt->off_y;
	unsigned int i;

	for (i = 0; i < gt->quads_num; i++)
		if (gt->quads[i].x1 == x1 && gt->quads[i].y1 == y1)
			return &gt->quads[i];
	assert(!"no quad at the cell");
	return NULL;
}

/*
 * The mask gltex uploaded for the glyph drawn at cell @col of row 0, and the
 * quad that draws it.
 */
static const struct upload *mask_at(struct gltex *gt, unsigned int col,
				    const struct gltex_quad **quad)
{
	*quad = quad_at(gt, col);
	for (unsigned int j = 0; j < n_uploads; j++)
		if (uploads[j].tex == (*quad)->atlas->tex &&
		    uploads[j].x == (GLint)((*quad)->texoff * FAKE_CELL_W))
			return &uploads[j];
	assert(!"no mask uploaded for the cell");
	return NULL;
}

static bool colour_eq(struct tsm_screen_color a, struct tsm_screen_color b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

/*
 * The decoration sprites gltex uploaded: one strip at its decoration cells,
 * the blank sprite then deco.c's sprite of each kind, a cell each.
 */
static void assert_sprites(struct gltex *gt)
{
	const struct upload *strip = NULL;
	unsigned int kind, x, y, inked;

	assert(gt->deco_atlas && gt->deco_atlas->format == GL_ALPHA);
	for (unsigned int j = 0; j < n_uploads; j++)
		if (uploads[j].tex == gt->deco_atlas->tex && uploads[j].x == 0)
			strip = &uploads[j];
	assert(strip && strip->format == GL_ALPHA);
	assert(strip->w == KMSCON_DECO_COUNT * FAKE_CELL_W && strip->h == FAKE_CELL_H);

	for (kind = KMSCON_DECO_NONE; kind < KMSCON_DECO_COUNT; kind++) {
		struct kmscon_glyph *sp =
			kmscon_deco_render(kind, FAKE_CELL_W, FAKE_CELL_H, &fake_deco);

		assert(!sp == (kind == KMSCON_DECO_NONE));
		inked = 0;
		for (y = 0; y < FAKE_CELL_H; y++)
			for (x = 0; x < FAKE_CELL_W; x++) {
				uint8_t want = sp ? sp->buf.data[y * FAKE_CELL_W + x] : 0;

				assert(strip->data[y * strip->w + kind * FAKE_CELL_W + x] == want);
				inked += want != 0;
			}
		/* a sprite that drew nothing would pass the loop above */
		assert(kind == KMSCON_DECO_NONE || inked >= FAKE_CELL_W / 2);
		free(sp);
	}
}

/* parse @bytes into a fresh screen and draw its frame, the cursor on or not */
static void draw_bytes(struct kmscon_text *txt, const char *bytes, size_t len, bool cursor_on)
{
	struct tsm_frame_age age = {0};
	struct tsm_screen_attr attr = {0};
	const struct tsm_frame *frame;
	struct tsm_screen *con;
	struct tsm_vte *vte;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, FAKE_COLS, FAKE_ROWS) == 0);
	tsm_vte_input(vte, bytes, len);
	frame = tsm_screen_frame_since(con, &age);
	assert(frame);
	assert(gltex_prepare(txt, &attr) == 0);
	assert(gltex_draw(txt, frame, cursor_on) == 0);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

static void free_uploads(void)
{
	while (n_uploads)
		free(uploads[--n_uploads].data);
}

/*
 * #116, flipped by ADR-102 step 6: gltex draws decorations as bbulk does,
 * deco.c's sprites in their own colours, and the glyph masks carry none. Each
 * decorated cell's quad names its sprites: every underline style, strike, both
 * at once, SGR 58's coloured underline and a wide underlined cell. The
 * underline takes underline-color, or SGR 58's colour unless
 * force-decoration-colors is on; strike takes strike-color; `fg` follows the
 * foreground.
 */
static void test_decorations(void)
{
	static const char bytes[] = "\x1b[?25l\x1b[38;2;10;20;30m"
				    "\x1b[4:1mA\x1b[4:2mB\x1b[4:3mC\x1b[4:4mD\x1b[4:5mE"
				    "\x1b[4:0;9mF\x1b[4:1;9mG\x1b[29;4:1;58:2::255:0:0mH"
				    "\x1b[59m\xe6\x97\xa5\x1b[0mI";
	/* each cell's underline and strike, one column a cell, the wide one two */
	static const struct {
		enum kmscon_deco ul;
		bool strike;
	} want[] = {
		{KMSCON_DECO_SINGLE, false}, {KMSCON_DECO_DOUBLE, false},
		{KMSCON_DECO_CURLY, false},  {KMSCON_DECO_DOTTED, false},
		{KMSCON_DECO_DASHED, false}, {KMSCON_DECO_NONE, true},
		{KMSCON_DECO_SINGLE, true},  {KMSCON_DECO_SINGLE, false},
		{KMSCON_DECO_SINGLE, false}, {KMSCON_DECO_NONE, false},
	};
	static const struct tsm_screen_color fg = {10, 20, 30}, red = {255, 0, 0};
	static const struct kmscon_text_deco decos[] = {
		{.ul_fg = true, .strike_fg = true},
		{.ul = {0, 255, 0}, .strike = {0, 0, 255}},
		{.ul = {0, 255, 0}, .strike = {0, 0, 255}, .force = true},
	};
	const struct gltex_quad *q;
	const struct upload *mask;
	struct kmscon_text txt = {0};
	struct tsm_screen_color ul;
	struct gltex *gt;
	unsigned int d, i, col, strike;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;

	for (d = 0; d < sizeof(decos) / sizeof(*decos); d++) {
		txt.deco = decos[d];
		/* the glyphs are new on the first pass only */
		keep_uploads = !d;
		draw_bytes(&txt, bytes, sizeof(bytes) - 1, false);
		keep_uploads = false;

		if (!d) {
			/* the sprites, ten cells' glyphs and the blank of the empty cells */
			assert(n_uploads == 12);
			assert_sprites(gt);
			for (i = 0, col = 0; i < sizeof(want) / sizeof(*want); i++, col++) {
				mask = mask_at(gt, col, &q);
				assert(mask->format == GL_ALPHA);
				assert(mask->w == (i == 8 ? 2 : 1) * FAKE_CELL_W);
				for (unsigned int b = 0; b < (unsigned int)(mask->w * mask->h); b++)
					assert(!mask->data[b]);
				if (i == 8)
					col++; /* the wide cell's trailing half */
			}
		}

		for (i = 0, col = 0; i < sizeof(want) / sizeof(*want); i++, col++) {
			q = quad_at(gt, col);
			/* 'I', after SGR 0, is in the default foreground */
			assert(i == 9 || colour_eq(q->fg, fg));
			assert(q->ulcell == want[i].ul);
			strike = want[i].strike ? KMSCON_DECO_STRIKE : KMSCON_DECO_NONE;
			assert(q->strikecell == strike);
			ul = txt.deco.ul_fg ? fg : txt.deco.ul;
			if (i == 7 && !txt.deco.force)
				ul = red; /* SGR 58 */
			if (want[i].ul)
				assert(colour_eq(q->ul, ul));
			if (want[i].strike)
				assert(colour_eq(q->strike,
						 txt.deco.strike_fg ? fg : txt.deco.strike));
			if (i == 8)
				col++;
		}

		n_draws = 0;
		assert(gltex_render(&txt) == 0);
		check_render(gt);
	}
	free_uploads();

	gltex_unset(&txt);
	gltex_destroy(&txt);
}

/*
 * An underline cursor is no decoration: bbulk draws it in the cell's
 * foreground, whatever underline-color says, and so does gltex.
 */
static void test_underline_cursor(void)
{
	static const char bytes[] = "\x1b[4 q\x1b[38;2;10;20;30mA";
	const struct gltex_quad *q;
	struct kmscon_text txt = {0};
	struct gltex *gt;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	txt.deco = (struct kmscon_text_deco){.ul = {0, 255, 0}, .strike = {0, 0, 255}};
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;

	draw_bytes(&txt, bytes, sizeof(bytes) - 1, true);
	q = quad_at(gt, 1);
	assert(q->ulcell == KMSCON_DECO_SINGLE);
	assert(colour_eq(q->ul, q->fg) && !colour_eq(q->ul, txt.deco.ul));
	/* the cell before it is no cursor and has no underline */
	assert(quad_at(gt, 0)->ulcell == KMSCON_DECO_NONE);

	gltex_unset(&txt);
	gltex_destroy(&txt);
}

/*
 * #68: a colour glyph from the font, premultiplied ARGB, goes into an RGBA
 * atlas as the bytes R, G, B, A, apart from the alpha atlas of the text beside
 * it, and its atlas is drawn untinted: the shader's colour path, where the
 * foreground is not read.
 */
static void test_colour_glyph(void)
{
	static const char bytes[] = "\x1b[?25l\x1b[38;2;40;50;60m\xf0\x9f\x98\x80"
				    "A";
	const struct gltex_quad *q, *qa;
	const struct upload *px;
	struct kmscon_text txt = {0};
	struct gltex *gt;
	unsigned int x, y, i, n_colour = 0;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;

	keep_uploads = true;
	draw_bytes(&txt, bytes, sizeof(bytes) - 1, false);
	keep_uploads = false;

	px = mask_at(gt, 0, &q);
	assert(q->atlas->format == GL_RGBA && texs[q->atlas->tex].format == GL_RGBA);
	assert(px->format == GL_RGBA);
	assert(px->w == 2 * FAKE_CELL_W && px->h == FAKE_CELL_H);
	for (y = 0; y < FAKE_CELL_H; y++)
		for (x = 0; x < 2 * FAKE_CELL_W; x++) {
			const uint8_t *b = &px->data[(y * px->w + x) * 4];
			uint32_t v = COLOUR_PX(x, y);

			assert(b[0] == ((v >> 16) & 0xff) && b[1] == ((v >> 8) & 0xff));
			assert(b[2] == (v & 0xff) && b[3] == v >> 24);
		}
	assert(q->texw == 2.0);

	/* the text beside it stays in an alpha atlas */
	qa = quad_at(gt, 2);
	assert(qa->atlas != q->atlas && qa->atlas->format == GL_ALPHA);

	n_draws = 0;
	assert(gltex_render(&txt) == 0);
	check_render(gt);
	for (i = 0; i < n_draws; i++) {
		if (draws[i].tex == q->atlas->tex)
			n_colour += draws[i].colour == 1;
		if (draws[i].tex == qa->atlas->tex)
			assert(draws[i].colour == 0);
	}
	assert(n_colour == 1);
	free_uploads();

	gltex_unset(&txt);
	gltex_destroy(&txt);
}

/*
 * A strip upload that fails, as uploads failed under #17's driver, costs the
 * frame its decorations and nothing else: every cell is still drawn with its
 * glyph and background, the strip is tried once that frame, not once a cell,
 * and the shader is told to draw no sprites. The next frame tries again.
 */
static void test_strip_failure(void)
{
	static const char bytes[] = "\x1b[?25l\x1b[48;2;1;2;3;4;9mA";
	const struct gltex_quad *q;
	struct kmscon_text txt = {0};
	struct gltex *gt;
	struct gl_glyph *a;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;

	fail_strip = true;
	strip_uploads = 0;
	draw_bytes(&txt, bytes, sizeof(bytes) - 1, false);
	assert(strip_uploads == 1);
	assert(!gt->deco_atlas);
	assert(gt->quads_num == FAKE_COLS * FAKE_ROWS);
	q = quad_at(gt, 0);
	a = find_glyph(&txt, 'A', 0, 1);
	assert(q->atlas == a->atlas && q->texoff == a->texoff);
	assert(colour_eq(q->bg, (struct tsm_screen_color){1, 2, 3}));
	n_draws = 0;
	assert(gltex_render(&txt) == 0);
	assert(n_draws);
	check_render(gt);

	fail_strip = false;
	draw_bytes(&txt, bytes, sizeof(bytes) - 1, false);
	assert(strip_uploads == 2 && gt->deco_atlas);
	q = quad_at(gt, 0);
	assert(q->ulcell == KMSCON_DECO_SINGLE);
	assert(q->strikecell == KMSCON_DECO_STRIKE);
	n_draws = 0;
	assert(gltex_render(&txt) == 0);
	check_render(gt);

	gltex_unset(&txt);
	gltex_destroy(&txt);
}

/*
 * A glyph that cannot be had leaves its cells their background and
 * decorations, as bbulk does: a blank in each cell of a wide glyph. The font
 * fails one, and a texture one cell wide holds no wide glyph (nor the
 * decoration strip, so that frame is undecorated).
 */
static void test_glyph_fallback(void)
{
	static const char fails[] = "\x1b[?25l\x1b[48;2;1;2;3;4m\xe4\xb8\x80"
				    "A";
	static const char wide[] = "\x1b[?25l\x1b[48;2;1;2;3m\xe6\x97\xa5"
				   "A";
	const struct gltex_quad *q;
	struct kmscon_text txt = {0};
	struct gl_glyph *blank, *a;
	struct gltex *gt;
	unsigned int col;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	txt.deco = (struct kmscon_text_deco){.ul = {0, 255, 0}, .strike = {0, 0, 255}};
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;

	draw_bytes(&txt, fails, sizeof(fails) - 1, false);
	blank = find_glyph(&txt, ' ', 0, 1);
	for (col = 0; col < 2; col++) {
		q = quad_at(gt, col);
		assert(q->atlas == blank->atlas && q->texoff == blank->texoff && q->texw == 1.0);
		assert(colour_eq(q->bg, (struct tsm_screen_color){1, 2, 3}));
		assert(q->ulcell == KMSCON_DECO_SINGLE);
		assert(colour_eq(q->ul, txt.deco.ul));
	}
	a = find_glyph(&txt, 'A', 0, 1);
	assert(quad_at(gt, 2)->atlas == a->atlas && quad_at(gt, 2)->texoff == a->texoff);
	n_draws = 0;
	assert(gltex_render(&txt) == 0);
	check_render(gt);
	gltex_unset(&txt);
	gltex_destroy(&txt);

	max_texture_size = FAKE_CELL_W;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;
	draw_bytes(&txt, wide, sizeof(wide) - 1, false);
	assert(!gt->deco_atlas);
	blank = find_glyph(&txt, ' ', 0, 1);
	for (col = 0; col < 2; col++) {
		q = quad_at(gt, col);
		assert(q->atlas == blank->atlas && q->texoff == blank->texoff && q->texw == 1.0);
		assert(colour_eq(q->bg, (struct tsm_screen_color){1, 2, 3}));
	}
	a = find_glyph(&txt, 'A', 0, 1);
	assert(quad_at(gt, 2)->atlas == a->atlas && quad_at(gt, 2)->texoff == a->texoff);
	n_draws = 0;
	assert(gltex_render(&txt) == 0);
	check_render(gt);
	gltex_unset(&txt);
	gltex_destroy(&txt);
	max_texture_size = 2048;
}

/* ---- the image layer (ADR-103 steps 3f and 4e) ---- */

static struct tsm_frame_cell frame_cells[FAKE_COLS * FAKE_ROWS];
static struct tsm_frame_placement placements[16];
static struct tsm_frame image_frame;

/* a frame of blank cells on the default background, with the first @n placements */
static const struct tsm_frame *make_frame(unsigned int n)
{
	unsigned int i;

	for (i = 0; i < FAKE_COLS * FAKE_ROWS; i++)
		frame_cells[i] = (struct tsm_frame_cell){.key = ' ', .width = 1};
	memset(&image_frame, 0, sizeof(image_frame));
	image_frame.cols = FAKE_COLS;
	image_frame.rows = FAKE_ROWS;
	image_frame.cell_size = sizeof(*frame_cells);
	image_frame.cells = frame_cells;
	image_frame.placement_size = sizeof(*placements);
	image_frame.placement_count = n;
	image_frame.placements = n ? placements : NULL;
	return &image_frame;
}

/*
 * Test image @key, @w x @h: each pixel's value names its place, with alpha
 * @a, premultiplied, so a swap or a shift of any bytes shows
 */
static void make_image(uint64_t key, unsigned int w, unsigned int h, uint8_t a)
{
	uint32_t *px = calloc((size_t)w * h, 4);
	unsigned int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			px[y * w + x] = (uint32_t)a << 24 | (x * 7 + key) % a << 16 |
					(y * 5 + key) % a << 8 | (x + y) % a;
	free((void *)test_images[key].pixels);
	test_images[key] = (struct tsm_frame_image){
		.width = w, .height = h, .stride = (size_t)w * 4, .pixels = px, .opaque = a == 255};
}

/*
 * Placement @i of image @key, its whole drawn over @cols x @rows cells at @col,
 * @row in @band, at the cell size it was made in
 */
static void place(unsigned int i, uint64_t key, int row, unsigned int col, unsigned int cols,
		  unsigned int rows, uint8_t band)
{
	struct tsm_frame_placement *p = &placements[i];

	memset(p, 0, sizeof(*p));
	p->key = 100 + i;
	p->image_key = key;
	p->row = row;
	p->col = col;
	p->cols = cols;
	p->rows = rows;
	p->width = cols * FAKE_CELL_W;
	p->height = rows * FAKE_CELL_H;
	p->src_width = test_images[key].width;
	p->src_height = test_images[key].height;
	p->cell_width = FAKE_CELL_W;
	p->cell_height = FAKE_CELL_H;
	p->band = band;
	p->flags = TSM_FRAME_CHANGED | (test_images[key].opaque ? TSM_FRAME_OPAQUE : 0);
}

static void open_txt(struct kmscon_text *txt, enum Orientation orientation)
{
	memset(txt, 0, sizeof(*txt));
	txt->font = &fake_font;
	txt->disp = (struct display *)0x1;
	txt->orientation = orientation;
	assert(gltex_init(txt) == 0);
	assert(gltex_set(txt) == 0);
}

static void close_txt(struct kmscon_text *txt)
{
	gltex_unset(txt);
	gltex_destroy(txt);
	/* its textures and its display given back */
	assert(image_budget_used() == 0 && image_budget_textures() == 0);
	assert(image_budget_displays_px() == 0);
}

/* one frame: @frame drawn and rendered, the draws recorded */
static void run_frame(struct kmscon_text *txt, const struct tsm_frame *frame, bool pointer)
{
	struct tsm_screen_attr attr = {0};

	assert(gltex_prepare(txt, &attr) == 0);
	assert(gltex_draw(txt, frame, false) == 0);
	if (pointer)
		assert(gltex_draw_pointer(txt, 100, 100) == 0);
	n_draws = 0;
	assert(gltex_render(txt) == 0);
}

static bool near(double a, double b)
{
	return a - b < 1e-6 && b - a < 1e-6;
}

/* the texture of image @key, which must have one */
static struct gltex_image *texture_of(struct gltex *gt, uint64_t key)
{
	struct gltex_image *gi = image_find(gt, false, key);

	assert(gi && texs[gi->tex].live);
	return gi;
}

/*
 * One texture per image key, uploaded on the first draw of a placement of it
 * as the image's bytes, and kept: two placements of one image share it, a
 * second frame uploads nothing. Each texture counts in the textures' budget,
 * not in the images'. A
 * placement drawn a texel a pixel is sampled as it is; one scaled, bilinear.
 * Each placement is one quad, with its picture's texture coordinates.
 */
static void test_image_texture(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	struct gltex_image *a, *b;
	const struct tsm_frame *frame;
	unsigned int uploads_before, i, n_pic_draws = 0;
	size_t want;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	make_image(1, 2 * FAKE_CELL_W, 2 * FAKE_CELL_H, 255);
	make_image(2, 3 * FAKE_CELL_W, FAKE_CELL_H, 128);
	place(0, 1, 1, 2, 2, 2, TSM_FRAME_BAND_ABOVE_TEXT);
	place(1, 1, 5, 10, 4, 4, TSM_FRAME_BAND_ABOVE_TEXT); /* twice its size */
	place(2, 2, 0, 0, 3, 1, TSM_FRAME_BAND_BELOW_TEXT);
	frame = make_frame(3);

	uploads_before = n_tex_images;
	run_frame(&txt, frame, false);
	assert(gt->images_num == 2 && n_tex_images == uploads_before + 2);
	a = texture_of(gt, 1);
	b = texture_of(gt, 2);
	for (i = 1; i <= 2; i++) {
		const struct gltex_image *gi = i == 1 ? a : b;
		const struct fake_tex *t = &texs[gi->tex];

		assert(t->format == GL_RGBA && t->w == (GLsizei)test_images[i].width &&
		       t->h == (GLsizei)test_images[i].height);
		/* the bytes as they are in memory, for the shader to swizzle */
		assert(!memcmp(t->data, test_images[i].pixels, (size_t)t->w * t->h * 4));
	}
	want = (size_t)16 * 32 * 4 + (size_t)24 * 16 * 4;
	assert(gt->image_bytes == want && image_budget_textures() == want);
	assert(image_budget_used() == 0);

	/* the quads: one each, in the frame's order */
	assert(gt->pics_num == 3);
	assert(gt->pics[0].image == a && gt->pics[1].image == a && gt->pics[2].image == b);
	assert(gt->pics[0].exact && !gt->pics[1].exact && gt->pics[2].exact);
	assert(near(gt->pics[0].x1, gt->off_x + 2 * gt->advance_x - 1.0));
	assert(near(gt->pics[0].x2, gt->off_x + 4 * gt->advance_x - 1.0));
	assert(near(gt->pics[0].y1, 1.0 - gt->off_y - gt->advance_y));
	assert(near(gt->pics[0].y2, 1.0 - gt->off_y - 3 * gt->advance_y));
	assert(near(gt->pics[1].x2, gt->off_x + 14 * gt->advance_x - 1.0));
	for (i = 0; i < 3; i++)
		assert(gt->pics[i].u1 == 0 && gt->pics[i].v1 == 0 && gt->pics[i].u2 == 1 &&
		       gt->pics[i].v2 == 1);
	for (i = 0; i < 3; i++) {
		/* the vertex arrays hold each quad at its slot */
		assert(gt->pic_pos[i * 12] == gt->pics[i].x1 &&
		       gt->pic_pos[i * 12 + 3] == gt->pics[i].y2);
		assert(gt->pic_texpos[i * 12 + 4] == gt->pics[i].u2);
	}
	for (i = 0; i < n_draws; i++) {
		if (draws[i].program != gt->img_shader)
			continue;
		assert(draws[i].count == 6 && draws[i].picture_unit == 0);
		assert(draws[i].first % 6 == 0);
		/* a texel a pixel: as it is; scaled: bilinear */
		assert(draws[i].filter ==
		       (gt->pics[draws[i].first / 6].exact ? GL_NEAREST : GL_LINEAR));
		assert(draws[i].mag == draws[i].filter);
		assert(draws[i].tex == gt->pics[draws[i].first / 6].image->tex);
		n_pic_draws++;
	}
	assert(n_pic_draws == 3);

	/* the next frame uploads nothing */
	run_frame(&txt, frame, false);
	assert(n_tex_images == uploads_before + 2 && gt->images_num == 2);
	assert(texture_of(gt, 1) == a && texture_of(gt, 2) == b);

	close_txt(&txt);
}

/*
 * The picture's part inside its box and the screen: a placement cut at the
 * right edge, one whose top has scrolled above the view, and a source
 * rectangle, each drawn from the texture coordinates of what shows.
 */
static void test_image_clip(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	const struct tsm_frame *frame;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	make_image(3, 4 * FAKE_CELL_W, 2 * FAKE_CELL_H, 255);
	place(0, 3, 0, FAKE_COLS - 1, 4, 2, TSM_FRAME_BAND_ABOVE_TEXT); /* one column shows */
	place(1, 3, -1, 10, 4, 2, TSM_FRAME_BAND_ABOVE_TEXT);		/* its lower row shows */
	place(2, 3, 5, 20, 2, 2, TSM_FRAME_BAND_ABOVE_TEXT);
	placements[2].src_x = 2 * FAKE_CELL_W; /* the right half, at its size */
	placements[2].src_width = 2 * FAKE_CELL_W;
	frame = make_frame(3);
	run_frame(&txt, frame, false);

	assert(gt->pics_num == 3);
	assert(near(gt->pics[0].x2, gt->off_x + FAKE_COLS * gt->advance_x - 1.0));
	assert(near(gt->pics[0].u1, 0) && near(gt->pics[0].u2, 0.25));
	assert(near(gt->pics[0].v1, 0) && near(gt->pics[0].v2, 1));
	assert(near(gt->pics[1].y1, 1.0 - gt->off_y) &&
	       near(gt->pics[1].y2, 1.0 - gt->off_y - gt->advance_y));
	assert(near(gt->pics[1].v1, 0.5) && near(gt->pics[1].v2, 1));
	assert(near(gt->pics[2].u1, 0.5) && near(gt->pics[2].u2, 1));
	assert(gt->pics[2].exact);
	/* one image, one texture */
	assert(gt->images_num == 1);

	close_txt(&txt);
}

/*
 * An image larger than GL_MAX_TEXTURE_SIZE, the GPU's own and not the
 * atlases' 2048, is uploaded scaled to fit, its aspect kept, as picture_scale
 * makes it; its placements draw the whole of it, scaled.
 */
static void test_image_scaled(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	struct gltex_image *gi;
	uint32_t *want;

	max_texture_size = texture_limit = 4096;
	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	assert(gt->max_image_size == 4096 && gt->max_tex_size == 2048);
	close_txt(&txt);

	max_texture_size = texture_limit = 64;
	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	make_image(4, 200, 100, 255);
	place(0, 4, 0, 0, 25, 6, TSM_FRAME_BAND_ABOVE_TEXT);
	run_frame(&txt, make_frame(1), false);

	gi = texture_of(gt, 4);
	assert(gi->w == 64 && gi->h == 32 && gi->scaled);
	assert(gi->iw == 200 && gi->ih == 100);
	assert(texs[gi->tex].w == 64 && texs[gi->tex].h == 32);
	want = malloc(64 * 32 * 4);
	assert(picture_scale(test_images[4].pixels, 200, 200, 100, 64, 32, 0, 0, 64, 32, want) ==
	       0);
	assert(!memcmp(texs[gi->tex].data, want, 64 * 32 * 4));
	free(want);
	assert(gi->bytes == 64 * 32 * 4 && image_budget_textures() == 64 * 32 * 4);
	assert(gt->pics_num == 1 && !gt->pics[0].exact);
	assert(gt->pics[0].u2 == 1 && gt->pics[0].v2 == 1);

	close_txt(&txt);
	max_texture_size = texture_limit = 2048;
}

/* image @key has a texture */
static bool has_texture(struct gltex *gt, uint64_t key)
{
	return image_find(gt, false, key);
}

/*
 * The textures past image_budget_cache_limit (twice the screen in ARGB here)
 * that no frame draws now go, least recently drawn first; those a frame draws
 * stay, however many. The budget's own call for room frees those the last
 * frame did not draw, and their names are deleted once the context is
 * gltex's, at its next frame. A texture that does not fit the budget leaves
 * its placement out, and the display asks to be drawn again once the budget
 * has room.
 */
static void test_image_evict(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	struct gltex_image *gi;
	size_t one = (size_t)320 * 240 * 4, limit;
	unsigned int k;
	GLuint name;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	limit = image_budget_cache_limit((uint64_t)disp_w * disp_h);
	assert(limit == (size_t)640 * 480 * 4 * 2 && limit / one == 8);

	/* a new picture each frame, as a video sends: the oldest go */
	for (k = 10; k < 30; k++) {
		make_image(k, 320, 240, 255);
		place(0, k, 0, 0, 40, 15, TSM_FRAME_BAND_ABOVE_TEXT);
		run_frame(&txt, make_frame(1), false);
		assert(gt->pics_num == 1 && gt->pics[0].image == texture_of(gt, k));
		assert(gt->image_bytes <= limit && image_budget_textures() == gt->image_bytes);
	}
	assert(gt->images_num == 8);
	for (k = 10; k < 30; k++)
		assert(has_texture(gt, k) == (k >= 22));

	/* ten drawn at once all stay, past the bound; a frame drawing none trims them */
	for (k = 0; k < 10; k++)
		place(k, 20 + k, (k % 2) * 15, (k / 2) % 2 * 40, 40, 15, TSM_FRAME_BAND_ABOVE_TEXT);
	run_frame(&txt, make_frame(10), false);
	assert(gt->pics_num == 10 && gt->images_num == 10 && gt->image_bytes == 10 * one);
	for (k = 20; k < 30; k++)
		assert(has_texture(gt, k));
	run_frame(&txt, make_frame(0), false);
	assert(gt->images_num == 8 && gt->image_bytes <= limit);

	/* a smaller limit asks for room: the textures the last frame did not draw go */
	place(0, 29, 0, 0, 40, 15, TSM_FRAME_BAND_ABOVE_TEXT);
	run_frame(&txt, make_frame(1), false);
	gi = texture_of(gt, 28);
	name = gi->tex;
	image_budget_set_limit(one);
	assert(gt->images_num == 1 && has_texture(gt, 29) && !has_texture(gt, 28));
	assert(image_budget_textures() == one);
	assert(texs[name].live); /* not this display's context yet */
	run_frame(&txt, make_frame(1), false);
	assert(!texs[name].live);

	/* one that does not fit is left out, and drawn once the budget grows */
	make_image(40, 320, 240, 255);
	place(0, 40, 0, 0, 40, 15, TSM_FRAME_BAND_ABOVE_TEXT);
	place(1, 29, 15, 0, 40, 15, TSM_FRAME_BAND_ABOVE_TEXT);
	run_frame(&txt, make_frame(2), false);
	assert(!has_texture(gt, 40) && gt->starved);
	assert(gt->pics_num == 1 && gt->pics[0].image == texture_of(gt, 29));
	assert(!gltex_starved(&txt));
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
	assert(gltex_starved(&txt));
	run_frame(&txt, make_frame(2), false);
	assert(has_texture(gt, 40) && gt->pics_num == 2 && !gt->starved);
	assert(!gltex_starved(&txt));

	/* a session in the background gives every texture back */
	gltex_release(&txt);
	assert(!gt->images && !gt->image_bytes && image_budget_textures() == 0);
	assert(!gt->doomed_num);

	close_txt(&txt);
}

/* the draws of @pass: the image shader for a band (-1), or the cells' layer */
static unsigned int next_draws(struct gltex *gt, unsigned int *i, int layer, bool blend)
{
	unsigned int n = 0;

	while (*i < n_draws &&
	       (layer < 0 ? draws[*i].program == gt->img_shader
			  : draws[*i].program == gt->shader && draws[*i].layer == layer &&
				    !(draws[*i].first == (GLint)(6 * gt->quads_size) &&
				      draws[*i].count == 6))) {
		assert(draws[*i].blend == blend);
		if (blend)
			assert(draws[*i].src == GL_ONE && draws[*i].dst == GL_ONE_MINUS_SRC_ALPHA);
		(*i)++;
		n++;
	}
	return n;
}

/*
 * ADR-103's layers: the lowest band, the backgrounds that are not the default
 * (opaque), the middle band, the text blended premultiplied over it, the top
 * band with the overlay last, each band's pictures blended premultiplied; the
 * pointer last of all, whole and opaque (#181). A background is marked for
 * the background pass only when it is not the default.
 */
static void test_band_order(void)
{
	struct kmscon_text txt;
	struct kmscon_text_overlay *ov;
	struct gltex *gt;
	const struct tsm_frame *frame;
	unsigned int i = 0, slot;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	make_image(5, FAKE_CELL_W, FAKE_CELL_H, 255);
	make_image(6, FAKE_CELL_W, FAKE_CELL_H, 200);
	make_image(7, FAKE_CELL_W, FAKE_CELL_H, 100);
	place(0, 5, 2, 2, 1, 1, TSM_FRAME_BAND_BELOW_BG);
	place(1, 6, 2, 2, 1, 1, TSM_FRAME_BAND_BELOW_TEXT);
	place(2, 7, 2, 2, 1, 1, TSM_FRAME_BAND_ABOVE_TEXT);
	frame = make_frame(3);
	frame_cells[0].bg = (struct tsm_screen_color){9, 9, 9};
	frame_cells[1].key = 'A';

	ov = calloc(1, sizeof(*ov) + FAKE_CELL_W * 2 * FAKE_CELL_H * 4);
	ov->image = (struct tsm_frame_image){.width = 2 * FAKE_CELL_W,
					     .height = FAKE_CELL_H,
					     .stride = 2 * FAKE_CELL_W * 4,
					     .pixels = ov->pixels,
					     .opaque = true};
	ov->placement = (struct tsm_frame_placement){.key = 1,
						     .image_key = 1,
						     .cols = 2,
						     .rows = 1,
						     .width = 2 * FAKE_CELL_W,
						     .height = FAKE_CELL_H,
						     .src_width = 2 * FAKE_CELL_W,
						     .src_height = FAKE_CELL_H,
						     .cell_width = FAKE_CELL_W,
						     .cell_height = FAKE_CELL_H,
						     .z = INT32_MAX,
						     .band = TSM_FRAME_BAND_ABOVE_TEXT,
						     .flags = TSM_FRAME_OPAQUE};
	txt.overlay = ov;

	run_frame(&txt, frame, true);
	assert(gt->pics_num == 4);
	assert(gt->pics[3].image->overlay);
	assert(near(gt->pics[3].x1, gt->off_x + (FAKE_COLS - 2) * gt->advance_x - 1.0));

	assert(next_draws(gt, &i, -1, true) == 1); /* the lowest band */
	assert(draws[0].tex == texture_of(gt, 5)->tex);
	assert(next_draws(gt, &i, LAYER_BACKGROUND, false) >= 1);
	assert(next_draws(gt, &i, -1, true) == 1); /* the middle band */
	assert(draws[i - 1].tex == texture_of(gt, 6)->tex);
	assert(next_draws(gt, &i, LAYER_TEXT, true) >= 1);
	assert(next_draws(gt, &i, -1, true) == 2); /* the top band, the overlay last */
	assert(draws[i - 2].tex == texture_of(gt, 7)->tex);
	assert(draws[i - 1].tex == image_find(gt, true, 1)->tex);
	/* the pointer, last */
	assert(i == n_draws - 1);
	assert(draws[i].program == gt->shader && draws[i].layer == LAYER_CELL && !draws[i].blend);
	assert(draws[i].first == (GLint)(6 * gt->quads_size) && draws[i].count == 6);
	assert(draws[i].tex == gt->pointer.atlas->tex);

	/* the background pass draws cell 0's background, not the default ones */
	for (slot = 0; slot < gt->quads_num; slot++) {
		const struct gltex_quad *q = &gt->quads[slot];
		unsigned int s = q->atlas->cache_off, k;

		for (k = 0; k < slot; k++)
			s += gt->quads[k].atlas == q->atlas;
		assert(gt->cache_bgcol[s * 24 + 3] == (q->bg.r == 9 ? 1.0 : 0.0));
		assert(gt->cache_bgcol[s * 24 + 23] == gt->cache_bgcol[s * 24 + 3]);
	}

	/* without placements, one opaque pass, and the pointer last */
	txt.overlay = NULL;
	run_frame(&txt, make_frame(0), true);
	check_render(gt);

	free(ov);
	close_txt(&txt);
}

/*
 * #181: the pointer's glyph in an atlas drawn before a cell's under it is
 * still drawn last, over every cell.
 */
static void test_pointer_last(void)
{
	static const char bytes[] = "\x1b[?25lBCDEFGH";
	struct kmscon_text txt;
	struct gltex *gt;
	struct gl_glyph *pointer, *b;

	max_texture_size = 2 * FAKE_CELL_W; /* two glyphs an atlas, a cell high */
	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	draw_bytes(&txt, bytes, sizeof(bytes) - 1, false);
	assert(gltex_draw_pointer(&txt, 4, 8) == 0);
	pointer = find_glyph(&txt, 'I', 0, 1);
	b = find_glyph(&txt, 'B', 0, 1);
	/* new atlases go first in the list, so the pointer's is drawn before B's */
	assert(pointer->atlas != b->atlas);
	assert(dlist_first(&gt->atlases, struct atlas, list) == pointer->atlas);
	n_draws = 0;
	assert(gltex_render(&txt) == 0);
	check_render(gt);
	assert(draws[n_draws - 1].tex == pointer->atlas->tex);

	close_txt(&txt);
	max_texture_size = 2048;
}

/* a turned display turns its pictures with its cells, in the image shader */
static void test_image_rotated(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int i, n = 0;

	open_txt(&txt, OR_RIGHT);
	gt = txt.data;
	assert(gt->cos == 0 && gt->sin == 1);
	make_image(8, 2 * FAKE_CELL_W, FAKE_CELL_H, 255);
	place(0, 8, 1, 1, 2, 1, TSM_FRAME_BAND_ABOVE_TEXT);
	run_frame(&txt, make_frame(1), false);
	for (i = 0; i < n_draws; i++) {
		assert(draws[i].cos == gt->cos && draws[i].sin == gt->sin);
		n += draws[i].program == gt->img_shader;
	}
	assert(n == 1 && gt->pics[0].exact);
	/* in the cells' coordinates, which the vertex shader turns */
	assert(near(gt->pics[0].x1, gt->off_x + gt->advance_x - 1.0));
	assert(near(gt->pics[0].y2, 1.0 - gt->off_y - 2 * gt->advance_y));

	close_txt(&txt);
}

/* the image shader's draws of the last frame sampled with @filter */
static unsigned int draws_of(struct gltex *gt, GLint filter)
{
	unsigned int i, n = 0;

	for (i = 0; i < n_draws; i++)
		n += draws[i].program == gt->img_shader && draws[i].filter == filter;
	return n;
}

/*
 * An image whose whole texture does not fit the textures' budget beside the
 * textures another display's frame draws (here its own texture of the same
 * image, more than half the budget) is drawn as bbulk draws it (ADR-108):
 * from a texture at the size the placement draws it, the scaler's pixels, a
 * texel a pixel. A placement drawing it larger makes that texture again,
 * larger, and one drawing it smaller uses the larger one.
 */
static void test_half_budget(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	struct gltex_image *gi;
	size_t img = (size_t)2048 * 1536 * 4;
	uint32_t *want;
	GLuint name;

	max_texture_size = texture_limit = 4096;
	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	image_budget_set_limit((size_t)16 << 20);
	image_budget_add_texture(img); /* stands for another display's texture of it */
	make_image(50, 2048, 1536, 255);
	place(0, 50, 0, 0, 40, 15, TSM_FRAME_BAND_ABOVE_TEXT); /* shrunk to 320x240 */
	run_frame(&txt, make_frame(1), false);
	assert(!gt->starved && gt->pics_num == 1);
	gi = texture_of(gt, 50);
	assert(gi->drawn_size && gi->w == 320 && gi->h == 240 && gi->bytes == 320 * 240 * 4);
	assert(gt->pics[0].exact && draws_of(gt, GL_NEAREST) == 1);
	want = malloc(320 * 240 * 4);
	assert(picture_scale(test_images[50].pixels, 2048, 2048, 1536, 320, 240, 0, 0, 320, 240,
			     want) == 0);
	assert(!memcmp(texs[gi->tex].data, want, 320 * 240 * 4));
	free(want);

	/* drawn larger: made again at that size, the smaller one given up */
	name = gi->tex;
	place(0, 50, 0, 0, 80, 30, TSM_FRAME_BAND_ABOVE_TEXT); /* 640x480 */
	run_frame(&txt, make_frame(1), false);
	gi = texture_of(gt, 50);
	assert(gi->drawn_size && gi->w == 640 && gi->h == 480 && gt->images_num == 1);
	assert(gt->image_bytes == 640 * 480 * 4 && !texs[name].live);
	assert(gt->pics_num == 1 && gt->pics[0].exact);

	/* drawn smaller again: the larger one serves */
	name = gi->tex;
	place(0, 50, 0, 0, 40, 15, TSM_FRAME_BAND_ABOVE_TEXT);
	run_frame(&txt, make_frame(1), false);
	assert(texture_of(gt, 50)->tex == name && !gt->pics[0].exact);

	/*
	 * a texel a pixel, but from a source rectangle that starts between the
	 * texels of the drawn-size texture (row 1 of 1536 is a third of a row
	 * of 480), as a placement clipped by a region scroll does: sampled
	 * bilinear, as NEAREST off the texels' centres would shift it; from a
	 * row that is a texel's edge, as it is
	 */
	place(0, 50, 2, 0, 80, 30, TSM_FRAME_BAND_ABOVE_TEXT);
	placements[0].height = 470;
	placements[0].src_y = 1;
	placements[0].src_height = 1504;
	run_frame(&txt, make_frame(1), false);
	gi = texture_of(gt, 50);
	assert(gi->w == 640 && gi->h == 480 && gt->pics_num == 1 && !gt->pics[0].exact);
	placements[0].src_y = 32; /* row 32 of 1536 is row 10 of 480 */
	run_frame(&txt, make_frame(1), false);
	assert(texture_of(gt, 50) == gi && gt->pics_num == 1 && gt->pics[0].exact);

	image_budget_remove_texture(img);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
	close_txt(&txt);
	max_texture_size = texture_limit = 2048;
}

/*
 * An upload GL refuses is remembered while the image is shown: it is not
 * tried again, nor room made for it, on later frames. Once no frame shows
 * it, it is forgotten, and a later placement tries again.
 */
static void test_upload_failure(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	struct gltex_image *gi;
	unsigned int tries;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	make_image(51, 2 * FAKE_CELL_W, FAKE_CELL_H, 255);
	place(0, 51, 0, 0, 2, 1, TSM_FRAME_BAND_ABOVE_TEXT);

	fail_teximage = true;
	tries = n_tex_rgba;
	run_frame(&txt, make_frame(1), false);
	assert(n_tex_rgba == tries + 1 && gt->pics_num == 0 && !gt->starved);
	gi = image_find(gt, false, 51);
	assert(gi && gi->failed && !gi->tex && !gi->bytes && image_budget_textures() == 0);
	run_frame(&txt, make_frame(1), false);
	fail_teximage = false;
	run_frame(&txt, make_frame(1), false);
	assert(n_tex_rgba == tries + 1 && gt->pics_num == 0);

	/* not shown, forgotten; shown again, tried again */
	run_frame(&txt, make_frame(0), false);
	assert(!image_find(gt, false, 51));
	run_frame(&txt, make_frame(1), false);
	assert(n_tex_rgba == tries + 2 && gt->pics_num == 1 && !texture_of(gt, 51)->failed);

	close_txt(&txt);
}

/*
 * A drawn-size texture drawn larger whose larger upload GL refuses keeps
 * drawing from the texture it has, and the larger upload is not tried again,
 * nor room made for it, while the image is shown; once it is not shown, a
 * placement tries again.
 */
static void test_grow_failure(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	struct gltex_image *gi;
	size_t img = (size_t)2048 * 1536 * 4;
	unsigned int tries, f;

	max_texture_size = texture_limit = 4096;
	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	image_budget_set_limit((size_t)16 << 20);
	image_budget_add_texture(img); /* stands for another display's texture of it */
	make_image(54, 2048, 1536, 255);
	place(0, 54, 0, 0, 40, 15, TSM_FRAME_BAND_ABOVE_TEXT); /* 320x240 */
	run_frame(&txt, make_frame(1), false);
	gi = texture_of(gt, 54);
	assert(gi->drawn_size && gi->w == 320);

	fail_teximage = true;
	tries = n_tex_rgba;
	place(0, 54, 0, 0, 80, 30, TSM_FRAME_BAND_ABOVE_TEXT); /* 640x480 */
	for (f = 0; f < 4; f++) {
		run_frame(&txt, make_frame(1), false);
		assert(texture_of(gt, 54) == gi && gt->images_num == 1 && gt->pics_num == 1);
	}
	assert(n_tex_rgba == tries + 1);

	/* not shown, the refusal is forgotten; shown again, it grows */
	fail_teximage = false;
	run_frame(&txt, make_frame(0), false);
	run_frame(&txt, make_frame(1), false);
	gi = texture_of(gt, 54);
	assert(n_tex_rgba == tries + 2 && gi->w == 640 && gi->h == 480 && gt->images_num == 1);

	image_budget_remove_texture(img);
	image_budget_set_limit(IMAGE_BUDGET_AUTO);
	close_txt(&txt);
	max_texture_size = texture_limit = 2048;
}

/* a picture whose rows are padded goes up a row at a time, each row its own pixels */
static void test_padded_rows(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	struct gltex_image *gi;
	unsigned int w = 2 * FAKE_CELL_W, h = FAKE_CELL_H, stride = w + 3, x, y;
	uint32_t *px = calloc((size_t)stride * h, 4);

	for (y = 0; y < h; y++)
		for (x = 0; x < stride; x++)
			px[y * stride + x] = x < w ? 0xff000000 | y << 8 | x : 0xdeadbeef;
	free((void *)test_images[52].pixels);
	test_images[52] = (struct tsm_frame_image){.width = w,
						   .height = h,
						   .stride = (size_t)stride * 4,
						   .pixels = px,
						   .opaque = true};
	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	place(0, 52, 0, 0, 2, 1, TSM_FRAME_BAND_ABOVE_TEXT);
	run_frame(&txt, make_frame(1), false);
	gi = texture_of(gt, 52);
	for (y = 0; y < h; y++)
		assert(!memcmp(&texs[gi->tex].data[(size_t)y * w * 4], &px[y * stride], w * 4));
	close_txt(&txt);
}

/*
 * A placement drawing a source rectangle scaled is sampled half a texel
 * inside it, so bilinear filtering takes nothing from beside it (a sprite
 * sheet's neighbour).
 */
static void test_source_clip(void)
{
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int i;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	make_image(53, 32, 16, 255);
	place(0, 53, 0, 0, 4, 2, TSM_FRAME_BAND_ABOVE_TEXT);
	placements[0].src_x = 8;
	placements[0].src_width = 8; /* texels 8 to 16 of 32, drawn 32 wide */
	run_frame(&txt, make_frame(1), false);
	assert(gt->pics_num == 1 && !gt->pics[0].exact);
	for (i = 0; i < n_draws && draws[i].program != gt->img_shader; i++)
		;
	assert(i < n_draws);
	assert(near(draws[i].clip[0], 8.5 / 32) && near(draws[i].clip[2], 15.5 / 32));
	assert(near(draws[i].clip[1], 0.5 / 16) && near(draws[i].clip[3], 15.5 / 16));
	close_txt(&txt);
}

/* a new render timer display gives the last one's texture back */
static void test_overlay_replaced(void)
{
	struct kmscon_text txt;
	struct kmscon_text_overlay *ov[2];
	struct gltex *gt;
	GLuint name = 0;
	unsigned int k;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	for (k = 0; k < 2; k++) {
		ov[k] = calloc(1, sizeof(*ov[k]) + FAKE_CELL_W * FAKE_CELL_H * 4);
		ov[k]->image = (struct tsm_frame_image){.width = FAKE_CELL_W,
							.height = FAKE_CELL_H,
							.stride = FAKE_CELL_W * 4,
							.pixels = ov[k]->pixels,
							.opaque = true};
		ov[k]->placement = (struct tsm_frame_placement){.key = 1,
								.image_key = k + 1,
								.cols = 1,
								.rows = 1,
								.width = FAKE_CELL_W,
								.height = FAKE_CELL_H,
								.src_width = FAKE_CELL_W,
								.src_height = FAKE_CELL_H,
								.cell_width = FAKE_CELL_W,
								.cell_height = FAKE_CELL_H,
								.band = TSM_FRAME_BAND_ABOVE_TEXT,
								.flags = TSM_FRAME_OPAQUE};
		txt.overlay = ov[k];
		run_frame(&txt, make_frame(0), false);
		assert(gt->images_num == 1 && image_find(gt, true, k + 1));
		assert(!name || !texs[name].live);
		name = image_find(gt, true, k + 1)->tex;
	}
	txt.overlay = NULL;
	free(ov[0]);
	free(ov[1]);
	close_txt(&txt);
}

/*
 * A texture never evicts an image, not even one its own frame draws: a
 * picture placed in scrollback and viewed there (not pinned, so evictable for
 * another image) stays when its texture does not fit the textures' budget,
 * and the placement waits for room.
 */
static void test_frame_draws(void)
{
	static uint32_t px[64 * 32];
	struct tsm_test_image ti = {
		.width = 64, .height = 32, .pixels = px, .kind = TSM_IMAGE_KITTY, .id = 1};
	struct tsm_test_placement pl = {0};
	struct tsm_frame_age age = {0};
	struct tsm_screen_attr attr = {0};
	const struct tsm_frame *frame;
	struct kmscon_text txt;
	struct tsm_screen *con;
	struct gltex *gt;
	uint64_t key, pkey;
	size_t other;
	unsigned int i;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, FAKE_COLS, FAKE_ROWS) == 0);
	tsm_screen_set_max_sb(con, 100);
	assert(tsm_screen_set_cell_size(con, FAKE_CELL_W, FAKE_CELL_H) == 0);
	assert(image_budget_add_screen(con) == 0);
	assert(tsm_screen_test_image(con, &ti, &key) == 0);
	pl.image = key;
	assert(tsm_screen_test_place(con, &pl, &pkey) == 0);
	for (i = 0; i < 2 * FAKE_ROWS; i++)
		tsm_screen_newline(con);
	tsm_screen_sb_up(con, 2 * FAKE_ROWS);
	/* no room for the texture beside other displays' */
	other = image_budget_limit() - 64 * 32 * 4 + 1;
	image_budget_add_texture(other);

	txt.screen = con;
	frame = tsm_screen_frame_since(con, &age);
	assert(frame && frame->placement_count == 1);
	assert(gltex_prepare(&txt, &attr) == 0);
	assert(gltex_draw(&txt, frame, false) == 0);
	assert(gltex_render(&txt) == 0);
	assert(tsm_screen_test_image_count(con) == 1);
	assert(gt->starved && gt->pics_num == 0 && !gltex_starved(&txt));

	/* room: drawn */
	image_budget_remove_texture(other);
	assert(gltex_starved(&txt));
	frame = tsm_screen_frame_since(con, &age);
	assert(gltex_prepare(&txt, &attr) == 0);
	assert(gltex_draw(&txt, frame, false) == 0);
	assert(gltex_render(&txt) == 0);
	assert(gt->pics_num == 1 && tsm_screen_test_image_count(con) == 1);
	txt.screen = NULL;

	image_budget_remove_screen(con);
	tsm_screen_unref(con);
	close_txt(&txt);
}

/*
 * The image budget just full of pictures, every one placed on the screen:
 * bbulk draws them all, from libtsm's copies, and so does gltex (ADR-108).
 * Its textures count in a budget of their own, so they take no room from
 * libtsm's pictures.
 */
static void test_budget_full(void)
{
	static uint32_t px[160 * 96];
	struct tsm_test_image ti = {.width = 160, .height = 96, .pixels = px};
	struct tsm_test_placement pl = {0};
	struct tsm_frame_age age = {0};
	struct tsm_screen_attr attr = {0};
	const struct tsm_frame *frame;
	struct kmscon_text txt;
	struct tsm_screen *con;
	struct gltex *gt;
	uint64_t key, pkey;
	size_t held;
	unsigned int i;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, FAKE_COLS, FAKE_ROWS) == 0);
	assert(tsm_screen_set_cell_size(con, FAKE_CELL_W, FAKE_CELL_H) == 0);
	assert(image_budget_add_screen(con) == 0);
	/* four sixel pictures, 20 x 6 cells each, down the screen */
	for (i = 0; i < 4; i++) {
		assert(tsm_screen_test_image(con, &ti, &key) == 0);
		pl = (struct tsm_test_placement){.image = key, .row = 6 * i};
		assert(tsm_screen_test_place(con, &pl, &pkey) == 0);
	}
	held = image_budget_used();
	image_budget_set_limit(held);
	assert(image_budget_used() == held && image_budget_limit() == held);

	txt.screen = con;
	frame = tsm_screen_frame_since(con, &age);
	assert(frame && frame->placement_count == 4);
	assert(gltex_prepare(&txt, &attr) == 0);
	assert(gltex_draw(&txt, frame, false) == 0);
	assert(gltex_render(&txt) == 0);
	assert(!gt->starved && gt->pics_num == 4 && gt->images_num == 4);
	assert(tsm_screen_test_image_count(con) == 4 && image_budget_used() == held);
	assert(image_budget_textures() == 4 * sizeof(px));
	txt.screen = NULL;

	image_budget_set_limit(IMAGE_BUDGET_AUTO);
	image_budget_remove_screen(con);
	tsm_screen_unref(con);
	close_txt(&txt);
}

/*
 * The significant bits of a float in fragment source @src, on a GPU that has
 * highp there (GL_FRAGMENT_PRECISION_HIGH defined, as radeonsi and virgl
 * have): 24 for highp, fp32; 11 for mediump, the least GLES 2 allows and what
 * a GPU that runs mediump at half precision gives. The last precision
 * statement outside an #else branch decides.
 */
static int frag_bits(const char *src)
{
	const char *p = src, *found = NULL, *skip;

	while ((p = strstr(p, "precision "))) {
		skip = strstr(src, "#else");
		if (!skip || p < skip || p > strstr(skip, "#endif"))
			found = p;
		p++;
	}
	assert(found);
	found += strlen("precision ");
	if (!strncmp(found, "highp float", 11))
		return 24;
	assert(!strncmp(found, "mediump float", 13));
	return 11;
}

/* @v rounded to @bits significant bits, as a GPU float of that precision holds it */
static float round_bits(float v, int bits)
{
	uint32_t u, drop = 24 - bits;

	memcpy(&u, &v, sizeof(u));
	if (drop)
		u = (u + (1u << (drop - 1))) & ~((1u << drop) - 1);
	memcpy(&v, &u, sizeof(v));
	return v;
}

/* a varying at the centre of pixel @k of @n between vertices carrying @t1 and @t2 */
static float varying(float t1, float t2, unsigned int k, unsigned int n, int bits)
{
	return round_bits(t1 + (t2 - t1) * ((k + 0.5f) / n), bits);
}

/*
 * The texel, of an atlas @size texels long, that the cell shader samples at
 * @v cells scaled by @advance, in floats of @bits as gltex_atlas.frag
 * computes it
 */
static double texel_at(float v, float advance, unsigned int size, int bits)
{
	return (double)round_bits(round_bits(v, bits) * round_bits(advance, bits), bits) * size;
}

/* @a is within a 64th of a texel of @b */
static bool on_centre(double a, double b)
{
	return a - b < 1.0 / 64 && b - a < 1.0 / 64;
}

/*
 * #197: glyphs are drawn a texel a pixel, so each pixel of a cell must sample
 * its own texel of the atlas, at the texel's centre, whatever the glyph's
 * place in the atlas, up to the far end of a 2048-texel atlas. The atlases
 * are sampled GL_NEAREST, and the cell shader computes at a precision that
 * holds a texel's centre there. Bilinear filtering off the centres thins the
 * strokes and takes texels from the next glyph in the atlas.
 *
 * Every cell of a frame of distinct glyphs, underlined and struck through, is
 * walked pixel by pixel through the shader's arithmetic, at the precision its
 * source asks for, for the glyph and both decoration sprites. Run for the
 * test's 8x16 cell and for tier 3's 10x19.
 */
static void check_sampling(unsigned int cw, unsigned int ch)
{
	struct kmscon_font font = {.width = cw, .height = ch};
	struct tsm_frame_cell cell = {
		.width = 1, .underline = TSM_UNDERLINE_SINGLE, .flags = TSM_FRAME_STRIKE};
	struct kmscon_text txt;
	struct gltex *gt;
	const struct tsm_frame *frame;
	struct atlas *atlas, *deco;
	unsigned int i, x, y, slot, far = 0;
	int bits;

	memset(&txt, 0, sizeof(txt));
	txt.font = &font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;
	bits = frag_bits(cell_frag);

	frame = make_frame(0);
	image_frame.cols = txt.cols;
	image_frame.rows = txt.rows;
	for (i = 0; i < txt.cols * txt.rows; i++) {
		frame_cells[i] = cell;
		frame_cells[i].key = 0x100 + i;
	}
	frame_cells[0].key = COLOUR_CP; /* an RGBA atlas too */
	run_frame(&txt, frame, false);
	assert(n_draws && gt->deco_atlas);

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		assert(texs[atlas->tex].filter == GL_NEAREST && texs[atlas->tex].mag == GL_NEAREST);
		atlas->cache_num = 0; /* counted again below */
	}
	for (i = 0; i < n_draws; i++)
		assert(draws[i].filter == GL_NEAREST && draws[i].mag == GL_NEAREST);

	deco = gt->deco_atlas;
	for (i = 0; i < gt->quads_num; i++) {
		const struct gltex_quad *q = &gt->quads[i];
		unsigned int n = q->texw * cw;

		slot = q->atlas->cache_off + q->atlas->cache_num++;
		assert(q->atlas->width == 2048);
		if ((q->texoff + q->texw) * cw > 2048 - 2 * cw)
			far++;
		for (x = 0; x < n; x++) {
			/* the glyph's texel, then each sprite's: its cell, and x within it */
			float gv = varying(gt->cache_texpos[slot * 12],
					   gt->cache_texpos[slot * 12 + 4], x, n, bits);
			float dv = varying(gt->cache_deco[slot * 18], gt->cache_deco[slot * 18 + 6],
					   x, n, bits);
			float frac = dv - (int)dv;

			assert(on_centre(
				texel_at(gv, q->atlas->advance_htex, q->atlas->width, bits),
				q->texoff * cw + x + 0.5));
			assert(on_centre(
				texel_at(q->ulcell + frac, deco->advance_htex, deco->width, bits),
				q->ulcell * cw + x % cw + 0.5));
			assert(on_centre(texel_at(q->strikecell + frac, deco->advance_htex,
						  deco->width, bits),
					 q->strikecell * cw + x % cw + 0.5));
		}
		for (y = 0; y < ch; y++) {
			float v = varying(0, 1, y, ch, bits);

			assert(on_centre(
				texel_at(v, q->atlas->advance_vtex, q->atlas->height, bits),
				y + 0.5));
			assert(on_centre(texel_at(v, deco->advance_vtex, deco->height, bits),
					 y + 0.5));
		}
	}
	assert(far > 1);

	gltex_unset(&txt);
	gltex_destroy(&txt);
}

static void test_atlas_sampling(void)
{
	check_sampling(FAKE_CELL_W, FAKE_CELL_H);
	check_sampling(10, 19);
}

/* ---- the glyph cache (ADR-102) ---- */

/*
 * The cache tests draw with patterned glyphs on strips of eight cells (a
 * 64-texel GL_MAX_TEXTURE_SIZE) under a bound of 2 KiB a pool, with no bound
 * on its glyph count but where a test sets one: an alpha pool
 * takes two strips of 1 KiB, sixteen narrow slots or eight wide ones, and an
 * RGBA pool one strip of 4 KiB, eight narrow slots or four wide ones. A few
 * frames then reach every step of take_slot().
 */
#define CACHE_TEX 64
#define CACHE_BOUND 2048
#define NARROW_CAP 16 /* alpha one cell wide */
#define WIDE_CAP 8    /* alpha two cells wide */

/* the glyphs of each kind the cache tests draw: codepoints apart, see glyph_kind() */
#define NARROW_CP 0x100
#define WIDE_CP 0x4e01 /* past FAIL_CP */
#define EMOJI_CP 0x1f400
#define EMOJI_NARROW_CP 0x1f300

/* a cell a cache test draws: @key, @width cells wide, with @flags and @underline */
struct gcell {
	uint64_t key;
	unsigned int width;
	uint16_t flags;
	uint8_t underline;
};

static void open_cache(struct kmscon_text *txt, struct kmscon_font *font,
		       enum Orientation orientation)
{
	memset(txt, 0, sizeof(*txt));
	txt->font = font;
	txt->disp = (struct display *)0x1;
	txt->orientation = orientation;
	max_texture_size = CACHE_TEX;
	patterned = true;
	assert(gltex_init(txt) == 0);
	assert(gltex_set(txt) == 0);
	((struct gltex *)txt->data)->pool_max_glyphs = UINT_MAX;
	((struct gltex *)txt->data)->pool_max_bytes = CACHE_BOUND;
}

static void close_cache(struct kmscon_text *txt)
{
	gltex_unset(txt);
	gltex_destroy(txt);
	max_texture_size = 2048;
	patterned = false;
	colour_clusters = false;
}

/*
 * One frame: @n cells from the top left, a wide one taking two columns, each
 * drawn with its glyph (gltex_draw_cell() returns 0: none is left blank),
 * then rendered.
 */
static void cache_frame(struct kmscon_text *txt, const struct gcell *cells, unsigned int n)
{
	struct tsm_screen_attr attr = {0};
	struct tsm_frame_cell fc = {.fg = {200, 200, 200}, .bg = {0, 0, 40}};
	unsigned int i, x = 0, y = 0;

	assert(gltex_prepare(txt, &attr) == 0);
	txt->frame_misses = 0;
	for (i = 0; i < n; i++) {
		if (x + cells[i].width > txt->cols) {
			x = 0;
			y++;
		}
		assert(y < txt->rows);
		fc.width = cells[i].width;
		fc.flags = cells[i].flags;
		fc.underline = cells[i].underline;
		assert(gltex_draw_cell(txt, &fc, cells[i].key, x, y, cells[i].width, false) == 0);
		x += cells[i].width;
	}
	n_draws = 0;
	assert(gltex_render(txt) == 0);
}

/* the slot quad @q draws from */
static struct gl_glyph *slot_of(const struct gltex_quad *q)
{
	struct atlas *atlas = q->atlas;

	assert(atlas->pool && atlas->slots);
	assert((unsigned int)q->texoff % atlas->pool->cells == 0);
	return &atlas->slots[(unsigned int)q->texoff / atlas->pool->cells];
}

/*
 * Each of the last frame's @n cells was drawn from a slot that holds its
 * glyph at render: the quad's slot is held, under the cell's key.
 */
static void assert_resident(struct gltex *gt, const struct gcell *cells, unsigned int n)
{
	struct gl_glyph *slot;
	unsigned int i;

	assert(gt->quads_num == n);
	for (i = 0; i < n; i++) {
		slot = slot_of(&gt->quads[i]);
		assert(slot->held && slot->key.key == cells[i].key);
		assert(slot->key.width == cells[i].width);
	}
}

struct bytes {
	uint8_t *p;
	size_t n, size;
};

static void put(struct bytes *b, const void *data, size_t len)
{
	if (b->n + len > b->size) {
		b->size = (b->n + len) * 2;
		b->p = realloc(b->p, b->size);
		assert(b->p);
	}
	memcpy(b->p + b->n, data, len);
	b->n += len;
}

/* @w x @h texels of @atlas from cell @cell, a row at a time */
static void put_texels(struct bytes *b, const struct atlas *atlas, unsigned int cell,
		       unsigned int cw, unsigned int w, unsigned int h)
{
	const struct fake_tex *t = &texs[atlas->tex];
	unsigned int bpp = bytes_per_pixel(atlas->format), y;

	assert(t->live && t->data && cell * cw + w <= (unsigned int)t->w);
	for (y = 0; y < h; y++)
		put(b, &t->data[((size_t)y * t->w + cell * cw) * bpp], (size_t)w * bpp);
}

/*
 * What the last frame's quads sample, in the order they were queued: each
 * quad's place, colours and width, its glyph's texels from its atlas, of its
 * format, and the texels of its underline and strike-through sprites. The
 * cell shader's output is a function of these alone (check_sampling holds
 * its sampling to them), so two frames that resolve alike draw alike, pixel
 * for pixel, wherever their glyphs sit in the atlases.
 */
static struct bytes resolve(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	unsigned int cw = FONT_WIDTH(txt), ch = FONT_HEIGHT(txt), i;
	struct bytes b = {0};

	for (i = 0; i < gt->quads_num; i++) {
		const struct gltex_quad *q = &gt->quads[i];
		const struct tsm_screen_color *c[] = {&q->fg, &q->bg, &q->ul, &q->strike};
		uint8_t colour = q->atlas->format == GL_RGBA;

		put(&b, &q->x1, sizeof(q->x1));
		put(&b, &q->y1, sizeof(q->y1));
		put(&b, &q->texw, sizeof(q->texw));
		put(&b, &colour, 1);
		for (unsigned int j = 0; j < 4; j++) {
			put(&b, &c[j]->r, 1);
			put(&b, &c[j]->g, 1);
			put(&b, &c[j]->b, 1);
		}
		put_texels(&b, q->atlas, q->texoff, cw, cw * q->texw, ch);
		if (gt->deco_atlas) {
			put_texels(&b, gt->deco_atlas, q->ulcell, cw, cw, ch);
			put_texels(&b, gt->deco_atlas, q->strikecell, cw, cw, ch);
		}
	}
	return b;
}

/*
 * Each of the last frame's @n cells samples its own glyph: the texels of its
 * quad are the font's pixels for the cell's key, as RGBA bytes for a colour
 * glyph, and clear past a glyph smaller than its cells.
 */
static void assert_own_texels(struct kmscon_text *txt, const struct gcell *cells, unsigned int n)
{
	struct gltex *gt = txt->data;
	unsigned int cw = FONT_WIDTH(txt), ch = FONT_HEIGHT(txt), rendered = n_rendered;
	unsigned int i, x, y, bpp;
	struct kmscon_font_attr attr = {0};
	const uint32_t *cps;
	uint32_t one;
	size_t len;

	assert(gt->quads_num == n);
	for (i = 0; i < n; i++) {
		const struct gltex_quad *q = &gt->quads[i];
		const struct fake_tex *t = &texs[q->atlas->tex];
		struct kmscon_glyph *g;

		attr.bold = !!(cells[i].flags & TSM_FRAME_BOLD);
		attr.italic = !!(cells[i].flags & TSM_FRAME_ITALIC);
		cps = kmscon_text_get_cluster(txt, cells[i].key, &one, &len);
		g = kmscon_font_render(txt->font, &attr, cps, len, cells[i].width);
		assert(g);
		bpp = g->buf.format == VIDEO_FORMAT_ARGB32 ? 4 : 1;
		if (bpp == 4)
			argb_to_rgba(g);
		assert(bytes_per_pixel(q->atlas->format) == bpp && q->texw == cells[i].width);
		for (y = 0; y < ch; y++)
			for (x = 0; x < cw * cells[i].width; x++) {
				const uint8_t *px =
					&t->data[((size_t)y * t->w + (size_t)q->texoff * cw + x) *
						 bpp];
				bool in = x < g->buf.width && y < g->buf.height;

				for (unsigned int b = 0; b < bpp; b++)
					assert(px[b] ==
					       (in ? g->buf.data[(y * g->buf.width + x) * bpp + b]
						   : 0));
			}
		free(g);
	}
	n_rendered = rendered;
}

/*
 * The last frame of @txt, whose cache has drawn whatever it has, resolves as
 * the same @n cells drawn by a gltex with an empty cache, and each cell
 * samples its own glyph.
 */
static void assert_as_cold(struct kmscon_text *txt, const struct gcell *cells, unsigned int n)
{
	struct kmscon_text cold;
	struct bytes warm, fresh;
	bool colour = colour_clusters;

	assert_own_texels(txt, cells, n);
	warm = resolve(txt);
	open_cache(&cold, txt->font, txt->orientation);
	colour_clusters = colour;
	cache_frame(&cold, cells, n);
	fresh = resolve(&cold);
	assert(warm.n == fresh.n && warm.n);
	assert(!memcmp(warm.p, fresh.p, warm.n));
	free(warm.p);
	free(fresh.p);
	gltex_unset(&cold);
	gltex_destroy(&cold);
	max_texture_size = CACHE_TEX;
	patterned = true;
	colour_clusters = colour;
}

/* a seeded generator, so a failure repeats */
static uint32_t rnd_state;

static uint32_t rnd(uint32_t n)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return (rnd_state >> 8) % n;
}

/* the pool a cache test's glyph @key, @width cells wide, goes to */
static unsigned int glyph_kind(uint64_t key, unsigned int width)
{
	return (key >= COLOUR_FIRST ? 2 : 0) + width - 1;
}

/*
 * A random frame of up to @max cells, from a small set of glyphs of every
 * kind, so frames share glyphs and every pool hits, misses and evicts.
 */
static unsigned int random_frame(struct gcell *cells, unsigned int max)
{
	unsigned int n = 1 + rnd(max), i;

	for (i = 0; i < n; i++) {
		switch (rnd(4)) {
		case 0:
			cells[i] = (struct gcell){NARROW_CP + rnd(24), 1};
			break;
		case 1:
			cells[i] = (struct gcell){WIDE_CP + rnd(12), 2};
			break;
		case 2:
			cells[i] = (struct gcell){EMOJI_NARROW_CP + rnd(12), 1};
			break;
		default:
			cells[i] = (struct gcell){EMOJI_CP + rnd(8), 2};
			break;
		}
	}
	return n;
}

/*
 * #1: after any frames, a screen drawn equals, pixel for pixel, the same
 * screen drawn from an empty cache. Three seeds of 300 random frames each,
 * compared every tenth frame.
 */
static void test_cache_cold_oracle(void)
{
	struct gcell cells[40];
	struct kmscon_text txt;
	unsigned int seed, f, n;

	for (seed = 1; seed <= 3; seed++) {
		rnd_state = seed;
		open_cache(&txt, &fake_font, OR_NORMAL);
		for (f = 0; f < 300; f++) {
			n = random_frame(cells, 40);
			cache_frame(&txt, cells, n);
			if (f % 10 == 9)
				assert_as_cold(&txt, cells, n);
			else
				assert_own_texels(&txt, cells, n);
		}
		close_cache(&txt);
	}
}

/*
 * #2: a reference LRU, a plain list per pool of its slots, the most recently
 * drawn first and the empty ones last, each slot knowing its strip. Glyphs
 * are looked up and placed as take_slot()'s comment says, and strips given
 * back as pool_give_back()'s says, newest first among the least recently
 * drawn, as gltex walks its atlases.
 */
#define REF_SLOTS 4096
#define REF_STRIPS (1 << 15) /* made over the run, given back or not */

struct ref_slot {
	uint64_t key; /* 0: empty */
	uint64_t last;
	unsigned int strip;
};

struct ref_pool {
	struct ref_slot s[REF_SLOTS];
	unsigned int n;			 /* slots, in list order */
	uint64_t strip_last[REF_STRIPS]; /* the frame that last drew from each strip */
	bool strip_live[REF_STRIPS];
	unsigned int strips; /* made, live or not */
	unsigned int live;
	size_t strip_bytes;
	unsigned int strip_slots;
	bool overshot; /* it took a strip past full */
};

/* the reference's bound: a pool is full at this many slots or bytes */
static unsigned int ref_glyphs;
static size_t ref_bytes;

static bool ref_full(const struct ref_pool *rp, unsigned int live)
{
	return live * rp->strip_slots >= ref_glyphs || live * rp->strip_bytes >= ref_bytes;
}

/* slot @i goes to the front, holding @key, drawn in @frame */
static void ref_front(struct ref_pool *rp, unsigned int i, uint64_t key, uint64_t frame)
{
	struct ref_slot slot = rp->s[i];

	memmove(&rp->s[1], &rp->s[0], i * sizeof(*rp->s));
	slot.key = key;
	slot.last = frame;
	rp->s[0] = slot;
	rp->strip_last[slot.strip] = frame;
}

static void ref_draw(struct ref_pool *rp, uint64_t key, uint64_t frame)
{
	unsigned int i, k;

	for (i = 0; i < rp->n && rp->s[i].key != key; i++)
		;
	if (i < rp->n) {
		ref_front(rp, i, key, frame);
		return;
	}
	/* a miss: an empty slot, else a strip, else the back's slot */
	if (!rp->n ||
	    (rp->s[rp->n - 1].key && (!ref_full(rp, rp->live) || rp->s[rp->n - 1].last == frame))) {
		rp->overshot |= ref_full(rp, rp->live);
		assert(rp->strips < REF_STRIPS && rp->n + rp->strip_slots <= REF_SLOTS);
		for (k = 0; k < rp->strip_slots; k++)
			rp->s[rp->n++] = (struct ref_slot){0, 0, rp->strips};
		rp->strip_last[rp->strips] = 0;
		rp->strip_live[rp->strips++] = true;
		rp->live++;
	}
	ref_front(rp, rp->n - 1, key, frame);
}

/* at the start of the frame after @last */
static void ref_give_back(struct ref_pool *rp, uint64_t last)
{
	unsigned int t, i, j, oldest;

	for (;;) {
		oldest = REF_STRIPS;
		for (t = rp->strips; t-- > 0;) {
			if (!rp->strip_live[t] || rp->strip_last[t] == last)
				continue;
			if (oldest == REF_STRIPS || rp->strip_last[t] < rp->strip_last[oldest])
				oldest = t;
		}
		if (oldest == REF_STRIPS || !ref_full(rp, rp->live - 1))
			return;
		for (i = j = 0; i < rp->n; i++)
			if (rp->s[i].strip != oldest)
				rp->s[j++] = rp->s[i];
		rp->n = j;
		rp->strip_live[oldest] = false;
		rp->live--;
	}
}

/* gltex's pool @pool holds what @rp holds, in its order, in as many strips */
static void assert_as_ref(const struct pool *pool, const struct ref_pool *rp)
{
	const struct gl_glyph *slot;
	unsigned int i = 0;

	dlist_for_each_entry(slot, &pool->lru, lru)
	{
		assert(i < rp->n && slot->held == !!rp->s[i].key);
		assert(!slot->held || slot->key.key == rp->s[i].key);
		i++;
	}
	assert(i == rp->n);
	assert(pool->slots == rp->live * rp->strip_slots &&
	       pool->bytes == rp->live * rp->strip_bytes);
	/* not full before its last strip, unless a frame of all its glyphs needed it */
	assert(rp->overshot || rp->live <= 1 || !ref_full(rp, rp->live - 1));
}

/*
 * #2: seeded random frames of narrow, wide and colour glyphs. After every
 * frame each pool holds the glyphs the reference LRU holds, in its order and
 * in as many slots and bytes, and every cell drew from a slot that held its
 * glyph. Run with the pools bounded in bytes, then in glyphs.
 */
static void cache_model(unsigned int glyphs, size_t max_bytes)
{
	static const size_t bytes[] = {1024, 1024, 4096, 4096};
	static struct ref_pool ref[GLTEX_POOLS];
	struct gcell cells[40];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int f, i, k, n, overshoots = 0;

	rnd_state = 42;
	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	gt->pool_max_glyphs = ref_glyphs = glyphs;
	gt->pool_max_bytes = ref_bytes = max_bytes;
	memset(ref, 0, sizeof(ref));
	for (k = 0; k < GLTEX_POOLS; k++) {
		ref[k].strip_bytes = bytes[k];
		ref[k].strip_slots = CACHE_TEX / FAKE_CELL_W / (k % 2 + 1);
	}

	for (f = 0; f < 3000; f++) {
		for (k = 0; k < GLTEX_POOLS; k++)
			ref_give_back(&ref[k], gt->frame_count);
		n = random_frame(cells, 40);
		cache_frame(&txt, cells, n);
		assert_resident(gt, cells, n);
		for (i = 0; i < n; i++)
			ref_draw(&ref[glyph_kind(cells[i].key, cells[i].width)], cells[i].key,
				 gt->frame_count);
		for (k = 0; k < GLTEX_POOLS; k++)
			assert_as_ref(&gt->pools[k], &ref[k]);
	}
	for (k = 0; k < GLTEX_POOLS; k++)
		overshoots += ref[k].overshot;
	fprintf(stderr, "cache model: 3000 frames, strips %u %u %u %u, %u pools past full\n",
		ref[0].live, ref[1].live, ref[2].live, ref[3].live, overshoots);
	/* the run reached step 4 and give-back, not only steps 1 to 3 */
	assert(overshoots > 0);
	close_cache(&txt);
}

static void test_cache_model(void)
{
	cache_model(UINT_MAX, CACHE_BOUND);
	cache_model(4, SIZE_MAX);
}

/* @n distinct glyphs from @cp, @width cells wide, as many a frame as fit */
static void draw_distinct(struct kmscon_text *txt, uint64_t cp, unsigned int width, unsigned int n)
{
	static struct gcell cells[FAKE_COLS * FAKE_ROWS];
	unsigned int per = txt->cols / width * txt->rows, i, k;

	for (i = 0; i < n; i += k) {
		for (k = 0; k < per && i + k < n; k++)
			cells[k] = (struct gcell){cp + i + k, width};
		cache_frame(txt, cells, k);
		assert_resident(txt->data, cells, k);
	}
}

/*
 * The defaults: a pool holds GLTEX_POOL_GLYPHS glyphs, so it grows with the
 * font, up to a fourth of GLTEX_CACHE_MAX_BYTES, whichever is less. In 8x16
 * cells, 1920x1080 with a small font, the count governs: the alpha pool one
 * cell wide stops at 8192 slots, 32 strips of 256 in 1 MiB, far under its
 * 64 MiB.
 */
static void test_cache_count_governs(void)
{
	struct kmscon_text txt;
	struct gltex *gt;

	open_txt(&txt, OR_NORMAL);
	gt = txt.data;
	assert(gt->pool_max_glyphs == 8192 && gt->pool_max_bytes == 64 << 20);
	draw_distinct(&txt, 0x100, 1, 8192 + 100);
	assert(gt->pools[0].slots == 8192 && gt->pools[0].bytes == 32 * 2048 * 16);
	close_txt(&txt);
}

/*
 * In 19x41 cells, as 7680x2160 with font size 30 has (drawn here at 1280x800,
 * as the strips do not depend on the screen), an RGBA strip is 2048x64 and
 * holds 53 colour glyphs two cells wide in 512 KiB: 8192 of them would take
 * 155 strips, 77.5 MiB, so the ceiling clamps the pool at 128 strips, 64 MiB,
 * 6784 glyphs. The alpha pool one cell wide in the same cells stops at its
 * count, 77 strips of 107, 9.6 MiB.
 */
static void test_cache_ceiling_clamps(void)
{
	struct kmscon_font big = {.width = 19, .height = 41};
	struct kmscon_text txt;
	struct gltex *gt;

	disp_w = 1280;
	disp_h = 800;
	open_txt(&txt, OR_NORMAL);
	txt.font = &big;
	gltex_unset(&txt);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;
	patterned = true;
	colour_clusters = true; /* clusters, as there are not 7000 emoji codepoints */
	draw_distinct(&txt, TSM_FRAME_CLUSTER_KEY, 2, 7000);
	colour_clusters = false;
	assert(gt->pools[3].slots == 128 * 53 && gt->pools[3].bytes == (size_t)64 << 20);
	draw_distinct(&txt, 0x100, 1, 8300);
	assert(gt->pools[0].slots == 77 * 107 && gt->pools[0].bytes == (size_t)77 * 2048 * 64);
	patterned = false;
	close_txt(&txt);
	disp_w = 640;
	disp_h = 480;
}

/*
 * #3: a working set exactly the pool's capacity, sixteen narrow glyphs, drawn
 * whole and then one a frame: after the first frame nothing is drawn from the
 * font again, in exactly the two strips the bound allows.
 */
static void test_cache_exact_capacity(void)
{
	struct gcell cells[NARROW_CAP];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int f, i, misses;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (i = 0; i < NARROW_CAP; i++)
		cells[i] = (struct gcell){NARROW_CP + i, 1};
	cache_frame(&txt, cells, NARROW_CAP);
	assert(txt.frame_misses == NARROW_CAP);

	misses = 0;
	for (f = 0; f < 10; f++) {
		cache_frame(&txt, cells, NARROW_CAP);
		misses += txt.frame_misses;
	}
	for (f = 0; f < 3 * NARROW_CAP; f++) {
		cache_frame(&txt, &cells[f % NARROW_CAP], 1);
		misses += txt.frame_misses;
	}
	assert(misses == 0);
	assert(gt->pools[0].bytes == CACHE_BOUND);
	close_cache(&txt);
}

/*
 * #4: a working set one past the capacity, drawn one a frame in a cycle:
 * LRU's worst case, where the glyph drawn is always the one just evicted, so
 * every draw misses; each cell still draws its own glyph.
 */
static void test_cache_capacity_plus_one(void)
{
	struct gcell cell;
	struct kmscon_text txt;
	unsigned int f, misses = 0, rounds = 5;

	open_cache(&txt, &fake_font, OR_NORMAL);
	for (f = 0; f < rounds * (NARROW_CAP + 1); f++) {
		cell = (struct gcell){NARROW_CP + f % (NARROW_CAP + 1), 1};
		cache_frame(&txt, &cell, 1);
		misses += txt.frame_misses;
		assert_resident(txt.data, &cell, 1);
		if (f % 7 == 0)
			assert_as_cold(&txt, &cell, 1);
	}
	fprintf(stderr, "capacity %u, working set %u in a cycle: %u draws, %u misses\n", NARROW_CAP,
		NARROW_CAP + 1, rounds * (NARROW_CAP + 1), misses);
	assert(misses == rounds * (NARROW_CAP + 1));
	close_cache(&txt);
}

/*
 * #5: one frame needs more glyphs than the cache holds: a screen of distinct
 * CJK, 1200 wide glyphs where the pool's bound holds eight. The pool takes
 * strips past its bound (step 4), and every cell draws its own glyph. It
 * keeps them while frames draw from them: the next screen of other glyphs
 * reuses their slots and takes no more.
 */
static void test_cache_frame_overshoot(void)
{
	static struct gcell cells[FAKE_COLS / 2 * FAKE_ROWS];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int i, n = FAKE_COLS / 2 * FAKE_ROWS, before;
	size_t bytes;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (i = 0; i < n; i++)
		cells[i] = (struct gcell){WIDE_CP + i, 2};
	before = n_textures;
	cache_frame(&txt, cells, n);
	assert_resident(gt, cells, n);
	/* 300 strips of four, and the sprites' */
	bytes = gt->pools[1].bytes;
	assert(bytes == (size_t)n / 4 * 1024 && n_textures - before == n / 4 + 1);
	assert_as_cold(&txt, cells, n);

	for (i = 0; i < n; i++)
		cells[i] = (struct gcell){WIDE_CP + n + i, 2};
	cache_frame(&txt, cells, n);
	assert_resident(gt, cells, n);
	assert(gt->pools[1].bytes == bytes);
	close_cache(&txt);
}

/*
 * #6: a glyph queued earlier in a frame keeps its slot through the frame. A,
 * cached by the last frame, is drawn first, then twenty glyphs the cache does
 * not hold, more than the pool's sixteen slots: they evict around it, and A's
 * cell still draws A.
 */
static void test_cache_same_frame(void)
{
	struct gcell cells[21];
	struct kmscon_text txt;
	unsigned int i;

	open_cache(&txt, &fake_font, OR_NORMAL);
	cells[0] = (struct gcell){'A', 1};
	cache_frame(&txt, cells, 1);
	for (i = 1; i < 21; i++)
		cells[i] = (struct gcell){NARROW_CP + i, 1};
	cache_frame(&txt, cells, 21);
	assert(txt.frame_misses == 20);
	assert_resident(txt.data, cells, 21);
	assert_as_cold(&txt, cells, 21);
	close_cache(&txt);
}

/*
 * No two glyphs the cache holds share a cell of an atlas, and every slot lies
 * whole in its strip, on a boundary of its pool's width.
 */
static void assert_slots_apart(struct gltex *gt)
{
	struct atlas *atlas;
	unsigned int i, n;

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		if (!atlas->pool)
			continue;
		n = atlas->count / atlas->pool->cells;
		for (i = 0; i < n; i++) {
			assert(atlas->slots[i].atlas == atlas);
			assert(atlas->slots[i].texoff == i * atlas->pool->cells);
			assert(atlas->slots[i].texoff + atlas->pool->cells <= atlas->count);
			assert(!atlas->slots[i].held ||
			       atlas->slots[i].key.width == atlas->pool->cells);
		}
	}
}

/*
 * #7: narrow and wide glyphs do not starve each other: a pool full of one
 * width leaves the other its own strips. Narrow glyphs fill their pool, wide
 * ones churn through theirs, and the narrow ones are all still cached; then
 * the same the other way round. A wide glyph always has both its cells.
 */
static void test_cache_narrow_wide(void)
{
	struct gcell narrow[NARROW_CAP], wide[WIDE_CAP], churn;
	struct kmscon_text txt;
	unsigned int i, order;

	for (i = 0; i < NARROW_CAP; i++)
		narrow[i] = (struct gcell){NARROW_CP + i, 1};
	for (i = 0; i < WIDE_CAP; i++)
		wide[i] = (struct gcell){WIDE_CP + i, 2};

	for (order = 0; order < 2; order++) {
		open_cache(&txt, &fake_font, OR_NORMAL);
		cache_frame(&txt, order ? wide : narrow, order ? WIDE_CAP : NARROW_CAP);
		for (i = 0; i < 100; i++) {
			churn = order ? (struct gcell){NARROW_CP + 100 + i, 1}
				      : (struct gcell){WIDE_CP + 100 + i, 2};
			cache_frame(&txt, &churn, 1);
			assert_slots_apart(txt.data);
		}
		cache_frame(&txt, order ? wide : narrow, order ? WIDE_CAP : NARROW_CAP);
		assert(txt.frame_misses == 0);
		assert_as_cold(&txt, order ? wide : narrow, order ? WIDE_CAP : NARROW_CAP);
		close_cache(&txt);
	}
}

/*
 * #8: colour glyphs churn without pushing out the text on screen. Each frame
 * draws a line of text and six colour glyphs it has never drawn: the text is
 * drawn from the font once, the alpha pools stay as they are, and the RGBA
 * pool takes the one strip past its bound six glyphs a frame need, then no
 * more.
 */
static void test_cache_colour_alpha(void)
{
	static const char text[] = "hello, world";
	struct gcell cells[sizeof(text) - 1 + 6];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int f, i, t = sizeof(text) - 1;
	size_t alpha = 0, rgba = 0;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (i = 0; i < t; i++)
		cells[i] = (struct gcell){(uint8_t)text[i], 1};
	for (f = 0; f < 50; f++) {
		for (i = 0; i < 6; i++)
			cells[t + i] = (struct gcell){EMOJI_CP + f * 6 + i, 2};
		cache_frame(&txt, cells, t + 6);
		/* the text's nine glyphs ("l" thrice, "o" twice) once */
		assert(txt.frame_misses == (f ? 6 : 9 + 6));
		if (f == 1) {
			alpha = gt->pools[0].bytes;
			rgba = gt->pools[3].bytes;
			assert(rgba == 2 * 4096);
		}
		if (f > 1)
			assert(gt->pools[0].bytes == alpha && gt->pools[3].bytes == rgba);
	}
	assert_as_cold(&txt, cells, t + 6);
	close_cache(&txt);
}

/*
 * #9: a glyph smaller than its slot leaves nothing of the slot's last glyph.
 * The pool is filled with whole glyphs, then a glyph three texels narrower
 * and two shorter takes the least recently drawn one's slot: those texels are
 * clear, the rest are its own. (A wide glyph never gives its slot to a narrow
 * one: their pools differ, which assert_slots_apart() checks.)
 */
static void test_cache_slot_cleared(void)
{
	struct gcell cells[NARROW_CAP + 1];
	struct kmscon_font_attr attr = {0};
	struct kmscon_text txt;
	struct gltex *gt;
	const struct fake_tex *t;
	struct gl_glyph *slot;
	unsigned int i, x, y, x0;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (i = 0; i < NARROW_CAP; i++)
		cells[i] = (struct gcell){NARROW_CP + i, 1};
	cache_frame(&txt, cells, NARROW_CAP);
	cells[NARROW_CAP] = (struct gcell){SHORT_CP, 1};
	cache_frame(&txt, &cells[NARROW_CAP], 1);
	assert(txt.frame_misses == 1);

	slot = slot_of(&gt->quads[0]);
	t = &texs[slot->atlas->tex];
	x0 = slot->texoff * FAKE_CELL_W;
	for (y = 0; y < FAKE_CELL_H; y++)
		for (x = 0; x < FAKE_CELL_W; x++) {
			uint8_t want = x < FAKE_CELL_W - 3 && y < FAKE_CELL_H - 2
					       ? (uint8_t)patterned_px(SHORT_CP, &attr, x, y)
					       : 0;

			assert(t->data[y * t->w + x0 + x] == want);
		}
	assert_slots_apart(gt);
	close_cache(&txt);
}

/*
 * #10: the decoration sprites stay through any churn. An underlined, struck
 * cell is drawn with two thousand frames of glyphs of every kind: the sprites'
 * strip is the one uploaded first, uploaded once, its cells deco.c's sprites,
 * and the cell resolves as from an empty cache.
 */
static void test_cache_deco_kept(void)
{
	struct gcell cells[41];
	struct kmscon_text txt;
	struct gltex *gt;
	struct atlas *deco;
	GLuint tex;
	unsigned int f, n, uploads;

	rnd_state = 7;
	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	uploads = strip_uploads;
	cells[0] = (struct gcell){'A', 1, TSM_FRAME_STRIKE, TSM_UNDERLINE_SINGLE};
	cache_frame(&txt, cells, 1);
	deco = gt->deco_atlas;
	assert(deco);
	tex = deco->tex;
	for (f = 0; f < 2000; f++) {
		n = random_frame(&cells[1], 40);
		cache_frame(&txt, cells, n + 1);
	}
	assert(gt->deco_atlas == deco && deco->tex == tex && texs[tex].live);
	assert(strip_uploads == uploads + 1);
	assert(gt->quads[0].ulcell == KMSCON_DECO_SINGLE &&
	       gt->quads[0].strikecell == KMSCON_DECO_STRIKE);
	assert_as_cold(&txt, cells, n + 1);
	assert(!deco->pool); /* in no pool, on no list */
	close_cache(&txt);
}

/* how many slots the pools' strips have */
static size_t cache_slots(struct gltex *gt)
{
	struct atlas *atlas;
	size_t n = 0;

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		if (atlas->pool)
			n += atlas->count / atlas->pool->cells;
	}
	return n;
}

/*
 * #11: a cluster rewritten 10,000 times, through libtsm: a combining mark on
 * a prompt line, then a ZWJ sequence drawn in colour. libtsm gives the
 * cluster a new key each time; each frame draws only it from the font, the
 * prompt's glyphs stay cached, the dead keys age out of the cache, which never
 * holds more glyphs than it has slots, and the pools stay within the bound.
 */
static void test_cache_cluster_churn(void)
{
	static const char prompt[] = "\x1b[?25l$ ok ";
	/* e and a combining acute; woman, ZWJ, rocket */
	static const char *const clusters[] = {
		"e\xcc\x81",
		"\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x9a\x80",
	};
	struct tsm_frame_age age = {0};
	struct tsm_screen_attr attr = {0};
	const struct tsm_frame *frame;
	struct kmscon_text txt;
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct gltex *gt;
	unsigned int c, i, k;
	uint64_t key, last = 0;

	for (c = 0; c < 2; c++) {
		open_cache(&txt, &fake_font, OR_NORMAL);
		colour_clusters = c;
		gt = txt.data;
		assert(tsm_screen_new(&con, NULL, NULL) == 0);
		assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);
		assert(tsm_screen_resize(con, 12, 2) == 0);
		tsm_vte_input(vte, prompt, sizeof(prompt) - 1);

		for (i = 0; i < 10000; i++) {
			tsm_vte_input(vte, "\r\x1b[5C", 5);
			tsm_vte_input(vte, clusters[c], strlen(clusters[c]));
			frame = tsm_screen_frame_since(con, &age);
			assert(frame);
			assert(gltex_prepare(&txt, &attr) == 0);
			txt.frame_misses = 0;
			assert(gltex_draw(&txt, frame, false) == 0);
			n_draws = 0;
			assert(gltex_render(&txt) == 0);

			key = tsm_frame_cell_at(frame, 5, 0)->key;
			assert(key >= TSM_FRAME_CLUSTER_KEY && key != last);
			last = key;
			/* the new cluster, and in the first frame the prompt and blanks */
			assert(txt.frame_misses == 1 || (!i && txt.frame_misses <= 6));
			assert(gt->glyphs.elems <= cache_slots(gt));
			for (k = 0; k < GLTEX_POOLS; k++)
				assert(gt->pools[k].bytes <= CACHE_BOUND + 4096);
		}
		assert(drawn_at(gt, 0, 0, &(struct atlas *){0}, &(float){0}) == 1);
		tsm_vte_unref(vte);
		tsm_screen_unref(con);
		close_cache(&txt);
	}
}

/*
 * #13: a font size change and a rotation in the middle of churn set the
 * renderer up again, as kmscon_text_set() and gltex_rotate() do: every
 * glyph, slot and strip of the old cache goes (ASan finds what is left or
 * used after), and the cache refills at the new size and orientation, drawing
 * as from an empty cache.
 */
static void test_cache_reset(void)
{
	struct kmscon_font big = {.width = 10, .height = 19};
	struct gcell cells[40];
	struct kmscon_text txt;
	unsigned int f, n = 0, step;

	rnd_state = 99;
	open_cache(&txt, &fake_font, OR_NORMAL);
	for (step = 0; step < 3; step++) {
		for (f = 0; f < 200; f++) {
			n = random_frame(cells, 40);
			cache_frame(&txt, cells, n);
		}
		assert_as_cold(&txt, cells, n);
		if (step == 0) {
			gltex_unset(&txt);
			txt.font = &big;
			assert(gltex_set(&txt) == 0);
		} else if (step == 1) {
			assert(gltex_rotate(&txt, OR_RIGHT) == 0);
		}
		((struct gltex *)txt.data)->pool_max_bytes = CACHE_BOUND;
		assert(dlist_empty(&((struct gltex *)txt.data)->atlases) || step == 2);
	}
	close_cache(&txt);
}

/*
 * An upload GL refuses into a slot step 3 took: the evicted glyph is gone
 * from the cache, the slot stays empty at the back of its list, the cell is
 * drawn blank with its background (a space would take the same slot and its
 * upload fails too), and the next glyph the cache misses takes that slot.
 */
static void test_cache_upload_failure(void)
{
	struct tsm_screen_attr attr = {0};
	struct tsm_frame_cell fc = {.fg = {200, 200, 200}, .bg = {0, 0, 40}, .width = 1};
	struct gcell cells[NARROW_CAP];
	struct kmscon_text txt;
	struct gltex *gt;
	struct gl_glyph *lru, *back;
	struct kmscon_glyph_key k0 = {.key = NARROW_CP, .width = 1};
	unsigned int i;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (i = 0; i < NARROW_CAP; i++)
		cells[i] = (struct gcell){NARROW_CP + i, 1};
	cache_frame(&txt, cells, NARROW_CAP);
	lru = lookup(gt, kmscon_glyph_key_hash(NARROW_CP, false, false, 1, 0), &k0);
	assert(lru && lru == dlist_last(&gt->pools[0].lru, struct gl_glyph, lru));

	assert(gltex_prepare(&txt, &attr) == 0);
	fail_uploads = 2;
	assert(gltex_draw_cell(&txt, &fc, 'X', 0, 0, 1, false) == -ENOMEM);
	assert(!fail_uploads);
	assert(!lookup(gt, kmscon_glyph_key_hash(NARROW_CP, false, false, 1, 0), &k0));
	back = dlist_last(&gt->pools[0].lru, struct gl_glyph, lru);
	assert(back == lru && !back->held);
	assert(gt->quads_num == 1 && gt->quads[0].atlas == gt->deco_atlas &&
	       gt->quads[0].texoff == KMSCON_DECO_NONE && colour_eq(gt->quads[0].bg, fc.bg));

	assert(gltex_draw_cell(&txt, &fc, 'Y', 1, 0, 1, false) == 0);
	assert(slot_of(&gt->quads[1]) == lru && lru->held && lru->key.key == 'Y');
	assert(gltex_render(&txt) == 0);
	close_cache(&txt);
}

/*
 * A rotation whose set fails, as with no GL context, leaves a gltex that
 * draws nothing until a set succeeds: gltex_rotate() says so, which its
 * callers ignore, and each gltex_prepare() tries the set again, failing
 * while the context is gone and saying so once; gltex_draw() refuses instead
 * of using what the unset freed. The first prepare whose set succeeds keeps
 * the size in cells the terminal gave meanwhile, and draws as from an empty
 * cache, in the new orientation.
 */
static void test_cache_failed_rotate(void)
{
	struct tsm_screen_attr attr = {0};
	struct gcell cells[40];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int f, n = 0;

	rnd_state = 5;
	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (f = 0; f < 50; f++) {
		n = random_frame(cells, 40);
		cache_frame(&txt, cells, n);
	}
	fail_display_use = true;
	assert(gltex_rotate(&txt, OR_RIGHT) < 0);
	assert(!gt->set_failed_logged);
	assert(gltex_prepare(&txt, &attr) == -EFAULT);
	assert(gt->set_failed_logged && !gt->shader);
	assert(gltex_prepare(&txt, &attr) == -EFAULT);
	assert(gltex_draw(&txt, make_frame(0), false) == -EINVAL);
	/* the terminal's size meanwhile, a cell short of the text's each way */
	gltex_resize(&txt, txt.max_cols - 1, txt.max_rows - 1);
	fail_display_use = false;

	assert(gltex_prepare(&txt, &attr) == 0);
	assert(gt->shader && !gt->set_failed_logged && txt.orientation == OR_RIGHT);
	assert(txt.cols == txt.max_cols - 1 && txt.rows == txt.max_rows - 1);
	gltex_resize(&txt, txt.max_cols, txt.max_rows);
	gt->pool_max_glyphs = UINT_MAX;
	gt->pool_max_bytes = CACHE_BOUND;
	cache_frame(&txt, cells, n);
	assert_as_cold(&txt, cells, n);
	close_cache(&txt);
}

/*
 * A gltex whose first set fails, as with no GL context, holds nothing:
 * gltex_init() leaves its atlases and pools empty, and a release, which a
 * switch away runs on every screen, leaves it alone without making a context
 * current. Its next set draws as from an empty cache.
 */
static void test_cache_unset_release(void)
{
	struct gcell cells[40];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int k, n, uses;

	memset(&txt, 0, sizeof(txt));
	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	max_texture_size = CACHE_TEX;
	patterned = true;
	assert(gltex_init(&txt) == 0);
	gt = txt.data;
	assert(dlist_empty(&gt->atlases));
	for (k = 0; k < GLTEX_POOLS; k++)
		assert(dlist_empty(&gt->pools[k].lru));

	fail_display_use = true;
	assert(gltex_set(&txt) == -EFAULT);
	fail_display_use = false;
	uses = display_uses;
	gltex_release(&txt);
	assert(display_uses == uses);

	assert(gltex_set(&txt) == 0);
	gt->pool_max_glyphs = UINT_MAX;
	gt->pool_max_bytes = CACHE_BOUND;
	rnd_state = 7;
	n = random_frame(cells, 40);
	cache_frame(&txt, cells, n);
	assert_as_cold(&txt, cells, n);
	close_cache(&txt);
}

/* the four pools' textures, and whether any is past @max[] */
static size_t pools_bytes(struct gltex *gt, const size_t *max)
{
	size_t bytes = 0;
	unsigned int k;

	for (k = 0; k < GLTEX_POOLS; k++) {
		assert(!max || gt->pools[k].bytes <= max[k]);
		bytes += gt->pools[k].bytes;
	}
	return bytes;
}

/*
 * An adversarial pattern: every cell of every frame a glyph never drawn
 * before, as a program writing a new cluster to each cell each frame would:
 * colour emoji two cells wide, then clusters of combining marks one cell
 * wide, then both. A pool grows past full only by what one frame draws of
 * its kind, so after every frame each pool holds at most its bound or one
 * screen of its kind, whichever is more, and the hard stop set at their sum,
 * the worst case the cache's comment gives, is never reached: every cell
 * draws its own glyph.
 */
static void test_cache_adversarial(void)
{
	static struct gcell cells[FAKE_COLS * FAKE_ROWS];
	static const size_t strip[] = {1024, 1024, 4096, 4096};
	size_t worst[GLTEX_POOLS], sum = 0, screen;
	struct kmscon_text txt;
	struct gltex *gt;
	uint64_t cluster = TSM_FRAME_CLUSTER_KEY;
	unsigned int f, i, k, n, x, emoji = 0, w;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (k = 0; k < GLTEX_POOLS; k++) {
		/* strips for a screen of this kind's glyphs, against the bound's */
		w = k % 2 + 1;
		screen = (FAKE_COLS / w * FAKE_ROWS + 8 / w - 1) / (8 / w) * strip[k];
		worst[k] = screen > CACHE_BOUND
				   ? screen
				   : (CACHE_BOUND + strip[k] - 1) / strip[k] * strip[k];
		sum += worst[k];
	}
	gt->cache_hard_bytes = sum;

	for (f = 0; f < 300; f++) {
		for (i = n = 0; i < FAKE_ROWS; i++)
			for (x = 0; x < FAKE_COLS; x += w) {
				bool colour = f % 3 == 0 || (f % 3 == 2 && x % 3 == 0);

				w = colour ? 2 : 1;
				if (x + w > FAKE_COLS)
					break;
				if (colour)
					cells[n++] =
						(struct gcell){EMOJI_NARROW_CP + emoji++ % 3000, 2};
				else
					cells[n++] = (struct gcell){cluster++, 1};
			}
		cache_frame(&txt, cells, n);
		assert(txt.frame_misses == n);
		assert(pools_bytes(gt, worst) <= gt->cache_hard_bytes);
	}
	assert(!gt->hard_stop_logged);
	close_cache(&txt);
}

/*
 * What a pool takes past full it gives back once no frame draws from it. A
 * screen of 1200 distinct CJK takes 300 strips where the bound is two; the
 * next frame draws four other glyphs, into the least recently drawn slots,
 * and the strips stay while that frame is the last; at the frame after it
 * the pool is back to its two strips, and the four still draw from the
 * cache.
 */
static void test_cache_give_back(void)
{
	static struct gcell cells[FAKE_COLS / 2 * FAKE_ROWS];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int i, n = FAKE_COLS / 2 * FAKE_ROWS;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (i = 0; i < n; i++)
		cells[i] = (struct gcell){WIDE_CP + i, 2};
	cache_frame(&txt, cells, n);
	assert(gt->pools[1].bytes == (size_t)n / 4 * 1024);

	for (i = 0; i < 4; i++)
		cells[i] = (struct gcell){WIDE_CP + n + i, 2};
	cache_frame(&txt, cells, 4);
	assert(gt->pools[1].bytes == (size_t)n / 4 * 1024);
	cache_frame(&txt, cells, 4);
	assert(txt.frame_misses == 0);
	assert(gt->pools[1].bytes == CACHE_BOUND && gt->pools[1].slots == WIDE_CAP);
	assert_resident(gt, cells, 4);
	assert_as_cold(&txt, cells, 4);
	close_cache(&txt);
}

/*
 * A session leaving the foreground (gltex_release()) frees the glyph cache
 * with its pictures: every strip, the sprites' too, and every glyph. Back in
 * the foreground its next frame draws its glyphs again, sprites and all, as
 * from an empty cache.
 */
static void test_cache_release(void)
{
	struct gcell cells[41];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int f, n = 0, k, uploads;

	rnd_state = 11;
	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	cells[0] = (struct gcell){'A', 1, TSM_FRAME_STRIKE, TSM_UNDERLINE_SINGLE};
	for (f = 0; f < 50; f++) {
		n = random_frame(&cells[1], 40);
		cache_frame(&txt, cells, n + 1);
	}
	uploads = strip_uploads;

	gltex_release(&txt);
	assert(dlist_empty(&gt->atlases) && !gt->deco_atlas && !gt->glyphs.elems);
	for (k = 0; k < GLTEX_POOLS; k++)
		assert(dlist_empty(&gt->pools[k].lru) && !gt->pools[k].slots &&
		       !gt->pools[k].bytes);

	cache_frame(&txt, cells, n + 1);
	assert(txt.frame_misses > 0 && strip_uploads == uploads + 1);
	assert_as_cold(&txt, cells, n + 1);
	close_cache(&txt);
}

/*
 * The hard stop: with the whole cache allowed one strip past the alpha
 * pool's bound, a frame of 30 distinct glyphs draws the 24 that fit, each its
 * own, and the other 6 blank, the sprites' blank cell, whose texels are
 * clear, with the cell's background and decorations; the cache never passes
 * the stop, which is logged in the first frame, and again 600 frames on.
 */
static void test_cache_hard_stop(void)
{
	struct tsm_screen_attr attr = {0};
	struct tsm_frame_cell fc = {.fg = {200, 200, 200}, .bg = {0, 0, 40}, .width = 1};
	struct kmscon_text txt;
	struct gltex *gt;
	struct gcell cells[30];
	const struct fake_tex *t;
	unsigned int f, i, x, y;
	int r;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	gt->cache_hard_bytes = CACHE_BOUND + 1024;
	fc.underline = TSM_UNDERLINE_SINGLE;
	for (i = 0; i < 30; i++)
		cells[i] = (struct gcell){NARROW_CP + i, 1};

	for (f = 0; f <= GLTEX_HARD_STOP_LOG_FRAMES; f++) {
		assert(gltex_prepare(&txt, &attr) == 0);
		for (i = 0; i < 30; i++) {
			r = gltex_draw_cell(&txt, &fc, cells[i].key, i, 0, 1, false);
			assert(r == (i < 24 ? 0 : -ENOMEM));
		}
		assert(gltex_render(&txt) == 0);
		assert(gt->quads_num == 30);
		for (i = 0; i < 24; i++)
			assert(slot_of(&gt->quads[i])->key.key == cells[i].key);
		for (i = 24; i < 30; i++) {
			const struct gltex_quad *q = &gt->quads[i];

			assert(q->atlas == gt->deco_atlas && q->texoff == KMSCON_DECO_NONE);
			assert(colour_eq(q->bg, fc.bg) && q->ulcell == KMSCON_DECO_SINGLE);
		}
		assert(pools_bytes(gt, NULL) <= gt->cache_hard_bytes);
		/* frame_count, plus one, of the frame that last logged it */
		assert(gt->hard_stop_logged == (f < GLTEX_HARD_STOP_LOG_FRAMES ? 2 : 602));
	}
	t = &texs[gt->deco_atlas->tex];
	for (y = 0; y < FAKE_CELL_H; y++)
		for (x = 0; x < FAKE_CELL_W; x++)
			assert(!t->data[y * t->w + KMSCON_DECO_NONE * FAKE_CELL_W + x]);
	close_cache(&txt);
}

/*
 * The hard stop counts a pool's first strip too: with the alpha pool at its
 * bound and the stop less than a colour strip above it, a colour glyph, the
 * first of its pool, is drawn blank, and its pool takes no strip.
 */
static void test_cache_hard_stop_first(void)
{
	struct tsm_screen_attr attr = {0};
	struct tsm_frame_cell fc = {.fg = {200, 200, 200}, .bg = {0, 0, 40}, .width = 1};
	struct gcell cells[NARROW_CAP];
	struct kmscon_text txt;
	struct gltex *gt;
	unsigned int i;

	open_cache(&txt, &fake_font, OR_NORMAL);
	gt = txt.data;
	for (i = 0; i < NARROW_CAP; i++)
		cells[i] = (struct gcell){NARROW_CP + i, 1};
	cache_frame(&txt, cells, NARROW_CAP);
	assert(pools_bytes(gt, NULL) == CACHE_BOUND);
	gt->cache_hard_bytes = CACHE_BOUND + 4096 - 1;

	assert(gltex_prepare(&txt, &attr) == 0);
	assert(gltex_draw_cell(&txt, &fc, EMOJI_NARROW_CP, 0, 0, 1, false) == -ENOMEM);
	assert(!gt->pools[2].bytes && dlist_empty(&gt->pools[2].lru));
	assert(gt->hard_stop_logged);
	assert(gltex_render(&txt) == 0);
	close_cache(&txt);
}

/*
 * Not run by default: GLTEX_MEASURE=1, or GLTEX_CACHE_TEST=measure, prints
 * what a schedule of frames at 7680x2160 in 19x41 cells (font size 30; 404x52
 * cells, 202 wide glyphs a line) costs the cache: under 4 MiB a pool and no
 * glyph count, the first bound tried, and under the defaults. A seeded text
 * of 6000 CJK ideographs, or of 1400 colour emoji, the first commoner by
 * Zipf's law, is scrolled a line a frame for 600 frames, paged down a screen
 * a frame for 100 and back up for 100. Misses are glyphs drawn from the font,
 * each 21 to 33 us with FreeType.
 */
#define MEASURE_LINES 8000
#define MEASURE_MAX_COLS 256

static void measure(bool emoji, unsigned int glyphs, size_t max_bytes, const char *what)
{
	struct kmscon_font font = {.width = 19, .height = 41};
	static uint32_t text[MEASURE_LINES][MEASURE_MAX_COLS];
	static double cdf[6000];
	static struct gcell cells[MEASURE_MAX_COLS * 64];
	unsigned int distinct = emoji ? 1400 : 6000, i, j, f, top = 0, worst = 0, cols, rows;
	uint32_t base = emoji ? EMOJI_CP : WIDE_CP;
	uint64_t misses = 0, first = 0;
	struct kmscon_text txt;
	struct timespec t0, t1;
	struct gltex *gt;
	double sum = 0, u;

	for (i = 0; i < distinct; i++)
		sum += 1.0 / (i + 1);
	for (i = 0, u = 0; i < distinct; i++)
		cdf[i] = (u += 1.0 / (i + 1) / sum);
	rnd_state = 2026;
	for (i = 0; i < MEASURE_LINES; i++)
		for (j = 0; j < MEASURE_MAX_COLS; j++) {
			unsigned int lo = 0, hi = distinct - 1;

			u = rnd(1u << 24) / (double)(1u << 24);
			while (lo < hi) {
				unsigned int mid = (lo + hi) / 2;

				if (cdf[mid] < u)
					lo = mid + 1;
				else
					hi = mid;
			}
			text[i][j] = base + lo;
		}

	disp_w = 7680;
	disp_h = 2160;
	memset(&txt, 0, sizeof(txt));
	txt.font = &font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	patterned = true;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;
	gt->pool_max_glyphs = glyphs;
	gt->pool_max_bytes = max_bytes;
	cols = txt.cols / 2;
	rows = txt.rows;
	assert(cols <= MEASURE_MAX_COLS && cols * rows <= sizeof(cells) / sizeof(*cells));

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (f = 0; f < 800; f++) {
		if (f < 600)
			top = f;
		else if (f < 700)
			top += rows;
		else
			top -= rows;
		for (i = 0; i < rows; i++)
			for (j = 0; j < cols; j++)
				cells[i * cols + j] = (struct gcell){text[top + i][j], 2};
		cache_frame(&txt, cells, cols * rows);
		misses += txt.frame_misses;
		first = f ? first : misses;
		if (f && txt.frame_misses > worst)
			worst = txt.frame_misses;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	fprintf(stderr,
		"measure %s, %s: 800 frames, %" PRIu64 " misses (%" PRIu64
		" in the first, at most %u in a later one), %u slots in %zu KiB of strips; "
		"%.2f ms a frame here; at 21-33 us a miss, %.1f-%.1f ms of glyph drawing a frame\n",
		emoji ? "emoji" : "CJK", what, misses, first, worst, gt->pools[emoji ? 3 : 1].slots,
		gt->pools[emoji ? 3 : 1].bytes >> 10,
		((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 800,
		misses * 0.021 / 800, misses * 0.033 / 800);
	gltex_unset(&txt);
	gltex_destroy(&txt);
	patterned = false;
	disp_w = 640;
	disp_h = 480;
}

static void test_cache_measure(void)
{
	if (!getenv("GLTEX_MEASURE") && !getenv("GLTEX_CACHE_TEST"))
		return;
	measure(false, UINT_MAX, 4 << 20, "4 MiB a pool");
	measure(false, GLTEX_POOL_GLYPHS, GLTEX_CACHE_MAX_BYTES / GLTEX_POOLS, "the defaults");
	measure(true, UINT_MAX, 4 << 20, "4 MiB a pool");
	measure(true, GLTEX_POOL_GLYPHS, GLTEX_CACHE_MAX_BYTES / GLTEX_POOLS, "the defaults");
}

/* the glyph cache's tests, by name; GLTEX_CACHE_TEST=name runs that one alone */
static const struct {
	const char *name;
	void (*run)(void);
} cache_tests[] = {
	{"cold_oracle", test_cache_cold_oracle},
	{"model", test_cache_model},
	{"count_governs", test_cache_count_governs},
	{"ceiling_clamps", test_cache_ceiling_clamps},
	{"exact_capacity", test_cache_exact_capacity},
	{"capacity_plus_one", test_cache_capacity_plus_one},
	{"frame_overshoot", test_cache_frame_overshoot},
	{"same_frame", test_cache_same_frame},
	{"narrow_wide", test_cache_narrow_wide},
	{"colour_alpha", test_cache_colour_alpha},
	{"slot_cleared", test_cache_slot_cleared},
	{"deco_kept", test_cache_deco_kept},
	{"cluster_churn", test_cache_cluster_churn},
	{"reset", test_cache_reset},
	{"adversarial", test_cache_adversarial},
	{"give_back", test_cache_give_back},
	{"release", test_cache_release},
	{"hard_stop", test_cache_hard_stop},
	{"hard_stop_first", test_cache_hard_stop_first},
	{"upload_failure", test_cache_upload_failure},
	{"failed_rotate", test_cache_failed_rotate},
	{"unset_release", test_cache_unset_release},
	{"measure", test_cache_measure},
};

int main(void)
{
	const char *only = getenv("GLTEX_CACHE_TEST");
	unsigned int i;

	if (only) {
		for (i = 0; i < sizeof(cache_tests) / sizeof(*cache_tests); i++)
			if (!strcmp(only, cache_tests[i].name))
				cache_tests[i].run();
		return 0;
	}

	/* gltex draws image placements (ADR-103 steps 3f and 4e), and says so */
	assert(kmscon_text_gltex_ops.draws_images);
	test_vertex_memory();
	test_first_glyph();
	test_blink();
	test_decorations();
	test_underline_cursor();
	test_colour_glyph();
	test_strip_failure();
	test_glyph_fallback();
	test_image_texture();
	test_image_clip();
	test_image_scaled();
	test_image_evict();
	test_band_order();
	test_pointer_last();
	test_image_rotated();
	test_half_budget();
	test_upload_failure();
	test_grow_failure();
	test_padded_rows();
	test_source_clip();
	test_overlay_replaced();
	test_frame_draws();
	test_budget_full();
	test_atlas_sampling();
	for (i = 0; i < sizeof(cache_tests) / sizeof(*cache_tests); i++)
		cache_tests[i].run();
	return 0;
}
