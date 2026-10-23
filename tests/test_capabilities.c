/*
 * Capability probes at the escape-sequence level: one small check per
 * terminal feature, run through libtsm's public API the way kmscon drives it.
 * tests/capabilities/run.py runs each probe by name and judges the result
 * against tests/capabilities/capabilities.list, where each probe is recorded
 * as works or missing.
 *
 *   test_capabilities --list    print the probe names
 *   test_capabilities NAME      run one probe: exit 0 if the feature works,
 *                               1 with the reason on stdout if it does not
 *
 * A probe feeds the bytes a program would send and checks the reply the
 * terminal writes back or the screen state it leaves, which is the smallest
 * observation that proves the feature. A feature libtsm has no interface for
 * yet is checked through the name its interface is expected to have (an
 * #ifdef on a frame flag), or is a placeholder that always fails and says so;
 * whoever implements the feature replaces the placeholder with a real probe.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"

#define KEY_1 0x31 /* XKB_KEY_1 */

struct term {
	struct tsm_screen *con;
	struct tsm_vte *vte;
	char reply[512];
	size_t reply_len;
};

static char why[512];

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
	struct term *t = data;

	(void)vte;
	if (t->reply_len + len >= sizeof(t->reply))
		len = sizeof(t->reply) - 1 - t->reply_len;
	memcpy(t->reply + t->reply_len, u8, len);
	t->reply_len += len;
	t->reply[t->reply_len] = 0;
}

static struct term *term_new(unsigned int cols, unsigned int rows)
{
	struct term *t = calloc(1, sizeof(*t));

	if (!t || tsm_screen_new(&t->con, NULL, NULL) ||
	    tsm_screen_resize(t->con, cols, rows) ||
	    tsm_vte_new(&t->vte, t->con, on_write, t, NULL, NULL)) {
		fprintf(stderr, "test_capabilities: cannot create a terminal\n");
		exit(2);
	}
	return t;
}

static void term_free(struct term *t)
{
	tsm_vte_unref(t->vte);
	tsm_screen_unref(t->con);
	free(t);
}

static void feed(struct term *t, const char *s)
{
	tsm_vte_input(t->vte, s, strlen(s));
}

static void clear_reply(struct term *t)
{
	t->reply_len = 0;
	t->reply[0] = 0;
}

/* @s printable, ESC as \e and other controls as \xNN; up to four of these
 * strings can be in use at once, as the arguments of one fail() */
static const char *shown(const char *s)
{
	static char bufs[4][1024];
	static unsigned int next;
	char *buf = bufs[next++ % 4];
	size_t n = 0;

	for (; *s && n < sizeof(bufs[0]) - 5; s++) {
		unsigned char c = *s;

		if (c == 0x1b)
			n += snprintf(buf + n, sizeof(bufs[0]) - n, "\\e");
		else if (c < 0x20 || c == 0x7f)
			n += snprintf(buf + n, sizeof(bufs[0]) - n, "\\x%02x", c);
		else
			buf[n++] = c;
	}
	buf[n] = 0;
	return buf;
}

/* feed @query and expect exactly @want back */
static bool reply_is(struct term *t, const char *query, const char *want)
{
	clear_reply(t);
	feed(t, query);
	if (strcmp(t->reply, want))
		return fail("%s replied \"%s\", want \"%s\"", shown(query), shown(t->reply),
			    shown(want));
	return true;
}

/* feed @query and expect a reply that starts with @prefix and ends with @suffix */
static bool reply_matches(struct term *t, const char *query, const char *prefix,
			  const char *suffix)
{
	size_t lp = strlen(prefix), ls = strlen(suffix);

	clear_reply(t);
	feed(t, query);
	if (t->reply_len < lp + ls || strncmp(t->reply, prefix, lp) ||
	    strcmp(t->reply + t->reply_len - ls, suffix))
		return fail("%s replied \"%s\", want \"%s...%s\"", shown(query),
			    shown(t->reply), shown(prefix), shown(suffix));
	return true;
}

