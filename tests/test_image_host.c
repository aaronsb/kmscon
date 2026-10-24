/*
 * kmscon's side of the image helper (src/image_host.c, ADR-103's Notes of
 * 2026-10-24), driven on its own loop without libtsm, against the real
 * helper and against a stand-in (fake_image_helper.c) whose replies each
 * break one check.
 *
 * Every reply that fails a check, a helper that closes its socket, and one
 * that does not answer in time end the request with EBADF and the helper
 * with it; the next request starts a new helper, and every descriptor a bad
 * reply carried is closed. A packet with no request out stops the helper. The
 * helper's memory limit is three times the budget plus 64 MiB, and a larger
 * budget, or another user, restarts it.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "image_host.h"
#include "shl/eloop.h"

static struct ev_eloop *loop;
static char dir[PATH_MAX];

static struct {
	unsigned int calls;
	uint64_t id;
	int error;
	unsigned int width;
	unsigned int height;
	uint8_t first[4];
} got;

static void reply(struct image_host *host, uint64_t id, int error, unsigned int width,
		  unsigned int height, const uint8_t *rgba, void *data)
{
	got.calls++;
	got.id = id;
	got.error = error;
	got.width = width;
	got.height = height;
	if (!error)
		memcpy(got.first, rgba, 4);
	else
		assert(!rgba && !width && !height);
}

static unsigned int open_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	unsigned int n = 0;

	assert(d);
	while (readdir(d))
		n++;
	closedir(d);
	return n;
}

/* turns of the loop until the request out ends, at most @ms */
static void wait_reply(struct image_host *h, unsigned int ms)
{
	unsigned int calls = got.calls;

	while (ms && got.calls == calls) {
		ev_eloop_dispatch(loop, 10);
		ms -= ms < 10 ? ms : 10;
	}
	assert(got.calls == calls + 1 && !image_host_busy(h));
}

static struct image_host_request request(uint64_t id, unsigned int medium, const char *name)
{
	return (struct image_host_request){
		.id = id,
		.medium = medium,
		.format = 32,
		.width = 1,
		.height = 1,
		.name = name,
		.max_width = 64,
		.max_height = 64,
		.max_bytes = 1 << 20,
	};
}

/* the helper's RLIMIT_AS, from its /proc entry */
static unsigned long long helper_as(pid_t pid)
{
	char path[64], line[256];
	unsigned long long soft = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/limits", pid);
	f = fopen(path, "r");
	assert(f);
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "Max address space", 17))
			assert(sscanf(line + 17, " %llu", &soft) == 1);
	fclose(f);
	return soft;
}

static void test_real(const char *helper)
{
	static const uint8_t px[4] = {9, 8, 7, 6};
	char path[PATH_MAX + 16];
	struct image_host_request r;
	struct image_host *h;
	pid_t pid;
	FILE *f;

	snprintf(path, sizeof(path), "%s/px.rgba", dir);
	f = fopen(path, "w");
	assert(f && fwrite(px, 4, 1, f) == 1 && !fclose(f));

	assert(image_host_new(&h, loop, helper, reply, NULL) == 0);
	assert(!image_host_pid(h) && !image_host_busy(h));

	/* started at the first request, as this user, and kept for the next */
	r = request(1, 'f', path);
	assert(image_host_send(h, getuid(), &r) == 0);
	assert(image_host_busy(h) && image_host_send(h, getuid(), &r) == -EBUSY);
	pid = image_host_pid(h);
	assert(pid > 0);
	wait_reply(h, 5000);
	assert(got.id == 1 && !got.error && got.width == 1 && got.height == 1 &&
	       !memcmp(got.first, px, 4));
	/* three copies of the budget and 64 MiB, read once it answered, so after its exec */
	assert(helper_as(pid) == 3ull * (1 << 20) + (64ull << 20));
	r.id = 2;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(image_host_pid(h) == pid && got.id == 2 && !got.error);

	/* the helper's own errors pass through, and a discard is EINVAL */
	r = request(3, 'f', "/proc/self/status");
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.error == -EBADF && image_host_pid(h) == pid);
	r = request(4, 's', "/kmscon-no-such-object");
	r.discard = true;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.error == -EINVAL && image_host_pid(h) == pid);

	/* inline data travels in a memfd */
	r = request(5, 'd', NULL);
	r.data = px;
	r.data_len = 4;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(!got.error && !memcmp(got.first, px, 4) && image_host_pid(h) == pid);

	/* a smaller budget keeps the helper; a larger one restarts it with its own limit */
	r = request(6, 'f', path);
	r.max_bytes = 1 << 19;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(image_host_pid(h) == pid);
	r.id = 7;
	r.max_bytes = 4 << 20;
	assert(image_host_send(h, getuid(), &r) == 0);
	assert(image_host_pid(h) != pid);
	pid = image_host_pid(h);
	wait_reply(h, 5000);
	assert(!got.error);
	assert(helper_as(pid) == 3ull * (4 << 20) + (64ull << 20));

	/* another user stops it; kmscon, not root here, cannot start one as that user */
	if (getuid()) {
		r.id = 8;
		assert(image_host_send(h, getuid() + 1, &r) == -EBADF);
		assert(!image_host_pid(h) && !image_host_busy(h));
	}

	/* cancelled: no reply, and the helper goes */
	r = request(9, 'f', path);
	assert(image_host_send(h, getuid(), &r) == 0);
	image_host_cancel(h);
	assert(!image_host_busy(h) && !image_host_pid(h));
	ev_eloop_dispatch(loop, 50);
	assert(got.id == 7);

	/* a helper whose exec fails closes its socket: EBADF */
	image_host_free(h);
	assert(image_host_new(&h, loop, "/nonexistent/helper", reply, NULL) == 0);
	r = request(10, 'f', path);
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.id == 10 && got.error == -EBADF && !image_host_pid(h));
	image_host_free(h);
	unlink(path);
}

