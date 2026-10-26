/*
 * kmscon - PSF font backend against crafted and real fonts
 *
 * Copyright (c) 2026 the kmscon fork authors
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

/*
 * font_psf.c against crafted fonts (#229), each written plain and gzipped:
 * - a header whose glyph count times glyph size wraps 32 bits, a glyph count
 *   past the cap, a glyph size that does not match its width and height, a
 *   width or height of 0 or past the cap, and a file shorter than its header
 *   says are refused;
 * - a unicode table with more entries than the cap, or a file larger than the
 *   byte cap, is refused;
 * - a UTF-8 sequence split across two reads of the table decodes, and one cut
 *   off by the end of the file is dropped;
 * - a font of fewer than 64 glyphs draws a missing character from glyph 0;
 * - a gzip font whose checksum is wrong is refused.
 * Every font the loader accepts has each of its glyphs drawn. Then every PSF
 * font in kbd's console font directory, when the build machine has one, must
 * load, as shipped and gunzipped, and every other file there must be refused.
 * Each load runs in a child process, so a crash fails that case alone. Built
 * with -Db_sanitize=address,undefined (make asan), a read past a buffer or a
 * leak on a refusal fails it too.
 *
 * First, tests/fonts/psf-test.psf, a PSF2 font of 256 8x16 glyphs, loads in
 * every build; the same font gzipped loads in a build with zlib and, in one
 * without (ADR-103, The zlib-free variant), fails with a line saying the
 * build leaves zlib out. Which build this is comes from meson's zlib feature
 * (the second argument), not from the define mod-psf is compiled with, so a
 * build with zlib whose mod-psf lost KMSCON_PSF_GZ fails here.
 *
 *   test_font_psf FONTS-DIR gz|no-gz
 */

#undef NDEBUG
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/font/font_psf.c"

#define KBD_FONT_DIR "/usr/share/kbd/consolefonts"

struct buf {
	uint8_t *p;
	size_t len, cap;
};

static char path[64];
static unsigned int failures;

static void put(struct buf *b, const void *src, size_t len)
{
	if (b->len + len > b->cap) {
		b->cap = (b->len + len) * 2;
		b->p = realloc(b->p, b->cap);
		assert(b->p);
	}
	memcpy(b->p + b->len, src, len);
	b->len += len;
}

static void put_bytes(struct buf *b, uint8_t c, size_t n)
{
	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2;
		b->p = realloc(b->p, b->cap);
		assert(b->p);
	}
	memset(b->p + b->len, c, n);
	b->len += n;
}

static void put32(struct buf *b, uint32_t v)
{
	uint8_t le[4] = {v, v >> 8, v >> 16, v >> 24};

	put(b, le, 4);
}

static void psf1(struct buf *b, uint8_t mode, uint8_t height)
{
	uint8_t hdr[4] = {0x36, 0x04, mode, height};

	put(b, hdr, 4);
}

static void psf2_header(struct buf *b, uint32_t headersize, uint32_t flags, uint32_t glyphs,
			uint32_t charsize, uint32_t height, uint32_t width)
{
	static const uint8_t magic[4] = {0x72, 0xb5, 0x4a, 0x86};

	put(b, magic, 4);
	put32(b, 0);
	put32(b, headersize);
	put32(b, flags);
	put32(b, glyphs);
	put32(b, charsize);
	put32(b, height);
	put32(b, width);
}

static void psf2(struct buf *b, uint32_t flags, uint32_t glyphs, uint32_t charsize, uint32_t height,
		 uint32_t width)
{
	psf2_header(b, 32, flags, glyphs, charsize, height, width);
}

/* writes @b to the scratch file, gzipped if @gz */
static void write_font(const struct buf *b, bool gz)
{
#ifdef ZLIB_VERSION
	if (gz) {
		gzFile f = gzopen(path, "wb");

		assert(f);
		assert(b->len == 0 || gzwrite(f, b->p, b->len) == (int)b->len);
		assert(gzclose(f) == Z_OK);
		return;
	}
#endif
	FILE *f = fopen(path, "wb");

	assert(f);
	assert(fwrite(b->p, 1, b->len, f) == b->len);
	assert(fclose(f) == 0);
}

