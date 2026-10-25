/*
 * Synchronized output's present rules (src/terminal_sync.c, ADR-201 step 5),
 * against a real libtsm vte and a fake clock.
 *
 * The first tests drive the rules one step at a time: the offer and DECRQM,
 * a frame end, the timeout and its wait for unparsed output, the resets that
 * end a hold, a stale timer, a flip in flight, a display that lost its
 * picture, the timer's arm time, a hold whose timer cannot be armed, and the
 * limit on sync-timeout, and the coalescing of output outside a hold. The last runs a schedule the
 * way terminal.c does, through the same calls (terminal_sync_draw for a redraw request,
 * terminal_sync_flipped at a flip, terminal_sync_arm_ns to arm): a program's
 * output bracketed in mode 2026, fed through tsm_vte_input_bounded in reads
 * of random sizes, a redraw at the end of every loop turn, flips that take a
 * random number of turns, one frame that stalls past the timeout, and
 * displays that lose their picture mid-frame. Every present must show a
 * whole frame, except at an expiry or on a display that lost its picture.
 * With mode 2026 not offered, the same schedule must present part-written
 * frames, so the check is seen to catch them.
 */

#undef NDEBUG
#include <assert.h>
#include <libtsm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "terminal_sync.h"

#define MS 1000000ULL
#define TIMEOUT_MS 200

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

struct term {
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct terminal_sync sync;
};

static void term_init(struct term *t, unsigned int cols, unsigned int rows, unsigned int timeout_ms)
{
	assert(tsm_screen_new(&t->con, NULL, NULL) == 0);
	assert(tsm_screen_resize(t->con, cols, rows) == 0);
	assert(tsm_vte_new(&t->vte, t->con, on_write, NULL, NULL, NULL) == 0);
	terminal_sync_init(&t->sync, t->vte, timeout_ms);
}

static void term_free(struct term *t)
{
	tsm_vte_unref(t->vte);
	tsm_screen_unref(t->con);
}

/* one bounded call; returns the bytes it took */
static size_t feed(struct term *t, const char *s)
{
	return tsm_vte_input_bounded(t->vte, s, strlen(s));
}

static const char *decrqm(struct term *t)
{
	reply_len = 0;
	reply[0] = 0;
	tsm_vte_input(t->vte, "\e[?2026$p", 9);
	return reply;
}

/* offered at a timeout above 0; at 0, DECRQM answers 4 and nothing holds */
static void test_offer(void)
{
	struct term t;

	term_init(&t, 20, 4, TIMEOUT_MS);
	assert(!strcmp(decrqm(&t), "\e[?2026;2$y"));
	assert(feed(&t, "\e[?2026h") == 8);
	assert(terminal_sync_input(&t.sync, 0, false) == TERMINAL_SYNC_ARM);
	assert(!strcmp(decrqm(&t), "\e[?2026;1$y"));
	term_free(&t);

	term_init(&t, 20, 4, 0);
	assert(!strcmp(decrqm(&t), "\e[?2026;4$y"));
	feed(&t, "\e[?2026hab");
	assert(terminal_sync_input(&t.sync, 0, false) == TERMINAL_SYNC_NONE);
	assert(!terminal_sync_holding(&t.sync));
	assert(terminal_sync_draw(&t.sync, false, false) == TERMINAL_SYNC_DRAW);
	assert(terminal_sync_timer(&t.sync, 10 * TIMEOUT_MS * MS, false) == TERMINAL_SYNC_NONE);
	assert(!strcmp(decrqm(&t), "\e[?2026;4$y"));
	term_free(&t);
}

