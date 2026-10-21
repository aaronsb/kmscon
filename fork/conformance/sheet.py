#!/usr/bin/env python3
"""Draw a conformance run as a contact sheet, and record it in the ledger.

The sheet is one SVG: a header with the run's revisions and pass counts, then
one tile per case and checkpoint. A passing tile shows the screen the run
produced. A failing tile shows the golden and the actual screen side by side,
with the rows that differ marked. The sheet is drawn only from the reports a
run wrote, so it shows what the verdict was based on and nothing else.

Identical reports and arguments give a byte-identical SVG.

Usage:
  sheet.py --out SHEET.svg --kmscon REV --libtsm REV --date YYYY-MM-DD
           [--ledger LEDGER.md --note TEXT] REPORT.json...

Tier 1 reports (from libtsm's check.py --report) carry screen dumps as text;
they are drawn as character grids. Tier 2 and 3 reports name PPM images; they
are drawn as thumbnails embedded in the SVG (golden, actual and diff for a
failure), with the reported regions outlined. Image paths are read relative to
the current directory, which is the repository root under `make`.

SPDX-License-Identifier: MIT
"""

import argparse
import base64
import json
import struct
import zlib
import sys
import unicodedata
from pathlib import Path
from xml.sax.saxutils import escape

CW, CH = 7, 13          # cell size in the tile, pixels
FONT = 11.5             # font size, pixels
PAD = 8                 # padding around a tile's grid
LABEL = 18              # height of a tile's label strip
GAP = 16                # space between tiles
COLUMNS = 3             # tile columns on the sheet
SHEET_BG = "#1b1d23"
INK = "#d8dee9"
THUMB_MAX_W = 384      # image tiles are scaled down to at most this width

STATUS_COLOR = {"pass": "#3fa34d", "near": "#d99a1e", "fail": "#d1453b", "missing": "#8b93a1"}

# xterm's 16 colours, then the default foreground and background (codes 16, 17).
PALETTE = ["#000000", "#cd0000", "#00cd00", "#cdcd00", "#0000ee", "#cd00cd", "#00cdcd", "#e5e5e5",
           "#7f7f7f", "#ff0000", "#00ff00", "#ffff00", "#5c5cff", "#ff00ff", "#00ffff", "#ffffff",
           "#d0d0d0", "#101216"]


# ---------------------------------------------------------------- dump parsing

def parse_dump(text):
    """Split a tsm-replay dump into screens: [{checkpoint, cursor, rows, attrs}]."""
    screens, cur = [], None

    def start(checkpoint):
        nonlocal cur
        cur = {"checkpoint": checkpoint, "cursor": (0, 0), "rows": {}, "attrs": {}, "flags": ""}
        screens.append(cur)

    for line in text.splitlines():
        if line.startswith("checkpoint "):
            start(line.split()[1])
            continue
        if cur is None:
            start(None)
        if line.startswith("cursor "):
            x, y = line.split()[1].split(",")
            cur["cursor"] = (int(x), int(y))
        elif line.startswith("flags "):
            cur["flags"] = line[6:]
        elif line.startswith("row "):
            head, _, rest = line.partition(" |")
            cur["rows"][int(head.split()[1])] = rest[:-1] if rest.endswith("|") else rest
        elif line.startswith("attr "):
            parts = line.split()
            row = int(parts[1])
            a, b = (int(v) for v in parts[2].split("-"))
            attr = {}
            for p in parts[3:]:
                if "=" in p:
                    k, v = p.split("=", 1)
                    attr[k] = v
                else:
                    attr[p] = True
            cur["attrs"].setdefault(row, []).append((a, b, attr))
    return screens


def cells_of(text):
    """Lay a row's text out on cells: [(column, string)], wide characters take two."""
    cells, col = [], 0
    for ch in text:
        if ord(ch) < 0x20 or ord(ch) == 0x7f:
            ch = "␀" if ord(ch) < 0x20 else "␡"
        joins = (unicodedata.combining(ch) or unicodedata.category(ch) in ("Mn", "Me", "Cf")
                 or 0xFE00 <= ord(ch) <= 0xFE0F or 0x1F3FB <= ord(ch) <= 0x1F3FF)
        if joins and cells:
            c, s = cells[-1]
            cells[-1] = (c, s + ch)
            continue
        cells.append((col, ch))
        wide = unicodedata.east_asian_width(ch) in ("W", "F") or ord(ch) >= 0x1F000
        col += 2 if wide else 1
    return cells


def color(value, default):
    if value is None:
        return PALETTE[default]
    if value.startswith("#"):
        return value
    code = int(value)
    return PALETTE[code] if 0 <= code < len(PALETTE) else PALETTE[default]


def cell_style(attrs, col):
    for a, b, attr in attrs:
        if a <= col <= b:
            return attr
    return {}


