/*
 * kmscon-image-helper over its socket (ADR-103, The image helper, and the
 * Notes of 2026-10-24), with no libtsm and no kmscon: each test starts the
 * helper as kmscon does, its end of a SOCK_SEQPACKET pair on descriptor 3,
 * sends it requests and reads its replies.
 *
 * The helper is mechanical: it applies offset, read_max, min_bytes and the
 * flags, and answers an outcome. Every refusal to read is UNREADABLE with no
 * descriptor: /proc, /sys, /dev, a FIFO, a socket, a device, a directory, a
 * symlink loop, an offset at or past the end, t=t outside a temporary
 * directory or without the name, and shared memory names that are not one
 * leading '/' and at most 255 bytes. Refused temporary files survive; an
 * accepted t=t file and a t=s object are removed once opened, whatever comes
 * of the read. Each step's outcomes (TOO_SHORT, CORRUPT, TOO_LARGE,
 * OVER_BUDGET) are checked at their bounds, as are the requests it exits on.
 * A discard removes only what the temporary-file rule allows and answers OK.
 * PNG and zlib decode; a PNG header past the limits is refused before its
 * pixels are allocated.
 *
 * The swaps between a check and the step it guards run against the helper's
 * test build (IMAGE_HELPER_TEST), which stops at each such point and waits
 * on descriptor 4 while the test changes the file system.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <png.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#include "image_helper_proto.h"

#define MiB ((uint64_t)1 << 20)

static const char *real_helper;
static const char *test_helper;
static char tmp[PATH_MAX];   /* a directory under /tmp */
static char other[PATH_MAX]; /* a directory under none of the temporary ones */

struct helper {
	pid_t pid;
	int sock;
	int seam; /* the test build's stops, or -1 */
};

struct reply {
	struct kmscon_image_reply r;
	char reason[KMSCON_IMAGE_REASON_MAX + 1];
	int fd;
};

static void start_env(struct helper *h, const char *path, bool seam, const char *tmpdir)
{
	int sv[2], ss[2] = {-1, -1};
	char env[PATH_MAX + 8];
	char *envp[] = {env, NULL};

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	if (seam)
		assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ss) == 0);
	snprintf(env, sizeof(env), "TMPDIR=%s", tmpdir ? tmpdir : "");
	if (!tmpdir)
		envp[0] = NULL;

	h->pid = fork();
	assert(h->pid >= 0);
	if (!h->pid) {
		/* above 4 first, so the dup2 calls overwrite neither */
		int s = fcntl(sv[1], F_DUPFD, 10), t = seam ? fcntl(ss[1], F_DUPFD, 10) : -1;

		if (dup2(s, 3) < 0 || (seam && dup2(t, 4) < 0))
			_exit(126);
		execve(path, (char *[]){(char *)path, NULL}, envp);
		_exit(127);
	}
	close(sv[1]);
	h->sock = sv[0];
	h->seam = -1;
	if (seam) {
		close(ss[1]);
		h->seam = ss[0];
	}
}

static void start(struct helper *h)
{
	start_env(h, real_helper, false, NULL);
}

