/*
 * kmscon - Image helper
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
 * kmscon-image-helper (ADR-103, The image helper)
 *
 * kmscon starts one per terminal session, as the session's user, to read the
 * files and shared memory a kitty graphics command names and to decode PNG
 * and zlib data, so that kmscon, which runs as root, does neither. It answers
 * one request at a time on descriptor 3 (image_helper_proto.h) and exits when
 * kmscon closes its end, or on a request kmscon would not build.
 *
 * It is mechanical: libtsm turns the kitty keys into an offset, the most
 * bytes to read, the fewest the image needs and flags, and the helper applies
 * those bounds and answers an outcome, never a kitty reply code. The reason
 * for a failure travels to kmscon's log. A file is checked before it is opened, since
 * opening can have side effects, and again from the opened descriptor, which
 * catches a symlink swapped between the two. The checks after the open, the
 * refused locations and the temporary-file rule follow Ghostty's file checks
 * (src/terminal/kitty, MIT) as ADR-103's survey describes them.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <png.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <zlib.h>
#include "image_helper_proto.h"

/*
 * Magics <linux/magic.h> lacks: configfs's arrived in a header after the
 * 6.8 one Ubuntu's CI builds against, and rpc_pipefs's was never there
 * (the kernel's net/sunrpc/rpc_pipe.c)
 */
#ifndef CONFIGFS_MAGIC
#define CONFIGFS_MAGIC 0x62656570
#endif
#ifndef RPC_PIPEFS_MAGIC
#define RPC_PIPEFS_MAGIC 0x67596969
#endif

#define TEMP_MARK "tty-graphics-protocol"
#define SHM_DIR "/dev/shm"
#define SHM_NAME_MAX 255

/* kmscon's TMPDIR, resolved, or NULL */
static char *tmpdir;

struct job {
	const struct kmscon_image_request *req;
	const char *name; /* NUL-terminated */
	int data_fd;	  /* 'd': the inline data */
	char reason[KMSCON_IMAGE_REASON_MAX + 1];
	/* the result, with status 0 */
	uint32_t width;
	uint32_t height;
	int out_fd;
	uint64_t ignored; /* step 7: the bytes left past what was used */
};

/* @status, with the reason for kmscon's log; KMSCON_IMAGE_OK only adds a note */
static int fail(struct job *j, int status, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf(j->reason, sizeof(j->reason), fmt, args);
	va_end(args);
	return status;
}

/*
 * A point between a check and the step it guards, where the test build lets
 * its test change the file system (tests/test_image_helper.c); nothing in the
 * helper kmscon runs.
 */
static void seam(const char *point)
{
#ifdef IMAGE_HELPER_TEST
	char c;

	if (write(4, point, strlen(point)) > 0 && read(4, &c, 1) < 0)
		_exit(3);
#else
	(void)point;
#endif
}

/* @path is @dir or lies under it; @dir has no trailing slash */
static bool in_dir(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return !strncmp(path, dir, n) && (path[n] == '/' || !path[n]);
}

/* /proc, /sys and /dev other than /dev/shm are refused */
static bool location_ok(const char *path)
{
	return !in_dir(path, "/proc") && !in_dir(path, "/sys") &&
	       (!in_dir(path, "/dev") || in_dir(path, SHM_DIR));
}

/* a temporary file kmscon may read and then delete */
static bool temp_ok(const char *path)
{
	return (in_dir(path, "/tmp") || in_dir(path, SHM_DIR) ||
		(tmpdir && in_dir(path, tmpdir))) &&
	       strstr(path, TEMP_MARK);
}

/* the path the kernel gives the opened @fd */
static bool fd_path(int fd, char *buf, size_t size)
{
	char link[64];
	ssize_t n;

	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	n = readlink(link, buf, size - 1);
	if (n < 0 || (size_t)n >= size - 1)
		return false;
	buf[n] = 0;
	return true;
}

/*
 * @fd lies on a kernel pseudo-filesystem, wherever it is mounted: procfs or
 * sysfs mounted outside /proc and /sys (a chroot's /mnt/proc) escapes the
 * check by path, and reading such files can have effects or leak state
 */
