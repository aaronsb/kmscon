/*
 * kmscon - A terminal's image helper
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
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "image_helper_proto.h"
#include "image_host.h"
#include "shl/log.h"

#define LOG_SUBSYSTEM "image_host"

/* the helper's address space beyond three copies of the budget: code and libraries */
#define HELPER_AS_BASE ((uint64_t)64 << 20)
/* descriptors one reply may carry that kmscon reads, to close them all */
#define REPLY_FDS 8

#define SEALS (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)

struct image_host {
	struct ev_eloop *loop;
	char *helper;
	char *tmpdir_env; /* "TMPDIR=<kmscon's>", or NULL */
	image_host_reply_cb cb;
	void *data;
	struct ev_timer *timer;
	unsigned int timeout_ms;

	/* the running helper; pid 0 when none */
	pid_t pid;
	int pidfd;
	int sock;
	struct ev_fd *efd;
	uid_t uid;
	uint64_t max_bytes; /* its memory limit was set from this */

	/* why the last helper was stopped, for tests */
	const char *failure;

	/* the request out */
	bool busy;
	struct image_host_request req; /* its pointers are not kept */
};

/* the identity the child takes, resolved before the fork */
struct creds {
	bool change;
	uid_t uid;
	gid_t gid;
	gid_t *groups;
	int n_groups;
};

static void helper_event(struct ev_fd *fd, int mask, void *data);

/*
 * The uid's primary gid and supplementary groups, found in kmscon before it
 * forks: the child of a threaded process may call only async-signal-safe
 * functions, and getpwuid_r and getgrouplist are not. Not running as root,
 * kmscon can start a helper only as its own user, and changes nothing.
 */
static int resolve_creds(uid_t uid, struct creds *c)
{
	struct passwd pw, *res = NULL;
	size_t size = 16384;
	char *buf = NULL, *tmp;
	gid_t *groups;
	int n = 32, ret;

	*c = (struct creds){.uid = uid};
	if (geteuid() != 0) {
		if (uid != geteuid()) {
			log_warning("not root: cannot start the image helper as uid %u", uid);
			return -EPERM;
		}
		return 0;
	}

	do {
		tmp = realloc(buf, size);
		if (!tmp) {
			free(buf);
			return -ENOMEM;
		}
		buf = tmp;
		ret = getpwuid_r(uid, &pw, buf, size, &res);
		size *= 2;
	} while (ret == ERANGE && size <= (1 << 22));
	if (ret || !res) {
		log_warning("no user %u for the image helper", uid);
		free(buf);
		return ret ? -ret : -ENOENT;
	}

	c->change = true;
	c->gid = pw.pw_gid;
	for (;;) {
		groups = realloc(c->groups, n * sizeof(*groups));
		if (!groups) {
			ret = -ENOMEM;
			break;
		}
		c->groups = groups;
		if (getgrouplist(pw.pw_name, pw.pw_gid, c->groups, &n) >= 0) {
			c->n_groups = n;
			ret = 0;
			break;
		}
		if (n > 65536) {
			ret = -E2BIG;
			break;
		}
	}
	free(buf);
	return ret;
}

/*
 * The forked child: only async-signal-safe calls, then the helper with a
 * clean environment, descriptors 0 to 2 on /dev/null, its socket as 3 and
 * nothing else open.
 */
static void __attribute__((noreturn)) exec_helper(const char *path, char *const argv[],
						  char *const envp[], const struct creds *c,
						  const struct rlimit *as, int null, int sock)
{
	sigset_t none;

	sigemptyset(&none);
	if (sigprocmask(SIG_SETMASK, &none, NULL) || dup2(null, 0) < 0 || dup2(null, 1) < 0 ||
	    dup2(null, 2) < 0 || dup2(sock, KMSCON_IMAGE_FD) < 0 ||
	    close_range(KMSCON_IMAGE_FD + 1, ~0U, 0))
		_exit(127);
	if (setrlimit(RLIMIT_AS, as))
		_exit(127);
	if (c->change && (setgroups(c->n_groups, c->groups) || setresgid(c->gid, c->gid, c->gid) ||
			  setresuid(c->uid, c->uid, c->uid)))
		_exit(127);
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || chdir("/"))
		_exit(127);
	execve(path, argv, envp);
	_exit(127);
}

