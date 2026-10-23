/*
 * The pty (ADR-103 step 3d). kmscon feeds the child's output to libtsm a
 * bounded amount of work at a time: bytes the input callback does not take
 * are held, nothing more is read while they are, and the pty hands them back
 * from an idle source on its own loop, in order and before anything read
 * later, then reads on with no new write needed (its fd is edge-triggered).
 * Closing drops them. The child's side is a pipe on the pty's real loop, so
 * no child runs.
 *
 * TIOCSWINSZ carries the text area in pixels from the cell size, clamped to
 * 16 bits, and a new cell size with the same size in cells still sets it. A
 * pseudo-terminal pair stands in for the child's.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/misc/pty.c"

static char seen[4 * KMSCON_NREAD];
static size_t n_seen;
static size_t take; /* the most the callback takes in one call */
static unsigned int calls;

static size_t input(struct kmscon_pty *pty, const char *u8, size_t len, void *data)
{
	size_t n = len < take ? len : take;

	(void)pty;
	(void)data;
	calls++;
	assert(n_seen + n <= sizeof(seen));
	memcpy(seen + n_seen, u8, n);
	n_seen += n;
	return n;
}

static void put(int fd, const void *s, size_t len)
{
	assert(write(fd, s, len) == (ssize_t)len);
}

static bool seen_is(const char *s)
{
	return n_seen == strlen(s) && !memcmp(seen, s, n_seen);
}

/* turns of the pty's loop, at most @max, until @want bytes are seen */
static void turn(struct kmscon_pty *pty, size_t want, unsigned int max)
{
	while (max-- && n_seen < want)
		kmscon_pty_dispatch(pty);
}

static void test_held(void)
{
	static char big[KMSCON_NREAD];
	struct kmscon_pty *pty;
	int fds[2];

	assert(kmscon_pty_new(&pty, input, NULL, NULL) == 0);
	assert(pipe2(fds, O_NONBLOCK) == 0);
	pty->fd = fds[0];
	assert(ev_eloop_new_fd(pty->eloop, &pty->efd, fds[0], EV_ET | EV_READABLE, pty_input,
			       pty) == 0);

	/* a callback that takes all: one turn reads everything */
	take = sizeof(seen);
	put(fds[1], "abc", 3);
	turn(pty, 3, 1);
	assert(seen_is("abc") && !pty->held_len && !pty->feeding);

	/* four bytes a call: one call a turn, in order, the pipe waiting */
	take = 4;
	n_seen = 0;
	calls = 0;
	put(fds[1], "defghijkl", 9);
	kmscon_pty_dispatch(pty);
	assert(calls == 1 && seen_is("defg") && pty->held_len && pty->feeding);
	put(fds[1], "MNO", 3);
	kmscon_pty_dispatch(pty);
	assert(calls == 2 && seen_is("defghijk"));
	turn(pty, 12, 10);
	assert(seen_is("defghijklMNO") && !pty->held_len && !pty->feeding);

	/*
	 * a read that fills io_buf, then a tail: the held bytes come over on
	 * later turns, and then the tail, with no new write to wake the fd
	 */
	memset(big, 'x', sizeof(big));
	take = 1000;
	n_seen = 0;
	put(fds[1], big, sizeof(big));
	put(fds[1], "tail", 4);
	kmscon_pty_dispatch(pty);
	assert(n_seen == 1000 && pty->held_len);
	turn(pty, sizeof(big) + 4, 100);
	assert(n_seen == sizeof(big) + 4 && !memcmp(seen + sizeof(big), "tail", 4));
	assert(!pty->held_len && !pty->feeding);

	/* closing drops what is held, and stops feeding it */
	take = 4;
	n_seen = 0;
	put(fds[1], "pqrstu", 6);
	kmscon_pty_dispatch(pty);
	assert(seen_is("pqrs") && pty->held_len && pty->feeding);
	kmscon_pty_close(pty);
	assert(!pty->held_len && !pty->feeding && pty->fd == -1);
	kmscon_pty_dispatch(pty);
	assert(seen_is("pqrs"));

	close(fds[1]);
	kmscon_pty_unref(pty);
}

static struct winsize size_of(int fd)
{
	struct winsize ws;

	assert(ioctl(fd, TIOCGWINSZ, &ws) == 0);
	return ws;
}

static void test_winsize(void)
{
	struct kmscon_pty *pty;
	struct winsize ws;
	int master = posix_openpt(O_RDWR | O_NOCTTY);

	assert(master >= 0 && kmscon_pty_new(&pty, input, NULL, NULL) == 0);
	pty->fd = master;

	kmscon_pty_resize(pty, 80, 25, 10, 19);
	ws = size_of(master);
	assert(ws.ws_col == 80 && ws.ws_row == 25 && ws.ws_xpixel == 800 && ws.ws_ypixel == 475);

	/* a new font that keeps the size in cells: the pixels change alone */
	kmscon_pty_resize(pty, 80, 25, 20, 40);
	ws = size_of(master);
	assert(ws.ws_col == 80 && ws.ws_row == 25 && ws.ws_xpixel == 1600 && ws.ws_ypixel == 1000);

	/* past 16 bits the pixels stop at 65535, not wrapping */
	kmscon_pty_resize(pty, 853, 270, 90, 300);
	ws = size_of(master);
	assert(ws.ws_col == 853 && ws.ws_row == 270 && ws.ws_xpixel == 65535 &&
	       ws.ws_ypixel == 65535);

	/* an unknown cell size: no pixels */
	kmscon_pty_resize(pty, 80, 25, 0, 0);
	ws = size_of(master);
	assert(ws.ws_xpixel == 0 && ws.ws_ypixel == 0);

	pty->fd = -1;
	close(master);
	kmscon_pty_unref(pty);
}

int main(void)
{
	test_held();
	test_winsize();
	printf("test_pty: ok\n");
	return 0;
}
