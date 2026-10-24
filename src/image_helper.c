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
 * kmscon closes its end, or on a request it does not understand.
 *
 * Every failure to read answers EBADF, one reply for all of them; the reason
 * travels to kmscon's log. A file is checked before it is opened, since
 * opening can have side effects, and again from the opened descriptor, which
 * catches a symlink swapped between the two. The checks after the open, the
 * refused locations and the temporary-file rule follow Ghostty's file checks
 * (src/terminal/kitty, MIT) as ADR-103's survey describes them.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include <sys/syscall.h>
#include <unistd.h>
#include <zlib.h>
#include "image_helper_proto.h"

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
};

static int fail(struct job *j, int err, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf(j->reason, sizeof(j->reason), fmt, args);
	va_end(args);
	return err;
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
 * Opens @path, under @dirfd and resolved as @resolve says, read-only into the
 * returned descriptor, or returns -errno, without opening anything but a
 * regular file: an O_PATH descriptor comes first, which opens nothing, and
 * only once fstat shows a regular file is the file itself opened through
 * /proc/self/fd. A FIFO or a device is never opened, so a writer waiting on a
 * FIFO is not let go and a device sees no open. @st describes the file opened.
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
	if (fstat(pfd, st) || !S_ISREG(st->st_mode)) {
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
		return fail(j, -EBADF, "cannot resolve the path: %s", strerror(errno));
	if (!location_ok(resolved))
		return fail(j, -EBADF, "refused location %s", resolved);
	if (temp && !temp_ok(resolved))
		return fail(j, -EBADF, "not a temporary file: %s", resolved);

	seam("open");
	*fd = open_regular(AT_FDCWD, resolved, RESOLVE_NO_SYMLINKS, st);
	if (*fd < 0)
		return fail(j, -EBADF, "cannot open %s as a regular file: %s", resolved,
			    strerror(-*fd));
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
	return fail(j, -EBADF, "the file opened for %s is not the file checked", resolved);
}

/*
 * Deletes the checked file @path, which descriptor @st describes: only the
 * entry with its device and inode, in a parent reached with no symlink, so a
 * directory component swapped for a symlink after the check deletes nothing.
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
	else if (unlinkat(dfd, base, 0))
		fail(j, 0, "not deleted: %s: %s", path, strerror(errno));
	close(dfd);
}

/*
 * Opens shared memory object @name read-only into @fd: a leading '/', no other
 * '/', at most SHM_NAME_MAX bytes, and a regular object, opened under /dev/shm
 * as shm_open does, but through open_regular, so a FIFO or a symlink there is
 * never opened. It is then unlinked, by its device and inode as
 * delete_checked does, whatever happens next; the same rename race as
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
		return fail(j, -EBADF, "refused shared memory name");
	if (strlen(name) > SHM_NAME_MAX)
		return fail(j, -EBADF, "shared memory name longer than %d bytes", SHM_NAME_MAX);

	dfd = open(SHM_DIR, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return fail(j, -EBADF, "cannot open " SHM_DIR ": %s", strerror(errno));
	*fd = open_regular(dfd, name + 1, RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS, st);
	if (*fd < 0) {
		fail(j, -EBADF, "cannot open shared memory %s as a regular object: %s", name,
		     strerror(-*fd));
		*fd = -1;
		goto out;
	}
	if (fstatat(dfd, name + 1, &entry, AT_SYMLINK_NOFOLLOW) || entry.st_dev != st->st_dev ||
	    entry.st_ino != st->st_ino || unlinkat(dfd, name + 1, 0))
		fail(j, 0, "shared memory %s not unlinked", name);
out:
	close(dfd);
	return *fd < 0 ? -EBADF : 0;
}

/*
 * Reads @offset to @offset + @size of @fd, @size 0 meaning to the end, into
 * @out, never more than the budget. A file shorter than that fails.
 */
static int read_range(struct job *j, int fd, const struct stat *st, uint64_t offset, uint64_t size,
		      uint8_t **out, size_t *len)
{
	uint64_t file = st->st_size, n, done = 0;
	ssize_t r;

	if (offset > file)
		return fail(j, -EBADF, "offset %llu past the end, %llu bytes",
			    (unsigned long long)offset, (unsigned long long)file);
	n = size ? size : file - offset;
	if (n > file - offset)
		return fail(j, -EBADF, "%llu bytes asked for, %llu there", (unsigned long long)n,
			    (unsigned long long)(file - offset));
	if (n > j->req->max_bytes || n > SIZE_MAX)
		return fail(j, -ENOSPC, "%llu bytes to read, past the budget",
			    (unsigned long long)n);
	*out = malloc(n ? n : 1);
	if (!*out)
		return fail(j, -ENOSPC, "cannot allocate %llu bytes", (unsigned long long)n);
	while (done < n) {
		r = pread(fd, *out + done, n - done, offset + done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			free(*out);
			*out = NULL;
			return fail(j, -EBADF, "short read: %s",
				    r ? strerror(errno) : "end of file");
		}
		done += r;
	}
	*len = n;
	return 0;
}

/* the data the request names, read under the medium's rules */
static int read_data(struct job *j, uint8_t **data, size_t *len)
{
	const struct kmscon_image_request *req = j->req;
	char resolved[PATH_MAX];
	struct stat st;
	int fd = -1, ret;

	switch (req->medium) {
	case 'd':
		if (fstat(j->data_fd, &st))
			return fail(j, -EBADF, "cannot stat the inline data");
		return read_range(j, j->data_fd, &st, 0, 0, data, len);
	case 'f':
	case 't':
		ret = open_file(j, req->medium == 't', NULL, resolved, &fd, &st);
		if (ret)
			return ret;
		ret = read_range(j, fd, &st, req->offset, req->size, data, len);
		if (!ret && req->medium == 't')
			delete_checked(j, resolved, &st);
		close(fd);
		return ret;
	case 's':
		ret = open_shm(j, &fd, &st);
		if (ret)
			return ret;
		ret = read_range(j, fd, &st, req->offset, req->size, data, len);
		close(fd);
		return ret;
	}
	return fail(j, -EBADF, "unknown medium");
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
				fail(j, 0, "discarded %s", resolved);
		} else {
			fail(j, 0, "not deleted, not a temporary file: %s", resolved);
		}
		close(fd);
		break;
	case 's':
		if (!open_shm(j, &fd, &st)) {
			close(fd);
			if (!j->reason[0])
				fail(j, 0, "discarded %s", j->name);
		}
		break;
	default:
		fail(j, 0, "nothing to discard");
		break;
	}
	return -EINVAL;
}

