/*
 * A terminal's kitty graphics host (src/terminal_image.c, ADR-103's Notes of
 * 2026-10-24, kmscon: the pause and the helper's lifetime), end to end: the
 * client's bytes go into a real pseudo-terminal, kmscon's pty reads them into
 * a real vte made as terminal_new makes it, and the real kmscon-image-helper
 * serves the requests. Only the session's user is stood in for
 * (image_identity_find, whose policy test_image_identity covers), so each
 * case can choose it.
 *
 * - Each medium: inline PNG, inline zlib, a file, a temporary file (deleted)
 *   and shared memory (unlinked) are stored and placed with their pixels,
 *   and the client is answered OK.
 * - A refused t=t command is still discarded: the file is deleted, the client
 *   gets libtsm's refusal, and the helper keeps running.
 * - Before login (#167): every file medium, a discard among them, is answered
 *   at once without a helper, the temporary file left in place; inline PNG
 *   still decodes in a helper run as the pre-login user.
 * - The pause: the bytes after a command, a DA1 query among them, are parsed
 *   only after its reply, in order; the pty reads nothing meanwhile.
 * - Cancelling: a reset while a request is out (terminal_open's, or turning
 *   kitty graphics off) leaves no reply to the client, and the helper's late
 *   reply resumes the pty; closing kills the helper and drops the request.
 * - The helper's lifetime: started at the first request and kept for the
 *   next, killed at close and started again after; one that does not answer
 *   in time costs the client EBADF and the helper its life, and input goes on.
 * - kmscon's own input: asciicast playback feeds nothing while a request is
 *   out and loses no byte; a terminal in the background resumes it only when
 *   it comes back; terminal_image_input refuses bytes libtsm would drop.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "../src/misc/pty.c"
#include "image_identity.h"
#include "render/image_budget.h"
#include "terminal_image.h"
#include "terminal_vte.h"

static const char *real_helper;
static const char *fake_helper;

/*
 * The session's user, stood in for. Logged in: the test's own uid. Before
 * login: file media refused, inline media run as the test's uid with its gid
 * alone, standing in for kmscon-image (a test not run as root can start a
 * helper only as itself).
 */
static struct {
	bool prelogin;
	unsigned int calls;
	pid_t child;
	int owner_ret;
	uid_t owner;
	bool file;
} id;

int image_identity_find(pid_t child, int owner_ret, uid_t owner, bool file, struct image_user *user)
{
	id.calls++;
	id.child = child;
	id.owner_ret = owner_ret;
	id.owner = owner;
	id.file = file;
	/* filled even for a refusal, so only the return value can stop the hook */
	*user = (struct image_user){.uid = geteuid(), .gid_only = id.prelogin, .gid = getegid()};
	return id.prelogin && file ? -EACCES : 0;
}

/* the terminal: its screen, vte, pty and host, as terminal_new makes them */
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
static int client; /* the pseudo-terminal's slave: the client's side */
static unsigned int replied;

/* what the vte wrote back to the client */
static char out[4096];
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
	replied++;
}

static void setup(const char *helper)
{
	struct termios tio;
	int master;

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, 40, 10) == 0);
	assert(terminal_vte_new(&vte, &images, con, &conf, vte_write, NULL) == 0);
	assert(kmscon_pty_new(&pty, pty_read, NULL, NULL) == 0);

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

	assert(terminal_image_init(&ti, vte, pty, &conf, helper, on_replied, NULL) == 0);
	id = (typeof(id)){0};
	out_len = 0;
	out[0] = 0;
	replied = 0;
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

