#!/usr/bin/env python3
"""Compare two PPM (P6) images pixel by pixel.

  pixcmp.py [--max-delta N] [--max-pixels M] [--diff OUT.ppm] [--json] GOLDEN ACTUAL

Exit status 0 when the images match, 1 when they differ, 2 on an unreadable
input. A pixel differs when any channel differs by more than --max-delta
(default 0). The images match when at most --max-pixels pixels differ
(default 0). The defaults are an exact comparison.

Prints the differing-pixel count, the bounding boxes of the changed regions
and the largest per-channel delta. --diff writes a PPM that shows unchanged
pixels as dim grey and changed pixels as red.

Standard library only; also importable (compare(), read_ppm()).
"""

import argparse
import json
import sys

# Differing pixels closer than this (pixels, both axes) share one bounding box.
MERGE_GAP = 8


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


def _row_runs(a, b, w, y, max_delta):
    """Runs [x0, x1] of differing pixels in row y, with the row's max delta."""
    ra = a[y * w * 3 : (y + 1) * w * 3]
    rb = b[y * w * 3 : (y + 1) * w * 3]
    runs = []
    count = 0
    top = 0
    for x in range(w):
        i = x * 3
        d = max(abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2]))
        if d > max_delta:
            count += 1
            top = max(top, d)
            if runs and x - runs[-1][1] <= MERGE_GAP:
                runs[-1][1] = x
            else:
                runs.append([x, x])
    return runs, count, top


def _merge_boxes(boxes):
    """Merge boxes that overlap or lie within MERGE_GAP of each other."""
    boxes = [list(b) for b in boxes]
    merged = True
    while merged:
        merged = False
        out = []
        for b in boxes:
            for o in out:
                if (
                    b[0] <= o[2] + MERGE_GAP
                    and o[0] <= b[2] + MERGE_GAP
                    and b[1] <= o[3] + MERGE_GAP
                    and o[1] <= b[3] + MERGE_GAP
                ):
                    o[0], o[1] = min(o[0], b[0]), min(o[1], b[1])
                    o[2], o[3] = max(o[2], b[2]), max(o[3], b[3])
                    merged = True
                    break
            else:
                out.append(b)
        boxes = out
    return sorted(boxes, key=lambda b: (b[1], b[0]))


def compare(golden, actual, max_delta=0, max_pixels=0, diff_path=None):
    """Compare two PPM files.

    Returns a dict: match (bool), pixels (differing count), bbox ([[x0, y0,
    x1, y1], ...], inclusive), max_delta (largest per-channel delta over the
    differing pixels), and note (set when the images cannot be compared
    pixel by pixel, e.g. different sizes).
    """
    gw, gh, gp = read_ppm(golden)
    aw, ah, ap = read_ppm(actual)
    if (gw, gh) != (aw, ah):
        return {
            "match": False,
            "pixels": max(gw * gh, aw * ah),
            "bbox": [[0, 0, max(gw, aw) - 1, max(gh, ah) - 1]],
            "max_delta": 255,
            "note": "size differs: golden %dx%d, actual %dx%d" % (gw, gh, aw, ah),
        }

    boxes = []
    pixels = 0
    top = 0
    diff = bytearray(len(ap)) if diff_path else None
    for y in range(gh):
        lo, hi = y * gw * 3, (y + 1) * gw * 3
        if gp[lo:hi] == ap[lo:hi]:
            if diff is not None:
                for i in range(lo, hi, 3):
                    g = (ap[i] + ap[i + 1] + ap[i + 2]) // 12
                    diff[i] = diff[i + 1] = diff[i + 2] = g
            continue
        runs, n, t = _row_runs(gp, ap, gw, y, max_delta)
        pixels += n
        top = max(top, t)
        boxes.extend([r[0], y, r[1], y] for r in runs)
        if diff is not None:
            for x in range(gw):
                i = lo + x * 3
                d = max(abs(gp[i] - ap[i]), abs(gp[i + 1] - ap[i + 1]), abs(gp[i + 2] - ap[i + 2]))
                if d > max_delta:
                    diff[i : i + 3] = b"\xff\x00\x00"
                else:
                    g = (ap[i] + ap[i + 1] + ap[i + 2]) // 12
                    diff[i] = diff[i + 1] = diff[i + 2] = g
    if diff is not None:
        write_ppm(diff_path, gw, gh, bytes(diff))
    return {
        "match": pixels <= max_pixels,
        "pixels": pixels,
        "bbox": _merge_boxes(boxes),
        "max_delta": top,
    }


def main(argv=None):
    p = argparse.ArgumentParser(description="Compare two P6 PPM images.")
    p.add_argument("golden")
    p.add_argument("actual")
    p.add_argument("--max-delta", type=int, default=0, help="per-channel delta tolerated per pixel")
    p.add_argument("--max-pixels", type=int, default=0, help="differing pixels tolerated")
    p.add_argument("--diff", metavar="OUT.ppm", help="write a diff image here")
    p.add_argument("--json", action="store_true", help="print the result as JSON")
    args = p.parse_args(argv)

    try:
        r = compare(args.golden, args.actual, args.max_delta, args.max_pixels, args.diff)
    except (OSError, ValueError) as e:
        print("pixcmp: %s" % e, file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(r))
    elif r["match"] and r["pixels"] == 0:
        print("identical")
    else:
        verdict = "within tolerance" if r["match"] else "DIFFERENT"
        print("%s: %d pixel(s) differ, max channel delta %d" % (verdict, r["pixels"], r["max_delta"]))
        if r.get("note"):
            print("  " + r["note"])
        for b in r["bbox"]:
            print("  region x=%d..%d y=%d..%d" % (b[0], b[2], b[1], b[3]))
    return 0 if r["match"] else 1


if __name__ == "__main__":
    sys.exit(main())