/* the helper's exit status after its socket closes */
static int stop(struct helper *h)
{
	int status;

	close(h->sock);
	if (h->seam >= 0)
		close(h->seam);
	assert(waitpid(h->pid, &status, 0) == h->pid);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* a request for @min_bytes under @flags, within roomy limits */
static struct kmscon_image_request req_of(unsigned int medium, unsigned int flags,
					  uint64_t min_bytes)
{
	static uint64_t id;

	return (struct kmscon_image_request){
		.version = KMSCON_IMAGE_VERSION,
		.type = KMSCON_IMAGE_TYPE_IMAGE,
		.id = ++id,
		.medium = medium,
		.flags = flags,
		.min_bytes = min_bytes,
		.max_width = 10000,
		.max_height = 10000,
		.max_bytes = 16 * MiB,
	};
}

static void send_req(struct helper *h, struct kmscon_image_request *r, const char *name, int fd)
{
	struct iovec iov[2] = {{.iov_base = r, .iov_len = sizeof(*r)}};
	char cbuf[CMSG_SPACE(sizeof(int))] = {0};
	struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
	struct cmsghdr *cmsg;

	if (name) {
		r->name_len = strlen(name);
		iov[1].iov_base = (void *)name;
		iov[1].iov_len = r->name_len;
	}
	if (fd >= 0) {
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	}
	assert(sendmsg(h->sock, &msg, MSG_NOSIGNAL) >= 0);
}

/* the reply, or 0 when the helper closed its socket instead */
static ssize_t recv_reply(struct helper *h, struct reply *out)
{
	union {
		struct kmscon_image_reply r;
		char buf[sizeof(struct kmscon_image_reply) + KMSCON_IMAGE_REASON_MAX + 1];
	} in;
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = {.iov_base = in.buf, .iov_len = sizeof(in.buf)};
	struct msghdr msg = {.msg_iov = &iov,
			     .msg_iovlen = 1,
			     .msg_control = cbuf,
			     .msg_controllen = sizeof(cbuf)};
	struct cmsghdr *cmsg;
	ssize_t n;

	n = recvmsg(h->sock, &msg, MSG_CMSG_CLOEXEC);
	assert(n >= 0);
	if (!n)
		return 0;
	assert(!(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
	assert((size_t)n == sizeof(in.r) + in.r.reason_len);
	assert(in.r.version == KMSCON_IMAGE_VERSION);
	out->r = in.r;
	memcpy(out->reason, in.buf + sizeof(in.r), in.r.reason_len);
	out->reason[in.r.reason_len] = 0;
	out->fd = -1;
	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg) {
		assert(cmsg->cmsg_type == SCM_RIGHTS && cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
		memcpy(&out->fd, CMSG_DATA(cmsg), sizeof(int));
	}
	return n;
}

static int ask(struct helper *h, struct kmscon_image_request r, const char *name, int fd,
	       struct reply *out)
{
	send_req(h, &r, name, fd);
	assert(recv_reply(h, out) > 0);
	assert(out->r.id == r.id);
	/* a descriptor with KMSCON_IMAGE_OK alone, and never for a discard */
	if (out->r.status == KMSCON_IMAGE_OK && !(r.flags & KMSCON_IMAGE_DISCARD))
		assert(out->fd >= 0);
	else
		assert(out->fd < 0 && !out->r.width && !out->r.height && !out->r.ignored);
	return out->r.status;
}

/* the reply's memfd holds exactly @len bytes, @want, sealed as kmscon requires */
static void expect_memfd(struct reply *rep, const uint8_t *want, size_t len)
{
	struct stat st;
	void *map;
	int seals;

	assert(fstat(rep->fd, &st) == 0 && (size_t)st.st_size == len);
	seals = fcntl(rep->fd, F_GET_SEALS);
	assert(seals >= 0 && (seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)) ==
				     (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE));
	map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, rep->fd, 0);
	assert(map != MAP_FAILED);
	assert(!memcmp(map, want, len));
	munmap(map, len);
	close(rep->fd);
}

/* a PNG reply: @w x @h pixels of RGBA, @want */
static void expect_pixels(struct reply *rep, unsigned int w, unsigned int h, const uint8_t *want)
{
	assert(rep->r.status == KMSCON_IMAGE_OK && rep->r.width == w && rep->r.height == h);
	expect_memfd(rep, want, (size_t)w * h * 4);
}

/* any other reply: exactly @len bytes, @want, with no size */
static void expect_bytes(struct reply *rep, const void *want, size_t len)
{
	assert(rep->r.status == KMSCON_IMAGE_OK && !rep->r.width && !rep->r.height);
	expect_memfd(rep, want, len);
}

/* four bytes from @path, as they are */
static int ask_file(struct helper *h, unsigned int medium, const char *path, struct reply *out)
{
	return ask(h, req_of(medium, 0, 4), path, -1, out);
}

/* a refusal: UNREADABLE, no descriptor */
static void refused(struct helper *h, unsigned int medium, const char *path)
{
	struct reply rep;

	if (ask_file(h, medium, path, &rep) != KMSCON_IMAGE_UNREADABLE) {
		fprintf(stderr, "%s: %u, not UNREADABLE (%s)\n", path, rep.r.status, rep.reason);
		abort();
	}
}

static const uint8_t px1[4] = {1, 2, 3, 4};

static void write_file(const char *path, const void *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	assert(fd >= 0 && write(fd, data, len) == (ssize_t)len);
	close(fd);
}

static bool exists(const char *path)
{
	struct stat st;

	return !lstat(path, &st);
}

static char *path_in(const char *dir, const char *name)
{
	static char bufs[8][PATH_MAX];
	static unsigned int n;
	char *p = bufs[n++ % 8];

	assert(snprintf(p, PATH_MAX, "%s/%s", dir, name) < PATH_MAX);
	return p;
}

/* a request for @min_bytes from @offset, at most @read_max */
static struct kmscon_image_request bounds(unsigned int medium, uint64_t offset, uint64_t read_max,
					  uint64_t min_bytes)
{
	struct kmscon_image_request r = req_of(medium, 0, min_bytes);

	r.offset = offset;
	r.read_max = read_max;
	return r;
}

static void test_files(void)
{
	static const uint8_t px[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	char *f = path_in(other, "img.rgba");
	struct kmscon_image_request r;
	uint8_t big[4096];
	struct helper h;
	struct reply rep;

	start(&h);
	write_file(f, px, sizeof(px));

	/*
	 * read_max 0 reads to the end; exactly min_bytes come back, and
	 * ignored counts the bytes within the bounds past them
	 */
	assert(ask(&h, bounds('f', 0, 0, 16), f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_bytes(&rep, px, 16);
	assert(ask(&h, bounds('f', 12, 0, 4), f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_bytes(&rep, px + 12, 4);
	/* read_max non-zero bounds the read */
	assert(ask(&h, bounds('f', 4, 4, 4), f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_bytes(&rep, px + 4, 4);
	/* min_bytes exceeded: the bytes past it are ignored, with read_max 0 or larger */
	assert(ask(&h, bounds('f', 4, 0, 4), f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 8);
	expect_bytes(&rep, px + 4, 4);
	assert(ask(&h, bounds('f', 4, 8, 4), f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 4);
	expect_bytes(&rep, px + 4, 4);
	/* read_max past the end reads to the end */
	assert(ask(&h, bounds('f', 8, 100, 8), f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_bytes(&rep, px + 8, 8);

	/* min_bytes missed: to the end, and within read_max */
	assert(ask(&h, bounds('f', 12, 0, 8), f, -1, &rep) == KMSCON_IMAGE_TOO_SHORT);
	assert(ask(&h, bounds('f', 4, 4, 8), f, -1, &rep) == KMSCON_IMAGE_TOO_SHORT);
	assert(ask(&h, bounds('f', 0, 100, 17), f, -1, &rep) == KMSCON_IMAGE_TOO_SHORT);
	/* an offset at the end, or past it */
	assert(ask(&h, bounds('f', 16, 0, 1), f, -1, &rep) == KMSCON_IMAGE_UNREADABLE);
	assert(ask(&h, bounds('f', 17, 4, 1), f, -1, &rep) == KMSCON_IMAGE_UNREADABLE);
	assert(exists(f));

	/*
	 * Without INFLATE or PNG only min_bytes are read, so a file larger than
	 * the budget is no matter; with either, what the bounds allow counts
	 * against it before it is read
	 */
	memset(big, 7, sizeof(big));
	write_file(f, big, sizeof(big));
	r = bounds('f', 0, 0, 4);
	r.max_bytes = 1024;
	assert(ask(&h, r, f, -1, &rep) == KMSCON_IMAGE_OK);
	/* counted from fstat, though the bytes were never read nor fit the budget */
	assert(rep.r.ignored == sizeof(big) - 4);
	expect_bytes(&rep, big, 4);
	r.flags = KMSCON_IMAGE_INFLATE;
	assert(ask(&h, r, f, -1, &rep) == KMSCON_IMAGE_OVER_BUDGET);
	r.flags = KMSCON_IMAGE_PNG;
	r.min_bytes = 0;
	assert(ask(&h, r, f, -1, &rep) == KMSCON_IMAGE_OVER_BUDGET);
	/* within read_max it fits */
	r.read_max = 1024;
	assert(ask(&h, r, f, -1, &rep) == KMSCON_IMAGE_CORRUPT);

	/* a missing file, and a NUL in the name */
	refused(&h, 'f', path_in(other, "missing"));
	{
		struct kmscon_image_request n = req_of('f', 0, 4);
		struct iovec iov[2] = {{.iov_base = &n, .iov_len = sizeof(n)},
				       {.iov_base = "/tmp/a\0b", .iov_len = 8}};
		struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};

		n.name_len = 8;
		assert(sendmsg(h.sock, &msg, 0) == sizeof(n) + 8);
		assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_UNREADABLE &&
		       rep.fd < 0);
	}
	unlink(f);
	assert(stop(&h) == 0);
}

/* a child waiting in open() to write to FIFO @path */
static pid_t start_writer(const char *path)
{
	pid_t pid = fork();

	assert(pid >= 0);
	if (!pid) {
		int w = open(path, O_WRONLY);

		_exit(w >= 0 ? 0 : 1);
	}
	usleep(20000);
	return pid;
}

/* the writer is still waiting, so nothing opened the FIFO; then lets it go */
static void still_waiting(pid_t writer, const char *path)
{
	int fd;

	usleep(50000);
	assert(waitpid(writer, NULL, WNOHANG) == 0);
	fd = open(path, O_RDONLY | O_NONBLOCK);
	assert(fd >= 0 && waitpid(writer, NULL, 0) == writer);
	close(fd);
}

/* the first regular file of @paths, or NULL */
static const char *first_regular(const char *const *paths)
{
	struct stat st;

	for (; *paths; paths++)
		if (!stat(*paths, &st) && S_ISREG(st.st_mode))
			return *paths;
	return NULL;
}

static void test_refusals(void)
{
	static const char *const sys[] = {"/sys/devices/system/cpu/online",
					  "/sys/kernel/mm/transparent_hugepage/enabled",
					  "/sys/power/state", NULL};
	struct sockaddr_un sun = {.sun_family = AF_UNIX};
	char *fifo = path_in(other, "fifo"), *sock = path_in(other, "sock");
	char *loop1 = path_in(other, "loop1"), *loop2 = path_in(other, "loop2");
	char *dev = path_in(other, "devlink");
	char *proc = path_in(other, "proclink");
	const char *s;
	struct helper h;
	pid_t writer;
	int fd;

	start(&h);
	/* /proc and /sys hold regular files: refused by where they are */
	refused(&h, 'f', "/proc/self/status");
	refused(&h, 'f', "/proc/self/environ");
	s = first_regular(sys);
	if (s)
		refused(&h, 'f', s);
	else
		printf("test_image_helper: no regular file in /sys to refuse\n");
	/* a device, and /dev other than /dev/shm, also through a symlink */
	refused(&h, 'f', "/dev/null");
	refused(&h, 'f', "/dev/shm/../null");
	assert(symlink("/dev/null", dev) == 0);
	refused(&h, 'f', dev);
	assert(symlink("/proc/self/status", proc) == 0);
	refused(&h, 'f', proc);
	/*
	 * a FIFO, refused before it is opened: a writer waiting in open() is
	 * still waiting after the reply, as opening it would have let it go on
	 */
	assert(mkfifo(fifo, 0600) == 0);
	writer = start_writer(fifo);
	refused(&h, 'f', fifo);
	still_waiting(writer, fifo);
	/* a socket and a directory */
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0 && bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == 0);
	refused(&h, 'f', sock);
	refused(&h, 'f', other);
	/* a symlink loop */
	assert(symlink(loop2, loop1) == 0 && symlink(loop1, loop2) == 0);
	refused(&h, 'f', loop1);
	/* /dev/shm is the exception in /dev */
	{
		struct reply rep;
		char shm[64];

		snprintf(shm, sizeof(shm), "/dev/shm/kmscon-f-%d", getpid());
		write_file(shm, px1, 4);
		assert(ask_file(&h, 'f', shm, &rep) == KMSCON_IMAGE_OK);
		expect_bytes(&rep, px1, 4);
		assert(exists(shm));
		unlink(shm);
	}
	assert(stop(&h) == 0);

	close(fd);
	unlink(sock);
	unlink(fifo);
	unlink(loop1);
	unlink(loop2);
	unlink(dev);
	unlink(proc);
}

static void test_temp(void)
{
	char *ok = path_in(tmp, "tty-graphics-protocol-1.rgba");
	char *noname = path_in(tmp, "img.rgba");
	char *outside = path_in(other, "tty-graphics-protocol-2.rgba");
	char tmpdir[PATH_MAX], *intmp;
	struct helper h;
	struct reply rep;

	start(&h);
	/* accepted, read, then deleted */
	write_file(ok, px1, 4);
	assert(ask_file(&h, 't', ok, &rep) == KMSCON_IMAGE_OK);
	expect_bytes(&rep, px1, 4);
	assert(!exists(ok));
	/* refused, and left in place: without the name, outside the temporary directories */
	write_file(noname, px1, 4);
	refused(&h, 't', noname);
	assert(exists(noname));
	write_file(outside, px1, 4);
	refused(&h, 't', outside);
	assert(exists(outside));
	/* deleted once opened, whatever the read: too short, and an offset at the end */
	write_file(ok, px1, 4);
	assert(ask(&h, req_of('t', 0, 8), ok, -1, &rep) == KMSCON_IMAGE_TOO_SHORT);
	assert(!exists(ok));
	write_file(ok, px1, 4);
	assert(ask(&h, bounds('t', 4, 0, 4), ok, -1, &rep) == KMSCON_IMAGE_UNREADABLE);
	assert(!exists(ok));
	write_file(ok, px1, 4);
	assert(ask(&h, req_of('t', KMSCON_IMAGE_INFLATE, 4), ok, -1, &rep) == KMSCON_IMAGE_CORRUPT);
	assert(!exists(ok));
	/* a t=f never deletes, whatever its name */
	write_file(ok, px1, 4);
	assert(ask_file(&h, 'f', ok, &rep) == KMSCON_IMAGE_OK);
	close(rep.fd);
	assert(exists(ok));
	assert(stop(&h) == 0);

	/* kmscon's TMPDIR counts as a temporary directory */
	assert(snprintf(tmpdir, sizeof(tmpdir), "%s/tmpdir", other) < (int)sizeof(tmpdir));
	assert(mkdir(tmpdir, 0700) == 0);
	intmp = path_in(tmpdir, "tty-graphics-protocol-3.rgba");
	write_file(intmp, px1, 4);
	start_env(&h, real_helper, false, tmpdir);
	assert(ask_file(&h, 't', intmp, &rep) == KMSCON_IMAGE_OK);
	close(rep.fd);
	assert(!exists(intmp));
	refused(&h, 't', outside);
	assert(stop(&h) == 0);

	unlink(ok);
	unlink(noname);
	unlink(outside);
	rmdir(tmpdir);
}

/* the test build stopped at @point; the reply to the seam lets it go on */
static void at_seam(struct helper *h, const char *point)
{
	char buf[32] = {0};

	assert(read(h->seam, buf, sizeof(buf) - 1) > 0);
	if (strcmp(buf, point)) {
		fprintf(stderr, "stopped at %s, not %s\n", buf, point);
		abort();
	}
}

static void go_on(struct helper *h)
{
	assert(write(h->seam, "", 1) == 1);
}

static void test_swaps(void)
{
	char f[PATH_MAX], link[PATH_MAX];
	char dir[PATH_MAX], moved[PATH_MAX], elsewhere[PATH_MAX];
	struct kmscon_image_request r;
	struct helper h;
	struct reply rep;
	pid_t writer;

	/* copies: path_in's buffers are reused */
	snprintf(f, sizeof(f), "%s", path_in(other, "swap.rgba"));
	snprintf(link, sizeof(link), "%s", path_in(other, "swap.link"));
	start_env(&h, test_helper, true, NULL);

	/* a symlink swapped in between the check and the open */
	write_file(f, px1, 4);
	assert(symlink("/proc/self/status", link) == 0);
	r = req_of('f', 0, 4);
	send_req(&h, &r, f, -1);
	at_seam(&h, "open");
	assert(rename(link, f) == 0);
	go_on(&h);
	assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_UNREADABLE && rep.fd < 0);

	/* a FIFO swapped in: refused, and never opened */
	unlink(f);
	write_file(f, px1, 4);
	r = req_of('f', 0, 4);
	send_req(&h, &r, f, -1);
	at_seam(&h, "open");
	assert(unlink(f) == 0 && mkfifo(f, 0600) == 0);
	writer = start_writer(f);
	go_on(&h);
	assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_UNREADABLE && rep.fd < 0);
	still_waiting(writer, f);
	unlink(f);

	/* a symlink to a FIFO swapped in: the FIFO is never opened either */
	write_file(f, px1, 4);
	assert(mkfifo(path_in(other, "swap.fifo"), 0600) == 0);
	r = req_of('f', 0, 4);
	send_req(&h, &r, f, -1);
	at_seam(&h, "open");
	assert(unlink(f) == 0 && symlink(path_in(other, "swap.fifo"), f) == 0);
	writer = start_writer(path_in(other, "swap.fifo"));
	go_on(&h);
	assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_UNREADABLE && rep.fd < 0);
	still_waiting(writer, path_in(other, "swap.fifo"));
	unlink(f);
	unlink(path_in(other, "swap.fifo"));

	/*
	 * a symlink to another readable regular file, in an allowed place:
	 * the resolved path admits no symlink, so it is refused
	 */
	write_file(f, px1, 4);
	write_file(path_in(other, "swap.other"), px1, 4);
	r = req_of('f', 0, 4);
	send_req(&h, &r, f, -1);
	at_seam(&h, "open");
	assert(unlink(f) == 0 && symlink(path_in(other, "swap.other"), f) == 0);
	go_on(&h);
	assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_UNREADABLE && rep.fd < 0);
	unlink(f);
	unlink(path_in(other, "swap.other"));

	/* a directory component swapped for a symlink before the open: refused too */
	{
		char d[PATH_MAX], real[PATH_MAX], alt[PATH_MAX], name[PATH_MAX];

		snprintf(d, sizeof(d), "%s", path_in(other, "dswap"));
		snprintf(real, sizeof(real), "%s", path_in(other, "dswap.real"));
		snprintf(alt, sizeof(alt), "%s", path_in(other, "dswap.alt"));
		assert(mkdir(d, 0700) == 0 && mkdir(alt, 0700) == 0);
		write_file(path_in(d, "img"), px1, 4);
		write_file(path_in(alt, "img"), px1, 4);
		snprintf(name, sizeof(name), "%s", path_in(d, "img"));
		r = req_of('f', 0, 4);
		send_req(&h, &r, name, -1);
		at_seam(&h, "open");
		assert(rename(d, real) == 0 && symlink(alt, d) == 0);
		go_on(&h);
		assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_UNREADABLE &&
		       rep.fd < 0);
		unlink(d);
		unlink(path_in(real, "img"));
		unlink(path_in(alt, "img"));
		rmdir(real);
		rmdir(alt);
	}

	/*
	 * t=t: a directory component swapped for a symlink after the check
	 * deletes nothing, neither the file read nor the one the symlink
	 * reaches
	 */
	assert(snprintf(dir, sizeof(dir), "%s/d", tmp) < (int)sizeof(dir));
	assert(snprintf(moved, sizeof(moved), "%s/d.moved", tmp) < (int)sizeof(moved));
	assert(snprintf(elsewhere, sizeof(elsewhere), "%s/elsewhere", tmp) <
	       (int)sizeof(elsewhere));
	assert(mkdir(dir, 0700) == 0 && mkdir(elsewhere, 0700) == 0);
	write_file(path_in(dir, "tty-graphics-protocol-s"), px1, 4);
	write_file(path_in(elsewhere, "tty-graphics-protocol-s"), px1, 4);
	r = req_of('t', 0, 4);
	send_req(&h, &r, path_in(dir, "tty-graphics-protocol-s"), -1);
	at_seam(&h, "open");
	go_on(&h);
	at_seam(&h, "unlink");
	assert(rename(dir, moved) == 0 && symlink(elsewhere, dir) == 0);
	go_on(&h);
	assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_OK);
	close(rep.fd);
	assert(exists(path_in(moved, "tty-graphics-protocol-s")));
	assert(exists(path_in(elsewhere, "tty-graphics-protocol-s")));

	/*
	 * the same with a symlink to the directory the file is in: the entry
	 * is the file read, but a parent reached through a symlink deletes
	 * nothing
	 */
	unlink(dir);
	assert(symlink(moved, dir) == 0);
	r = req_of('t', 0, 4);
	send_req(&h, &r, path_in(moved, "tty-graphics-protocol-s"), -1);
	at_seam(&h, "open");
	go_on(&h);
	at_seam(&h, "unlink");
	assert(rename(moved, path_in(tmp, "d.real")) == 0 && unlink(dir) == 0 &&
	       symlink(path_in(tmp, "d.real"), moved) == 0);
	go_on(&h);
	assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_OK);
	close(rep.fd);
	assert(exists(path_in(path_in(tmp, "d.real"), "tty-graphics-protocol-s")));
	unlink(moved);
	assert(rename(path_in(tmp, "d.real"), moved) == 0);

	/* the file itself replaced after it was opened: the new one is not deleted */
	assert(rename(moved, dir) == 0);
	r = req_of('t', 0, 4);
	send_req(&h, &r, path_in(dir, "tty-graphics-protocol-s"), -1);
	at_seam(&h, "open");
	go_on(&h);
	at_seam(&h, "unlink");
	assert(unlink(path_in(dir, "tty-graphics-protocol-s")) == 0);
	write_file(path_in(dir, "tty-graphics-protocol-s"), px1, 4);
	go_on(&h);
	assert(recv_reply(&h, &rep) > 0 && rep.r.status == KMSCON_IMAGE_OK);
	close(rep.fd);
	assert(exists(path_in(dir, "tty-graphics-protocol-s")));

	assert(stop(&h) == 0);
	unlink(path_in(dir, "tty-graphics-protocol-s"));
	unlink(path_in(elsewhere, "tty-graphics-protocol-s"));
	rmdir(dir);
	rmdir(elsewhere);
}

static void shm_write(const char *name, const void *data, size_t len)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/dev/shm/%s", name + (name[0] == '/'));
	write_file(path, data, len);
}

static bool shm_exists(const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/dev/shm/%s", name + (name[0] == '/'));
	return exists(path);
}

static void test_shm(void)
{
	char name[300], longest[300];
	struct helper h;
	struct reply rep;
	pid_t writer;

	snprintf(name, sizeof(name), "/kmscon-test-%d", getpid());
	start(&h);

	/* read, then unlinked */
	shm_write(name, px1, 4);
	assert(ask_file(&h, 's', name, &rep) == KMSCON_IMAGE_OK);
	expect_bytes(&rep, px1, 4);
	assert(!shm_exists(name));

	/* unlinked whatever the read: an offset at the end, too short */
	shm_write(name, px1, 4);
	assert(ask(&h, bounds('s', 4, 4, 4), name, -1, &rep) == KMSCON_IMAGE_UNREADABLE);
	assert(!shm_exists(name));
	shm_write(name, px1, 4);
	assert(ask(&h, req_of('s', 0, 5), name, -1, &rep) == KMSCON_IMAGE_TOO_SHORT);
	assert(!shm_exists(name));

	/*
	 * a name without its leading '/' is refused, and its object stays:
	 * both the object it names and the one its tail names
	 */
	shm_write(name, px1, 4);
	refused(&h, 's', name + 1);
	name[0] = '_';
	refused(&h, 's', name);
	name[0] = '/';
	assert(shm_exists(name));
	unlink(path_in("/dev/shm", name + 1));
	/* other slashes, '/' alone, and the directory's own entries */
	refused(&h, 's', "/a/b");
	refused(&h, 's', "//kmscon");
	refused(&h, 's', "/");
	refused(&h, 's', "/..");
	/* 255 bytes, the slash included, is the longest */
	longest[0] = '/';
	memset(longest + 1, 'k', 254);
	longest[255] = 0;
	shm_write(longest, px1, 4);
	assert(ask_file(&h, 's', longest, &rep) == KMSCON_IMAGE_OK);
	close(rep.fd);
	assert(!shm_exists(longest));
	/* 256 bytes is refused, though an object of that name, less its '/', exists */
	longest[255] = 'k';
	longest[256] = 0;
	shm_write(longest, px1, 4);
	refused(&h, 's', longest);
	assert(shm_exists(longest));
	unlink(path_in("/dev/shm", longest + 1));
	/* not a regular object: refused, never opened, and left */
	assert(mkfifo(path_in("/dev/shm", name + 1), 0600) == 0);
	writer = start_writer(path_in("/dev/shm", name + 1));
	refused(&h, 's', name);
	assert(shm_exists(name));
	still_waiting(writer, path_in("/dev/shm", name + 1));
	unlink(path_in("/dev/shm", name + 1));

	assert(stop(&h) == 0);
}

/* a discard answers OK with no descriptor, whatever it removed */
static void ask_discard(struct helper *h, unsigned int medium, const char *name)
{
	struct reply rep;

	assert(ask(h, req_of(medium, KMSCON_IMAGE_DISCARD, 0), name, -1, &rep) == KMSCON_IMAGE_OK);
}

static void test_discard(void)
{
	char *file = path_in(tmp, "tty-graphics-protocol-d");
	char *link = path_in(tmp, "tty-graphics-protocol-link");
	char *fifo = path_in(tmp, "tty-graphics-protocol-fifo");
	char *bad = path_in(other, "tty-graphics-protocol-bad");
	char name[64];
	struct helper h;
	struct reply rep;

	start(&h);
	/* a temporary file is removed, and nothing is read */
	write_file(file, px1, 4);
	ask_discard(&h, 't', file);
	assert(!exists(file));
	/* a symlink to a temporary file: the file goes, the symlink stays */
	write_file(file, px1, 4);
	assert(symlink(file, link) == 0);
	ask_discard(&h, 't', link);
	assert(!exists(file) && exists(link));
	unlink(link);
	/* a symlink whose target fails the temporary-file rule: nothing goes */
	write_file(bad, px1, 4);
	assert(symlink(bad, link) == 0);
	ask_discard(&h, 't', link);
	assert(exists(bad) && exists(link));
	unlink(link);
	/* the same file named directly */
	ask_discard(&h, 't', bad);
	assert(exists(bad));
	/* a FIFO is refused and stays */
	assert(mkfifo(fifo, 0600) == 0);
	ask_discard(&h, 't', fifo);
	assert(exists(fifo));
	/* shared memory is unlinked */
	snprintf(name, sizeof(name), "/kmscon-discard-%d", getpid());
	shm_write(name, px1, 4);
	ask_discard(&h, 's', name);
	assert(!shm_exists(name));
	/* a refused name unlinks nothing */
	shm_write(name, px1, 4);
	ask_discard(&h, 's', name + 1);
	assert(shm_exists(name));
	unlink(path_in("/dev/shm", name + 1));
	/* the helper still runs after its discards */
	write_file(file, px1, 4);
	assert(ask_file(&h, 'f', file, &rep) == KMSCON_IMAGE_OK);
	expect_bytes(&rep, px1, 4);
	unlink(file);
	assert(stop(&h) == 0);

	unlink(fifo);
	unlink(bad);
}

/* a PNG of @w x @h in @type and @depth, from the 8-bit RGBA at @rgba */
static size_t make_png(uint8_t **out, unsigned int w, unsigned int h, int type, int depth,
		       int interlace, const uint8_t *rgba)
{
	png_structp png;
	png_infop info;
	FILE *f;
	char *buf = NULL;
	size_t len = 0;
	unsigned int x, y, c, ch, bytes;
	png_bytep row;

	f = open_memstream(&buf, &len);
	assert(f);
	png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png_create_info_struct(png);
	assert(png && info && !setjmp(png_jmpbuf(png)));
	png_init_io(png, f);
	png_set_IHDR(png, info, w, h, depth, type, interlace, PNG_COMPRESSION_TYPE_DEFAULT,
		     PNG_FILTER_TYPE_DEFAULT);
	if (type == PNG_COLOR_TYPE_PALETTE) {
		png_color pal[4];

		for (c = 0; c < 4; c++)
			pal[c] = (png_color){(png_byte)(c * 80), (png_byte)(c * 40),
					     (png_byte)(255 - c * 60)};
		png_set_PLTE(png, info, pal, 4);
	}
	png_write_info(png, info);
	ch = type == PNG_COLOR_TYPE_RGBA ? 4 : type == PNG_COLOR_TYPE_RGB ? 3 : 1;
	bytes = depth == 16 ? 2 : 1;
	row = malloc((size_t)w * ch * bytes + 1);
	assert(row);
	png_set_interlace_handling(png);
	for (int pass = 0; pass < (interlace ? 7 : 1); pass++) {
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				const uint8_t *p = rgba + ((size_t)y * w + x) * 4;

				for (c = 0; c < ch; c++) {
					uint8_t v = type == PNG_COLOR_TYPE_PALETTE ? p[0] : p[c];

					if (bytes == 2) {
						row[(x * ch + c) * 2] = v;
						row[(x * ch + c) * 2 + 1] = v;
					} else {
						row[x * ch + c] = v;
					}
				}
			}
			png_write_row(png, row);
		}
	}
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	free(row);
	fclose(f);
	*out = (uint8_t *)buf;
	return len;
}

