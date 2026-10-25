/*
 * Coalescing without mode 2026 (ADR-201 step 6), measured.
 *
 * Usage: test_coalesce FONT_DIR
 *
 * Replays terminal.c's present rules on a fake clock, with coalescing and
 * without it: bulk writes at 60 and 144 Hz and with draws longer than a
 * refresh, frames in 16 KB and 4 KB write() calls, a large write and then
 * quiet, small output, frames in mode 2026, and a key's echo, on a display
 * with flips and one without, and three of them on a real pty. The display
 * and the frames are test_present's (present_harness.h). It prints presents
 * per second and mixed cells per present (cells showing neither the write's
 * end nor what came before it where the write changes them), and checks the
 * rule's bounds. COALESCE_N in the environment reruns everything at another
 * threshold.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "present_harness.h"
#include "terminal_sync.h"

/* ---- coalescing without mode 2026 (ADR-201 step 6), on a fake clock ---- */

/*
 * terminal.c's present rules replayed on a fake clock, with the real vte,
 * terminal_sync and renderer: the program's writes arrive as write() calls
 * of a set size at a set pace; each loop turn takes what an earlier input
 * call left, or reads until the pty is empty or 50 reads are done, as
 * src/misc/pty.c does. A read takes at most KREAD bytes, what a Linux pty
 * hands over, and one that takes less leaves n_tty empty until the next turn.
 * Parsing and drawing cost fixed times. At each turn's end the screen is drawn as terminal_turn_end
 * does, by the rule (coalesced) or at once (as before the rule). A display with flips shows a
 * present at the next refresh and defers a draw asked for meanwhile to that flip, as on DRM; one
 * without shows it at once.
 */

#define US 1000ULL
#define MS (1000 * US)
#define REFRESH_NS (1000000000ULL / 60) /* unless a schedule sets its own */
#define PARSE_NS_PER_KB (10 * US)	/* 100 MB/s */
#define DRAW_NS (3 * MS)
#define KREAD 4095 /* the most a read takes from a Linux pty (n_tty's buffer) */

/*
 * The bytes in a turn that count as part way through a write; COALESCE_N in
 * the environment sets another, to measure it against terminal.c's.
 */
static size_t coalesce_n = TERMINAL_SYNC_COALESCE_BYTES;
#define ECHO_NS (200 * US) /* from a key press to its echo arriving */

struct write {
	uint64_t start_ns;
	size_t off, len;  /* in sim.data */
	size_t wsize;	  /* the write() calls it is made of; 0: one call */
	uint64_t wgap_ns; /* between them */
};

enum why {
	WHY_TURN,  /* the turn's end, the rule finding the write ended or not holding it back */
	WHY_TIMER, /* one refresh after the last present, mid-write */
	WHY_FLIP,  /* a draw deferred to a flip */
	WHY_SYNC,  /* a frame end of mode 2026 */
	WHY_KEY,   /* a key press */
};

struct sim {
	bool coalesce, flips;
	uint64_t refresh_ns, draw_ns;
	char *data;
	struct write w[512];
	unsigned int nw;
	size_t total;
	uint64_t key_ns; /* 0 or a key press */

	uint64_t t;
	size_t read, parsed;
	char rbuf[NREAD];
	size_t held_off, held_len;
	bool unparsed, redraw_owed;
	struct terminal_coalesce co;
	struct terminal_coalesce_display cd;

	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct terminal_sync sync;
	uint64_t sync_due;
	struct output o;
	bool swapping, pending;
	uint64_t flip_at, coalesce_due;

	/* the screen as it is once the write in progress is parsed whole */
	struct tsm_screen *ref_con;
	struct tsm_vte *ref_vte;
	struct output ref;
	unsigned int ref_writes;

	/* what was presented */
	unsigned int presents, mixed_presents, mid_presents;
	uint64_t mixed_cells, max_mixed;
	uint64_t first_ns, last_ns;
	uint64_t prev_ns, prev_shown_ns;
	bool prev_partial;
	uint64_t max_partial_gap; /* from a present part way through a write to the next */
	uint64_t min_mid_gap;	  /* before a draw the rule made part way through a write */
	uint64_t echo_latency;	  /* from the echo's arrival to its present */
	enum why last_why;	  /* what made the last present */
	uint64_t shown_at[64];	  /* when the first presents were shown */
	uint64_t parsed_ns;	  /* when the last byte was parsed */
	unsigned int failures;
};

static size_t sim_readable(const struct sim *s, uint64_t t)
{
	size_t n = 0;

	for (unsigned int k = 0; k < s->nw && s->w[k].start_ns <= t; k++) {
		const struct write *w = &s->w[k];
		size_t calls, got;

		if (!w->wsize || !w->wgap_ns) {
			got = w->len;
		} else {
			calls = (t - w->start_ns) / w->wgap_ns + 1;
			got = calls * w->wsize < w->len ? calls * w->wsize : w->len;
		}
		n = w->off + got;
	}
	return n;
}

