/*
 * The render timer's counters (src/render_timer.c, ADR-103), driven by a fake
 * clock and a fake display: each test presents frames that cost a set CPU
 * time and damage a set area, completes flips, and closes intervals, then
 * checks the figures and the log line.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "render_timer.h"

#define MS 1000000ULL
#define S (1000 * MS)

struct fake_clock {
	uint64_t now;
	uint64_t cpu;
};

static uint64_t fake_now(void *data)
{
	return ((struct fake_clock *)data)->now;
}

static uint64_t fake_cpu(void *data)
{
	return ((struct fake_clock *)data)->cpu;
}

/* A display of @w x @h pixels whose frames the timer measures. */
struct fake_display {
	struct fake_clock clock;
	struct render_timer *rt;
	uint64_t w, h;
};

static void display_init(struct fake_display *d, uint64_t w, uint64_t h)
{
	struct render_timer_clock clock = {fake_now, fake_cpu, &d->clock};

	memset(d, 0, sizeof(*d));
	d->w = w;
	d->h = h;
	d->clock.now = 5 * S;
	assert(render_timer_new(&d->rt, &clock) == 0);
}

/* A render pass that costs @cpu of CPU time and @wall of wall time. */
static void display_frame(struct fake_display *d, uint64_t cpu, uint64_t wall, bool queued,
			  uint64_t damage)
{
	render_timer_frame_begin(d->rt);
	d->clock.cpu += cpu;
	d->clock.now += wall;
	render_timer_frame_end(d->rt, queued, damage);
}

static bool display_report(struct fake_display *d, struct render_timer_stats *st)
{
	return render_timer_report(d->rt, d->w * d->h, st);
}

static void test_new_rejects(void)
{
	struct render_timer_clock none = {NULL, fake_cpu, NULL};
	struct render_timer *rt = NULL;

	assert(render_timer_new(&rt, &none) < 0);
	assert(render_timer_new(&rt, NULL) < 0);
	assert(!rt);
	/* every call accepts the NULL a screen without a timer holds */
	render_timer_frame_begin(NULL);
	render_timer_frame_end(NULL, true, 1);
	render_timer_flip(NULL);
	render_timer_free(NULL);
}

static void test_quiet_from_start(void)
{
	struct fake_display d;
	struct render_timer_stats st;

	display_init(&d, 1280, 800);
	d.clock.now += S;
	assert(!display_report(&d, &st));
	render_timer_free(d.rt);
}

static void test_one_interval(void)
{
	struct fake_display d;
	struct render_timer_stats st;

	display_init(&d, 1280, 800);
	display_frame(&d, 3 * MS, 4 * MS, true, 1000);
	render_timer_flip(d.rt);
	display_frame(&d, 1 * MS, 4 * MS, true, 2000);
	render_timer_flip(d.rt);
	display_frame(&d, 2 * MS, 4 * MS, true, 6000);
	render_timer_flip(d.rt);
	d.clock.now += S - 12 * MS;

	assert(display_report(&d, &st));
	assert(st.interval_ns == S);
	assert(st.flips == 3);
	assert(st.frames == 3);
	assert(st.skipped == 0);
	assert(st.cpu_ns_mean == 2 * MS);
	assert(st.cpu_ns_median == 2 * MS);
	assert(st.cpu_ns_p95 == 3 * MS);
	assert(st.cpu_ns_max == 3 * MS);
	assert(st.damage_px_mean == 3000);
	assert(st.screen_px == 1280 * 800);
	render_timer_free(d.rt);
}