static int memfd_of(const void *data, size_t len)
{
	int fd = memfd_create("test", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	assert(fd >= 0 && write(fd, data, len) == (ssize_t)len);
	assert(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) ==
	       0);
	return fd;
}

static int ask_inline(struct helper *h, struct kmscon_image_request r, const void *data, size_t len,
		      struct reply *rep)
{
	int fd = memfd_of(data, len), ret;

	ret = ask(h, r, NULL, fd, rep);
	close(fd);
	return ret;
}

/* @len bytes compressed with zlib into a new buffer */
static uint8_t *zip(const void *data, size_t len, uLongf *zlen)
{
	uint8_t *z;

	*zlen = compressBound(len);
	z = malloc(*zlen);
	assert(z && compress(z, zlen, data, len) == Z_OK);
	return z;
}

static void test_png_zlib(void)
{
	uint8_t rgba[3 * 2 * 4], want[3 * 2 * 4], *png, *z, *zeros;
	const unsigned int pz = KMSCON_IMAGE_PNG | KMSCON_IMAGE_INFLATE;
	struct kmscon_image_request r;
	struct helper h;
	struct reply rep;
	size_t len, i;
	uLongf zlen;
	char *f = path_in(other, "img.png");

	for (i = 0; i < sizeof(rgba); i++)
		rgba[i] = (uint8_t)(i * 13 + 7);
	start(&h);

	/* RGBA, inline and from a file */
	len = make_png(&png, 3, 2, PNG_COLOR_TYPE_RGBA, 8, PNG_INTERLACE_NONE, rgba);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), png, len, &rep) == KMSCON_IMAGE_OK);
	expect_pixels(&rep, 3, 2, rgba);
	write_file(f, png, len);
	assert(ask(&h, req_of('f', KMSCON_IMAGE_PNG, 0), f, -1, &rep) == KMSCON_IMAGE_OK);
	expect_pixels(&rep, 3, 2, rgba);

	/*
	 * zlib over PNG inline under EXACT: the inflated length must be min_bytes,
	 * fewer TOO_SHORT and one byte past it CORRUPT; without EXACT, no length
	 */
	z = zip(png, len, &zlen);
	r = req_of('d', pz | KMSCON_IMAGE_EXACT, len);
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_OK);
	expect_pixels(&rep, 3, 2, rgba);
	r.min_bytes = len + 1;
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_TOO_SHORT);
	r.min_bytes = len - 1;
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_CORRUPT);
	assert(ask_inline(&h, req_of('d', pz, 0), z, zlen, &rep) == KMSCON_IMAGE_OK);
	close(rep.fd);
	/* from a file, read_max bounds what is read */
	write_file(f, z, zlen);
	r = req_of('f', pz, 0);
	r.read_max = zlen;
	assert(ask(&h, r, f, -1, &rep) == KMSCON_IMAGE_OK);
	expect_pixels(&rep, 3, 2, rgba);
	r.read_max = zlen - 1;
	assert(ask(&h, r, f, -1, &rep) == KMSCON_IMAGE_CORRUPT);
	free(z);
	free(png);

	/* interlaced and 16-bit RGB: alpha 255, the high bytes kept */
	len = make_png(&png, 3, 2, PNG_COLOR_TYPE_RGB, 16, PNG_INTERLACE_ADAM7, rgba);
	for (i = 0; i < 6; i++) {
		memcpy(want + i * 4, rgba + i * 4, 3);
		want[i * 4 + 3] = 255;
	}
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), png, len, &rep) == KMSCON_IMAGE_OK);
	expect_pixels(&rep, 3, 2, want);
	free(png);
	/* grey */
	len = make_png(&png, 3, 2, PNG_COLOR_TYPE_GRAY, 8, PNG_INTERLACE_NONE, rgba);
	for (i = 0; i < 6; i++) {
		memset(want + i * 4, rgba[i * 4], 3);
		want[i * 4 + 3] = 255;
	}
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), png, len, &rep) == KMSCON_IMAGE_OK);
	expect_pixels(&rep, 3, 2, want);
	free(png);
	/* a palette */
	for (i = 0; i < 6; i++)
		rgba[i * 4] = i % 4;
	len = make_png(&png, 3, 2, PNG_COLOR_TYPE_PALETTE, 8, PNG_INTERLACE_NONE, rgba);
	for (i = 0; i < 6; i++) {
		want[i * 4] = (i % 4) * 80;
		want[i * 4 + 1] = (i % 4) * 40;
		want[i * 4 + 2] = 255 - (i % 4) * 60;
		want[i * 4 + 3] = 255;
	}
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), png, len, &rep) == KMSCON_IMAGE_OK);
	expect_pixels(&rep, 3, 2, want);

	/* corrupt: truncated, and not a PNG */
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), png, len - 20, &rep) ==
	       KMSCON_IMAGE_CORRUPT);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), "not a png", 9, &rep) ==
	       KMSCON_IMAGE_CORRUPT);
	free(png);

	/*
	 * INFLATE without PNG: min_bytes met; exceeded, the rest ignored, even
	 * where it does not decode; missed, TOO_SHORT; a stream that does not
	 * inflate, or ends early, CORRUPT
	 */
	z = zip(rgba, sizeof(rgba), &zlen);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 24), z, zlen, &rep) ==
	       KMSCON_IMAGE_OK);
	expect_bytes(&rep, rgba, 24);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 16), z, zlen, &rep) ==
	       KMSCON_IMAGE_OK);
	expect_bytes(&rep, rgba, 16);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 16), z, zlen - 4, &rep) ==
	       KMSCON_IMAGE_OK);
	expect_bytes(&rep, rgba, 16);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 25), z, zlen, &rep) ==
	       KMSCON_IMAGE_TOO_SHORT);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 16), "\x78\x9c garbage", 10,
			  &rep) == KMSCON_IMAGE_CORRUPT);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 24), z, 4, &rep) ==
	       KMSCON_IMAGE_CORRUPT);
	/* INFLATE and EXACT without PNG: met, short, and one byte long */
	r = req_of('d', KMSCON_IMAGE_INFLATE | KMSCON_IMAGE_EXACT, 24);
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_OK);
	expect_bytes(&rep, rgba, 24);
	r.min_bytes = 25;
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_TOO_SHORT);
	r.min_bytes = 23;
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_CORRUPT);
	/* from a file, as raw data compressed */
	write_file(f, z, zlen);
	assert(ask(&h, req_of('f', KMSCON_IMAGE_INFLATE, 24), f, -1, &rep) == KMSCON_IMAGE_OK);
	expect_bytes(&rep, rgba, 24);
	free(z);

	/*
	 * Past the budget: inflating to more than max_bytes is OVER_BUDGET, but
	 * under EXACT the byte past min_bytes is CORRUPT first, even at
	 * max_bytes
	 */
	zeros = calloc(1, 2 * MiB);
	assert(zeros);
	z = zip(zeros, 2 * MiB, &zlen);
	r = req_of('d', pz, 0);
	r.max_bytes = MiB;
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_OVER_BUDGET);
	r = req_of('d', pz | KMSCON_IMAGE_EXACT, MiB);
	r.max_bytes = MiB;
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_CORRUPT);
	/* without PNG or EXACT, it stops at min_bytes, within any budget */
	r = req_of('d', KMSCON_IMAGE_INFLATE, MiB);
	r.max_bytes = MiB;
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_OK);
	expect_bytes(&rep, zeros, MiB);
	/* inline data past the budget */
	r = req_of('d', KMSCON_IMAGE_PNG, 0);
	r.max_bytes = 1024;
	assert(ask_inline(&h, r, zeros, 2048, &rep) == KMSCON_IMAGE_OVER_BUDGET);
	free(z);
	free(zeros);

	unlink(f);
	assert(stop(&h) == 0);
}