static void send_client(const char *s, size_t len)
{
	assert(write(client, s, len) == (ssize_t)len);
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* turns of the pty's loop, which carries the helper too, until @done or 5 s */
static void run_until(bool (*done)(void))
{
	uint64_t end = now_ms() + 5000;

	while (!done()) {
		if (now_ms() >= end)
			fprintf(stderr,
				"stuck: busy %d paused %d held %zu cursor %u out %zu replied %u\n",
				terminal_image_busy(&ti), pty->paused, pty->held_len,
				tsm_screen_get_cursor_x(con), out_len, replied);
		assert(now_ms() < end);
		ev_eloop_dispatch(pty->eloop, 10);
	}
}

static void run_for(unsigned int ms)
{
	uint64_t end = now_ms() + ms;

	while (now_ms() < end)
		ev_eloop_dispatch(pty->eloop, 10);
}

static bool idle(void)
{
	return !terminal_image_busy(&ti) && !pty->paused && !pty->held_len;
}

static bool request_out(void)
{
	return terminal_image_busy(&ti);
}

/* the cursor reached column cursor_want: the text the client sent is parsed */
static unsigned int cursor_want;

static bool cursor_there(void)
{
	return tsm_screen_get_cursor_x(con) == cursor_want;
}

static void run_to_column(unsigned int x)
{
	cursor_want = x;
	run_until(cursor_there);
}

static const char *b64(const void *data, size_t len)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	static char buf[4096];
	const uint8_t *p = data;
	size_t i, o = 0;

	assert((len + 2) / 3 * 4 < sizeof(buf));
	for (i = 0; i < len; i += 3) {
		uint32_t v = p[i] << 16 | (i + 1 < len ? p[i + 1] << 8 : 0) |
			     (i + 2 < len ? p[i + 2] : 0);

		buf[o++] = tbl[v >> 18 & 63];
		buf[o++] = tbl[v >> 12 & 63];
		buf[o++] = i + 1 < len ? tbl[v >> 6 & 63] : '=';
		buf[o++] = i + 2 < len ? tbl[v & 63] : '=';
	}
	buf[o] = 0;
	return buf;
}

/* a kitty command: APC G @keys ; @payload ST */
static void kitty(const char *keys, const char *payload)
{
	char cmd[8192];
	int n = snprintf(cmd, sizeof(cmd), "\033_G%s;%s\033\\", keys, payload);

	assert(n > 0 && (size_t)n < sizeof(cmd));
	send_client(cmd, n);
}

/* the client was answered @want for image @i, and nothing else */
static void answered(unsigned int i, const char *want)
{
	char exp[128];

	snprintf(exp, sizeof(exp), "\033_Gi=%u;%s\033\\", i, want);
	if (strcmp(out, exp)) {
		fprintf(stderr, "answered %zu bytes, not %s\n", out_len, want);
		assert(0);
	}
	out_len = 0;
	out[0] = 0;
}

static bool have_answer(void)
{
	return out_len && out[out_len - 1] == '\\';
}

/* 2 x 1 opaque pixels, the f=32 layout: R, G, B, A */
static const uint8_t px[8] = {0x11, 0x22, 0x33, 0xff, 0x44, 0x55, 0x66, 0xff};

/* the newest placement's image holds px, premultiplied ARGB */
static void placed_px(void)
{
	const struct tsm_frame *f = tsm_screen_frame(con);
	const struct tsm_frame_placement *pl;
	struct tsm_frame_image img;

	assert(f->placement_count);
	pl = tsm_frame_placement_at(f, f->placement_count - 1);
	assert(tsm_screen_frame_image(con, pl->image_key, &img) == 0);
	assert(img.width == 2 && img.height == 1);
	assert(img.pixels[0] == 0xff112233 && img.pixels[1] == 0xff445566);
}

static void png_chunk(uint8_t **p, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t *c = *p;
	uint32_t crc;

	c[0] = len >> 24;
	c[1] = len >> 16;
	c[2] = len >> 8;
	c[3] = len;
	memcpy(c + 4, type, 4);
	memcpy(c + 8, data, len);
	crc = crc32(0, c + 4, len + 4);
	c[8 + len] = crc >> 24;
	c[9 + len] = crc >> 16;
	c[10 + len] = crc >> 8;
	c[11 + len] = crc;
	*p += 12 + len;
}

/* px as a 2 x 1 RGBA PNG */
static size_t make_png(uint8_t *buf)
{
	static const uint8_t ihdr[13] = {0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0};
	uint8_t raw[9] = {0}, z[64], *p = buf;
	uLongf zlen = sizeof(z);

	memcpy(raw + 1, px, sizeof(px));
	assert(compress(z, &zlen, raw, sizeof(raw)) == Z_OK);
	memcpy(p, "\x89PNG\r\n\x1a\n", 8);
	p += 8;
	png_chunk(&p, "IHDR", ihdr, sizeof(ihdr));
	png_chunk(&p, "IDAT", z, zlen);
	png_chunk(&p, "IEND", NULL, 0);
	return p - buf;
}

static char tmpdir[] = "/tmp/kmscon-terminal-image-XXXXXX";

/* a file under the test's directory holding px; its path */
static const char *file_with_px(const char *name)
{
	static char path[256];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	assert(fd >= 0 && write(fd, px, sizeof(px)) == sizeof(px));
	close(fd);
	return path;
}

static bool exists(const char *path)
{
	return !access(path, F_OK);
}

