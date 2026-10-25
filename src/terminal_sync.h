/*
 * kmscon - Synchronized output: when a terminal presents
 *
 * Copyright (c) 2026 the kmscon fork authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Synchronized output, DEC private mode 2026 (ADR-201 step 5)
 * A program brackets a frame in CSI ? 2026 h and CSI ? 2026 l. libtsm keeps
 * parsing during the hold and stops a bounded input call right after the
 * byte that ends it; these rules decide when the terminal presents:
 *
 *  - While a hold is in force, the displays keep the last complete frame: a
 *    redraw request is owed, and nothing is drawn.
 *  - When a frame ends, the terminal presents at once, or when the flip in
 *    flight completes; a hold begun by then waits for its own end.
 *  - A hold that has lasted sync-timeout ends through tsm_vte_end_sync, so
 *    DECRQM reports the mode reset, and the terminal presents what the
 *    program has drawn. If the pty still holds output kmscon read and has not
 *    parsed, the frame end may be in it: the end waits until that output is
 *    parsed, up to 1 s after the hold began, or sync-timeout when longer.
 *  - A display that no longer shows the last presented picture (a VT switch
 *    back in, a hotplug or mode set, a resize) draws the state as it is, and
 *    the hold goes on.
 *
 * The terminal owns the timer and the displays and passes the time in, so
 * the rules can be tested with a fake clock.
 */

#ifndef KMSCON_TERMINAL_SYNC_H
#define KMSCON_TERMINAL_SYNC_H

#include <libtsm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the longest an expiry waits for read output to be parsed, unless sync-timeout is longer */
#define TERMINAL_SYNC_WAIT_NS (1000ULL * 1000 * 1000)
/*
 * The longest sync-timeout: a program that stops inside a frame freezes the
 * console's output for that long, and ADR-201 rejects a hold without a bound.
 */
#define TERMINAL_SYNC_MAX_MS 10000

struct terminal_sync {
	struct tsm_vte *vte;
	uint64_t timeout_ns;  /* sync-timeout; 0: mode 2026 is not offered */
	uint64_t frames;      /* the vte's count of ended holds when last read */
	bool held;	      /* a hold is in force, and the timer runs for it */
	bool waiting;	      /* the timeout passed while read output was unparsed */
	uint64_t began_ns;    /* when the terminal saw the hold begin */
	uint64_t deadline_ns; /* when the timer is due */
};

/* what the terminal does after a call */
enum terminal_sync_action {
	TERMINAL_SYNC_NONE,    /* nothing */
	TERMINAL_SYNC_ARM,     /* arm the timer for deadline_ns */
	TERMINAL_SYNC_PRESENT, /* the frame ended: disarm the timer and present */
	TERMINAL_SYNC_EXPIRED, /* the timeout ended the hold: as PRESENT */
};

/* how a display answers a redraw request, or a completed flip */
enum terminal_sync_draw {
	TERMINAL_SYNC_DRAW, /* draw now */
	TERMINAL_SYNC_FLIP, /* draw when the flip in flight completes */
	TERMINAL_SYNC_OWED, /* a hold is in force: draw at its end */
	TERMINAL_SYNC_IDLE, /* a completed flip with no draw pending: nothing to do */
};

/*
 * Offers mode 2026 on @vte when @timeout_ms is above 0; at 0 DECRQM answers
 * 4 (permanently reset) and nothing is ever held. A timeout above
 * TERMINAL_SYNC_MAX_MS is cut to it. Returns the timeout in force, in ms, so
 * the terminal can say when it was cut.
 */
unsigned int terminal_sync_init(struct terminal_sync *s, struct tsm_vte *vte,
				unsigned int timeout_ms);

/*
 * The vte took input or was reset, at @now_ns. @unparsed: the pty still
 * holds output the input call left.
 */
enum terminal_sync_action terminal_sync_input(struct terminal_sync *s, uint64_t now_ns,
					      bool unparsed);

/* The timer fired at @now_ns. @unparsed as above. */
enum terminal_sync_action terminal_sync_timer(struct terminal_sync *s, uint64_t now_ns,
					      bool unparsed);

/*
 * The terminal cannot run the hold's timer: the hold ends through libtsm now,
 * so nothing is held without a timer. Returns PRESENT when a hold ended,
 * NONE otherwise.
 */
enum terminal_sync_action terminal_sync_cancel(struct terminal_sync *s);

/*
 * The relative time to arm the timer for, from @now_ns to the hold's
 * deadline; 1 once the deadline has passed, since 0 disarms a timer.
 */
uint64_t terminal_sync_arm_ns(const struct terminal_sync *s, uint64_t now_ns);

/* A hold is in force: present nothing unless the display lost its picture. */
bool terminal_sync_holding(const struct terminal_sync *s);

/*
 * A redraw request for one display. @swapping: a flip is in flight. @lost:
 * the display no longer shows the last presented picture.
 */
enum terminal_sync_draw terminal_sync_draw(const struct terminal_sync *s, bool swapping, bool lost);

/*
 * A flip completed on a display. @pending: a draw was deferred to it, and
 * @lost as above for that draw. Returns DRAW, OWED when a hold has begun
 * since the draw was deferred, or IDLE when none was.
 */
enum terminal_sync_draw terminal_sync_flipped(const struct terminal_sync *s, bool pending,
					      bool lost);