/* zlib data inflated into @out: exactly @exact bytes when not 0, else at most the budget */
static int inflate_data(struct job *j, const uint8_t *in, size_t in_len, uint64_t exact,
			uint8_t **out, size_t *out_len)
{
	uint64_t limit = exact ? exact : j->req->max_bytes;
	size_t size, grown;
	uint8_t *buf, *tmp;
	z_stream zs = {0};
	int zret, ret;

	if (limit > j->req->max_bytes || limit > SIZE_MAX)
		return fail(j, -ENOSPC, "%llu bytes to inflate, past the budget",
			    (unsigned long long)limit);
	size = exact ? exact : (in_len < limit / 4 ? in_len * 4 : limit);
	if (size < 4096 && size < limit)
		size = limit < 4096 ? limit : 4096;
	buf = malloc(size ? size : 1);
	if (!buf)
		return fail(j, -ENOSPC, "cannot allocate %zu bytes", size);
	if (inflateInit(&zs) != Z_OK) {
		free(buf);
		return fail(j, -ENOSPC, "cannot start zlib");
	}
	zs.next_in = (Bytef *)in;
	zs.avail_in = in_len > UINT_MAX ? UINT_MAX : in_len;
	zs.next_out = buf;
	zs.avail_out = size > UINT_MAX ? UINT_MAX : size;
	for (;;) {
		zret = inflate(&zs, Z_NO_FLUSH);
		if (zret == Z_STREAM_END)
			break;
		if (zret != Z_OK && zret != Z_BUF_ERROR) {
			ret = fail(j, -EINVAL, "corrupt zlib data: %s", zs.msg ? zs.msg : "error");
			goto err;
		}
		if (!zs.avail_in) {
			if (in_len > zs.total_in) {
				size_t left = in_len - zs.total_in;

				zs.avail_in = left > UINT_MAX ? UINT_MAX : left;
				continue;
			}
			if (zs.avail_out) {
				ret = fail(j, -EINVAL, "truncated zlib data");
				goto err;
			}
		}
		if (zs.avail_out)
			continue;
		if (zs.total_out < size) {
			size_t left = size - zs.total_out;

			zs.avail_out = left > UINT_MAX ? UINT_MAX : left;
			continue;
		}
		if (size >= limit) {
			if (exact)
				ret = fail(j, -EINVAL, "zlib data inflates past %llu bytes",
					   (unsigned long long)exact);
			else
				ret = fail(j, -ENOSPC, "zlib data inflates past the budget");
			goto err;
		}
		grown = size > limit / 2 ? limit : size * 2;
		tmp = realloc(buf, grown);
		if (!tmp) {
			ret = fail(j, -ENOSPC, "cannot allocate %zu bytes", grown);
			goto err;
		}
		buf = tmp;
		zs.next_out = buf + size;
		zs.avail_out = grown - size > UINT_MAX ? UINT_MAX : grown - size;
		size = grown;
	}
	if (exact && zs.total_out != exact) {
		ret = fail(j, -EINVAL, "zlib data inflates to %lu bytes, not %llu", zs.total_out,
			   (unsigned long long)exact);
		goto err;
	}
	inflateEnd(&zs);
	*out = buf;
	*out_len = zs.total_out;
	return 0;

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
		return fail(j, -ENOSPC, "cannot create a memfd: %s", strerror(errno));
	if (ftruncate(j->out_fd, len))
		goto err;
	*map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, j->out_fd, 0);
	if (*map == MAP_FAILED)
		goto err;
	j->width = w;
	j->height = h;
	return 0;