/* when the program next hands the pty bytes after @t; UINT64_MAX when it has no more */
static uint64_t sim_next_arrival(const struct sim *s, uint64_t t)
{
	for (unsigned int k = 0; k < s->nw; k++) {
		const struct write *w = &s->w[k];

		if (w->start_ns > t)
			return w->start_ns;
		if (sim_readable(s, t) < w->off + w->len && w->wsize && w->wgap_ns)
			return w->start_ns + ((t - w->start_ns) / w->wgap_ns + 1) * w->wgap_ns;
	}
	return UINT64_MAX;
}

/* the write @off lies in, or nw past the end */
static unsigned int sim_write_at(const struct sim *s, size_t off)
{
	unsigned int k = 0;

	while (k < s->nw && off >= s->w[k].off + s->w[k].len)
		k++;
	return k;
}

/* cells whose pixels differ between what @a and @b show */
static unsigned int cells_differing(const struct output *a, const struct output *b)
{
	const uint32_t *pa = on_screen(a), *pb = on_screen(b);
	unsigned int n = 0;

	for (unsigned int row = 0; row < ROWS; row++) {
		for (unsigned int col = 0; col < COLS; col++) {
			bool differs = false;

			for (unsigned int y = 0; y < font->height && !differs; y++) {
				size_t i =
					(size_t)(row * font->height + y) * fb_w + col * font->width;

				differs = memcmp(pa + i, pb + i, font->width * 4) != 0;
			}
			n += differs;
		}
	}
	return n;
}

static void sim_draw(struct sim *s, enum why why, bool mid)
{
	unsigned int mixed;
	uint64_t shown;
	bool partial;

	uint64_t start = s->t;

	s->pending = false;
	s->t += s->draw_ns;
	if (!output_frame(&s->o, s->con, s->vte)) {
		terminal_coalesce_drawn(&s->co, &s->cd, start, s->t, false);
		return;
	}
	terminal_coalesce_drawn(&s->co, &s->cd, start, s->t, true);
	shown = s->t;
	if (s->flips) {
		s->swapping = true;
		s->flip_at = (s->t / s->refresh_ns + 1) * s->refresh_ns;
		shown = s->flip_at;
	}

	output_frame(&s->ref, s->ref_con, s->ref_vte);
	mixed = cells_differing(&s->o, &s->ref);
	partial = s->parsed < s->total && s->parsed != s->w[sim_write_at(s, s->parsed)].off;
	if (!s->presents)
		s->first_ns = s->t;
	if (s->presents && s->prev_partial && shown - s->prev_shown_ns > s->max_partial_gap)
		s->max_partial_gap = shown - s->prev_shown_ns;
	/* between the starts of the draws, which the rule decides */
	if (s->presents && mid && start - s->prev_ns < s->min_mid_gap)
		s->min_mid_gap = start - s->prev_ns;
	if (s->key_ns && !s->echo_latency && s->parsed == s->total)
		s->echo_latency = shown - (s->key_ns + ECHO_NS);
	if (s->presents < sizeof(s->shown_at) / sizeof(s->shown_at[0]))
		s->shown_at[s->presents] = shown;
	s->presents++;
	s->mid_presents += mid;
	s->mixed_presents += mixed > 0;
	s->mixed_cells += mixed;
	if (mixed > s->max_mixed)
		s->max_mixed = mixed;
	s->last_ns = s->t;
	s->prev_ns = start;
	s->prev_shown_ns = shown;
	s->prev_partial = partial;
	s->last_why = why;
}

/* redraw_screen */
static void sim_redraw(struct sim *s, enum why why, bool mid)
{
	switch (terminal_sync_draw(&s->sync, s->swapping, false)) {
	case TERMINAL_SYNC_DRAW:
		sim_draw(s, why, mid);
		break;
	case TERMINAL_SYNC_FLIP:
		s->pending = true;
		break;
	case TERMINAL_SYNC_OWED:
	case TERMINAL_SYNC_IDLE:
		break;
	}
}

/* sync_act */
static void sim_act(struct sim *s, enum terminal_sync_action action)
{
	switch (action) {
	case TERMINAL_SYNC_NONE:
		break;
	case TERMINAL_SYNC_ARM:
		s->sync_due = s->t + terminal_sync_arm_ns(&s->sync, s->t);
		break;
	case TERMINAL_SYNC_EXPIRED:
	case TERMINAL_SYNC_PRESENT:
		s->sync_due = 0;
		sim_redraw(s, WHY_SYNC, false);
		break;
	}
}

