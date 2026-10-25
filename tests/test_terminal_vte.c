/*
 * The vte kmscon makes from its settings (src/terminal_vte.c), in every build:
 * kmscon's blink setting reaches libtsm, so DECRQM ?12 matches what the
 * renderer does, the default cursor style blinking only while blink is on.
 */

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

int main(void)
{
	cursor_blink(false, "\e[?12;2$y");
	cursor_blink(true, "\e[?12;1$y");
	printf("test_terminal_vte: ok\n");
	return 0;
}
