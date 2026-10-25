/*
 * A terminal in a build without images (src/terminal_image_none.c, ADR-103's
 * Notes of 2026-10-25, The images option), end to end: asciicast playback
 * through kmscon's pty and a vte made as terminal_new makes it, with no image
 * host. Playback feeds every event in order while the terminal is in the
 * foreground, pauses when it leaves it and resumes, losing nothing, when it
 * returns, and pauses on close. Nothing is ever busy, and a kitty command or
 * a sixel string the client sends leaves no picture and no reply.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "../src/misc/pty.c"
#include "render/image_budget.h"
#include "terminal_image.h"
#include "terminal_vte.h"

/* kmscon's defaults, with the image settings a build without images ignores */
static struct kmscon_conf_t conf = {
	.sixel = true,
	.kitty_graphics = true,
	.max_graphic_size = {1000, 1000},
	.num_color_registers = 256,
	.grapheme_width = TSM_WIDTH_UNICODE,
};
static struct tsm_screen *con;
static struct tsm_vte *vte;
static struct image_budget_terminal images;
static struct kmscon_pty *pty;
static struct terminal_image ti;
static int client;
static struct ev_eloop *loop;

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

/* src/terminal.c's pty_input */
static size_t pty_read(struct kmscon_pty *p, const char *u8, size_t len, void *data)
{
	(void)p;
	(void)data;
	return len ? tsm_vte_input_bounded(vte, u8, len) : 0;
}

static void on_replied(void *data)
{
	(void)data;
	assert(!"no image host to reply");
}

static void setup(void)
{
	struct termios tio;
	int master;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	assert(tsm_screen_set_cell_size(con, 8, 16) == 0);
	assert(terminal_vte_new(&vte, &images, con, &conf, vte_write, NULL) == 0);
	assert(kmscon_pty_new(&pty, pty_read, NULL, NULL) == 0);
	loop = pty->eloop;

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	assert(master >= 0 && !grantpt(master) && !unlockpt(master));
	client = open(ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
	assert(client >= 0);
	assert(!tcgetattr(client, &tio));
	cfmakeraw(&tio);
	assert(!tcsetattr(client, TCSANOW, &tio));
	pty->fd = master;
	pty->child = 4242;
	assert(ev_eloop_new_fd(pty->eloop, &pty->efd, master, EV_ET | EV_READABLE, pty_input,
			       pty) == 0);
	assert(terminal_image_init(&ti, vte, pty, &conf, "/nonexistent", on_replied, NULL) == 0);
	out_len = 0;
	out[0] = 0;
}

static void teardown(void)
{
	terminal_image_close(&ti);
	kmscon_pty_close(pty);
	terminal_image_destroy(&ti);
	close(client);
	kmscon_pty_unref(pty);
	image_budget_free_terminal(&images);
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void run_until(bool (*done)(void))
{
	uint64_t end = now_ms() + 5000;

	while (!done()) {
		assert(now_ms() < end);
		ev_eloop_dispatch(loop, 10);
	}
}

static void run_for(unsigned int ms)
{
	uint64_t end = now_ms() + ms;

	while (now_ms() < end)
		ev_eloop_dispatch(loop, 10);
}

static unsigned int cursor_want;

static bool cursor_there(void)
{
	return tsm_screen_get_cursor_x(con) == cursor_want;
}

/* the client's sixel string and kitty commands between two characters */
static void test_client_images(void)
{
	static const char s[] = "a\033Pq#0;2;100;0;0#0~\033\\"
				"\033_Ga=T,f=24,s=1,v=1;AAAA\033\\"
				"\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\b";

	setup();
	assert(write(client, s, sizeof(s) - 1) == (ssize_t)sizeof(s) - 1);
	cursor_want = 2;
	run_until(cursor_there);
	assert(!terminal_image_busy(&ti));
	assert(out_len == 0);
	assert(tsm_screen_get_image_bytes(con) == 0);
	teardown();
}

static char played[64];
static size_t n_played;
static char tmpdir[] = "/tmp/kmscon-terminal-image-none-XXXXXX";
static char cast_path[256];

static void playback_write(const char *u8, size_t len, void *data)
{
	(void)data;
	assert(terminal_image_input(&ti, u8, len));
	assert(n_played + len < sizeof(played));
	memcpy(played + n_played, u8, len);
	n_played += len;
}

static unsigned int want_played;

static bool played_enough(void)
{
	return n_played >= want_played;
}

/* events 100 ms apart, the first due 100 ms after the start */
static struct kmscon_asciinema *player_new(void)
{
	static const char cast[] = "{\"version\":2,\"width\":80,\"height\":24}\n"
				   "[0.1,\"o\",\"P\"]\n"
				   "[0.2,\"o\",\"Q\"]\n"
				   "[0.6,\"o\",\"R\"]\n"
				   "[0.7,\"o\",\"S\"]\n";
	struct kmscon_asciinema *player;
	int fd;

	snprintf(cast_path, sizeof(cast_path), "%s/play.cast", tmpdir);
	fd = open(cast_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	assert(fd >= 0 && write(fd, cast, sizeof(cast) - 1) == (ssize_t)sizeof(cast) - 1);
	close(fd);
	assert(kmscon_asciinema_new(&player, loop, cast_path, false, playback_write, NULL) == 0);
	terminal_image_set_playback(&ti, player);
	n_played = 0;
	return player;
}

static void player_free(struct kmscon_asciinema *player)
{
	terminal_image_set_playback(&ti, NULL);
	kmscon_asciinema_free(player);
	unlink(cast_path);
}

static void test_playback(void)
{
	struct kmscon_asciinema *player;

	setup();
	player = player_new();
	terminal_image_play(&ti, true);
	assert(kmscon_asciinema_start(player));
	want_played = 2;
	run_until(played_enough);
	assert(n_played == 2 && !memcmp(played, "PQ", 2));

	/* in the background past R's and S's times: nothing is fed */
	terminal_image_play(&ti, false);
	run_for(800);
	assert(n_played == 2);

	/* back: the rest, in order */
	terminal_image_play(&ti, true);
	want_played = 4;
	run_until(played_enough);
	assert(!memcmp(played, "PQRS", 4) && tsm_screen_get_cursor_x(con) == 4);
	player_free(player);
	teardown();

	/* close pauses playback */
	setup();
	player = player_new();
	terminal_image_play(&ti, true);
	assert(kmscon_asciinema_start(player));
	terminal_image_close(&ti);
	run_for(400);
	assert(n_played == 0);
	player_free(player);
	teardown();
}

int main(void)
{
	char cmd[512];

	assert(mkdtemp(tmpdir));
	test_client_images();
	test_playback();
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(!system(cmd));
	assert(image_budget_screens() == 0);
	printf("test_terminal_image_none: ok\n");
	return 0;
}
