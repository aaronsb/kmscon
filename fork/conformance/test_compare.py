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
import shutil
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
              "cpus": 4, "memory_mb": 4096,
              "kmscon": ["Display x with backend [drm2d] text renderer [bbulk] font engine [pango]"]}}
TEXT = ["hello world", "", "the 4:3 curly line"]


def screen(paint=(), w=W, h=H):
    """A screen with a line of text-coloured pixels, then each (x, y, rgb)."""
    data = bytearray(BG * (w * h))
    for x in range(0, 110):
        data[(10 * w + x) * 3:(10 * w + x) * 3 + 3] = FG
    for x, y, rgb in paint:
        data[(y * w + x) * 3:(y * w + x) * 3 + 3] = rgb
    return t3img.Image(w, h, bytes(data))


# Two separate differences: row 1, columns 1-2, and row 3 over "4:3 curly".
CHANGED = [(x, 5, RED) for x in range(15)] + [(x, 45, RED) for x in range(40, 130)]


def write_run(path, screens, tools=TOOLS, meta=META, run_id=None):
    """A saved run: one tier 3 report and its PNGs, as compare.py save writes."""
    path = Path(path)
    (path / "t3").mkdir(parents=True)
    cases = []
    for name, img in screens.items():
        rel = "t3/%s.png" % compare.slug(name)
        t3img.write_png(str(path / rel), img)
        cases.append({"name": name, "status": "pass", "actual": rel, "text": TEXT,
                      "description": "A screen of text. Correct: every glyph in its cell.",
                      "wrong": "A glyph cut at *one* edge <or> in two_cells.",
                      "stats": {"pixels": 0, "bbox": [], "max_delta": 0}})
    report = {"tier": 3, "run_id": run_id or "run-" + Path(path).name, "tools": tools,
              "variants": meta, "cases": cases}
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
        # The caption's markdown characters show as themselves.
        self.assertIn("**What wrong looks like:** A glyph cut at \\*one\\* edge \\<or\\> in two\\_cells.",
                      page)
        self.assertIn("**Which is correct:** *not yet judged*", page)
        strip = read_png("out/v-curly-strip.png")
        self.assertIn(pageimg.OUTLINE_RGB, strip.data)
        self.assertIn(pageimg.DIFF_RGB, strip.data)

    def test_merged_regions_footnoted(self):
        # 52 specks 32 pixels apart are more areas than the regions shown.
        specks = [(x, y, RED) for x in range(5, 400, 32) for y in range(5, 100, 32)]
        write_run("ma", {"v/curly": screen(w=400, h=100)})
        write_run("mb", {"v/curly": screen(specks, w=400, h=100)})
        page = self.page("out", "--a", "ma", "--b", "mb")
        self.assertIn("in 16 regions |", page)
        self.assertIn("*More than 16 separate areas differ, so nearby ones are merged", page)
        self.assertNotIn("separate areas differ", self.page())

    def test_verdicts(self):
        page = self.page("out", "--verdicts", "verdicts")
        self.assertIn("**Which is correct:** upstream is right; the red is stray.", page)

    def test_reproducibility(self):
        page = self.page("out", "--a2", "a2", "--b2", "b2")
        self.assertIn("| Screen | Display | upstream: run 1 vs run 2 | fork: run 1 vs run 2 |", page)
        self.assertIn("| `same` | 200×60, bbulk | identical (0 px) | **3 px differ** |", page)
        self.assertIn("| `curly` | 200×60, bbulk | identical (0 px) | identical (0 px) |", page)
        self.assertIn("Each build was captured once.", self.page())

    def test_second_run_is_another_run(self):
        # The same run saved twice is refused as its own second run; a run
        # without an id is flagged.
        shutil.copytree("a", "a-again")
        with self.assertRaises(SystemExit) as e:
            self.page("out", "--a2", "a-again")
        self.assertIn("--a2 holds the same tier 3 run (run-a) as --a", str(e.exception))
        with self.assertRaises(SystemExit):
            self.page("out", "--b2", "b")
        rep = json.loads(Path("b2/tier3-report.json").read_text())
        del rep["run_id"]
        Path("b2/tier3-report.json").write_text(json.dumps(rep))
        page = self.page("out", "--a2", "a2", "--b2", "b2")
        self.assertIn("The reports of fork carry no run id", page)

    def test_header_and_footer(self):
        page = self.page()
        self.assertIn("| kmscon commit | [`1111111`](https://github.com/aaronsb/kmscon/commit/1111111) |", page)
        # A report that names its repository links there.
        write_run("up", {"v/same": screen()}, tools=dict(TOOLS, kmscon_repo="https://github.com/kmscon/kmscon",
                                                          libtsm_repo="https://github.com/kmscon/libtsm"))
        up = self.page("out", "--a", "up")
        self.assertIn("| kmscon commit | [`1111111`](https://github.com/kmscon/kmscon/commit/1111111) | "
                      "[`1111111`](https://github.com/aaronsb/kmscon/commit/1111111) |", up)
        self.assertIn("| libtsm commit | [`2222222`](https://github.com/kmscon/libtsm/commit/2222222) |", up)
        self.assertIn("| Text renderer | bbulk | bbulk |", page)
        self.assertIn("`cloudimg.qcow2` (SHA-256 `abc123`)", page)
        self.assertIn("QEMU device `virtio-vga,xres=200`. Machine: 4 vCPUs, 4096 MB. kmscon "
                      "configuration: `font-size=16`. Character cell 10×20 pixels.", page)
        self.assertIn("container image `archlinux@sha256:feed`", page)
        self.assertIn("- **This page:** `make sheet-compare A=a B=b`", page)
        self.assertNotIn("golden", page.lower())

    def test_machines_differ(self):
        # The same machine is claimed only when both runs recorded it; a
        # difference shows each run's value, and regions fall back to pixels
        # when the cells differ.
        self.assertIn("installed in the same virtual machine", self.page())
        tools = dict(TOOLS, guest_image="other.qcow2")
        meta = {"v": dict(META["v"], device="virtio-gpu", cell=[8, 16], cpus=1, memory_mb=512)}
        write_run("c", {"v/curly": screen(CHANGED)}, tools=tools, meta=meta)
        page = self.page("out", "--b", "c")
        self.assertNotIn("same virtual machine", page)
        self.assertIn("the two runs' machines or displays differ", page)
        self.assertIn("cloud image `cloudimg.qcow2` for upstream, `other.qcow2` for fork "
                      "(SHA-256 `abc123`)", page)
        self.assertIn("QEMU device `virtio-vga,xres=200` for upstream, `virtio-gpu` for fork. "
                      "Machine: 4 vCPUs, 4096 MB for upstream, 1 vCPU, 512 MB for fork. "
                      "kmscon configuration: `font-size=16`. Character cell 10×20 pixels for "
                      "upstream, 8×16 pixels for fork.", page)
        self.assertIn("| 1 | pixels 0,5 to 15,6 |", page)
        self.assertIn("the runs record different character cells, 10×20 and 8×16.", page)

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

    def test_save_refuses_tier2_beside_another_build(self):
        # Tier 2 is always the fork's renderer, so it is never saved with
        # upstream's tier 3 run, where it would read as upstream's.
        Path("build").mkdir()
        rep = json.loads(Path("a/tier3-report.json").read_text())
        rep["tools"] = dict(TOOLS, kmscon_repo="https://github.com/kmscon/kmscon")
        Path("build/tier3-report.json").write_text(json.dumps(rep))
        Path("build/tier2-report.json").write_text(json.dumps({"tier": 2, "cases": []}))
        with self.assertRaises(SystemExit) as e:
            compare.main(["save", "--out", "saved", "build/tier2-report.json", "build/tier3-report.json"])
        self.assertIn("save tier 3 alone", str(e.exception))
        self.assertFalse(Path("saved").exists())
        with contextlib.redirect_stdout(io.StringIO()):
            compare.main(["save", "--out", "saved", "build/tier3-report.json"])
        self.assertEqual(json.loads(Path("saved/tier3-report.json").read_text())["tools"]["kmscon_repo"],
                         "https://github.com/kmscon/kmscon")

    def test_refuses_foreign_output_dir(self):
        # Each refusal deletes nothing: not the checkout (out "."), not a
        # directory holding an input, not one compare.py did not write.
        Path("build").mkdir()
        Path("build/tier3-report.json").write_text(Path("a/tier3-report.json").read_text())
        Path("mine").mkdir()
        Path("mine/notes.png").write_bytes(b"x")
        Path("mine/README.md").write_text("mine\n")

        def files():
            return sorted((str(p), p.stat().st_size) for p in Path(".").rglob("*"))
        before = files()
        for out in (".", "build", "mine"):
            with self.assertRaises(SystemExit, msg=out) as e, contextlib.redirect_stdout(io.StringIO()):
                compare.main(["save", "--out", out, "build/tier3-report.json"])
            self.assertIn("refusing to write into %s" % out, str(e.exception))
            self.assertEqual(files(), before, out)
        for out in (".", "a", "mine"):
            with self.assertRaises(SystemExit, msg=out) as e:
                self.page(out)
            self.assertIn("refusing to write into %s" % out, str(e.exception))
            self.assertEqual(files(), before, out)
        # Its own directories, marked, are rewritten.
        for _ in range(2):
            with contextlib.redirect_stdout(io.StringIO()):
                compare.main(["save", "--out", "saved", "build/tier3-report.json"])
            Path("saved/stale").write_text("")
            self.page("out")
            Path("out/stale.png").write_bytes(b"")
        with contextlib.redirect_stdout(io.StringIO()):
            compare.main(["save", "--out", "saved", "build/tier3-report.json"])
        self.page("out")
        self.assertFalse(Path("saved/stale").exists() or Path("out/stale.png").exists())

    def test_wide_characters(self):
        rows = compare.screen_rows(["a日b", "0123456789abc"], 10)
        self.assertEqual(rows[0], [(0, "a"), (1, "日"), (3, "b")])
        self.assertEqual(rows[2], [(0, "a"), (1, "b"), (2, "c")])     # wrapped at 10 cells
        self.assertEqual(compare.cells((10, 0, 30, 20), (10, 20), rows, (100, 40)), ("row 1, cols 2-3", "日"))
        # Either half of a wide character names it.
        self.assertEqual(compare.cells((20, 0, 30, 20), (10, 20), rows, (100, 40)), ("row 1, col 3", "日"))
        self.assertEqual(compare.cells((10, 0, 20, 20), (10, 20), rows, (100, 40)), ("row 1, col 2", "日"))

    def test_pipes_escaped_in_tables(self):
        self.assertEqual(compare.code("|一|二|"), "`\\|一\\|二\\|`")
        self.assertEqual(compare.code("a`|b"), "`` a`\\|b ``")

    def test_grid_offset(self):
        # kmscon centres the grid: 205×63 in 10×19 cells is 20 columns by 3
        # rows, starting at pixel 2,3. A change at the foot of row 3, as an
        # underline would make, is in row 3, not row 4.
        meta = {"v": dict(META["v"], cell=[10, 19])}
        under = [(x, 3 + 2 * 19 + 17, RED) for x in range(2 + 40, 2 + 130)]
        write_run("ga", {"v/curly": screen(w=205, h=63)}, meta=meta)
        write_run("gb", {"v/curly": screen(under, w=205, h=63)}, meta=meta)
        page = self.page("out", "--a", "ga", "--b", "gb")
        self.assertIn("| 1 | row 3, cols 5-13 | 90 | 192 | `4:3 curly` |", page)
        self.assertIn("offset 2,3 here", page)
        self.assertEqual(compare.cells((0, 0, 2, 63), (10, 19), [], (205, 63)),
                         ("the margin around the character grid", ""))


if __name__ == "__main__":
    unittest.main()
