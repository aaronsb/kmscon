/*
 * kmscon - capability probes through kmscon's own vte
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
 * The probes of tests/capabilities/run.py that need kmscon's own path, not
 * libtsm's defaults: sixel, the kitty graphics protocol and sixel's three
 * modes (ADR-103, The capability probes), and XTVERSION and XTGETTCAP, whose
 * text and table kmscon sets (ADR-201's note of 2026-10-26). The
 * configuration is kmscon's defaults, parsed by its option table, with
 * --term kmscon, so a build without images has --sixel and --kitty-graphics
 * turned off by their afterchecks and a build whose default TERM is another
 * still probes the table; the vte is terminal_vte_new's, with the process's
 * image budget. In a build with images each image probe passes; in one
 * without, each must fail (-Dimages in capabilities.list). The XTVERSION and
 * XTGETTCAP probes pass in both.
 *
 *   test_capabilities_images --list    print the probe names
 *   test_capabilities_images NAME      run one probe: exit 0 if it works, 1
 *                                      with the reason on stdout if not
 */

#include <stdarg.h>
/* each file names its log subsystem */
#include "../src/misc/conf.c"
#undef LOG_SUBSYSTEM
#include "../src/config.c"
#undef LOG_SUBSYSTEM
#include "../src/render/image_budget.c"
#undef LOG_SUBSYSTEM
#include "../src/terminal_vte.c"

static char why[512];
static char reply[512];
static size_t reply_len;

static bool fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	return false;
}

static void on_write(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	(void)vte;
	(void)data;
	if (reply_len + len < sizeof(reply)) {
		memcpy(reply + reply_len, u8, len);
		reply_len += len;
		reply[reply_len] = 0;
	}
}

static struct tsm_vte *vte;

/* @query's reply, with escapes shown as \e */
static const char *ask(const char *query)
{
	static char shown[sizeof(reply) * 2];
	size_t i, n = 0;

	reply_len = 0;
	reply[0] = 0;
	tsm_vte_input(vte, query, strlen(query));
	for (i = 0; i < reply_len && n + 3 < sizeof(shown); i++) {
		if (reply[i] == '\033') {
			shown[n++] = '\\';
			shown[n++] = 'e';
		} else {
			shown[n++] = reply[i];
		}
	}
	shown[n] = 0;
	return shown;
}

/* the DA1 reply \e[?<a>;<a>;...c lists attribute 4 */
static bool da1_has_sixel(const char *r)
{
	char attrs[sizeof(reply) * 2], *tok, *save;
	size_t n;

	if (strncmp(r, "\\e[?", 4))
		return false;
	n = strlen(r + 4);
	if (!n || r[4 + n - 1] != 'c' || n > sizeof(attrs))
		return false;
	memcpy(attrs, r + 4, n - 1);
	attrs[n - 1] = 0;
	for (tok = strtok_r(attrs, ";", &save); tok; tok = strtok_r(NULL, ";", &save))
		if (!strcmp(tok, "4"))
			return true;
	return false;
}

/* DA1 carries attribute 4, and XTSMGRAPHICS reads the register count */
static bool probe_sixel(void)
{
	const char *r = ask("\033[c");

	if (!da1_has_sixel(r))
		return fail("DA1 replies %s, without attribute 4", r);
	r = ask("\033[?1;1S");
	if (strncmp(r, "\\e[?1;0;", 8))
		return fail("XTSMGRAPHICS 1;1 replies %s, not a register count", r);
	return true;
}

/* a query with an id gets OK */
static bool probe_kitty_graphics(void)
{
	const char *r = ask("\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\");

	if (strcmp(r, "\\e_Gi=31;OK\\e\\"))
		return fail("a=q replies \"%s\", not OK", r);
	return true;
}

/* DECRQM answers 1 or 2: the mode is offered */
static bool probe_mode(unsigned int number)
{
	char query[32], set[32], reset[32];
	const char *r;

	snprintf(query, sizeof(query), "\033[?%u$p", number);
	snprintf(set, sizeof(set), "\\e[?%u;1$y", number);
	snprintf(reset, sizeof(reset), "\\e[?%u;2$y", number);
	r = ask(query);
	if (strcmp(r, set) && strcmp(r, reset))
		return fail("DECRQM ?%u replies %s, not 1 or 2", number, r);
	return true;
}

static bool probe_mode_80(void)
{
	return probe_mode(80);
}

static bool probe_mode_1070(void)
{
	return probe_mode(1070);
}

static bool probe_mode_8452(void)
{
	return probe_mode(8452);
}

/* XTVERSION: DCS > | kmscon(version) ST */
static bool probe_xtversion(void)
{
	const char *r = ask("\033[>q");

	if (strcmp(r, "\\eP>|kmscon(" BUILD_VERSION ")\\e\\"))
		return fail("CSI > q replies \"%s\", not kmscon(" BUILD_VERSION ")", r);
	return true;
}

/* XTGETTCAP: TN is kmscon, and Smulx comes back byte for byte as the entry
 * has it, \E decoded and its % sequence kept, in one reply */
static bool probe_xtgettcap(void)
{
	const char *r = ask("\033P+q544e;536d756c78\033\\");

	if (strcmp(r, "\\eP1+r544e=6B6D73636F6E;536d756c78=1B5B343A25703125646D\\e\\"))
		return fail("DCS + q TN;Smulx replies \"%s\", not TN=kmscon;Smulx=\\E[4:%%p1%%dm",
			    r);
	return true;
}

static const struct {
	const char *name;
	bool (*run)(void);
} probes[] = {
	{"sixel", probe_sixel},		{"kitty-graphics", probe_kitty_graphics},
	{"mode-80", probe_mode_80},	{"mode-1070", probe_mode_1070},
	{"mode-8452", probe_mode_8452}, {"xtversion", probe_xtversion},
	{"xtgettcap", probe_xtgettcap},
};

int main(int argc, char **argv)
{
	char *args[] = {"kmscon", "--term", "kmscon"};
	struct image_budget_terminal images = {0};
	struct kmscon_conf_t *conf;
	struct conf_ctx *ctx;
	struct tsm_screen *con;
	size_t i;
	bool ok;

	if (argc != 2) {
		fprintf(stderr, "usage: %s --list | NAME\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "--list")) {
		for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
			printf("%s\n", probes[i].name);
		return 0;
	}
	for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
		if (!strcmp(argv[1], probes[i].name))
			break;
	if (i == sizeof(probes) / sizeof(probes[0])) {
		fprintf(stderr, "test_capabilities_images: no probe %s\n", argv[1]);
		return 2;
	}

	if (kmscon_conf_new(&ctx) || conf_ctx_parse_argv(ctx, 3, args)) {
		fprintf(stderr, "test_capabilities_images: cannot parse kmscon's defaults\n");
		return 2;
	}
	conf = conf_ctx_get_mem(ctx);
	if (tsm_screen_new(&con, NULL, NULL) || tsm_screen_resize(con, 80, 24) ||
	    tsm_screen_set_cell_size(con, 8, 16) ||
	    terminal_vte_new(&vte, &images, con, conf, on_write, NULL)) {
		fprintf(stderr, "test_capabilities_images: cannot make the terminal\n");
		return 2;
	}
	ok = probes[i].run();
	image_budget_remove_terminal(&images);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	conf_ctx_free(ctx);
	if (!ok)
		printf("%s\n", why);
	return ok ? 0 : 1;
}