static const struct tsm_frame_cell *cell(struct term *t, unsigned int x, unsigned int y)
{
	return tsm_frame_cell_at(tsm_screen_frame(t->con), x, y);
}

/* --- SGR --- */

static bool probe_sgr_italic(struct term *t)
{
	feed(t, "\e[3mA\e[23mB");
	if (!(cell(t, 0, 0)->flags & TSM_FRAME_ITALIC))
		return fail("SGR 3 did not set italic");
	if (cell(t, 1, 0)->flags & TSM_FRAME_ITALIC)
		return fail("SGR 23 did not clear italic");
	return true;
}

static bool probe_sgr_truecolor(struct term *t)
{
	const struct tsm_frame_cell *c;

	feed(t, "\e[38;2;1;2;3;48;2;4;5;6mA");
	c = cell(t, 0, 0);
	if (c->fg.r != 1 || c->fg.g != 2 || c->fg.b != 3 || c->bg.r != 4 || c->bg.g != 5 ||
	    c->bg.b != 6)
		return fail("SGR 38;2 / 48;2 drew fg %u,%u,%u bg %u,%u,%u", c->fg.r, c->fg.g,
			    c->fg.b, c->bg.r, c->bg.g, c->bg.b);
	return true;
}

/* ECMA-48 SGR 53 overline, 55 not overlined */
static bool probe_sgr_overline(struct term *t)
{
#ifdef TSM_FRAME_OVERLINE
	feed(t, "\e[53mA\e[55mB");
	if (!(cell(t, 0, 0)->flags & TSM_FRAME_OVERLINE))
		return fail("SGR 53 did not set overline");
	if (cell(t, 1, 0)->flags & TSM_FRAME_OVERLINE)
		return fail("SGR 55 did not clear overline");
	return true;
#else
	(void)t;
	return fail("libtsm has no TSM_FRAME_OVERLINE: SGR 53/55 are dropped");
#endif
}

/* mintty: SGR 73 superscript, 74 subscript, 75 neither */
static bool probe_sgr_super_subscript(struct term *t)
{
#if defined(TSM_FRAME_SUPERSCRIPT) && defined(TSM_FRAME_SUBSCRIPT)
	feed(t, "\e[73mA\e[74mB\e[75mC");
	if (!(cell(t, 0, 0)->flags & TSM_FRAME_SUPERSCRIPT))
		return fail("SGR 73 did not set superscript");
	if (!(cell(t, 1, 0)->flags & TSM_FRAME_SUBSCRIPT) ||
	    (cell(t, 1, 0)->flags & TSM_FRAME_SUPERSCRIPT))
		return fail("SGR 74 did not switch to subscript");
	if (cell(t, 2, 0)->flags & (TSM_FRAME_SUPERSCRIPT | TSM_FRAME_SUBSCRIPT))
		return fail("SGR 75 did not clear super/subscript");
	return true;
#else
	(void)t;
	return fail("libtsm has no TSM_FRAME_SUPERSCRIPT/SUBSCRIPT: SGR 73-75 are dropped");
#endif
}

/* --- modes and reports --- */

/* synchronized output: DECRQM must know mode 2026 (1 set, 2 reset) */
static bool probe_mode_2026_sync(struct term *t)
{
	clear_reply(t);
	feed(t, "\e[?2026$p");
	if (strcmp(t->reply, "\e[?2026;1$y") && strcmp(t->reply, "\e[?2026;2$y"))
		return fail("DECRQM ?2026 replied \"%s\", want \"\\e[?2026;1$y\" or ;2$y",
			    shown(t->reply));
	return true;
}

/* DECRQM for a DEC private mode (DECTCEM, set) and an ANSI mode (IRM, reset) */
static bool probe_decrqm(struct term *t)
{
	return reply_is(t, "\e[?25$p", "\e[?25;1$y") && reply_is(t, "\e[4$p", "\e[4;2$y");
}

static bool probe_da1(struct term *t)
{
	return reply_matches(t, "\e[c", "\e[?", "c");
}

