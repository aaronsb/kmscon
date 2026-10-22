/*
 * Tests for the freetype font backend's monospace fontconfig filter.
 *
 * Background: there is no monospace Vazirmatn font, yet on an Arabic locale
 * fontconfig happily offers the proportional Vazirmatn for a "monospace"
 * request (reproduce with `fc-match monospace:lang=ar`). A proportional font
 * used as a glyph fallback breaks column alignment in a cell-based terminal.
 *
 * The backend requests monospace spacing on the pattern AND installs a
 * fontconfig filter (_fc_monospace_font_filter_func). The per-pattern request
 * only re-ranks fonts (proportional ones still sit in the sorted fallback set
 * and can be picked for a glyph the top monospace font lacks); the filter is
 * what removes proportional fonts from the set entirely. That set-wide property
 * is the real guarantee and the one worth testing.
 *
 * Two levels:
 *   1. test_filter_unit          - the filter predicate in isolation.
 *   2. test_monospace_fallback_set - exercise kmscon the way it really runs:
 *      register the backend and kmscon_font_find() it through the
 *      kmscon_font_ops callback table, then check that every font in the
 *      fallback set it built is monospace.
 *
 * The public font API does not expose the fallback set (callers only get
 * metrics and a glyph bitmap), so to inspect it we reach into the backend's
 * private ft_data (font->data). Both src files are #included directly (like
 * test_text.c) to get at that layout and the static filter.
 */

#include <assert.h>
#include <fontconfig/fontconfig.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "../src/font/font.c"	       /* real dispatch: register / find / render */
#undef LOG_SUBSYSTEM		       /* font.c and font_freetype.c both define it */
#include "../src/font/font_freetype.c" /* backend ops + ft_data layout */

#define ARABIC_ALEF 0x0627u

/* font.c pulls in shl/module.h; the backend registers with a NULL owner, so
 * module ref-counting is a no-op here. */
void shl_module_ref(struct shl_module *module) {}
void shl_module_unref(struct shl_module *module) {}

static bool spacing_is_monospace(int spacing)
{
	return spacing == FC_CHARCELL || spacing == FC_MONO || spacing == FC_DUAL;
}

/* --- 1. unit test of the filter predicate --- */

static FcPattern *pattern_with_spacing(int spacing)
{
	FcPattern *pat = FcPatternCreate();
	assert(pat);
	assert(FcPatternAddInteger(pat, FC_SPACING, spacing));
	return pat;
}

static void test_filter_unit(void)
{
	FcPattern *pat;

	/* Monospace spacings must be accepted. */
	pat = pattern_with_spacing(FC_CHARCELL);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcTrue);
	FcPatternDestroy(pat);

	pat = pattern_with_spacing(FC_MONO);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcTrue);
	FcPatternDestroy(pat);

	pat = pattern_with_spacing(FC_DUAL);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcTrue);
	FcPatternDestroy(pat);

	/* A proportional font (e.g. Vazirmatn) must be rejected. */
	pat = pattern_with_spacing(FC_PROPORTIONAL);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcFalse);
	FcPatternDestroy(pat);

	/* Proportional fonts commonly omit FC_SPACING entirely; reject those too
	 * (this is the real-world Vazirmatn shape). */
	pat = FcPatternCreate();
	assert(pat);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcFalse);
	FcPatternDestroy(pat);
}

/* --- 2. behavioral test through the public kmscon font API --- */

/* Build the pattern kmscon's prepare_font() uses for a "monospace" request. */
static FcPattern *kmscon_monospace_pattern(void)
{
	FcPattern *pat = FcNameParse((const FcChar8 *)KMSCON_FONT_DEFAULT_NAME);
	assert(pat);
	FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_NORMAL);
	FcPatternAddDouble(pat, FC_PIXEL_SIZE, 20.0);
	FcPatternAddInteger(pat, FC_SPACING, FC_CHARCELL);
	FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
	FcPatternAddInteger(pat, FC_SPACING, FC_DUAL);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	return pat;
}

/*
 * With no kmscon filter active, count the proportional fonts in the "monospace"
 * sorted set for the current locale. If @ch is non-zero, only count fonts that
 * cover @ch. This establishes that the unfiltered set really does contain
 * proportional fonts for the filter to remove -- otherwise the monospace
 * assertions below would hold trivially and guard nothing.
 *
 * Must be called before kmscon_font_find(), i.e. before the filter is installed.
 */
