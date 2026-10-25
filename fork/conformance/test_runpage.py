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
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import runpage  # noqa: E402
from pageimg import t3img  # noqa: E402
import pageimg  # noqa: E402

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
            case("broken", "fail", g + "broken.ppm", "out/broken.ppm", pixels=6, description="Broken.",
                 wrong="A gap between *the* cells."),
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
        self.assertIn("- **Newly skipped:** none.", page)
        self.assertIn("- **Removed cases:** tier 2: `gone`.", page)
        self.assertIn("- **Goldens changed:** tier 2: [`moved`](#moved-tier-2-pass).", page)
        self.assertIn("- **New cases:** none.", page)
        self.assertIn("[2026-10-20](../prev/README.md), \"Earlier\"", page)
        self.assertIn("| 1 | 1 of 2 | 2 of 3 |", page)

    def test_skipped_is_not_failing(self):
        # A case a variant does not draw is listed as skipped, never as
        # failing, though it passed before.
        t2 = json.loads(Path("tier2.json").read_text())
        for c in t2["cases"]:
            if c["name"] == "same":
                c.update(status="skipped", actual=None, error="not drawn")
        Path("tier2.json").write_text(json.dumps(t2))
        page = self.fx.run("ledger/runs/now")
        self.assertIn("- **Newly skipped:** tier 2: [`same`](#same-tier-2-skipped).", page)
        self.assertIn("- **Newly failing:** tier 1: [`t1-bad`](#t1-bad-tier-1-fail); "
                      "tier 2: [`broken`](#broken-tier-2-fail).", page)

    def test_previous_run_by_url(self):
        # A run whose sheet left the tree: its row links to GitHub and it
        # left no per-case record, so verdicts then come from its counts.
        url = "https://github.com/aaronsb/kmscon/blob/627f3d1/docs/conformance/sheets/old.svg"
        Path("ledger/LEDGER.md").write_text(runpage.LEDGER_HEAD + (
            "| 2026-10-20 | Earlier | `aaaaaaa` | `bbbbbbb` | 3/3 | 3/3 | — | [old.svg](%s) |\n" % url))
        page = self.fx.run("ledger/runs/now")
        self.assertIn("[2026-10-20](%s), \"Earlier\"" % url, page)
        self.assertIn("- **Newly failing:** tier 1: [`t1-bad`](#t1-bad-tier-1-fail); "
                      "tier 2: [`broken`](#broken-tier-2-fail).", page)

    def test_tier_not_run(self):
        # Tier 2 only then, tier 3 only now: neither tier's cases are new
        # or removed.
        Path("tier3.json").write_text(json.dumps({"tier": 3, "variants": {}, "cases": [
            case("v/x", "pass", None, None)]}))
        argv = ["--out", "ledger/runs/now", "--kmscon", "ccccccc", "--libtsm", "ddddddd",
                "--date", "2026-10-22", "--ledger", "ledger/LEDGER.md", "--history", "history",
                "tier1.json", "tier3.json"]
        with contextlib.redirect_stdout(io.StringIO()):
            runpage.main(argv)
        page = Path("ledger/runs/now/README.md").read_text()
        self.assertIn("- **New cases:** none.", page)
        self.assertIn("- **Removed cases:** none.", page)
        self.assertIn("Tier 3 was not run then, so its cases are not compared.", page)
        self.assertIn("Tier 2 is not run now, so its cases are not compared.", page)
        self.assertIn("| 3 | not run | 1 of 1 |", page)

    def test_variant_machine_and_bench(self):
        # A tier 3 variant names its machine and each benchmark's figures,
        # or why it has none.
        bench = {"lines": 20000, "seconds": 2.5, "lines_per_s": 8000, "intervals": 3,
                 "flips": 150, "frames": 150, "presented_per_s": 60.0, "skipped": 0,
                 "cpu_ms_mean": 1.25, "cpu_ms_median": 1.2, "cpu_ms_p95": 2.0,
                 "cpu_ms_max": 3.5, "damage_px_mean": 1049088, "damage_pct": 100.0}
        Path("tier3.json").write_text(json.dumps({"tier": 3, "cases": [
            case("small/x", "pass", None, None), case("up/x", "pass", None, None)], "variants": {
                "small": {"cpus": 1, "memory_mb": 512, "bench": {"s1-scroll": bench}},
                "up": {"cpus": 4, "memory_mb": 4096, "bench": {"s1-scroll": {
                    "lines": 20000, "seconds": 4.0, "lines_per_s": 5000, "intervals": 0,
                    "note": "this build has no render timer"}}}}}))
        argv = ["--out", "ledger/runs/now", "--kmscon", "ccccccc", "--libtsm", "ddddddd",
                "--date", "2026-10-22", "--ledger", "ledger/LEDGER.md", "--history", "history",
                "tier1.json", "tier3.json"]
        with contextlib.redirect_stdout(io.StringIO()):
            runpage.main(argv)
        page = Path("ledger/runs/now/README.md").read_text()
        self.assertIn("Its machine had 1 vCPU and 512 MB of memory.", page)
        self.assertIn("Its machine had 4 vCPUs and 4096 MB of memory.", page)
        self.assertIn("Benchmark `s1-scroll`: 20,000 lines in 2.50 s, 8,000 lines per second; "
                      "60.0 frames presented per second; kmscon's CPU time per frame 1.250 ms "
                      "mean, 1.200 ms median, 2.000 ms at the 95th percentile, 3.500 ms at most; "
                      "1,049,088 pixels damaged per frame, 100.00% of the screen.", page)
        self.assertIn("Benchmark `s1-scroll`: 20,000 lines in 4.00 s, 5,000 lines per second; "
                      "no render-timer figures: this build has no render timer.", page)
        self.assertEqual(runpage.bench_line("b", {"error": "no line"}),
                         "Benchmark `b`: not measured: no line.")
        # A screen that plays video names mpv's frames and kmscon's whole CPU
        # time, and the redraws that presented nothing.
        mpv = {"seconds": 5.15, "intervals": 6, "flips": 304, "frames": 304,
               "presented_per_s": 59.0, "skipped": 120, "skipped_cpu_ms": 51.5,
               "skipped_cpu_pct": 1.0, "cpu_ms_mean": 0.6, "cpu_ms_median": 0.589,
               "cpu_ms_p95": 1.195, "cpu_ms_max": 1.232, "damage_px_mean": 925000,
               "damage_pct": 90.4, "mpv_frames": 150, "mpv_dropped": 0, "mpv_per_s": 29.1,
               "kmscon_cpu_ms": 1010, "kmscon_cpu_pct": 19.6,
               "kmscon_cpu_ms_per_mpv_frame": 6.733}
        self.assertEqual(runpage.bench_line("mpv-sixel", mpv),
                         "Benchmark `mpv-sixel`: mpv showed 150 frames and dropped 0 in 5.15 s, "
                         "29.1 per second; kmscon used 19.6% of a CPU, 6.733 ms per frame mpv "
                         "sent, parsing included; 59.0 frames presented per second; kmscon's "
                         "CPU time per frame 0.600 ms mean, 0.589 ms median, 1.195 ms at the "
                         "95th percentile, 1.232 ms at most; 925,000 pixels damaged per frame, "
                         "90.40% of the screen; 120 redraws presented nothing, 51.5 ms of CPU "
                         "time.")

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
        # A screen's wrong-looks-like guidance follows its caption; a case without
        # it shows none.
        self.assertIn("Broken.\n\nWrong looks like: A gap between \\*the\\* cells.\n", page)
        self.assertEqual(page.count("Wrong looks like:"), 1)
        # Cropped to the drawn region with its margin, not the whole screen.
        thumb = read_png(out / "t2-same.png")
        self.assertEqual((thumb.width, thumb.height), (40 - 10 + 2 * pageimg.MARGIN, 1 + 2 * pageimg.MARGIN))
        strip = read_png(out / "t2-broken-strip.png")
        self.assertIn(pageimg.DIFF_RGB, strip.data)

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

    def test_refuses_foreign_output_dir(self):
        # Each refusal deletes nothing: not the checkout (out "."), not a
        # directory holding an input, not one runpage did not write.
        def files():
            return sorted(str(p) for p in Path(".").rglob("*"))
        before = files()
        for out in (".", "ledger", "history", "out"):
            with self.assertRaises(SystemExit, msg=out) as e:
                self.fx.run(out)
            self.assertIn("refusing to write into %s" % out, str(e.exception))
            self.assertEqual(files(), before, out)
        # Its own directory, marked, is rewritten.
        self.fx.run("ledger/runs/now")
        Path("ledger/runs/now/stale.png").write_bytes(b"")
        self.fx.run("ledger/runs/now")
        self.assertFalse(Path("ledger/runs/now/stale.png").exists())

    def test_report_from_other_commits(self):
        # A tier 3 report left by a run of another commit, or of a dirty
        # tree, is named on a preview and refused for a ledger row.
        Path("tier3.json").write_text(json.dumps({"tier": 3, "tools": {
            "kmscon": "eeeeeee+dirty", "libtsm": "ddddddd+dirty"}, "variants": {}, "cases": []}))
        argv = ["tier3.json"]
        page = self.fx.run("ledger/runs/now", *argv)
        self.assertIn("**Not what this page names:** the tier 3 report measured kmscon `eeeeeee`, "
                      "not `ccccccc`; the tier 3 report measured libtsm `ddddddd` with uncommitted "
                      "changes.", page)
        self.assertNotIn("Not what this page names", self.fx.run("ledger/runs/now"))
        before = Path("ledger/LEDGER.md").read_text()
        with self.assertRaises(SystemExit) as e:
            self.fx.run("ledger/runs/then", "--append", "--note", "n", *argv)
        self.assertIn("refusing a ledger row: the tier 3 report measured kmscon `eeeeeee`", str(e.exception))
        self.assertEqual(Path("ledger/LEDGER.md").read_text(), before)
        self.assertFalse(Path("ledger/runs/then").exists())

    def test_large_screen_scaled(self):
        # Content wider than MAX_W is kept one pixel in two, and a single
        # differing pixel still marks its block.
        want = screen(3000, 50, [(x, 10) for x in range(0, 2500)])
        got = screen(3000, 50, [(x, 10) for x in range(0, 2500)] + [(1001, 40)])
        box = pageimg.crop_box([want, got])
        step = pageimg.step_for(box)
        self.assertEqual(step, 3)
        img, n = pageimg.diff_image(want, got, box, step)
        self.assertEqual(n, 1)
        self.assertLessEqual(img.width, pageimg.MAX_W)