# ---------------------------------------------------------------- drawing

def cell_map(screen, cols):
    """{(row, col): (string, style)} for every non-blank or styled cell."""
    cells = {}
    for r, text in screen["rows"].items():
        attrs = screen["attrs"].get(r, [])
        for col, ch in cells_of(text):
            if col < cols:
                cells[(r, col)] = (ch, tuple(sorted(cell_style(attrs, col).items())))
    for r, runs in screen["attrs"].items():
        for a, b, attr in runs:
            for col in range(a, min(b, cols - 1) + 1):
                cells.setdefault((r, col), (" ", tuple(sorted(attr.items()))))
    return cells


def changed_regions(want, got, cols, rows):
    """Rectangles (c0, r0, c1, r1), inclusive, around every cell that differs.

    Changed cells on a row are joined into spans, and spans that overlap or
    touch on adjacent rows are merged, so a reader sees a few marked regions
    rather than one mark per cell. A moved cursor marks both positions.
    """
    a, b = cell_map(want, cols), cell_map(got, cols)
    diff = {k for k in set(a) | set(b) if a.get(k, (" ", ())) != b.get(k, (" ", ()))}
    if want["cursor"] != got["cursor"]:
        for cx, cy in (want["cursor"], got["cursor"]):
            if cx < cols and cy < rows:
                diff.add((cy, cx))
    spans = []
    for r in range(rows):
        cs = sorted(c for (rr, c) in diff if rr == r)
        for c in cs:
            if spans and spans[-1][1] == r and spans[-1][3] == r and c <= spans[-1][2] + 2:
                spans[-1][2] = c
            else:
                spans.append([c, r, c, r])
    regions = []
    for c0, r0, c1, r1 in spans:
        for reg in regions:
            if r0 <= reg[3] + 1 and c0 <= reg[2] + 2 and c1 >= reg[0] - 2:
                reg[0], reg[2], reg[3] = min(reg[0], c0), max(reg[2], c1), max(reg[3], r1)
                break
        else:
            regions.append([c0, r0, c1, r1])
    return [tuple(r) for r in regions]


def draw_marks(out, regions, x0, y0, color):
    for c0, r0, c1, r1 in regions:
        out.append(f'<rect x="{x0 + c0 * CW - 3}" y="{y0 + r0 * CH - 2}" width="{(c1 - c0 + 1) * CW + 6}" '
                   f'height="{(r1 - r0 + 1) * CH + 4}" rx="4" fill="none" stroke="{color}" stroke-width="2"/>')


