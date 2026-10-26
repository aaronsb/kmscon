/*
 * The decisions terminal.c makes for a terminal's input modes
 * (src/terminal_vte.c), against a real libtsm vte: a key repeat is dropped
 * while the program has reset DECARM (mode 8), and a real key press never is;
 * a change of focus is reported once while the program has set mode 1004, and
 * a repeat of the last focus is not reported again; a pointer at the text
 * area's edge reaches libtsm inside it.
 */

#undef NDEBUG
#include <assert.h>
#include <libtsm.h>
#include <stdio.h>
#include <string.h>
#include "terminal_vte.h"

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

/* the pointer can sit at the width or height; libtsm is given the last pixel */
static void test_pointer(void)
{
	assert(terminal_vte_pointer(0, 800) == 0);
	assert(terminal_vte_pointer(799, 800) == 799);
	assert(terminal_vte_pointer(800, 800) == 799);
	assert(terminal_vte_pointer(-3, 800) == 0);
	assert(terminal_vte_pointer(5, 0) == 0);
}

int main(void)
{
	test_decarm();
	test_focus();
	test_pointer();
	printf("test_terminal_vte: ok\n");
	return 0;
}
