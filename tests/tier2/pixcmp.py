#!/usr/bin/env python3
"""Compare two PPM (P6) images pixel by pixel.

  pixcmp.py [--tol FILE] [--max-delta N] [--max-pixels M] [--mask X0,Y0,X1,Y1]
            [--diff OUT.ppm] [--json] GOLDEN ACTUAL

Exit status 0 when the images are identical or differ within tolerance
("near"), 1 when they are beyond it, 2 on an unreadable input. Tolerance is
the largest per-channel delta (--max-delta) and the number of differing
pixels (--max-pixels) allowed, with mask rectangles excluded from the
comparison; both default to 0, an exact comparison. A .tol sidecar holds the
same as key=value lines (max_delta=, max_pixels=, mask=x0,y0,x1,y1).

Prints the differing-pixel count, the clustered bounding boxes (at most 16, tile-based)
of the changed regions and the largest per-channel delta. --diff writes a PPM
that shows unchanged pixels as dim grey and changed pixels as red.

Standard library only; also importable (compare(), read_ppm()).
"""

import argparse
import json
import sys

# Differing pixels are grouped by the TILE x TILE tiles they fall in; touching
# dirty tiles form one region.
TILE = 8
# Most regions reported; the closest are merged beyond this.
MAX_BOXES = 16


def read_ppm(path):
    """Return (width, height, bytes) for a binary PPM with maxval 255."""
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    tokens = []
    while len(tokens) < 4:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos : pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        if start == pos:
            raise ValueError("truncated PPM header")
        tokens.append(data[start:pos])
    pos += 1  # exactly one whitespace byte separates the header from the data
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError("not a P6 PPM with maxval 255")
    w, h = int(tokens[1]), int(tokens[2])
    pix = data[pos:]
    if len(pix) != w * h * 3:
        raise ValueError("pixel data is %d bytes, expected %d" % (len(pix), w * h * 3))
    return w, h, pix


def write_ppm(path, w, h, pix):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(pix)


def read_tol(path):
    """Read a .tol sidecar: key=value lines, '#' comments.

    Keys: max_delta, max_pixels (integers, default 0) and mask=x0,y0,x1,y1
    (inclusive rectangle, repeatable). Returns {"max_delta", "max_pixels",
    "masks"}.
    """
    tol = {"max_delta": 0, "max_pixels": 0, "masks": []}
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            key, sep, val = line.partition("=")
            key, val = key.strip(), val.strip()
            try:
                if not sep:
                    raise ValueError
                if key in ("max_delta", "max_pixels"):
                    tol[key] = int(val)
                elif key == "mask":
                    r = [int(v) for v in val.split(",")]
                    if len(r) != 4:
                        raise ValueError
                    tol["masks"].append(r)
                else:
                    raise ValueError
            except ValueError:
                raise ValueError("%s:%d: bad line %r" % (path, n, line))
    return tol


def _masked(x, y, masks):
    for m in masks:
        if m[0] <= x <= m[2] and m[1] <= y <= m[3]:
            return True
    return False


def _row_diffs(a, b, w, y, masks):
    """x positions of differing pixels in row y, with their count and max delta."""
    ra = a[y * w * 3 : (y + 1) * w * 3]
    rb = b[y * w * 3 : (y + 1) * w * 3]
    xs = []
    top = 0
    for x in range(w):
        i = x * 3
        d = max(abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2]))
        if d and not _masked(x, y, masks):
            xs.append(x)
            top = max(top, d)
    return xs, top


def _regions(tiles, cap=MAX_BOXES):
    """Connected components of dirty tiles -> bounding boxes of their pixels.

    tiles maps (tx, ty) to the pixel extent [x0, y0, x1, y1] inside it. While
    there are more than `cap` components the tile size doubles. Linear in the
    number of dirty tiles per pass.
    """
    shift = 0
    while True:
        seen = set()
        boxes = []
        for start in tiles:
            if start in seen:
                continue
            seen.add(start)
            stack = [start]
            box = list(tiles[start])
            while stack:
                tx, ty = stack.pop()
                e = tiles[(tx, ty)]
                box = [min(box[0], e[0]), min(box[1], e[1]), max(box[2], e[2]), max(box[3], e[3])]
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        n = (tx + dx, ty + dy)
                        if n in tiles and n not in seen:
                            seen.add(n)
                            stack.append(n)
            boxes.append(box)
        if len(boxes) <= cap:
            return sorted(boxes, key=lambda b: (b[1], b[0]))
        coarse = {}
        for (tx, ty), e in tiles.items():
            k = (tx >> 1, ty >> 1)
            c = coarse.get(k)
            coarse[k] = list(e) if c is None else [min(c[0], e[0]), min(c[1], e[1]), max(c[2], e[2]), max(c[3], e[3])]
        tiles = coarse
        shift += 1


