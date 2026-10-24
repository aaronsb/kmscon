/*
 * A helper that truncates its reply's memfd after kmscon's size check and
 * seals it before kmscon reads the seals (src/image_host.c, map_pixels). The
 * mapping would then reach past the memfd's end and fault kmscon, which runs
 * as root, on its first read of the pixels. kmscon must read the seals before
 * the size, so the size it checks can no longer change.
 *
 * fcntl is wrapped (-Wl,--wrap) so that the window before F_GET_SEALS stays
 * open until the stand-in helper (fake_image_helper.c, "race:") has shrunk
 * and sealed the memfd. The reviewer's proof of concept for PR #162 had this
 * shape.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "image_host.h"

static char dir[PATH_MAX];
static bool racing;

int __real_fcntl(int fd, int cmd, ...);
int __real_fcntl64(int fd, int cmd, ...);

/* at F_GET_SEALS, lets the stand-in shrink and seal, then goes on */
static void hold_window(int cmd)
{
	char path[PATH_MAX + 16], b;
	int go, ack;

	if (!racing || cmd != F_GET_SEALS)
		return;
	racing = false;
	snprintf(path, sizeof(path), "%s/go.fifo", dir);
	go = open(path, O_WRONLY);
	snprintf(path, sizeof(path), "%s/ack.fifo", dir);
	ack = open(path, O_RDONLY);
	assert(go >= 0 && ack >= 0);
	assert(write(go, "g", 1) == 1 && read(ack, &b, 1) == 1);
	close(go);
	close(ack);
}

int __wrap_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	long arg;

	va_start(ap, cmd);
	arg = va_arg(ap, long);
	va_end(ap);
	hold_window(cmd);
	return __real_fcntl(fd, cmd, arg);
}

int __wrap_fcntl64(int fd, int cmd, ...)
{
	va_list ap;
	long arg;

	va_start(ap, cmd);
	arg = va_arg(ap, long);
	va_end(ap);
	hold_window(cmd);
	return __real_fcntl64(fd, cmd, arg);
}

static int calls, error;
static unsigned long sum;

/* reads every pixel, as tsm_vte_image_reply would */
static void reply(struct image_host *host, uint64_t id, int err, unsigned int width,
		  unsigned int height, const uint8_t *rgba, void *data)
{
	size_t i;

	calls++;
	error = err;
	for (i = 0; !err && i < (size_t)width * height * 4; i++)
		sum += rgba[i];
}

int main(int argc, char **argv)
{
	char fake[PATH_MAX], name[PATH_MAX + 8], path[PATH_MAX + 16];
	struct image_host_request r = {
		.id = 1,
		.medium = 'f',
		.format = 32,
		.width = 64,
		.height = 64,
		.max_width = 64,
		.max_height = 64,
		.max_bytes = 1 << 20,
	};
	struct ev_eloop *loop;
	struct image_host *h;
	const char *failure;
	int i;

	assert(argc == 3 && realpath(argv[1], fake));
	signal(SIGPIPE, SIG_IGN);
	snprintf(dir, sizeof(dir), "%s/image-host-race-XXXXXX", argv[2]);
	assert(mkdtemp(dir));
	snprintf(path, sizeof(path), "%s/go.fifo", dir);
	assert(mkfifo(path, 0600) == 0);
	snprintf(path, sizeof(path), "%s/ack.fifo", dir);
	assert(mkfifo(path, 0600) == 0);
	snprintf(name, sizeof(name), "race:%s", dir);
	r.name = name;

	assert(ev_eloop_new(&loop) == 0);
	assert(image_host_new(&h, loop, fake, reply, NULL) == 0);
	racing = true;
	assert(image_host_send(h, getuid(), &r) == 0);
	for (i = 0; i < 500 && !calls; i++)
		ev_eloop_dispatch(loop, 10);

	/* refused at the size, read once the seals hold it; no fault */
	failure = image_host_last_failure(h);
	assert(calls == 1 && error == -EBADF && !racing);
	assert(failure && strstr(failure, "not exactly width x height x 4"));

	image_host_free(h);
	ev_eloop_unref(loop);
	snprintf(path, sizeof(path), "%s/go.fifo", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/ack.fifo", dir);
	unlink(path);
	assert(rmdir(dir) == 0);
	printf("test_image_host_race: ok\n");
	return 0;
}