/* A pass that queues no flip costs CPU but is not a frame. */
static void test_skipped_passes(void)
{
	struct fake_display d;
	struct render_timer_stats st;

	display_init(&d, 100, 100);
	display_frame(&d, 4 * MS, 0, true, 50);
	display_frame(&d, 1 * MS, 0, false, 0);
	display_frame(&d, 2 * MS, 0, false, 0);
	d.clock.now += S;

	assert(display_report(&d, &st));
	assert(st.frames == 1);
	assert(st.flips == 0); /* the flip has not completed yet */
	assert(st.skipped == 2);
	assert(st.skipped_cpu_ns == 3 * MS);
	assert(st.cpu_ns_mean == 4 * MS);
	assert(st.damage_px_mean == 50);
	render_timer_free(d.rt);
}

/* Each interval counts its own frames; one flip alone makes it active. */
static void test_intervals_reset(void)
{
	struct fake_display d;
	struct render_timer_stats st;

	display_init(&d, 100, 100);
	display_frame(&d, 9 * MS, 0, true, 10000);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.frames == 1 && st.cpu_ns_max == 9 * MS);

	render_timer_flip(d.rt);
	d.clock.now += S / 2;
	assert(display_report(&d, &st));
	assert(st.interval_ns == S / 2);
	assert(st.flips == 1);
	assert(st.frames == 0 && st.skipped == 0);
	assert(st.cpu_ns_mean == 0 && st.cpu_ns_max == 0 && st.damage_px_mean == 0);
	render_timer_free(d.rt);
}

/* After activity, the first quiet interval has a line of zeros; the next has none. */
static void test_quiet_after_activity(void)
{
	struct fake_display d;
	struct render_timer_stats st;

	display_init(&d, 100, 100);
	display_frame(&d, MS, 0, true, 1);
	render_timer_flip(d.rt);
	d.clock.now += S;
	assert(display_report(&d, &st));

	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.interval_ns == S);
	assert(st.flips == 0 && st.frames == 0 && st.skipped == 0);

	d.clock.now += S;
	assert(!display_report(&d, &st));
	d.clock.now += S;
	assert(!display_report(&d, &st));

	/* activity again: a line, then one quiet line */
	render_timer_flip(d.rt);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.flips == 1);
	assert(display_report(&d, &st));
	assert(!display_report(&d, &st));
	render_timer_free(d.rt);
}

/*
 * The blink timers redraw twice a second. With nothing to blink, each pass is
 * skipped and the log stays quiet; once a blink queues frames and flips, the
 * second is logged, skipped passes included.
 */
static void test_blink_idle_and_flipping(void)
{
	struct fake_display d;
	struct render_timer_stats st;
	unsigned int i;

	display_init(&d, 100, 100);
	for (i = 0; i < 10; i++) {
		display_frame(&d, MS / 10, 0, false, 0);
		display_frame(&d, MS / 10, 0, false, 0);
		d.clock.now += S;
		assert(!display_report(&d, &st));
	}

	display_frame(&d, MS / 10, 0, false, 0);
	display_frame(&d, MS, 0, true, 100);
	render_timer_flip(d.rt);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.flips == 1 && st.frames == 1 && st.skipped == 1);
	assert(st.skipped_cpu_ns == MS / 10);

	/* the quiet second after it is logged once, then skipped passes stay quiet */
	display_frame(&d, MS / 10, 0, false, 0);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.flips == 0 && st.frames == 0 && st.skipped == 1);
	display_frame(&d, MS / 10, 0, false, 0);
	d.clock.now += S;
	assert(!display_report(&d, &st));
	render_timer_free(d.rt);
}

/* Median and 95th percentile by nearest rank, whatever order frames came in. */
static void test_percentiles(void)
{
	struct fake_display d;
	struct render_timer_stats st;
	unsigned int i;

	display_init(&d, 100, 100);
	for (i = 20; i >= 1; i--)
		display_frame(&d, i * MS, 0, true, 0);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.cpu_ns_median == 10 * MS);
	assert(st.cpu_ns_p95 == 19 * MS);
	assert(st.cpu_ns_max == 20 * MS);

	for (i = 1; i <= 100; i++)
		display_frame(&d, i * MS, 0, true, 0);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.cpu_ns_median == 50 * MS);
	assert(st.cpu_ns_p95 == 95 * MS);

	display_frame(&d, 7 * MS, 0, true, 0);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.cpu_ns_median == 7 * MS && st.cpu_ns_p95 == 7 * MS);
	render_timer_free(d.rt);
}

