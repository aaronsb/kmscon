/*
 * The pty's held output (ADR-103 step 3d): kmscon feeds the child's output to
 * libtsm a bounded amount of work at a time. Bytes the input callback does not
 * take are held, nothing more is read while they are, and they reach the
 * callback again in order, before anything read later. Closing drops them.
 * The child's side is a pipe here, so no child runs.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/misc/pty.c"

static char seen[256];
static size_t n_seen;
static size_t take; /* the most the callback takes in one call */
static unsigned int calls;

static size_t input(struct kmscon_pty *pty, const char *u8, size_t len, void *data)
{
	size_t n = len < take ? len : take;

	(void)pty;
	(void)data;
	calls++;
	assert(n_seen + n < sizeof(seen));
	memcpy(seen + n_seen, u8, n);
	n_seen += n;
	return n;
}

static void put(int fd, const char *s)
{
	assert(write(fd, s, strlen(s)) == (ssize_t)strlen(s));
}

static bool seen_is(const char *s)
{
	return n_seen == strlen(s) && !memcmp(seen, s, n_seen);
}

int main(void)
{
	struct kmscon_pty *pty;
	int fds[2];

	assert(kmscon_pty_new(&pty, input, NULL, NULL) == 0);
	assert(pipe(fds) == 0);
	assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
	pty->fd = fds[0];

	/* a callback that takes all reads on until nothing is left */
	take = sizeof(seen);
	put(fds[1], "abc");
	read_buf(pty);
	assert(seen_is("abc") && !kmscon_pty_held(pty));

	/* one that takes four bytes a call: the rest of the read is held */
	take = 4;
	put(fds[1], "defghijkl");
	read_buf(pty);
	assert(calls == 2 && seen_is("abcdefg") && kmscon_pty_held(pty));

	/* while bytes are held, more output waits in the pipe */
	put(fds[1], "MNO");
	read_buf(pty);
	assert(calls == 2 && kmscon_pty_held(pty));

	/* the held bytes go first, four at a time, then reading resumes */
	kmscon_pty_feed_held(pty);
	assert(calls == 3 && seen_is("abcdefghijk") && kmscon_pty_held(pty));
	kmscon_pty_feed_held(pty);
	assert(calls == 4 && seen_is("abcdefghijkl") && !kmscon_pty_held(pty));
	kmscon_pty_feed_held(pty);
	assert(calls == 4);
	read_buf(pty);
	assert(seen_is("abcdefghijklMNO") && !kmscon_pty_held(pty));

	/* closing drops what is held */
	put(fds[1], "pqrstu");
	read_buf(pty);
	assert(kmscon_pty_held(pty));
	kmscon_pty_close(pty);
	assert(!kmscon_pty_held(pty) && pty->fd == -1);
	kmscon_pty_feed_held(pty);
	assert(seen_is("abcdefghijklMNOpqrs"));

	close(fds[1]);
	kmscon_pty_unref(pty);
	printf("test_pty: ok\n");
	return 0;
}
