/*
 * Capability probes for the freetype font backend, run like
 * test_capabilities (see there): --list, or one probe by name.
 *
 *   test_capabilities_font FONT_DIR --list | NAME
 *
 * Each probe pins fontconfig to one or two files from tests/fonts, draws one
 * glyph with and without the attribute, and fails when the two bitmaps are
 * the same: the backend drew nothing different for the attribute. The fonts
 * have no italic face and the CJK font has no bold one, so a pass needs the
 * backend to slant or embolden the outline itself, as other terminals do when
 * the family has no such face.
 *
 * The fontconfig configuration is empty apart from the pinned file: no
 * system rules, so fontconfig never sets FC_EMBOLDEN or FC_MATRIX on a match.
 * A fix that only honours those (rather than emboldening or slanting when
 * the matched face lacks the weight or slant) passes in production, where
 * the system's 90-synthetic.conf sets them, and still fails here; such a fix
 * loads that rule into the probe's configuration with it (#100).
 */

#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <string.h>

#include "../src/font/font.c"
#include "../src/font/font_box.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_freetype.c"
#undef LOG_SUBSYSTEM
#include "../src/font/font_colour.c"

void shl_module_ref(struct shl_module *module) {}
void shl_module_unref(struct shl_module *module) {}

static const char *font_dir;

static struct kmscon_font *pinned_font(const char *file, const char *family)
{
	FcConfig *cfg = FcConfigCreate();
	struct kmscon_font *font = NULL;
	char path[4096];

	snprintf(path, sizeof(path), "%s/%s", font_dir, file);
	if (!cfg || !FcConfigAppFontAddFile(cfg, (const FcChar8 *)path) ||
	    !FcConfigSetCurrent(cfg)) {
		fprintf(stderr, "test_capabilities_font: cannot load %s\n", path);
		exit(2);
	}
	if (kmscon_font_find(&font, family, 24, "freetype") || !font ||
	    font->ops != &kmscon_font_freetype_ops) {
		fprintf(stderr, "test_capabilities_font: %s not found\n", family);
		exit(2);
	}
	return font;
}

/* whether @cp draws the same with @a as with @b */
static bool same_glyph(struct kmscon_font *font, uint32_t cp, unsigned int cwidth,
		       struct kmscon_font_attr a, struct kmscon_font_attr b)
{
	struct kmscon_glyph *ga = kmscon_font_render(font, &a, &cp, 1, cwidth);
	struct kmscon_glyph *gb = kmscon_font_render(font, &b, &cp, 1, cwidth);
	const struct video_buffer *ba, *bb;

	if (!ga || !gb) {
		fprintf(stderr, "test_capabilities_font: cannot draw U+%04X\n", cp);
		exit(2);
	}
	ba = &ga->buf;
	bb = &gb->buf;
	if (ba->format || bb->format) {
		fprintf(stderr, "test_capabilities_font: U+%04X is not an A8 glyph\n", cp);
		exit(2);
	}
	return ba->width == bb->width && ba->height == bb->height &&
	       !memcmp(ba->data, bb->data, (size_t)ba->width * ba->height);
}

static bool probe_italic(void)
{
	struct kmscon_font *font = pinned_font("DejaVuSansMono.ttf", "DejaVu Sans Mono");
	bool same = same_glyph(font, 'l', 1, (struct kmscon_font_attr){0},
			       (struct kmscon_font_attr){.italic = true});

	kmscon_font_unref(font);
	if (same)
		printf("an italic 'l' draws the same as an upright one\n");
	return !same;
}

static bool probe_synthetic_bold(void)
{
	struct kmscon_font *font =
		pinned_font("NotoSansMonoCJKJP-Subset.otf", "Noto Sans Mono CJK JP");
	bool same = same_glyph(font, 0x3042, 2, (struct kmscon_font_attr){0},
			       (struct kmscon_font_attr){.bold = true});

	kmscon_font_unref(font);
	if (same)
		printf("a bold U+3042 from a family with no bold face draws the same as a regular "
		       "one\n");
	return !same;
}

static const struct probe {
	const char *name;
	bool (*run)(void);
} probes[] = {
	{"font-freetype-italic", probe_italic},
	{"font-freetype-synthetic-bold", probe_synthetic_bold},
};

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s FONT_DIR --list | NAME\n", argv[0]);
		return 2;
	}
	font_dir = argv[1];
	for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		if (!strcmp(argv[2], "--list")) {
			printf("%s\n", probes[i].name);
			continue;
		}
		if (strcmp(argv[2], probes[i].name))
			continue;
		if (kmscon_font_register(&kmscon_font_freetype_ops)) {
			fprintf(stderr, "test_capabilities_font: cannot register freetype\n");
			return 2;
		}
		return probes[i].run() ? 0 : 1;
	}
	if (strcmp(argv[2], "--list")) {
		fprintf(stderr, "test_capabilities_font: no probe %s\n", argv[2]);
		return 2;
	}
	return 0;
}