static int start_helper(struct image_host *h, uid_t uid, uint64_t max_bytes)
{
	char *argv[] = {h->helper, NULL};
	char *envp[] = {h->tmpdir_env, NULL};
	struct creds c;
	struct rlimit as;
	int sv[2], null = -1, sock = -1, ret;
	uint64_t limit;
	pid_t pid;

	ret = resolve_creds(uid, &c);
	if (ret)
		goto out;

	/* three copies of the budget: the data read, an inflated copy and the output */
	limit = max_bytes > (UINT64_MAX - HELPER_AS_BASE) / 3 ? UINT64_MAX
							      : max_bytes * 3 + HELPER_AS_BASE;
	as.rlim_cur = as.rlim_max = limit >= RLIM_INFINITY ? RLIM_INFINITY - 1 : limit;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
		ret = -errno;
		goto out;
	}
	/* both above 3, so the child's dup2 calls cannot overwrite either */
	sock = fcntl(sv[1], F_DUPFD_CLOEXEC, KMSCON_IMAGE_FD + 1);
	close(sv[1]);
	ret = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (ret >= 0) {
		null = fcntl(ret, F_DUPFD_CLOEXEC, KMSCON_IMAGE_FD + 1);
		close(ret);
	}
	if (sock < 0 || null < 0) {
		ret = -errno;
		goto err_sock;
	}

	pid = fork();
	if (pid < 0) {
		ret = -errno;
		goto err_sock;
	}
	if (!pid)
		exec_helper(h->helper, argv, envp, &c, &as, null, sock);

	close(sock);
	close(null);
	sock = null = -1;
	/* not reaped yet: the loop reaps only when it dispatches SIGCHLD */
	h->pidfd = syscall(SYS_pidfd_open, pid, 0);
	if (h->pidfd < 0) {
		ret = -errno;
		kill(pid, SIGKILL);
		goto err_sock;
	}
	ret = ev_eloop_new_fd(h->loop, &h->efd, sv[0], EV_READABLE, helper_event, h);
	if (ret) {
		syscall(SYS_pidfd_send_signal, h->pidfd, SIGKILL, NULL, 0);
		close(h->pidfd);
		h->pidfd = -1;
		goto err_sock;
	}
	h->pid = pid;
	h->sock = sv[0];
	h->uid = uid;
	h->max_bytes = max_bytes;
	log_debug("image helper %d started as uid %u", pid, uid);
	free(c.groups);
	return 0;

err_sock:
	if (sock >= 0)
		close(sock);
	if (null >= 0)
		close(null);
	close(sv[0]);
out:
	free(c.groups);
	log_warning("cannot start the image helper %s: %d", h->helper, ret);
	return ret;
}

/* the loop reaps it, whoever's child callback runs; its pid is never signalled */
static void stop_helper(struct image_host *h)
{
	if (!h->pid)
		return;
	syscall(SYS_pidfd_send_signal, h->pidfd, SIGKILL, NULL, 0);
	close(h->pidfd);
	ev_eloop_rm_fd(h->efd);
	close(h->sock);
	h->pidfd = -1;
	h->efd = NULL;
	h->sock = -1;
	h->pid = 0;
}

static void end_request(struct image_host *h)
{
	h->busy = false;
	ev_timer_update(h->timer, NULL);
}

/* the request out, if any, ends with EBADF, and the helper with SIGKILL */
static void fail(struct image_host *h, const char *why)
{
	uint64_t id = h->req.id;
	bool busy = h->busy;

	log_warning("image helper %d: %s; stopping it", h->pid, why);
	h->failure = why;
	stop_helper(h);
	if (!busy)
		return;
	end_request(h);
	h->cb(h, id, -EBADF, 0, 0, NULL, h->data);
}

