/*
 * kmscon's side of the image helper (src/image_host.c, ADR-103's Notes of
 * 2026-10-24), driven on its own loop without libtsm, against the real
 * helper and against a stand-in (fake_image_helper.c) whose replies each
 * break one check.
 *
 * Every reply that fails a check, a helper that closes its socket, and one
 * that does not answer in time end the request as IMAGE_HOST_FAILED, EBADF
 * to the client, and the helper with it; the next request starts a new
 * helper, and every descriptor a bad reply carried is closed. A packet with
 * no request out stops the helper. The helper's outcomes pass through, a
 * discard's among them, and keep the helper. A request whose min_bytes
 * passes the budget is answered KMSCON_IMAGE_OVER_BUDGET without reaching
 * the helper. The helper's memory limit is three times the budget plus
 * 64 MiB, and a larger budget, or another user, restarts it.
 *
 * The helper starts with descriptors 0 to 2 on /dev/null and its socket as 3
 * and nothing else, TMPDIR its only environment variable, / as its working
 * directory, no signal blocked, no_new_privs set and no ambient capability,
 * whatever kmscon had. Run as root (in the dev container), it also takes the
 * user's uid, gid and groups, and loses an ambient capability kmscon raised.
 * The helper's reason reaches the log as printable ASCII only.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../src/image_host.c"

static struct ev_eloop *loop;
static char dir[PATH_MAX];

static struct {
	unsigned int calls;
	uint64_t id;
	unsigned int status;
	unsigned int width;
	unsigned int height;
	size_t len;
	uint8_t first[4];
} got;

/* a discard's KMSCON_IMAGE_OK, like any other outcome, carries no bytes */
static void reply(struct image_host *host, uint64_t id, unsigned int status, unsigned int width,
		  unsigned int height, const uint8_t *data, size_t len, void *cb_data)
{
	got.calls++;
	got.id = id;
	got.status = status;
	got.width = width;
	got.height = height;
	got.len = len;
	if (data)
		memcpy(got.first, data, len < 4 ? len : 4);
	else
		assert(!width && !height && !len);
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

/* four bytes, as they are */
static struct image_host_request request(uint64_t id, unsigned int medium, const char *name)
{
	return (struct image_host_request){
		.id = id,
		.medium = medium,
		.min_bytes = 4,
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

	/* min_bytes past the budget: answered from the loop, no helper started or asked */
	r = request(1, 'f', path);
	r.min_bytes = r.max_bytes + 1;
	assert(image_host_send(h, getuid(), &r) == 0);
	assert(image_host_busy(h) && !image_host_pid(h) && !got.calls);
	wait_reply(h, 1000);
	assert(got.id == 1 && got.status == KMSCON_IMAGE_OVER_BUDGET && !image_host_pid(h));

	/* started at the first request, as this user, and kept for the next */
	r = request(2, 'f', path);
	assert(image_host_send(h, getuid(), &r) == 0);
	assert(image_host_busy(h) && image_host_send(h, getuid(), &r) == -EBUSY);
	pid = image_host_pid(h);
	assert(pid > 0);
	wait_reply(h, 5000);
	assert(got.id == 2 && got.status == KMSCON_IMAGE_OK && !got.width && !got.height &&
	       got.len == 4 && !memcmp(got.first, px, 4));
	/* three copies of the budget and 64 MiB, read once it answered, so after its exec */
	assert(helper_as(pid) == 3ull * (1 << 20) + (64ull << 20));
	r.id = 3;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(image_host_pid(h) == pid && got.id == 3 && got.status == KMSCON_IMAGE_OK);

	/* the helper's outcomes pass through, and keep it */
	r = request(4, 'f', "/proc/self/status");
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_UNREADABLE && image_host_pid(h) == pid);
	r = request(5, 'f', path);
	r.min_bytes = 5;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_TOO_SHORT && image_host_pid(h) == pid);
	/* again past the budget, with a helper running: answered, nothing sent, the helper kept */
	r = request(6, 'f', path);
	r.min_bytes = r.max_bytes + 1;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 1000);
	assert(got.status == KMSCON_IMAGE_OVER_BUDGET && image_host_pid(h) == pid);
	assert(!image_host_last_failure(h));
	/* a discard is answered KMSCON_IMAGE_OK with no bytes, and the helper still runs */
	r = request(7, 's', "/kmscon-no-such-object");
	r.flags = KMSCON_IMAGE_DISCARD;
	r.min_bytes = 0;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_OK && !got.len && image_host_pid(h) == pid);
	r = request(8, 'f', path);
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_OK && image_host_pid(h) == pid);

	/* inline data travels in a memfd */
	r = request(9, 'd', NULL);
	r.data = px;
	r.data_len = 4;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_OK && !memcmp(got.first, px, 4) &&
	       image_host_pid(h) == pid);

	/* a smaller budget keeps the helper; a larger one restarts it with its own limit */
	r = request(10, 'f', path);
	r.max_bytes = 1 << 19;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(image_host_pid(h) == pid);
	r.id = 11;
	r.max_bytes = 4 << 20;
	assert(image_host_send(h, getuid(), &r) == 0);
	assert(image_host_pid(h) != pid);
	pid = image_host_pid(h);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_OK);
	assert(helper_as(pid) == 3ull * (4 << 20) + (64ull << 20));

	/* another user stops it; kmscon, not root here, cannot start one as that user */
	if (getuid()) {
		r.id = 12;
		assert(image_host_send(h, getuid() + 1, &r) == -EBADF);
		assert(!image_host_pid(h) && !image_host_busy(h));
	}

	/* cancelled: no reply, and the helper goes */
	r = request(14, 'f', path);
	assert(image_host_send(h, getuid(), &r) == 0);
	image_host_cancel(h);
	assert(!image_host_busy(h) && !image_host_pid(h));
	ev_eloop_dispatch(loop, 50);
	assert(got.id != 14);
	/* and a deferred answer cancelled: none either */
	r = request(15, 'f', path);
	r.min_bytes = r.max_bytes + 1;
	assert(image_host_send(h, getuid(), &r) == 0);
	image_host_cancel(h);
	ev_eloop_dispatch(loop, 50);
	assert(got.id != 15 && !image_host_busy(h));

	/* a helper whose exec fails closes its socket: a failure */
	image_host_free(h);
	assert(image_host_new(&h, loop, "/nonexistent/helper", reply, NULL) == 0);
	r = request(16, 'f', path);
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.id == 16 && got.status == IMAGE_HOST_FAILED && !image_host_pid(h));
	image_host_free(h);
	unlink(path);
}

