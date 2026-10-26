/*
 * The pty (ADR-103 step 3d). kmscon feeds the child's output to libtsm a
 * bounded amount of work at a time: bytes the input callback does not take
 * are held, nothing more is read while they are, and the pty hands them back
 * from an idle source on its own loop, in order and before anything read
 * later, then reads on with no new write needed (its fd is edge-triggered).
 * Closing drops them. The child's side is a pipe on the pty's real loop, so
 * no child runs.
 *
 * A pause from inside the input callback (an image request, ADR-103's Notes
 * of 2026-10-24) holds what it did not take with no idle source and reads
 * nothing until resumed, through either path that feeds the callback; the
 * resume hands the held bytes over first, then reads on. Closing clears it.
 *
 * TIOCSWINSZ carries the text area in pixels from the cell size, clamped to
 * 16 bits, and a new cell size with the same size in cells still sets it. A
 * pseudo-terminal pair stands in for the child's, and the slave's owner is
 * read through the master.
 *
 * Closing drops what was queued for a child that did not read it, so the
 * next child, which may be another user's session, never reads it.
 *
 * The replies the input callback writes are bounded (#245): a child that
 * floods queries and never reads its input stops being read once
 * PTY_REPLY_HIGH of them wait, and is read again once it drains them, with
 * every reply delivered whole and in order. Past PTY_REPLY_MAX replies are
 * dropped, and after a dropped reply none from the same call arrives. A
 * paste larger than the bound, after a query was answered, never stops
 * reading; the bound and a pause each hold reading until both let go; output
 * a callback left held when the backlog began is fed after the drain; and
 * reading starts again below PTY_REPLY_LOW, not just under PTY_REPLY_HIGH.
 * Closing drops the queue. Replies the child has read stop counting though user
 * bytes wait behind them, and a failed write still lifts the backlog. A raw pseudo-terminal slave
 * is the child.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "../src/misc/pty.c"

/*
 * malloc is wrapped (-Wl,--wrap=malloc): with fail_after at n, it fails once
 * after n more calls succeed. Volatile: the compiler takes the call for the
 * builtin malloc, which touches no other memory, and would keep the count
 * it last read across it.
 */
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size);
static volatile long fail_after = -1;

void *__wrap_malloc(size_t size)
{
	if (fail_after >= 0 && !fail_after--)
		return NULL;
	return __real_malloc(size);
}

static char seen[4 * KMSCON_NREAD];
static size_t n_seen;
static size_t take; /* the most the callback takes in one call */
static unsigned int calls;
static char pause_at; /* the callback pauses the pty after taking this byte */

