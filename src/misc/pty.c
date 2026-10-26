/*
 * kmscon - Pseudo Terminal Handling
 *
 * Copyright (c) 2012 Ran Benita <ran234@gmail.com>
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
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "pty.h"
#include "shl/eloop.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "shl/ring.h"

#define LOG_SUBSYSTEM "pty"

#define KMSCON_NREAD 16384

#define MAX_RETRY_TIME 2
#define MAX_RETRY_COUNT 5

/*
 * Replies are what the input callback writes back to the child while it
 * parses the child's output: answers to queries. msgbuf holds what the child
 * has not read yet. Once replies there reach PTY_REPLY_HIGH, nothing more of
 * the child's output is read or fed until they drain below PTY_REPLY_LOW, so
 * a child that queries without reading stalls itself. A reply that would take
 * them past PTY_REPLY_MAX, which only one callback call can do, is dropped
 * with the rest of that call's replies. What the user sends (keys, pastes,
 * pointer reports) is queued whole and never stops reading: a paste larger
 * than the bound would otherwise stall a child that echoes it.
 */
#define PTY_REPLY_HIGH (64 * 1024)
#define PTY_REPLY_LOW (16 * 1024)
#define PTY_REPLY_MAX (4 * PTY_REPLY_HIGH)

struct kmscon_pty {
	unsigned long ref;
	struct ev_eloop *eloop;

	int fd;
	pid_t child;
	struct ev_fd *efd;
	struct shl_ring *msgbuf;
	char io_buf[KMSCON_NREAD];
	/* io_buf[held_off, held_len) is what the input callback has not taken */
	size_t held_off;
	size_t held_len;
	bool feeding;	/* held_idle is registered */
	bool paused;	/* kmscon_pty_pause: nothing is read or fed */
	size_t queued;	/* the bytes msgbuf holds */
	size_t replies; /* at least the replies msgbuf holds, at most queued */
	bool in_input;	/* the input callback is running */
	bool backlog;	/* replies reached PTY_REPLY_HIGH: nothing is read or fed */
	bool dropping;	/* a reply passed PTY_REPLY_MAX: the rest are dropped */
	/* the size last set with TIOCSWINSZ */
	struct winsize ws;

	kmscon_pty_input_cb input_cb;
	kmscon_pty_exit_cb exit_cb;
	void *data;

	char *term;
	char *colorterm;
	char **argv;
	char *seat;
	char *vtnr;
	bool env_reset;
	bool backspace_delete;
	bool oneshot;

	time_t last_spawn_time;
	int retry_count;
};

int kmscon_pty_new(struct kmscon_pty **out, kmscon_pty_input_cb input_cb,
		   kmscon_pty_exit_cb exit_cb, void *data)
{
	struct kmscon_pty *pty;
	int ret;

	if (!out || !input_cb)
		return -EINVAL;

	pty = calloc(1, sizeof(*pty));
	if (!pty)
		return -ENOMEM;

	pty->fd = -1;
	pty->ref = 1;
	pty->input_cb = input_cb;
	pty->exit_cb = exit_cb;
	pty->last_spawn_time = time(NULL);
	pty->retry_count = 0;
	pty->data = data;

	ret = ev_eloop_new(&pty->eloop);
	if (ret)
		goto err_free;

	ret = shl_ring_new(&pty->msgbuf);
	if (ret)
		goto err_eloop;

	log_debug("new pty object");
	*out = pty;
	return 0;

err_eloop:
	ev_eloop_unref(pty->eloop);
err_free:
	free(pty);
	return ret;
}

void kmscon_pty_ref(struct kmscon_pty *pty)
{
	if (!pty)
		return;

	pty->ref++;
}

void kmscon_pty_unref(struct kmscon_pty *pty)
{
	if (!pty || !pty->ref || --pty->ref)
		return;

	log_debug("free pty object");
	kmscon_pty_close(pty);
	free(pty->vtnr);
	free(pty->seat);
	free(pty->argv);
	free(pty->colorterm);
	free(pty->term);
	shl_ring_free(pty->msgbuf);
	ev_eloop_unref(pty->eloop);
	free(pty);
}