/* every glyph of a loaded font, a code point it may lack, and a lookup sweep */
static void exercise(struct kmscon_font *kfont)
{
	psf_font_t *font = kfont->data;
	struct kmscon_font_attr attr = {0};
	struct kmscon_glyph *g;
	uint32_t i;

	for (i = 0; i < font->glyphs; i++) {
		g = new_glyph(kfont, &attr, i);
		assert(g);
		free(g);
	}
	for (i = 0; i < 0x300; i++)
		kmscon_font_psf_has_glyph(kfont, &attr, i);
	i = 0x10ffff;
	g = kmscon_font_psf_render(kfont, &attr, &i, 1, 1);
	assert(g);
	free(g);
}

/* what a case's child exits with; a crash or a sanitizer report exits otherwise */
#define LOADED 0
#define REFUSED 10
#define MISMAPPED 11

/*
 * Runs mod-psf's init on @file in a child process, so a crash fails one case.
 * A font it accepts is exercised and must map @code, when not 0, to glyph 1.
 */
static int run(const char *file, uint32_t code)
{
	struct kmscon_font kfont = {0};
	psf_font_t *font;
	int status, ret = LOADED;
	pid_t pid;

	fflush(NULL);
	pid = fork();
	assert(pid >= 0);
	if (pid) {
		assert(waitpid(pid, &status, 0) == pid);
		return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	}

	if (kmscon_font_psf_ops.init(&kfont, file, 16))
		exit(REFUSED);
	exercise(&kfont);
	font = kfont.data;
	if (code && unicode_table_get(&font->unicode_table, code) != 1)
		ret = MISMAPPED;
	kmscon_font_psf_ops.destroy(&kfont);
	exit(ret);
}

static const char *outcome(int ret)
{
	static char other[32];

	if (ret == LOADED)
		return "loaded";
	if (ret == REFUSED)
		return "refused";
	if (ret == MISMAPPED)
		return "mapped wrong";
	snprintf(other, sizeof(other), "crashed (%d)", ret);
	return other;
}

static void expect_code(const char *what, const struct buf *b, bool gz, int want, uint32_t code)
{
	int ret;

	write_font(b, gz);
	ret = run(path, code);
	if (ret != want) {
		fprintf(stderr, "test_font_psf: %s%s: %s\n", what, gz ? " (gz)" : "", outcome(ret));
		failures++;
	}
}

/* @what, plain and (with zlib) gzipped */
static void expect_both(const char *what, struct buf *b, bool loads)
{
	expect_code(what, b, false, loads ? LOADED : REFUSED, 0);
#ifdef ZLIB_VERSION
	expect_code(what, b, true, loads ? LOADED : REFUSED, 0);
#endif
	free(b->p);
	*b = (struct buf){0};
}