/* a hold arms the timer and owes redraws; its end presents and disarms */
static void test_frame_end(void)
{
	struct term t;
	const char *rm = "\e[?2026lnext";

	term_init(&t, 20, 4, TIMEOUT_MS);
	assert(terminal_sync_draw(&t.sync, false, false) == TERMINAL_SYNC_DRAW);

	feed(&t, "\e[?2026habc");
	assert(terminal_sync_input(&t.sync, 5 * MS, false) == TERMINAL_SYNC_ARM);
	assert(t.sync.deadline_ns == 5 * MS + TIMEOUT_MS * MS);
	assert(terminal_sync_holding(&t.sync));
	assert(terminal_sync_draw(&t.sync, false, false) == TERMINAL_SYNC_OWED);
	assert(terminal_sync_draw(&t.sync, true, false) == TERMINAL_SYNC_OWED);

	/* more input in the hold changes nothing; SM again does not extend it */
	feed(&t, "def\e[?2026h");
	assert(terminal_sync_input(&t.sync, 50 * MS, false) == TERMINAL_SYNC_NONE);
	assert(t.sync.deadline_ns == 5 * MS + TIMEOUT_MS * MS);

	/* the bounded call stops right after RM, and the frame end presents */
	assert(feed(&t, rm) == 8);
	assert(terminal_sync_input(&t.sync, 60 * MS, true) == TERMINAL_SYNC_PRESENT);
	assert(!terminal_sync_holding(&t.sync));
	assert(terminal_sync_draw(&t.sync, false, false) == TERMINAL_SYNC_DRAW);
	feed(&t, rm + 8);
	assert(terminal_sync_input(&t.sync, 61 * MS, false) == TERMINAL_SYNC_NONE);

	/* the timer of the ended hold, if it fires anyway, does nothing */
	assert(terminal_sync_timer(&t.sync, 205 * MS, false) == TERMINAL_SYNC_NONE);
	term_free(&t);
}

/* the timeout ends the hold through libtsm and presents once */
static void test_timeout(void)
{
	struct term t;

	term_init(&t, 20, 4, TIMEOUT_MS);
	feed(&t, "\e[?2026habc");
	assert(terminal_sync_input(&t.sync, 0, false) == TERMINAL_SYNC_ARM);

	/* early: a timer that fires before the deadline is armed again */
	assert(terminal_sync_timer(&t.sync, 150 * MS, false) == TERMINAL_SYNC_ARM);
	assert(t.sync.deadline_ns == TIMEOUT_MS * MS);
	assert(terminal_sync_holding(&t.sync));

	assert(terminal_sync_timer(&t.sync, TIMEOUT_MS * MS, false) == TERMINAL_SYNC_EXPIRED);
	assert(!terminal_sync_holding(&t.sync));
	assert(!strcmp(decrqm(&t), "\e[?2026;2$y"));
	/* the end it caused is not taken for a frame end at the next input */
	feed(&t, "x");
	assert(terminal_sync_input(&t.sync, 201 * MS, false) == TERMINAL_SYNC_NONE);
	/* a later RM is redundant */
	feed(&t, "\e[?2026l");
	assert(terminal_sync_input(&t.sync, 202 * MS, false) == TERMINAL_SYNC_NONE);

	/* the next hold gets a timer of its own */
	feed(&t, "\e[?2026h");
	assert(terminal_sync_input(&t.sync, 300 * MS, false) == TERMINAL_SYNC_ARM);
	assert(t.sync.deadline_ns == 300 * MS + TIMEOUT_MS * MS);
	term_free(&t);
}

/*
 * At the timeout, read output still unparsed may hold the frame end: the
 * expiry waits for it to be parsed, up to 1 s after the hold began.
 */