def draw_grid(out, screen, cols, rows, x0, y0, marks=()):
    """Draw one screen's cells at (x0, y0), then outline the marked regions."""
    w, h = cols * CW, rows * CH
    out.append(f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="{PALETTE[17]}"/>')
    for r in range(rows):
        text = screen["rows"].get(r, "")
        attrs = screen["attrs"].get(r, [])
        y = y0 + r * CH
        # Words: runs of adjacent non-blank cells with one style. Each is drawn
        # from its first cell and stretched to exactly its cells (textLength),
        # so the grid holds whatever font the viewer substitutes.
        words = []  # [key, first_col, next_col, text]
        for col, s in cells_of(text):
            if col >= cols:
                break
            st = cell_style(attrs, col)
            fg, bg = color(st.get("fg"), 16), color(st.get("bg"), 17)
            if st.get("inverse"):
                fg, bg = bg, fg
            width = 2 if (unicodedata.east_asian_width(s[0]) in ("W", "F") or ord(s[0]) >= 0x1F000) else 1
            if bg != PALETTE[17]:
                out.append(f'<rect x="{x0 + col * CW}" y="{y}" width="{CW * width}" height="{CH}" fill="{bg}"/>')
            if s == " ":
                continue
            key = (fg, "bold" if st.get("bold") else "normal",
                   "underline" if st.get("underline") else "none",
                   "0.55" if st.get("dim") else "1", "italic" if st.get("italic") else "normal")
            if words and words[-1][0] == key and words[-1][2] == col:
                words[-1][2] = col + width
                words[-1][3] += s
            else:
                words.append([key, col, col + width, s])
        for (fg, weight, deco, opacity, style), first, end, s in words:
            out.append(f'<text x="{x0 + first * CW}" y="{y + CH - 3}" textLength="{(end - first) * CW}" '
                       f'lengthAdjust="spacingAndGlyphs" fill="{fg}" font-weight="{weight}" '
                       f'font-style="{style}" text-decoration="{deco}" opacity="{opacity}">{escape(s)}</text>')
    cx, cy = screen["cursor"]
    if cx < cols and cy < rows:
        out.append(f'<rect x="{x0 + cx * CW}" y="{y0 + cy * CH}" width="{CW}" height="{CH}" '
                   f'fill="none" stroke="#ebcb8b" stroke-width="1"/>')
    draw_marks(out, marks, x0, y0, STATUS_COLOR["fail"])
    return w, h


def tile_size(t):
    if t.get("kind") == "image":
        w = t["tw"] + 2 * PAD
        panes = 3 if t["status"] == "fail" else 1
        return panes * w + (panes - 1) * PAD, t["th"] + 2 * PAD + LABEL
    w = t["cols"] * CW + 2 * PAD
    return (2 * w + PAD if t["status"] == "fail" else w), t["rows"] * CH + 2 * PAD + LABEL


# ---------------------------------------------------------------- image tiles

def read_ppm(path):
    """(width, height, rgb bytes) of a binary P6 PPM with maxval 255."""
    data = Path(path).read_bytes()
    fields, i = [], 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    if fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError(f"{path}: not an 8-bit P6 PPM")
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[i + 1:i + 1 + w * h * 3]


def thumbnail(w, h, rgb):
    """Scale down by an integer step (nearest pixel) to at most THUMB_MAX_W wide."""
    step = max(1, -(-w // THUMB_MAX_W))
    tw, th = w // step, h // step
    rows = []
    for y in range(th):
        line = rgb[(y * step) * w * 3:(y * step + 1) * w * 3]
        rows.append(b"".join(line[x * step * 3:x * step * 3 + 3] for x in range(tw)) if step > 1 else line)
    return tw, th, step, rows


def png_uri(tw, th, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\x00" + r for r in rows)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", tw, th, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    return "data:image/png;base64," + base64.b64encode(png).decode()


def tiles_for_images(report):
    tiles = []
    for case in report["cases"]:
        panes = {}
        for key in ("golden", "actual", "diff"):
            path = case.get(key)
            if path and Path(path).exists():
                w, h, rgb = read_ppm(path)
                tw, th, step, rows = thumbnail(w, h, rgb)
                panes[key] = (png_uri(tw, th, rows), tw, th, step)
        if not panes:
            continue
        tw = max(p[1] for p in panes.values())
        th = max(p[2] for p in panes.values())
        step = next(iter(panes.values()))[3]
        regions = [tuple(b) for b in (case.get("stats") or {}).get("bbox", [])]
        tiles.append({"kind": "image", "label": f"tier {report['tier']}: {case['name']}",
                      "status": case["status"] if case["status"] in STATUS_COLOR else "fail",
                      "panes": panes, "tw": tw, "th": th, "step": step, "regions": regions})
    return tiles


def draw_image_tile(out, t, tx, gy):
    order = ("golden", "actual", "diff") if t["status"] == "fail" else ("actual",)
    color = STATUS_COLOR["near"] if t["status"] == "near" else STATUS_COLOR["fail"]
    w = t["tw"] + 2 * PAD
    for i, key in enumerate(order):
        if key not in t["panes"]:
            continue
        uri, pw, ph, step = t["panes"][key]
        x0 = tx + PAD + i * (w + PAD)
        out.append(f'<image x="{x0}" y="{gy}" width="{pw}" height="{ph}" href="{uri}" '
                   f'style="image-rendering:pixelated"/>')
        if len(order) > 1:
            out.append(f'<text x="{x0 + pw // 2}" y="{gy - 6}" fill="#8b93a1" font-size="10" '
                       f'text-anchor="middle">{key}</text>')
        if t["status"] in ("fail", "near"):
            for x1, y1, x2, y2 in t["regions"]:
                out.append(f'<rect x="{x0 + x1 // step - 2}" y="{gy + y1 // step - 2}" '
                           f'width="{(x2 - x1) // step + 5}" height="{(y2 - y1) // step + 5}" rx="3" '
                           f'fill="none" stroke="{color}" stroke-width="2"/>')


def tiles_for_tier1(report):
    tiles = []
    for case in report["cases"]:
        actual = parse_dump(case["actual"] or "")
        golden = parse_dump(case["golden"] or "") if case["golden"] else []
        for i, screen in enumerate(actual):
            want = golden[i] if i < len(golden) else None
            status = case["status"]
            if status == "fail":
                status = "pass" if want and want["rows"] == screen["rows"] and \
                    want["attrs"] == screen["attrs"] and want["cursor"] == screen["cursor"] else "fail"
            label = case["name"] + (f" @{screen['checkpoint']}" if screen["checkpoint"] else "")
            tiles.append({"label": label, "status": status, "cols": case["cols"], "rows": case["rows"],
                          "actual": screen, "golden": want})
    return tiles


def render(reports, args):
    tiles = []
    counts = []
    for rep in reports:
        n = len(rep["cases"])
        ok = sum(1 for c in rep["cases"] if c["status"] in ("pass", "near"))
        counts.append((rep["tier"], ok, n))
        if rep["tier"] == 1:
            tiles.extend(tiles_for_tier1(rep))
        else:
            tiles.extend(tiles_for_images(rep))

    # Lay tiles out in rows of COLUMNS, each grid row as tall as its tallest tile.
    layout, x, y, row_h, col = [], GAP, 0, 0, 0
    header = 70
    y = header
    width = GAP
    for t in tiles:
        tw, th = tile_size(t)
        if col == COLUMNS:
            x, y, col, row_h = GAP, y + row_h + GAP, 0, 0
        layout.append((t, x, y, tw, th))
        x += tw + GAP
        width = max(width, x)
        row_h = max(row_h, th)
        col += 1
    height = y + row_h + GAP

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" font-family="DejaVu Sans Mono, Menlo, Consolas, monospace" '
           f'font-size="{FONT}">',
           f'<rect width="{width}" height="{height}" fill="{SHEET_BG}"/>']
    title = f"kmscon fork conformance — {args.date}"
    out.append(f'<text x="{GAP}" y="28" fill="{INK}" font-size="18" font-weight="bold">{escape(title)}</text>')
    summary = f"kmscon {args.kmscon} · libtsm {args.libtsm}"
    for tier, ok, n in counts:
        summary += f" · tier {tier}: {ok}/{n} cases pass"
    out.append(f'<text x="{GAP}" y="50" fill="{INK}" font-size="12">{escape(summary)}</text>')

    for t, tx, ty, tw, th in layout:
        sc = STATUS_COLOR[t["status"]]
        out.append(f'<rect x="{tx}" y="{ty}" width="{tw}" height="{th}" fill="#262a33" stroke="{sc}" stroke-width="2"/>')
        out.append(f'<text x="{tx + PAD}" y="{ty + 13}" fill="{INK}" font-size="11">{escape(t["label"])}</text>')
        out.append(f'<text x="{tx + tw - PAD}" y="{ty + 13}" fill="{sc}" font-size="11" font-weight="bold" '
                   f'text-anchor="end">{t["status"].upper()}</text>')
        gy = ty + LABEL + PAD - 4
        if t.get("kind") == "image":
            draw_image_tile(out, t, tx, gy)
        elif t["status"] == "fail":
            want = t["golden"]
            regions = changed_regions(want, t["actual"], t["cols"], t["rows"]) if want else []
            if want:
                draw_grid(out, want, t["cols"], t["rows"], tx + PAD, gy, regions)
            w = t["cols"] * CW + 2 * PAD
            draw_grid(out, t["actual"], t["cols"], t["rows"], tx + w + PAD, gy, regions)
            for caption, cx in (("golden", tx + w // 2), ("actual", tx + w + PAD + w // 2)):
                out.append(f'<text x="{cx}" y="{ty + 13}" fill="#8b93a1" font-size="10" '
                           f'text-anchor="middle">{caption}</text>')
        else:
            draw_grid(out, t["actual"], t["cols"], t["rows"], tx + PAD, gy)
    out.append("</svg>")
    return "\n".join(out) + "\n", counts


LEDGER_HEAD = """# Conformance ledger

One row per committed contact sheet, oldest first. Each sheet is drawn from a single run of the harness (ADR-301): `make sheet NOTE="..."` runs the tiers, writes the sheet under `sheets/`, and appends its row here. The kmscon and libtsm columns name the commits that run measured.

| Date (UTC) | Note | kmscon | libtsm | Tier 1 | Tier 2 | Tier 3 | Sheet |
|---|---|---|---|---|---|---|---|
"""


def append_ledger(path, args, counts, sheet):
    by_tier = {tier: f"{ok}/{n}" for tier, ok, n in counts}
    rel = Path(sheet).resolve().relative_to(Path(path).resolve().parent)
    row = (f"| {args.date} | {args.note} | `{args.kmscon}` | `{args.libtsm}` | "
           f"{by_tier.get(1, '—')} | {by_tier.get(2, '—')} | {by_tier.get(3, '—')} | [{rel.name}]({rel.as_posix()}) |\n")
    p = Path(path)
    text = p.read_text() if p.exists() else LEDGER_HEAD
    p.write_text(text + row)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("reports", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--kmscon", required=True)
    ap.add_argument("--libtsm", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--ledger")
    ap.add_argument("--note", default="")
    args = ap.parse_args()

    reports = [json.loads(Path(r).read_text()) for r in args.reports]
    svg, counts = render(reports, args)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(svg)
    for tier, ok, n in counts:
        print(f"sheet: tier {tier}: {ok}/{n} cases pass")
    print(f"sheet: wrote {args.out}")
    if args.ledger:
        append_ledger(args.ledger, args, counts, args.out)
        print(f"sheet: ledger row added to {args.ledger}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