/* pty_input, then the pty's feed: true if the call took all of it */
static bool sim_input(struct sim *s, const char *u8, size_t len)
{
	uint64_t start = s->t;
	size_t n;

	s->redraw_owed = true;
	n = tsm_vte_input_bounded(s->vte, u8, len);
	s->t += n * PARSE_NS_PER_KB / 1024;
	terminal_coalesce_input(&s->co, start, s->t, n);
	s->parsed += n;
	if (s->parsed == s->total)
		s->parsed_ns = s->t;
	while (s->ref_writes < s->nw && s->w[s->ref_writes].off < s->parsed) {
		const struct write *w = &s->w[s->ref_writes++];

		tsm_vte_input(s->ref_vte, s->data + w->off, w->len);
	}
	s->unparsed = n < len;
	sim_act(s, terminal_sync_input(&s->sync, s->t, s->unparsed));
	if (s->unparsed) {
		s->held_off = (u8 - s->rbuf) + n;
		s->held_len = (u8 - s->rbuf) + len;
		return false;
	}
	s->held_len = 0;
	return true;
}

/* terminal_turn_end */
static void sim_turn_end(struct sim *s)
{
	bool mid = s->co.bytes >= s->co.mid_bytes || s->unparsed;

	if (s->redraw_owed && kmscon_text_changed(s->o.txt, s->con)) {
		if (!s->coalesce || !terminal_coalesce_decide(&s->sync, &s->co, &s->cd, s->unparsed,
							      s->t, s->refresh_ns)) {
			/* coalesce_end */
			terminal_coalesce_end(&s->cd);
			sim_redraw(s, WHY_TURN, mid);
		}
		/* coalesce_arm */
		s->coalesce_due = s->cd.due_ns;
	}
	s->redraw_owed = false;
	terminal_coalesce_turn_end(&s->co);
}

static void sim_run(struct sim *s)
{
	bool key_done = !s->key_ns;

	for (;;) {
		uint64_t next = UINT64_MAX;

		if (s->swapping && s->flip_at <= s->t) {
			s->swapping = false;
			/* display_pageflip */
			switch (terminal_sync_flipped(&s->sync, s->pending, false)) {
			case TERMINAL_SYNC_DRAW:
			case TERMINAL_SYNC_FLIP:
				sim_draw(s, WHY_FLIP, false);
				break;
			default:
				break;
			}
		}
		if (s->coalesce_due && s->coalesce_due <= s->t) {
			s->coalesce_due = 0;
			if (s->cd.due_ns && s->cd.due_ns <= s->t) {
				terminal_coalesce_end(&s->cd);
				sim_redraw(s, WHY_TIMER, true);
			}
		}
		if (s->sync_due && s->sync_due <= s->t) {
			s->sync_due = 0;
			sim_act(s, terminal_sync_timer(&s->sync, s->t, s->unparsed));
		}
		if (!key_done && s->key_ns <= s->t) {
			key_done = true;
			sim_redraw(s, WHY_KEY, false);
		}

		if (s->held_len) {
			sim_input(s, s->rbuf + s->held_off, s->held_len - s->held_off);
		} else {
			for (unsigned int num = 50; num; num--) {
				size_t n = sim_readable(s, s->t) - s->read;

				if (!n)
					break;
				if (n > KREAD)
					n = KREAD;
				memcpy(s->rbuf, s->data + s->read, n);
				s->read += n;
				if (!sim_input(s, s->rbuf, n))
					break;
				/* the next read finds n_tty empty until its buffer is refilled */
				if (n < KREAD)
					break;
			}
		}
		sim_turn_end(s);

		if (s->held_len || sim_readable(s, s->t) > s->read)
			continue;
		next = sim_next_arrival(s, s->t);
		if (s->swapping && s->flip_at < next)
			next = s->flip_at;
		if (s->coalesce_due && s->coalesce_due < next)
			next = s->coalesce_due;
		if (s->sync_due && s->sync_due < next)
			next = s->sync_due;
		if (!key_done && s->key_ns < next)
			next = s->key_ns;
		if (next == UINT64_MAX)
			break;
		if (next > s->t)
			s->t = next;
	}
}

static void sim_init(struct sim *s, bool coalesce, bool flips)
{
	memset(s, 0, sizeof(*s));
	s->coalesce = coalesce;
	s->flips = flips;
	s->refresh_ns = REFRESH_NS;
	s->draw_ns = DRAW_NS;
	s->min_mid_gap = UINT64_MAX;
	terminal_coalesce_init(&s->co);
	s->co.mid_bytes = coalesce_n;
	assert(tsm_screen_new(&s->con, NULL, NULL) == 0);
	assert(tsm_screen_resize(s->con, COLS, ROWS) == 0);
	assert(tsm_vte_new(&s->vte, s->con, vte_write, NULL, NULL, NULL) == 0);
	tsm_vte_set_palette(s->vte, "default");
	tsm_vte_input(s->vte, ESC "[?25l", 6);
	terminal_sync_init(&s->sync, s->vte, 200);
	assert(tsm_screen_new(&s->ref_con, NULL, NULL) == 0);
	assert(tsm_screen_resize(s->ref_con, COLS, ROWS) == 0);
	assert(tsm_vte_new(&s->ref_vte, s->ref_con, vte_write, NULL, NULL, NULL) == 0);
	tsm_vte_set_palette(s->ref_vte, "default");
	tsm_vte_input(s->ref_vte, ESC "[?25l", 6);
	output_init(&s->o, false);
	output_init(&s->ref, false);
	/* both start from the blank screen, presented long ago */
	output_frame(&s->o, s->con, s->vte);
	output_frame(&s->ref, s->ref_con, s->ref_vte);
}

