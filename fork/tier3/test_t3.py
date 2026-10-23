#!/usr/bin/env python3
"""Self-test for the parts of tier 3 that run on the host without a guest:
the caption and screen text a case's report carries (t3_evaluate.py), the
colours and cells of the sixel goldens, the packages a run installs and the
guest's account of them, and the machine each variant boots
(t3_scenario.py), the console lines the driver waits for and the QEMU
command line (t3_qemu.py), a benchmark screen's figures (t3_bench.py), and
the logger a worker process receives (t3_common.py).

    python3 -I fork/tier3/test_t3.py
"""

import contextlib
import io
import os
import pickle
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t3_bench  # noqa: E402
import t3_evaluate  # noqa: E402
import t3_qemu  # noqa: E402
import t3_scenario  # noqa: E402
import t3img  # noqa: E402
from t3_common import Logger  # noqa: E402

LOG = Logger()

SCRIPTS = {
    "colours": r"printf '\033[1;31mred\033[0m plain\nnext\n'",
    "fails": "echo partial; exit 3",
    "hangs": "sleep 5; echo late",
    "reads": 'read -r x; echo "got:$x"',
    "moves": r"printf 'a\033[2;5Hb\n'",
    "tab": r"printf 'a\tb\n'",
    "charset": r"printf '\033(0q\033(B\n'",
    "title": r"printf '\033]0;title\007x\n'",
    "scrolls": "# A screen.\n# screen-text: none, it scrolls.\necho never-run",
}


class ScreenTextTest(unittest.TestCase):
    def test_real_screens(self):
        self.assertTrue(t3_evaluate.screen_text("sgr", LOG)[2]
                        .startswith("bold underline reverse "))
        self.assertEqual(t3_evaluate.screen_text("cjk", LOG)[2], "|一|二|三|四|五|")
        self.assertEqual(t3_evaluate.screen_text("vt-restore", LOG),
                         t3_evaluate.screen_text(t3_evaluate.SCREENS[-1], LOG))
        self.assertIsNone(t3_evaluate.screen_text("vt-switch", LOG))
        # wide-margin moves the cursor to reach the screen's real edge,
        # s1-scroll prints 500 000 lines of which the screen shows the last,
        # and sixel and sixel-bound draw pictures, so they have no text on
        # purpose; every other screen has its rows.
        textless = ("wide-margin", "s1-scroll", "sixel", "sixel-bound")
        with contextlib.redirect_stderr(io.StringIO()):
            for name in textless:
                self.assertIsNone(t3_evaluate.screen_text(name, LOG))
        for name in t3_evaluate.SCREENS:
            if name not in textless:
                with self.subTest(name):
                    self.assertTrue(t3_evaluate.screen_text(name, LOG))
        # e and U+0301 between bars: one cell built from two code points.
        self.assertEqual(t3_evaluate.screen_text("combining", LOG)[1][27:31], "|e\u0301|")

    def test_cjk_last(self):
        # vt-restore compares the screen left on tty2 with the last screen's
        # golden; its caption and the tier 3 README name cjk. A screen added
        # after cjk would move that check without a word.
        self.assertEqual(t3_evaluate.SCREENS[-1], "cjk")
        self.assertIn("the cjk golden", t3_evaluate.describe("vt-restore")[0])

    def text(self, name):
        with tempfile.TemporaryDirectory() as guest, contextlib.redirect_stderr(io.StringIO()):
            os.mkdir(os.path.join(guest, "screens"))
            with open(os.path.join(guest, "screens", name + ".sh"), "w") as f:
                f.write(SCRIPTS[name] + "\n")
            saved = t3_evaluate.GUEST, t3_evaluate.SCREENS, t3_evaluate.SCREEN_TIMEOUT
            t3_evaluate.GUEST, t3_evaluate.SCREENS, t3_evaluate.SCREEN_TIMEOUT = guest, [name], 1
            try:
                return t3_evaluate.screen_text(name, LOG)
            finally:
                t3_evaluate.GUEST, t3_evaluate.SCREENS, t3_evaluate.SCREEN_TIMEOUT = saved

    def test_colours_removed(self):
        self.assertEqual(self.text("colours"), ["red plain", "next"])

    def test_failure_gives_none(self):
        self.assertIsNone(self.text("fails"))

    def test_hang_gives_none(self):
        self.assertIsNone(self.text("hangs"))

    def test_input_not_inherited(self):
        # The run's own stdin carries a line the script must not see.
        r, w = os.pipe()
        os.write(w, b"secret\n")
        os.close(w)
        saved = os.dup(0)
        os.dup2(r, 0)
        try:
            self.assertEqual(self.text("reads"), ["got:"])
        finally:
            os.dup2(saved, 0)
            os.close(saved)
            os.close(r)

    def test_marked_none_not_run(self):
        # The script would print a line; its header says its lines are not
        # its rows, so it is not run.
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertIsNone(self.text("scrolls"))
        self.assertNotIn("never-run", err.getvalue())

    def test_other_controls_give_none(self):
        # Each would make line N of the output something other than row N.
        for name in ("moves", "tab", "charset", "title"):
            with self.subTest(name):
                self.assertIsNone(self.text(name))


