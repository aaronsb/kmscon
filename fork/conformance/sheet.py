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
they are drawn as character grids. Reports from other tiers are counted in the
header and ledger; their image tiles are not drawn yet.

SPDX-License-Identifier: MIT
"""

import argparse
import json
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
STATUS_COLOR = {"pass": "#3fa34d", "fail": "#d1453b", "missing": "#d99a1e"}

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

def draw_grid(out, screen, cols, rows, x0, y0, changed_rows=()):
    """Draw one screen's cells at (x0, y0)."""
    w, h = cols * CW, rows * CH
    out.append(f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="{PALETTE[17]}"/>')
    for r in sorted(changed_rows):
        out.append(f'<rect x="{x0 - 3}" y="{y0 + r * CH}" width="2" height="{CH}" fill="{STATUS_COLOR["fail"]}"/>')
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
    return w, h


def tile_size(cols, rows, failing):
    w = cols * CW + 2 * PAD
    return (2 * w + PAD if failing else w), rows * CH + 2 * PAD + LABEL


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
        ok = sum(1 for c in rep["cases"] if c["status"] == "pass")
        counts.append((rep["tier"], ok, n))
        if rep["tier"] == 1:
            tiles.extend(tiles_for_tier1(rep))

    # Lay tiles out in rows of COLUMNS, each grid row as tall as its tallest tile.
    layout, x, y, row_h, col = [], GAP, 0, 0, 0
    header = 70
    y = header
    width = GAP
    for t in tiles:
        tw, th = tile_size(t["cols"], t["rows"], t["status"] == "fail")
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
        if t["status"] == "fail":
            want = t["golden"]
            changed = set()
            if want:
                for r in range(t["rows"]):
                    if (want["rows"].get(r) != t["actual"]["rows"].get(r)
                            or want["attrs"].get(r) != t["actual"]["attrs"].get(r)):
                        changed.add(r)
                draw_grid(out, want, t["cols"], t["rows"], tx + PAD, gy)
            w = t["cols"] * CW + 2 * PAD
            draw_grid(out, t["actual"], t["cols"], t["rows"], tx + w + PAD, gy, changed)
            for caption, cx in (("golden", tx + w // 2), ("actual", tx + w + PAD + w // 2)):
                out.append(f'<text x="{cx}" y="{ty + 13}" fill="#8b93a1" font-size="10" '
                           f'text-anchor="middle">{caption}</text>')
        else:
            draw_grid(out, t["actual"], t["cols"], t["rows"], tx + PAD, gy)
    out.append("</svg>")
    return "\n".join(out) + "\n", counts


LEDGER_HEAD = """# Conformance ledger

One row per committed contact sheet, oldest first. Each sheet is drawn from a single run of the harness (ADR-301): `make sheet NOTE="..."` runs the tiers, writes the sheet under `sheets/`, and appends its row here. The kmscon and libtsm columns name the commits that run measured.

| Date | Note | kmscon | libtsm | Tier 1 | Tier 2 | Tier 3 | Sheet |
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
