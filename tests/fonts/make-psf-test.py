#!/usr/bin/env python3
"""Write psf-test.psf and psf-test.psf.gz for test_font_psf: a PSF2 font of
256 glyphs, 8x16, glyph G's row R holding the byte (G + R) & 0xff, with no
unicode table; the second is the first gzipped with no timestamp, so the
output is the same on every run.

  make-psf-test.py DIR
"""

import gzip
import struct
import sys
from pathlib import Path

GLYPHS, HEIGHT, WIDTH = 256, 16, 8


def main():
    out = Path(sys.argv[1])
    header = struct.pack("<4sIIIIIII", b"\x72\xb5\x4a\x86", 0, 32, 0, GLYPHS, HEIGHT, HEIGHT, WIDTH)
    glyphs = bytes((g + r) & 0xFF for g in range(GLYPHS) for r in range(HEIGHT))
    font = header + glyphs
    (out / "psf-test.psf").write_bytes(font)
    (out / "psf-test.psf.gz").write_bytes(gzip.compress(font, mtime=0))


if __name__ == "__main__":
    main()
