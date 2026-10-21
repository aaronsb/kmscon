#!/usr/bin/env python3
"""Image I/O and masked exact comparison for the tier 3 harness (stdlib only).

Reads binary PPM (P6, what QEMU's screendump writes) and 8-bit RGB/RGBA PNG;
writes PNG through zlib, so goldens of very large screens stay small enough to
commit. Comparison is exact: any channel difference outside a mask rectangle
is a differing pixel.

Tier 2 is building its own compare tool, pixcmp, in parallel. This module is
deliberately minimal; the two should be merged once both exist.

Usage:
    t3img.py compare ACTUAL GOLDEN [--mask MASK.json] [--diff OUT.png]
    t3img.py topng IN OUT.png
"""

import argparse
import json
import struct
import sys
import zlib


class Image:
    """An RGB image: width, height and packed 8-bit RGB rows."""

    def __init__(self, width, height, data):
        if len(data) != width * height * 3:
            raise ValueError("pixel data does not match %dx%d" % (width, height))
        self.width = width
        self.height = height
        self.data = data

    def row(self, y):
        stride = self.width * 3
        return self.data[y * stride:(y + 1) * stride]


def _ppm_tokens(buf, count):
    """Return `count` header tokens of a PPM and the offset after them."""
    tokens, i = [], 0
    while len(tokens) < count:
        while buf[i:i + 1].isspace():
            i += 1
        if buf[i:i + 1] == b"#":
            while buf[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not buf[j:j + 1].isspace():
            j += 1
        tokens.append(buf[i:j])
        i = j
    return tokens, i + 1


def read_ppm(buf):
    (magic, w, h, maxval), off = _ppm_tokens(buf, 4)
    if magic != b"P6" or maxval != b"255":
        raise ValueError("only 8-bit binary PPM (P6) is supported")
    w, h = int(w), int(h)
    return Image(w, h, bytes(buf[off:off + w * h * 3]))


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(buf):
    if buf[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    off, idat, hdr = 8, [], None
    while off < len(buf):
        length, kind = struct.unpack(">I4s", buf[off:off + 8])
        chunk = buf[off + 8:off + 8 + length]
        off += 12 + length
        if kind == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            idat.append(chunk)
        elif kind == b"IEND":
            break
    w, h, depth, ctype, _, _, interlace = hdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError("only 8-bit non-interlaced RGB/RGBA PNG is supported")
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    prev = bytearray(stride)
    out = bytearray()
    pos = 0
    for _ in range(h):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                upleft = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
        elif ftype != 0:
            raise ValueError("bad PNG filter %d" % ftype)
        prev = line
        if bpp == 4:
            del line[3::4]
        out += line
    return Image(w, h, bytes(out))


def read(path):
    with open(path, "rb") as f:
        buf = f.read()
    return read_png(buf) if buf[:4] == b"\x89PNG" else read_ppm(buf)


def write_png(path, img):
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    # Filter type 0 on every row: screens are mostly flat colour, which zlib
    # compresses well without the row filters.
    raw = b"".join(b"\x00" + img.row(y) for y in range(img.height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", img.width, img.height,
                                           8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def load_mask(path):
    """Mask rectangles [[x0, y0, x1, y1], ...], end-exclusive, from JSON."""
    if not path:
        return []
    try:
        with open(path) as f:
            doc = json.load(f)
    except FileNotFoundError:
        return []
    return [tuple(r) for r in doc.get("rects", [])]


def _bboxes(points, cell=16):
    """Group differing pixels into bounding boxes of touching cell-sized tiles."""
    tiles = {}
    for x, y in points:
        key = (x // cell, y // cell)
        b = tiles.get(key)
        if b is None:
            tiles[key] = [x, y, x + 1, y + 1]
        else:
            b[0], b[1] = min(b[0], x), min(b[1], y)
            b[2], b[3] = max(b[2], x + 1), max(b[3], y + 1)
    seen, boxes = set(), []
    for start in tiles:
        if start in seen:
            continue
        seen.add(start)
        stack, box = [start], list(tiles[start])
        while stack:
            tx, ty = stack.pop()
            t = tiles[(tx, ty)]
            box = [min(box[0], t[0]), min(box[1], t[1]),
                   max(box[2], t[2]), max(box[3], t[3])]
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    n = (tx + dx, ty + dy)
                    if n in tiles and n not in seen:
                        seen.add(n)
                        stack.append(n)
        boxes.append(box)
    return sorted(boxes, key=lambda b: (b[1], b[0]))


def compare(actual, golden, mask=()):
    """Compare two images exactly outside the mask rectangles.

    Returns (stats, diff_image_or_None). stats has pixels, bbox and max_delta;
    a size mismatch counts every pixel and boxes the whole larger frame.
    """
    if (actual.width, actual.height) != (golden.width, golden.height):
        w, h = max(actual.width, golden.width), max(actual.height, golden.height)
        return {"pixels": w * h, "bbox": [[0, 0, w, h]], "max_delta": 255,
                "size_mismatch": [[actual.width, actual.height],
                                  [golden.width, golden.height]]}, None
    w = actual.width
    points, max_delta = [], 0
    for y in range(actual.height):
        ra, rg = actual.row(y), golden.row(y)
        if ra == rg:
            continue
        for x in range(w):
            i = x * 3
            pa, pg = ra[i:i + 3], rg[i:i + 3]
            if pa == pg:
                continue
            if any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in mask):
                continue
            points.append((x, y))
            max_delta = max(max_delta, *(abs(a - b) for a, b in zip(pa, pg)))
    stats = {"pixels": len(points), "bbox": _bboxes(points), "max_delta": max_delta}
    if not points:
        return stats, None
    # The diff shows the actual screen dimmed, differing pixels in red and
    # mask rectangles tinted blue.
    out = bytearray(b // 3 for b in actual.data)
    for x0, y0, x1, y1 in mask:
        for y in range(max(0, y0), min(actual.height, y1)):
            for x in range(max(0, x0), min(w, x1)):
                out[(y * w + x) * 3 + 2] = min(255, out[(y * w + x) * 3 + 2] + 96)
    for x, y in points:
        i = (y * w + x) * 3
        out[i:i + 3] = b"\xff\x00\x00"
    return stats, Image(w, actual.height, bytes(out))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare", help="compare ACTUAL with GOLDEN; exit 1 on difference")
    c.add_argument("actual")
    c.add_argument("golden")
    c.add_argument("--mask")
    c.add_argument("--diff")
    t = sub.add_parser("topng", help="convert a PPM or PNG to PNG")
    t.add_argument("src")
    t.add_argument("dst")
    args = ap.parse_args(argv)
    if args.cmd == "topng":
        write_png(args.dst, read(args.src))
        return 0
    stats, diff = compare(read(args.actual), read(args.golden), load_mask(args.mask))
    if diff is not None and args.diff:
        write_png(args.diff, diff)
    json.dump(stats, sys.stdout)
    print()
    return 1 if stats["pixels"] else 0


if __name__ == "__main__":
    sys.exit(main())