static bool pseudo_fs(int fd)
{
	static const unsigned long magics[] = {
		PROC_SUPER_MAGIC,    SYSFS_MAGIC,	    DEBUGFS_MAGIC,	 TRACEFS_MAGIC,
		SECURITYFS_MAGIC,    CGROUP_SUPER_MAGIC,    CGROUP2_SUPER_MAGIC, DEVPTS_SUPER_MAGIC,
		CONFIGFS_MAGIC,	     BPF_FS_MAGIC,	    PSTOREFS_MAGIC,	 EFIVARFS_MAGIC,
		SELINUX_MAGIC,	     SMACK_MAGIC,	    BINFMTFS_MAGIC,	 NSFS_MAGIC,
		ANON_INODE_FS_MAGIC, USBDEVICE_SUPER_MAGIC, RPC_PIPEFS_MAGIC,
	};
	struct statfs fs;
	size_t i;

	if (fstatfs(fd, &fs))
		return true;
	for (i = 0; i < sizeof(magics) / sizeof(magics[0]); i++)
		if ((unsigned long)fs.f_type == magics[i])
			return true;
	return false;
}

/*
 * Opens @path, under @dirfd and resolved as @resolve says, read-only into the
 * returned descriptor, or returns -errno, without opening anything but a
 * regular file: an O_PATH descriptor comes first, which opens nothing, and
 * only once fstat shows a regular file, on no kernel pseudo-filesystem, is
 * the file itself opened through /proc/self/fd. A FIFO or a device is never
 * opened, so a writer waiting on a FIFO is not let go and a device sees no
 * open. @st describes the file opened.
 */
static int open_regular(int dirfd, const char *path, uint64_t resolve, struct stat *st)
{
	struct open_how how = {
		.flags = O_PATH | O_NOFOLLOW | O_CLOEXEC,
		.resolve = resolve,
	};
	struct stat opened;
	char proc[64];
	int pfd, fd;

	pfd = syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
	if (pfd < 0)
		return -errno;
	if (fstat(pfd, st) || !S_ISREG(st->st_mode) || pseudo_fs(pfd)) {
		close(pfd);
		return -EINVAL;
	}
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", pfd);
	fd = open(proc, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	close(pfd);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &opened) || !S_ISREG(opened.st_mode) || opened.st_dev != st->st_dev ||
	    opened.st_ino != st->st_ino) {
		close(fd);
		return -EINVAL;
	}
	*st = opened;
	return fd;
}

/*
 * Opens a t=f or t=t path read-only into @fd: resolved, following symlinks
 * and failing on loops, refused in /proc, /sys and /dev, then opened by
 * open_regular with no symlink allowed on the resolved path, and checked
 * again from the opened descriptor, whose own path must pass the same checks.
 * A symlink swapped into the resolved path between the check and the open
 * fails the open. @temp also applies the temporary-file rule to both paths;
 * @temp_too, when given, reports whether both pass it instead. The resolved
 * path is left in @resolved, PATH_MAX bytes.
 */
static int open_file(struct job *j, bool temp, bool *temp_too, char *resolved, int *fd,
		     struct stat *st)
{
	char opened[PATH_MAX];
	bool t;

	if (!realpath(j->name, resolved))
		return fail(j, KMSCON_IMAGE_UNREADABLE, "cannot resolve the path: %s",
			    strerror(errno));
	if (!location_ok(resolved))
		return fail(j, KMSCON_IMAGE_UNREADABLE, "refused location %s", resolved);
	if (temp && !temp_ok(resolved))
		return fail(j, KMSCON_IMAGE_UNREADABLE, "not a temporary file: %s", resolved);

	seam("open");
	*fd = open_regular(AT_FDCWD, resolved, RESOLVE_NO_SYMLINKS, st);
	if (*fd < 0)
		return fail(j, KMSCON_IMAGE_UNREADABLE, "cannot open %s as a regular file: %s",
			    resolved, strerror(-*fd));
	if (!fd_path(*fd, opened, sizeof(opened)) || !location_ok(opened))
		goto refuse;
	t = temp_ok(resolved) && temp_ok(opened);
	if (temp && !t)
		goto refuse;
	if (temp_too)
		*temp_too = t;
	return 0;

refuse:
	close(*fd);
	*fd = -1;
	return fail(j, KMSCON_IMAGE_UNREADABLE, "the file opened for %s is not the file checked",
		    resolved);
}

/*
 * Deletes the checked file @path, which descriptor @st describes: only the
 * entry with its device and inode, in a parent reached with no symlink, so a
 * directory component swapped for a symlink after the check deletes nothing,
 * and only when the helper's own uid owns it, which gives a root helper the
 * sticky bit's rule in /tmp and /dev/shm.
 * A failure leaves the file and only adds to the reason.
 *
 * Between fstatat and unlinkat the entry can still be renamed away and
 * another file put in its place; unlinkat then removes that file. The race
 * needs write access to the directory the checked file is in, which a
 * temporary file's directory gives only to users who could delete the entry
 * themselves, and the parent was reached with no symlink, so it cannot lead
 * outside that directory.
 */
