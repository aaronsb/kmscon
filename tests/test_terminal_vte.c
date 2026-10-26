/*
 * The vte kmscon makes from its settings (src/terminal_vte.c), in every build:
 * kmscon's blink setting reaches libtsm, so DECRQM ?12 matches what the
 * renderer does, the default cursor style blinking only while blink is on.
 * And the decisions terminal.c makes for a terminal's input modes, against a
 * real libtsm vte: a key repeat is dropped while the program has reset DECARM
 * (mode 8), and a real key press never is; a change of focus is reported once
 * while the program has set mode 1004, and a repeat of the last focus is not
 * reported again; a pointer at the text area's edge reaches libtsm inside it.
 * XTVERSION names kmscon and its version, and XTGETTCAP answers from the
 * kmscon entry's table only when the session's TERM is kmscon.
 */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/render/image_budget.c"
#include "../src/terminal_vte.c"

static char out[256];
static size_t out_len;

static void vte_write(struct tsm_vte *v, const char *u8, size_t len, void *data)
{
	(void)v;
	(void)data;
	assert(out_len + len < sizeof(out));
	memcpy(out + out_len, u8, len);
	out_len += len;
	out[out_len] = 0;
}

/* DECRQM ?12's reply for a vte made with blink @blink */
static void cursor_blink(bool blink, const char *want)
{
	struct kmscon_conf_t conf = {
		.blink = blink,
		.grapheme_width = TSM_WIDTH_UNICODE,
	};
	struct image_budget_terminal images = {0};
	struct tsm_screen *con;
	struct tsm_vte *vte;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	assert(terminal_vte_new(&vte, &images, con, &conf, vte_write, NULL) == 0);
	out_len = 0;
	out[0] = 0;
	tsm_vte_input(vte, "\e[?12$p", 7);
	assert(strstr(out, want));
	image_budget_remove_terminal(&images);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

/* @query's reply from a vte made with TERM @term */
static void identify_reply(const char *term, const char *query, const char *want)
{
	struct kmscon_conf_t conf = {
		.term = (char *)term,
		.blink = true,
		.grapheme_width = TSM_WIDTH_UNICODE,
	};
	struct image_budget_terminal images = {0};
	struct tsm_screen *con;
	struct tsm_vte *vte;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	assert(terminal_vte_new(&vte, &images, con, &conf, vte_write, NULL) == 0);
	out_len = 0;
	out[0] = 0;
	tsm_vte_input(vte, query, strlen(query));
	if (strcmp(out, want)) {
		fprintf(stderr, "TERM=%s, %s: replied \"%s\", want \"%s\"\n",
			term ? term : "(none)", query + 1, out + 1, want + 1);
		assert(!"identify_reply");
	}
	image_budget_remove_terminal(&images);
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
}

/* XTVERSION and XTGETTCAP: kmscon's name, and its entry's table for TERM=kmscon */
static void test_identify(void)
{
#if defined(TSM_VTE_XTVERSION) && defined(TSM_VTE_TERMCAP)
	static const char version[] = "\eP>|kmscon(" BUILD_VERSION ")\e\\";

	identify_reply("kmscon", "\e[>q", version);
	identify_reply("xterm-256color", "\e[>q", version);
	/* TN and Co, xterm's names outside a description, and two of the entry's
	 * strings, escapes decoded and % sequences kept */
	identify_reply("kmscon", "\eP+q544e;436f\e\\", "\eP1+r544e=6B6D73636F6E;436f=323536\e\\");
	identify_reply("kmscon", "\eP+q536d756c78\e\\",
		       "\eP1+r536d756c78=1B5B343A25703125646D\e\\");
	identify_reply("kmscon", "\eP+q6b63757531\e\\", "\eP1+r6b63757531=1B5B41\e\\");
	/* a boolean, and lines, which the table leaves out */
	identify_reply("kmscon", "\eP+q5463\e\\", "\eP1+r5463\e\\");
	identify_reply("kmscon", "\eP+q6c696e6573\e\\", "\eP0+r\e\\");
	/* another TERM, or none: no table */
	identify_reply("xterm-256color", "\eP+q544e\e\\", "\eP0+r\e\\");
	identify_reply(NULL, "\eP+q544e\e\\", "\eP0+r\e\\");
#endif
}

static char reply[256];
static size_t reply_len;

static void on_write(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	(void)vte;
	(void)data;
	if (reply_len + len >= sizeof(reply))
		len = sizeof(reply) - 1 - reply_len;
	memcpy(reply + reply_len, u8, len);
	reply_len += len;
	reply[reply_len] = 0;
}

static void clear_reply(void)
{
	reply_len = 0;
	reply[0] = 0;
}

struct term {
	struct tsm_screen *con;
	struct tsm_vte *vte;
};

static void term_init(struct term *t)
{
	assert(tsm_screen_new(&t->con, NULL, NULL) == 0);
	assert(tsm_screen_resize(t->con, 40, 10) == 0);
	assert(tsm_vte_new(&t->vte, t->con, on_write, NULL, NULL, NULL) == 0);
	clear_reply();
}

static void term_free(struct term *t)
{
	tsm_vte_unref(t->vte);
	tsm_screen_unref(t->con);
}

static void feed(struct term *t, const char *s)
{
	tsm_vte_input(t->vte, s, strlen(s));
}

/* DECARM (xterm ctlseqs): set by default, so a held key repeats */
static void test_decarm(void)
{
	struct term t;

	term_init(&t);
	assert(!terminal_vte_drop_repeat(t.vte, true));
	assert(!terminal_vte_drop_repeat(t.vte, false));

	/* reset: the timer's repeats stop, a press still goes through */
	feed(&t, "\e[?8l");
	assert(terminal_vte_drop_repeat(t.vte, true));
	assert(!terminal_vte_drop_repeat(t.vte, false));

	feed(&t, "\e[?8h");
	assert(!terminal_vte_drop_repeat(t.vte, true));

	/* DECSTR sets it again */
	feed(&t, "\e[?8l\e[!p");
	assert(!terminal_vte_drop_repeat(t.vte, true));
	term_free(&t);
}

/* focus events, mode 1004 (xterm ctlseqs, FocusIn/FocusOut) */
static void test_focus(void)
{
	struct term t;
	bool last = false;

	term_init(&t);
	/* mode 1004 reset: the focus is kept, nothing is written */
	assert(!terminal_vte_focus(t.vte, &last, true));
	assert(last && reply_len == 0);

	/* setting the mode reports nothing; the next change does */
	feed(&t, "\e[?1004h");
	assert(reply_len == 0);
	assert(!terminal_vte_focus(t.vte, &last, true));
	assert(reply_len == 0);
	assert(terminal_vte_focus(t.vte, &last, false));
	assert(!strcmp(reply, "\e[O") && !last);
	clear_reply();
	assert(!terminal_vte_focus(t.vte, &last, false));
	assert(reply_len == 0);
	assert(terminal_vte_focus(t.vte, &last, true));
	assert(!strcmp(reply, "\e[I") && last);
	clear_reply();

	/* reset again: changes are kept and not reported */
	feed(&t, "\e[?1004l");
	assert(!terminal_vte_focus(t.vte, &last, false));
	assert(reply_len == 0 && !last);
	term_free(&t);
}

/* the pointer can sit at the screen's width or height, past the grid's;
 * libtsm is given the grid's last pixel */
static void test_pointer(void)
{
	/* 100 cells of 8 pixels: the last pixel is 799 */
	assert(terminal_vte_pointer(0, 100, 8) == 0);
	assert(terminal_vte_pointer(799, 100, 8) == 799);
	assert(terminal_vte_pointer(800, 100, 8) == 799);
	assert(terminal_vte_pointer(-3, 100, 8) == 0);
	assert(terminal_vte_pointer(5, 0, 8) == 0);
	/* a 3840x1080 screen holding 274x38 cells of 14x28: the grid ends at
	 * 3836x1064, and the screen's last pixel, 3839x1079, is clamped to it */
	assert(terminal_vte_pointer(3839, 274, 14) == 3835);
	assert(terminal_vte_pointer(1079, 38, 28) == 1063);
	assert(terminal_vte_pointer(3835, 274, 14) == 3835);
}

int main(void)
{
	cursor_blink(false, "\e[?12;2$y");
	cursor_blink(true, "\e[?12;1$y");
	test_decarm();
	test_focus();
	test_pointer();
	test_identify();
	printf("test_terminal_vte: ok\n");
	return 0;
}
