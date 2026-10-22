#!/usr/bin/env python3
"""Self-test for the parts of tier 3 that run on the host without a guest:
the caption and screen text a case's report carries (t3_evaluate.py) and
the logger a worker process receives (t3_common.py).

    python3 -I fork/tier3/test_t3.py
"""

import contextlib
import io
import os
import pickle
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t3_evaluate  # noqa: E402
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
        # The demo's guidance lines, in any order among the caption's.
        self.assertEqual(self.describe("# look-for: Lines\n# look-for: joined.\n"
                                       "# wrong-looks-like: A gap.\n# A screen.\n"),
                         ("A screen. Correct: Lines joined.", "A gap."))

    def test_plain_caption(self):
        self.assertEqual(self.describe("# A screen. Correct: x: y.\n"),
                         ("A screen. Correct: x: y.", None))

    def test_check(self):
        self.assertEqual(t3_evaluate.describe("vt-switch"),
                         (t3_evaluate.CHECKS["vt-switch"], None))


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