static void test_timeout_wait(void)
{
	struct term t;

	/* the output is parsed without a frame end: the hold ends then */
	term_init(&t, 20, 4, TIMEOUT_MS);
	feed(&t, "\e[?2026h");
	assert(terminal_sync_input(&t.sync, 0, true) == TERMINAL_SYNC_ARM);
	assert(terminal_sync_timer(&t.sync, TIMEOUT_MS * MS, true) == TERMINAL_SYNC_ARM);
	assert(t.sync.deadline_ns == TERMINAL_SYNC_WAIT_NS);
	assert(terminal_sync_holding(&t.sync));
	feed(&t, "abc");
	assert(terminal_sync_input(&t.sync, 300 * MS, true) == TERMINAL_SYNC_NONE);
	assert(terminal_sync_holding(&t.sync));
	feed(&t, "def");
	assert(terminal_sync_input(&t.sync, 400 * MS, false) == TERMINAL_SYNC_EXPIRED);
	assert(!terminal_sync_holding(&t.sync));
	assert(!strcmp(decrqm(&t), "\e[?2026;2$y"));
	term_free(&t);

	/* the frame end is in it: an ordinary frame end, not an expiry */
	term_init(&t, 20, 4, TIMEOUT_MS);
	feed(&t, "\e[?2026h");
	terminal_sync_input(&t.sync, 0, true);
	assert(terminal_sync_timer(&t.sync, TIMEOUT_MS * MS, true) == TERMINAL_SYNC_ARM);
	feed(&t, "abc\e[?2026l");
	assert(terminal_sync_input(&t.sync, 300 * MS, true) == TERMINAL_SYNC_PRESENT);
	term_free(&t);

	/* output keeps coming: the hold ends at 1 s */
	term_init(&t, 20, 4, TIMEOUT_MS);
	feed(&t, "\e[?2026h");
	terminal_sync_input(&t.sync, 0, true);
	assert(terminal_sync_timer(&t.sync, TIMEOUT_MS * MS, true) == TERMINAL_SYNC_ARM);
	feed(&t, "abc");
	assert(terminal_sync_input(&t.sync, 900 * MS, true) == TERMINAL_SYNC_NONE);
	assert(terminal_sync_timer(&t.sync, TERMINAL_SYNC_WAIT_NS, true) == TERMINAL_SYNC_EXPIRED);
	assert(!terminal_sync_holding(&t.sync));
	term_free(&t);

	/* a timeout of 1 s or more waits no longer than itself */
	term_init(&t, 20, 4, 1500);
	feed(&t, "\e[?2026h");
	assert(terminal_sync_input(&t.sync, 0, true) == TERMINAL_SYNC_ARM);
	assert(t.sync.deadline_ns == 1500 * MS);
	assert(terminal_sync_timer(&t.sync, 1500 * MS, true) == TERMINAL_SYNC_EXPIRED);
	term_free(&t);
}

/* RIS and DECSTR in the stream, and the host's own resets, end a hold */
static void test_resets(void)
{
	static const char *const ends[] = {"\ec", "\e[!p"};
	struct term t;

	for (size_t i = 0; i < 2; i++) {
		char buf[32];

		term_init(&t, 20, 4, TIMEOUT_MS);
		feed(&t, "\e[?2026habc");
		assert(terminal_sync_input(&t.sync, 0, false) == TERMINAL_SYNC_ARM);
		snprintf(buf, sizeof(buf), "%sxyz", ends[i]);
		assert(feed(&t, buf) == strlen(ends[i]));
		assert(terminal_sync_input(&t.sync, MS, true) == TERMINAL_SYNC_PRESENT);
		assert(!terminal_sync_holding(&t.sync));
		/* the offer survives the reset */
		assert(!strcmp(decrqm(&t), "\e[?2026;2$y"));
		term_free(&t);
	}

	/* terminal_open's hard reset and terminal_image_close's reset */
	for (size_t i = 0; i < 2; i++) {
		term_init(&t, 20, 4, TIMEOUT_MS);
		feed(&t, "\e[?2026habc");
		terminal_sync_input(&t.sync, 0, false);
		if (i)
			tsm_vte_reset(t.vte);
		else
			tsm_vte_hard_reset(t.vte);
		assert(terminal_sync_input(&t.sync, MS, false) == TERMINAL_SYNC_PRESENT);
		assert(!terminal_sync_holding(&t.sync));
		term_free(&t);
	}

	/* a hold ended where kmscon did not look is never held past it */
	term_init(&t, 20, 4, TIMEOUT_MS);
	feed(&t, "\e[?2026h");
	terminal_sync_input(&t.sync, 0, false);
	tsm_vte_reset(t.vte);
	assert(!terminal_sync_holding(&t.sync));
	assert(terminal_sync_draw(&t.sync, false, false) == TERMINAL_SYNC_DRAW);
	assert(terminal_sync_timer(&t.sync, TIMEOUT_MS * MS, false) == TERMINAL_SYNC_PRESENT);
	term_free(&t);
}