class DescribeTest(unittest.TestCase):
    def describe(self, header):
        with tempfile.TemporaryDirectory() as guest:
            os.mkdir(os.path.join(guest, "screens"))
            with open(os.path.join(guest, "screens", "s.sh"), "w") as f:
                f.write(header + "echo body\n# not header\n")
            saved = t3_evaluate.GUEST
            t3_evaluate.GUEST = guest
            try:
                return t3_evaluate.describe("s")
            finally:
                t3_evaluate.GUEST = saved

    def test_guidance(self):
        # The demo's guidance lines, in any order among the caption's, after
        # the interpreter line.
        self.assertEqual(self.describe("#!/bin/bash\n# look-for: Lines\n# look-for: joined.\n"
                                       "# wrong-looks-like: A gap.\n# A screen.\n"),
                         ("A screen. Correct: Lines joined.", "A gap."))

    def test_plain_caption(self):
        self.assertEqual(self.describe("# A screen. Correct: x: y.\n"),
                         ("A screen. Correct: x: y.", None))

    def test_check(self):
        self.assertEqual(t3_evaluate.describe("vt-switch"),
                         (t3_evaluate.CHECKS["vt-switch"], None))

    def test_screen_text_not_in_caption(self):
        self.assertEqual(self.describe("# A screen.\n# screen-text: none, it scrolls.\n"),
                         ("A screen.", None))
        caption, _ = t3_evaluate.describe("s1-scroll")
        self.assertTrue(caption.startswith("S1 (ADR-103): text scrolling"))
        self.assertNotIn("screen-text", caption)
        self.assertIn("Correct: Numbered lines", caption)

    def test_benchmarks_are_screens(self):
        # A benchmark is drawn and compared like any screen, before cjk.
        for name in t3_evaluate.BENCHMARKS:
            self.assertIn(name, t3_evaluate.SCREENS[:-1])


LIBTSM = "ef2365d0f9f1a370d721a82470d69c3646fe97f0"