err:
	fail(j, -ENOSPC, "cannot size the memfd to %zu bytes: %s", len, strerror(errno));
	close(j->out_fd);
	j->out_fd = -1;
	return -ENOSPC;
}

/* unmapped first: F_SEAL_WRITE refuses while a writable shared mapping exists */
static int out_seal(struct job *j, uint8_t *map)
{
	munmap(map, (size_t)j->width * j->height * 4);
	if (fcntl(j->out_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) < 0) {
		close(j->out_fd);
		j->out_fd = -1;
		return fail(j, -ENOSPC, "cannot seal the memfd: %s", strerror(errno));
	}
	return 0;
}

/* @w x @h within max-graphic-size, and its pixels within the budget */
static int check_size(struct job *j, uint64_t w, uint64_t h)
{
	uint64_t bytes;

	if (!w || !h)
		return fail(j, -EINVAL, "no size");
	if (w > j->req->max_width || h > j->req->max_height)
		return fail(j, -EINVAL, "%llux%llu past max-graphic-size", (unsigned long long)w,
			    (unsigned long long)h);
	if (__builtin_mul_overflow(w, h, &bytes) || __builtin_mul_overflow(bytes, 4, &bytes) ||
	    bytes > j->req->max_bytes)
		return fail(j, -ENOSPC, "%llux%llu past the budget", (unsigned long long)w,
			    (unsigned long long)h);
	return 0;
}

/* f=24 or f=32: exactly s x v pixels, 24 expanded with alpha 255 */
static int decode_raw(struct job *j, const uint8_t *data, size_t len)
{
	const struct kmscon_image_request *req = j->req;
	unsigned int bpp = req->format == 24 ? 3 : 4;
	uint64_t i, n = (uint64_t)req->width * req->height;
	uint8_t *map;
	int ret;

	if (len != n * bpp)
		return fail(j, -EINVAL, "%zu bytes of data for %ux%u pixels of f=%u", len,
			    req->width, req->height, req->format);
	ret = out_create(j, req->width, req->height, &map);
	if (ret)
		return ret;
	if (bpp == 4) {
		memcpy(map, data, len);
	} else {
		for (i = 0; i < n; i++) {
			map[i * 4] = data[i * 3];
			map[i * 4 + 1] = data[i * 3 + 1];
			map[i * 4 + 2] = data[i * 3 + 2];
			map[i * 4 + 3] = 255;
		}
	}
	return out_seal(j, map);
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

	fail(src->job, -EINVAL, "corrupt PNG: %s", msg);
	png_longjmp(png, 1);
}

static void png_warn(png_structp png, png_const_charp msg)
{
	(void)png;
	(void)msg;
}

/*
 * PNG to 8-bit RGBA: palette, grey, 16-bit and interlaced images expanded, no
 * gamma or colour-profile correction. The header's size is checked before any
 * pixel is allocated, and the rows are decoded straight into the reply's
 * memfd.
 */