/*
 * tsm_vte_input parses through: a frame can end and the next begin in one
 * call. The screen holds part of the next frame, so nothing presents.
 */
static void test_parse_through(void)
{
	struct term t;
	const char *s = "\e[?2026ha\e[?2026l\e[?2026hb";

	term_init(&t, 20, 4, TIMEOUT_MS);
	tsm_vte_input(t.vte, s, strlen(s));
	assert(terminal_sync_input(&t.sync, 0, false) == TERMINAL_SYNC_ARM);
	assert(terminal_sync_holding(&t.sync));
	tsm_vte_input(t.vte, "\e[?2026l\e[?2026hc", 17);
	assert(terminal_sync_input(&t.sync, 50 * MS, false) == TERMINAL_SYNC_ARM);
	assert(t.sync.deadline_ns == 50 * MS + TIMEOUT_MS * MS);
	assert(terminal_sync_holding(&t.sync));
	term_free(&t);
}

/*
 * A flip in flight takes the draw at its end; a hold begun by then waits for
 * its own end. A display that lost its picture draws through a hold.
 */
static void test_flip_and_lost(void)
{
	struct term t;

	term_init(&t, 20, 4, TIMEOUT_MS);
	assert(terminal_sync_draw(&t.sync, true, false) == TERMINAL_SYNC_FLIP);

	feed(&t, "\e[?2026h");
	terminal_sync_input(&t.sync, 0, false);
	assert(terminal_sync_draw(&t.sync, true, false) == TERMINAL_SYNC_OWED);
	assert(terminal_sync_draw(&t.sync, false, true) == TERMINAL_SYNC_DRAW);
	assert(terminal_sync_draw(&t.sync, true, true) == TERMINAL_SYNC_FLIP);
	assert(terminal_sync_holding(&t.sync));

	/* the frame ends while a flip is in flight: drawn at the flip */
	feed(&t, "\e[?2026l");
	assert(terminal_sync_input(&t.sync, MS, false) == TERMINAL_SYNC_PRESENT);
	assert(terminal_sync_draw(&t.sync, true, false) == TERMINAL_SYNC_FLIP);
	assert(terminal_sync_draw(&t.sync, false, false) == TERMINAL_SYNC_DRAW);

	assert(terminal_sync_flipped(&t.sync, true, false) == TERMINAL_SYNC_DRAW);
	assert(terminal_sync_flipped(&t.sync, false, false) == TERMINAL_SYNC_IDLE);

	/* a new hold begins before the flip completes: the flip draws nothing */
	feed(&t, "\e[?2026h");
	terminal_sync_input(&t.sync, 2 * MS, false);
	assert(terminal_sync_flipped(&t.sync, true, false) == TERMINAL_SYNC_OWED);
	assert(terminal_sync_flipped(&t.sync, false, false) == TERMINAL_SYNC_IDLE);
	/* unless the draw it was deferred for is a lost picture's */
	assert(terminal_sync_flipped(&t.sync, true, true) == TERMINAL_SYNC_DRAW);
	term_free(&t);
}

/* the timer's relative arm time; 1 once the deadline has passed */
static void test_arm_ns(void)
{
	struct term t;

	term_init(&t, 20, 4, TIMEOUT_MS);
	feed(&t, "\e[?2026h");
	assert(terminal_sync_input(&t.sync, 30 * MS, false) == TERMINAL_SYNC_ARM);
	assert(terminal_sync_arm_ns(&t.sync, 30 * MS) == TIMEOUT_MS * MS);
	assert(terminal_sync_arm_ns(&t.sync, 100 * MS) == (TIMEOUT_MS - 70) * MS);
	assert(terminal_sync_arm_ns(&t.sync, (30 + TIMEOUT_MS) * MS) == 1);
	assert(terminal_sync_arm_ns(&t.sync, 900 * MS) == 1);
	term_free(&t);
}