/* @len bytes of @data, then @extra bytes of junk, in a new buffer */
static uint8_t *with_junk(const void *data, size_t len, size_t extra)
{
	uint8_t *buf = malloc(len + extra);

	assert(buf);
	memcpy(buf, data, len);
	memset(buf + len, 0xa5, extra);
	return buf;
}

/*
 * Step 7: ignored, in bytes read from the source. A well-formed PNG or zlib
 * payload read whole reports 0, inline, from a file and under every flag;
 * bytes past IEND with PNG alone, or those the inflater did not consume with
 * INFLATE, are counted: past the zlib stream's end, or everything past the
 * last byte fed when it stopped at min_bytes. Decoded bytes never count:
 * neither bytes inflated past min_bytes nor inflated bytes past IEND. A PNG that
 * ends without IEND after its image still decodes, with 0.
 */
static void test_ignored(void)
{
	const unsigned int pz = KMSCON_IMAGE_PNG | KMSCON_IMAGE_INFLATE;
	uint8_t rgba[3 * 2 * 4], *png, *z, *junk;
	char *f = path_in(other, "ignored.png");
	struct kmscon_image_request r;
	struct helper h;
	struct reply rep;
	uLongf zlen;
	size_t len, i;

	for (i = 0; i < sizeof(rgba); i++)
		rgba[i] = (uint8_t)(i * 7 + 3);
	start(&h);

	/* PNG: well-formed is 0, inline and from a file; 5 bytes past IEND are 5 */
	len = make_png(&png, 3, 2, PNG_COLOR_TYPE_RGBA, 8, PNG_INTERLACE_NONE, rgba);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), png, len, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_pixels(&rep, 3, 2, rgba);
	write_file(f, png, len);
	assert(ask(&h, req_of('f', KMSCON_IMAGE_PNG, 0), f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_pixels(&rep, 3, 2, rgba);
	junk = with_junk(png, len, 5);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), junk, len + 5, &rep) ==
	       KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 5);
	expect_pixels(&rep, 3, 2, rgba);
	free(junk);
	/* no IEND after the image (its last 12 bytes): decoded, 0 */
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), png, len - 12, &rep) ==
	       KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_pixels(&rep, 3, 2, rgba);

	/* zlib over PNG: well-formed is 0 with and without EXACT; 3 bytes past the stream are 3 */
	z = zip(png, len, &zlen);
	r = req_of('d', pz | KMSCON_IMAGE_EXACT, len);
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_pixels(&rep, 3, 2, rgba);
	assert(ask_inline(&h, req_of('d', pz, 0), z, zlen, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_pixels(&rep, 3, 2, rgba);
	junk = with_junk(z, zlen, 3);
	assert(ask_inline(&h, req_of('d', pz, 0), junk, zlen + 3, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 3);
	expect_pixels(&rep, 3, 2, rgba);
	free(junk);
	free(z);
	/*
	 * junk past IEND inside the zlib stream is decoded bytes: not counted,
	 * so a file's count stays within read_max
	 */
	junk = with_junk(png, len, 4096);
	z = zip(junk, len + 4096, &zlen);
	write_file(f, z, zlen);
	r = req_of('f', pz, 0);
	r.read_max = zlen;
	assert(ask(&h, r, f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_pixels(&rep, 3, 2, rgba);
	free(junk);
	free(z);
	free(png);

	/*
	 * Stopped at min_bytes before the stream ends: what was read past the
	 * last byte fed counts, never the bytes inflated past min_bytes. A
	 * stored (level 0) stream of 24 bytes is a 2-byte header, a 5-byte block
	 * header, the 24 bytes and a 4-byte trailer: stopping at 16 leaves 8 of
	 * them and the trailer, 12, unconsumed
	 */
	zlen = compressBound(sizeof(rgba));
	z = malloc(zlen);
	assert(z && compress2(z, &zlen, rgba, sizeof(rgba), 0) == Z_OK && zlen == 35);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 16), z, zlen, &rep) ==
	       KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 12);
	expect_bytes(&rep, rgba, 16);
	free(z);

	/*
	 * INFLATE alone: well-formed is 0, exactly min_bytes long or under EXACT;
	 * stopped early, it is the input left unfed; 3 bytes past the end of a
	 * stream exactly min_bytes long are 3; from a file within read_max too
	 */
	z = zip(rgba, sizeof(rgba), &zlen);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 24), z, zlen, &rep) ==
	       KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_bytes(&rep, rgba, 24);
	r = req_of('d', KMSCON_IMAGE_INFLATE | KMSCON_IMAGE_EXACT, 24);
	assert(ask_inline(&h, r, z, zlen, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 0);
	expect_bytes(&rep, rgba, 24);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 16), z, zlen, &rep) ==
	       KMSCON_IMAGE_OK);
	assert(rep.r.ignored > 0 && rep.r.ignored < zlen);
	expect_bytes(&rep, rgba, 16);
	junk = with_junk(z, zlen, 3);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 24), junk, zlen + 3, &rep) ==
	       KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 3);
	expect_bytes(&rep, rgba, 24);
	assert(ask_inline(&h, r, junk, zlen + 3, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 3);
	expect_bytes(&rep, rgba, 24);
	write_file(f, junk, zlen + 3);
	r = req_of('f', KMSCON_IMAGE_INFLATE, 24);
	r.read_max = zlen + 1;
	assert(ask(&h, r, f, -1, &rep) == KMSCON_IMAGE_OK);
	assert(rep.r.ignored == 1);
	expect_bytes(&rep, rgba, 24);
	free(junk);
	free(z);

	/* an error reports 0 (ask checks it) */
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_INFLATE, 25), rgba, 4, &rep) ==
	       KMSCON_IMAGE_CORRUPT);

	unlink(f);
	assert(stop(&h) == 0);
}

