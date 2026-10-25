/*
 * mod-psf's loader (src/font/font_psf.c) on tests/fonts/psf-test.psf, a PSF2
 * font of 256 8x16 glyphs, in every build; and on the same font gzipped,
 * which loads in a build with zlib and, in one without (ADR-103, The
 * zlib-free variant), fails with a line saying the build leaves zlib out.
 * Which build this is comes from meson's zlib feature (the second argument),
 * not from the define mod-psf is compiled with, so a build with zlib whose
 * mod-psf lost KMSCON_PSF_GZ fails here.
 *
 *   test_font_psf FONTS-DIR gz|no-gz
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/font/font_psf.c"

static char path[512];

static const char *font(const char *dir, const char *name)
{
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return path;
}

/* the font loaded: its size, and glyph 'A' row 3 holding 'A' + 3 */
static void check_loaded(struct kmscon_font *f)
{
	psf_font_t *psf = f->data;

	assert(f->width == 8 && f->height == 16);
	assert(psf->glyphs == 256 && psf->width == 8 && psf->height == 16);
	assert(psf->data['A' * psf->step + 3] == 'A' + 3);
	kmscon_font_psf_ops.destroy(f);
}

/* @name loaded, with stderr, where the log goes, captured into @buf */
static void capture_log(const char *name, char *buf, size_t size, int *ret)
{
	struct kmscon_font f = {0};
	FILE *log = tmpfile();
	int saved = dup(STDERR_FILENO);
	size_t n;

	assert(log && saved >= 0);
	fflush(stderr);
	dup2(fileno(log), STDERR_FILENO);
	*ret = kmscon_font_psf_ops.init(&f, name, 16);
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
	rewind(log);
	n = fread(buf, 1, size - 1, log);
	buf[n] = 0;
	fclose(log);
	if (!*ret)
		check_loaded(&f);
}

int main(int argc, char **argv)
{
	struct kmscon_font f = {0};
	char logged[4096];
	int ret;

	bool gz;

	assert(argc == 3 && (!strcmp(argv[2], "gz") || !strcmp(argv[2], "no-gz")));
	gz = !strcmp(argv[2], "gz");
	log_set_level(LOG_DEBUG);

	assert(kmscon_font_psf_ops.init(&f, font(argv[1], "psf-test.psf"), 16) == 0);
	check_loaded(&f);

	capture_log(font(argv[1], "psf-test.psf.gz"), logged, sizeof(logged), &ret);
	if (gz) {
		assert(ret == 0);
	} else {
		assert(ret != 0);
		assert(strstr(logged,
			      "compressed PSF fonts need zlib, which this build leaves out"));
	}

	printf("test_font_psf: ok (%s)\n",
	       gz ? "gzip fonts load" : "gzip fonts refused, a build without zlib");
	return 0;
}