/* Frames past the sample buffer still count in the mean and the maximum. */
static void test_more_frames_than_samples(void)
{
	struct fake_display d;
	struct render_timer_stats st;
	unsigned int i;

	display_init(&d, 100, 100);
	for (i = 0; i < RENDER_TIMER_SAMPLES; i++)
		display_frame(&d, MS, 0, true, 10);
	for (i = 0; i < RENDER_TIMER_SAMPLES; i++)
		display_frame(&d, 3 * MS, 0, true, 30);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.frames == 2 * RENDER_TIMER_SAMPLES);
	assert(st.cpu_ns_mean == 2 * MS);
	assert(st.cpu_ns_max == 3 * MS);
	assert(st.damage_px_mean == 20);
	assert(st.cpu_ns_median == MS && st.cpu_ns_p95 == MS);
	render_timer_free(d.rt);
}

/* An end without a begin is not a frame. */
static void test_end_without_begin(void)
{
	struct fake_display d;
	struct render_timer_stats st;

	display_init(&d, 100, 100);
	render_timer_frame_end(d.rt, true, 5);
	display_frame(&d, MS, 0, true, 5);
	render_timer_frame_end(d.rt, true, 5);
	d.clock.now += S;
	assert(display_report(&d, &st));
	assert(st.frames == 1);
	render_timer_free(d.rt);
}

static void test_format(void)
{
	struct fake_display d;
	struct render_timer_stats st;
	char line[512];
	unsigned int i;

	display_init(&d, 1366, 768);
	for (i = 0; i < 59; i++) {
		display_frame(&d, 1234567, 0, true, 1366 * 19);
		render_timer_flip(d.rt);
	}
	display_frame(&d, 250000, 0, false, 0);
	d.clock.now += 1002 * MS;
	assert(display_report(&d, &st));
	assert(render_timer_format(&st, "pci-0000:00:01.0[Virtual-1]", line, sizeof(line)) == 0);
	assert(!strcmp(line, "render-timer: display=pci-0000:00:01.0[Virtual-1] interval_ms=1002.000 "
			     "presented_per_s=58.88 flips=59 frames=59 cpu_ms_mean=1.235 "
			     "cpu_ms_median=1.235 cpu_ms_p95=1.235 cpu_ms_max=1.235 "
			     "damage_px_mean=25954 damage_pct=2.47 skipped=1 skipped_cpu_ms=0.250"));

	/* a quiet line, and a display of no size */
	d.clock.now += S;
	assert(render_timer_report(d.rt, 0, &st));
	assert(render_timer_format(&st, "x", line, sizeof(line)) == 0);
	assert(!strcmp(line, "render-timer: display=x interval_ms=1000.000 presented_per_s=0.00 "
			     "flips=0 frames=0 cpu_ms_mean=0.000 cpu_ms_median=0.000 "
			     "cpu_ms_p95=0.000 cpu_ms_max=0.000 damage_px_mean=0 damage_pct=0.00 "
			     "skipped=0 skipped_cpu_ms=0.000"));

	/* a short buffer is cut, and terminated */
	assert(render_timer_format(&st, "x", line, 8) == 0);
	assert(!strcmp(line, "render-"));
	assert(render_timer_format(&st, "x", line, 0) < 0);
	render_timer_free(d.rt);
}

int main(void)
{
	test_new_rejects();
	test_quiet_from_start();
	test_one_interval();
	test_skipped_passes();
	test_intervals_reset();
	test_quiet_after_activity();
	test_blink_idle_and_flipping();
	test_percentiles();
	test_more_frames_than_samples();
	test_end_without_begin();
	test_format();
	printf("test_render_timer: all passed\n");
	return 0;
}