static void delete_checked(struct job *j, const char *path, const struct stat *st)
{
	struct open_how how = {
		.flags = O_PATH | O_DIRECTORY | O_CLOEXEC,
		.resolve = RESOLVE_NO_SYMLINKS,
	};
	char dir[PATH_MAX];
	const char *base;
	struct stat entry;
	int dfd;

	base = strrchr(path, '/');
	if (!base || !base[1] || (size_t)(base - path) >= sizeof(dir)) {
		fail(j, 0, "not deleted: no file name in %s", path);
		return;
	}
	memcpy(dir, path, base - path);
	dir[base - path] = 0;
	if (!dir[0])
		strcpy(dir, "/");
	base++;

	seam("unlink");
	dfd = syscall(SYS_openat2, AT_FDCWD, dir, &how, sizeof(how));
	if (dfd < 0) {
		fail(j, 0, "not deleted: cannot open %s without symlinks: %s", dir,
		     strerror(errno));
		return;
	}
	if (fstatat(dfd, base, &entry, AT_SYMLINK_NOFOLLOW) || entry.st_dev != st->st_dev ||
	    entry.st_ino != st->st_ino)
		fail(j, 0, "not deleted: %s is no longer the file read", path);
	else if (entry.st_uid != geteuid())
		fail(j, 0, "not deleted: %s belongs to uid %u", path, entry.st_uid);
	else if (unlinkat(dfd, base, 0))
		fail(j, 0, "not deleted: %s: %s", path, strerror(errno));
	close(dfd);
}

/*
 * Opens shared memory object @name read-only into @fd: a leading '/', no other
 * '/', at most SHM_NAME_MAX bytes, and a regular object, opened under /dev/shm
 * as shm_open does, but through open_regular, so a FIFO or a symlink there is
 * never opened. It is then unlinked, by its device and inode and only when
 * the helper's uid owns it, as delete_checked does, whatever happens next; the same rename race as
 * delete_checked's applies, bounded by /dev/shm's sticky bit to objects the
 * user could unlink anyway.
 */
static int open_shm(struct job *j, int *fd, struct stat *st)
{
	const char *name = j->name;
	struct stat entry;
	int dfd;

	if (name[0] != '/' || strchr(name + 1, '/') || !name[1] || !strcmp(name, "/.") ||
	    !strcmp(name, "/.."))
		return fail(j, KMSCON_IMAGE_UNREADABLE, "refused shared memory name");
	if (strlen(name) > SHM_NAME_MAX)
		return fail(j, KMSCON_IMAGE_UNREADABLE, "shared memory name longer than %d bytes",
			    SHM_NAME_MAX);

	dfd = open(SHM_DIR, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return fail(j, KMSCON_IMAGE_UNREADABLE, "cannot open " SHM_DIR ": %s",
			    strerror(errno));
	*fd = open_regular(dfd, name + 1, RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS, st);
	if (*fd < 0) {
		fail(j, KMSCON_IMAGE_UNREADABLE,
		     "cannot open shared memory %s as a regular object: %s", name, strerror(-*fd));
		*fd = -1;
		goto out;
	}
	if (fstatat(dfd, name + 1, &entry, AT_SYMLINK_NOFOLLOW) || entry.st_dev != st->st_dev ||
	    entry.st_ino != st->st_ino || entry.st_uid != geteuid() || unlinkat(dfd, name + 1, 0))
		fail(j, 0, "shared memory %s not unlinked", name);
out:
	close(dfd);
	return *fd < 0 ? KMSCON_IMAGE_UNREADABLE : KMSCON_IMAGE_OK;
}

/* the @n bytes at @offset of @fd into a new buffer at @out */
static int read_bytes(struct job *j, int fd, uint64_t offset, uint64_t n, uint8_t **out)
{
	uint64_t done = 0;
	ssize_t r;

	*out = malloc(n ? n : 1);
	if (!*out)
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot allocate %llu bytes",
			    (unsigned long long)n);
	while (done < n) {
		r = pread(fd, *out + done, n - done, offset + done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			free(*out);
			*out = NULL;
			return fail(j, KMSCON_IMAGE_UNREADABLE, "short read: %s",
				    r ? strerror(errno) : "end of file");
		}
		done += r;
	}
	return KMSCON_IMAGE_OK;
}