int kmscon_pty_set_conf(struct kmscon_pty *pty, const char *term, const char *colorterm,
			char **argv, const char *seat, unsigned int vtnr, bool do_reset,
			bool backspace, bool oneshot)
{
	char *vt;

	if (!pty)
		return -EINVAL;

	pty->env_reset = do_reset;
	pty->backspace_delete = backspace;
	pty->oneshot = oneshot;

	if (term) {
		pty->term = strdup(term);
		if (!pty->term)
			return -ENOMEM;
	}
	if (colorterm) {
		pty->colorterm = strdup(colorterm);
		if (!pty->colorterm)
			return -ENOMEM;
	}
	if (seat) {
		pty->seat = strdup(seat);
		if (!pty->seat)
			return -ENOMEM;
	}
	if (vtnr) {
		if (asprintf(&vt, "%u", vtnr) < 0)
			return -ENOMEM;
		pty->vtnr = vt;
	}
	if (argv && *argv && **argv)
		return shl_dup_array(&pty->argv, argv);
	return 0;
}

int kmscon_pty_get_fd(struct kmscon_pty *pty)
{
	if (!pty)
		return -EINVAL;

	return ev_eloop_get_fd(pty->eloop);
}

void kmscon_pty_dispatch(struct kmscon_pty *pty)
{
	if (!pty)
		return;

	ev_eloop_dispatch(pty->eloop, 0);
}

struct ev_eloop *kmscon_pty_get_eloop(struct kmscon_pty *pty)
{
	return pty ? pty->eloop : NULL;
}

static bool pty_is_open(struct kmscon_pty *pty)
{
	return pty->fd >= 0;
}

static void __attribute__((noreturn)) exec_child(const char *term, const char *colorterm,
						 char **argv, const char *seat, const char *vtnr,
						 bool env_reset, bool drm)
{
	char **env;
	char **def_argv;
	char **argv_parsed;
	int ret;

	if (env_reset) {
		env = calloc(1, sizeof(char *));
		if (!env) {
			log_error("cannot allocate memory for environment (%d): %m", errno);
			exit(EXIT_FAILURE);
		}
		environ = env;

		def_argv = (char *[]){"/bin/login", "-p", NULL};
	} else {
		def_argv = (char *[]){"/bin/login", NULL};
	}

	if (!term)
		term = "vt220";
	if (!argv)
		argv = def_argv;

	setenv("TERM", term, 1);
	if (colorterm)
		setenv("COLORTERM", colorterm, 1);
	if (seat)
		setenv("XDG_SEAT", seat, 1);
	if (vtnr)
		setenv("XDG_VTNR", vtnr, 1);

	if (drm) {
		setenv("KMS_START_SCRIPT", "kmscon-launch-gui", 1);
		setenv("TERM_SESSION_TYPE", "kms", 1);
	} else {
		setenv("TERM_SESSION_TYPE", "fb", 1);
	}
	ret = shl_replace_array_with_env(&argv_parsed, argv);
	if (ret)
		argv_parsed = argv;

	execve(argv_parsed[0], argv_parsed, environ);

	log_err("failed to exec child %s: %m", argv[0]);

	exit(EXIT_FAILURE);
}