static void sim_free(struct sim *s)
{
	output_destroy(&s->o);
	output_destroy(&s->ref);
	tsm_vte_unref(s->vte);
	tsm_screen_unref(s->con);
	tsm_vte_unref(s->ref_vte);
	tsm_screen_unref(s->ref_con);
}

/* a program's output: @writes writes of @frames frames each, @period_ns apart */
struct sched {
	const char *name;
	unsigned int writes, frames;
	uint64_t period_ns;
	size_t wsize;
	uint64_t wgap_ns;
	bool sync;
	uint64_t refresh_ns, draw_ns; /* 0: REFRESH_NS, DRAW_NS */
};

static char *sched_data(const struct sched *sc, struct write *w, size_t *total)
{
	size_t size = (size_t)sc->writes * sc->frames * 48 * 1024 + 4096;
	char *data = malloc(size);
	size_t n = 0;

	assert(data);
	rnd_state = 1;
	memset(grid, 0, sizeof(grid));
	for (unsigned int k = 0; k < sc->writes; k++) {
		w[k].start_ns = k * sc->period_ns;
		w[k].off = n;
		w[k].wsize = sc->wsize;
		w[k].wgap_ns = sc->wgap_ns;
		if (sc->sync)
			n += snprintf(data + n, size - n, ESC "[?2026h");
		for (unsigned int f = 0; f < sc->frames; f++)
			n += frame_bytes(data + n, size - n, k * sc->frames + f);
		if (sc->sync)
			n += snprintf(data + n, size - n, ESC "[?2026l");
		w[k].len = n - w[k].off;
		/* each write is in the pty before the next begins */
		assert(!sc->wsize || !sc->wgap_ns ||
		       (w[k].len + sc->wsize - 1) / sc->wsize * sc->wgap_ns <= sc->period_ns ||
		       k + 1 == sc->writes);
	}
	*total = n;
	return data;
}

/* presents per second of the program's output, from its first byte to the last present */
static double sim_rate(const struct sim *s)
{
	return s->last_ns ? s->presents * 1e9 / (double)s->last_ns : 0;
}

static void sim_report(const char *name, const char *display, const struct sim *b,
		       const struct sim *a)
{
	fprintf(stderr,
		"test_coalesce: %-24s %-8s before: %4u presents, %6.1f/s, %7.1f mixed cells per "
		"present (max %5" PRIu64 ") | after: %4u presents, %6.1f/s, %7.1f mixed cells per "
		"present (max %5" PRIu64 ")\n",
		name, display, b->presents, sim_rate(b),
		b->presents ? (double)b->mixed_cells / b->presents : 0.0, b->max_mixed, a->presents,
		sim_rate(a), a->presents ? (double)a->mixed_cells / a->presents : 0.0,
		a->max_mixed);
}

static void sim_fail(const char *name, const char *display, const char *what)
{
	fprintf(stderr, "test_coalesce: coalescing, %s, %s: %s\n", name, display, what);
	failures++;
}

/* one schedule before the rule and after it, on @flips' display; leaves the runs in @b and @a */
static void sim_sched(const struct sched *sc, bool flips, struct sim *b, struct sim *a)
{
	struct sim *runs[2] = {b, a};

	for (int i = 0; i < 2; i++) {
		struct sim *s = runs[i];

		sim_init(s, i == 1, flips);
		if (sc->refresh_ns)
			s->refresh_ns = sc->refresh_ns;
		if (sc->draw_ns)
			s->draw_ns = sc->draw_ns;
		s->nw = sc->writes;
		assert(s->nw <= sizeof(s->w) / sizeof(s->w[0]));
		s->data = sched_data(sc, s->w, &s->total);
		sim_run(s);
		assert(s->parsed == s->total);
		/* the last present shows the program's last frame whole */
		if (cells_differing(&s->o, &s->ref))
			sim_fail(sc->name, flips ? "flips" : "no flips",
				 "the screen is left part written");
	}
	sim_report(sc->name, flips ? "flips" : "no flips", b, a);
}

static void sim_done(struct sim *s)
{
	free(s->data);
	sim_free(s);
}

/*
 * A bulk write, as cat of a large file: 128 KB write() calls faster than
 * kmscon parses, so every read is full. A present part way through comes at
 * most once a refresh, and never two refreshes after the one before.
 */