static void test_media(void)
{
	uint8_t png[128], z[64];
	uLongf zlen = sizeof(z);
	char shm[64];
	const char *path;
	pid_t helper;
	int fd;

	setup(real_helper);

	/* inline PNG */
	kitty("a=T,f=100,i=1,z=1,C=1", b64(png, make_png(png)));
	run_until(have_answer);
	answered(1, "OK");
	assert(tsm_screen_test_image_count(con) == 1);
	placed_px();
	assert(id.calls == 1 && !id.file && id.child == 4242);
	assert(id.owner_ret == 0 && id.owner == geteuid());
	helper = image_host_pid(ti.host);
	assert(helper > 0 && replied == 1);
	run_until(idle);

	/* inline zlib */
	assert(compress(z, &zlen, px, sizeof(px)) == Z_OK);
	kitty("a=T,f=32,s=2,v=1,o=z,i=2,z=2,C=1", b64(z, zlen));
	run_until(have_answer);
	answered(2, "OK");
	placed_px();

	/* a file, read and left */
	path = file_with_px("image.rgba");
	kitty("a=T,t=f,f=32,s=2,v=1,i=3,z=3,C=1", b64(path, strlen(path)));
	run_until(have_answer);
	answered(3, "OK");
	placed_px();
	assert(exists(path) && id.file);

	/* a temporary file, deleted once read */
	path = file_with_px("tty-graphics-protocol-media");
	kitty("a=T,t=t,f=32,s=2,v=1,i=4,z=4,C=1", b64(path, strlen(path)));
	run_until(have_answer);
	answered(4, "OK");
	placed_px();
	assert(!exists(path));

	/* shared memory, unlinked once read */
	snprintf(shm, sizeof(shm), "/kmscon-test-%d", (int)getpid());
	fd = shm_open(shm, O_RDWR | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && write(fd, px, sizeof(px)) == sizeof(px));
	close(fd);
	kitty("a=T,t=s,f=32,s=2,v=1,i=5,z=5,C=1", b64(shm, strlen(shm)));
	run_until(have_answer);
	answered(5, "OK");
	placed_px();
	fd = shm_open(shm, O_RDONLY, 0);
	assert(fd < 0 && errno == ENOENT);

	/* one helper served them all, its user found again for each */
	assert(tsm_screen_test_image_count(con) == 5 && id.calls == 5);
	assert(image_host_pid(ti.host) == helper && replied == 5);
	run_until(idle);
	teardown();
}

/*
 * a=f with t=t, animation left to step 5, is refused, and its file still
 * goes: the spec gives its removal to the terminal
 */
static void test_discard(void)
{
	const char *path;
	pid_t helper;

	setup(real_helper);
	path = file_with_px("tty-graphics-protocol-discard");
	kitty("a=f,t=t,f=32,s=2,v=1,i=6", b64(path, strlen(path)));
	run_until(have_answer);
	answered(6, "EINVAL:invalid argument");
	assert(!exists(path) && id.calls == 1 && id.file);
	assert(tsm_screen_test_image_count(con) == 0);
	helper = image_host_pid(ti.host);
	assert(helper > 0);
	run_until(idle);
	/* the helper stays for the next request */
	path = file_with_px("image.rgba");
	kitty("a=T,t=f,f=32,s=2,v=1,i=7,C=1", b64(path, strlen(path)));
	run_until(have_answer);
	answered(7, "OK");
	assert(image_host_pid(ti.host) == helper);
	teardown();
}

/*
 * #167: the hook acts on a refusal before login. Each file medium is
 * answered in the same input call, no helper starts and the pty never
 * pauses; a refused t=t command and a valid one both leave their file.
 */
static void test_prelogin(void)
{
	uint8_t png[128];
	char p1[256], p2[256], p3[256];

	setup(real_helper);
	id.prelogin = true;

	strcpy(p1, file_with_px("image.rgba"));
	kitty("a=T,t=f,f=32,s=2,v=1,i=8,C=1", b64(p1, strlen(p1)));
	strcpy(p2, file_with_px("tty-graphics-protocol-prelogin"));
	kitty("a=T,t=t,f=32,s=2,v=1,i=9,C=1", b64(p2, strlen(p2)));
	strcpy(p3, file_with_px("tty-graphics-protocol-prelogin-discard"));
	kitty("a=f,t=t,f=32,s=2,v=1,i=10", b64(p3, strlen(p3)));
	send_client("\033[c", 3);
	ev_eloop_dispatch(pty->eloop, 100);
	/* one turn read and answered all four, in order, with no request out */
	assert(strstr(out, "\033_Gi=8;EBADF:Failed to read image file\033\\"
			   "\033_Gi=9;EBADF:Failed to read image file\033\\"
			   "\033_Gi=10;EINVAL:invalid argument\033\\"
			   "\033[?") == out);
	assert(id.calls == 3 && id.file);
	assert(!image_host_pid(ti.host) && !terminal_image_busy(&ti) && !pty->paused);
	assert(exists(p1) && exists(p2) && exists(p3));
	assert(tsm_screen_test_image_count(con) == 0);
	out_len = 0;

	/* inline data still decodes, as the pre-login user */
	kitty("a=T,f=100,i=11,C=1", b64(png, make_png(png)));
	run_until(have_answer);
	answered(11, "OK");
	assert(!id.file && image_host_pid(ti.host) > 0);
	placed_px();
	teardown();
}

