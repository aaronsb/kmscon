#!/usr/bin/env python3
"""Self-test for the parts of tier 3 that run on the host without a guest:
the screen text a case's report carries (evaluate.py).

    python3 -I fork/tier3/test_t3.py
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import evaluate  # noqa: E402
from common import Logger  # noqa: E402

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
        self.assertTrue(evaluate.screen_text("sgr", LOG)[2].startswith("bold underline reverse "))
        self.assertEqual(evaluate.screen_text("cjk", LOG)[2], "|一|二|三|四|五|")
        self.assertEqual(evaluate.screen_text("vt-restore", LOG),
                         evaluate.screen_text(evaluate.SCREENS[-1], LOG))
        self.assertIsNone(evaluate.screen_text("vt-switch", LOG))

    def text(self, name):
        with tempfile.TemporaryDirectory() as guest, contextlib.redirect_stderr(io.StringIO()):
            os.mkdir(os.path.join(guest, "screens"))
            with open(os.path.join(guest, "screens", name + ".sh"), "w") as f:
                f.write(SCRIPTS[name] + "\n")
            saved = evaluate.GUEST, evaluate.SCREENS, evaluate.SCREEN_TIMEOUT
            evaluate.GUEST, evaluate.SCREENS, evaluate.SCREEN_TIMEOUT = guest, [name], 1
            try:
                return evaluate.screen_text(name, LOG)
            finally:
                evaluate.GUEST, evaluate.SCREENS, evaluate.SCREEN_TIMEOUT = saved

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


if __name__ == "__main__":
    unittest.main()