/* a PNG signature, an IHDR claiming @w x @h, and two bytes of image data: nothing to decode */
static size_t png_header(uint8_t *buf, uint32_t w, uint32_t h)
{
	static const uint8_t idat[10] = {0, 0, 0, 2, 'I', 'D', 'A', 'T', 0x78, 0x9c};
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	uint8_t ihdr[17] = {'I', 'H', 'D', 'R'};
	uint32_t crc;

	memcpy(buf, sig, 8);
	buf[8] = 0, buf[9] = 0, buf[10] = 0, buf[11] = 13;
	ihdr[4] = w >> 24, ihdr[5] = w >> 16, ihdr[6] = w >> 8, ihdr[7] = w;
	ihdr[8] = h >> 24, ihdr[9] = h >> 16, ihdr[10] = h >> 8, ihdr[11] = h;
	ihdr[12] = 8; /* depth */
	ihdr[13] = 6; /* RGBA */
	memcpy(buf + 12, ihdr, 17);
	crc = crc32(0, ihdr, 17);
	buf[29] = crc >> 24, buf[30] = crc >> 16, buf[31] = crc >> 8, buf[32] = crc;
	/* the first IDAT's start, where libpng's header reading stops */
	memcpy(buf + 33, idat, sizeof(idat));
	return 33 + sizeof(idat);
}