static void test_coalesce_bulk_at(const struct sched *sc, bool flips, bool controls)
{
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(sc, flips, &b, &a);
	if (controls && a.mid_presents < 2)
		sim_fail(sc->name, display, "the write never presented part way through");
	if (a.min_mid_gap < a.refresh_ns)
		sim_fail(sc->name, display, "two presents part way through came within a refresh");
	if (a.max_partial_gap > 2 * a.refresh_ns)
		sim_fail(sc->name, display, "two refreshes in a row were skipped mid-write");
	if (controls && !flips && b.min_mid_gap >= b.refresh_ns)
		sim_fail(sc->name, display,
			 "control: before the rule, presents came a refresh apart");
	sim_done(&b);
	sim_done(&a);
}

/*
 * The bulk write at 60 Hz, and at 144 Hz with a draw of more than half a
 * refresh, where a refresh counted from the draw's end rather than its start
 * skips two refreshes in a row. Then draws of 1.1 and 1.5 refreshes at 144
 * and 60 Hz: output that never stops begins its next turn more than a refresh
 * after the last present, and a rule that takes the draw for an idle spell
 * waits out a second refresh.
 */
static void test_coalesce_bulk(bool flips)
{
	static const struct sched at60 = {"bulk write", 1, 300, 0, 128 * 1024, 30 * US, false};
	static const struct sched at144 = {
		"bulk write, 144 Hz", 1,     300, 0, 128 * 1024, 30 * US, false,
		1000000000ULL / 144,  6 * MS};

	static const struct sched slow[] = {
		{"bulk, 144 Hz, 1.1R draw", 1, 300, 0, 128 * 1024, 30 * US, false,
		 1000000000ULL / 144, 7640 * US},
		{"bulk, 144 Hz, 1.5R draw", 1, 300, 0, 128 * 1024, 30 * US, false,
		 1000000000ULL / 144, 10420 * US},
		{"bulk, 60 Hz, 1.1R draw", 1, 300, 0, 128 * 1024, 30 * US, false,
		 1000000000ULL / 60, 18330 * US},
		{"bulk, 60 Hz, 1.5R draw", 1, 300, 0, 128 * 1024, 30 * US, false,
		 1000000000ULL / 60, 25 * MS},
	};

	test_coalesce_bulk_at(&at60, flips, true);
	/* a draw and a turn take longer than a refresh: every turn's end is past the deadline */
	test_coalesce_bulk_at(&at144, flips, false);
	for (size_t i = 0; i < sizeof(slow) / sizeof(slow[0]); i++)
		test_coalesce_bulk_at(&slow[i], flips, false);
}

/*
 * Frames of three screens, written as 16 KB write() calls slower than kmscon
 * parses, 50 ms apart: each read is full and leaves the pty empty. Each
 * write begins after an idle spell, so its wait counts from its first read,
 * and the rule draws at its last, short read: no frame is presented part
 * written.
 */
static void test_coalesce_frames(bool flips)
{
	static const struct sched sc = {
		"frames, 16 KB writes", 24, 3, 50 * MS, NREAD, 300 * US, false};
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(&sc, flips, &b, &a);
	if (!b.mixed_cells)
		sim_fail(sc.name, display, "control: before the rule, no cell was presented mixed");
	if (a.mixed_cells)
		sim_fail(sc.name, display, "a frame was presented part written");
	if (a.max_partial_gap > 2 * a.refresh_ns)
		sim_fail(sc.name, display, "two refreshes in a row were skipped mid-write");
	sim_done(&b);
	sim_done(&a);
}

/*
 * The same frames as 4 KB write() calls, slower than kmscon parses: each
 * turn reads one call, under the threshold, so the rule holds nothing back
 * and presents are as before (ADR-201's cost: a write slower than kmscon
 * reads still tears).
 */
static void test_coalesce_short(bool flips)
{
	static const struct sched sc = {
		"frames, 4 KB writes", 24, 3, 50 * MS, 4096, 80 * US, false};
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(&sc, flips, &b, &a);
	if (a.presents != b.presents || a.mixed_cells != b.mixed_cells)
		sim_fail(sc.name, display, "turns under the threshold were held back");
	sim_done(&b);
	sim_done(&a);
}

/*
 * A large write, two screens in one write() call, and nothing after it: the
 * turn that reads it parses more than the threshold, so it waits, and with
 * no more output to end the wait the timer presents it whole within one
 * refresh.
 */
static void test_coalesce_large(bool flips)
{
	static const struct sched sc = {"large write, then quiet", 1, 2, 0, 0, 0, false};
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(&sc, flips, &b, &a);
	if (a.mixed_cells)
		sim_fail(sc.name, display, "the write was presented part written");
	if (a.last_why != WHY_TIMER)
		sim_fail(sc.name, display, "the write's end was not presented by the timer");
	else if (a.last_ns - a.parsed_ns > a.refresh_ns + a.draw_ns)
		sim_fail(sc.name, display, "the timer presented more than a refresh late");
	sim_done(&b);
	sim_done(&a);
}