/*
 * Step 3: from offset, up to read_max bytes or to the end when it is 0. An
 * offset at or past the end of a file or shared memory object is UNREADABLE.
 * Without INFLATE or PNG only min_bytes are read, and fewer available is
 * TOO_SHORT before anything is read, so a large file costs nothing past
 * them. With either, all the bounds allow is read, and more than max_bytes is
 * OVER_BUDGET before it is read.
 */
static int read_bounded(struct job *j, int fd, const struct stat *st, uint8_t **out, size_t *len)
{
	const struct kmscon_image_request *req = j->req;
	uint64_t size = st->st_size, avail;
	int ret;

	if (req->medium != 'd' && req->offset >= size)
		return fail(j, KMSCON_IMAGE_UNREADABLE,
			    "offset %llu at or past the end, %llu bytes",
			    (unsigned long long)req->offset, (unsigned long long)size);
	avail = req->offset < size ? size - req->offset : 0;
	if (req->read_max && req->read_max < avail)
		avail = req->read_max;
	if (!(req->flags & (KMSCON_IMAGE_INFLATE | KMSCON_IMAGE_PNG))) {
		if (avail < req->min_bytes)
			return fail(j, KMSCON_IMAGE_TOO_SHORT, "%llu bytes to read, %llu needed",
				    (unsigned long long)avail, (unsigned long long)req->min_bytes);
		/* step 7: counted from the bounds and fstat, never read */
		j->ignored = avail - req->min_bytes;
		avail = req->min_bytes;
	} else if (avail > req->max_bytes || avail > SIZE_MAX) {
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "%llu bytes to read, past the budget",
			    (unsigned long long)avail);
	}
	ret = read_bytes(j, fd, req->offset, avail, out);
	if (!ret)
		*len = avail;
	return ret;
}

/*
 * Steps 1 and 2: the source opened into @fd. A 't' file that passed the
 * temporary-file rule is deleted once opened, and a 's' object unlinked, so
 * both go whatever the steps after this make of them.
 */
static int open_source(struct job *j, int *fd, struct stat *st)
{
	char resolved[PATH_MAX];
	int ret;

	switch (j->req->medium) {
	case 'd':
		*fd = fcntl(j->data_fd, F_DUPFD_CLOEXEC, 0);
		if (*fd < 0 || fstat(*fd, st)) {
			if (*fd >= 0)
				close(*fd);
			*fd = -1;
			return fail(j, KMSCON_IMAGE_UNREADABLE, "cannot read the inline data");
		}
		return KMSCON_IMAGE_OK;
	case 'f':
	case 't':
		ret = open_file(j, j->req->medium == 't', NULL, resolved, fd, st);
		if (!ret && j->req->medium == 't')
			delete_checked(j, resolved, st);
		return ret;
	case 's':
		return open_shm(j, fd, st);
	}
	return fail(j, KMSCON_IMAGE_UNREADABLE, "unknown medium");
}

/* a refused t=t or t=s command's object, removed as the rules allow; nothing is read */
static int discard(struct job *j)
{
	char resolved[PATH_MAX];
	struct stat st;
	bool temp;
	int fd = -1;

	switch (j->req->medium) {
	case 't':
		if (open_file(j, false, &temp, resolved, &fd, &st))
			break;
		if (temp) {
			delete_checked(j, resolved, &st);
			if (!j->reason[0])
				fail(j, KMSCON_IMAGE_OK, "discarded %s", resolved);
		} else {
			fail(j, KMSCON_IMAGE_OK, "not deleted, not a temporary file: %s", resolved);
		}
		close(fd);
		break;
	case 's':
		if (!open_shm(j, &fd, &st)) {
			close(fd);
			if (!j->reason[0])
				fail(j, KMSCON_IMAGE_OK, "discarded %s", j->name);
		}
		break;
	}
	return KMSCON_IMAGE_OK;
}

/* the next of @in's @in_len bytes into @zs once it has used those it had */
static void inflate_feed(z_stream *zs, size_t in_len)
{
	size_t left = in_len - zs->total_in;

	if (!zs->avail_in && left)
		zs->avail_in = left > UINT_MAX ? UINT_MAX : left;
}

/* a zlib return that ends inflating: the stream does not decode */
static int inflate_failed(struct job *j, z_stream *zs, int zret)
{
	if (zret == Z_MEM_ERROR)
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "zlib: out of memory");
	if (zret == Z_BUF_ERROR)
		return fail(j, KMSCON_IMAGE_CORRUPT, "truncated zlib data");
	return fail(j, KMSCON_IMAGE_CORRUPT, "corrupt zlib data: %s", zs->msg ? zs->msg : "error");
}