static int decode_png(struct job *j, const uint8_t *data, size_t len)
{
	struct png_src src = {.job = j, .data = data, .len = len};
	png_structp png;
	png_infop info;
	png_uint_32 w, h, y;
	int depth, type, ret;
	uint8_t *m, *volatile map = NULL;
	png_bytep *volatile rows = NULL;

	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &src, png_err, png_warn);
	if (!png)
		return fail(j, -ENOSPC, "cannot start libpng");
	info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		return fail(j, -ENOSPC, "cannot start libpng");
	}
	if (setjmp(png_jmpbuf(png))) {
		ret = -EINVAL;
		goto out;
	}
	png_set_read_fn(png, &src, png_read);
	png_set_user_limits(png, j->req->max_width > 0x7fffffff ? 0x7fffffff : j->req->max_width,
			    j->req->max_height > 0x7fffffff ? 0x7fffffff : j->req->max_height);
	png_set_chunk_malloc_max(png, j->req->max_bytes < 8 << 20 ? j->req->max_bytes : 8 << 20);
	png_read_info(png, info);
	png_get_IHDR(png, info, &w, &h, &depth, &type, NULL, NULL, NULL);
	ret = check_size(j, w, h);
	if (ret)
		goto out;

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
		ret = fail(j, -EINVAL, "PNG rows are not RGBA");
		goto out;
	}

	ret = out_create(j, w, h, &m);
	if (ret)
		goto out;
	map = m;
	rows = malloc(sizeof(*rows) * h);
	if (!rows) {
		ret = fail(j, -ENOSPC, "cannot allocate the rows");
		goto out;
	}
	for (y = 0; y < h; y++)
		rows[y] = map + (size_t)y * w * 4;
	png_read_image(png, rows);
	ret = out_seal(j, map);
	map = NULL;

out:
	if (ret && map) {
		munmap(map, (size_t)j->width * j->height * 4);
		close(j->out_fd);
		j->out_fd = -1;
	}
	free(rows);
	png_destroy_read_struct(&png, &info, NULL);
	return ret;
}

/* reads, inflates and decodes, in ADR-103's order */
static int handle(struct job *j)
{
	const struct kmscon_image_request *req = j->req;
	bool raw = req->format == 24 || req->format == 32;
	uint8_t *data = NULL, *inflated = NULL;
	size_t len = 0, inflated_len = 0;
	uint64_t exact = 0;
	int ret;

	if (req->flags & KMSCON_IMAGE_DISCARD)
		return discard(j);
	if (!raw && req->format != 100)
		return fail(j, -EINVAL, "unknown format %u", req->format);
	if (raw) {
		ret = check_size(j, req->width, req->height);
		if (ret)
			return ret;
	}

	ret = read_data(j, &data, &len);
	if (ret)
		return ret;
	if (req->flags & KMSCON_IMAGE_ZLIB) {
		if (raw)
			exact = (uint64_t)req->width * req->height * (req->format == 24 ? 3 : 4);
		else if (req->medium == 'd')
			exact = req->size;
		ret = inflate_data(j, data, len, exact, &inflated, &inflated_len);
		free(data);
		if (ret)
			return ret;
		data = inflated;
		len = inflated_len;
	}
	ret = raw ? decode_raw(j, data, len) : decode_png(j, data, len);
	free(data);
	return ret;
}

static void reply(const struct job *j, int status)
{
	struct kmscon_image_reply r = {
		.version = KMSCON_IMAGE_VERSION,
		.status = status,
		.id = j->req->id,
		.width = status ? 0 : j->width,
		.height = status ? 0 : j->height,
		.reason_len = strlen(j->reason),
	};
	struct iovec iov[2] = {
		{.iov_base = &r, .iov_len = sizeof(r)},
		{.iov_base = (void *)j->reason, .iov_len = r.reason_len},
	};
	char cbuf[CMSG_SPACE(sizeof(int))] = {0};
	struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
	struct cmsghdr *cmsg;

	if (!status) {
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

		/* a request kmscon would not send ends the helper */
		if ((size_t)n < sizeof(*req) || req->version != KMSCON_IMAGE_VERSION ||
		    req->type != KMSCON_IMAGE_TYPE_KITTY ||
		    (req->flags & ~(KMSCON_IMAGE_ZLIB | KMSCON_IMAGE_DISCARD)))
			return 2;
		if ((size_t)n != sizeof(*req) + req->name_len)
			return 2;
		if (req->medium == 'd') {
			if (req->name_len || n_fds != 1)
				return 2;
		} else if (req->medium == 'f' || req->medium == 't' || req->medium == 's') {
			if (!req->name_len || req->name_len > KMSCON_IMAGE_NAME_MAX || n_fds)
				return 2;
		} else {
			return 2;
		}
		memcpy(name, in.buf + sizeof(*req), req->name_len);
		name[req->name_len] = 0;

		j = (struct job){
			.req = req,
			.name = name,
			.data_fd = n_fds ? fds[0] : -1,
			.out_fd = -1,
		};
		if (strlen(name) != req->name_len)
			status = fail(&j, -EBADF, "a NUL byte in the name");
		else
			status = handle(&j);
		reply(&j, status);
		if (j.out_fd >= 0)
			close(j.out_fd);
		if (j.data_fd >= 0)
			close(j.data_fd);
	}
}