static void test_header(void)
{
	struct buf b = {0};

	/* 16 * 0x10000001 is 16 in 32 bits */
	psf2(&b, 0, 0x10000001, 16, 16, 8);
	put_bytes(&b, 0xaa, 64 * 16);
	expect_both("glyph count times glyph size wraps", &b, false);

	psf2(&b, 0, 0x01000000, 16, 16, 8);
	put_bytes(&b, 0xaa, 16);
	expect_both("16M glyphs", &b, false);

	/* glyph 0xffff would collide with the table's no-entry index */
	psf2(&b, 0, 0x10000, 1, 1, 8);
	put_bytes(&b, 0xaa, 0x10000);
	expect_both("65536 glyphs", &b, false);

	psf2(&b, 0, 0xffff, 1, 1, 8);
	put_bytes(&b, 0xaa, 0xffff);
	expect_both("65535 glyphs", &b, true);

	/* a row of 32 pixels is 4 bytes, so a 2-row glyph is 8 */
	psf2(&b, 0, 256, 2, 2, 32);
	put_bytes(&b, 0xaa, 256 * 2);
	expect_both("glyph size short of width and height", &b, false);

	psf2(&b, 0, 256, 32, 2, 8);
	put_bytes(&b, 0xaa, 256 * 32);
	expect_both("glyph size past width and height", &b, false);

	psf2(&b, 0, 256, 2 * 5, 2, 33);
	put_bytes(&b, 0xaa, 256 * 2 * 5);
	expect_both("33 pixels wide", &b, false);

	psf2(&b, 0, 256, 2 * 4, 2, 32);
	put_bytes(&b, 0xaa, 256 * 2 * 4);
	expect_both("32 pixels wide", &b, true);

	psf2(&b, 0, 256, 0, 0, 8);
	expect_both("0 rows", &b, false);

	psf2(&b, 0, 256, 0, 16, 0);
	expect_both("0 pixels wide", &b, false);

	psf2(&b, 0, 0, 16, 16, 8);
	expect_both("0 glyphs", &b, false);

	psf1(&b, 0, 0);
	expect_both("psf1 of 0 rows", &b, false);

	psf2(&b, 0, 4, 257, 257, 8);
	put_bytes(&b, 0xaa, 4 * 257);
	expect_both("257 rows", &b, false);

	psf2(&b, 0, 4, 256 * 4, 256, 32);
	put_bytes(&b, 0xaa, 4 * 256 * 4);
	expect_both("32x256", &b, true);

	psf2_header(&b, 16, 0, 256, 16, 16, 8);
	put_bytes(&b, 0xaa, 16 + 256 * 16);
	expect_both("16-byte header", &b, false);

	psf2_header(&b, 0xfffffff0, 0, 256, 16, 16, 8);
	put_bytes(&b, 0xaa, 256 * 16);
	expect_both("header size wraps with the glyphs", &b, false);

	/* inside the byte cap, but past the end of the file */
	psf2_header(&b, 15u << 20, 0, 256, 16, 16, 8);
	put_bytes(&b, 0xaa, 256 * 16);
	expect_both("15 MiB header", &b, false);
}

/* a 64-byte PSF2 header: glyph i, all of whose bytes are i, starts at 64 + 16i */
static void test_header_size(void)
{
	struct kmscon_font kfont;
	struct buf b = {0};
	psf_font_t *font;
	uint32_t i;
	int gz;

	psf2_header(&b, 64, 0, 256, 16, 16, 8);
	put_bytes(&b, 0x55, 32);
	for (i = 0; i < 256; i++)
		put_bytes(&b, i, 16);
	for (gz = 0; gz < 2; gz++) {
#ifndef ZLIB_VERSION
		if (gz)
			break;
#endif
		write_font(&b, gz);
		memset(&kfont, 0, sizeof(kfont));
		if (kmscon_font_psf_ops.init(&kfont, path, 16)) {
			fprintf(stderr, "test_font_psf: 64-byte header%s: refused\n",
				gz ? " (gz)" : "");
			failures++;
			continue;
		}
		font = kfont.data;
		for (i = 0; i < 256 * 16; i++) {
			if (font->data[i] != i / 16) {
				fprintf(stderr, "test_font_psf: 64-byte header%s: byte %u is %u\n",
					gz ? " (gz)" : "", i, font->data[i]);
				failures++;
				break;
			}
		}
		kmscon_font_psf_ops.destroy(&kfont);
	}
	free(b.p);
}

static void test_short(void)
{
	struct buf b = {0};

	psf1(&b, PSF1_FLAG_512, 16);
	put_bytes(&b, 0xaa, 100);
	expect_both("psf1 shorter than its glyphs", &b, false);

	psf2(&b, 0, 256, 16, 16, 8);
	put_bytes(&b, 0xaa, 256 * 16 - 1);
	expect_both("psf2 shorter than its glyphs", &b, false);

	psf2(&b, 0, 256, 16, 16, 8);
	b.len = 20;
	expect_both("psf2 header cut short", &b, false);

	put_bytes(&b, 0x36, 1);
	expect_both("one byte", &b, false);

	expect_both("empty file", &b, false);
}

static void test_small(void)
{
	struct buf b = {0};

	/* a missing character draws from '?' (63), which this font lacks */
	psf2(&b, 0, 1, 16, 16, 8);
	put_bytes(&b, 0xaa, 16);
	expect_both("one glyph", &b, true);
}