static void test_png_limits(void)
{
	struct kmscon_image_request r;
	struct helper h;
	struct reply rep;
	uint8_t hdr[64];
	size_t len;

	start(&h);
	/*
	 * Past max-graphic-size: TOO_LARGE. Within it but past the budget:
	 * OVER_BUDGET, though the stream ends two bytes into its image data, so
	 * the answer comes from the header, before any pixel is allocated or
	 * decoded.
	 */
	len = png_header(hdr, 20000, 10);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), hdr, len, &rep) ==
	       KMSCON_IMAGE_TOO_LARGE);
	len = png_header(hdr, 10, 20000);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), hdr, len, &rep) ==
	       KMSCON_IMAGE_TOO_LARGE);
	len = png_header(hdr, 10001, 1);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), hdr, len, &rep) ==
	       KMSCON_IMAGE_TOO_LARGE);
	len = png_header(hdr, 5000, 5000);
	r = req_of('d', KMSCON_IMAGE_PNG, 0);
	r.max_bytes = 16 * MiB;
	assert(ask_inline(&h, r, hdr, len, &rep) == KMSCON_IMAGE_OVER_BUDGET);
	/* within both, the missing data is what fails */
	len = png_header(hdr, 100, 100);
	assert(ask_inline(&h, req_of('d', KMSCON_IMAGE_PNG, 0), hdr, len, &rep) ==
	       KMSCON_IMAGE_CORRUPT);
	assert(stop(&h) == 0);
}