/*
 * The bytes after a command wait for its reply: a put of the image, text and
 * DA1 in the same write are parsed after it, in order, and nothing more is
 * read from the pty meanwhile.
 */
static void test_pause(void)
{
	char buf[512];
	const char *path = file_with_px("image.rgba");
	int n;

	setup(real_helper);
	n = snprintf(buf, sizeof(buf), "\033_Ga=t,t=f,f=32,s=2,v=1,i=12;%s\033\\",
		     b64(path, strlen(path)));
	n += snprintf(buf + n, sizeof(buf) - n, "\033_Ga=p,i=12,C=1;\033\\AB\033[c");
	send_client(buf, n);
	run_until(request_out);
	/*
	 * the request is out: the rest, held or still unread, is unparsed and
	 * nothing is answered; later output waits as well
	 */
	assert(pty->paused && !out_len && tsm_screen_get_cursor_x(con) == 0);
	send_client("CD", 2);
	ev_eloop_dispatch(pty->eloop, 20);
	assert(pty->paused && !out_len && tsm_screen_get_cursor_x(con) == 0);

	run_until(idle);
	/* the transmission's OK, then the put's, then DA1; then AB and CD */
	assert(strstr(out, "\033_Gi=12;OK\033\\\033_Gi=12;OK\033\\\033[?") == out);
	run_to_column(4);
	assert(tsm_screen_test_image_count(con) == 1 && replied == 1);
	placed_px();
	teardown();
}

/*
 * A reset while a request is out, as terminal_open's hard reset or a
 * keyboard reset makes it, drops the request with no reply; the helper's
 * reply comes later, libtsm answers -ENOENT, and the pty resumes all the
 * same. Turning kitty graphics off, as a display without placements does
 * (image_budget.h), cancels the same way.
 */
static void test_cancel(void)
{
	const char *path = file_with_px("image.rgba");

	setup(real_helper);
	kitty("a=T,t=f,f=32,s=2,v=1,i=13,C=1", b64(path, strlen(path)));
	send_client("AB", 2);
	run_until(request_out);
	tsm_vte_hard_reset(vte);
	/*
	 * KNOWN: aaronsb/libtsm#53. The request was taken at the ESC of its
	 * ST, and the reset leaves the parser in ground, so the ST's final
	 * backslash prints before AB. No kmscon path resets and keeps the
	 * pty's held bytes; this flips red when libtsm swallows it.
	 */
	run_to_column(3);
	run_until(idle);
	assert(!out_len && replied == 1);
	assert(tsm_screen_test_image_count(con) == 0);

	kitty("a=T,t=f,f=32,s=2,v=1,i=14,C=1", b64(path, strlen(path)));
	run_until(request_out);
	image_budget_terminal_display(&images, false, true);
	run_until(idle);
	assert(!out_len && tsm_screen_test_image_count(con) == 0);
	/* kitty graphics off: a query gets no reply */
	kitty("a=q,i=15,s=1,v=1,f=32", "AAAAAA==");
	run_for(50);
	assert(!out_len && !terminal_image_busy(&ti));
	image_budget_terminal_display(&images, false, false);
	kitty("a=q,i=15,s=1,v=1,f=32", "AAAAAA==");
	run_until(have_answer);
	answered(15, "OK");
	teardown();
}

/*
 * Closing the terminal while a request is out kills the helper and drops the
 * request: no reply reaches the client, libtsm takes input again, and the
 * next request starts another helper.
 */