static void setup_child(int master, bool backspace_delete, struct winsize *ws)
{
	int ret;
	sigset_t sigset;
	pid_t pid;
	char slave_name[128];
	int slave = -1, i;
	struct termios attr;

	/* The child should not inherit our signal mask. */
	sigemptyset(&sigset);
	ret = pthread_sigmask(SIG_SETMASK, &sigset, NULL);
	if (ret)
		log_warn("cannot reset blocked signals: %m");

	for (i = 1; i < SIGSYS; ++i)
		signal(i, SIG_DFL);

	ret = grantpt(master);
	if (ret < 0) {
		log_err("grantpt failed: %m");
		goto err_out;
	}

	ret = unlockpt(master);
	if (ret < 0) {
		log_err("cannot unlock pty: %m");
		goto err_out;
	}

	ret = ptsname_r(master, slave_name, sizeof(slave_name));
	if (ret) {
		log_err("cannot find slave name: %m");
		goto err_out;
	}

	/* This also loses our controlling tty. */
	pid = setsid();
	if (pid < 0) {
		log_err("cannot start a new session: %m");
		goto err_out;
	}

	/* And the slave pty becomes our controlling tty. */
	slave = open(slave_name, O_RDWR | O_CLOEXEC);
	if (slave < 0) {
		log_err("cannot open slave: %m");
		goto err_out;
	}

	/* get terminal attributes */
	if (tcgetattr(slave, &attr) < 0) {
		log_err("cannot get terminal attributes: %m");
		goto err_out;
	}

	if (backspace_delete) {
		/* erase character should be delete */
		attr.c_cc[VERASE] = 0177;
	} else {
		/* erase character should be normal backspace */
		attr.c_cc[VERASE] = 010;
	}

	/* set changed terminal attributes */
	if (tcsetattr(slave, TCSANOW, &attr) < 0) {
		log_warn("cannot set terminal attributes: %m");
		goto err_out;
	}

	if (ws) {
		ret = ioctl(slave, TIOCSWINSZ, ws);
		if (ret)
			log_warn("cannot set slave window size: %m");
	}

	if (dup2(slave, STDIN_FILENO) != STDIN_FILENO ||
	    dup2(slave, STDOUT_FILENO) != STDOUT_FILENO ||
	    dup2(slave, STDERR_FILENO) != STDERR_FILENO) {
		log_err("cannot duplicate slave: %m");
		goto err_out;
	}

	close(master);
	close(slave);
	return;

err_out:
	ret = -errno;
	if (slave >= 0)
		close(slave);
	close(master);
	exit(EXIT_FAILURE);
}

/*
 * This is functionally equivalent to forkpty(3). We do it manually to obtain
 * a little bit more control of the process, and as a bonus avoid linking to
 * the libutil library in glibc.
 */
static int pty_spawn(struct kmscon_pty *pty, int master, const struct winsize *size, bool drm)
{
	pid_t pid;
	struct winsize ws = *size;

	pid = fork();
	switch (pid) {
	case -1:
		log_err("cannot fork: %m");
		return -errno;
	case 0:
		setup_child(master, pty->backspace_delete, &ws);
		exec_child(pty->term, pty->colorterm, pty->argv, pty->seat, pty->vtnr,
			   pty->env_reset, drm);
		exit(EXIT_FAILURE);
	default:
		log_debug("forking child %d", pid);
		pty->fd = master;
		pty->child = pid;
		break;
	}

	return 0;
}

/* a pause or a backlog: nothing is read or fed */
static bool stalled(struct kmscon_pty *pty)
{
	return pty->paused || pty->backlog;
}

/*
 * The replies msgbuf holds are not tracked byte by byte: after a drain they
 * are counted as at most what is left, which may count user bytes queued
 * behind replies the child has read, but never counts too few.
 */
static void queue_drop(struct kmscon_pty *pty, size_t len)
{
	shl_ring_drop(pty->msgbuf, len);
	pty->queued -= len;
	if (pty->replies > pty->queued)
		pty->replies = pty->queued;
}

static void queue_write(struct kmscon_pty *pty, const char *u8, size_t len)
{
	if (pty->in_input && (pty->dropping || pty->replies + len > PTY_REPLY_MAX)) {
		if (!pty->dropping)
			log_warn("child %d does not read its input; dropping replies", pty->child);
		pty->dropping = true;
		return;
	}
	if (shl_ring_write(pty->msgbuf, u8, len)) {
		log_warn("cannot allocate buffer; dropping output");
		return;
	}
	pty->queued += len;
	if (!pty->in_input)
		return;
	pty->replies += len;
	if (!pty->backlog && pty->replies >= PTY_REPLY_HIGH) {
		log_debug("child %d does not read its input; reading stops", pty->child);
		pty->backlog = true;
	}
}

static void restart(struct kmscon_pty *pty);

static int send_buf(struct kmscon_pty *pty)
{
	const char *buf;
	size_t len;
	int ret = 0;

	while ((buf = shl_ring_peek(pty->msgbuf, &len, 0))) {
		ret = write(pty->fd, buf, len);
		if (ret <= 0)
			break;
		queue_drop(pty, ret);
	}

	if (buf && ret < 0 && errno != EWOULDBLOCK) {
		log_warn("cannot write to child process (%d): %m", errno);
		return ret;
	}
	if (!buf)
		ev_fd_update(pty->efd, EV_READABLE | EV_ET);
	if (pty->backlog && pty->replies < PTY_REPLY_LOW) {
		log_debug("child %d read its input; reading goes on", pty->child);
		pty->backlog = false;
		pty->dropping = false;
		restart(pty);
	}
	return 0;
}

