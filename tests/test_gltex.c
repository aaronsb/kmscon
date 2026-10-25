/*
 * The gltex glyph cache stays bounded. A cluster's frame key carries the
 * screen's generation, so a screen that re-creates its clusters asks for new
 * keys every frame; the atlases must not grow with them. Its memory stays
 * linear in the cell count at a 4K cell grid. The GL stubs hold gltex to the
 * GLES 2 texture rules a real driver enforces (#17). Blinking cells lose their glyph
 * in the hidden phase. Decorations and colour glyphs reach the atlas as masks,
 * from the real decoration sprites (deco.c). The GL and display calls are
 * stubs: the test counts the textures gltex creates, keeps the masks it
 * uploads, and checks where its glyphs land in them and in the frame. We
 * include the implementation to reach its statics.
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
static GLuint bound_tex;	/* the texture glBindTexture last bound */
static struct draw_call {
	GLuint tex;
	GLint first;
	GLsizei count;
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
	bool live;    /* generated and not deleted, or bound as a stray */
	GLsizei w, h; /* its storage; 0 before glTexImage2D */
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

/* the glyph masks uploaded while keep_uploads is set */
static bool keep_uploads;
static struct upload {
	GLuint tex;
	GLint x;
	GLsizei w, h;
	uint8_t *data;
} uploads[64];
static unsigned int n_uploads;

/* ---- GL ---- */
void glActiveTexture(GLenum texture) {}
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
	draws[n_draws++] = (struct draw_call){bound_tex, first, count};
}
void glEnableVertexAttribArray(GLuint index) {}
GLenum glGetError(void)
{
	return GL_NO_ERROR;
}
void glGetIntegerv(GLenum pname, GLint *data)
{
	*data = 2048;
}
void glPixelStorei(GLenum pname, GLint param) {}
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
		  GLint border, GLenum format, GLenum type, const void *pixels)
{
	struct fake_tex *t = tex_of(bound_tex);

	assert(level == 0);
	assert(internalformat == GL_ALPHA && format == GL_ALPHA && type == GL_UNSIGNED_BYTE);
	assert(width > 0 && width <= 2048 && height > 0 && height <= 2048);
	t->w = width;
	t->h = height;
}
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
		     GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	const struct fake_tex *t = tex_of(bound_tex);
	struct upload *u;

	assert(t->live && t->w && level == 0 && format == GL_ALPHA && type == GL_UNSIGNED_BYTE);
	assert(xoffset >= 0 && yoffset >= 0 && width > 0 && height > 0);
	assert(xoffset + width <= t->w && yoffset + height <= t->h);
	if (!keep_uploads)
		return;
	assert(format == GL_ALPHA && type == GL_UNSIGNED_BYTE && yoffset == 0);
	assert(n_uploads < sizeof(uploads) / sizeof(*uploads));
	u = &uploads[n_uploads++];
	*u = (struct upload){bound_tex, xoffset, width, height, malloc(width * height)};
	memcpy(u->data, pixels, width * height);
}
void glUniform1f(GLint location, GLfloat v0) {}
void glUniform1i(GLint location, GLint v0) {}
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
/* the colour glyph's pixel at @x, @y: its alpha varies, its colour does not */
#define COLOUR_CP 0x1f600
#define COLOUR_PX(x, y) ((uint32_t)((x) * 16 + (y)) << 24 | 0xff8000)

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
struct tsm_screen_color kmscon_text_ul_color(struct kmscon_text *txt,
					     const struct tsm_frame_cell *cell)
{
	return cell->fg;
}
struct tsm_screen_color kmscon_text_strike_color(struct kmscon_text *txt,
						 const struct tsm_frame_cell *cell)
{
	return cell->fg;
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

	a = find_glyph(txt, 'A', 0, 1, 0, false);
	assert(a);
	seen[0] = a;
	for (i = 1; i <= n; i++) {
		g = find_glyph(txt, (*next)++, 0, 1 + i % 2, 0, false);
		assert(g);
		assert(g->texoff + (g->double_width ? 2 : 1) <= g->atlas->count);
		for (j = 0; j < i; j++)
			assert(seen[j]->atlas != g->atlas || seen[j]->texoff != g->texoff);
		seen[i] = g;
	}
	/* a second look within the frame is a hit */
	j = n_rendered;
	assert(find_glyph(txt, 'A', 0, 1, 0, false) == a);
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
 * hold its quads in the order they were queued.
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
				assert(gltex_draw_cell(&txt, &cell, next++, x, y, 1) == 0);
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

	want = find_glyph(txt, key, fc->flags, fc->width == 2 ? 2 : 1, fc->underline,
			  fc->flags & TSM_FRAME_STRIKE);
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

	g = find_glyph(&txt, 'A', 0, 1, 0, false);
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

/*
 * @mask is the blank glyph with the sprites of @a and @b (KMSCON_DECO_NONE:
 * none) baked in, the cell's sprite repeated across a wide glyph.
 */
static void assert_mask(const struct upload *mask, enum kmscon_deco a, enum kmscon_deco b)
{
	struct kmscon_glyph *sa = kmscon_deco_render(a, FAKE_CELL_W, FAKE_CELL_H, &fake_deco);
	struct kmscon_glyph *sb = kmscon_deco_render(b, FAKE_CELL_W, FAKE_CELL_H, &fake_deco);
	unsigned int x, y, inked = 0;

	assert(sa && mask->h == FAKE_CELL_H);
	for (y = 0; y < FAKE_CELL_H; y++)
		for (x = 0; x < (unsigned int)mask->w; x++) {
			uint8_t va = sa->buf.data[y * FAKE_CELL_W + x % FAKE_CELL_W];
			uint8_t vb = sb ? sb->buf.data[y * FAKE_CELL_W + x % FAKE_CELL_W] : 0;
			uint8_t want = va > vb ? va : vb;

			assert(mask->data[y * mask->w + x] == want);
			inked += want != 0;
		}
	/* a sprite that drew nothing would pass the loop above */
	assert(inked >= (unsigned int)mask->w / 2);
	free(sa);
	free(sb);
}

/* parse @bytes into a fresh screen and draw its frame */
static void draw_bytes(struct kmscon_text *txt, const char *bytes, size_t len)
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
	assert(gltex_draw(txt, frame, false) == 0);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

static void free_uploads(void)
{
	while (n_uploads)
		free(uploads[--n_uploads].data);
}

/*
 * #116: decorations as gltex draws them until it composes decoration sprites
 * itself (ADR-102 step 6): deco.c's sprites baked into the glyph's mask and
 * drawn in the cell's foreground. Every underline style, strike, both at
 * once, SGR 58's coloured underline and a wide underlined cell. The
 * configured decoration colours, forced or not, leave the tint at the
 * foreground; step 6 is what makes them change it.
 */
static void test_decorations(void)
{
	static const char bytes[] = "\x1b[?25l\x1b[38;2;10;20;30m"
				    "\x1b[4:1mA\x1b[4:2mB\x1b[4:3mC\x1b[4:4mD\x1b[4:5mE"
				    "\x1b[4:0;9mF\x1b[4:1;9mG\x1b[29;4:1;58:2::255:0:0mH"
				    "\x1b[59m\xe6\x97\xa5\x1b[0mI";
	/* the sprites in each cell's mask, one column a cell, the wide one two */
	static const enum kmscon_deco want[][2] = {
		{KMSCON_DECO_SINGLE, KMSCON_DECO_NONE},	  {KMSCON_DECO_DOUBLE, KMSCON_DECO_NONE},
		{KMSCON_DECO_CURLY, KMSCON_DECO_NONE},	  {KMSCON_DECO_DOTTED, KMSCON_DECO_NONE},
		{KMSCON_DECO_DASHED, KMSCON_DECO_NONE},	  {KMSCON_DECO_STRIKE, KMSCON_DECO_NONE},
		{KMSCON_DECO_SINGLE, KMSCON_DECO_STRIKE}, {KMSCON_DECO_SINGLE, KMSCON_DECO_NONE},
		{KMSCON_DECO_SINGLE, KMSCON_DECO_NONE},
	};
	static const struct kmscon_text_deco decos[] = {
		{.ul_fg = true, .strike_fg = true},
		{.ul = {0, 255, 0}, .strike = {0, 0, 255}},
		{.ul = {0, 255, 0}, .strike = {0, 0, 255}, .force = true},
	};
	const struct gltex_quad *q;
	const struct upload *mask;
	struct kmscon_text txt = {0};
	unsigned int d, i, col;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);

	for (d = 0; d < sizeof(decos) / sizeof(*decos); d++) {
		txt.deco = decos[d];
		/* the glyphs are new on the first pass only */
		keep_uploads = !d;
		draw_bytes(&txt, bytes, sizeof(bytes) - 1);
		keep_uploads = false;

		if (!d) {
			/* ten cells' glyphs and the blank of the empty cells */
			assert(n_uploads == 11);
			for (i = 0, col = 0; i < sizeof(want) / sizeof(*want); i++, col++) {
				mask = mask_at(txt.data, col, &q);
				assert(mask->w == (i == 8 ? 2 : 1) * FAKE_CELL_W);
				assert_mask(mask, want[i][0], want[i][1]);
				if (i == 8)
					col++; /* the wide cell's trailing half */
			}
			/* after SGR 0, no decoration: a blank mask */
			mask = mask_at(txt.data, col, &q);
			for (i = 0; i < FAKE_CELL_W * FAKE_CELL_H; i++)
				assert(!mask->data[i]);
		}
		/* every decorated cell drawn in its foreground */
		for (col = 0; col < 9; col++) {
			q = quad_at(txt.data, col);
			assert(q->fg.r == 10 && q->fg.g == 20 && q->fg.b == 30);
		}
	}
	free_uploads();

	gltex_unset(&txt);
	gltex_destroy(&txt);
}