/*
 * Step 4: @in inflated into @out. Without PNG or EXACT it stops once
 * min_bytes are out and ignores the rest. Under EXACT a byte past min_bytes
 * is CORRUPT, found before the max_bytes bound. Otherwise a byte past
 * max_bytes is OVER_BUDGET. A stream that does not inflate, or ends before
 * its end is reached, is CORRUPT; fewer than min_bytes out is TOO_SHORT.
 * @rest is how many bytes of @in the inflater did not consume: those past the
 * stream's end, its trailer counted in the stream, or, where it stopped at
 * min_bytes, everything past the last byte fed to it.
 */
static int inflate_bounded(struct job *j, const uint8_t *in, size_t in_len, uint8_t **out,
			   size_t *out_len, size_t *rest)
{
	const struct kmscon_image_request *req = j->req;
	bool exact = req->flags & KMSCON_IMAGE_EXACT;
	bool stop = !exact && !(req->flags & KMSCON_IMAGE_PNG);
	uint64_t limit = exact || stop ? req->min_bytes : req->max_bytes;
	size_t size, grown;
	uint8_t *buf, *tmp, probe;
	z_stream zs = {0};
	int zret, ret;

	if (limit > SIZE_MAX)
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "%llu bytes to inflate, past memory",
			    (unsigned long long)limit);
	size = in_len < limit / 4 ? in_len * 4 : limit;
	if (size < 4096)
		size = limit < 4096 ? limit : 4096;
	buf = malloc(size ? size : 1);
	if (!buf)
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot allocate %zu bytes", size);
	if (inflateInit(&zs) != Z_OK) {
		free(buf);
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot start zlib");
	}
	*rest = 0;
	zs.next_in = (Bytef *)in;
	zs.next_out = buf;

	/* until the stream ends or limit bytes are out */
	for (;;) {
		inflate_feed(&zs, in_len);
		if (!zs.avail_out) {
			if (zs.total_out == limit)
				break;
			if (zs.total_out == size) {
				grown = size > limit / 2 ? limit : size * 2;
				tmp = realloc(buf, grown);
				if (!tmp) {
					ret = fail(j, KMSCON_IMAGE_OVER_BUDGET,
						   "cannot allocate %zu bytes", grown);
					goto err;
				}
				buf = tmp;
				size = grown;
			}
			zs.next_out = buf + zs.total_out;
			zs.avail_out =
				size - zs.total_out > UINT_MAX ? UINT_MAX : size - zs.total_out;
		}
		zret = inflate(&zs, Z_NO_FLUSH);
		if (zret == Z_STREAM_END)
			goto end;
		if (zret != Z_OK && (zret != Z_BUF_ERROR || (!zs.avail_in && zs.avail_out))) {
			ret = inflate_failed(j, &zs, zret);
			goto err;
		}
	}

	/*
	 * limit bytes out and the stream not ended: past them it is ignored,
	 * though the stream's end, its trailer among it, may follow with no
	 * more output, which one call with no room consumes; or one byte more
	 * counts
	 */
	if (stop) {
		inflate_feed(&zs, in_len);
		inflate(&zs, Z_NO_FLUSH);
		*rest = in_len - zs.total_in;
		goto done;
	}
	zs.next_out = &probe;
	zs.avail_out = 1;
	for (;;) {
		inflate_feed(&zs, in_len);
		zret = inflate(&zs, Z_NO_FLUSH);
		if (!zs.avail_out) {
			if (exact)
				ret = fail(j, KMSCON_IMAGE_CORRUPT,
					   "zlib data inflates past %llu bytes",
					   (unsigned long long)limit);
			else
				ret = fail(j, KMSCON_IMAGE_OVER_BUDGET,
					   "zlib data inflates past the budget");
			goto err;
		}
		if (zret == Z_STREAM_END)
			goto end;
		if (zret != Z_OK && (zret != Z_BUF_ERROR || !zs.avail_in)) {
			ret = inflate_failed(j, &zs, zret);
			goto err;
		}
	}

end:
	*rest = in_len - zs.total_in;
done:
	if (zs.total_out < req->min_bytes) {
		ret = fail(j, KMSCON_IMAGE_TOO_SHORT,
			   "zlib data inflates to %lu bytes, %llu needed", zs.total_out,
			   (unsigned long long)req->min_bytes);
		goto err;
	}
	*out_len = zs.total_out;
	*out = buf;
	inflateEnd(&zs);
	return KMSCON_IMAGE_OK;