/* a hold whose timer cannot run ends at once, through libtsm */
static void test_cancel(void)
{
	struct term t;

	term_init(&t, 20, 4, TIMEOUT_MS);
	assert(terminal_sync_cancel(&t.sync) == TERMINAL_SYNC_NONE);
	feed(&t, "\e[?2026habc");
	assert(terminal_sync_input(&t.sync, 0, false) == TERMINAL_SYNC_ARM);
	assert(terminal_sync_cancel(&t.sync) == TERMINAL_SYNC_PRESENT);
	assert(!terminal_sync_holding(&t.sync));
	assert(!strcmp(decrqm(&t), "\e[?2026;2$y"));
	/* nothing is left: no second present, no expiry */
	assert(terminal_sync_input(&t.sync, MS, false) == TERMINAL_SYNC_NONE);
	assert(terminal_sync_timer(&t.sync, TIMEOUT_MS * MS, false) == TERMINAL_SYNC_NONE);
	assert(terminal_sync_cancel(&t.sync) == TERMINAL_SYNC_NONE);
	term_free(&t);
}

/*
 * Coalescing without mode 2026 (ADR-201 step 6): part way through a write,
 * the draw waits until one refresh after the last present, or, when the
 * write began after that, one refresh after the first turn that waited; a
 * short read, a refresh already past, and a hold, which decides for itself,
 * draw now.
 */
static void test_coalesce(void)
{
	const uint64_t refresh = 1000000000ULL / 144;
	struct term t;

	term_init(&t, 20, 4, TIMEOUT_MS);
	assert(terminal_sync_coalesce(&t.sync, false, 5 * MS, 4 * MS, 0, refresh) == 0);
	/* output owed since the present: one refresh after it */
	assert(terminal_sync_coalesce(&t.sync, true, 5 * MS, 4 * MS, 0, refresh) ==
	       4 * MS + refresh);
	assert(terminal_sync_coalesce(&t.sync, true, 6 * MS, 4 * MS, 5 * MS, refresh) ==
	       4 * MS + refresh);
	assert(terminal_sync_coalesce(&t.sync, true, 4 * MS + refresh, 4 * MS, 5 * MS, refresh) ==
	       0);
	/* a write begun after an idle spell: one refresh after the first wait */
	assert(terminal_sync_coalesce(&t.sync, true, 90 * MS, 4 * MS, 0, refresh) ==
	       90 * MS + refresh);
	assert(terminal_sync_coalesce(&t.sync, true, 91 * MS, 4 * MS, 90 * MS, refresh) ==
	       90 * MS + refresh);
	assert(terminal_sync_coalesce(&t.sync, true, 90 * MS + refresh, 4 * MS, 90 * MS, refresh) ==
	       0);
	/* a display that does not report its refresh is taken as 60 Hz */
	assert(terminal_sync_coalesce(&t.sync, true, 5 * MS, 4 * MS, 0, 0) ==
	       4 * MS + TERMINAL_SYNC_REFRESH_NS);

	feed(&t, "\e[?2026habc");
	assert(terminal_sync_input(&t.sync, 5 * MS, true) == TERMINAL_SYNC_ARM);
	assert(terminal_sync_coalesce(&t.sync, true, 5 * MS, 4 * MS, 0, refresh) == 0);
	assert(terminal_sync_draw(&t.sync, false, false) == TERMINAL_SYNC_OWED);
	feed(&t, "\e[?2026l");
	assert(terminal_sync_input(&t.sync, 6 * MS, true) == TERMINAL_SYNC_PRESENT);
	assert(terminal_sync_coalesce(&t.sync, true, 6 * MS, 4 * MS, 0, refresh) ==
	       4 * MS + refresh);
	term_free(&t);
}