/* edge-triggered: asks for EV_READABLE again, which reports data already there */
static void rearm(struct kmscon_pty *pty)
{
	int mask = EV_READABLE | EV_ET;

	if (!shl_ring_is_empty(pty->msgbuf))
		mask |= EV_WRITEABLE;
	ev_fd_update(pty->efd, mask);
}

static void held_idle(struct ev_eloop *eloop, void *unused, void *data);

static void feeding_stop(struct kmscon_pty *pty)
{
	if (!pty->feeding)
		return;
	ev_eloop_unregister_idle_cb(pty->eloop, held_idle, pty, EV_NORMAL);
	pty->feeding = false;
}

/*
 * Hands io_buf[@off, @len) to the input callback. What it does not take is
 * held: nothing more is read, and an idle source on the pty's loop hands the
 * rest over on later turns, so other events are handled between the parts.
 * Without an idle source the rest is handed over at once. A pause from inside
 * the callback, or a backlog of replies it wrote, holds the rest with no idle
 * source until kmscon_pty_resume or the drain. Returns true when all of it was
 * taken and the pty is not stalled.
 */
static size_t call_input(struct kmscon_pty *pty, size_t off, size_t len)
{
	bool was = pty->in_input;
	size_t n;

	pty->in_input = true;
	n = pty->input_cb(pty, pty->io_buf + off, len - off, pty->data);
	pty->in_input = was;
	/* a backlog holds the drop until the drain; nothing is fed till then */
	if (!was && !pty->backlog)
		pty->dropping = false;
	return n;
}

static bool feed(struct kmscon_pty *pty, size_t off, size_t len)
{
	off += call_input(pty, off, len);
	if (off < len && !stalled(pty) && !pty->feeding) {
		if (!ev_eloop_register_idle_cb(pty->eloop, held_idle, pty, EV_NORMAL)) {
			pty->feeding = true;
		} else {
			log_warn("cannot feed pty output in parts");
			while (off < len && !stalled(pty))
				off += call_input(pty, off, len);
		}
	}
	if (off >= len) {
		pty->held_len = 0;
		feeding_stop(pty);
		return !stalled(pty);
	}
	pty->held_off = off;
	pty->held_len = len;
	return false;
}

static int read_buf(struct kmscon_pty *pty)
{
	ssize_t len, num;

	if (pty->held_len || stalled(pty))
		return 0;

	/* Use a maximum of 50 steps to avoid staying here forever.
	 * TODO: recheck where else a user might flush our queues and try to
	 * install an explicit policy. */
	num = 50;
	do {
		len = read(pty->fd, pty->io_buf, sizeof(pty->io_buf));
		if (len > 0) {
			if (!feed(pty, 0, len))
				return 0;
		} else if (len == 0) {
			log_debug("HUP during read on pty of child %d", pty->child);
			break;
		} else if (errno != EWOULDBLOCK) {
			log_debug("cannot read from pty of child %d (%d): %m", pty->child, errno);
			break;
		}
	} while (len > 0 && --num);

	if (!num) {
		log_debug("cannot read application data fast enough");

		/* We are edge-triggered so update the mask to get the
		 * EV_READABLE event again next round. */
		rearm(pty);
	}

	return 0;
}

/* the held output, one input call a turn; reading resumes once it is all taken */
static void held_idle(struct ev_eloop *eloop, void *unused, void *data)
{
	struct kmscon_pty *pty = data;

	if (!pty->held_len || stalled(pty)) {
		feeding_stop(pty);
		return;
	}
	if (feed(pty, pty->held_off, pty->held_len))
		rearm(pty);
}

void kmscon_pty_pause(struct kmscon_pty *pty)
{
	if (!pty || !pty_is_open(pty))
		return;
	pty->paused = true;
	feeding_stop(pty);
}

/*
 * After a pause or a backlog, once neither holds. The fd is edge-triggered,
 * so rearm() reports output that came while stalled; a read finds the held
 * output first and leaves it to held_idle, which rearms again once it is all
 * taken.
 */
