/*
 * The gltex glyph cache stays bounded. A cluster's frame key carries the
 * screen's generation, so a screen that re-creates its clusters asks for new
 * keys every frame; the atlases must not grow with them. Its memory stays
 * linear in the cell count at a 4K cell grid. Blinking cells lose their glyph
 * in the hidden phase. The GL and display calls are stubs: the test counts
 * the textures gltex creates and checks where its glyphs land in them and in
 * the frame. We include the implementation to reach its statics.
 */

#include <GLES2/gl2.h>
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/font/font.h"
#include "../src/render/deco.h"
#include "../src/render/text.h"
#include "../src/shl/gl.h"
#include "../src/video/video.h"

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

/* ---- GL ---- */
void glActiveTexture(GLenum texture) {}
void glBindTexture(GLenum target, GLuint texture)
{
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
}
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
		     GLsizei height, GLenum format, GLenum type, const void *pixels)
{
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
	*tex = ++n_textures;
}
void gl_tex_free(GLuint *tex, size_t num) {}

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
struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, struct kmscon_font_attr *attr,
					const uint32_t *cps, size_t len, unsigned int cwidth)
{
	struct kmscon_glyph *g = calloc(1, sizeof(*g) + cwidth * FAKE_CELL_W * FAKE_CELL_H);

	g->buf.width = cwidth * FAKE_CELL_W;
	g->buf.height = FAKE_CELL_H;
	g->double_width = cwidth == 2;
	n_rendered++;
	return g;
}
bool kmscon_font_has_glyph(struct kmscon_font *font, struct kmscon_font_attr *attr,
			   const uint32_t ch)
{
	return true;
}
void kmscon_font_get_deco(struct kmscon_font *font, struct kmscon_font_deco *out)
{
	memset(out, 0, sizeof(*out));
}
struct kmscon_glyph *kmscon_deco_render(enum kmscon_deco kind, unsigned int w, unsigned int h,
					const struct kmscon_font_deco *m)
{
	return NULL;
}
const uint32_t *kmscon_text_get_cluster(struct kmscon_text *txt, uint64_t key, uint32_t *one,
					size_t *len)
{
	*one = 'c';
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
	struct mallinfo2 mi = mallinfo2();

	return mi.uordblks + mi.hblkhd;
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

	fprintf(stderr, "4K grid: %zu cells, at most %zu bytes held, %zu per cell\n", cells, peak,
		peak / cells);
	assert(peak <= cells * 1024);

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

int main(void)
{
	/* gltex draws no image placements until ADR-103 step 3f, and says so */
	assert(!kmscon_text_gltex_ops.draws_images);
	test_bounded();
	test_vertex_memory();
	test_blink();
	return 0;
}