class SixelGoldenTest(unittest.TestCase):
    # The sixel page's registers at full strength (sixel.sh), as kmscon draws
    # them: the bars, the six discs, and the ring around its blue disc.
    BARS = [(255, 0, 0), (255, 255, 0), (0, 255, 0), (0, 255, 255), (0, 0, 255),
            (255, 0, 255), (255, 255, 255), (128, 128, 128)]
    DISCS = {"red": (255, 0, 0), "green": (0, 255, 0), "blue": (0, 0, 255),
             "cyan": (0, 255, 255), "magenta": (255, 0, 255), "yellow": (255, 255, 0)}
    ORANGE = (255, 128, 0)

    def test_colours_in_cells(self):
        # Every default variant's golden holds each picture in the cells the
        # page puts it in, in its colours. The cell is read from the golden:
        # the red bar is the first red pixel and one cell, at column 2, row 2
        # of a grid kmscon may centre on the display.
        for variant in t3_scenario.DEFAULT_VARIANTS:
            with self.subTest(variant):
                img = t3img.read(t3_evaluate.golden_for(variant, "sixel")[0])

                def px(x, y):
                    i = (y * img.width + x) * 3
                    return tuple(img.data[i:i + 3])

                x0, y0 = next((x, y) for y in range(200) for x in range(200)
                              if px(x, y) == self.BARS[0])
                cw = next(n for n in range(1, 200) if px(x0 + n, y0) != self.BARS[0])
                ch = next(n for n in range(1, 200) if px(x0, y0 + n) != self.BARS[0])
                left, top = x0 - 2 * cw, y0 - 2 * ch

                def at(col, row):
                    return px(left + int(col * cw), top + int(row * ch))

                self.assertEqual([at(2.5 + i, 2.5) for i in range(8)], self.BARS)
                for k, (name, rgb) in enumerate(self.DISCS.items()):
                    self.assertEqual(at(3.5 + 10 * k, 10), rgb, name)
                d = min(4 * cw, 2 * ch)
                self.assertEqual(at(42 - 3 * d / 8 / cw, 4), self.ORANGE)
                self.assertEqual(at(42, 4), self.DISCS["blue"])


class PackagesTest(unittest.TestCase):
    UP = ["kmscon-upstream-10.0.4.r68.g7d33779-1-x86_64.pkg.tar.zst",
          "kmscon-upstream-terminfo-10.0.4.r68.g7d33779-1-x86_64.pkg.tar.zst"]
    LINE = ("[  12.3] TIER3-INSTALL-OK packages: kmscon-upstream 10.0.4.r68.g7d33779-1 "
            "kmscon-upstream-terminfo 10.0.4.r68.g7d33779-1 ; libtsm: " + LIBTSM)

    def setUp(self):
        self.env = os.environ.pop("T3_LIBTSM_REV", None)

    def tearDown(self):
        os.environ.pop("T3_LIBTSM_REV", None)
        if self.env is not None:
            os.environ["T3_LIBTSM_REV"] = self.env

    def test_newest_of_one_name(self):
        # Each build's packages are told apart by name, so the fork's never
        # stand in for upstream's; the newest build of each is taken.
        with tempfile.TemporaryDirectory() as d:
            names = ["kmscon-aaronsb-10.0.4.r1.gabc-1-x86_64.pkg.tar.zst",
                     "kmscon-aaronsb-terminfo-10.0.4.r1.gabc-1-x86_64.pkg.tar.zst",
                     "kmscon-upstream-terminfo-9-1-x86_64.pkg.tar.zst",
                     "kmscon-upstream-9-1-x86_64.pkg.tar.zst"] + self.UP
            for i, name in enumerate(names):
                open(os.path.join(d, name), "w").close()
                os.utime(os.path.join(d, name), (i, i))
            got = t3_scenario.newest_packages(d, "kmscon-upstream")
            self.assertEqual([os.path.basename(p) for p in got], self.UP)
            self.assertEqual([t3_scenario.package_name(p) for p in got],
                             ["kmscon-upstream", "kmscon-upstream-terminfo"])
            with self.assertRaises(SystemExit):
                t3_scenario.newest_packages(d, "kmscon-other")

    def test_install_checked(self):
        os.environ["T3_LIBTSM_REV"] = "ef2365d"
        got = t3_scenario.check_install(self.LINE, self.UP)
        self.assertEqual(got, {"packages": ["kmscon-upstream 10.0.4.r68.g7d33779-1",
                                            "kmscon-upstream-terminfo 10.0.4.r68.g7d33779-1"],
                               "libtsm": LIBTSM})
        # A dirty libtsm passes only when the run declares it.
        os.environ["T3_LIBTSM_REV"] = "ef2365d+dirty"
        t3_scenario.check_install(self.LINE.replace(LIBTSM, LIBTSM + "+dirty"), self.UP)

    def test_install_refused(self):
        # Another build's package in the guest, a package built from another
        # libtsm than the run names, from an unknown or undeclared dirty one,
        # a run naming no libtsm, or no account at all fails the variant.
        os.environ["T3_LIBTSM_REV"] = "ef2365d"
        line = self.LINE.replace("; libtsm", "kmscon-aaronsb 10.0.4-1 ; libtsm")
        with self.assertRaisesRegex(RuntimeError, "kmscon-aaronsb installed"):
            t3_scenario.check_install(line, self.UP)
        for libtsm in (LIBTSM + "+dirty", "unknown", "none"):
            with self.assertRaisesRegex(RuntimeError, "built with libtsm %s," % re.escape(libtsm)):
                t3_scenario.check_install(self.LINE.replace(LIBTSM, libtsm), self.UP)
        with self.assertRaisesRegex(RuntimeError, "cannot read"):
            t3_scenario.check_install("TIER3-INSTALL-OK", self.UP)
        for want in ("06331b1", "ef2365d+dirty"):
            os.environ["T3_LIBTSM_REV"] = want
            with self.assertRaisesRegex(RuntimeError, "built with libtsm ef2365d0"):
                t3_scenario.check_install(self.LINE, self.UP)
        os.environ.pop("T3_LIBTSM_REV")
        with self.assertRaisesRegex(RuntimeError, "names no libtsm commit"):
            t3_scenario.check_install(self.LINE, self.UP)