static void close_fds(const int *fds, unsigned int n)
{
	while (n--)
		close(fds[n]);
}

/* the helper's reason, as printable ASCII, for the log */
static void log_reason(const struct image_host *h, int status, const char *reason, size_t len)
{
	char buf[KMSCON_IMAGE_REASON_MAX + 1];
	size_t i;

	if (!len)
		return;
	for (i = 0; i < len; i++)
		buf[i] = reason[i] >= 0x20 && reason[i] <= 0x7e ? reason[i] : '?';
	buf[len] = 0;
	if (status)
		log_info("image request %llu: %d: %s", (unsigned long long)h->req.id, status, buf);
	else
		log_debug("image request %llu: %s", (unsigned long long)h->req.id, buf);
}

/*
 * The pixels of a status 0 reply in @fd: a sealed memfd of exactly width x
 * height x 4 bytes, within the request's limits. Returns NULL, with @why set,
 * when any check fails.
 */
static const uint8_t *map_pixels(const struct image_host *h, const struct kmscon_image_reply *r,
				 int fd, size_t *len, const char **why)
{
	const struct image_host_request *req = &h->req;
	bool raw = req->format == 24 || req->format == 32;
	uint64_t bytes;
	struct stat st;
	int seals;
	void *map;

	if (!r->width || !r->height || r->width > req->max_width || r->height > req->max_height) {
		*why = "a reply's size is outside max-graphic-size";
		return NULL;
	}
	bytes = (uint64_t)r->width * r->height * 4;
	if (bytes > req->max_bytes || bytes > SIZE_MAX) {
		*why = "a reply larger than the budget";
		return NULL;
	}
	if (raw && req->width && req->height &&
	    (r->width != req->width || r->height != req->height)) {
		*why = "a reply's size differs from s and v";
		return NULL;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		*why = "a reply's descriptor is not a regular file";
		return NULL;
	}
	if ((uint64_t)st.st_size != bytes) {
		*why = "a reply's memfd is not exactly width x height x 4 bytes";
		return NULL;
	}
	/* F_SEAL_FUTURE_WRITE does not stand in for F_SEAL_WRITE */
	seals = fcntl(fd, F_GET_SEALS);
	if (seals < 0 || (seals & SEALS) != SEALS) {
		*why = "a reply's memfd is not sealed";
		return NULL;
	}
	map = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		*why = "a reply's memfd cannot be mapped";
		return NULL;
	}
	*len = bytes;
	return map;
}

/*
 * One packet from the helper, checked before any pixel is read: a request
 * out, a length of exactly the header and its reason, the version, the id, a
 * known status, and a descriptor only with status 0. Every descriptor
 * received is closed whatever the outcome.
 */