/*
 * Small output, as a shell's: lines of text from one byte to just under the
 * threshold, each one write() call 50 ms after the last. Every one presents
 * at the same moment as without the rule.
 */
static void test_coalesce_small(bool flips)
{
	static const size_t sizes[] = {1, 100, 2000, 4095, 8000, 12000, 16000};
	const char *display = flips ? "flips" : "no flips";
	struct sim runs[2];

	for (int i = 0; i < 2; i++) {
		struct sim *s = &runs[i];
		size_t n = 0;

		sim_init(s, i == 1, flips);
		s->data = malloc(64 * 1024);
		assert(s->data);
		for (unsigned int k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
			s->w[k] = (struct write){k * 50 * MS, n, sizes[k], 0, 0};
			for (size_t j = 0; j < sizes[k]; j++)
				s->data[n++] = j % 80 == 78 ? '\r' : j % 80 == 79 ? '\n' : 'a' + k;
		}
		s->nw = sizeof(sizes) / sizeof(sizes[0]);
		s->total = n;
		sim_run(s);
	}
	fprintf(stderr, "test_coalesce: %-24s %-8s %u presents before, %u after\n", "small output",
		display, runs[0].presents, runs[1].presents);
	if (runs[1].presents != runs[0].presents ||
	    memcmp(runs[1].shown_at, runs[0].shown_at, sizeof(runs[0].shown_at)))
		sim_fail("small output", display, "small output waited for the rule");
	for (int i = 0; i < 2; i++) {
		free(runs[i].data);
		sim_free(&runs[i]);
	}
}

/*
 * Frames bracketed in mode 2026, written back to back as 16 KB write()
 * calls faster than kmscon parses, so frame ends fall inside full reads with
 * the next frame behind them: the hold decides. Nothing is presented part
 * written, and every present is as it was without the rule.
 */
static void test_coalesce_sync(bool flips)
{
	static const struct sched sc = {"frames in 2026", 24, 3, 2 * MS, NREAD, 100 * US, true};
	const char *display = flips ? "flips" : "no flips";
	struct sim b, a;

	sim_sched(&sc, flips, &b, &a);
	if (!a.presents)
		sim_fail(sc.name, display, "no frame was presented");
	if (a.mixed_cells || b.mixed_cells)
		sim_fail(sc.name, display, "a frame was presented part written");
	if (a.presents != b.presents || a.last_ns != b.last_ns)
		sim_fail(sc.name, display, "the rule changed a present the hold decides");
	sim_done(&b);
	sim_done(&a);
}

/*
 * A key press 2 ms after a present, and its echo, one byte, 0.2 ms later:
 * the echo is a short read, and presents as it did before the rule, at once
 * after the key's own draw, or at the flip in flight.
 */
static uint64_t echo_latency(bool coalesce, bool flips)
{
	static const char echo[] = "x";
	struct sim s;
	uint64_t latency;
	size_t n;

	sim_init(&s, coalesce, flips);
	s.data = malloc(64 * 1024);
	assert(s.data);
	rnd_state = 1;
	memset(grid, 0, sizeof(grid));
	n = frame_bytes(s.data, 64 * 1024, 0);
	s.w[0] = (struct write){0, 0, n, 0, 0};
	memcpy(s.data + n, echo, 1);
	/* the frame's present ends at 3.6 ms */
	s.key_ns = n * PARSE_NS_PER_KB / 1024 + DRAW_NS + 2 * MS;
	s.w[1] = (struct write){s.key_ns + ECHO_NS, n, 1, 0, 0};
	s.nw = 2;
	s.total = n + 1;
	sim_run(&s);
	latency = s.echo_latency;
	free(s.data);
	sim_free(&s);
	return latency;
}

static void test_coalesce_echo(bool flips)
{
	const char *display = flips ? "flips" : "no flips";
	uint64_t before = echo_latency(false, flips), after = echo_latency(true, flips);

	fprintf(stderr,
		"test_coalesce: %-24s %-8s echo shown %.2f ms after it arrived before, %.2f ms "
		"after\n",
		"key echo", display, before / 1e6, after / 1e6);
	if (!after)
		sim_fail("key echo", display, "the echo was not presented");
	else if (after != before)
		sim_fail("key echo", display, "the echo waited for the rule");
	/* the key's own draw, then the echo's, then the flips in flight */
	else if (after > 2 * DRAW_NS + (flips ? 2 * REFRESH_NS : 0))
		sim_fail("key echo", display, "the echo waited");
}

/*
 * The same rules on a real pty: a child writes the schedule's write() calls
 * into the pty at their times, and the test reads it as src/misc/pty.c does
 * (NREAD bytes a read, up to 50 reads a turn, until a read finds nothing),
 * parses with the real vte, and decides each turn's draw by the rule on the
 * real clock, a draw taking DRAW_NS. Reads are what the kernel hands over,
 * so the read sizes and turns are realistic; times vary run to run, so only
 * the last present is checked, and the rest is printed. Mixed cells are
 * counted afterwards, from the bytes parsed at each present.
 */