/*
 * Coalescing without mode 2026 (ADR-201 step 6). Outside a hold, the
 * terminal declines to draw the child's output while it is part way through
 * a write: the loop turn parsed at least TERMINAL_SYNC_COALESCE_BYTES, or the
 * pty still holds output an input call left. It draws at the first turn that
 * finds neither, or once one refresh of the display has passed since its last
 * present, whichever comes first. When the output began after an idle spell,
 * at least a refresh in which the terminal neither parsed nor drew, the
 * refresh is counted from when it began. The draw's own time does not count
 * as idle: when a draw takes longer than a refresh, output that never stopped
 * finds the refresh already passed, and draws at once. The wait adds at most
 * one refresh interval. Every other redraw request (a key press, the blink
 * timers, the pointer, a frame end, a lost picture) draws as before, so a
 * keystroke's echo, a short read, draws at once.
 *
 * The terminal keeps one struct terminal_coalesce for the child's output and
 * one struct terminal_coalesce_display for each display, tells them of each
 * input call and each draw, and asks terminal_coalesce_decide at the end of
 * each loop turn.
 */

/*
 * The refresh assumed for a display that does not report its own, or reports
 * one outside 20 to 1000 Hz (fbdev forces 1 Hz when the mode says 0): 60 Hz.
 */
#define TERMINAL_SYNC_REFRESH_NS (1000000000ULL / 60)

/* the bytes one loop turn parses that count as part way through a write */
#define TERMINAL_SYNC_COALESCE_BYTES 16384
#define TERMINAL_SYNC_REFRESH_MIN_NS (1000000000ULL / 1000)
#define TERMINAL_SYNC_REFRESH_MAX_NS (1000000000ULL / 20)

/* the child's output in the loop turn in progress, and when the terminal was last busy */
struct terminal_coalesce {
	size_t mid_bytes;   /* TERMINAL_SYNC_COALESCE_BYTES, unless a test measures another */
	size_t bytes;	    /* parsed this turn */
	uint64_t first_ns;  /* when this turn's first input call began; 0: none yet */
	uint64_t before_ns; /* busy_ns as that call began */
	uint64_t busy_ns;   /* when the last input call or draw ended */
};

/* one display's wait */
struct terminal_coalesce_display {
	uint64_t presented_ns; /* when the draw of its last present began */
	uint64_t due_ns;       /* when its waiting draw is due; 0: none waits */
};

void terminal_coalesce_init(struct terminal_coalesce *c);

/* An input call parsed @parsed bytes of the child's output, from @start_ns to @end_ns. */
void terminal_coalesce_input(struct terminal_coalesce *c, uint64_t start_ns, uint64_t end_ns,
			     size_t parsed);

/*
 * A draw of @d ran from @start_ns to @end_ns; @presented: it queued a frame.
 * It ends @d's wait.
 */
void terminal_coalesce_drawn(struct terminal_coalesce *c, struct terminal_coalesce_display *d,
			     uint64_t start_ns, uint64_t end_ns, bool presented);

/*
 * The loop turn ended and the child's output changed @d's screen, at @now_ns.
 * @unparsed: the pty still holds output an input call left. @refresh_ns: the
 * display's refresh (0: unknown, as is one out of range). Returns 0 to draw
 * now, which a hold turns into a redraw owed, or the time by which to draw if
 * no turn has drawn by then; @d waits until then.
 */
uint64_t terminal_coalesce_decide(const struct terminal_sync *s, const struct terminal_coalesce *c,
				  struct terminal_coalesce_display *d, bool unparsed,
				  uint64_t now_ns, uint64_t refresh_ns);

/* The loop turn ended: the next input call begins another. */
void terminal_coalesce_turn_end(struct terminal_coalesce *c);

/* @d's wait ends without a draw of its own: the timer fired, or cannot run. */
void terminal_coalesce_end(struct terminal_coalesce_display *d);

/*
 * Trying a failed font set again (#206). A display whose kmscon_text_set()
 * failed tries the set again in a frame's kmscon_text_prepare(), from the
 * time kmscon_text_retry_due() gives. A display with no output and no blink
 * draws no frame, so the terminal keeps a timer for that time and asks the
 * display for a frame when it fires. The terminal keeps one struct
 * terminal_retry for each display, reads @due into it before each call, arms
 * the timer for the earliest terminal_retry_earliest() gives, and when the
 * timer fires asks for a frame of each display terminal_retry_ask() names.
 *
 * A due time already past is armed for TERMINAL_RETRY_MIN_NS from now: the
 * timer may have expired unread when it was armed again (a timer armed again
 * drops its expiry), or the display that recovered was the one armed for. A
 * display the timer has asked a frame of is not asked again for the same due
 * time until TERMINAL_RETRY_ASK_NS has passed: a frame that tries the set
 * moves the due time (a second on, or to none), and one that a flip or a
 * hold of mode 2026 defers, or that a disabled display or a session not yet
 * in the foreground drops, is asked again then.
 */
#define TERMINAL_RETRY_MIN_NS (1000000000ULL / 1000)
#define TERMINAL_RETRY_ASK_NS 1000000000ULL

struct terminal_retry {
	uint64_t due;	   /* kmscon_text_retry_due(): the set may be tried from then; 0: none */
	uint64_t asked;	   /* the due time the timer last asked a frame for; 0: none */
	uint64_t asked_ns; /* when it asked */
};

/*
 * The time to arm the timer for, given @arm_ns, the earliest of the displays
 * before @r (0: none), and @r, at @now_ns; @awake: the terminal draws. 0:
 * none, the timer is disarmed.
 */
uint64_t terminal_retry_earliest(uint64_t arm_ns, const struct terminal_retry *r, bool awake,
				 uint64_t now_ns);

/* The timer fired at @now_ns: true to ask @r's display for a frame, which @r records. */
bool terminal_retry_ask(struct terminal_retry *r, uint64_t now_ns);

#endif /* KMSCON_TERMINAL_SYNC_H */