/*
 * @name's reply to a PNG request, or with @raw to a request for bytes, or a
 * discard, fails the check @why names: IMAGE_HOST_FAILED, the helper gone,
 * every descriptor closed
 */
static void bad(struct image_host *h, const char *name, const char *why, char kind, uint64_t id)
{
	struct image_host_request r = request(id, 'f', name);
	unsigned int fds = open_fds();
	const char *failure;
	pid_t pid;

	if (kind == 'p') {
		r.flags = KMSCON_IMAGE_PNG;
		r.min_bytes = 0;
	} else if (kind == 'x') {
		r.medium = 't';
		r.flags = KMSCON_IMAGE_DISCARD;
		r.min_bytes = 0;
	}
	if (!strcmp(name, "budget"))
		r.max_bytes = 64 * 64 * 4 - 1;
	if (!strcmp(name, "huge"))
		r.max_width = r.max_height = 0xffffffffu;
	assert(image_host_send(h, getuid(), &r) == 0);
	pid = image_host_pid(h);
	wait_reply(h, 5000);
	failure = image_host_last_failure(h);
	if (got.status != IMAGE_HOST_FAILED || image_host_pid(h) || !failure ||
	    !strstr(failure, why)) {
		fprintf(stderr, "%s: status %u, helper %d, failure \"%s\", not \"%s\"\n", name,
			got.status, image_host_pid(h), failure ? failure : "", why);
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
	/* the name, what kmscon finds, and the request: p a PNG, r bytes, x a discard */
	static const struct {
		const char *name;
		const char *why;
		char kind;
	} cases[] = {
		{"nofd", "without exactly one descriptor", 'p'},
		{"nofd", "without exactly one descriptor", 'r'},
		{"twofd", "without exactly one descriptor", 'p'},
		{"twocmsg", "without exactly one descriptor", 'r'},
		{"errfd", "a descriptor it should not carry", 'p'},
		{"discardfd", "a descriptor it should not carry", 'x'},
		{"unsealed", "not sealed", 'p'},
		{"noshrink", "not sealed", 'r'},
		{"nogrow", "not sealed", 'p'},
		{"nowrite", "not sealed", 'r'},
		{"futurewrite", "not sealed", 'p'},
		{"futurewrite", "not sealed", 'r'},
		{"short", "not exactly the size", 'p'},
		{"long", "not exactly the size", 'p'},
		{"short", "not exactly the size", 'r'},
		{"long", "not exactly the size", 'r'},
		{"pipe", "not a regular file", 'p'},
		{"pipe", "not a regular file", 'r'},
		{"wide", "outside max-graphic-size", 'p'},
		{"zero", "outside max-graphic-size", 'p'},
		{"budget", "larger than the budget", 'p'},
		{"huge", "larger than the budget", 'p'},
		{"sized", "a size for bytes", 'r'},
		{"id", "another request", 'p'},
		{"version", "another version", 'r'},
		{"reserved", "reserved field", 'p'},
		{"status", "unknown status", 'r'},
		{"truncated", "not its header and reason", 'p'},
		{"reasonlen", "not its header and reason", 'r'},
		{"reasonmax", "not its header and reason", 'p'},
		{"close", "closed its socket", 'r'},
	};
	struct image_host_request r;
	struct image_host *h;
	unsigned int i;
	pid_t pid;

	assert(image_host_new(&h, loop, fake, reply, NULL) == 0);

	/* the stand-in's good replies pass, and so does an outcome with a reason */
	r = request(100, 'f', "good");
	r.flags = KMSCON_IMAGE_PNG;
	r.min_bytes = 0;
	assert(image_host_send(h, getuid(), &r) == 0);
	pid = image_host_pid(h);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_OK && got.width == 1 && got.height == 1 && got.len == 4 &&
	       got.first[0] == 0x55);
	r = request(101, 'f', "good");
	r.min_bytes = 7;
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_OK && !got.width && got.len == 7 &&
	       image_host_pid(h) == pid);
	r = request(102, 'f', "error");
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_CORRUPT && image_host_pid(h) == pid);

	/* each case starts with no helper, so the descriptors open compare */
	image_host_cancel(h);
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		bad(h, cases[i].name, cases[i].why, cases[i].kind, 200 + i);

	/* a reply with no request out: the helper is stopped, no callback */
	r = request(300, 'f', "extra");
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_OK && image_host_pid(h));
	for (i = 0; i < 100 && image_host_pid(h); i++)
		ev_eloop_dispatch(loop, 10);
	assert(!image_host_pid(h) && got.calls && got.id == 300);
	assert(strstr(image_host_last_failure(h), "no request out"));

	/* no reply in time: a failure, and a new helper next time */
	image_host_set_timeout(h, 200);
	r = request(301, 'f', "hang");
	assert(image_host_send(h, getuid(), &r) == 0);
	pid = image_host_pid(h);
	wait_reply(h, 2000);
	assert(got.id == 301 && got.status == IMAGE_HOST_FAILED && !image_host_pid(h));
	assert(strstr(image_host_last_failure(h), "in time"));
	assert(kill(pid, 0) == 0 || errno == ESRCH);
	r = request(302, 'f', "good");
	assert(image_host_send(h, getuid(), &r) == 0);
	assert(image_host_pid(h) && image_host_pid(h) != pid);
	wait_reply(h, 5000);
	assert(got.status == KMSCON_IMAGE_OK);
	image_host_free(h);

	/* the 2 s default */
	assert(IMAGE_HOST_TIMEOUT_MS == 2000);
	assert(image_host_new(&h, loop, fake, reply, NULL) == 0);
	r = request(303, 'f', "hang");
	assert(image_host_send(h, getuid(), &r) == 0);
	ev_eloop_dispatch(loop, 1500);
	assert(image_host_busy(h));
	wait_reply(h, 2000);
	assert(got.id == 303 && got.status == IMAGE_HOST_FAILED);
	image_host_free(h);
}