struct real {
	bool coalesce;
	size_t at[4096]; /* the bytes parsed at each present */
	unsigned int presents;
	uint64_t ns; /* from the first write to the last present */
	unsigned int turns, reads, full_reads, capped_turns, last_full_turns;
	/* turns ending part way through a write, and of the signals, which turns each flags */
	unsigned int mid_turns, cap_mid, cap_end, big_mid, big_end;
	uint64_t mixed_cells, max_mixed;
};

static uint64_t real_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void real_until(uint64_t t)
{
	while (real_now() < t)
		;
}

/* the child: each write's calls at their times, then a pause before closing */
static void real_writer(int fd, const char *data, const struct write *w, unsigned int nw,
			uint64_t t0)
{
	for (unsigned int k = 0; k < nw; k++) {
		size_t call = w[k].wsize ? w[k].wsize : w[k].len;

		for (size_t off = 0, j = 0; off < w[k].len; off += call, j++) {
			size_t n = w[k].len - off < call ? w[k].len - off : call;

			real_until(t0 + w[k].start_ns + j * w[k].wgap_ns);
			for (size_t done = 0; done < n;) {
				ssize_t r = write(fd, data + w[k].off + off + done, n - done);

				if (r < 0 && errno != EINTR && errno != EAGAIN)
					_exit(1);
				done += r > 0 ? r : 0;
			}
		}
	}
	usleep(300 * 1000);
	_exit(0);
}

static void real_present(struct real *r, size_t parsed, struct terminal_coalesce *co,
			 struct terminal_coalesce_display *cd)
{
	uint64_t start = real_now();

	real_until(start + DRAW_NS);
	terminal_coalesce_drawn(co, cd, start, real_now(), true);
	if (r->presents < sizeof(r->at) / sizeof(r->at[0]))
		r->at[r->presents] = parsed;
	r->presents++;
}

static void real_run(const struct sched *sc, bool coalesce, struct real *r)
{
	static char buf[NREAD];
	struct write w[512];
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct terminal_sync sync;
	struct terminal_coalesce co;
	struct terminal_coalesce_display cd = {0};
	struct termios tio;
	size_t total, parsed = 0;
	uint64_t t0;
	char *data;
	int m, sl;
	pid_t pid;
	bool ended = false; /* the writer closed the pty */

	memset(r, 0, sizeof(*r));
	r->coalesce = coalesce;
	data = sched_data(sc, w, &total);
	m = posix_openpt(O_RDWR | O_NOCTTY);
	assert(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0);
	sl = open(ptsname(m), O_RDWR | O_NOCTTY);
	assert(sl >= 0);
	assert(tcgetattr(sl, &tio) == 0);
	cfmakeraw(&tio);
	assert(tcsetattr(sl, TCSANOW, &tio) == 0);

	assert(tsm_screen_new(&con, NULL, NULL) == 0);
	assert(tsm_screen_resize(con, COLS, ROWS) == 0);
	assert(tsm_vte_new(&vte, con, vte_write, NULL, NULL, NULL) == 0);
	terminal_sync_init(&sync, vte, 200);
	terminal_coalesce_init(&co);
	co.mid_bytes = coalesce_n;

	t0 = real_now() + 20 * MS;
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		close(m);
		real_writer(sl, data, w, sc->writes, t0);
	}
	close(sl);
	assert(fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK) == 0);

	while (parsed < total || cd.due_ns) {
		/* a closed pty polls as hung up at once: from then on poll only waits */
		struct pollfd pfd = {ended ? -1 : m, POLLIN, 0};
		uint64_t now = real_now();
		int timeout = 2000;
		unsigned int num, last = 0, n_reads = 0;
		bool mid;

		if (cd.due_ns)
			timeout = cd.due_ns > now ? (int)((cd.due_ns - now + MS - 1) / MS) : 0;
		if (poll(&pfd, 1, timeout) < 0 && errno != EINTR)
			break;
		/* coalesce_timer_event */
		if (cd.due_ns && real_now() >= cd.due_ns) {
			terminal_coalesce_end(&cd);
			real_present(r, parsed, &co, &cd);
		}
		for (num = 50; num && !ended; num--) {
			uint64_t start;
			ssize_t n;

			n = read(m, buf, sizeof(buf));
			/* a pty master reads EIO once the writer is gone and nothing is left */
			ended = !n || (n < 0 && errno == EIO);
			if (n <= 0)
				break;
			start = real_now();
			assert(tsm_vte_input_bounded(vte, buf, n) == (size_t)n);
			terminal_coalesce_input(&co, start, real_now(), n);
			parsed += n;
			last = n;
			n_reads++;
			r->full_reads += n >= NREAD;
		}
		if (!n_reads) {
			if (ended && !cd.due_ns)
				break;
			continue;
		}
		r->turns++;
		r->reads += n_reads;
		r->capped_turns += !num;
		r->last_full_turns += last >= NREAD;
		{
			unsigned int k = 0;

			while (k < sc->writes && parsed >= w[k].off + w[k].len)
				k++;
			mid = k < sc->writes && parsed != w[k].off;
		}
		r->mid_turns += mid;
		if (!num)
			mid ? r->cap_mid++ : r->cap_end++;
		if (co.bytes >= coalesce_n)
			mid ? r->big_mid++ : r->big_end++;
		/* terminal_turn_end */
		if (!coalesce ||
		    !terminal_coalesce_decide(&sync, &co, &cd, false, real_now(), REFRESH_NS)) {
			terminal_coalesce_end(&cd);
			real_present(r, parsed, &co, &cd);
		}
		terminal_coalesce_turn_end(&co);
	}
	r->ns = cd.presented_ns + DRAW_NS - t0;
	waitpid(pid, NULL, 0);
	close(m);

	/* the cells each present showed mixed, against the end of the write in progress */
	if (parsed != total) {
		sim_fail(sc->name, "real pty", "the writer ended before its last write");
	} else {
		struct sim s;
		size_t fed = 0;

		sim_init(&s, false, false);
		for (unsigned int i = 0; i < r->presents && i < sizeof(r->at) / sizeof(r->at[0]);
		     i++) {
			unsigned int mixed;

			tsm_vte_input(s.vte, data + fed, r->at[i] - fed);
			fed = r->at[i];
			while (s.ref_writes < sc->writes && w[s.ref_writes].off < fed) {
				tsm_vte_input(s.ref_vte, data + w[s.ref_writes].off,
					      w[s.ref_writes].len);
				s.ref_writes++;
			}
			output_frame(&s.o, s.con, s.vte);
			output_frame(&s.ref, s.ref_con, s.ref_vte);
			mixed = cells_differing(&s.o, &s.ref);
			r->mixed_cells += mixed;
			if (mixed > r->max_mixed)
				r->max_mixed = mixed;
		}
		if (cells_differing(&s.o, &s.ref) || fed != total)
			sim_fail(sc->name, "real pty", "the screen is left part written");
		sim_free(&s);
	}
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	free(data);
}