static void receive(struct image_host *h)
{
	union {
		struct kmscon_image_reply r;
		char buf[sizeof(struct kmscon_image_reply) + KMSCON_IMAGE_REASON_MAX + 1];
	} in;
	char cbuf[CMSG_SPACE(REPLY_FDS * sizeof(int))];
	struct iovec iov = {.iov_base = in.buf, .iov_len = sizeof(in.buf)};
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = cbuf,
		.msg_controllen = sizeof(cbuf),
	};
	const struct kmscon_image_reply *r = &in.r;
	int fds[REPLY_FDS], status;
	unsigned int n_fds = 0, i, n;
	const char *why = NULL;
	const uint8_t *pixels;
	struct cmsghdr *cmsg;
	uint64_t id;
	size_t len;
	ssize_t got;

	got = recvmsg(h->sock, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (got < 0 && (errno == EAGAIN || errno == EINTR))
		return;

	for (cmsg = CMSG_FIRSTHDR(&msg); got >= 0 && cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
			why = "a reply with a control message other than descriptors";
			continue;
		}
		n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (i = 0; i < n && n_fds < REPLY_FDS; i++)
			memcpy(&fds[n_fds++], CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
	}

	if (got < 0)
		why = "its socket failed";
	else if (!got)
		why = "it closed its socket";
	else if (!h->busy)
		why = "a reply with no request out";
	else if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
		why = "a truncated reply";
	else if ((size_t)got < sizeof(*r) || r->reason_len > KMSCON_IMAGE_REASON_MAX ||
		 (size_t)got != sizeof(*r) + r->reason_len)
		why = "a reply whose length is not its header and reason";
	else if (r->reserved)
		why = "a reply whose reserved field is not 0";
	else if (r->version != KMSCON_IMAGE_VERSION)
		why = "a reply of another version";
	else if (r->id != h->req.id)
		why = "a reply to another request";
	else if (r->status && r->status != -EBADF && r->status != -EINVAL && r->status != -ENOSPC)
		why = "a reply with an unknown status";
	else if (r->status ? n_fds != 0 : n_fds != 1)
		why = r->status ? "an error reply with a descriptor"
				: "a reply without exactly one descriptor";

	if (why) {
		close_fds(fds, n_fds);
		/* the helper exiting with no request out is no failure */
		if (!got && !h->busy) {
			log_debug("image helper %d exited", h->pid);
			stop_helper(h);
			return;
		}
		fail(h, why);
		return;
	}

	log_reason(h, r->status, in.buf + sizeof(*r), r->reason_len);
	status = r->status;
	id = h->req.id;
	if (status) {
		end_request(h);
		h->cb(h, id, status, 0, 0, NULL, h->data);
		return;
	}

	pixels = map_pixels(h, r, fds[0], &len, &why);
	if (!pixels) {
		close(fds[0]);
		fail(h, why);
		return;
	}
	end_request(h);
	h->cb(h, id, 0, r->width, r->height, pixels, h->data);
	munmap((void *)pixels, len);
	close(fds[0]);
}

static void helper_event(struct ev_fd *fd, int mask, void *data)
{
	receive(data);
}

static void timer_event(struct ev_timer *timer, uint64_t num, void *data)
{
	struct image_host *h = data;

	if (h->busy)
		fail(h, "no reply in time");
}

static void child_event(struct ev_eloop *loop, struct ev_child_data *chld, void *data)
{
	struct image_host *h = data;

	if (h->pid && chld->pid == h->pid)
		log_debug("image helper %d exited with status %d", chld->pid, chld->status);
}

