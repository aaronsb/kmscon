#!/usr/bin/env python3
"""Text for the glyphs kit's pages (ADR-102's glyph cache), written to stdout.

  glyph-text.py cjk LINES COLS   a seeded "novel" of CJK ideographs, COLS
                                 cells a line: 6000 ideographs of the URO
                                 block, the first commoner by Zipf's law, as
                                 a reader's working set is
  glyph-text.py bmp COLS         every printable BMP character (Unicode
                                 categories L, M, N, P, S and Zs, no private
                                 use or surrogates), a run of them a line

The output is the same on every run, so two builds see the same glyphs.
"""

import random
import sys
import unicodedata


def cjk(lines, cols):
    rng = random.Random(2026)
    chars = [chr(c) for c in range(0x4E00, 0x9FF0)]
    rng.shuffle(chars)
    chars = chars[:6000]
    weights = [1.0 / (rank + 1) for rank in range(len(chars))]
    per_line = cols // 2
    for n in range(lines):
        text = "".join(rng.choices(chars, weights, k=per_line - 6))
        sys.stdout.write(f"{n:5d} {text}\n")


def bmp(cols):
    line, width = [], 0
    for c in range(0x20, 0x10000):
        ch = chr(c)
        cat = unicodedata.category(ch)
        if cat[0] not in "LMNPS" and cat != "Zs":
            continue
        w = 2 if unicodedata.east_asian_width(ch) in "WF" else 1
        if cat[0] == "M":
            ch, w = "◌" + ch, 1  # a mark on a dotted circle
        if width + w > cols - 1:
            sys.stdout.write("".join(line) + "\n")
            line, width = [], 0
        line.append(ch)
        width += w
    sys.stdout.write("".join(line) + "\n")


def main(argv):
    if len(argv) == 4 and argv[1] == "cjk":
        cjk(int(argv[2]), int(argv[3]))
    elif len(argv) == 3 and argv[1] == "bmp":
        bmp(int(argv[2]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