/* DA1's first parameter is the conformance level: 61 (VT100) to 65 (VT510) */
static bool probe_da1_level(struct term *t)
{
	int level;

	clear_reply(t);
	feed(t, "\e[c");
	if (sscanf(t->reply, "\e[?%d", &level) != 1 || level < 61 || level > 65)
		return fail("DA1 replied \"%s\": the level is not one of 61-65", shown(t->reply));
	return true;
}

static bool probe_da2(struct term *t)
{
	return reply_matches(t, "\e[>c", "\e[>", "c");
}

/* DA3 (DECRPTUI): DCS ! | unit id ST */
static bool probe_da3(struct term *t)
{
	return reply_matches(t, "\e[=c", "\eP!|", "\e\\");
}

/* XTVERSION: DCS > | name and version ST */
static bool probe_xtversion(struct term *t)
{
	return reply_matches(t, "\e[>q", "\eP>|", "\e\\");
}

/* XTGETTCAP for TN (the terminal name), hex encoded: DCS 1 + r 544e=... ST */
static bool probe_xtgettcap(struct term *t)
{
	return reply_matches(t, "\eP+q544e\e\\", "\eP1+r544e=", "\e\\");
}

/* --- OSC colours --- */

static bool probe_osc_4_query(struct term *t)
{
	return reply_matches(t, "\e]4;1;?\e\\", "\e]4;1;rgb:", "\e\\");
}

static bool probe_osc_4_set(struct term *t)
{
	feed(t, "\e]4;1;rgb:12/34/56\e\\");
	return reply_is(t, "\e]4;1;?\e\\", "\e]4;1;rgb:1212/3434/5656\e\\");
}

static bool probe_osc_10_11_query(struct term *t)
{
	return reply_matches(t, "\e]10;?\e\\", "\e]10;rgb:", "\e\\") &&
	       reply_matches(t, "\e]11;?\a", "\e]11;rgb:", "\a");
}

static bool probe_osc_10_11_set(struct term *t)
{
	feed(t, "\e]10;rgb:12/34/56\e\\\e]11;rgb:65/43/21\e\\");
	return reply_is(t, "\e]10;?\e\\", "\e]10;rgb:1212/3434/5656\e\\") &&
	       reply_is(t, "\e]11;?\e\\", "\e]11;rgb:6565/4343/2121\e\\");
}

static bool probe_osc_12_query(struct term *t)
{
	return reply_matches(t, "\e]12;?\e\\", "\e]12;rgb:", "\e\\");
}

/* OSC 104 and 110 put back what OSC 4 and 10 changed; a set that did not
 * take is a failure, so the probe never passes on a terminal that cannot set */
static bool probe_osc_104_110_111_reset(struct term *t)
{
	char c1[sizeof(t->reply)], fg[sizeof(t->reply)], bg[sizeof(t->reply)];

	clear_reply(t);
	feed(t, "\e]4;1;?\e\\");
	snprintf(c1, sizeof(c1), "%s", t->reply);
	clear_reply(t);
	feed(t, "\e]10;?\e\\");
	snprintf(fg, sizeof(fg), "%s", t->reply);
	clear_reply(t);
	feed(t, "\e]11;?\e\\");
	snprintf(bg, sizeof(bg), "%s", t->reply);

	feed(t, "\e]4;1;rgb:12/34/56\e\\\e]10;rgb:12/34/56\e\\\e]11;rgb:12/34/56\e\\");
	if (!reply_is(t, "\e]4;1;?\e\\", "\e]4;1;rgb:1212/3434/5656\e\\") ||
	    !reply_is(t, "\e]10;?\e\\", "\e]10;rgb:1212/3434/5656\e\\") ||
	    !reply_is(t, "\e]11;?\e\\", "\e]11;rgb:1212/3434/5656\e\\")) {
		char set[sizeof(why)];

		snprintf(set, sizeof(set), "%s", why);
		return fail("nothing to reset: %s", set);
	}

	feed(t, "\e]104;1\e\\\e]110\e\\\e]111\e\\");
	return reply_is(t, "\e]4;1;?\e\\", c1) && reply_is(t, "\e]10;?\e\\", fg) &&
	       reply_is(t, "\e]11;?\e\\", bg);
}

