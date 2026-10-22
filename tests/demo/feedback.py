#!/usr/bin/env python3
"""Drives fork/demo/demo.sh through scripted keys under a pseudo-terminal
(#47) and checks what it records and what fork/demo/feedback.sh prints.

demo.sh reads its keys from /dev/tty, so it runs as the session leader of a
pty of 25 rows by 80 columns, as on a console. Each key is sent only once the
prompt it answers is on the screen. demo.sh asks the terminal for the cursor's
row (DSR 6) to decide whether the footer fits below the page; this driver is
no terminal emulator, so it answers with a row it is told to: row 5, where
every footer fits, or row 25, where none does.

    feedback.py --root SOURCE_ROOT
"""

import argparse
import fcntl
import os
import pty
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROWS, COLS = 25, 80
ESC = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
ISO = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d[+-]\d\d:\d\d$")
RUN = re.compile(r"^\d{8}T\d{6}-\d+$")
PROMPT = b"n note  b back  q quit"


class Demo:
    """demo.sh running on a pty, read and answered like a console."""

    def __init__(self, root, args, env):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
            os.execve("/bin/bash", ["bash", os.path.join(root, "fork/demo/demo.sh"), *args], env)
        self.out = b""
        self.seen = 0
        self.asked = 0
        self.row = 5

    def pump(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return False
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            return False
        if not data:
            return False
        self.out += data
        while (at := self.out.find(b"\x1b[6n", self.asked)) >= 0:
            self.asked = at + 4
            os.write(self.fd, b"\x1b[%d;1R" % self.row)
        return True

    def expect(self, text, timeout=15):
        """Waits for TEXT after the last text expected, and returns the screen
        output from there up to its end, escape sequences removed."""
        text = text.encode() if isinstance(text, str) else text
        end = time.monotonic() + timeout
        while (at := self.out.find(text, self.seen)) < 0:
            if time.monotonic() > end or not self.pump(end - time.monotonic()):
                tail = ESC.sub(b"", self.out[self.seen:])[-600:]
                raise AssertionError(f"waiting for {text!r}; the screen shows:\n{tail.decode(errors='replace')}")
        start, self.seen = self.seen, at + len(text)
        return ESC.sub(b"", self.out[start:self.seen]).decode(errors="replace")

    def send(self, keys):
        os.write(self.fd, keys.encode())

    def wait(self, timeout=15):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump(0.1)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                os.close(self.fd)
                return os.waitstatus_to_exitcode(status)
        raise AssertionError("demo.sh did not exit")


failures = []


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)
        if detail:
            print(detail)


def rows_of(path):
    with open(path, encoding="utf-8") as f:
        return [line.rstrip("\n").split("\t") for line in f]