/*
 * @name's reply fails the check @why names: EBADF, the helper gone, every
 * descriptor closed. Raw formats meet the s and v check, so the others ask
 * for PNG.
 */
static void bad(struct image_host *h, const char *name, const char *why, uint64_t id)
{
	struct image_host_request r = request(id, 'f', name);
	unsigned int fds = open_fds();
	const char *failure;
	pid_t pid;

	if (strcmp(name, "sv")) {
		r.format = 100;
		r.width = r.height = 0;
	}
	if (!strcmp(name, "budget"))
		r.max_bytes = 64 * 64 * 4 - 1;
	assert(image_host_send(h, getuid(), &r) == 0);
	pid = image_host_pid(h);
	wait_reply(h, 5000);
	failure = image_host_last_failure(h);
	if (got.error != -EBADF || image_host_pid(h) || !failure || !strstr(failure, why)) {
		fprintf(stderr, "%s: error %d, helper %d, failure \"%s\", not \"%s\"\n", name,
			got.error, image_host_pid(h), failure ? failure : "", why);
		abort();
	}
	assert(got.id == id && pid > 0);
	if (open_fds() != fds) {
		fprintf(stderr, "%s: %u descriptors open, %u before\n", name, open_fds(), fds);
		abort();
	}
}

static void test_checks(const char *fake)
{
	static const char *const cases[][2] = {
		{"nofd", "without exactly one descriptor"},
		{"twofd", "without exactly one descriptor"},
		{"twocmsg", "without exactly one descriptor"},
		{"errfd", "an error reply with a descriptor"},
		{"unsealed", "not sealed"},
		{"noshrink", "not sealed"},
		{"nogrow", "not sealed"},
		{"nowrite", "not sealed"},
		{"futurewrite", "not sealed"},
		{"short", "not exactly width x height x 4"},
		{"long", "not exactly width x height x 4"},
		{"pipe", "not a regular file"},
		{"wide", "outside max-graphic-size"},
		{"zero", "outside max-graphic-size"},
		{"budget", "larger than the budget"},
		{"sv", "differs from s and v"},
		{"id", "another request"},
		{"version", "another version"},
		{"reserved", "reserved field"},
		{"status", "unknown status"},
		{"truncated", "not its header and reason"},
		{"reasonlen", "not its header and reason"},
		{"reasonmax", "not its header and reason"},
		{"close", "closed its socket"},
	};
	struct image_host_request r;
	struct image_host *h;
	unsigned int i;
	pid_t pid;

	assert(image_host_new(&h, loop, fake, reply, NULL) == 0);

	/* the stand-in's good reply passes, and so does an error with a reason */
	r = request(100, 'f', "good");
	assert(image_host_send(h, getuid(), &r) == 0);
	pid = image_host_pid(h);
	wait_reply(h, 5000);
	assert(!got.error && got.width == 1 && got.first[0] == 0x55);
	r = request(101, 'f', "error");
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.error == -EINVAL && image_host_pid(h) == pid);

	/* each case starts with no helper, so the descriptors open compare */
	image_host_cancel(h);
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		bad(h, cases[i][0], cases[i][1], 200 + i);

	/* a reply with no request out: the helper is stopped, no callback */
	r = request(300, 'f', "extra");
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(!got.error && image_host_pid(h));
	for (i = 0; i < 100 && image_host_pid(h); i++)
		ev_eloop_dispatch(loop, 10);
	assert(!image_host_pid(h) && got.calls && got.id == 300);
	assert(strstr(image_host_last_failure(h), "no request out"));

	/* no reply in time: EBADF and a new helper next time */
	image_host_set_timeout(h, 200);
	r = request(301, 'f', "hang");
	assert(image_host_send(h, getuid(), &r) == 0);
	pid = image_host_pid(h);
	wait_reply(h, 2000);
	assert(got.id == 301 && got.error == -EBADF && !image_host_pid(h));
	assert(strstr(image_host_last_failure(h), "in time"));
	assert(kill(pid, 0) == 0 || errno == ESRCH);
	r = request(302, 'f', "good");
	assert(image_host_send(h, getuid(), &r) == 0);
	assert(image_host_pid(h) && image_host_pid(h) != pid);
	wait_reply(h, 5000);
	assert(!got.error);
	image_host_free(h);

	/* the 2 s default */
	assert(IMAGE_HOST_TIMEOUT_MS == 2000);
	assert(image_host_new(&h, loop, fake, reply, NULL) == 0);
	r = request(303, 'f', "hang");
	assert(image_host_send(h, getuid(), &r) == 0);
	ev_eloop_dispatch(loop, 1500);
	assert(image_host_busy(h));
	wait_reply(h, 2000);
	assert(got.id == 303 && got.error == -EBADF);
	image_host_free(h);
}

int main(int argc, char **argv)
{
	char helper[PATH_MAX], fake[PATH_MAX];

	assert(argc == 4 && realpath(argv[1], helper) && realpath(argv[2], fake));
	signal(SIGPIPE, SIG_IGN);
	snprintf(dir, sizeof(dir), "%s/image-host-test-XXXXXX", argv[3]);
	assert(mkdtemp(dir));
	assert(ev_eloop_new(&loop) == 0);

	/* a relative path is refused: the child changes to / before its exec */
	{
		struct image_host *h;

		assert(image_host_new(&h, loop, "kmscon-image-helper", reply, NULL) == -EINVAL);
	}
	test_real(helper);
	test_checks(fake);

	ev_eloop_unref(loop);
	assert(rmdir(dir) == 0);
	printf("test_image_host: ok\n");
	return 0;
}