/* --- keyboard --- */

/* kitty keyboard protocol: CSI ? u reports the current flags as CSI ? flags u */
static bool probe_kitty_keyboard(struct term *t)
{
	return reply_matches(t, "\e[?u", "\e[?", "u");
}

/* xterm modifyOtherKeys level 2: Ctrl+1 is sent as CSI 27 ; 5 ; 49 ~ */
static bool probe_modify_other_keys(struct term *t)
{
	feed(t, "\e[>4;2m");
	clear_reply(t);
	tsm_vte_handle_keyboard(t->vte, KEY_1, '1', TSM_CONTROL_MASK, '1');
	if (strcmp(t->reply, "\e[27;5;49~"))
		return fail("Ctrl+1 under CSI > 4;2 m sent \"%s\", want \"\\e[27;5;49~\"",
			    shown(t->reply));
	return true;
}

/* --- mouse and paste --- */

static bool mouse_is(struct term *t, unsigned int x, unsigned int y, unsigned int button,
		     unsigned int event, const char *want)
{
	clear_reply(t);
	tsm_vte_handle_mouse(t->vte, x, y, x * 10, y * 10, button, event, 0);
	if (strcmp(t->reply, want))
		return fail("mouse event %u at %u,%u sent \"%s\", want \"%s\"", event, x, y,
			    shown(t->reply), shown(want));
	return true;
}

/* mode 1000 with the X10 encoding: CSI M, then button, column and row + 32 */
static bool probe_mouse_1000(struct term *t)
{
	feed(t, "\e[?1000h");
	return mouse_is(t, 2, 3, TSM_MOUSE_BUTTON_LEFT, TSM_MOUSE_EVENT_PRESSED, "\e[M #$");
}

static bool probe_mouse_1006(struct term *t)
{
	feed(t, "\e[?1000h\e[?1006h");
	return mouse_is(t, 2, 3, TSM_MOUSE_BUTTON_LEFT, TSM_MOUSE_EVENT_PRESSED,
			"\e[<0;3;4M") &&
	       mouse_is(t, 2, 3, TSM_MOUSE_BUTTON_LEFT, TSM_MOUSE_EVENT_RELEASED,
			"\e[<0;3;4m");
}

/* 1006 is an encoding, independent of which tracking mode came first */
static bool probe_mouse_1006_first(struct term *t)
{
	feed(t, "\e[?1006h\e[?1000h");
	return mouse_is(t, 2, 3, TSM_MOUSE_BUTTON_LEFT, TSM_MOUSE_EVENT_PRESSED,
			"\e[<0;3;4M");
}

/* any-event tracking reports motion with no button (35) in SGR */
static bool probe_mouse_1003_sgr(struct term *t)
{
	feed(t, "\e[?1003h\e[?1006h");
	return mouse_is(t, 2, 3, 0, TSM_MOUSE_EVENT_MOVED, "\e[<35;3;4M");
}

/* any-event tracking in the X10 encoding: motion with no button is 32 + 35 */
static bool probe_mouse_1003_x10(struct term *t)
{
	feed(t, "\e[?1003h");
	return mouse_is(t, 2, 3, 0, TSM_MOUSE_EVENT_MOVED, "\e[MC#$");
}

static bool probe_mouse_1016(struct term *t)
{
	feed(t, "\e[?1000h\e[?1016h");
	return mouse_is(t, 2, 3, TSM_MOUSE_BUTTON_LEFT, TSM_MOUSE_EVENT_PRESSED,
			"\e[<0;20;30M");
}

static bool probe_bracketed_paste(struct term *t)
{
	feed(t, "\e[?2004h");
	clear_reply(t);
	tsm_vte_paste(t->vte, "x");
	if (strcmp(t->reply, "\e[200~x\e[201~"))
		return fail("paste under ?2004h sent \"%s\", want \"\\e[200~x\\e[201~\"",
			    shown(t->reply));
	return true;
}

/* --- screen --- */