/* a 512-glyph 8x16 font with a unicode table whose bytes follow */
static void psf2_with_table(struct buf *b)
{
	psf2(b, PSF2_FLAG_TAB, 512, 16, 16, 8);
	put_bytes(b, 0xaa, 512 * 16);
}

static void test_table(void)
{
	struct buf b = {0};

	psf2_with_table(&b);
	put_bytes(&b, 'A', PSF_MAX_ENTRIES);
	expect_both("table at the entry cap", &b, true);

	psf2_with_table(&b);
	put_bytes(&b, 'A', PSF_MAX_ENTRIES + 1);
	expect_both("table past the entry cap", &b, false);

	/* glyph separators only: no entries, so only the byte cap ends it */
	psf2_with_table(&b);
	put_bytes(&b, 0xff, PSF_MAX_BYTES);
	expect_both("file past the byte cap", &b, false);

	psf1(&b, PSF1_FLAG_TAB, 16);
	put_bytes(&b, 0xaa, 256 * 16);
	put_bytes(&b, 'A', 2 * (PSF_MAX_ENTRIES + 1));
	expect_both("psf1 table past the entry cap", &b, false);
}

/*
 * A table of 1022 entries of 'B' for glyph 0, a glyph separator, then @tail,
 * whose first byte is the last of the table's first read. @code, if not 0,
 * must map to glyph 1.
 */
static void check_split(const char *what, const uint8_t *tail, size_t len, uint32_t code)
{
	struct buf b = {0};

	psf2_with_table(&b);
	put_bytes(&b, 'B', 1022);
	put_bytes(&b, 0xff, 1);
	put(&b, tail, len);
	expect_code(what, &b, false, LOADED, code);
#ifdef ZLIB_VERSION
	expect_code(what, &b, true, LOADED, code);
#endif
	free(b.p);
}

static void test_split(void)
{
	/* U+00E9, its two bytes either side of the first read */
	static const uint8_t e_acute[] = {0xc3, 0xa9, 0xff};
	/* the lead byte of a 4-byte sequence, then the end of the file */
	static const uint8_t cut[] = {0xf0};

	check_split("UTF-8 split across reads", e_acute, sizeof(e_acute), 0xe9);
	check_split("UTF-8 cut off by the end", cut, sizeof(cut), 0);
}

static void test_gz_corrupt(void)
{
#ifdef ZLIB_VERSION
	struct buf b = {0};
	FILE *f;
	long size;
	uint8_t c;
	int ret;

	psf2_with_table(&b);
	put_bytes(&b, 'A', 0x10000);
	write_font(&b, true);
	free(b.p);

	/* the trailer's CRC-32 is the 8th to 5th bytes from the end */
	f = fopen(path, "r+b");
	assert(f);
	assert(fseek(f, -8, SEEK_END) == 0);
	size = ftell(f);
	assert(size > 0 && fread(&c, 1, 1, f) == 1);
	c ^= 0xff;
	assert(fseek(f, size, SEEK_SET) == 0 && fwrite(&c, 1, 1, f) == 1);
	assert(fclose(f) == 0);

	ret = run(path, 0);
	if (ret != REFUSED) {
		fprintf(stderr, "test_font_psf: gz with a bad checksum: %s\n", outcome(ret));
		failures++;
	}
#endif
}

/* @file into @b, gunzipped when the build reads gzip; whether it was gzipped */
static bool slurp(const char *file, struct buf *b)
{
	uint8_t chunk[4096];
	bool gz = false;
	FILE *f;
	size_t n;

	f = fopen(file, "rb");
	assert(f);
	while ((n = fread(chunk, 1, sizeof(chunk), f)))
		put(b, chunk, n);
	fclose(f);
#ifdef ZLIB_VERSION
	if (b->len >= 2 && IS_GZ_MAGIC(b->p)) {
		gzFile z = gzopen(file, "rb");
		int got;

		assert(z);
		gz = true;
		b->len = 0;
		while ((got = gzread(z, chunk, sizeof(chunk))) > 0)
			put(b, chunk, got);
		gzclose(z);
	}
#endif
	return gz;
}

