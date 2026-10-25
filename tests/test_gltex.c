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
 */

#include <GLES2/gl2.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/font/font.h"
#include "../src/render/deco.h"
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
	"cos",	  "sin",  "projection", "atlas",     "advance_htex", "advance_vtex",
	"colour", "deco", "deco_htex",	"deco_vtex", "decorated",
};
#define N_UNIFORMS (sizeof(uniform_names) / sizeof(*uniform_names))
static GLint uniform_i[N_UNIFORMS];
static GLfloat uniform_f[N_UNIFORMS];
enum {
	U_ATLAS = 3,
	U_ADVANCE_H = 4,
	U_ADVANCE_V = 5,
	U_COLOUR = 6,
	U_DECO = 7,
	U_DECO_H = 8,
	U_DECO_V = 9,
	U_DECORATED = 10,
};

/* a draw: the textures on units 0 and 1, and the uniforms it was drawn with */
static struct draw_call {
	GLuint tex, deco_tex;
	GLint first;
	GLsizei count;
	GLint colour, atlas_unit, deco_unit;
	GLfloat advance_h, advance_v, deco_h, deco_v, decorated;
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
} texs[4096], strays[16];
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
void glClear(GLbitfield mask) {}
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {}
void glDisable(GLenum cap) {}
void glDisableVertexAttribArray(GLuint index) {}
void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	assert(n_draws < sizeof(draws) / sizeof(*draws));
	draws[n_draws++] = (struct draw_call){
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
/* GL_MAX_TEXTURE_SIZE, which a test may lower */
static GLint max_texture_size = 2048;
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
	assert(width > 0 && width <= 2048 && height > 0 && height <= 2048);
	t->w = width;
	t->h = height;
	t->format = format;
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
int gl_shader_new(struct gl_shader **out, const char *vert, int vert_len, const char *frag,
		  int frag_len, char **attr, size_t attr_count)
{
	*out = (struct gl_shader *)0x1;
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
void gl_shader_use(struct gl_shader *shader) {}
void gl_tex_new(GLuint *tex, size_t num)
{
	for (size_t i = 0; i < num; i++) {
		tex[i] = ++n_textures;
		assert(tex[i] < sizeof(texs) / sizeof(*texs));
		texs[tex[i]] = (struct fake_tex){.name = tex[i], .live = true};
	}
}
void gl_tex_free(GLuint *tex, size_t num)
{
	for (size_t i = 0; i < num; i++) {
		assert(texs[tex[i]].live);
		texs[tex[i]].live = false;
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
 * gltex_render() drew every queued quad once, from its own atlas: each atlas
 * with quads in one draw over its slots of the shared vertex arrays, which
 * hold its quads in the order they were queued. Each draw has its atlas on
 * unit 0, sampled untinted when it is an RGBA atlas, and the decoration
 * sprites' atlas on unit 1.
 */
static void check_render(struct gltex *gt)
{
	struct atlas *atlas;
	unsigned int i, n = 0, total = 0;

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
	assert(n == n_draws);
	assert(total == gt->quads_num);

	for (i = 0; i < gt->quads_num; i++) {
		const struct gltex_quad *q = &gt->quads[i];
		unsigned int slot = q->atlas->cache_off + q->atlas->cache_num++;

		assert(gt->cache_pos[slot * 12] == q->x1 && gt->cache_pos[slot * 12 + 3] == q->y2);
		assert(gt->cache_texpos[slot * 12] == q->texoff);
		assert(gt->cache_texpos[slot * 12 + 4] == q->texoff + q->texw);
		assert(gt->cache_fgcol[slot * 18] == (GLfloat)(q->fg.r / 255.0));
		assert(gt->cache_bgcol[slot * 18 + 17] == (GLfloat)(q->bg.b / 255.0));
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

int main(void)
{
	/* gltex draws no image placements until ADR-103 step 3f, and says so */
	assert(!kmscon_text_gltex_ops.draws_images);
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
	return 0;
}
