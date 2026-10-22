#!/usr/bin/env python3
"""Unit tests of compare.py on fixture runs: identical and differing pairs,
regions named by terminal cell and text, screens on one side only, the
second-run reproducibility check, saving a run, and byte-identical output.

  python3 fork/conformance/test_compare.py

SPDX-License-Identifier: MIT
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare  # noqa: E402
import pageimg  # noqa: E402
from pageimg import t3img  # noqa: E402
from test_runpage import read_png  # noqa: E402

W, H = 200, 60                      # 20 columns by 3 rows of 10×20 cells
BG, FG, RED = b"\x10\x12\x16", b"\xd0\xd0\xd0", b"\xd0\x00\x00"
TOOLS = {"kmscon": "1111111", "libtsm": "2222222", "image_base": "archlinux@sha256:feed",
         "archive_date": "2026/10/21", "guest_image": "cloudimg.qcow2",
         "guest_image_sha256": "abc123", "guest_archive": "2026/10/20"}
META = {"v": {"cell": [10, 20], "device": "virtio-vga,xres=200", "config": ["font-size=16"],
              "kmscon": ["Display x with backend [drm2d] text renderer [bbulk] font engine [pango]"]}}
TEXT = ["hello world", "", "the 4:3 curly line"]


def screen(paint=()):
    """A screen with a line of text-coloured pixels, then each (x, y, rgb)."""
    data = bytearray(BG * (W * H))
    for x in range(0, 110):
        data[(10 * W + x) * 3:(10 * W + x) * 3 + 3] = FG
    for x, y, rgb in paint:
        data[(y * W + x) * 3:(y * W + x) * 3 + 3] = rgb
    return t3img.Image(W, H, bytes(data))


# Two separate differences: row 1, columns 1-2, and row 3 over "4:3 curly".
CHANGED = [(x, 5, RED) for x in range(15)] + [(x, 45, RED) for x in range(40, 130)]


def write_run(path, screens, tools=TOOLS):
    """A saved run: one tier 3 report and its PNGs, as compare.py save writes."""
    path = Path(path)
    (path / "t3").mkdir(parents=True)
    cases = []
    for name, img in screens.items():
        rel = "t3/%s.png" % compare.slug(name)
        t3img.write_png(str(path / rel), img)
        cases.append({"name": name, "status": "pass", "actual": rel, "text": TEXT,
                      "description": "A screen of text. Correct: every glyph in its cell.",
                      "stats": {"pixels": 0, "bbox": [], "max_delta": 0}})
    report = {"tier": 3, "tools": tools, "variants": META, "cases": cases}
    (path / "tier3-report.json").write_text(json.dumps(report))


class CompareTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cwd = os.getcwd()
        os.chdir(self.tmp.name)
        write_run("a", {"v/same": screen(), "v/curly": screen(), "v/onlya": screen()})
        write_run("b", {"v/same": screen(), "v/curly": screen(CHANGED), "v/onlyb": screen()})
        write_run("a2", {"v/same": screen(), "v/curly": screen(), "v/onlya": screen()})
        write_run("b2", {"v/same": screen([(1, 1, RED), (2, 1, RED), (3, 1, RED)]),
                         "v/curly": screen(CHANGED), "v/onlyb": screen()})
        Path("verdicts").write_text("# screen: verdict\ncurly: upstream is right; the red is stray.\n")

    def tearDown(self):
        os.chdir(self.cwd)
        self.tmp.cleanup()

    def page(self, out="out", *extra):
        argv = ["page", "--out", out, "--a", "a", "--b", "b", "--label-a", "upstream",
                "--label-b", "fork", "--harness", "3333333", "--command", "make sheet-compare A=a B=b",
                *extra]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(compare.main(argv), 0)
        return Path(out, "README.md").read_text()

    def test_pairs(self):
        page = self.page()
        self.assertIn("| [`same`](#same-200x60-bbulk) | 200×60, bbulk | identical |", page)
        self.assertIn("| [`curly`](#curly-200x60-bbulk) | 200×60, bbulk | differ: 105 pixels in 2 regions |", page)
        self.assertIn("Of the 2 screens both builds drew: 1 identical, 1 different.", page)
        self.assertIn("| `v/onlya` | upstream | fork |", page)
        self.assertIn("| `v/onlyb` | fork | upstream |", page)
        self.assertIn("Identical: both builds drew exactly the same pixels.", page)
        self.assertEqual(sorted(p.name for p in Path("out").glob("*.png")),
                         ["v-curly-strip.png", "v-same.png"])

    def test_regions_named_by_cell_and_text(self):
        page = self.page()
        self.assertIn("| 1 | row 1, cols 1-2 | 15 | 192 | `he` |", page)
        self.assertIn("| 2 | row 3, cols 5-13 | 90 | 192 | `4:3 curly` |", page)
        self.assertIn("**What correct looks like:** every glyph in its cell.", page)
        self.assertIn("**Which is correct:** *not yet judged*", page)
        strip = read_png("out/v-curly-strip.png")
        self.assertIn(pageimg.OUTLINE_RGB, strip.data)
        self.assertIn(pageimg.DIFF_RGB, strip.data)

    def test_verdicts(self):
        page = self.page("out", "--verdicts", "verdicts")
        self.assertIn("**Which is correct:** upstream is right; the red is stray.", page)

    def test_reproducibility(self):
        page = self.page("out", "--a2", "a2", "--b2", "b2")
        self.assertIn("| Screen | Display | upstream: run 1 vs run 2 | fork: run 1 vs run 2 |", page)
        self.assertIn("| `same` | 200×60, bbulk | identical (0 px) | **3 px differ** |", page)
        self.assertIn("| `curly` | 200×60, bbulk | identical (0 px) | identical (0 px) |", page)
        self.assertIn("Each build was captured once.", self.page())

    def test_header_and_footer(self):
        page = self.page()
        self.assertIn("| kmscon commit | [`1111111`](https://github.com/aaronsb/kmscon/commit/1111111) |", page)
        self.assertIn("| Text renderer | bbulk | bbulk |", page)
        self.assertIn("`cloudimg.qcow2` (SHA-256 `abc123`)", page)
        self.assertIn("QEMU device `virtio-vga,xres=200`. kmscon configuration: `font-size=16`. "
                      "Character cell 10×20 pixels.", page)
        self.assertIn("container image `archlinux@sha256:feed`", page)
        self.assertIn("- **This page:** `make sheet-compare A=a B=b`", page)
        self.assertNotIn("golden", page.lower())

    def test_deterministic(self):
        def snapshot():
            self.page("out", "--a2", "a2", "--b2", "b2")
            return {p.name: p.read_bytes() for p in sorted(Path("out").iterdir())}
        self.assertEqual(snapshot(), snapshot())

    def test_save(self):
        # A report as a run leaves it in the build directory, paths from the root.
        Path("build/t3").mkdir(parents=True)
        t3img.write_png("build/t3/x.png", screen())
        Path("build/tier3-report.json").write_text(json.dumps({"tier": 3, "tools": TOOLS, "variants": META,
            "cases": [{"name": "v/x", "status": "pass", "actual": "build/t3/x.png",
                       "golden": "g.png", "thumb_actual": "t.png", "stats": {}},
                      {"name": "v/lost", "status": "fail", "actual": None, "error": "no capture"}]}))
        with contextlib.redirect_stdout(io.StringIO()):
            compare.main(["save", "--out", "saved", "build/tier3-report.json"])
        rep = json.loads(Path("saved/tier3-report.json").read_text())
        self.assertEqual([c["actual"] for c in rep["cases"]], ["t3/v-x.png", None])
        self.assertNotIn("golden", rep["cases"][0])
        self.assertEqual(t3img.read("saved/t3/v-x.png").data, screen().data)
        run = compare.Run("saved")
        self.assertEqual(run.image((3, "v/x")).data, screen().data)
        self.assertIsNone(run.image((3, "v/lost")))

    def test_wide_characters(self):
        rows = compare.screen_rows(["a日b", "0123456789abc"], 10)
        self.assertEqual(rows[0], [(0, "a"), (1, "日"), (3, "b")])
        self.assertEqual(rows[2], [(0, "a"), (1, "b"), (2, "c")])     # wrapped at 10 cells
        self.assertEqual(compare.cells((10, 0, 30, 20), (10, 20), rows), ("row 1, cols 2-3", "日"))


if __name__ == "__main__":
    unittest.main()