class GoldenHistoryTest(unittest.TestCase):
    """golden-history.sh in a scratch repository: the previous run's commit
    read from a ledger row whose note holds an escaped |, a modified and an
    untracked golden, and a failing git diff."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.git("init", "-q")
        golden = self.root / "tests/goldens/tier2"
        golden.mkdir(parents=True)
        (golden / "a.ppm").write_text("one\n")
        self.git("add", ".")
        self.git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "goldens")
        self.prev = self.git("rev-parse", "--short", "HEAD").strip()
        (golden / "a.ppm").write_text("two\n")
        (golden / "new.ppm").write_text("new\n")
        (self.root / "LEDGER.md").write_text(runpage.LEDGER_HEAD + (
            "| 2026-10-20 | a \\| b | `%s` | `1234567` | 1/1 | 1/1 | — | [r](r/README.md) |\n" % self.prev))

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True,
                              text=True).stdout

    def history(self, env=None):
        subprocess.run(["sh", str(HERE / "golden-history.sh"), "LEDGER.md", "hist"], cwd=self.root,
                       check=True, capture_output=True, env=env)
        return (self.root / "hist/changes.tsv").read_text().splitlines()

    def test_changes(self):
        lines = self.history()
        self.assertEqual(lines[0], "previous\t%s\t1234567" % self.prev)
        self.assertIn("kmscon\tM\ttests/goldens/tier2/a.ppm", lines)
        self.assertIn("kmscon\tA\ttests/goldens/tier2/new.ppm", lines)
        self.assertEqual((self.root / "hist/kmscon/tests/goldens/tier2/a.ppm").read_text(), "one\n")

    def test_failing_diff_is_not_no_change(self):
        # A git whose diff fails: the history is unreadable, not unchanged.
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        real = subprocess.run(["sh", "-c", "command -v git"], capture_output=True, text=True).stdout.strip()
        (bin_dir / "git").write_text('#!/bin/sh\ncase " $* " in *" diff "*) exit 128;; esac\nexec %s "$@"\n' % real)
        (bin_dir / "git").chmod(0o755)
        env = dict(os.environ, PATH="%s:%s" % (bin_dir, os.environ["PATH"]))
        lines = self.history(env)
        self.assertIn("kmscon\t?\tgit cannot list the changes since %s" % self.prev, lines)
        self.assertFalse(any(line.startswith("kmscon\tM") for line in lines))


if __name__ == "__main__":
    unittest.main()
