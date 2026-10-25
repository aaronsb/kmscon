/*
 * kmscon - Render timer
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
 * Render timer (ADR-103, foot's render-timer option)
 * Counts, for one display, the page flips that complete, the thread CPU time
 * kmscon spends on each frame from preparing it to queuing its flip, and the
 * pixels in each frame's damage rectangles, and the redraws a hold of
 * synchronized output (mode 2026) owed and the holds that reached
 * sync-timeout (ADR-201). render_timer_report() closes an
 * interval and returns its figures; the terminal calls it once a second, logs
 * the line render_timer_format() writes, and draws the one
 * render_timer_format_osd() writes in a corner of the display.
 *
 * Both clocks are passed in, so the counters can be tested with fake ones.
 */

#ifndef KMSCON_RENDER_TIMER_H
#define KMSCON_RENDER_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Frames whose CPU time an interval keeps for its median and 95th
 * percentile. Frames past it still count in the mean and the maximum.
 */
#define RENDER_TIMER_SAMPLES 1024

struct render_timer_clock {
	uint64_t (*now_ns)(void *data); /* monotonic time */
	uint64_t (*cpu_ns)(void *data); /* the calling thread's CPU time */
	void *data;
};

/* The figures of one interval. */
struct render_timer_stats {
	uint64_t interval_ns;
	unsigned int flips;   /* page flips completed: frames presented */
	unsigned int frames;  /* frames rendered and queued for a flip */
	unsigned int skipped; /* render passes that drew nothing and queued no flip */
	uint64_t cpu_ns_mean; /* per queued frame */
	uint64_t cpu_ns_median;
	uint64_t cpu_ns_p95;
	uint64_t cpu_ns_max;
	uint64_t skipped_cpu_ns; /* all skipped passes together */
	uint64_t damage_px_mean; /* per queued frame */
	uint64_t screen_px;	 /* the display's pixels when the interval closed */
	unsigned int held;	 /* redraws owed to a hold of mode 2026, not drawn */
	unsigned int expired;	 /* holds of mode 2026 that reached sync-timeout */
};

struct render_timer;

int render_timer_new(struct render_timer **out, const struct render_timer_clock *clock);
void render_timer_free(struct render_timer *rt);

/* A render pass begins: kmscon starts preparing a frame. */
void render_timer_frame_begin(struct render_timer *rt);
/*
 * The pass ends. @queued says whether it queued a flip; @damage_px is the
 * pixels in the frame's damage rectangles.
 */
void render_timer_frame_end(struct render_timer *rt, bool queued, uint64_t damage_px);
/* A page flip completed on the display. */
void render_timer_flip(struct render_timer *rt);
/* A redraw was owed to a hold of synchronized output: nothing was drawn. */
void render_timer_held(struct render_timer *rt);
/* A hold of synchronized output reached sync-timeout and was ended. */
void render_timer_expired(struct render_timer *rt);

/*
 * Close the interval and start the next one. Returns true and fills @out
 * when the interval had a flip or a queued frame, or when it is the first
 * quiet interval after one that had, so a log shows where activity stopped;
 * returns false for the quiet intervals after that. Skipped passes alone do
 * not count: a blink timer that finds nothing to change redraws twice a
 * second and draws nothing. @screen_px is the display's size in pixels.
 */
bool render_timer_report(struct render_timer *rt, uint64_t screen_px,
			 struct render_timer_stats *out);

/* One log line of @st for the display @name, NUL-terminated in @buf. */
int render_timer_format(const struct render_timer_stats *st, const char *name, char *buf,
			size_t size);
/*
 * The on-screen display's line of @st, NUL-terminated in @buf: frames
 * presented per second, the median CPU time per frame and the mean share of
 * the screen damaged per frame, then the held and expired counts when either
 * is above 0.
 */
int render_timer_format_osd(const struct render_timer_stats *st, char *buf, size_t size);

#endif /* KMSCON_RENDER_TIMER_H */