/* the inline data in a sealed memfd, which kmscon closes once sent */
static int data_memfd(const struct image_host_request *req)
{
	size_t done = 0;
	ssize_t n;
	int fd;

	fd = memfd_create("kmscon-image-data", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0)
		return -errno;
	while (done < req->data_len) {
		n = write(fd, req->data + done, req->data_len - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto err;
		done += n;
	}
	if (fcntl(fd, F_ADD_SEALS, SEALS | F_SEAL_SEAL) < 0)
		goto err;
	return fd;

err:
	close(fd);
	return -EIO;
}

static int send_request(struct image_host *h, const struct image_host_request *req)
{
	struct kmscon_image_request r = {
		.version = KMSCON_IMAGE_VERSION,
		.type = KMSCON_IMAGE_TYPE_KITTY,
		.id = req->id,
		.medium = req->medium,
		.flags = (req->zlib ? KMSCON_IMAGE_ZLIB : 0) |
			 (req->discard ? KMSCON_IMAGE_DISCARD : 0),
		.format = req->format,
		.width = req->width,
		.height = req->height,
		.offset = req->offset,
		.size = req->size,
		.max_width = req->max_width,
		.max_height = req->max_height,
		.max_bytes = req->max_bytes,
	};
	struct iovec iov[2] = {{.iov_base = &r, .iov_len = sizeof(r)}};
	char cbuf[CMSG_SPACE(sizeof(int))] = {0};
	struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
	struct cmsghdr *cmsg;
	int fd = -1, ret = 0;

	if (req->medium == 'd') {
		fd = data_memfd(req);
		if (fd < 0)
			return fd;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	} else {
		r.name_len = req->name ? strnlen(req->name, KMSCON_IMAGE_NAME_MAX + 1) : 0;
		if (!r.name_len || r.name_len > KMSCON_IMAGE_NAME_MAX)
			return -EINVAL;
		iov[1].iov_base = (void *)req->name;
		iov[1].iov_len = r.name_len;
	}

	if (sendmsg(h->sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT) < 0)
		ret = -errno;
	if (fd >= 0)
		close(fd);
	return ret;
}

int image_host_send(struct image_host *host, uid_t uid, const struct image_host_request *req)
{
	struct itimerspec spec = {
		.it_value = {host->timeout_ms / 1000, (host->timeout_ms % 1000) * 1000000L},
	};
	int ret;

	if (!host || !req)
		return -EINVAL;
	if (host->busy)
		return -EBUSY;
	if (req->medium != 'd' && req->medium != 'f' && req->medium != 't' && req->medium != 's')
		return -EBADF;

	if (host->pid && (host->uid != uid || req->max_bytes > host->max_bytes)) {
		log_debug("image helper %d: a new user or a larger budget; restarting it",
			  host->pid);
		stop_helper(host);
	}
	if (!host->pid && start_helper(host, uid, req->max_bytes))
		return -EBADF;

	ret = send_request(host, req);
	if (ret) {
		log_warning("cannot send image request %llu: %d", (unsigned long long)req->id, ret);
		stop_helper(host);
		return -EBADF;
	}
	host->req = *req;
	host->req.name = NULL;
	host->req.data = NULL;
	host->busy = true;
	ev_timer_update(host->timer, &spec);
	return 0;
}

void image_host_cancel(struct image_host *host)
{
	if (!host)
		return;
	stop_helper(host);
	end_request(host);
}

bool image_host_busy(struct image_host *host)
{
	return host && host->busy;
}

pid_t image_host_pid(struct image_host *host)
{
	return host ? host->pid : 0;
}

void image_host_set_timeout(struct image_host *host, unsigned int ms)
{
	host->timeout_ms = ms;
}

const char *image_host_last_failure(struct image_host *host)
{
	return host->failure;
}

int image_host_new(struct image_host **out, struct ev_eloop *loop, const char *helper,
		   image_host_reply_cb cb, void *data)
{
	struct image_host *h;
	const char *tmpdir;
	int ret;

	/* the child changes to / before its exec */
	if (!out || !loop || !helper || helper[0] != '/' || !cb)
		return -EINVAL;

	h = calloc(1, sizeof(*h));
	if (!h)
		return -ENOMEM;
	h->loop = loop;
	h->cb = cb;
	h->data = data;
	h->pidfd = -1;
	h->sock = -1;
	h->timeout_ms = IMAGE_HOST_TIMEOUT_MS;
	h->helper = strdup(helper);
	if (!h->helper) {
		ret = -ENOMEM;
		goto err_free;
	}
	/* the helper reads t=t files in kmscon's TMPDIR too */
	tmpdir = getenv("TMPDIR");
	if (tmpdir && tmpdir[0] == '/' && asprintf(&h->tmpdir_env, "TMPDIR=%s", tmpdir) < 0) {
		h->tmpdir_env = NULL;
		ret = -ENOMEM;
		goto err_free;
	}

	ret = ev_eloop_new_timer(loop, &h->timer, NULL, timer_event, h);
	if (ret)
		goto err_free;
	ret = ev_eloop_register_child_cb(loop, child_event, h);
	if (ret)
		goto err_timer;

	ev_eloop_ref(loop);
	*out = h;
	return 0;

err_timer:
	ev_eloop_rm_timer(h->timer);
err_free:
	free(h->tmpdir_env);
	free(h->helper);
	free(h);
	return ret;
}

void image_host_free(struct image_host *host)
{
	if (!host)
		return;
	image_host_cancel(host);
	ev_eloop_unregister_child_cb(host->loop, child_event, host);
	ev_eloop_rm_timer(host->timer);
	ev_eloop_unref(host->loop);
	free(host->tmpdir_env);
	free(host->helper);
	free(host);
}
