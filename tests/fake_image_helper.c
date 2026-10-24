/*
 * A stand-in for kmscon-image-helper that answers each request the way its
 * name says (test_image_host.c): a well-formed reply, or one that breaks one
 * of the checks kmscon makes before reading a byte of it. A PNG request gets
 * a 1 x 1 picture, any other min_bytes bytes; an inline request or a discard
 * gets the well-formed reply.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include "image_helper_proto.h"

#define FD KMSCON_IMAGE_FD

/* a memfd of @len bytes of 0x55, sealed with @seals */
static int memfd(size_t len, int seals)
{
	int fd = memfd_create("fake", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	uint8_t *buf = malloc(len ? len : 1);

	memset(buf, 0x55, len);
	if (fd < 0 || write(fd, buf, len) != (ssize_t)len)
		_exit(4);
	free(buf);
	if (seals && fcntl(fd, F_ADD_SEALS, seals) < 0)
		_exit(5);
	return fd;
}

static void send_fds(const void *head, size_t head_len, const void *tail, size_t tail_len,
		     const int *fds, int n_fds, int n_cmsgs)
{
	struct iovec iov[2] = {{.iov_base = (void *)head, .iov_len = head_len},
			       {.iov_base = (void *)tail, .iov_len = tail_len}};
	char cbuf[2 * CMSG_SPACE(2 * sizeof(int))] = {0};
	struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
	struct cmsghdr *cmsg;
	int i;

	if (n_fds) {
		msg.msg_control = cbuf;
		msg.msg_controllen = n_cmsgs == 2 ? 2 * CMSG_SPACE(sizeof(int))
						  : CMSG_SPACE(n_fds * sizeof(int));
		cmsg = CMSG_FIRSTHDR(&msg);
		for (i = 0; i < n_cmsgs; i++) {
			int n = n_cmsgs == 2 ? 1 : n_fds;

			cmsg->cmsg_level = SOL_SOCKET;
			cmsg->cmsg_type = SCM_RIGHTS;
			cmsg->cmsg_len = CMSG_LEN(n * sizeof(int));
			memcpy(CMSG_DATA(cmsg), fds + i, n * sizeof(int));
			cmsg = CMSG_NXTHDR(&msg, cmsg);
		}
	}
	if (sendmsg(FD, &msg, MSG_NOSIGNAL) < 0)
		_exit(6);
}

int main(void)
{
	union {
		struct kmscon_image_request req;
		char buf[sizeof(struct kmscon_image_request) + KMSCON_IMAGE_NAME_MAX + 1];
	} in;
	char cbuf[CMSG_SPACE(sizeof(int))];
	const int seals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
	char name[KMSCON_IMAGE_NAME_MAX + 1];
	struct kmscon_image_reply r;
	int fds[2];
	ssize_t n;

	for (;;) {
		struct iovec iov = {.iov_base = in.buf, .iov_len = sizeof(in.buf)};
		struct msghdr msg = {.msg_iov = &iov,
				     .msg_iovlen = 1,
				     .msg_control = cbuf,
				     .msg_controllen = sizeof(cbuf)};
		const struct kmscon_image_request *q = &in.req;
		bool png, discard;
		size_t len;

		n = recvmsg(FD, &msg, MSG_CMSG_CLOEXEC);
		if (n <= 0)
			return 0;
		png = q->flags & KMSCON_IMAGE_PNG;
		discard = q->flags & KMSCON_IMAGE_DISCARD;
		len = png ? 4 : q->min_bytes;
		memcpy(name, in.buf + sizeof(*q), q->name_len);
		name[q->name_len] = 0;
		if (q->medium == 'd')
			strcpy(name, "good");

		r = (struct kmscon_image_reply){
			.version = KMSCON_IMAGE_VERSION,
			.id = q->id,
			.width = png ? 1 : 0,
			.height = png ? 1 : 0,
		};

		if (discard && strcmp(name, "discardfd")) {
			if (!strcmp(name, "discardignored"))
				r.ignored = 1;
			send_fds(&r, sizeof(r), NULL, 0, NULL, 0, 0);
		} else if (!strcmp(name, "ignoredok") || !strcmp(name, "ignoredmax")) {
			/* read_max bytes ignored, the most a reply may report, or one more */
			r.ignored = q->read_max + !strcmp(name, "ignoredmax");
			fds[0] = memfd(len, seals);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			close(fds[0]);
		} else if (!strcmp(name, "good") || !strcmp(name, "extra")) {
			fds[0] = memfd(len, seals);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			close(fds[0]);
			if (!strcmp(name, "extra")) {
				fds[0] = memfd(len, seals);
				send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
				close(fds[0]);
			}
		} else if (!strcmp(name, "error")) {
			r.status = KMSCON_IMAGE_CORRUPT;
			r.width = r.height = 0;
			r.reason_len = 12;
			send_fds(&r, sizeof(r), "bad\x01\x7f\n data!", 12, NULL, 0, 0);
		} else if (!strcmp(name, "nofd")) {
			send_fds(&r, sizeof(r), NULL, 0, NULL, 0, 0);
		} else if (!strcmp(name, "twofd") || !strcmp(name, "twocmsg")) {
			fds[0] = memfd(len, seals);
			fds[1] = memfd(len, seals);
			send_fds(&r, sizeof(r), NULL, 0, fds, 2, strcmp(name, "twofd") ? 2 : 1);
			close(fds[0]);
			close(fds[1]);
		} else if (!strcmp(name, "errfd") || !strcmp(name, "discardfd")) {
			if (!strcmp(name, "errfd"))
				r.status = KMSCON_IMAGE_UNREADABLE;
			fds[0] = memfd(len, seals);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			close(fds[0]);
		} else if (!strcmp(name, "unsealed") || !strcmp(name, "noshrink") ||
			   !strcmp(name, "nogrow") || !strcmp(name, "nowrite") ||
			   !strcmp(name, "futurewrite") || !strcmp(name, "short") ||
			   !strcmp(name, "long")) {
			int s = !strcmp(name, "unsealed")   ? 0
				: !strcmp(name, "noshrink") ? F_SEAL_GROW | F_SEAL_WRITE
				: !strcmp(name, "nogrow")   ? F_SEAL_SHRINK | F_SEAL_WRITE
				: !strcmp(name, "nowrite")  ? F_SEAL_SHRINK | F_SEAL_GROW
				: !strcmp(name, "futurewrite")
					? F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE
					: seals;

			fds[0] = memfd(!strcmp(name, "short")  ? len - 1
				       : !strcmp(name, "long") ? len + 1
							       : len,
				       s);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			close(fds[0]);
		} else if (!strcmp(name, "pipe")) {
			if (pipe(fds))
				_exit(7);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			close(fds[0]);
			close(fds[1]);
		} else if (!strcmp(name, "wide") || !strcmp(name, "zero") ||
			   !strcmp(name, "budget") || !strcmp(name, "sized")) {
			r.width = !strcmp(name, "wide")	    ? q->max_width + 1
				  : !strcmp(name, "zero")   ? 0
				  : !strcmp(name, "budget") ? q->max_width
							    : 1;
			r.height = !strcmp(name, "budget") ? q->max_height : 1;
			fds[0] = memfd(png ? (size_t)(r.width ? r.width : 1) * r.height * 4 : len,
				       seals);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			close(fds[0]);
		} else if (!strcmp(name, "id") || !strcmp(name, "version") ||
			   !strcmp(name, "ignorederr") || !strcmp(name, "status")) {
			r.status = KMSCON_IMAGE_CORRUPT;
			r.width = r.height = 0;
			if (!strcmp(name, "id"))
				r.id++;
			else if (!strcmp(name, "version"))
				r.version = 1;
			else if (!strcmp(name, "ignorederr"))
				r.ignored = 1;
			else
				r.status = KMSCON_IMAGE_OVER_BUDGET + 1;
			send_fds(&r, sizeof(r), NULL, 0, NULL, 0, 0);
		} else if (!strcmp(name, "truncated")) {
			send_fds(&r, 16, NULL, 0, NULL, 0, 0);
		} else if (!strcmp(name, "reasonlen")) {
			r.status = KMSCON_IMAGE_CORRUPT;
			r.reason_len = 10;
			send_fds(&r, sizeof(r), "short", 5, NULL, 0, 0);
		} else if (!strcmp(name, "reasonmax")) {
			static char reason[256];

			memset(reason, 'r', sizeof(reason));
			r.status = KMSCON_IMAGE_CORRUPT;
			r.reason_len = sizeof(reason);
			send_fds(&r, sizeof(r), reason, sizeof(reason), NULL, 0, 0);
		} else if (!strcmp(name, "huge")) {
			/* 2^31 x 2^31 x 4 is 2^64, 0 in 64 bits; an empty memfd to match */
			r.width = r.height = 0x80000000u;
			fds[0] = memfd(0, seals);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			close(fds[0]);
		} else if (!strncmp(name, "race:", 5)) {
			/*
			 * an unsealed memfd of the right size; when kmscon asks
			 * for its seals (test_image_host_race.c holds it there),
			 * shrunk to one page and only then sealed
			 */
			char go_path[KMSCON_IMAGE_NAME_MAX + 16],
				ack_path[KMSCON_IMAGE_NAME_MAX + 16];
			int go, ack;
			char b;

			if (png) {
				r.width = r.height = 64;
				len = 64 * 64 * 4;
			}
			snprintf(go_path, sizeof(go_path), "%s/go.fifo", name + 5);
			snprintf(ack_path, sizeof(ack_path), "%s/ack.fifo", name + 5);
			fds[0] = memfd(len, 0);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			go = open(go_path, O_RDONLY);
			ack = open(ack_path, O_WRONLY);
			if (go < 0 || ack < 0 || read(go, &b, 1) != 1 || ftruncate(fds[0], 4096) ||
			    fcntl(fds[0], F_ADD_SEALS, seals) < 0 || write(ack, "k", 1) != 1)
				_exit(8);
			close(go);
			close(ack);
			close(fds[0]);
		} else if (!strcmp(name, "lateid") || !strcmp(name, "lateclose")) {
			/*
			 * a good reply, then after a pause either a reply to the
			 * next id, which kmscon has not sent, or an exit
			 */
			fds[0] = memfd(len, seals);
			send_fds(&r, sizeof(r), NULL, 0, fds, 1, 1);
			close(fds[0]);
			usleep(100000);
			if (!strcmp(name, "lateclose"))
				return 0;
			r.id++;
			r.status = KMSCON_IMAGE_TOO_LARGE;
			send_fds(&r, sizeof(r), NULL, 0, NULL, 0, 0);
		} else if (!strcmp(name, "close")) {
			return 0;
		} else if (!strcmp(name, "hang")) {
			pause();
		}
	}
}