static void restart(struct kmscon_pty *pty)
{
	if (!pty_is_open(pty) || stalled(pty))
		return;
	rearm(pty);
	if (!pty->held_len || pty->feeding)
		return;
	if (!ev_eloop_register_idle_cb(pty->eloop, held_idle, pty, EV_NORMAL)) {
		pty->feeding = true;
		return;
	}
	log_warn("cannot feed pty output in parts");
	if (feed(pty, pty->held_off, pty->held_len))
		rearm(pty);
}

void kmscon_pty_resume(struct kmscon_pty *pty)
{
	if (!pty || !pty->paused)
		return;
	pty->paused = false;
	restart(pty);
}

static void pty_input(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_pty *pty = data;

	/* Programs like /bin/login tend to perform a vhangup() on their TTY
	 * before running the login procedure. This also causes the pty master
	 * to get a EV_HUP event as long as no client has the TTY opened.
	 * This means, we cannot use the TTY connection as reliable way to track
	 * the client. Instead, we _must_ rely on the PID of the client to track
	 * them.
	 * However, this has the side effect that if the client forks and the
	 * parent exits, we loose them and restart the client. But this seems to
	 * be the expected behavior so we implement it here.
	 * Unfortunately, epoll always polls for EPOLLHUP so as long as the
	 * vhangup() is ongoing, we will _always_ get EPOLLHUP and cannot sleep.
	 * This gets worse if the client closes the TTY but doesn't exit.
	 * Therefore, we set the fd as edge-triggered in the epoll-set so we
	 * only get the events once they change. This has to be taken into
	 * account at all places of kmscon_pty to avoid missing events. */

	if (mask & EV_ERR)
		log_warn("error on pty socket of child %d", pty->child);
	if (mask & EV_HUP)
		log_debug("HUP on pty of child %d", pty->child);
	if (mask & EV_WRITEABLE)
		send_buf(pty);
	if (mask & EV_READABLE)
		read_buf(pty);
}

static void sig_child(struct ev_eloop *eloop, struct ev_child_data *chld, void *data)
{
	struct kmscon_pty *pty = data;
	time_t current_time;

	if (chld->pid != pty->child)
		return;

	log_info("child exited: pid: %u status: %d", chld->pid, chld->status);

	if (pty->retry_count == MAX_RETRY_COUNT) {
		log_err("reached max retry attempts for login process %d retries in %d seconds",
			MAX_RETRY_COUNT, MAX_RETRY_TIME);
		pty->exit_cb(pty, false, pty->data);
		return;
	}
	if (pty->oneshot) {
		log_info("oneshot is enabled, exiting kmscon...");
		pty->exit_cb(pty, false, pty->data);
		return;
	}

	current_time = time(NULL);

	if (current_time - pty->last_spawn_time < MAX_RETRY_TIME) {
		pty->retry_count++;
		pty->last_spawn_time = current_time;
	} else {
		pty->retry_count = 0;
	}
	pty->exit_cb(pty, true, pty->data);
}

/* a TIOCSWINSZ pixel size, which holds 16 bits */
static unsigned short winsize_px(unsigned int cells, unsigned int cell)
{
	return (uint64_t)cells * cell > USHRT_MAX ? USHRT_MAX : cells * cell;
}

static struct winsize winsize(unsigned short width, unsigned short height, unsigned int cell_w,
			      unsigned int cell_h)
{
	return (struct winsize){.ws_col = width,
				.ws_row = height,
				.ws_xpixel = winsize_px(width, cell_w),
				.ws_ypixel = winsize_px(height, cell_h)};
}

int kmscon_pty_open(struct kmscon_pty *pty, unsigned short width, unsigned short height,
		    unsigned int cell_w, unsigned int cell_h, bool drm)
{
	struct winsize ws = winsize(width, height, cell_w, cell_h);
	int ret;
	int master;

	if (!pty)
		return -EINVAL;

	if (pty_is_open(pty))
		return -EALREADY;

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	if (master < 0) {
		log_err("cannot open master: %m");
		return -errno;
	}

	ret = ev_eloop_new_fd(pty->eloop, &pty->efd, master, EV_ET | EV_READABLE, pty_input, pty);
	if (ret)
		goto err_master;

	ret = ev_eloop_register_child_cb(pty->eloop, sig_child, pty);
	if (ret)
		goto err_fd;

	ret = pty_spawn(pty, master, &ws, drm);
	if (ret)
		goto err_sig;
	pty->ws = ws;
	pty->paused = false;

	return 0;

err_sig:
	ev_eloop_unregister_child_cb(pty->eloop, sig_child, pty);
err_fd:
	ev_eloop_rm_fd(pty->efd);
	pty->efd = NULL;
err_master:
	close(master);
	return ret;
}