/* the value of @key in the helper's /proc status, or "" */
static const char *status_of(pid_t pid, const char *key)
{
	static char value[256];
	char path[64], line[512];
	size_t n = strlen(key);
	FILE *f;

	value[0] = 0;
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	f = fopen(path, "r");
	assert(f);
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, n) && line[n] == ':') {
			snprintf(value, sizeof(value), "%s",
				 line + n + 1 + strspn(line + n + 1, " \t"));
			value[strcspn(value, "\n")] = 0;
			/* the kernel ends Groups with a space */
			for (n = strlen(value); n && value[n - 1] == ' '; n--)
				value[n - 1] = 0;
			n = strlen(key);
		}
	}
	fclose(f);
	return value;
}

/* raises CAP_NET_RAW as an ambient capability of this process (root only) */
static bool raise_ambient(void)
{
	struct __user_cap_header_struct hdr = {.version = _LINUX_CAPABILITY_VERSION_3};
	struct __user_cap_data_struct caps[2];

	if (syscall(SYS_capget, &hdr, caps))
		return false;
	caps[0].inheritable |= 1u << CAP_NET_RAW;
	return !syscall(SYS_capset, &hdr, caps) &&
	       !prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_RAW, 0, 0);
}

/*
 * The helper as started, read from /proc: what kmscon leaked to it (a
 * descriptor, a blocked signal, an environment variable, an ambient
 * capability) does not reach it
 */