err:
	inflateEnd(&zs);
	free(buf);
	return ret;
}

/* the reply's memfd, @w x @h x 4 bytes, mapped writable into @map */
static int out_create(struct job *j, uint32_t w, uint32_t h, uint8_t **map)
{
	size_t len = (size_t)w * h * 4;

	j->out_fd = memfd_create("kmscon-image", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (j->out_fd < 0)
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot create a memfd: %s",
			    strerror(errno));
	if (ftruncate(j->out_fd, len))
		goto err;
	*map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, j->out_fd, 0);
	if (*map == MAP_FAILED)
		goto err;
	j->width = w;
	j->height = h;
	return KMSCON_IMAGE_OK;

err:
	fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot size the memfd to %zu bytes: %s", len,
	     strerror(errno));
	close(j->out_fd);
	j->out_fd = -1;
	return KMSCON_IMAGE_OVER_BUDGET;
}

/* sealed once nothing maps it writable: F_SEAL_WRITE refuses while a writable shared mapping exists
 */
static int out_seal(struct job *j)
{
	if (fcntl(j->out_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) < 0) {
		close(j->out_fd);
		j->out_fd = -1;
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot seal the memfd: %s",
			    strerror(errno));
	}
	return KMSCON_IMAGE_OK;
}

/* step 6: exactly the first min_bytes bytes, in a memfd of that length */
static int out_bytes(struct job *j, const uint8_t *data)
{
	uint64_t n = j->req->min_bytes, done = 0;
	ssize_t r;

	j->out_fd = memfd_create("kmscon-image", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (j->out_fd < 0)
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot create a memfd: %s",
			    strerror(errno));
	while (done < n) {
		r = write(j->out_fd, data + done, n - done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			close(j->out_fd);
			j->out_fd = -1;
			return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot write the memfd: %s",
				    r ? strerror(errno) : "no space");
		}
		done += r;
	}
	return out_seal(j);
}

struct png_src {
	struct job *job;
	const uint8_t *data;
	size_t len;
	size_t off;
};

static void png_read(png_structp png, png_bytep out, size_t n)
{
	struct png_src *src = png_get_io_ptr(png);

	if (src->len - src->off < n)
		png_error(png, "truncated");
	memcpy(out, src->data + src->off, n);
	src->off += n;
}

static void png_err(png_structp png, png_const_charp msg)
{
	struct png_src *src = png_get_error_ptr(png);

	fail(src->job, KMSCON_IMAGE_CORRUPT, "corrupt PNG: %s", msg);
	png_longjmp(png, 1);
}

static void png_warn(png_structp png, png_const_charp msg)
{
	(void)png;
	(void)msg;
}

/*
 * Step 5: PNG to 8-bit RGBA, palette, grey, 16-bit and interlaced images
 * expanded, no gamma or colour-profile correction. The header's size is
 * checked before any pixel is allocated: past max_width or max_height is
 * TOO_LARGE, and width x height x 4 past max_bytes OVER_BUDGET. libpng's own
 * size limits are set to the format's, so that this check decides. The rows
 * are decoded straight into the reply's memfd. @rest is how many bytes follow
 * the IEND chunk, or 0 when the stream ends without one.
 */