class VariantTest(unittest.TestCase):
    def test_every_variant_sizes_its_machine(self):
        for name, v in t3_scenario.VARIANTS.items():
            with self.subTest(name):
                self.assertIsInstance(v["cpus"], int)
                self.assertIsInstance(v["memory_mb"], int)

    def test_potato(self):
        # ADR-103's small machine, in the default run.
        v = t3_scenario.VARIANTS["2d-1366x768-potato"]
        self.assertEqual((v["cpus"], v["memory_mb"], v["renderer"]), (1, 512, "bbulk"))
        self.assertIn("xres=1366,yres=768", v["device"])
        self.assertFalse(t3_scenario.uses_virgl("2d-1366x768-potato"))
        self.assertIn("2d-1366x768-potato", t3_scenario.DEFAULT_VARIANTS)

    def test_qemu_gets_the_machine(self):
        class Started(Exception):
            pass

        def popen(args, **kwargs):
            raise Started(args)

        saved = t3_qemu.subprocess.Popen
        t3_qemu.subprocess.Popen = popen
        try:
            with tempfile.TemporaryDirectory() as d:
                with self.assertRaises(Started) as cm:
                    t3_qemu.Qemu(d, [], "virtio-vga", lambda msg: None, cpus=1, memory_mb=512)
                args = cm.exception.args[0]
                self.assertEqual(args[args.index("-smp") + 1], "1")
                self.assertEqual(args[args.index("-m") + 1], "512")
                with self.assertRaises(Started) as cm:
                    t3_qemu.Qemu(d, [], "virtio-vga", lambda msg: None)
                args = cm.exception.args[0]
                self.assertEqual(args[args.index("-smp") + 1], "4")
                self.assertEqual(args[args.index("-m") + 1], "4096")
        finally:
            t3_qemu.subprocess.Popen = saved


def timer_line(t, flips, frames, interval_ms=1000.0, cpu=(1.0, 1.0, 2.0, 3.0), damage=(500, 1.5),
               pid=505, before=""):
    return ("%s[%12.6f] kmscon[%d]: terminal: render-timer: display=pci-0000:00:01.0[Virtual-1] "
            "interval_ms=%.3f presented_per_s=%.2f flips=%d frames=%d cpu_ms_mean=%.3f "
            "cpu_ms_median=%.3f cpu_ms_p95=%.3f cpu_ms_max=%.3f damage_px_mean=%d "
            "damage_pct=%.2f skipped=0 skipped_cpu_ms=0.000"
            % (before, t, pid, interval_ms, flips * 1000 / interval_ms, flips, frames, cpu[0],
               cpu[1], cpu[2], cpu[3], damage[0], damage[1]))


