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

#include <string.h>
#include "terminal_sync.h"

#ifdef TSM_VTE_SYNC

unsigned int terminal_sync_init(struct terminal_sync *s, struct tsm_vte *vte,
				unsigned int timeout_ms)
{
	if (timeout_ms > TERMINAL_SYNC_MAX_MS)
		timeout_ms = TERMINAL_SYNC_MAX_MS;
	memset(s, 0, sizeof(*s));
	s->vte = vte;
	s->timeout_ns = (uint64_t)timeout_ms * 1000 * 1000;
	tsm_vte_set_sync(vte, timeout_ms > 0);
	s->frames = tsm_vte_get_sync_frames(vte);
	return timeout_ms;
}

static bool mode_set(const struct terminal_sync *s)
{
	return tsm_vte_get_mode(s->vte, true, 2026) == 1;
}

static void begin(struct terminal_sync *s, uint64_t now_ns)
{
	s->held = true;
	s->waiting = false;
	s->began_ns = now_ns;
	s->deadline_ns = now_ns + s->timeout_ns;
}

/* the timeout ends the hold: libtsm reports the mode reset from now on */
static enum terminal_sync_action expire(struct terminal_sync *s)
{
	tsm_vte_end_sync(s->vte);
	s->frames = tsm_vte_get_sync_frames(s->vte);
	s->held = false;
	s->waiting = false;
	return TERMINAL_SYNC_EXPIRED;
}

/*
 * A hold ended since the count was last read. After tsm_vte_input, which
 * parses through, the next hold may already be in force: the screen holds
 * part of that frame, so it is held in turn, from now.
 */
static bool ended(struct terminal_sync *s)
{
	uint64_t frames = tsm_vte_get_sync_frames(s->vte);

	if (frames == s->frames)
		return false;
	s->frames = frames;
	s->held = false;
	s->waiting = false;
	return true;
}

/* after an end: present, or hold the frame tsm_vte_input has already begun */
static enum terminal_sync_action after_end(struct terminal_sync *s, uint64_t now_ns)
{
	if (!mode_set(s))
		return TERMINAL_SYNC_PRESENT;
	begin(s, now_ns);
	return TERMINAL_SYNC_ARM;
}

enum terminal_sync_action terminal_sync_input(struct terminal_sync *s, uint64_t now_ns,
					      bool unparsed)
{
	if (!s->timeout_ns)
		return TERMINAL_SYNC_NONE;

	if (ended(s))
		return after_end(s, now_ns);
	if (!s->held) {
		if (!mode_set(s))
			return TERMINAL_SYNC_NONE;
		begin(s, now_ns);
		return TERMINAL_SYNC_ARM;
	}
	/* the timeout passed and the output it waited for is parsed */
	if (s->waiting && !unparsed)
		return expire(s);
	return TERMINAL_SYNC_NONE;
}

enum terminal_sync_action terminal_sync_timer(struct terminal_sync *s, uint64_t now_ns,
					      bool unparsed)
{
	uint64_t wait;

	if (!s->held)
		return TERMINAL_SYNC_NONE;
	/* a hold ended where nobody looked */
	if (ended(s))
		return after_end(s, now_ns);
	if (now_ns < s->deadline_ns)
		return TERMINAL_SYNC_ARM;

	wait = s->timeout_ns > TERMINAL_SYNC_WAIT_NS ? s->timeout_ns : TERMINAL_SYNC_WAIT_NS;
	if (unparsed && now_ns < s->began_ns + wait) {
		s->waiting = true;
		s->deadline_ns = s->began_ns + wait;
		return TERMINAL_SYNC_ARM;
	}
	return expire(s);
}

enum terminal_sync_action terminal_sync_cancel(struct terminal_sync *s)
{
	if (!s->held)
		return TERMINAL_SYNC_NONE;
	tsm_vte_end_sync(s->vte);
	s->frames = tsm_vte_get_sync_frames(s->vte);
	s->held = false;
	s->waiting = false;
	return TERMINAL_SYNC_PRESENT;
}

bool terminal_sync_holding(const struct terminal_sync *s)
{
	/* no hold without its timer, and none past its end, whoever ended it */
	return s->held && mode_set(s);
}

#else /* !TSM_VTE_SYNC: a libtsm without mode 2026 holds nothing */