def feedback(root, *args):
    r = subprocess.run(["bash", os.path.join(root, "fork/demo/feedback.sh"), *args],
                       capture_output=True, text=True, check=False)
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = os.path.abspath(ap.parse_args().root)
    tmp = tempfile.mkdtemp(prefix="demo-feedback-")
    path = os.path.join(tmp, "kmscon-demo", "feedback.tsv")
    base = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": tmp,
            "TERM": "xterm-256color", "DEMO_FEEDBACK": path}

    # Every section carries both guidance lines.
    sections = os.path.join(root, "fork/demo/sections")
    for name in sorted(os.listdir(sections)):
        with open(os.path.join(sections, name), encoding="utf-8") as f:
            text = f.read()
        check(all(re.search(rf"^# {k}: \S", text, re.M) for k in ("look-for", "wrong-looks-like")),
              f"{name} has look-for and wrong-looks-like lines")

    # Run 1: a reprompt, ratings 1, 2 (with a note) and 3, b and a new rating,
    # the footer on a screen of its own, q.
    d = Demo(root, ["sgr", "colour-forms", "underline-styles", "strike"],
             {**base, "DEMO_LABEL": "dev\tbuild"})
    d.expect("press a key for the first section")
    d.send(" ")
    page = d.expect(PROMPT)
    check("[1/4] sgr" in page and "right:    16 different colours" in page,
          "page 1 shows its header and the right: line in the footer")
    d.send("x")
    d.expect(PROMPT)
    check(rows_of(path) == [["time", "run", "label", "term", "size", "section", "rating", "note"]],
          "the file holds only its header before an answer; x reprompted")
    d.send("1")
    d.expect("[2/4] colour-forms")
    d.expect(PROMPT)
    d.send("n")
    d.expect("note: ")
    d.send("swatch 3 differs\r")
    page = d.expect(PROMPT)
    check("[2/4] colour-forms" in page and "note:     swatch 3 differs" in page,
          "after n the page is drawn again with the note in its footer")
    d.send("2")
    d.expect("[3/4] underline-styles")
    d.expect(PROMPT)
    d.send("3")
    d.expect("[4/4] strike")
    d.expect(PROMPT)
    d.send("b")
    d.expect("[3/4] underline-styles")
    d.expect(PROMPT)
    d.row = 25
    d.send("1")
    page = d.expect("press a key for the questions on this page")
    check("[4/4] strike" in page and "right:" not in page,
          "a footer that does not fit is not drawn below the page")
    d.send(" ")
    page = d.expect(PROMPT)
    check("[4/4] strike: questions" in page and "right:    A line through the middle" in page,
          "the footer gets a screen of its own")
    d.send("q")
    page = d.expect("press a key to finish")
    summary = [line.rstrip() for line in page.replace("\r", "").split("\n")]
    check(["  10-sgr                 right",
           "  15-colour-forms        wrong   swatch 3 differs",
           "  20-underline-styles    right",
           "  30-strike              -"] == [s for s in summary if re.match(r"  \d\d-", s)],
          "the summary shows each section's last rating, b's included")
    check(path in page, "the summary names the file")
    d.send(" ")
    check(d.wait() == 0, "q, then a key, ends the run with status 0")

    rows = rows_of(path)
    run1 = rows[1][1] if len(rows) > 1 else "(none)"
    check([r[2:] for r in rows[1:]] == [
        ["dev build", "xterm-256color", "25x80", "10-sgr", "right", ""],
        ["dev build", "xterm-256color", "25x80", "15-colour-forms", "wrong", "swatch 3 differs"],
        ["dev build", "xterm-256color", "25x80", "20-underline-styles", "unsure", ""],
        ["dev build", "xterm-256color", "25x80", "20-underline-styles", "right", ""],
    ], "run 1 recorded four answers, the tab in the label made a space")
    check(all(ISO.match(r[0]) for r in rows[1:]), "each answer carries an ISO timestamp")
    check(RUN.match(run1) and all(r[1] == run1 for r in rows[1:]), "one run id for the whole run")
    check(oct(os.stat(path).st_mode & 0o777) == "0o644"
          and oct(os.stat(os.path.dirname(path)).st_mode & 0o777) == "0o755",
          "the file is 0644 in a directory of 0755")

    # Run 2, another label, into the same file: 2, then q.
    d = Demo(root, ["sgr", "colour-forms"], {**base, "DEMO_LABEL": "stock"})
    d.expect("press a key for the first section")
    d.send(" ")
    d.expect(PROMPT)
    d.send("2")
    d.expect("[2/2] colour-forms")
    d.expect(PROMPT)
    d.send("q")
    d.expect("press a key to finish")
    d.send(" ")
    check(d.wait() == 0, "run 2 ends with status 0")
    rows = rows_of(path)
    run2 = rows[-1][1]
    check(len(rows) == 6 and rows[0][0] == "time" and rows[5][2:] ==
          ["stock", "xterm-256color", "25x80", "10-sgr", "wrong", ""] and run2 != run1,
          "run 2 appended one answer under a run id of its own, no second header")
    if len(rows) != 6:
        print(f"{len(failures)} check(s) failed; the files are in {tmp}")
        return 1

    status, out = feedback(root, "--file", path)
    check(status == 0 and out == (
        f"run       {run2}\nlabel     stock\nterminal  TERM=xterm-256color, 25x80\n"
        f"started   {rows[5][0]}\nanswers   1 for 1 sections\n\n"
        "section                   rating   note\n"
        "10-sgr                    wrong\n"), "feedback.sh prints the latest run", out)
    status, out = feedback(root, "--file", path, "--run", run1)
    check(status == 0 and out == (
        f"run       {run1}\nlabel     dev build\nterminal  TERM=xterm-256color, 25x80\n"
        f"started   {rows[1][0]}\nanswers   4 for 3 sections\n\n"
        "section                   rating   note\n"
        "10-sgr                    right\n"
        "15-colour-forms           wrong    swatch 3 differs\n"
        "20-underline-styles       right\n"), "feedback.sh --run prints run 1", out)
    status, out = feedback(root, "--file", path, "--compare")
    check(status == 0 and out == (
        f"A  dev build\n   run {run1}, TERM=xterm-256color, 25x80\n"
        f"B  stock\n   run {run2}, TERM=xterm-256color, 25x80\n\n"
        "section                   A        B\n"
        "10-sgr                    right    wrong\n"
        "15-colour-forms           wrong    -\n"
        "20-underline-styles       right    -\n\n"
        "notes\n"
        "A  15-colour-forms          swatch 3 differs\n"), "feedback.sh --compare", out)
    status, out = feedback(root, "--file", path, "--run", "nope")
    check(status == 1 and "no run nope" in out, "feedback.sh --run with an unknown id fails")

    # DEMO_NOPROMPT=1: the guidance, no questions, nothing recorded.
    none = os.path.join(tmp, "none", "feedback.tsv")
    d = Demo(root, ["sgr", "colour-forms"], {**base, "DEMO_NOPROMPT": "1", "DEMO_FEEDBACK": none})
    d.expect("press a key for the first section")
    d.send(" ")
    page = d.expect("press a key for next, q to quit")
    check("right:    16 different colours" in page and "q quit" not in page,
          "DEMO_NOPROMPT shows the guidance and asks nothing")
    d.send(" ")
    d.expect("end of the demo: press a key to finish")
    d.send(" ")
    check(d.wait() == 0, "DEMO_NOPROMPT pages to the end")
    check(not os.path.exists(os.path.dirname(none)), "DEMO_NOPROMPT records nothing")

    if failures:
        print(f"{len(failures)} check(s) failed; the files are in {tmp}")
        return 1
    shutil.rmtree(tmp)
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