def compare(golden, actual, tol=None, diff_path=None):
    """Compare two PPM files under a tolerance (default exact).

    tol is {"max_delta", "max_pixels", "masks"}; pixels inside a mask
    rectangle are ignored. Returns a dict: status ("identical", "near" when
    different but within tolerance, "fail"), match (status != "fail"),
    pixels (differing pixels outside masks), bbox (at most MAX_BOXES clustered
    regions [x0, y0, x1, y1], inclusive), max_delta (largest per-channel delta
    outside masks) and note (set when sizes differ).
    """
    tol = tol or {"max_delta": 0, "max_pixels": 0, "masks": []}
    masks = tol["masks"]
    gw, gh, gp = read_ppm(golden)
    aw, ah, ap = read_ppm(actual)
    if (gw, gh) != (aw, ah):
        return {
            "status": "fail",
            "match": False,
            "pixels": max(gw * gh, aw * ah),
            "bbox": [[0, 0, max(gw, aw) - 1, max(gh, ah) - 1]],
            "max_delta": 255,
            "note": "size differs: golden %dx%d, actual %dx%d" % (gw, gh, aw, ah),
        }

    tiles = {}
    pixels = 0
    top = 0
    diff = bytearray(len(ap)) if diff_path else None
    for y in range(gh):
        lo, hi = y * gw * 3, (y + 1) * gw * 3
        if gp[lo:hi] != ap[lo:hi]:
            xs, t = _row_diffs(gp, ap, gw, y, masks)
            pixels += len(xs)
            top = max(top, t)
            for x in xs:
                k = (x // TILE, y // TILE)
                e = tiles.get(k)
                if e is None:
                    tiles[k] = [x, y, x, y]
                else:
                    e[0], e[1], e[2], e[3] = min(e[0], x), min(e[1], y), max(e[2], x), max(e[3], y)
        if diff is not None:
            for x in range(gw):
                i = lo + x * 3
                if gp[i : i + 3] != ap[i : i + 3] and not _masked(x, y, masks):
                    diff[i : i + 3] = b"\xff\x00\x00"
                else:
                    g = (ap[i] + ap[i + 1] + ap[i + 2]) // 12
                    diff[i] = diff[i + 1] = diff[i + 2] = g
    if diff is not None:
        write_ppm(diff_path, gw, gh, bytes(diff))
    if pixels == 0:
        status = "identical"
    elif top <= tol["max_delta"] and pixels <= tol["max_pixels"]:
        status = "near"
    else:
        status = "fail"
    return {
        "status": status,
        "match": status != "fail",
        "pixels": pixels,
        "bbox": _regions(tiles),
        "max_delta": top,
    }


def main(argv=None):
    p = argparse.ArgumentParser(description="Compare two P6 PPM images.")
    p.add_argument("golden")
    p.add_argument("actual")
    p.add_argument("--tol", metavar="FILE", help="read tolerance from a .tol sidecar")
    p.add_argument("--max-delta", type=int, help="largest per-channel delta allowed")
    p.add_argument("--max-pixels", type=int, help="differing pixels allowed")
    p.add_argument("--mask", action="append", default=[], metavar="X0,Y0,X1,Y1",
                   help="rectangle excluded from the comparison (repeatable)")
    p.add_argument("--diff", metavar="OUT.ppm", help="write a diff image here")
    p.add_argument("--json", action="store_true", help="print the result as JSON")
    args = p.parse_args(argv)

    try:
        tol = read_tol(args.tol) if args.tol else {"max_delta": 0, "max_pixels": 0, "masks": []}
        if args.max_delta is not None:
            tol["max_delta"] = args.max_delta
        if args.max_pixels is not None:
            tol["max_pixels"] = args.max_pixels
        tol["masks"] += [[int(v) for v in m.split(",")] for m in args.mask]
        r = compare(args.golden, args.actual, tol, args.diff)
    except (OSError, ValueError) as e:
        print("pixcmp: %s" % e, file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(r))
    elif r["status"] == "identical":
        print("identical")
    else:
        print("%s: %d pixel(s) differ, max channel delta %d"
              % ("near (within tolerance)" if r["match"] else "FAIL", r["pixels"], r["max_delta"]))
        if r.get("note"):
            print("  " + r["note"])
        for b in r["bbox"]:
            print("  region x=%d..%d y=%d..%d" % (b[0], b[2], b[1], b[3]))
    return 0 if r["match"] else 1


if __name__ == "__main__":
    sys.exit(main())