static size_t input(struct kmscon_pty *pty, const char *u8, size_t len, void *data)
{
	size_t n = len < take ? len : take;
	const char *p;

	(void)data;
	calls++;
	if (pty->paused)
		return 0;
	p = pause_at ? memchr(u8, pause_at, n) : NULL;
	if (p) {
		n = p - u8 + 1;
		kmscon_pty_pause(pty);
	}
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

static void test_pause(void)
{
	struct kmscon_pty *pty;
	int fds[2];

	assert(kmscon_pty_new(&pty, input, NULL, NULL) == 0);
	assert(pipe2(fds, O_NONBLOCK) == 0);
	pty->fd = fds[0];
	assert(ev_eloop_new_fd(pty->eloop, &pty->efd, fds[0], EV_ET | EV_READABLE, pty_input,
			       pty) == 0);

	/* paused at '|': the rest held, nothing fed or read until resumed */
	take = sizeof(seen);
	pause_at = '|';
	n_seen = 0;
	put(fds[1], "ab|cd", 5);
	kmscon_pty_dispatch(pty);
	assert(seen_is("ab|") && pty->paused && pty->held_len && !pty->feeding);
	put(fds[1], "ef", 2);
	calls = 0;
	turn(pty, 10, 5);
	assert(seen_is("ab|") && !calls);

	/* resumed: the held bytes first, then what came while paused */
	pause_at = 0;
	kmscon_pty_resume(pty);
	assert(!pty->paused && pty->feeding);
	turn(pty, 7, 10);
	assert(seen_is("ab|cdef") && !pty->held_len && !pty->feeding);

	/* a pause that takes the last byte: nothing held, and no read until resumed */
	pause_at = '|';
	n_seen = 0;
	put(fds[1], "gh|", 3);
	kmscon_pty_dispatch(pty);
	assert(seen_is("gh|") && pty->paused && !pty->held_len);
	put(fds[1], "ij", 2);
	calls = 0;
	turn(pty, 10, 5);
	assert(seen_is("gh|") && !calls);
	pause_at = 0;
	kmscon_pty_resume(pty);
	turn(pty, 5, 10);
	assert(seen_is("gh|ij"));

	/* a pause while held output is fed in parts stops the idle source */
	take = 2;
	pause_at = '|';
	n_seen = 0;
	put(fds[1], "klmn|op", 7);
	turn(pty, 5, 10);
	assert(seen_is("klmn|") && pty->paused && !pty->feeding && pty->held_len);
	turn(pty, 10, 5);
	assert(seen_is("klmn|"));
	pause_at = 0;
	take = sizeof(seen);
	kmscon_pty_resume(pty);
	turn(pty, 7, 10);
	assert(seen_is("klmn|op") && !pty->feeding);

	/*
	 * without an idle source the rest is fed at once, but not past a
	 * pause: the callback takes nothing while paused, and the loop would
	 * spin in the root process
	 */
	{
		struct ev_eloop *loop = pty->eloop;

		take = 1;
		pause_at = '|';
		n_seen = 0;
		memcpy(pty->io_buf, "qr|st", 5);
		pty->eloop = NULL; /* ev_eloop_register_idle_cb fails */
		assert(!feed(pty, 0, 5));
		pty->eloop = loop;
		assert(seen_is("qr|") && pty->paused && pty->held_off == 3 && pty->held_len == 5);
	}

	/* closing clears the pause and drops what is held */
	kmscon_pty_close(pty);
	assert(!pty->paused && !pty->held_len && pty->fd == -1);

	close(fds[1]);
	kmscon_pty_unref(pty);
	pause_at = 0;
}

static void test_owner(void)
{
	struct kmscon_pty *pty;
	struct stat st;
	char name[64];
	uid_t uid = (uid_t)-1;
	int master = posix_openpt(O_RDWR | O_NOCTTY);

	assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
	assert(kmscon_pty_new(&pty, input, NULL, NULL) == 0);
	assert(kmscon_pty_get_slave_owner(pty, &uid) == -EINVAL);
	pty->fd = master;
	assert(ptsname_r(master, name, sizeof(name)) == 0 && stat(name, &st) == 0);
	assert(kmscon_pty_get_slave_owner(pty, &uid) == 0 && uid == st.st_uid);
	pty->child = 1234;
	assert(kmscon_pty_get_child(pty) == 1234);
	pty->fd = -1;
	assert(kmscon_pty_get_child(pty) == 0);
	close(master);
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

static char reply[1024];
static size_t reply_len;   /* the callback answers each byte with this much */
static size_t answer_take; /* the most it takes in one call, as libtsm may; 0: all */
static size_t answered;

/*
 * With indexed on, the replies alternate 1024 and 8 bytes, each starting
 * with L or s and its query's number in seven digits, and the call that
 * answered each query is kept.
 */
static bool indexed;
static unsigned int answer_calls;
static unsigned int call_of[1 << 20];

static void indexed_reply(struct kmscon_pty *pty, size_t q)
{
	size_t len = q % 2 ? 8 : 1024;

	assert(q < sizeof(call_of) / sizeof(*call_of));
	call_of[q] = answer_calls;
	snprintf(reply, 9, "%c%07zu", len == 8 ? 's' : 'L', q);
	reply[8] = 'x';
	kmscon_pty_write(pty, reply, len);
}

static size_t answer(struct kmscon_pty *pty, const char *u8, size_t len, void *data)
{
	size_t i;

	(void)u8;
	(void)data;
	if (answer_take && len > answer_take)
		len = answer_take;
	answer_calls++;
	for (i = 0; i < len; ++i) {
		if (indexed)
			indexed_reply(pty, answered + i);
		else
			kmscon_pty_write(pty, reply, reply_len);
	}
	answered += len;
	return len;
}

/*
 * The indexed replies the child reads, byte by byte: in order, each whole,
 * and after a dropped reply none from the same call, which the drop latch
 * holds to.
 */
static char rec[1024];
static size_t rec_len;
static long last_q = -1;

static void read_indexed(const char *buf, size_t len)
{
	size_t want;
	long q;

	while (len--) {
		rec[rec_len++] = *buf++;
		assert(rec[0] == 'L' || rec[0] == 's');
		want = rec[0] == 's' ? 8 : 1024;
		if (rec_len < want)
			continue;
		rec[8] = 0;
		q = strtol(rec + 1, NULL, 10);
		assert(q > last_q && (q % 2 ? 8 : 1024) == want);
		if (q != last_q + 1)
			assert(call_of[q] != call_of[last_q + 1]);
		last_q = q;
		rec_len = 0;
	}
}

/* a pty whose child is a raw slave that reads and writes only when told to */
static struct kmscon_pty *open_child(int *slave)
{
	struct kmscon_pty *pty;
	struct termios attr;
	char name[64];
	int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);

	assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
	assert(ptsname_r(master, name, sizeof(name)) == 0);
	*slave = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	assert(*slave >= 0 && tcgetattr(*slave, &attr) == 0);
	cfmakeraw(&attr);
	assert(tcsetattr(*slave, TCSANOW, &attr) == 0);
	assert(kmscon_pty_new(&pty, answer, NULL, NULL) == 0);
	pty->fd = master;
	assert(ev_eloop_new_fd(pty->eloop, &pty->efd, master, EV_ET | EV_READABLE, pty_input,
			       pty) == 0);
	answered = 0;
	return pty;
}

static void test_close_queue(void)
{
	static char keys[256 * 1024];
	struct kmscon_pty *pty;
	int slave;

	/* more than the slave holds, which reads nothing: the rest is queued */
	pty = open_child(&slave);
	memset(keys, 'k', sizeof(keys));
	assert(kmscon_pty_write(pty, keys, sizeof(keys)) == 0);
	assert(!shl_ring_is_empty(pty->msgbuf));
	kmscon_pty_close(pty);
	assert(shl_ring_is_empty(pty->msgbuf));

	close(slave);
	kmscon_pty_unref(pty);
}

/*
 * A turn of the pty's loop that waits a little for the fd: bytes written to
 * one side of a pseudo-terminal reach the other through a kernel worker, a
 * moment later.
 */
static void wait_turn(struct kmscon_pty *pty)
{
	ev_eloop_dispatch(pty->eloop, 10);
	assert(pty->replies <= pty->queued && pty->replies <= PTY_REPLY_MAX);
	assert(!pty->replying);
}

/*
 * The child writes queries, reading nothing, until its output is full and
 * the pty answers nothing more; returns the bytes it wrote. The replies
 * waiting never pass the ceiling.
 */
static size_t flood(struct kmscon_pty *pty, int slave)
{
	static char q[4096];
	size_t sent = 0, before;
	unsigned int idle = 0;
	ssize_t n;

	memset(q, 'q', sizeof(q));
	while (idle < 10) {
		before = answered;
		n = write(slave, q, sizeof(q));
		if (n > 0)
			sent += n;
		else
			assert(n < 0 && errno == EAGAIN);
		wait_turn(pty);
		idle = n > 0 || answered != before ? 0 : idle + 1;
		assert(sent < 64 * 1024 * 1024);
	}
	return sent;
}

/* the child reads all its input there is, for @rounds turns of the pty's loop */
static size_t drain(struct kmscon_pty *pty, int slave, unsigned int rounds)
{
	static char buf[4096];
	size_t got = 0;
	ssize_t n;

	while (rounds--) {
		while ((n = read(slave, buf, sizeof(buf))) > 0) {
			if (indexed)
				read_indexed(buf, n);
			got += n;
		}
		assert(n < 0 && errno == EAGAIN);
		wait_turn(pty);
	}
	return got;
}

/*
 * The child reads until every query it wrote is answered, nothing waits for
 * it, and nothing more arrives; returns the bytes it read.
 */
static size_t drain_all(struct kmscon_pty *pty, int slave, size_t sent)
{
	size_t got = 0, n;
	unsigned int turns = 0, quiet = 0;

	while (quiet < 10) {
		n = drain(pty, slave, 1);
		got += n;
		quiet = n || answered < sent || pty->queued || pty->backlog ? 0 : quiet + 1;
		assert(++turns < 1000);
	}
	return got;
}

/* whether turns of the pty's loop answer more queries */
static bool answers(struct kmscon_pty *pty)
{
	size_t before = answered;
	unsigned int i;

	for (i = 0; i < 10; ++i)
		wait_turn(pty);
	return answered != before;
}

/* the bytes the ring holds, block by block */
static size_t ring_bytes(struct shl_ring *ring)
{
	struct shl_ring_entry *e;
	size_t n = 0;

	for (e = ring->first; e; e = e->next)
		n += e->len;
	return n;
}

/*
 * A write whose second new ring block cannot be allocated leaves nothing of
 * itself queued: the child reads every byte counted and no more, and the
 * count drains to zero.
 */
static void test_queue_nomem(void)
{
	static char keys[256 * 1024], more[1500];
	struct kmscon_pty *pty;
	size_t queued;
	int slave;

	pty = open_child(&slave);
	memset(keys, 'k', sizeof(keys));
	assert(kmscon_pty_write(pty, keys, sizeof(keys)) == 0);
	assert(kmscon_pty_write(pty, keys, 100) == 0);
	queued = pty->queued;
	assert(queued && ring_bytes(pty->msgbuf) == queued);

	/* 412 bytes fill the last block, one block is allocated, the next is not */
	memset(more, 'm', sizeof(more));
	fail_after = 1;
	assert(kmscon_pty_write(pty, more, sizeof(more)) == 0);
	assert(fail_after == -1);
	assert(pty->queued == queued && ring_bytes(pty->msgbuf) == queued);
	assert(kmscon_pty_write(pty, more, 10) == 0 && pty->queued == queued + 10);
	assert(drain_all(pty, slave, 0) == sizeof(keys) + 110 && !pty->queued);

	kmscon_pty_close(pty);
	close(slave);
	kmscon_pty_unref(pty);

	/* an empty ring stays empty, and takes the next write */
	{
		struct shl_ring *ring;

		assert(shl_ring_new(&ring) == 0);
		fail_after = 1;
		assert(shl_ring_write(ring, more, sizeof(more)) == -ENOMEM && fail_after == -1);
		assert(!ring->first && !ring->last);
		assert(shl_ring_write(ring, more, sizeof(more)) == 0);
		assert(ring_bytes(ring) == sizeof(more));
		shl_ring_free(ring);
	}
}

static void test_backlog(void)
{
	struct kmscon_pty *pty;
	size_t sent, got;
	int slave;

	memset(reply, 'r', sizeof(reply));

	/*
	 * eight bytes a query: reading stops with the replies between the
	 * bound and the ceiling, the child's own output backed up behind it
	 */
	reply_len = 8;
	pty = open_child(&slave);
	sent = flood(pty, slave);
	assert(pty->backlog && !pty->dropping);
	assert(pty->replies >= PTY_REPLY_HIGH && pty->replies <= PTY_REPLY_MAX);
	assert(answered < sent && !answers(pty));

	/*
	 * the child reads: every query is answered, every reply arrives once,
	 * and output held when the backlog began (as a callback that takes
	 * part of it leaves) is fed first
	 */
	assert(!pty->held_len);
	memset(pty->io_buf, 'q', 4);
	pty->held_off = 0;
	pty->held_len = 4;
	sent += 4;
	got = drain_all(pty, slave, sent);
	assert(answered == sent && got == sent * reply_len);
	assert(!pty->backlog && !pty->replies);

	/* a pause and the bound: reading waits for both to let go */
	sent = flood(pty, slave);
	answered = 0;
	assert(pty->backlog);
	kmscon_pty_pause(pty);
	kmscon_pty_resume(pty);
	assert(!pty->paused && pty->backlog && !answers(pty));
	kmscon_pty_pause(pty);
	drain(pty, slave, 20);
	assert(!pty->backlog && !pty->queued && pty->paused && !answers(pty));
	kmscon_pty_resume(pty);
	assert(answers(pty));

	/* closing with replies waiting drops them and their count */
	flood(pty, slave);
	assert(pty->backlog);
	kmscon_pty_close(pty);
	assert(shl_ring_is_empty(pty->msgbuf) && !pty->queued && !pty->replies);
	assert(!pty->backlog && !pty->paused);
	close(slave);
	kmscon_pty_unref(pty);

	/*
	 * a kilobyte a query: one call's replies would pass the ceiling, and the
	 * rest of that call's replies are dropped whole
	 */
	reply_len = 1024;
	pty = open_child(&slave);
	sent = flood(pty, slave);
	assert(pty->backlog && pty->dropping);
	got = drain_all(pty, slave, sent);
	assert(answered == sent && !pty->dropping);
	assert(got % reply_len == 0 && got < sent * reply_len);
	kmscon_pty_close(pty);
	close(slave);
	kmscon_pty_unref(pty);

	/*
	 * a paste larger than the bound, after a query was answered, is queued
	 * whole and stops no reading
	 */
	{
		static char paste[4 * PTY_REPLY_MAX];

		reply_len = 8;
		pty = open_child(&slave);
		put(slave, "q", 1);
		assert(answers(pty) && answered == 1);
		memset(paste, 'p', sizeof(paste));
		assert(kmscon_pty_write(pty, paste, sizeof(paste)) == 0);
		assert(pty->queued > PTY_REPLY_MAX && !pty->replies);
		put(slave, "qqq", 3);
		assert(answers(pty) && answered == 4 && !pty->backlog);
		got = drain_all(pty, slave, 4);
		assert(got == sizeof(paste) + 4 * reply_len);
		kmscon_pty_close(pty);
		close(slave);
		kmscon_pty_unref(pty);
	}
}

/*
 * Replies the child has read stop counting though user bytes wait behind
 * them: the review's case of #246, where they kept a backlog up with only
 * the newest replies unread until the whole paste drained.
 */
static void test_backlog_exact(void)
{
	static char paste[1024 * 1024], q[6000], buf[4096];
	struct kmscon_pty *pty;
	size_t want, got = 0;
	unsigned int turns = 0;
	ssize_t n;
	int slave;

	reply_len = 8;
	pty = open_child(&slave);
	memset(q, 'q', sizeof(q));
	put(slave, q, sizeof(q));
	while (answered < sizeof(q))
		wait_turn(pty), assert(++turns < 1000);
	assert(pty->replies && pty->replies < PTY_REPLY_HIGH);

	/* the user pastes behind the replies; the child reads the replies alone */
	memset(paste, 'p', sizeof(paste));
	assert(kmscon_pty_write(pty, paste, sizeof(paste)) == 0);
	want = sizeof(q) * reply_len;
	turns = 0;
	while (got < want) {
		n = read(slave, buf, want - got < sizeof(buf) ? want - got : sizeof(buf));
		if (n > 0)
			got += n;
		wait_turn(pty);
		assert(++turns < 1000);
	}
	assert(!pty->replies && pty->queued > PTY_REPLY_HIGH);

	/* new replies queue behind the paste, and only they count */
	put(slave, q, 1500);
	turns = 0;
	while (answered < sizeof(q) + 1500)
		wait_turn(pty), assert(++turns < 1000);
	assert(pty->replies == 1500 * reply_len && !pty->backlog);

	kmscon_pty_close(pty);
	close(slave);
	kmscon_pty_unref(pty);
}

/*
 * A callback that takes part of what it is handed, as libtsm does when its
 * work bound or a hold ends a call: the backlog stops feeding the rest from
 * the idle source, just past the bound, and the drain feeds it.
 */
static void test_backlog_held(void)
{
	struct kmscon_pty *pty;
	size_t sent;
	int slave;

	reply_len = 8;
	answer_take = 64;
	pty = open_child(&slave);
	sent = flood(pty, slave);
	assert(pty->backlog && !pty->dropping && !pty->feeding);
	assert(pty->replies < PTY_REPLY_HIGH + answer_take * reply_len);
	assert(drain_all(pty, slave, sent) == sent * reply_len && answered == sent);
	answer_take = 0;
	kmscon_pty_close(pty);
	close(slave);
	kmscon_pty_unref(pty);
}

/*
 * Replies of 1024 and 8 bytes in turn: once one is dropped, the rest of that
 * call's are, though a small one would fit, so no reply arrives out of place.
 */
static void test_backlog_latch(void)
{
	struct kmscon_pty *pty;
	size_t sent;
	int slave;

	indexed = true;
	last_q = -1;
	rec_len = 0;
	pty = open_child(&slave);
	sent = flood(pty, slave);
	assert(pty->backlog && pty->dropping);
	drain_all(pty, slave, sent);
	assert(answered == sent && last_q >= 0 && !rec_len);
	indexed = false;
	kmscon_pty_close(pty);
	close(slave);
	kmscon_pty_unref(pty);
}

/*
 * Reading starts again only once the replies drain below PTY_REPLY_LOW, not
 * as soon as they fall under PTY_REPLY_HIGH: the child reads a little a
 * turn, and the turns that end with nothing more answered see the replies
 * between the two. The kernel takes the replies back in steps of some
 * kilobytes, smaller than the gap.
 */
static void test_backlog_hysteresis(void)
{
	static char buf[512];
	struct kmscon_pty *pty;
	size_t sent, before, stopped = SIZE_MAX;
	unsigned int turns = 0;
	int slave;

	reply_len = 8;
	pty = open_child(&slave);
	sent = flood(pty, slave);
	assert(pty->backlog);
	for (;;) {
		before = answered;
		assert(read(slave, buf, sizeof(buf)) > 0 || errno == EAGAIN);
		wait_turn(pty);
		assert(++turns < 10000);
		if (answered != before)
			break;
		if (pty->replies < stopped)
			stopped = pty->replies;
	}
	assert(stopped >= PTY_REPLY_LOW && stopped < PTY_REPLY_HIGH);
	drain_all(pty, slave, sent);
	assert(answered == sent);
	kmscon_pty_close(pty);
	close(slave);
	kmscon_pty_unref(pty);
}

/*
 * A write to the child that fails, after the replies drained below the low
 * mark in the same call, still lifts the backlog: the edge-triggered fd may
 * report nothing more to try again with.
 */
static void test_backlog_write_error(void)
{
	static char keys[256 * 1024];
	struct kmscon_pty *pty;
	int slave, master, bad = open("/dev/null", O_RDONLY);

	pty = open_child(&slave);
	memset(keys, 'k', sizeof(keys));
	assert(kmscon_pty_write(pty, keys, sizeof(keys)) == 0);
	kmscon_pty_begin_reply(pty);
	assert(kmscon_pty_write(pty, "reply", 5) == 0);
	kmscon_pty_end_reply(pty);
	assert(pty->replies == 5 && !pty->replying);

	/* as if replies above PTY_REPLY_HIGH had just drained to these five */
	pty->backlog = true;
	master = pty->fd;
	assert(bad >= 0);
	pty->fd = bad;
	assert(send_buf(pty) < 0);
	pty->fd = master;
	assert(!pty->backlog);

	close(bad);
	kmscon_pty_close(pty);
	close(slave);
	kmscon_pty_unref(pty);
}

int main(void)
{
	test_held();
	test_pause();
	test_owner();
	test_winsize();
	test_close_queue();
	test_queue_nomem();
	test_backlog();
	test_backlog_exact();
	test_backlog_held();
	test_backlog_latch();
	test_backlog_hysteresis();
	test_backlog_write_error();
	printf("test_pty: ok\n");
	return 0;
}
