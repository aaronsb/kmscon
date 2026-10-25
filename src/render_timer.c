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

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "render_timer.h"

struct render_timer {
	struct render_timer_clock clock;

	uint64_t start_ns; /* the open interval's start */
	bool reported;	   /* the last closed interval had a line */

	uint64_t frame_cpu_ns; /* CPU time when the current pass began */
	bool in_frame;

	unsigned int flips;
	unsigned int frames;
	unsigned int skipped;
	uint64_t cpu_ns_sum;
	uint64_t cpu_ns_max;
	uint64_t skipped_cpu_ns;
	uint64_t damage_px_sum;
	unsigned int held;
	unsigned int expired;
	uint64_t samples[RENDER_TIMER_SAMPLES];
};

int render_timer_new(struct render_timer **out, const struct render_timer_clock *clock)
{
	struct render_timer *rt;

	if (!out || !clock || !clock->now_ns || !clock->cpu_ns)
		return -EINVAL;

	rt = calloc(1, sizeof(*rt));
	if (!rt)
		return -ENOMEM;
	rt->clock = *clock;
	rt->start_ns = rt->clock.now_ns(rt->clock.data);
	*out = rt;
	return 0;
}

void render_timer_free(struct render_timer *rt)
{
	free(rt);
}

void render_timer_frame_begin(struct render_timer *rt)
{
	if (!rt)
		return;
	rt->frame_cpu_ns = rt->clock.cpu_ns(rt->clock.data);
	rt->in_frame = true;
}

void render_timer_frame_end(struct render_timer *rt, bool queued, uint64_t damage_px)
{
	uint64_t cpu;

	if (!rt || !rt->in_frame)
		return;
	rt->in_frame = false;
	cpu = rt->clock.cpu_ns(rt->clock.data) - rt->frame_cpu_ns;

	if (!queued) {
		rt->skipped++;
		rt->skipped_cpu_ns += cpu;
		return;
	}
	if (rt->frames < RENDER_TIMER_SAMPLES)
		rt->samples[rt->frames] = cpu;
	rt->frames++;
	rt->cpu_ns_sum += cpu;
	if (cpu > rt->cpu_ns_max)
		rt->cpu_ns_max = cpu;
	rt->damage_px_sum += damage_px;
}

void render_timer_flip(struct render_timer *rt)
{
	if (rt)
		rt->flips++;
}

void render_timer_held(struct render_timer *rt)
{
	if (rt)
		rt->held++;
}

void render_timer_expired(struct render_timer *rt)
{
	if (rt)
		rt->expired++;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/* The nearest-rank percentile @p of the @n sorted values @v. */
static uint64_t percentile(const uint64_t *v, unsigned int n, unsigned int p)
{
	unsigned int rank = (n * p + 99) / 100;

	return v[rank ? rank - 1 : 0];
}

bool render_timer_report(struct render_timer *rt, uint64_t screen_px,
			 struct render_timer_stats *out)
{
	uint64_t now;
	unsigned int n;
	bool active, line;

	if (!rt || !out)
		return false;

	now = rt->clock.now_ns(rt->clock.data);
	/* a skipped pass draws nothing: blinking that changes no cell is idle */
	active = rt->flips || rt->frames;
	line = active || rt->reported;

	if (line) {
		memset(out, 0, sizeof(*out));
		out->interval_ns = now - rt->start_ns;
		out->flips = rt->flips;
		out->frames = rt->frames;
		out->skipped = rt->skipped;
		out->skipped_cpu_ns = rt->skipped_cpu_ns;
		out->screen_px = screen_px;
		out->held = rt->held;
		out->expired = rt->expired;
		if (rt->frames) {
			n = rt->frames < RENDER_TIMER_SAMPLES ? rt->frames : RENDER_TIMER_SAMPLES;
			qsort(rt->samples, n, sizeof(*rt->samples), cmp_u64);
			out->cpu_ns_mean = rt->cpu_ns_sum / rt->frames;
			out->cpu_ns_median = percentile(rt->samples, n, 50);
			out->cpu_ns_p95 = percentile(rt->samples, n, 95);
			out->cpu_ns_max = rt->cpu_ns_max;
			out->damage_px_mean = rt->damage_px_sum / rt->frames;
		}
	}

	rt->reported = active;
	rt->start_ns = now;
	rt->flips = 0;
	rt->frames = 0;
	rt->skipped = 0;
	rt->cpu_ns_sum = 0;
	rt->cpu_ns_max = 0;
	rt->skipped_cpu_ns = 0;
	rt->damage_px_sum = 0;
	rt->held = 0;
	rt->expired = 0;
	return line;
}

/* @ns in milliseconds with three decimals: microsecond resolution. */
static void ms(char *buf, size_t size, uint64_t ns)
{
	uint64_t us = (ns + 500) / 1000;

	snprintf(buf, size, "%" PRIu64 ".%03" PRIu64, us / 1000, us % 1000);
}

int render_timer_format(const struct render_timer_stats *st, const char *name, char *buf,
			size_t size)
{
	char interval[32], mean[32], median[32], p95[32], max[32], skipped[32];
	uint64_t rate, pct;

	if (!st || !buf || !size)
		return -EINVAL;

	/* flips per second and damage share, both in hundredths */
	rate = st->interval_ns ? (uint64_t)st->flips * 100000000000ULL / st->interval_ns : 0;
	pct = st->screen_px ? st->damage_px_mean * 10000 / st->screen_px : 0;
	ms(interval, sizeof(interval), st->interval_ns);
	ms(mean, sizeof(mean), st->cpu_ns_mean);
	ms(median, sizeof(median), st->cpu_ns_median);
	ms(p95, sizeof(p95), st->cpu_ns_p95);
	ms(max, sizeof(max), st->cpu_ns_max);
	ms(skipped, sizeof(skipped), st->skipped_cpu_ns);

	snprintf(buf, size,
		 "render-timer: display=%s interval_ms=%s presented_per_s=%" PRIu64 ".%02" PRIu64
		 " flips=%u frames=%u cpu_ms_mean=%s cpu_ms_median=%s cpu_ms_p95=%s"
		 " cpu_ms_max=%s damage_px_mean=%" PRIu64 " damage_pct=%" PRIu64 ".%02" PRIu64
		 " skipped=%u skipped_cpu_ms=%s held=%u expired=%u",
		 name ? name : "?", interval, rate / 100, rate % 100, st->flips, st->frames, mean,
		 median, p95, max, st->damage_px_mean, pct / 100, pct % 100, st->skipped, skipped,
		 st->held, st->expired);
	return 0;
}

int render_timer_format_osd(const struct render_timer_stats *st, char *buf, size_t size)
{
	char median[32];
	uint64_t rate, pct;
	int n;

	if (!st || !buf || !size)
		return -EINVAL;

	/* flips per second and damage share, both in tenths */
	rate = st->interval_ns ? ((uint64_t)st->flips * 10000000000ULL + st->interval_ns / 2) /
					 st->interval_ns
			       : 0;
	pct = st->screen_px ? (st->damage_px_mean * 1000 + st->screen_px / 2) / st->screen_px : 0;
	ms(median, sizeof(median), st->cpu_ns_median);
	n = snprintf(buf, size, "%" PRIu64 ".%" PRIu64 " fps %s ms %" PRIu64 ".%" PRIu64 "%%",
		     rate / 10, rate % 10, median, pct / 10, pct % 10);
	if ((st->held || st->expired) && n > 0 && (size_t)n < size)
		snprintf(buf + n, size - n, " %u held %u expired", st->held, st->expired);
	return 0;
}
