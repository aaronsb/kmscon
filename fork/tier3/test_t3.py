#!/usr/bin/env python3
"""Self-test for the parts of tier 3 that run on the host without a guest:
the caption and screen text a case's report carries (t3_evaluate.py), the
packages a run installs and the guest's account of them (t3_scenario.py),
the console lines the driver waits for (t3_qemu.py), and the logger a worker
process receives (t3_common.py).

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
import t3_evaluate  # noqa: E402
import t3_qemu  # noqa: E402
import t3_scenario  # noqa: E402
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
}


class ScreenTextTest(unittest.TestCase):
    def test_real_screens(self):
        self.assertTrue(t3_evaluate.screen_text("sgr", LOG)[2]
                        .startswith("bold underline reverse "))
        self.assertEqual(t3_evaluate.screen_text("cjk", LOG)[2], "|一|二|三|四|五|")
        self.assertEqual(t3_evaluate.screen_text("vt-restore", LOG),
                         t3_evaluate.screen_text(t3_evaluate.SCREENS[-1], LOG))
        self.assertIsNone(t3_evaluate.screen_text("vt-switch", LOG))
        # wide-margin moves the cursor to reach the screen's real edge, so it
        # has no text on purpose; every other screen has its rows.
        self.assertIsNone(t3_evaluate.screen_text("wide-margin", LOG))
        for name in t3_evaluate.SCREENS:
            if name != "wide-margin":
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


LIBTSM = "ef2365d0f9f1a370d721a82470d69c3646fe97f0"


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
