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

#include <stddef.h>
#include <stdint.h>

#define KMSCON_IMAGE_VERSION 2
/* the request's type; other values are reserved, #77's fonts among them */
#define KMSCON_IMAGE_TYPE_IMAGE 1
/* the helper's end of the socket across exec */
#define KMSCON_IMAGE_FD 3

/*
 * Request flags, libtsm's TSM_VTE_IMAGE_* values; any other bit makes the
 * helper exit. The helper applies them as bounds and never reads a kitty key.
 */
#define KMSCON_IMAGE_INFLATE 0x01 /* zlib-inflate the bytes read first */
#define KMSCON_IMAGE_PNG 0x02	  /* decode them as PNG; without it, return min_bytes of them */
#define KMSCON_IMAGE_EXACT 0x04	  /* the inflated bytes must number exactly min_bytes */
#define KMSCON_IMAGE_DISCARD 0x08 /* remove a 't' or 's' object, read nothing */
#define KMSCON_IMAGE_FLAGS                                                                         \
	(KMSCON_IMAGE_INFLATE | KMSCON_IMAGE_PNG | KMSCON_IMAGE_EXACT | KMSCON_IMAGE_DISCARD)

/* the outcomes, libtsm's tsm_vte_image_status values */
#define KMSCON_IMAGE_OK 0
#define KMSCON_IMAGE_UNREADABLE                                                                    \
	1			 /* not opened, a refused path or name, the offset at or past      \
				    the end, or a read error */
#define KMSCON_IMAGE_TOO_SHORT 2 /* fewer bytes than min_bytes, read or inflated */
#define KMSCON_IMAGE_CORRUPT                                                                       \
	3			   /* zlib or PNG data that does not decode, or more than          \
				      min_bytes inflated under KMSCON_IMAGE_EXACT */
#define KMSCON_IMAGE_TOO_LARGE 4   /* PNG dimensions past max_width or max_height */
#define KMSCON_IMAGE_OVER_BUDGET 5 /* read, inflated or decoded past max_bytes */

#define KMSCON_IMAGE_NAME_MAX 4095
#define KMSCON_IMAGE_REASON_MAX 255

/* kmscon to the helper; name_len bytes of name follow, without a NUL */
struct kmscon_image_request {
	uint32_t version;   /* KMSCON_IMAGE_VERSION */
	uint32_t type;	    /* KMSCON_IMAGE_TYPE_IMAGE */
	uint64_t id;	    /* libtsm's request id */
	uint8_t medium;	    /* 'd', 'f', 't' or 's' */
	uint8_t flags;	    /* KMSCON_IMAGE_FLAGS */
	uint16_t reserved;  /* 0, or the helper exits */
	uint32_t name_len;  /* 1 to KMSCON_IMAGE_NAME_MAX for 'f', 't' and 's'; 0 for 'd' */
	uint64_t offset;    /* where the read starts; 0 for 'd' */
	uint64_t read_max;  /* the most bytes to read, 0 to the end; 0 for 'd' */
	uint64_t min_bytes; /* the fewest bytes the image needs, after inflating; without
			       KMSCON_IMAGE_PNG or _DISCARD, at least 1 and exactly these come back
			     */
	uint32_t max_width; /* max-graphic-size */
	uint32_t max_height;
	uint64_t max_bytes; /* image-storage-limit, the whole budget */
};

/*
 * The helper to kmscon; reason_len bytes follow, for kmscon's log. With
 * KMSCON_IMAGE_OK, except for a discard, the packet carries a sealed memfd:
 * for a PNG request width x height x 4 bytes of straight-alpha RGBA, else
 * exactly min_bytes bytes with width and height 0. Any other reply carries
 * none. ignored counts the bytes read from the source that the helper left
 * past what it used, which kmscon logs at debug level: without INFLATE or
 * PNG, those past min_bytes within the bounds, from fstat; with INFLATE,
 * with or without PNG, those the inflater did not consume; with PNG alone,
 * those past the IEND chunk. Decoded bytes never count.
 */
struct kmscon_image_reply {
	uint32_t version; /* KMSCON_IMAGE_VERSION */
	uint32_t status;  /* KMSCON_IMAGE_OK to KMSCON_IMAGE_OVER_BUDGET */
	uint64_t id;	  /* the request's */
	uint32_t width;	  /* KMSCON_IMAGE_OK with KMSCON_IMAGE_PNG; else 0 */
	uint32_t height;
	uint32_t reason_len; /* 0 to KMSCON_IMAGE_REASON_MAX */
	uint32_t ignored;    /* bytes past what was used, at most UINT32_MAX; 0 for an error
				or discard */
};

/* ADR-103's layout, with no padding */
_Static_assert(sizeof(struct kmscon_image_request) == 64, "the request is 64 bytes");
_Static_assert(offsetof(struct kmscon_image_request, type) == 4, "type at 4");
_Static_assert(offsetof(struct kmscon_image_request, id) == 8, "id at 8");
_Static_assert(offsetof(struct kmscon_image_request, medium) == 16, "medium at 16");
_Static_assert(offsetof(struct kmscon_image_request, flags) == 17, "flags at 17");
_Static_assert(offsetof(struct kmscon_image_request, reserved) == 18, "reserved at 18");
_Static_assert(offsetof(struct kmscon_image_request, name_len) == 20, "name_len at 20");
_Static_assert(offsetof(struct kmscon_image_request, offset) == 24, "offset at 24");
_Static_assert(offsetof(struct kmscon_image_request, read_max) == 32, "read_max at 32");
_Static_assert(offsetof(struct kmscon_image_request, min_bytes) == 40, "min_bytes at 40");
_Static_assert(offsetof(struct kmscon_image_request, max_width) == 48, "max_width at 48");
_Static_assert(offsetof(struct kmscon_image_request, max_height) == 52, "max_height at 52");
_Static_assert(offsetof(struct kmscon_image_request, max_bytes) == 56, "max_bytes at 56");
_Static_assert(sizeof(struct kmscon_image_reply) == 32, "the reply is 32 bytes");
_Static_assert(offsetof(struct kmscon_image_reply, status) == 4, "status at 4");
_Static_assert(offsetof(struct kmscon_image_reply, id) == 8, "id at 8");
_Static_assert(offsetof(struct kmscon_image_reply, width) == 16, "width at 16");
_Static_assert(offsetof(struct kmscon_image_reply, height) == 20, "height at 20");
_Static_assert(offsetof(struct kmscon_image_reply, reason_len) == 24, "reason_len at 24");
_Static_assert(offsetof(struct kmscon_image_reply, ignored) == 28, "ignored at 28");

#endif /* KMSCON_IMAGE_HELPER_PROTO_H */