class BenchTest(unittest.TestCase):
    EVENT = "T3-BENCH s1-scroll lines=20000 start=100.00 end=102.00"
    CONF = ["font-size=16", "render-timer=log"]

    def test_timer_lines(self):
        text = "\n".join([
            "[  OK  ] Started something.",
            timer_line(99.5, 3, 3),
            # a console line after a prompt on the same line
            timer_line(100.5, 60, 60, before="tier3 login: "),
            "[  100.600000] kmscon[505]: terminal: Resized terminal to 128x42",
        ])
        got = t3_bench.timer_lines(text)
        self.assertEqual([x["t"] for x in got], [99.5, 100.5])
        self.assertEqual(got[1]["display"], "pci-0000:00:01.0[Virtual-1]")
        self.assertEqual((got[1]["flips"], got[1]["frames"], got[1]["pid"]), (60, 60, 505))
        self.assertEqual(got[1]["damage_pct"], 1.5)

    def test_window(self):
        self.assertEqual(t3_bench.bench_window("x " + self.EVENT),
                         ("s1-scroll", 20000, 100.0, 102.0))
        self.assertIsNone(t3_bench.bench_window("T3-DONE s1-scroll"))

    def test_summarize(self):
        lines = t3_bench.timer_lines("\n".join([
            timer_line(99.9, 5, 5),                 # ends before the window: out
            timer_line(100.6, 30, 30, cpu=(2.0, 2.0, 4.0, 5.0), damage=(1000, 10.0)),
            timer_line(101.6, 60, 60, cpu=(1.0, 1.0, 3.0, 9.0), damage=(400, 4.0)),
            timer_line(102.6, 10, 10, cpu=(4.0, 4.0, 6.0, 6.0), damage=(100, 1.0)),
            timer_line(103.6, 0, 0),                # starts after the window: out
        ]))
        got = t3_bench.summarize(lines, 20000, 100.0, 102.0)
        self.assertEqual(got["intervals"], 3)
        self.assertEqual((got["flips"], got["frames"]), (100, 100))
        self.assertEqual(got["presented_per_s"], 50.0)
        self.assertEqual(got["lines_per_s"], 10000)
        self.assertEqual(got["cpu_ms_mean"], 1.6)          # (60 + 60 + 40) / 100
        self.assertEqual(got["cpu_ms_median"], 2.0)        # the seconds' medians 2, 1, 4
        self.assertEqual((got["cpu_ms_p95"], got["cpu_ms_max"]), (6.0, 9.0))
        self.assertEqual(got["damage_px_mean"], 550)       # (30000 + 24000 + 1000) / 100
        self.assertEqual(got["damage_pct"], 5.5)
        self.assertEqual(got["displays"], ["pci-0000:00:01.0[Virtual-1]"])

    def test_measure(self):
        serial = timer_line(101.0, 60, 60)
        got = t3_bench.measure(serial, self.EVENT, self.CONF)
        self.assertNotIn("error", got)
        self.assertEqual(got["frames"], 60)
        # No line in the window: a fault of a build that has the timer...
        self.assertIn("error", t3_bench.measure(timer_line(50.0, 1, 1), self.EVENT, self.CONF))
        self.assertIn("error", t3_bench.measure("", self.EVENT, ["render-timer=both"]))
        # ...and none for a build without it, or a variant not configured.
        got = t3_bench.measure("[ 3.1] kmscon[9]: config: unknown config option 'render-timer'",
                               self.EVENT, self.CONF)
        self.assertEqual(got["note"], "this build has no render timer")
        self.assertNotIn("error", got)
        got = t3_bench.measure("", self.EVENT, ["render-timer=none"])
        self.assertIn("not configured", got["note"])
        self.assertNotIn("error", got)
        # A screen that reported no window.
        self.assertIn("no benchmark window", t3_bench.measure(serial, None, self.CONF)["error"])

    def test_bench_error_fails_case(self):
        meta = {"bench": {"s1-scroll": {"error": "no render-timer line"}}}
        with tempfile.TemporaryDirectory() as d:
            ppm = os.path.join(d, "a.ppm")
            t3_qemu.write_ppm(ppm, t3img.Image(2, 1, b"\0" * 6))
            saved = t3_evaluate.GOLDEN, t3_evaluate.RUNDIR
            t3_evaluate.GOLDEN, t3_evaluate.RUNDIR = d, d
            try:
                os.makedirs(os.path.join(d, "v"))
                t3img.write_png(os.path.join(d, "v", "s1-scroll.png"), t3img.Image(2, 1, b"\0" * 6))
                with contextlib.redirect_stderr(io.StringIO()):
                    entry = t3_evaluate.evaluate("v", "s1-scroll", ppm, meta, LOG)
                self.assertEqual((entry["status"], entry["error"]), ("fail", "no render-timer line"))
                with contextlib.redirect_stderr(io.StringIO()):
                    entry = t3_evaluate.evaluate("v", "s1-scroll", ppm, {}, LOG)
                self.assertEqual(entry["status"], "pass")
            finally:
                t3_evaluate.GOLDEN, t3_evaluate.RUNDIR = saved