/*
 * #116: a colour glyph from the font, ARGB, goes into the alpha atlas as its
 * alpha and is drawn in the cell's foreground, until gltex has an RGBA atlas
 * (ADR-102 step 6, #68).
 */
static void test_colour_glyph(void)
{
	static const char bytes[] = "\x1b[?25l\x1b[38;2;40;50;60m\xf0\x9f\x98\x80";
	const struct gltex_quad *q;
	const struct upload *mask;
	struct kmscon_text txt = {0};
	unsigned int x, y;

	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	assert(gltex_init(&txt) == 0);
	assert(gltex_set(&txt) == 0);

	keep_uploads = true;
	draw_bytes(&txt, bytes, sizeof(bytes) - 1);
	keep_uploads = false;

	mask = mask_at(txt.data, 0, &q);
	assert(mask->w == 2 * FAKE_CELL_W && mask->h == FAKE_CELL_H);
	for (y = 0; y < FAKE_CELL_H; y++)
		for (x = 0; x < 2 * FAKE_CELL_W; x++)
			assert(mask->data[y * mask->w + x] == COLOUR_PX(x, y) >> 24);
	assert(q->texw == 2.0);
	assert(q->fg.r == 40 && q->fg.g == 50 && q->fg.b == 60);
	free_uploads();

	gltex_unset(&txt);
	gltex_destroy(&txt);
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
	test_colour_glyph();
	return 0;
}