static int unfiltered_proportional_count(uint32_t ch)
{
	FcPattern *pat = kmscon_monospace_pattern();
	FcResult res;
	FcFontSet *fs = FcFontSort(NULL, pat, FcTrue, NULL, &res);
	int count = 0;

	for (int i = 0; fs && i < fs->nfont; i++) {
		int spacing = FC_PROPORTIONAL;

		if (ch) {
			FcCharSet *cs;
			if (FcPatternGetCharSet(fs->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch)
				continue;
			if (!FcCharSetHasChar(cs, ch))
				continue;
		}
		FcPatternGetInteger(fs->fonts[i], FC_SPACING, 0, &spacing);
		if (!spacing_is_monospace(spacing))
			count++;
	}
	if (fs)
		FcFontSetDestroy(fs);
	FcPatternDestroy(pat);
	return count;
}

static void test_monospace_fallback_set(void)
{
	struct kmscon_font *font = NULL;
	struct ft_data *ftd;
	FcFontSet *set;
	int arabic_fonts = 0;
	int prop_total, prop_arabic;
	int ret;

	/* Use an Arabic locale so the ordering matches the real bug scenario. */
	setlocale(LC_ALL, "ar_AE.UTF-8");

	/* Measure the unfiltered set *before* the filter is installed by find(). */
	prop_total = unfiltered_proportional_count(0);
	prop_arabic = unfiltered_proportional_count(ARABIC_ALEF);

	/* If no fonts are installed, don't go further in the tests */
	if (!prop_total || !prop_arabic)
		return;

	/* Register and look up the backend exactly as kmscon does at runtime;
	 * init() installs the monospace filter as a side effect. */
	ret = kmscon_font_register(&kmscon_font_freetype_ops);
	assert(ret == 0);
	ret = kmscon_font_find(&font, KMSCON_FONT_DEFAULT_NAME, 20, "freetype");
	assert(ret == 0);
	assert(font);
	assert(font->ops == &kmscon_font_freetype_ops); /* freetype really chosen */

	ftd = font->data;
	assert(ftd);
	set = ftd->regular.fc;
	assert(set);

	/*
	 * Core, language-agnostic guarantee: every font kmscon put in the fallback
	 * set carries a proper monospace FC_SPACING, no matter which script it
	 * covers. The proportional Vazirmatn (and every other proportional font) is
	 * therefore absent. Without the filter this set contains proportional fonts
	 * (prop_total of them) and these assertions fail.
	 */
	if (prop_total == 0)
		fprintf(stderr, "note: no proportional fonts installed; the monospace "
				"assertions hold trivially and guard nothing\n");
	for (int i = 0; i < set->nfont; i++) {
		FcChar8 *family = NULL;
		int spacing = -1;

		/* proper, present FC_SPACING, and it must be a monospace value */
		assert(FcPatternGetInteger(set->fonts[i], FC_SPACING, 0, &spacing) ==
		       FcResultMatch);
		assert(spacing_is_monospace(spacing));

		FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &family);
		assert(family);
		assert(strcasestr((const char *)family, "Vazirmatn") == NULL);

		FcCharSet *cs;
		if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch &&
		    FcCharSetHasChar(cs, ARABIC_ALEF))
			arabic_fonts++;
	}
	fprintf(stderr,
		"kmscon monospace fallback set: %d fonts, %d cover Arabic "
		"(unfiltered had %d proportional, %d covering Arabic)\n",
		set->nfont, arabic_fonts, prop_total, prop_arabic);

	kmscon_font_unref(font);
	kmscon_font_unregister(kmscon_font_freetype_ops.name);
}

/*
 * copy_mono clips a 1-bit bitmap to the cell. One that starts right of the cell
 * draws nothing, and nothing lands past the cell's buffer.
 */
static void test_copy_mono_clip(void)
{
	enum { W = 8, H = 16, GUARD = 64 };
	uint8_t bits[H * 2];
	FT_GlyphSlotRec slot = {0};
	struct video_buffer *buf;
	unsigned int i;

	memset(bits, 0xff, sizeof(bits));
	slot.bitmap.rows = H;
	slot.bitmap.width = W;
	slot.bitmap.pitch = 2;
	slot.bitmap.buffer = bits;
	slot.bitmap.pixel_mode = FT_PIXEL_MODE_MONO;
	slot.bitmap_top = H;

	buf = calloc(1, sizeof(*buf) + W * H + GUARD);
	assert(buf);
	buf->width = W;
	buf->height = H;

	/* starts two pixels past the right edge */
	slot.bitmap_left = W + 2;
	copy_mono(buf, &slot, H, 0);
	for (i = 0; i < W * H + GUARD; i++)
		assert(buf->data[i] == 0);

	/* a dx that pushes it past the edge clips the same way */
	slot.bitmap_left = 0;
	copy_mono(buf, &slot, H, W + 1);
	for (i = 0; i < W * H + GUARD; i++)
		assert(buf->data[i] == 0);

	/* in the cell it draws, and stays inside it */
	copy_mono(buf, &slot, H, 4);
	assert(buf->data[0] == 0 && buf->data[4] == 0xff && buf->data[W - 1] == 0xff);
	assert(buf->data[(H - 1) * W + 4] == 0xff);
	for (i = W * H; i < W * H + GUARD; i++)
		assert(buf->data[i] == 0);

	free(buf);
}