void kmscon_pty_close(struct kmscon_pty *pty)
{
	if (!pty || !pty_is_open(pty))
		return;

	ev_eloop_rm_fd(pty->efd);
	pty->efd = NULL;
	ev_eloop_unregister_child_cb(pty->eloop, sig_child, pty);
	close(pty->fd);
	pty->fd = -1;
	pty->child = 0;
	pty->held_len = 0;
	pty->paused = false;
	feeding_stop(pty);
	/* what was queued for this child must not reach the next one */
	shl_ring_flush(pty->msgbuf);
	pty->queued = 0;
	pty->replies = 0;
	pty->backlog = false;
	pty->dropping = false;
}

int kmscon_pty_write(struct kmscon_pty *pty, const char *u8, size_t len)
{
	int ret;

	if (!pty || !pty_is_open(pty) || !u8 || !len)
		return -EINVAL;

	if (!shl_ring_is_empty(pty->msgbuf))
		goto buf;

	ret = write(pty->fd, u8, len);
	if (ret < 0) {
		if (errno != EWOULDBLOCK) {
			log_warn("cannot write to child process");
			return ret;
		}
	} else if (ret >= len) {
		return 0;
	} else if (ret > 0) {
		len -= ret;
		u8 = &u8[ret];
	}

	ev_fd_update(pty->efd, EV_READABLE | EV_WRITEABLE | EV_ET);

buf:
	queue_write(pty, u8, len);
	return 0;
}

void kmscon_pty_signal(struct kmscon_pty *pty, int signum)
{
	int ret;

	if (!pty || !pty_is_open(pty) || signum < 0)
		return;

	ret = ioctl(pty->fd, TIOCSIG, signum);
	if (ret) {
		log_warn("cannot send signal %d to child", signum);
		return;
	}

	log_debug("send signal %d to child", signum);
}

int kmscon_pty_get_slave_name(struct kmscon_pty *pty, char *buf, size_t len)
{
	if (!pty || !buf || !len || !pty_is_open(pty))
		return -EINVAL;

	return ptsname_r(pty->fd, buf, len);
}

pid_t kmscon_pty_get_child(struct kmscon_pty *pty)
{
	return pty && pty_is_open(pty) ? pty->child : 0;
}

/* the slave reached from the master, not by its name, which could be replaced */
int kmscon_pty_get_slave_owner(struct kmscon_pty *pty, uid_t *uid)
{
	struct stat st;
	int slave, ret = 0;

	if (!pty || !uid || !pty_is_open(pty))
		return -EINVAL;

	slave = ioctl(pty->fd, TIOCGPTPEER, O_RDONLY | O_NOCTTY | O_CLOEXEC);
	if (slave < 0)
		return -errno;
	if (fstat(slave, &st))
		ret = -errno;
	else
		*uid = st.st_uid;
	close(slave);
	return ret;
}

void kmscon_pty_resize(struct kmscon_pty *pty, unsigned short width, unsigned short height,
		       unsigned int cell_w, unsigned int cell_h)
{
	int ret;
	struct winsize ws = winsize(width, height, cell_w, cell_h);

	if (!pty || !pty_is_open(pty))
		return;
	/* a new font with the same size in cells changes the pixels alone */
	if (ws.ws_col == pty->ws.ws_col && ws.ws_row == pty->ws.ws_row &&
	    ws.ws_xpixel == pty->ws.ws_xpixel && ws.ws_ypixel == pty->ws.ws_ypixel)
		return;

	/*
	 * This will send SIGWINCH to the pty slave foreground process group.
	 * We will also get one, but we don't need it.
	 */
	ret = ioctl(pty->fd, TIOCSWINSZ, &ws);
	if (ret) {
		log_warn("cannot set window size");
		return;
	}
	pty->ws = ws;
}