static int decode_png(struct job *j, const uint8_t *data, size_t len, size_t *rest)
{
	const struct kmscon_image_request *req = j->req;
	struct png_src src = {.job = j, .data = data, .len = len};
	png_structp png;
	png_infop info;
	png_uint_32 w, h, y;
	uint64_t bytes;
	int depth, type, ret;
	uint8_t *m, *volatile map = NULL;
	png_bytep *volatile rows = NULL;
	volatile bool decoded = false;

	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &src, png_err, png_warn);
	if (!png)
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot start libpng");
	info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		return fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot start libpng");
	}
	*rest = 0;
	if (setjmp(png_jmpbuf(png))) {
		/* a stream that ends without IEND after its image costs the image nothing */
		if (decoded)
			ret = fail(j, KMSCON_IMAGE_OK, "PNG without IEND after its image");
		else
			ret = KMSCON_IMAGE_CORRUPT;
		goto out;
	}
	png_set_read_fn(png, &src, png_read);
	png_set_user_limits(png, PNG_UINT_31_MAX, PNG_UINT_31_MAX);
	png_set_chunk_malloc_max(png, req->max_bytes < 8 << 20 ? req->max_bytes : 8 << 20);
	png_read_info(png, info);
	png_get_IHDR(png, info, &w, &h, &depth, &type, NULL, NULL, NULL);
	if (w > req->max_width || h > req->max_height) {
		ret = fail(j, KMSCON_IMAGE_TOO_LARGE, "%ux%u past max-graphic-size", w, h);
		goto out;
	}
	if (__builtin_mul_overflow((uint64_t)w, h, &bytes) ||
	    __builtin_mul_overflow(bytes, 4, &bytes) || bytes > req->max_bytes) {
		ret = fail(j, KMSCON_IMAGE_OVER_BUDGET, "%ux%u past the budget", w, h);
		goto out;
	}

	if (type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (type == PNG_COLOR_TYPE_GRAY && depth < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	else if (!(type & PNG_COLOR_MASK_ALPHA))
		png_set_filler(png, 0xff, PNG_FILLER_AFTER);
	if (depth == 16)
		png_set_strip_16(png);
	if (!(type & PNG_COLOR_MASK_COLOR))
		png_set_gray_to_rgb(png);
	png_set_interlace_handling(png);
	png_read_update_info(png, info);
	if (png_get_rowbytes(png, info) != (size_t)w * 4) {
		ret = fail(j, KMSCON_IMAGE_CORRUPT, "PNG rows are not RGBA");
		goto out;
	}

	ret = out_create(j, w, h, &m);
	if (ret)
		goto out;
	map = m;
	rows = malloc(sizeof(*rows) * h);
	if (!rows) {
		ret = fail(j, KMSCON_IMAGE_OVER_BUDGET, "cannot allocate the rows");
		goto out;
	}
	for (y = 0; y < h; y++)
		rows[y] = map + (size_t)y * w * 4;
	png_read_image(png, rows);
	munmap(map, (size_t)w * h * 4);
	map = NULL;
	ret = out_seal(j);
	if (ret)
		goto out;
	/* step 7: the image is done; what follows IEND is counted */
	decoded = true;
	png_read_end(png, NULL);
	*rest = src.len - src.off;

out:
	if (ret && map) {
		munmap(map, (size_t)j->width * j->height * 4);
		close(j->out_fd);
		j->out_fd = -1;
	}
	if (ret)
		j->width = j->height = 0;
	free(rows);
	png_destroy_read_struct(&png, &info, NULL);
	return ret;
}

/* the steps of ADR-103's "What the helper does", in order, knowing nothing of the kitty keys */
static int handle(struct job *j)
{
	const struct kmscon_image_request *req = j->req;
	uint8_t *data = NULL, *inflated = NULL;
	size_t len = 0, inflated_len = 0, rest = 0, png_rest = 0;
	struct stat st;
	int fd, ret;

	if (req->flags & KMSCON_IMAGE_DISCARD)
		return discard(j);

	ret = open_source(j, &fd, &st);
	if (ret)
		return ret;
	ret = read_bounded(j, fd, &st, &data, &len);
	close(fd);
	if (ret)
		return ret;
	if (req->flags & KMSCON_IMAGE_INFLATE) {
		ret = inflate_bounded(j, data, len, &inflated, &inflated_len, &rest);
		free(data);
		if (ret)
			return ret;
		data = inflated;
		len = inflated_len;
	}
	ret = req->flags & KMSCON_IMAGE_PNG ? decode_png(j, data, len, &png_rest)
					    : out_bytes(j, data);
	free(data);
	/*
	 * step 7, in bytes read from the source, never decoded bytes: with
	 * INFLATE those the inflater did not consume, IEND unconsulted, so the
	 * count stays within read_max; with PNG alone those past IEND
	 */
	if (req->flags & KMSCON_IMAGE_INFLATE)
		j->ignored = rest;
	else if (req->flags & KMSCON_IMAGE_PNG)
		j->ignored = png_rest;
	return ret;
}

static void reply(const struct job *j, uint32_t status)
{
	bool fd = status == KMSCON_IMAGE_OK && j->out_fd >= 0;
	struct kmscon_image_reply r = {
		.version = KMSCON_IMAGE_VERSION,
		.status = status,
		.id = j->req->id,
		.width = fd ? j->width : 0,
		.height = fd ? j->height : 0,
		.reason_len = strlen(j->reason),
		.ignored = fd ? (j->ignored > UINT32_MAX ? UINT32_MAX : j->ignored) : 0,
	};
	struct iovec iov[2] = {
		{.iov_base = &r, .iov_len = sizeof(r)},
		{.iov_base = (void *)j->reason, .iov_len = r.reason_len},
	};
	char cbuf[CMSG_SPACE(sizeof(int))] = {0};
	struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
	struct cmsghdr *cmsg;

	if (fd) {
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &j->out_fd, sizeof(int));
	}
	while (sendmsg(KMSCON_IMAGE_FD, &msg, MSG_NOSIGNAL) < 0)
		if (errno != EINTR)
			_exit(1);
}