static void real_sched(const struct sched *sc)
{
	struct real b, a;

	real_run(sc, false, &b);
	real_run(sc, true, &a);
	fprintf(stderr,
		"test_coalesce: %-24s real pty before: %4u presents, %6.1f/s, %7.1f mixed cells "
		"per present (max %5" PRIu64 ") | after: %4u presents, %6.1f/s, %7.1f mixed "
		"cells per present (max %5" PRIu64 ")\n",
		sc->name, b.presents, b.presents * 1e9 / b.ns,
		b.presents ? (double)b.mixed_cells / b.presents : 0.0, b.max_mixed, a.presents,
		a.presents * 1e9 / a.ns, a.presents ? (double)a.mixed_cells / a.presents : 0.0,
		a.max_mixed);
	fprintf(stderr,
		"test_coalesce: %-24s real pty reads: %u in %u turns, %u full (%d bytes), %u turns "
		"at the 50-read cap, %u turns ending on a full read\n",
		sc->name, b.reads, b.turns, b.full_reads, NREAD, b.capped_turns, b.last_full_turns);
	fprintf(stderr,
		"test_coalesce: %-24s real pty signals: %u turns end mid-write; 50-read cap flags "
		"%u "
		"of them and %u write ends; %d bytes in a turn flags %u of them and %u write "
		"ends\n",
		sc->name, b.mid_turns, b.cap_mid, b.cap_end, (int)coalesce_n, b.big_mid, b.big_end);
}

static void test_coalesce_real(void)
{
	static const struct sched scheds[] = {
		{"bulk write", 1, 300, 0, 128 * 1024, 30 * US, false},
		{"frames, 16 KB writes", 24, 3, 50 * MS, NREAD, 300 * US, false},
		{"frames, 4 KB writes", 24, 3, 50 * MS, 4096, 80 * US, false},
	};

	for (size_t i = 0; i < sizeof(scheds) / sizeof(scheds[0]); i++)
		real_sched(&scheds[i]);
}

static void test_coalesce(void)
{
	for (int flips = 0; flips < 2; flips++) {
		test_coalesce_bulk(flips);
		test_coalesce_frames(flips);
		test_coalesce_short(flips);
		test_coalesce_large(flips);
		test_coalesce_small(flips);
		test_coalesce_sync(flips);
		test_coalesce_echo(flips);
	}
	test_coalesce_real();
}

int main(int argc, char **argv)
{
	int ret = harness_init(argc, argv, "test_coalesce");

	if (ret)
		return ret;
	if (getenv("COALESCE_N"))
		coalesce_n = strtoul(getenv("COALESCE_N"), NULL, 0);

	test_coalesce();

	kmscon_font_unref(font);
	fprintf(stderr, "test_coalesce: %d failures\n", failures);
	return failures ? 1 : 0;
}
