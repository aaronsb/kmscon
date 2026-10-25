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
#include <stdint.h>

/* the longest an expiry waits for read output to be parsed, unless sync-timeout is longer */
#define TERMINAL_SYNC_WAIT_NS (1000ULL * 1000 * 1000)

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

/* how a display answers a redraw request */
enum terminal_sync_draw {
	TERMINAL_SYNC_DRAW, /* draw now */
	TERMINAL_SYNC_FLIP, /* draw when the flip in flight completes */
	TERMINAL_SYNC_OWED, /* a hold is in force: draw at its end */
};

/*
 * Offers mode 2026 on @vte when @timeout_ms is above 0; at 0 DECRQM answers
 * 4 (permanently reset) and nothing is ever held.
 */
void terminal_sync_init(struct terminal_sync *s, struct tsm_vte *vte, unsigned int timeout_ms);

/*
 * The vte took input or was reset, at @now_ns. @unparsed: the pty still
 * holds output the input call left.
 */
enum terminal_sync_action terminal_sync_input(struct terminal_sync *s, uint64_t now_ns,
					      bool unparsed);

/* The timer fired at @now_ns. @unparsed as above. */
enum terminal_sync_action terminal_sync_timer(struct terminal_sync *s, uint64_t now_ns,
					      bool unparsed);

/* A hold is in force: present nothing unless the display lost its picture. */
bool terminal_sync_holding(const struct terminal_sync *s);

/*
 * A redraw request for one display. @swapping: a flip is in flight. @lost:
 * the display no longer shows the last presented picture.
 */
enum terminal_sync_draw terminal_sync_draw(const struct terminal_sync *s, bool swapping, bool lost);

#endif /* KMSCON_TERMINAL_SYNC_H */