/* @r makes the helper exit with status 2, before any reply */
static void exits(struct kmscon_image_request r, const char *name, int fd)
{
	struct helper h;
	struct reply rep;

	start(&h);
	send_req(&h, &r, name, fd);
	assert(recv_reply(&h, &rep) == 0 && stop(&h) == 2);
}

/* requests kmscon would not build end the helper */
static void test_protocol(void)
{
	struct kmscon_image_request r;
	int fd = memfd_of("x", 1);

	/* an unknown flag bit, a non-zero reserved, another version or type */
	r = req_of('f', 0x10, 4);
	exits(r, "/tmp/x", -1);
	r = req_of('f', 0, 4);
	r.reserved = 1;
	exits(r, "/tmp/x", -1);
	r = req_of('f', 0, 4);
	r.version = 1;
	exits(r, "/tmp/x", -1);
	r = req_of('f', 0, 4);
	r.type = 2;
	exits(r, "/tmp/x", -1);
	/* inline data without its descriptor, a name with it, an unknown medium */
	exits(req_of('d', KMSCON_IMAGE_PNG, 0), NULL, -1);
	exits(req_of('d', KMSCON_IMAGE_PNG, 0), "/tmp/x", fd);
	exits(req_of('x', 0, 4), "/tmp/x", -1);
	/* min_bytes 0 without PNG or DISCARD, with INFLATE alone among them */
	exits(req_of('f', 0, 0), "/tmp/x", -1);
	exits(req_of('f', KMSCON_IMAGE_INFLATE, 0), "/tmp/x", -1);
	/* EXACT without INFLATE */
	exits(req_of('d', KMSCON_IMAGE_PNG | KMSCON_IMAGE_EXACT, 4), NULL, fd);
	/* DISCARD with another flag, with medium 'f' or 'd', or with a bound */
	exits(req_of('t', KMSCON_IMAGE_DISCARD | KMSCON_IMAGE_INFLATE, 0), "/tmp/x", -1);
	exits(req_of('t', KMSCON_IMAGE_DISCARD | KMSCON_IMAGE_PNG, 0), "/tmp/x", -1);
	exits(req_of('f', KMSCON_IMAGE_DISCARD, 0), "/tmp/x", -1);
	exits(req_of('d', KMSCON_IMAGE_DISCARD, 0), NULL, fd);
	r = req_of('s', KMSCON_IMAGE_DISCARD, 0);
	r.offset = 1;
	exits(r, "/x", -1);
	r = req_of('s', KMSCON_IMAGE_DISCARD, 0);
	r.read_max = 1;
	exits(r, "/x", -1);
	exits(req_of('t', KMSCON_IMAGE_DISCARD, 1), "/tmp/x", -1);
	/* min_bytes past max_bytes */
	r = req_of('f', 0, 1025);
	r.max_bytes = 1024;
	exits(r, "/tmp/x", -1);
	close(fd);
}

/* @argv: the helper, its test build, and a directory outside the temporary ones */
int main(int argc, char **argv)
{
	assert(argc == 4);
	real_helper = argv[1];
	test_helper = argv[2];
	signal(SIGPIPE, SIG_IGN);

	if (!strncmp(argv[3], "/tmp/", 5) || !strncmp(argv[3], "/dev/", 5)) {
		printf("test_image_helper: the build tree %s is a temporary directory\n", argv[3]);
		return 77;
	}
	snprintf(tmp, sizeof(tmp), "/tmp/kmscon-helper-test-XXXXXX");
	assert(mkdtemp(tmp));
	snprintf(other, sizeof(other), "%s/image-helper-test-XXXXXX", argv[3]);
	assert(mkdtemp(other));

	test_files();
	test_refusals();
	test_temp();
	test_swaps();
	test_shm();
	test_discard();
	test_png_zlib();
	test_ignored();
	test_png_limits();
	test_protocol();

	assert(rmdir(tmp) == 0);
	assert(rmdir(other) == 0);
	printf("test_image_helper: ok\n");
	return 0;
}
