#!/usr/bin/env python3
"""Unit tests of runpage.py on fixture reports: a pass, a failure, a case
whose golden changed, verdicts that moved since the previous run, and
byte-identical output from identical inputs.

  python3 fork/conformance/test_runpage.py

SPDX-License-Identifier: MIT
"""

import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import runpage  # noqa: E402
from runpage import t3img  # noqa: E402

BG, FG = b"\x10\x12\x16", b"\xd0\xd0\xd0"


def screen(w, h, marks):
    """A w×h screen on the background colour with FG pixels at `marks`."""
    data = bytearray(BG * (w * h))
    for x, y in marks:
        data[(y * w + x) * 3:(y * w + x) * 3 + 3] = FG
    return t3img.Image(w, h, bytes(data))


def read_png(path):
    """An image from a PNG runpage wrote: palette or RGB, rows unfiltered."""
    buf, off, idat = Path(path).read_bytes(), 8, b""
    while off < len(buf):
        n, kind = struct.unpack(">I4s", buf[off:off + 8])
        body = buf[off + 8:off + 8 + n]
        off += 12 + n
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"PLTE":
            plte = [body[i:i + 3] for i in range(0, n, 3)]
        elif kind == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    bpp = 1 if ctype == 3 else 3
    rows = [raw[y * (w * bpp + 1):(y + 1) * (w * bpp + 1)] for y in range(h)]
    assert all(r[0] == 0 for r in rows)
    data = b"".join(b"".join(plte[i] for i in r[1:]) if ctype == 3 else r[1:] for r in rows)
    return t3img.Image(w, h, data)


def write_ppm(path, img):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(b"P6\n%d %d\n255\n" % (img.width, img.height) + img.data)


def case(name, status, golden, actual, pixels=0, **extra):
    c = {"name": name, "status": status, "golden": golden, "actual": actual, "diff": None,
         "tolerance": None, "stats": {"pixels": pixels, "bbox": [], "max_delta": 255 if pixels else 0}}
    c.update(extra)
    return c


class Fixture:
    """A repository root holding reports, images, a ledger with one previous
    run, its per-case record, and the golden history since."""

    def __init__(self, root):
        self.root = root
        line = [(x, 20) for x in range(10, 40)]
        g = "tests/goldens/tier2/"
        write_ppm(g + "same.ppm", screen(200, 100, line))
        write_ppm("out/same.ppm", screen(200, 100, line))
        write_ppm(g + "broken.ppm", screen(200, 100, line))
        write_ppm("out/broken.ppm", screen(200, 100, line[:-5] + [(50, 60)]))
        write_ppm(g + "moved.ppm", screen(200, 100, [(x, 30) for x in range(10, 40)]))
        write_ppm("out/moved.ppm", screen(200, 100, [(x, 30) for x in range(10, 40)]))
        write_ppm("history/kmscon/" + g + "moved.ppm", screen(200, 100, line))

        tier1 = {"tier": 1, "cases": [
            {"name": "t1-ok", "cols": 4, "rows": 1, "status": "pass",
             "golden": "row 0 |a|\n", "actual": "row 0 |a|\n"},
            {"name": "t1-bad", "cols": 4, "rows": 1, "status": "fail",
             "golden": "row 0 |a|\n", "actual": "row 0 |b|\n"},
            {"name": "t1-fixed", "cols": 4, "rows": 1, "status": "pass",
             "golden": "row 0 |c|\n", "actual": "row 0 |c|\n"}]}
        tier2 = {"tier": 2, "tools": {"image_base": "archlinux@sha256:0123456789abcdef", "archive_date":
                                      "2026/10/21", "packages": {"freetype2": "2.15.0-1"}}, "cases": [
            case("same", "pass", g + "same.ppm", "out/same.ppm", description="A line of text."),
            case("broken", "fail", g + "broken.ppm", "out/broken.ppm", pixels=6, description="Broken."),
            case("moved", "pass", g + "moved.ppm", "out/moved.ppm", description="Moved down.")]}
        Path("tier1.json").write_text(json.dumps(tier1))
        Path("tier2.json").write_text(json.dumps(tier2))

        Path("ledger").mkdir()
        Path("ledger/runs/prev").mkdir(parents=True)
        Path("ledger/LEDGER.md").write_text(runpage.LEDGER_HEAD + (
            "| 2026-10-20 | Earlier | `aaaaaaa` | `bbbbbbb` | 1/2 | 3/3 | — | "
            "[prev](runs/prev/README.md) |\n"))
        Path("ledger/runs/prev/README.md").write_text("# earlier\n")
        Path("ledger/runs/prev/cases.json").write_text(json.dumps({"tiers": {
            "1": {"t1-ok": "pass", "t1-bad": "pass", "t1-fixed": "fail"},
            "2": {"same": "pass", "broken": "pass", "moved": "pass", "gone": "pass"}}}))
        Path("history/changes.tsv").write_text(
            "previous\taaaaaaa\tbbbbbbb\nkmscon\tM\t%smoved.ppm\n" % g)
        Path("fork-goldens.list").write_text("# header\nt1-ok   # kept on purpose\n")

    def run(self, out, *extra):
        argv = ["--out", out, "--kmscon", "ccccccc", "--libtsm", "ddddddd", "--date", "2026-10-22",
                "--ledger", "ledger/LEDGER.md", "--history", "history",
                "--fork-goldens", "fork-goldens.list", *extra, "tier1.json", "tier2.json"]
        with contextlib.redirect_stdout(io.StringIO()):
            rc = runpage.main(argv)
        assert rc == 0
        return Path(out, "README.md").read_text()


class RunPageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cwd = os.getcwd()
        os.chdir(self.tmp.name)
        self.fx = Fixture(self.tmp.name)

    def tearDown(self):
        os.chdir(self.cwd)
        self.tmp.cleanup()

    def test_since_previous_run(self):
        page = self.fx.run("ledger/runs/now")
        self.assertIn("- **Newly failing:** tier 1: [`t1-bad`](#t1-bad-tier-1-fail); "
                      "tier 2: [`broken`](#broken-tier-2-fail).", page)
        self.assertIn("- **Newly passing:** tier 1: [`t1-fixed`](#t1-fixed-tier-1-pass).", page)
        self.assertIn("- **Removed cases:** tier 2: `gone`.", page)
        self.assertIn("- **Goldens changed:** tier 2: [`moved`](#moved-tier-2-pass).", page)
        self.assertIn("- **New cases:** none.", page)
        self.assertIn("[2026-10-20](../prev/README.md), \"Earlier\"", page)
        self.assertIn("| 1 | 1 of 2 | 2 of 3 |", page)

    def test_tier1_text(self):
        page = self.fx.run("ledger/runs/now")
        self.assertIn("| `t1-ok` | 4×1 | pass | fork: kept on purpose |", page)
        self.assertIn("| [`t1-bad`](#t1-bad-tier-1-fail) | 4×1 | FAIL | upstream |", page)
        self.assertIn("### `t1-bad` (tier 1): FAIL", page)
        self.assertIn("-row 0 |a|\n+row 0 |b|", page)
        self.assertNotIn(".png", page.split("## Tier 2")[0])

    def test_images(self):
        page = self.fx.run("ledger/runs/now")
        out = Path("ledger/runs/now")
        # A pass shows one thumbnail, a failure and a changed golden a strip.
        self.assertIn("![t2-same](t2-same.png)", page)
        self.assertIn("(t2-broken-strip.png)", page)
        self.assertIn("(t2-moved-golden-strip.png)", page)
        self.assertNotIn("(t2-moved.png)", page)
        self.assertEqual(sorted(p.name for p in out.glob("*.png")),
                         ["t2-broken-strip.png", "t2-moved-golden-strip.png", "t2-same.png"])
        self.assertIn("6 pixels differ, by at most 255 per channel.", page)
        self.assertIn("A line of text.", page)
        # Cropped to the drawn region with its margin, not the whole screen.
        thumb = read_png(out / "t2-same.png")
        self.assertEqual((thumb.width, thumb.height), (40 - 10 + 2 * runpage.MARGIN, 1 + 2 * runpage.MARGIN))
        strip = read_png(out / "t2-broken-strip.png")
        self.assertIn(runpage.DIFF_RGB, strip.data)

    def test_ledger_row(self):
        self.fx.run("ledger/runs/2026-10-22-ccccccc-ddddddd", "--append", "--note", "a | note")
        last = Path("ledger/LEDGER.md").read_text().splitlines()[-1]
        self.assertEqual(last, "| 2026-10-22 | a \\| note | `ccccccc` | `ddddddd` | 2/3 | 2/3 | — | "
                               "[2026-10-22-ccccccc-ddddddd](runs/2026-10-22-ccccccc-ddddddd/README.md) |")
        self.assertEqual(runpage.ledger_rows("ledger/LEDGER.md")[-1]["note"], "a | note")

    def test_deterministic(self):
        def snapshot():
            self.fx.run("ledger/runs/now")
            return {p.name: p.read_bytes() for p in sorted(Path("ledger/runs/now").iterdir())}
        first = snapshot()
        self.assertEqual(first, snapshot())
        self.assertIn("cases.json", first)

    def test_large_screen_scaled(self):
        # Content wider than MAX_W is kept one pixel in two, and a single
        # differing pixel still marks its block.
        want = screen(3000, 50, [(x, 10) for x in range(0, 2500)])
        got = screen(3000, 50, [(x, 10) for x in range(0, 2500)] + [(1001, 40)])
        box = runpage.crop_box([want, got])
        step = runpage.step_for(box)
        self.assertEqual(step, 3)
        img, n = runpage.diff_image(want, got, box, step)
        self.assertEqual(n, 1)
        self.assertLessEqual(img.width, runpage.MAX_W)


if __name__ == "__main__":
    unittest.main()