/*
 * The positive control: every PSF font kbd ships loads, and loads again
 * gunzipped when it ships gzipped; every other file there is refused.
 */
static void test_kbd(void)
{
	unsigned int loaded = 0, gunzipped = 0, refused = 0, missed = 0;
	struct buf b = {0};
	struct dirent *e;
	char file[512];
	struct stat st;
	bool psf, gz;
	DIR *d;

#ifndef ZLIB_VERSION
	/* kbd ships every font gzipped, so here none would load */
	printf("test_font_psf: a build without zlib, positive control skipped\n");
	return;
#endif
	d = opendir(KBD_FONT_DIR);
	if (!d) {
		printf("test_font_psf: no %s, positive control skipped\n", KBD_FONT_DIR);
		return;
	}
	log_set_level(LOG_FATAL);
	while ((e = readdir(d))) {
		snprintf(file, sizeof(file), "%s/%s", KBD_FONT_DIR, e->d_name);
		if (stat(file, &st) || !S_ISREG(st.st_mode))
			continue;
		b.len = 0;
		gz = slurp(file, &b);
		psf = b.len >= 4 && (IS_PSF1_MAGIC(b.p) || IS_PSF2_MAGIC(b.p));
		if (run(file, 0) != (psf ? LOADED : REFUSED)) {
			fprintf(stderr, "test_font_psf: %s: %s\n", file,
				psf ? "PSF, refused" : "not PSF, loaded");
			missed += psf;
			failures++;
		} else if (!psf) {
			refused++;
		} else {
			loaded++;
			if (gz) {
				write_font(&b, false);
				if (run(path, 0) == LOADED) {
					gunzipped++;
				} else {
					fprintf(stderr, "test_font_psf: %s gunzipped: refused\n",
						file);
					missed++;
					failures++;
				}
			}
		}
	}
	closedir(d);
	free(b.p);
	log_set_level(LOG_NOTICE);
	printf("test_font_psf: %s: %u PSF fonts loaded and %u of them gunzipped, %u refused; "
	       "%u other files refused\n",
	       KBD_FONT_DIR, loaded, gunzipped, missed, refused);
}

/* @name in @dir */
static const char *fixture(const char *dir, const char *name)
{
	static char file[512];

	snprintf(file, sizeof(file), "%s/%s", dir, name);
	return file;
}

/* the fixture loaded: its size, and glyph 'A' row 3 holding 'A' + 3 */
static void check_fixture(struct kmscon_font *f)
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
		check_fixture(&f);
}

static void test_fixture(const char *dir, bool gz)
{
	struct kmscon_font f = {0};
	char logged[4096];
	int ret;

	assert(kmscon_font_psf_ops.init(&f, fixture(dir, "psf-test.psf"), 16) == 0);
	check_fixture(&f);

	capture_log(fixture(dir, "psf-test.psf.gz"), logged, sizeof(logged), &ret);
	if (gz) {
		assert(ret == 0);
	} else {
		assert(ret != 0);
		assert(strstr(logged,
			      "compressed PSF fonts need zlib, which this build leaves out"));
	}
}

int main(int argc, char **argv)
{
	char dir[] = "/tmp/test_font_psf.XXXXXX";
	bool gz;

	assert(argc == 3 && (!strcmp(argv[2], "gz") || !strcmp(argv[2], "no-gz")));
	gz = !strcmp(argv[2], "gz");
	test_fixture(argv[1], gz);

	assert(mkdtemp(dir));
	snprintf(path, sizeof(path), "%s/font", dir);

	test_header();
	test_header_size();
	test_short();
	test_small();
	test_table();
	test_split();
	test_gz_corrupt();
	test_kbd();

	unlink(path);
	rmdir(dir);
	if (failures) {
		fprintf(stderr, "test_font_psf: %u failures\n", failures);
		return 1;
	}
	printf("test_font_psf: ok (%s)\n",
	       gz ? "gzip fonts load" : "gzip fonts refused, a build without zlib");
	return 0;
}