static void test_hardening(const char *helper)
{
	struct image_host_request r = request(400, 'f', "/nonexistent");
	char path[PATH_MAX + 64], link[PATH_MAX], want[64];
	char env[4096];
	unsigned int fds = 0;
	bool ambient = false;
	struct dirent *e;
	struct image_host *h;
	sigset_t usr1, old;
	ssize_t n;
	pid_t pid;
	FILE *f;
	DIR *d;
	int leak, i;

	leak = open("/dev/zero", O_RDONLY); /* no O_CLOEXEC */
	assert(leak >= 0);
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	assert(!sigprocmask(SIG_BLOCK, &usr1, &old));
	assert(!setenv("KMSCON_TEST_SECRET", "x", 1) && !setenv("TMPDIR", "/tmp", 1));
	if (!geteuid()) {
		ambient = raise_ambient();
		assert(ambient);
	}

	/* as root, the helper's own uid 0, so an ambient capability would survive */
	assert(image_host_new(&h, loop, helper, reply, NULL) == 0);
	assert(image_host_send(h, getuid(), &r) == 0);
	wait_reply(h, 5000);
	pid = image_host_pid(h);
	assert(got.status == KMSCON_IMAGE_UNREADABLE && pid > 0);

	snprintf(path, sizeof(path), "/proc/%d/fd", pid);
	d = opendir(path);
	assert(d);
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/proc/%d/fd/%s", pid, e->d_name);
		n = readlink(path, link, sizeof(link) - 1);
		assert(n > 0);
		link[n] = 0;
		i = atoi(e->d_name);
		if (i < 3)
			assert(!strcmp(link, "/dev/null"));
		else
			assert(i == 3 && !strncmp(link, "socket:", 7));
		fds++;
	}
	closedir(d);
	assert(fds == 4);

	snprintf(path, sizeof(path), "/proc/%d/environ", pid);
	f = fopen(path, "r");
	assert(f);
	n = fread(env, 1, sizeof(env), f);
	fclose(f);
	assert(n == (ssize_t)sizeof("TMPDIR=/tmp") && !memcmp(env, "TMPDIR=/tmp", n));

	snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
	n = readlink(path, link, sizeof(link) - 1);
	assert(n == 1 && link[0] == '/');

	assert(!strcmp(status_of(pid, "SigBlk"), "0000000000000000"));
	assert(!strcmp(status_of(pid, "NoNewPrivs"), "1"));
	assert(!strcmp(status_of(pid, "CapAmb"), "0000000000000000"));
	image_host_free(h);

	/* as root, the session user's uid, gid and groups */
	if (!geteuid()) {
		struct passwd *pw = getpwnam("nobody");
		gid_t groups[64];
		int ng = 64, k;
		char line[256];
		size_t off = 0;

		assert(pw);
		assert(getgrouplist(pw->pw_name, pw->pw_gid, groups, &ng) >= 0);
		assert(image_host_new(&h, loop, helper, reply, NULL) == 0);
		r.id = 401;
		assert(image_host_send(h, pw->pw_uid, &r) == 0);
		wait_reply(h, 5000);
		pid = image_host_pid(h);
		assert(pid > 0);
		snprintf(want, sizeof(want), "%u\t%u\t%u\t%u", pw->pw_uid, pw->pw_uid, pw->pw_uid,
			 pw->pw_uid);
		assert(!strcmp(status_of(pid, "Uid"), want));
		snprintf(want, sizeof(want), "%u\t%u\t%u\t%u", pw->pw_gid, pw->pw_gid, pw->pw_gid,
			 pw->pw_gid);
		assert(!strcmp(status_of(pid, "Gid"), want));
		line[0] = 0;
		for (k = 0; k < ng; k++)
			off += snprintf(line + off, sizeof(line) - off, "%u ", groups[k]);
		if (off)
			line[off - 1] = 0;
		assert(!strcmp(status_of(pid, "Groups"), line));
		assert(!strcmp(status_of(pid, "CapAmb"), "0000000000000000"));

		image_host_free(h);
		printf("test_image_host: root: uid, gid, groups and ambient capabilities "
		       "checked\n");
	}

	if (ambient)
		prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
	unsetenv("KMSCON_TEST_SECRET");
	unsetenv("TMPDIR");
	sigprocmask(SIG_SETMASK, &old, NULL);
	close(leak);
}

/* the reason a helper gives reaches the log as printable ASCII */
static void test_sanitize(void)
{
	char buf[KMSCON_IMAGE_REASON_MAX + 1];
	static const char reason[] = "bad\x01\x7f\n data!\x1b[2J\xff";

	sanitize_reason(buf, reason, sizeof(reason) - 1);
	assert(!strcmp(buf, "bad??? data!?[2J?"));
	sanitize_reason(buf, "", 0);
	assert(!buf[0]);
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
	test_sanitize();
	test_real(helper);
	test_checks(fake);
	test_hardening(helper);

	ev_eloop_unref(loop);
	assert(rmdir(dir) == 0);
	printf("test_image_host: ok\n");
	return 0;
}