/* sync-timeout is cut to TERMINAL_SYNC_MAX_MS; 0 and values under it stand */
static void test_timeout_limit(void)
{
	struct term t;

	term_init(&t, 20, 4, 0);
	assert(terminal_sync_init(&t.sync, t.vte, 50000) == TERMINAL_SYNC_MAX_MS);
	feed(&t, "\e[?2026h");
	assert(terminal_sync_input(&t.sync, 0, false) == TERMINAL_SYNC_ARM);
	assert(t.sync.deadline_ns == TERMINAL_SYNC_MAX_MS * MS);
	assert(terminal_sync_init(&t.sync, t.vte, TERMINAL_SYNC_MAX_MS) == TERMINAL_SYNC_MAX_MS);
	assert(terminal_sync_init(&t.sync, t.vte, TIMEOUT_MS) == TIMEOUT_MS);
	assert(terminal_sync_init(&t.sync, t.vte, 0) == 0);
	assert(!strcmp(decrqm(&t), "\e[?2026;4$y"));
	term_free(&t);
}

/* ---- the schedule ---- */

#define COLS 24
#define ROWS 8
#define FRAMES 60
#define STALL_FRAME 31
#define STALL_TURNS 300 /* turns of 1 ms: past the timeout */

/*
 * terminal.c's present path, reduced to one display: redraw_screen,
 * display_pageflip, and the actions after each input call and at the timer.
 */
struct model {
	struct term t;
	uint64_t now;
	bool armed;
	uint64_t timer_at;
	bool swapping;
	unsigned int flip_turns;
	bool pending, pending_lost;
	bool expired_live; /* a timeout ended the hold, and no hold has begun since */

	unsigned int presents, whole, partial, partial_expired, partial_lost, expiries;
	int last_whole;
};

/* the frame number every row shows, -1 for a blank screen, -2 for a mix */
static int screen_frame(struct model *m)
{
	const struct tsm_frame *f = tsm_screen_frame(m->t.con);
	int frame = -3;

	for (unsigned int y = 0; y < ROWS; y++) {
		char row[8];
		int n = -1;

		for (unsigned int x = 0; x < 5; x++) {
			uint64_t key = tsm_frame_cell_at(f, x, y)->key;

			row[x] = key ? (char)key : ' ';
		}
		row[5] = 0;
		if (row[0] == 'F')
			n = atoi(row + 1);
		if (frame == -3)
			frame = n;
		else if (frame != n)
			return -2;
	}
	return frame;
}

static void present(struct model *m, bool lost)
{
	int frame = screen_frame(m);

	m->pending = false;
	m->pending_lost = false;
	m->presents++;
	if (frame >= -1) {
		m->whole++;
		m->last_whole = frame;
	} else {
		m->partial++;
		if (lost)
			m->partial_lost++;
		else if (m->expired_live)
			m->partial_expired++;
	}
	m->swapping = true;
	m->flip_turns = 1 + rand() % 25;
}

static void redraw(struct model *m, bool lost)
{
	switch (terminal_sync_draw(&m->t.sync, m->swapping, lost)) {
	case TERMINAL_SYNC_DRAW:
		present(m, lost);
		break;
	case TERMINAL_SYNC_FLIP:
		m->pending = true;
		m->pending_lost |= lost;
		break;
	case TERMINAL_SYNC_OWED:
	case TERMINAL_SYNC_IDLE:
		break;
	}
}

static void flip(struct model *m)
{
	m->swapping = false;
	if (terminal_sync_flipped(&m->t.sync, m->pending, m->pending_lost) == TERMINAL_SYNC_DRAW)
		present(m, m->pending_lost);
}

static void act(struct model *m, enum terminal_sync_action a, bool from_input)
{
	switch (a) {
	case TERMINAL_SYNC_NONE:
		break;
	case TERMINAL_SYNC_ARM:
		m->armed = true;
		m->timer_at = m->now + terminal_sync_arm_ns(&m->t.sync, m->now);
		if (from_input)
			m->expired_live = false;
		break;
	case TERMINAL_SYNC_EXPIRED:
		m->expiries++;
		m->expired_live = true;
		/* fall through */
	case TERMINAL_SYNC_PRESENT:
		m->armed = false;
		redraw(m, false);
		break;
	}
}