/*
 * A request kmscon would not build, on which the helper exits (ADR-103, The
 * outcomes): a length that is not the header and its name, a version, type,
 * flag bit or reserved value it does not know, a medium with the wrong name
 * or descriptor, min_bytes 0 without PNG or DISCARD, EXACT without INFLATE, a
 * DISCARD with another flag, with medium 'f' or 'd' or with a non-zero bound,
 * or min_bytes past max_bytes.
 */
static bool request_valid(const struct kmscon_image_request *req, size_t n, int n_fds)
{
	if (n < sizeof(*req) || req->version != KMSCON_IMAGE_VERSION ||
	    req->type != KMSCON_IMAGE_TYPE_IMAGE || (req->flags & ~KMSCON_IMAGE_FLAGS) ||
	    req->reserved || n != sizeof(*req) + req->name_len)
		return false;
	if (req->medium == 'd') {
		if (req->name_len || n_fds != 1)
			return false;
	} else if (req->medium == 'f' || req->medium == 't' || req->medium == 's') {
		if (!req->name_len || req->name_len > KMSCON_IMAGE_NAME_MAX || n_fds)
			return false;
	} else {
		return false;
	}
	if (!req->min_bytes && !(req->flags & (KMSCON_IMAGE_PNG | KMSCON_IMAGE_DISCARD)))
		return false;
	if ((req->flags & KMSCON_IMAGE_EXACT) && !(req->flags & KMSCON_IMAGE_INFLATE))
		return false;
	if ((req->flags & KMSCON_IMAGE_DISCARD) &&
	    (req->flags != KMSCON_IMAGE_DISCARD || req->medium == 'f' || req->medium == 'd' ||
	     req->offset || req->read_max || req->min_bytes))
		return false;
	return req->min_bytes <= req->max_bytes;
}

int main(void)
{
	union {
		struct kmscon_image_request req;
		char buf[sizeof(struct kmscon_image_request) + KMSCON_IMAGE_NAME_MAX + 1];
	} in;
	char cbuf[CMSG_SPACE(4 * sizeof(int))];
	char name[KMSCON_IMAGE_NAME_MAX + 1];
	const struct kmscon_image_request *req = &in.req;
	struct iovec iov = {.iov_base = in.buf, .iov_len = sizeof(in.buf)};
	struct msghdr msg;
	struct cmsghdr *cmsg;
	int fds[4], n_fds, i, status;
	const char *env;
	struct job j;
	ssize_t n;

	env = getenv("TMPDIR");
	if (env && env[0] == '/')
		tmpdir = realpath(env, NULL);
	if (tmpdir && !strcmp(tmpdir, "/")) {
		free(tmpdir);
		tmpdir = NULL;
	}

	for (;;) {
		msg = (struct msghdr){
			.msg_iov = &iov,
			.msg_iovlen = 1,
			.msg_control = cbuf,
			.msg_controllen = sizeof(cbuf),
		};
		n = recvmsg(KMSCON_IMAGE_FD, &msg, MSG_CMSG_CLOEXEC);
		if (n < 0 && errno == EINTR)
			continue;
		if (n == 0)
			return 0;
		if (n < 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
			return 1;

		n_fds = 0;
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
				return 1;
			for (i = 0; (size_t)i < (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) {
				if (n_fds == 4)
					return 1;
				memcpy(&fds[n_fds++], CMSG_DATA(cmsg) + i * sizeof(int),
				       sizeof(int));
			}
		}

		if (!request_valid(req, n, n_fds))
			return 2;
		memcpy(name, in.buf + sizeof(*req), req->name_len);
		name[req->name_len] = 0;

		j = (struct job){
			.req = req,
			.name = name,
			.data_fd = n_fds ? fds[0] : -1,
			.out_fd = -1,
		};
		if (strlen(name) != req->name_len)
			status = fail(&j, KMSCON_IMAGE_UNREADABLE, "a NUL byte in the name");
		else
			status = handle(&j);
		reply(&j, status);
		if (j.out_fd >= 0)
			close(j.out_fd);
		if (j.data_fd >= 0)
			close(j.data_fd);
	}
}