unsigned int terminal_sync_init(struct terminal_sync *s, struct tsm_vte *vte,
				unsigned int timeout_ms)
{
	memset(s, 0, sizeof(*s));
	s->vte = vte;
	return 0;
}

enum terminal_sync_action terminal_sync_input(struct terminal_sync *s, uint64_t now_ns,
					      bool unparsed)
{
	return TERMINAL_SYNC_NONE;
}

enum terminal_sync_action terminal_sync_timer(struct terminal_sync *s, uint64_t now_ns,
					      bool unparsed)
{
	return TERMINAL_SYNC_NONE;
}

enum terminal_sync_action terminal_sync_cancel(struct terminal_sync *s)
{
	return TERMINAL_SYNC_NONE;
}

bool terminal_sync_holding(const struct terminal_sync *s)
{
	return false;
}

#endif /* TSM_VTE_SYNC */

uint64_t terminal_sync_arm_ns(const struct terminal_sync *s, uint64_t now_ns)
{
	return s->deadline_ns > now_ns ? s->deadline_ns - now_ns : 1;
}

enum terminal_sync_draw terminal_sync_draw(const struct terminal_sync *s, bool swapping, bool lost)
{
	if (!lost && terminal_sync_holding(s))
		return TERMINAL_SYNC_OWED;
	return swapping ? TERMINAL_SYNC_FLIP : TERMINAL_SYNC_DRAW;
}

enum terminal_sync_draw terminal_sync_flipped(const struct terminal_sync *s, bool pending,
					      bool lost)
{
	if (!pending)
		return TERMINAL_SYNC_IDLE;
	return terminal_sync_draw(s, false, lost);
}

void terminal_coalesce_init(struct terminal_coalesce *c)
{
	memset(c, 0, sizeof(*c));
	c->mid_bytes = TERMINAL_SYNC_COALESCE_BYTES;
}

void terminal_coalesce_input(struct terminal_coalesce *c, uint64_t start_ns, uint64_t end_ns,
			     size_t parsed)
{
	if (!c->first_ns) {
		c->first_ns = start_ns;
		c->before_ns = c->busy_ns;
	}
	c->bytes += parsed;
	if (end_ns > c->busy_ns)
		c->busy_ns = end_ns;
}

void terminal_coalesce_drawn(struct terminal_coalesce *c, struct terminal_coalesce_display *d,
			     uint64_t start_ns, uint64_t end_ns, bool presented)
{
	d->due_ns = 0;
	/* from the draw's start, so presents come a refresh apart, not a refresh and a draw */
	if (presented)
		d->presented_ns = start_ns;
	if (end_ns > c->busy_ns)
		c->busy_ns = end_ns;
}

uint64_t terminal_coalesce_decide(const struct terminal_sync *s, const struct terminal_coalesce *c,
				  struct terminal_coalesce_display *d, bool unparsed,
				  uint64_t now_ns, uint64_t refresh_ns)
{
	uint64_t due, began, before;

	/* a hold decides for itself, and presents its frame end at once */
	if ((c->bytes < c->mid_bytes && !unparsed) || terminal_sync_holding(s)) {
		d->due_ns = 0;
		return 0;
	}
	if (refresh_ns < TERMINAL_SYNC_REFRESH_MIN_NS || refresh_ns > TERMINAL_SYNC_REFRESH_MAX_NS)
		refresh_ns = TERMINAL_SYNC_REFRESH_NS;
	/* a wait keeps the deadline its first turn set */
	due = d->due_ns;
	if (!due) {
		began = c->first_ns ? c->first_ns : now_ns;
		before = c->first_ns ? c->before_ns : c->busy_ns;
		/*
		 * After an idle spell nothing was late until the output began. A
		 * draw is not idle: counted from its start, the refresh after a
		 * draw longer than a refresh would wait out a second one.
		 */
		if (began >= before + refresh_ns)
			due = began + refresh_ns;
		else
			due = d->presented_ns + refresh_ns;
	}
	d->due_ns = now_ns < due ? due : 0;
	return d->due_ns;
}

void terminal_coalesce_turn_end(struct terminal_coalesce *c)
{
	c->bytes = 0;
	c->first_ns = 0;
}

void terminal_coalesce_end(struct terminal_coalesce_display *d)
{
	d->due_ns = 0;
}