static size_t frame_bytes(char *buf, size_t size, unsigned int n, unsigned int from,
			  unsigned int to, bool sm, bool rm)
{
	size_t len = 0;

	if (sm)
		len += snprintf(buf + len, size - len, "\e[?2026h");
	for (unsigned int y = from; y < to; y++)
		len += snprintf(buf + len, size - len, "\e[%u;1HF%04u", y + 1, n);
	if (rm)
		len += snprintf(buf + len, size - len, "\e[?2026l");
	return len;
}

static void run_schedule(unsigned int timeout_ms, struct model *m)
{
	static char stream[FRAMES * (ROWS * 16 + 32)];
	size_t len = 0, pos = 0, stall_at = 0, chunk_end = 0;
	unsigned int stall_left = 0;

	memset(m, 0, sizeof(*m));
	m->last_whole = -3;
	term_init(&m->t, COLS, ROWS, timeout_ms);
	for (unsigned int n = 0; n < FRAMES; n++) {
		if (n == STALL_FRAME) {
			len += frame_bytes(stream + len, sizeof(stream) - len, n, 0, ROWS / 2, true,
					   false);
			stall_at = len;
			len += frame_bytes(stream + len, sizeof(stream) - len, n, ROWS / 2, ROWS,
					   false, true);
		} else {
			len += frame_bytes(stream + len, sizeof(stream) - len, n, 0, ROWS, true,
					   true);
		}
	}

	while (pos < len || m->swapping || m->armed) {
		/* one loop turn: one bounded call on what the pty holds, or a new read */
		if (pos == stall_at && stall_at && !stall_left)
			stall_left = STALL_TURNS, stall_at = 0;
		if (stall_left) {
			stall_left--;
		} else if (pos < len) {
			size_t n;

			if (pos >= chunk_end) {
				chunk_end = pos + 1 + rand() % 96;
				if (chunk_end > len)
					chunk_end = len;
				if (stall_at > pos && chunk_end > stall_at)
					chunk_end = stall_at;
			}
			n = tsm_vte_input_bounded(m->t.vte, stream + pos, chunk_end - pos);
			pos += n;
			act(m, terminal_sync_input(&m->t.sync, m->now, pos < chunk_end), true);
			/* terminal_turn_end */
			redraw(m, false);
		}
		/* a display that loses its picture now and then, mid-hold or not */
		if (rand() % 97 == 0)
			redraw(m, true);

		m->now += MS;
		if (m->swapping && !--m->flip_turns)
			flip(m);
		if (m->armed && m->now >= m->timer_at)
			act(m, terminal_sync_timer(&m->t.sync, m->now, pos < chunk_end), false);
		assert(m->now < 100000 * MS);
	}
	/* the last frame is on screen once the flips are done */
	redraw(m, false);
	while (m->swapping) {
		m->now += MS;
		if (!--m->flip_turns)
			flip(m);
	}
}

static void test_schedule(void)
{
	struct model m;

	for (unsigned int seed = 1; seed <= 20; seed++) {
		srand(seed);
		run_schedule(TIMEOUT_MS, &m);
		/* only an expiry or a lost picture shows part of a frame */
		assert(m.partial == m.partial_expired + m.partial_lost);
		assert(m.expiries == 1);
		assert(m.last_whole == FRAMES - 1);
		assert(m.whole > 3);
		term_free(&m.t);

		/* the control: offered nothing, the same output presents part-written frames */
		srand(seed);
		run_schedule(0, &m);
		assert(m.partial > m.partial_lost);
		assert(m.expiries == 0);
		term_free(&m.t);
	}

	/* the stalled frame shows its first half at the expiry */
	srand(7);
	run_schedule(TIMEOUT_MS, &m);
	assert(m.partial_expired >= 1);
	term_free(&m.t);
}

int main(void)
{
	test_offer();
	test_frame_end();
	test_timeout();
	test_timeout_wait();
	test_resets();
	test_parse_through();
	test_flip_and_lost();
	test_arm_ns();
	test_cancel();
	test_timeout_limit();
	test_coalesce();
	test_schedule();
	printf("test_terminal_sync: ok\n");
	return 0;
}