static void test_close(void)
{
	const char *path = file_with_px("image.rgba");
	pid_t first;

	setup(real_helper);
	kitty("a=T,t=f,f=32,s=2,v=1,i=16,C=1", b64(path, strlen(path)));
	run_until(request_out);
	first = image_host_pid(ti.host);
	assert(first > 0);
	terminal_image_close(&ti);
	assert(!terminal_image_busy(&ti) && !image_host_pid(ti.host));
	/* what libtsm takes now is parsed, not dropped as a host error */
	assert(terminal_image_input(&ti, "\033[c", 3) && strstr(out, "\033[?") == out);
	out_len = 0;
	run_for(50);
	assert(!out_len && replied == 0 && tsm_screen_test_image_count(con) == 0);

	/* the session goes on (a restart reopens the pty, its pause cleared) */
	assert(pty->paused);
	kmscon_pty_resume(pty);
	run_until(idle);
	kitty("a=T,t=f,f=32,s=2,v=1,i=17,C=1", b64(path, strlen(path)));
	run_until(have_answer);
	answered(17, "OK");
	assert(image_host_pid(ti.host) > 0 && image_host_pid(ti.host) != first);
	teardown();
}

/*
 * A helper that never answers: after the timeout the client gets EBADF, the
 * helper is killed, and the bytes after the command are parsed.
 */
static void test_timeout(void)
{
	setup(fake_helper);
	image_host_set_timeout(ti.host, 200);
	kitty("a=T,t=f,f=32,s=2,v=1,i=18,C=1", b64("hang", 4));
	send_client("AB", 2);
	run_until(request_out);
	assert(image_host_pid(ti.host) > 0);
	run_until(have_answer);
	answered(18, "EBADF:Failed to read image file");
	run_to_column(2);
	assert(!image_host_pid(ti.host) && !terminal_image_busy(&ti));
	teardown();
}

/* what playback fed the vte */
static char played[64];
static size_t n_played;

static void playback_write(const char *u8, size_t len, void *data)
{
	(void)data;
	if (terminal_image_input(&ti, u8, len)) {
		assert(n_played + len < sizeof(played));
		memcpy(played + n_played, u8, len);
		n_played += len;
	}
}

static bool played_all(void)
{
	return n_played == 4;
}

/*
 * Playback (src/terminal.c's asciinema_write) feeds nothing while a request
 * is out and resumes with the reply, losing no byte. A terminal sent to the
 * background meanwhile resumes it only when it returns. terminal_image_input
 * refuses what libtsm would drop.
 */
static void test_playback(void)
{
	static const char cast[] = "{\"version\":2,\"width\":80,\"height\":24}\n"
				   "[0.3,\"o\",\"P\"]\n"
				   "[0.31,\"o\",\"Q\"]\n"
				   "[0.32,\"o\",\"R\"]\n"
				   "[0.33,\"o\",\"S\"]\n";
	struct kmscon_asciinema *player;
	char path[256];
	int fd;

	setup(fake_helper);
	image_host_set_timeout(ti.host, 1000);
	snprintf(path, sizeof(path), "%s/play.cast", tmpdir);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	assert(fd >= 0 && write(fd, cast, sizeof(cast) - 1) == (ssize_t)sizeof(cast) - 1);
	close(fd);
	assert(kmscon_asciinema_new(&player, pty->eloop, path, false, playback_write, NULL) == 0);
	terminal_image_set_playback(&ti, player);
	terminal_image_play(&ti, true);
	n_played = 0;

	/* the first event falls due 300 ms after the start */
	assert(kmscon_asciinema_start(player));
	kitty("a=T,t=f,f=32,s=2,v=1,i=19,C=1", b64("hang", 4));
	run_until(request_out);
	assert(n_played == 0);
	/* the events fall due while the request is out: none is fed */
	assert(!terminal_image_input(&ti, "X", 1));
	run_for(500);
	assert(terminal_image_busy(&ti) && n_played == 0);
	/* sent to the background meanwhile: the reply does not resume it */
	terminal_image_play(&ti, false);
	run_until(have_answer);
	answered(19, "EBADF:Failed to read image file");
	run_for(200);
	assert(n_played == 0);
	/* back in the foreground: every event, in order */
	terminal_image_play(&ti, true);
	run_until(played_all);
	assert(!memcmp(played, "PQRS", 4) && tsm_screen_get_cursor_x(con) == 4);

	terminal_image_set_playback(&ti, NULL);
	kmscon_asciinema_free(player);
	unlink(path);
	teardown();
}

int main(int argc, char **argv)
{
	char cmd[512];

	assert(argc == 3);
	/* the helper is started by its absolute path */
	real_helper = realpath(argv[1], NULL);
	fake_helper = realpath(argv[2], NULL);
	assert(real_helper && fake_helper);
	assert(mkdtemp(tmpdir));

	test_media();
	test_discard();
	test_prelogin();
	test_pause();
	test_cancel();
	test_close();
	test_timeout();
	test_playback();

	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(!system(cmd));
	assert(image_budget_screens() == 0);
	printf("test_terminal_image: ok\n");
	return 0;
}
