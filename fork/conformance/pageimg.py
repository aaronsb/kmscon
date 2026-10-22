#!/usr/bin/env python3
"""Screen images for the conformance pages: crop a capture to what was drawn,
mark where two captures differ, lay panes out in a strip, write small PNGs.

Stdlib only; captures are read through tier 3's t3img. Every function is a
pure function of its inputs, so the pages that use them are deterministic.

SPDX-License-Identifier: MIT
"""

import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tier3"))
import t3img  # noqa: E402

MARGIN = 8              # pixels kept around the drawn region of a screen
MAX_W = 1200            # a crop wider than this is scaled down by an integer step
STRIP_W = 1000          # three panes wider than this are stacked, not side by side
GUTTER = 6              # pixels between the panes of a strip
GUTTER_RGB = b"\x80\x80\x80"
DIFF_RGB = b"\xff\x00\x00"
OUTLINE_RGB = b"\xff\xd7\x00"
DIM = bytes(b // 3 for b in range(256))
# 3x5 digits for numbering outlined regions, one string of rows per digit.
DIGITS = ["111101101101111", "010110010010111", "111001111100111", "111001111001111",
          "101101111001001", "111100111001111", "111100111101111", "111001001001001",
          "111101111101111", "111101111001111"]


def load(path):
    return t3img.read(path) if path and Path(path).exists() else None


def content_box(img):
    """(x0, y0, x1, y1), end-exclusive, around every pixel unlike the top-left one."""
    bg = img.data[:3]
    blank = bg * img.width
    rows = [y for y in range(img.height) if img.row(y) != blank]
    if not rows:
        return (0, 0, min(img.width, 64), min(img.height, 16))
    x0, x1 = img.width, 0
    for y in rows:
        r = img.row(y)
        lo, hi = 0, img.width
        while lo < x0 and r[lo * 3:lo * 3 + 3] == bg:
            lo += 1
        while hi > x1 and r[hi * 3 - 3:hi * 3] == bg:
            hi -= 1
        x0, x1 = min(x0, lo), max(x1, hi)
    return (x0, rows[0], x1, rows[-1] + 1)


def crop_box(images, regions=()):
    """The union of the images' drawn regions and the reported difference
    regions, with a margin, within the first image."""
    boxes = [content_box(i) for i in images] + [tuple(r) for r in regions]
    w, h = images[0].width, images[0].height
    return (max(0, min(b[0] for b in boxes) - MARGIN), max(0, min(b[1] for b in boxes) - MARGIN),
            min(w, max(b[2] for b in boxes) + MARGIN), min(h, max(b[3] for b in boxes) + MARGIN))


def step_for(box):
    return max(1, -(-(box[2] - box[0]) // MAX_W))


def crop(img, box, step):
    """The box of an image, keeping one pixel in `step` each way."""
    x0, y0, x1, y1 = box
    w = (x1 - x0 + step - 1) // step
    rows = []
    for y in range(y0, y1, step):
        r = img.row(y)[x0 * 3:x1 * 3]
        if step > 1:
            out = bytearray(w * 3)
            for c in range(3):
                out[c::3] = r[c::3 * step]
            r = bytes(out)
        rows.append(r)
    return t3img.Image(w, len(rows), b"".join(rows))


def diff_image(want, got, box, step):
    """`got` dimmed, with every block that holds a differing pixel in red, so
    a single differing pixel stays visible after scaling; and how many blocks
    are red."""
    base = crop(got, box, step)
    out = bytearray(base.data.translate(DIM))
    x0, y0, x1, y1 = box
    marked = set()
    for y in range(y0, y1):
        a, b = want.row(y), got.row(y)
        if a == b:
            continue
        oy = (y - y0) // step
        for bx in range(x0, x1, step):
            end = min(bx + step, x1) * 3
            if a[bx * 3:end] != b[bx * 3:end]:
                i = (oy * base.width + (bx - x0) // step) * 3
                out[i:i + 3] = DIFF_RGB
                marked.add(i)
    return t3img.Image(base.width, base.height, bytes(out)), len(marked)


def strip(panes):
    """Panes side by side, or stacked when that would be too wide."""
    w = max(p.width for p in panes)
    h = max(p.height for p in panes)
    across = len(panes) * w + (len(panes) - 1) * GUTTER <= STRIP_W
    W = len(panes) * w + (len(panes) - 1) * GUTTER if across else w
    H = h if across else len(panes) * h + (len(panes) - 1) * GUTTER
    out = bytearray(GUTTER_RGB * (W * H))
    for i, p in enumerate(panes):
        ox, oy = (i * (w + GUTTER), 0) if across else (0, i * (h + GUTTER))
        for y in range(p.height):
            at = ((oy + y) * W + ox) * 3
            out[at:at + p.width * 3] = p.row(y)
    return t3img.Image(W, H, bytes(out)), across


def write_png(path, img):
    """A palette PNG when the image has at most 256 colours, else RGB; rows
    unfiltered, which suits flat screen content."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    px = [img.data[i:i + 3] for i in range(0, len(img.data), 3)]
    colours = sorted(set(px))
    if len(colours) <= 256:
        index = {c: bytes([i]) for i, c in enumerate(colours)}
        rows = (b"".join(index[c] for c in px[y * img.width:(y + 1) * img.width]) for y in range(img.height))
        ctype, extra = 3, chunk(b"PLTE", b"".join(colours))
    else:
        rows = (img.row(y) for y in range(img.height))
        ctype, extra = 2, b""
    raw = b"".join(b"\x00" + r for r in rows)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"
                          + chunk(b"IHDR", struct.pack(">IIBBBBB", img.width, img.height, 8, ctype, 0, 0, 0))
                          + extra + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def outline(img, regions, box, step):
    """`img`, a crop of `box` kept one pixel in `step`, with each region (in
    screen pixels) outlined and numbered from 1 in list order."""
    out = bytearray(img.data)
    w, h = img.width, img.height

    def put(x, y):
        if 0 <= x < w and 0 <= y < h:
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = OUTLINE_RGB

    for n, (x0, y0, x1, y1) in enumerate(regions, 1):
        # Two pixels outside the region, so the outline covers no difference.
        a0, b0 = (x0 - box[0]) // step - 3, (y0 - box[1]) // step - 3
        a1, b1 = (x1 - 1 - box[0]) // step + 3, (y1 - 1 - box[1]) // step + 3
        for t in (0, 1):
            for x in range(a0, a1 + 1):
                put(x, b0 + t)
                put(x, b1 - t)
            for y in range(b0, b1 + 1):
                put(a0 + t, y)
                put(a1 - t, y)
        # The number, two pixels per dot, above the box's left corner, or
        # beside its top right corner when there is no room above.
        lx, ly = (a0, b0 - 12) if b0 >= 12 else (a1 + 4, b0)
        for i, d in enumerate(str(n)):
            for k, on in enumerate(DIGITS[int(d)]):
                if on == "1":
                    for dx in (0, 1):
                        for dy in (0, 1):
                            put(lx + i * 8 + (k % 3) * 2 + dx, ly + (k // 3) * 2 + dy)
    return t3img.Image(w, h, bytes(out))
