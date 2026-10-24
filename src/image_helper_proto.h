/*
 * kmscon - The image helper's protocol
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
 * What kmscon and kmscon-image-helper send each other (ADR-103, Notes of
 * 2026-10-24, the helper protocol). The transport is a SOCK_SEQPACKET pair;
 * the helper's end is descriptor 3. A message is one packet: a fixed header in
 * host byte order, both ends being built from one tree, then its variable
 * part, with at most one descriptor.
 */

#ifndef KMSCON_IMAGE_HELPER_PROTO_H
#define KMSCON_IMAGE_HELPER_PROTO_H

#include <stdint.h>

#define KMSCON_IMAGE_VERSION 1
#define KMSCON_IMAGE_TYPE_KITTY 1
/* the helper's end of the socket across exec */
#define KMSCON_IMAGE_FD 3

/* request flags; any other bit makes the helper exit */
#define KMSCON_IMAGE_ZLIB 0x01
#define KMSCON_IMAGE_DISCARD 0x02

#define KMSCON_IMAGE_NAME_MAX 4095
#define KMSCON_IMAGE_REASON_MAX 255

/* kmscon to the helper; name_len bytes of name follow, without a NUL */
struct kmscon_image_request {
	uint32_t version; /* KMSCON_IMAGE_VERSION */
	uint32_t type;	/* KMSCON_IMAGE_TYPE_KITTY; other values reserved, #77's fonts among them */
	uint64_t id;	/* libtsm's request id */
	uint8_t medium; /* 'd', 'f', 't' or 's' */
	uint8_t flags;	/* KMSCON_IMAGE_ZLIB, KMSCON_IMAGE_DISCARD */
	uint16_t format;    /* 24, 32 or 100 */
	uint32_t width;	    /* s, or 0 */
	uint32_t height;    /* v, or 0 */
	uint32_t name_len;  /* 1 to KMSCON_IMAGE_NAME_MAX for 'f', 't' and 's'; 0 for 'd' */
	uint64_t offset;    /* O */
	uint64_t size;	    /* S, or 0 */
	uint32_t max_width; /* max-graphic-size */
	uint32_t max_height;
	uint64_t max_bytes; /* image-storage-limit, the whole budget */
};

/*
 * The helper to kmscon; reason_len bytes follow, why it failed, for kmscon's
 * log. With status 0 the packet carries a sealed memfd of exactly width x
 * height x 4 bytes of straight-alpha RGBA; an error reply carries none.
 */
struct kmscon_image_reply {
	uint32_t version; /* KMSCON_IMAGE_VERSION */
	int32_t status;	  /* 0, -EBADF, -EINVAL or -ENOSPC */
	uint64_t id;	  /* the request's */
	uint32_t width;	  /* with status 0 */
	uint32_t height;
	uint32_t reason_len; /* 0 to KMSCON_IMAGE_REASON_MAX */
	uint32_t reserved;   /* 0 */
};

_Static_assert(sizeof(struct kmscon_image_request) == 64, "the request is 64 bytes");
_Static_assert(sizeof(struct kmscon_image_reply) == 32, "the reply is 32 bytes");

#endif /* KMSCON_IMAGE_HELPER_PROTO_H */
