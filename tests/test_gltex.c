/*
 * The gltex glyph cache stays bounded. A cluster's frame key carries the
 * screen's generation, so a screen that re-creates its clusters asks for new
 * keys every frame; the atlases must not grow with them. Its memory stays
 * linear in the cell count at a 4K cell grid. The GL stubs hold gltex to the
 * GLES 2 texture rules a real driver enforces (#17). Blinking cells lose their glyph
 * in the hidden phase. Decorations are the real decoration sprites (deco.c),
 * drawn in their own colours, and colour glyphs go to an RGBA atlas drawn
 * untinted (ADR-102 step 6). The GL and display calls are stubs: the test
 * counts the textures gltex creates, keeps the pixels it uploads, records each
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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
} texs[4096], strays[16];
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
	} else if (format == GL_RGBA) {
		/* filled by glTexSubImage2D, as a picture with padded rows is */
		t->data = calloc((size_t)width * height, 4);
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
	if (t->data && format == GL_RGBA)
		for (GLsizei y = 0; y < height; y++)
			memcpy(&t->data[((size_t)(yoffset + y) * t->w + xoffset) * 4],
			       (const uint8_t *)pixels + (size_t)y * width * 4, (size_t)width * 4);
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
int gl_shader_new(struct gl_shader **out, const char *vert, int vert_len, const char *frag,
		  int frag_len, char **attr, size_t attr_count)
{
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
/* as shl's: bound, and filtered GL_LINEAR */
void gl_tex_new(GLuint *tex, size_t num)
{
	for (size_t i = 0; i < num; i++) {
		tex[i] = ++n_textures;
		assert(tex[i] < sizeof(texs) / sizeof(*texs));
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
int display_use(struct display *disp)
{
	return 0;
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
 * A blank mask, or for COLOUR_CP an ARGB glyph, as the font gives a colour
 * emoji (ADR-102 step 4c).
 */
struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, struct kmscon_font_attr *attr,
					const uint32_t *cps, size_t len, unsigned int cwidth)
{
	unsigned int w = cwidth * FAKE_CELL_W, x, y;
	struct kmscon_glyph *g;

	n_rendered++;
	if (len == 1 && cps[0] == FAIL_CP)
		return NULL;
	if (len == 1 && cps[0] == COLOUR_CP) {
		uint32_t *px;

		g = calloc(1, sizeof(*g) + w * FAKE_CELL_H * 4);
		g->buf.format = VIDEO_FORMAT_ARGB32;
		px = (uint32_t *)g->buf.data;
		for (y = 0; y < FAKE_CELL_H; y++)
			for (x = 0; x < w; x++)
				px[y * w + x] = COLOUR_PX(x, y);
	} else {
		g = calloc(1, sizeof(*g) + w * FAKE_CELL_H);
	}
	g->buf.width = w;
	g->buf.height = FAKE_CELL_H;
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
/* a codepoint names itself; every cluster is 'c' */
const uint32_t *kmscon_text_get_cluster(struct kmscon_text *txt, uint64_t key, uint32_t *one,
					size_t *len)
{
	*one = key < TSM_FRAME_CLUSTER_KEY ? key : 'c';
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

static unsigned int count_atlases(struct gltex *gt)
{
	struct atlas *atlas;
	unsigned int n = 0;

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		n++;
	}
	return n;
}

/*
 * One frame: @n glyphs under fresh cluster keys starting at @next (a screen
 * that re-created its clusters), plus 'A', which is the same every frame. No
 * two glyphs of the frame may share an atlas cell.
 */
static void draw_frame(struct kmscon_text *txt, uint64_t *next, unsigned int n)
{
	static struct gl_glyph *seen[FAKE_COLS * FAKE_ROWS + 1];
	struct tsm_screen_attr attr = {0};
	struct gl_glyph *a, *g;
	unsigned int i, j;

	assert(gltex_prepare(txt, &attr) == 0);

	a = find_glyph(txt, 'A', 0, 1);
	assert(a);
	seen[0] = a;
	for (i = 1; i <= n; i++) {
		g = find_glyph(txt, (*next)++, 0, 1 + i % 2);
		assert(g);
		assert(g->texoff + (g->double_width ? 2 : 1) <= g->atlas->count);
		for (j = 0; j < i; j++)
			assert(seen[j]->atlas != g->atlas || seen[j]->texoff != g->texoff);
		seen[i] = g;
	}
	/* a second look within the frame is a hit */
	j = n_rendered;
	assert(find_glyph(txt, 'A', 0, 1) == a);
	assert(n_rendered == j);
}

static void test_bounded(void)
{
	struct kmscon_text txt = {0};
	struct gltex *gt;
	uint64_t next = TSM_FRAME_CLUSTER_KEY;
	unsigned int i;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;

	/* 80x30 cells, so 4 * 2401 cells between flushes */
	assert(gt->max_cells == 9604);

	for (i = 0; i < 2000; i++)
		draw_frame(&txt, &next, FAKE_COLS);

	/*
	 * 2000 frames of 80 new glyphs, alternately one and two cells wide, hand
	 * out 240000 cells; unbounded that is over 900 atlases of 256 cells. A
	 * flush comes once more than 9604 cells are out, so at most 9604 plus
	 * one frame's 121 are; with one cell lost at the end of each atlas to a
	 * glyph two cells wide, they fit in 39 atlases.
	 */
	fprintf(stderr, "atlases %u, textures %u\n", count_atlases(gt), n_textures);
	assert(count_atlases(gt) <= 39);
	assert(n_textures == count_atlases(gt));
	assert(gt->cells <= 9604 + 121);

	gltex_unset(&txt);
	gltex_destroy(&txt);
}

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
 * room check, and that plant fails here (#180). Today the queue follows the
 * head, and the old lookup alone fails test_bounded instead.
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
	assert(g->texoff == 0 && g->atlas->fill == 1);

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
		if (uploads[j].tex == gt->deco_atlas->tex &&
		    uploads[j].x == (GLint)(gt->deco_off * FAKE_CELL_W))
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
			assert(q->ulcell == gt->deco_off + want[i].ul);
			strike = want[i].strike ? KMSCON_DECO_STRIKE : KMSCON_DECO_NONE;
			assert(q->strikecell == gt->deco_off + strike);
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
	assert(q->ulcell == gt->deco_off + KMSCON_DECO_SINGLE);
	assert(colour_eq(q->ul, q->fg) && !colour_eq(q->ul, txt.deco.ul));
	/* the cell before it is no cursor and has no underline */
	assert(quad_at(gt, 0)->ulcell == gt->deco_off + KMSCON_DECO_NONE);

	gltex_unset(&txt);
	gltex_destroy(&txt);
}

/*
 * A flush drops the decoration sprites with the glyphs, as their cells are
 * refilled; the next frame uploads them again, counted against the cache
 * bound, and its quads name the new cells.
 */
static void test_deco_flush(void)
{
	static const char bytes[] = "\x1b[?25l\x1b[4mA";
	struct kmscon_text txt = {0};
	struct gltex *gt;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);
	gt = txt.data;

	draw_bytes(&txt, bytes, sizeof(bytes) - 1, false);
	assert(gt->deco_atlas);
	gltex_flush(&txt);
	assert(!gt->deco_atlas && !gt->cells);

	keep_uploads = true;
	draw_bytes(&txt, bytes, sizeof(bytes) - 1, false);
	keep_uploads = false;
	assert_sprites(gt);
	assert(gt->cells >= KMSCON_DECO_COUNT);
	assert(quad_at(gt, 0)->ulcell == gt->deco_off + KMSCON_DECO_SINGLE);
	free_uploads();

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
	assert(q->ulcell == gt->deco_off + KMSCON_DECO_SINGLE);
	assert(q->strikecell == gt->deco_off + KMSCON_DECO_STRIKE);
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
		assert(q->ulcell == gt->deco_off + KMSCON_DECO_SINGLE);
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

int main(void)
{
	/* gltex draws image placements (ADR-103 steps 3f and 4e), and says so */
	assert(kmscon_text_gltex_ops.draws_images);
	test_bounded();
	test_vertex_memory();
	test_first_glyph();
	test_blink();
	test_decorations();
	test_underline_cursor();
	test_deco_flush();
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
	return 0;
}