/* a line wrapped at 10 columns becomes one line again at 20 */
static bool probe_reflow(struct term *t)
{
	const char *line = "abcdefghijklmno";

	feed(t, line);
	if (tsm_screen_resize(t->con, 20, 4))
		return fail("resize failed");
	for (unsigned int x = 0; x < strlen(line); x++)
		if (cell(t, x, 0)->key != (uint64_t)line[x])
			return fail("after widening, row 0 column %u holds U+%04llX, want '%c'",
				    x, (unsigned long long)cell(t, x, 0)->key, line[x]);
	return true;
}

/* --- placeholders: no interface yet to observe the feature through --- */

static bool placeholder(const char *what)
{
	return fail("placeholder: %s; the change that adds it replaces this probe", what);
}

static bool probe_focus_1004(struct term *t)
{
	(void)t;
	return placeholder("libtsm has no call for kmscon to report focus in and out (CSI I, CSI O)");
}

static bool probe_osc_8(struct term *t)
{
	(void)t;
	return placeholder("cells carry no hyperlink, and OSC 8 is passed to kmscon, which drops it");
}

static bool probe_osc_52(struct term *t)
{
	(void)t;
	return placeholder("kmscon's OSC handler drops OSC 52");
}

static bool probe_osc_133(struct term *t)
{
	(void)t;
	return placeholder("kmscon's OSC handler drops OSC 133, and nothing records prompt marks");
}

static bool probe_bidi(struct term *t)
{
	(void)t;
	return placeholder("no bidirectional reordering anywhere in libtsm or kmscon");
}

static const struct probe {
	const char *name;
	bool (*run)(struct term *t);
	unsigned int cols;
} probes[] = {
	{"sgr-italic", probe_sgr_italic},
	{"sgr-truecolor", probe_sgr_truecolor},
	{"sgr-overline", probe_sgr_overline},
	{"sgr-super-subscript", probe_sgr_super_subscript},
	{"mode-2026-sync", probe_mode_2026_sync},
	{"decrqm", probe_decrqm},
	{"da1", probe_da1},
	{"da1-level", probe_da1_level},
	{"da2", probe_da2},
	{"da3", probe_da3},
	{"xtversion", probe_xtversion},
	{"xtgettcap", probe_xtgettcap},
	{"osc-4-query", probe_osc_4_query},
	{"osc-4-set", probe_osc_4_set},
	{"osc-10-11-query", probe_osc_10_11_query},
	{"osc-10-11-set", probe_osc_10_11_set},
	{"osc-12-query", probe_osc_12_query},
	{"osc-104-110-111-reset", probe_osc_104_110_111_reset},
	{"kitty-keyboard", probe_kitty_keyboard},
	{"modify-other-keys", probe_modify_other_keys},
	{"mouse-1000", probe_mouse_1000},
	{"mouse-1006", probe_mouse_1006},
	{"mouse-1006-first", probe_mouse_1006_first},
	{"mouse-1003-sgr", probe_mouse_1003_sgr},
	{"mouse-1003-x10", probe_mouse_1003_x10},
	{"mouse-1016", probe_mouse_1016},
	{"bracketed-paste", probe_bracketed_paste},
	{"focus-1004", probe_focus_1004},
	{"reflow", probe_reflow, 10},
	{"osc-8-hyperlink", probe_osc_8},
	{"osc-52-clipboard", probe_osc_52},
	{"osc-133-marks", probe_osc_133},
	{"bidi", probe_bidi},
};

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s --list | NAME\n", argv[0]);
		return 2;
	}
	for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		const struct probe *p = &probes[i];
		struct term *t;
		bool ok;

		if (!strcmp(argv[1], "--list")) {
			printf("%s\n", p->name);
			continue;
		}
		if (strcmp(argv[1], p->name))
			continue;
		t = term_new(p->cols ? p->cols : 80, 4);
		ok = p->run(t);
		term_free(t);
		if (!ok)
			printf("%s\n", why);
		return ok ? 0 : 1;
	}
	if (strcmp(argv[1], "--list")) {
		fprintf(stderr, "test_capabilities: no probe %s\n", argv[1]);
		return 2;
	}
	return 0;
}