/*
 * A cluster's codepoint the face lacks is left out, not drawn as the face's
 * missing-glyph box: '1' with a keycap mark the face does not have draws as '1'
 * with a variation selector, which shaping hides.
 */
static void test_cluster_missing_mark(void)
{
	static const uint32_t keycap[] = {'1', 0x20e3};
	static const uint32_t plain[] = {'1', 0xfe0f};
	struct kmscon_font font = {.width = 9, .height = 18};
	struct kmscon_glyph *a, *b;
	FT_Library ft;
	FT_Face face;
	unsigned int i, ink = 0;

	assert(!FT_Init_FreeType(&ft));
	assert(!FT_New_Face(ft, TEST_FONT_DIR "/DejaVuSansMono.ttf", 0, &face));
	assert(!FT_Set_Pixel_Sizes(face, 0, 14));
	/* the premise: the face has no keycap mark */
	assert(!FT_Get_Char_Index(face, 0x20e3));

	a = render_cluster(&font, face, keycap, 2, 2);
	b = render_cluster(&font, face, plain, 2, 2);
	assert(a && b);
	assert(a->buf.width == b->buf.width && a->buf.height == b->buf.height);
	for (i = 0; i < b->buf.width * b->buf.height; i++)
		ink += b->buf.data[i] != 0;
	assert(ink > 0);
	assert(!memcmp(a->buf.data, b->buf.data, a->buf.width * a->buf.height));

	free(a);
	free(b);
	FT_Done_Face(face);
	FT_Done_FreeType(ft);
}

/*
 * a + U+0303 + U+0300: HarfBuzz composes atilde, and the face does not place
 * the grave on it. The grave is drawn above the tilde, over the base, and the
 * base and tilde stay where they are drawn without it. The cell is the one
 * kmscon uses for this face at 14 px.
 */
static void test_cluster_stacked_mark(void)
{
	static const uint32_t tilde[] = {'a', 0x303};
	static const uint32_t both[] = {'a', 0x303, 0x300};
	struct kmscon_font font = {.width = 8, .height = 16};
	struct kmscon_glyph *t, *g;
	FT_Library ft;
	FT_Face face;
	int x, y, w, h, t_top = -1, t_x0 = -1, t_x1 = -1;
	int d_top = -1, d_bottom = -1, d_x0 = -1, d_x1 = -1;

	assert(!FT_Init_FreeType(&ft));
	assert(!FT_New_Face(ft, TEST_FONT_DIR "/DejaVuSansMono.ttf", 0, &face));
	assert(!FT_Set_Pixel_Sizes(face, 0, 14));

	t = render_cluster(&font, face, tilde, 2, 1);
	g = render_cluster(&font, face, both, 3, 1);
	assert(t && g);
	w = t->buf.width;
	h = t->buf.height;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint8_t tv = t->buf.data[y * w + x], gv = g->buf.data[y * w + x];

			/* the base and its tilde did not move */
			assert(gv >= tv);
			if (tv) {
				if (t_top < 0)
					t_top = y;
				if (t_x0 < 0 || x < t_x0)
					t_x0 = x;
				t_x1 = x > t_x1 ? x : t_x1;
			}
			/* the grave: ink the tilde alone does not have */
			if (gv > tv) {
				if (d_top < 0)
					d_top = y;
				d_bottom = y;
				if (d_x0 < 0 || x < d_x0)
					d_x0 = x;
				d_x1 = x > d_x1 ? x : d_x1;
			}
		}
	fprintf(stderr, "tilde top %d cols %d-%d; grave rows %d-%d cols %d-%d\n", t_top, t_x0, t_x1,
		d_top, d_bottom, d_x0, d_x1);
	assert(d_top >= 0);
	/* above the tilde, and over the base's columns */
	assert(d_top < t_top && d_bottom <= t_top);
	assert(d_x0 > t_x0 && d_x1 < t_x1);

	free(t);
	free(g);
	FT_Done_Face(face);
	FT_Done_FreeType(ft);
}

int main(void)
{
	assert(FcInit());

	test_filter_unit();
	test_monospace_fallback_set();
	test_copy_mono_clip();
	test_cluster_missing_mark();
	test_cluster_stacked_mark();

	FcFini();
	return 0;
}