class SerialTest(unittest.TestCase):
    """The guest writes its console a few bytes at a time, so the driver can
    read a line before its end has arrived (#61)."""
    LINE = (b"TIER3-INSTALL-OK packages: kmscon-aaronsb 10.0.4.r208.gb5e9e66-1 "
            b"kmscon-aaronsb-terminfo 10.0.4.r208.gb5e9e66-1 ; libtsm: " + LIBTSM.encode())

    class Clock:
        """Stands in for t3_qemu's time: each sleep advances the clock and
        lets the guest write its next chunk."""
        def __init__(self, path, chunks):
            self.path, self.chunks, self.now = path, list(chunks), 0.0

        def time(self):
            return self.now

        def sleep(self, seconds):
            self.now += seconds
            if self.chunks:
                with open(self.path, "ab") as f:
                    f.write(self.chunks.pop(0))

    class Proc:
        def poll(self):
            return None

    def wait(self, chunks, marker="TIER3-INSTALL-OK", timeout=180):
        with tempfile.TemporaryDirectory() as d:
            vm = t3_qemu.Qemu.__new__(t3_qemu.Qemu)
            vm.serial, vm.proc = os.path.join(d, "serial.log"), self.Proc()
            with open(vm.serial, "wb") as f:
                f.write(b"[  OK  ] Started \x1b[0;1;39mUser Login Management\x1b[0m.\r\r\n")
            saved = t3_qemu.time
            t3_qemu.time = self.Clock(vm.serial, chunks)
            try:
                return vm.wait_serial(marker, timeout)
            finally:
                t3_qemu.time = saved

    def test_two_chunks(self):
        cut = self.LINE.index(b" kmscon-aaronsb-terminfo")
        self.assertEqual(self.wait([self.LINE[:cut], self.LINE[cut:] + b"\r\r\n"]),
                         self.LINE.decode())

    def test_many_chunks(self):
        # One byte at a time, a marker split across chunks included.
        data = self.LINE + b"\r\r\n"
        self.assertEqual(self.wait([data[i:i + 1] for i in range(len(data))]),
                         self.LINE.decode())

    def test_partial_line_times_out(self):
        # A line whose end never arrives is never read, and the wait fails
        # at its deadline.
        with self.assertRaisesRegex(RuntimeError, "no 'TIER3-INSTALL-OK' within 5 s"):
            self.wait([self.LINE[:40], self.LINE[40:]], timeout=5)

    def test_split_utf8(self):
        # A character split between chunks is read whole once the line ends.
        line = "T3-DONE cjk \u4e2d".encode()
        self.assertEqual(self.wait([line[:-1], line[-1:] + b"\n"], marker="T3-DONE"),
                         "T3-DONE cjk \u4e2d")


class LoggerTest(unittest.TestCase):
    def line(self, log):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            log("hello")
        return err.getvalue()

    def test_labelled(self):
        log = Logger(t0=0)
        self.assertRegex(self.line(log), r"^\[t3 \+[0-9.]+s\] hello\n$")
        self.assertRegex(self.line(log.labelled("2d-1280x800")),
                         r"^\[t3 \+[0-9.]+s 2d-1280x800\] hello\n$")

    def test_pickles(self):
        # A worker process receives the run's logger by pickle.
        log = pickle.loads(pickle.dumps(Logger(t0=123.5).labelled("v")))
        self.assertEqual((log.t0, log.label), (123.5, "v"))


if __name__ == "__main__":
    unittest.main()
