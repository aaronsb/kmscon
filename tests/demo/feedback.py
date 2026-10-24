#!/usr/bin/env python3
"""Drives fork/demo/demo.sh through scripted keys under a pseudo-terminal
(#47) and checks what it records and what fork/demo/feedback.sh prints.

demo.sh reads its keys from /dev/tty, so it runs as the session leader of a
pty of 25 rows by 80 columns, as on a console. Each key is sent only once the
prompt it answers is on the screen. demo.sh asks the terminal for the cursor's
row (DSR 6) to decide whether the footer fits below the page; this driver is
no terminal emulator, so it answers with a row it is told to: row 5, where
every footer fits, or row 25, where none does.

A key the driver sends may reach the terminal while demo.sh draws, between two
of its reads. Each time it reads the screen, the driver also notes whether the
terminal is editing lines then (#131): a Ctrl-D sent to a line-editing
terminal is an end of file, never a key.

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
PROMPT = b"c comment  r redraw  b back  q quit"
COMMENT = b"a line holding only ."
RATE = b"Enter no rating"
RERUN = b"r review again from page 1  q exit"


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
        self.canonical = 0

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
            # From the title page, drawn once demo.sh has set the terminal up,
            # to the last key, after which demo.sh restores it.
            if b"local VT demo" in self.out and termios.tcgetattr(self.fd)[3] & termios.ICANON:
                self.canonical += 1
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


def summary_rows(page):
    """The section rows of a summary page."""
    return [line.rstrip() for line in page.replace("\r", "").split("\n") if re.match(r"  \d\d-", line)]


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

    # Every section carries both guidance lines, and belongs to a review kit
    # (#123): its "# kit:" line, or text without one.
    sections = os.path.join(root, "fork/demo/sections")
    kits = {}
    for name in sorted(os.listdir(sections)):
        with open(os.path.join(sections, name), encoding="utf-8") as f:
            text = f.read()
        check(all(re.search(rf"^# {k}: \S", text, re.M) for k in ("look-for", "wrong-looks-like")),
              f"{name} has look-for and wrong-looks-like lines")
        found = re.findall(r"^# kit: (\S+)$", text, re.M)
        check(len(found) <= 1 and set(found) <= {"text", "sixel", "kitty"},
              f"{name} names one known kit, or none", repr(found))
        kits.setdefault(found[0] if found else "text", []).append(name[:-3])
    check(kits.get("sixel") == ["80-sixel", "81-sixel-ramps", "82-sixel-article", "83-photos",
                                "83-sixel-evict", "84-photo-scroll", "85-mpv-sixel", "86-tmux-2x2",
                                "87-tmux-3x3"],
          "the sixel kit holds the sixel pages and the acceptance pages", repr(kits.get("sixel")))
    check(kits.get("kitty") == ["90-kitty", "91-kitty-photo", "92-kitty-files", "93-kitty-layers"],
          "the kitty kit holds the kitty pages",
          repr(kits.get("kitty")))
    check("10-sgr" in kits.get("text", []) and "70-emoji" in kits.get("text", []),
          "the text kit holds the text pages", repr(kits.get("text")))

    # DEMO_KIT picks the pages: text by default, all for every page; an
    # unknown kit is refused. Named sections run whatever their kit.
    def listed(env, *args):
        p = subprocess.run(["bash", os.path.join(root, "fork/demo/demo.sh"), *args],
                           env={**base, "DEMO_NOPAGE": "1", **env}, stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=120)
        names = re.findall(rb"^ +\d+  (\S+)$", p.stdout, re.M)
        return p.returncode, [n.decode() for n in names], p.stdout.decode(errors="replace") + p.stderr.decode()

    every = sorted(n[3:] for k in kits.values() for n in k)
    status, names, out = listed({})
    check(status == 0 and names == [n[3:] for n in kits["text"]],
          "with no DEMO_KIT the title page lists the text kit", out)
    check("kit       text" in out, "the title page names the kit", out)
    status, names, out = listed({"DEMO_KIT": "sixel"})
    check(status == 0 and names == [n[3:] for n in kits["sixel"]],
          "DEMO_KIT=sixel lists the sixel kit", out)
    status, names, out = listed({"DEMO_KIT": "kitty"})
    check(status == 0 and names == [n[3:] for n in kits["kitty"]] and "kitty graphics query" in out,
          "DEMO_KIT=kitty lists the kitty pages, which say when nothing answers their query",
          out)
    status, names, out = listed({"DEMO_KIT": "all"})
    check(status == 0 and sorted(names) == every, "DEMO_KIT=all lists every page", out)
    status, names, out = listed({"DEMO_KIT": "bogus"})
    check(status == 2 and "DEMO_KIT=bogus" in out and "text, sixel, kitty or all" in out,
          "an unknown DEMO_KIT is refused", out)
    status, names, out = listed({}, "sixel", "cjk")
    check(status == 0 and names == ["cjk", "sixel"], "named sections run whatever their kit", out)

    # Run 1: reprompts on x and on Ctrl-D, r drawing page 1 again, ratings 1
    # and 3, a comment of two lines ended by "." and rated 2, b, an empty
    # comment, a comment ended by Ctrl-D with no rating, the footer on a
    # screen of its own, q, then r for a second pass and q.
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
    d.send("\x04")
    d.expect(PROMPT)
    d.send("r")
    page = d.expect(PROMPT)
    check("[1/4] sgr" in page and "right:    16 different colours" in page
          and rows_of(path) == [["time", "run", "label", "term", "size", "section", "rating",
                                 "note"]],
          "r draws the page again and records nothing")
    d.send("1")
    d.expect("[2/4] colour-forms")
    d.expect(PROMPT)
    d.send("c")
    d.expect(COMMENT)
    d.expect("> ")
    d.send("swatch 3 differs\r")
    check("swatch 3 differs" in d.expect("> "), "a comment's text is echoed as it is typed")
    d.send("rows 2\\4 too\r")
    d.expect("> ")
    d.send(".\r")
    page = d.expect(RATE)
    check("[3/4]" not in page, "a comment ended by . asks for a rating on the same page")
    d.send("2")
    d.expect("[3/4] underline-styles")
    d.expect(PROMPT)
    d.send("3")
    d.expect("[4/4] strike")
    d.expect(PROMPT)
    d.send("b")
    d.expect("[3/4] underline-styles")
    d.expect(PROMPT)
    d.send("c")
    d.expect(COMMENT)
    d.expect("> ")
    d.send("\x04")
    page = d.expect(PROMPT)
    check("[3/4] underline-styles" in page and RATE.decode() not in page,
          "Ctrl-D on an empty comment draws the page again and asks nothing")
    d.send("c")
    d.expect(COMMENT)
    d.expect("> ")
    d.send("curly ok\r")
    d.expect("> ")
    d.send("\x04")
    d.expect(RATE)
    d.row = 25
    d.send("\r")
    page = d.expect("press a key for the questions on this page")
    check("[4/4] strike" in page and "right:" not in page,
          "Enter after a comment goes on; a footer that does not fit is not drawn below the page")
    d.send(" ")
    page = d.expect(PROMPT)
    check("[4/4] strike: questions" in page and "right:    A line through the middle" in page,
          "the footer gets a screen of its own")
    d.send("q")
    page = d.expect(RERUN)
    check(summary_rows(page) == [
        "  10-sgr                 right",
        "  15-colour-forms        wrong   swatch 3 differs / rows 2\\4 too",
        "  20-underline-styles    none    curly ok",
        "  30-strike              -"], "the summary shows each section's last answer, b's included",
        "\n".join(summary_rows(page)))
    check(path in page, "the summary names the file")
    d.row = 5
    d.send("r")
    d.expect("[1/4] sgr")
    d.expect(PROMPT)
    d.send("1")
    d.expect("[2/4] colour-forms")
    d.expect(PROMPT)
    d.send("q")
    page = d.expect(RERUN)
    check(summary_rows(page) == [
        "  10-sgr                 right",
        "  15-colour-forms        -",
        "  20-underline-styles    -",
        "  30-strike              -"], "after r the summary shows only the new pass",
        "\n".join(summary_rows(page)))
    d.send("q")
    check(d.wait() == 0, "q on the summary ends the run with status 0")
    check(not d.canonical, "the terminal never edits lines between keys, so a Ctrl-D sent then is a key",
          f"{d.canonical} reads of the screen found it editing lines")

    rows = rows_of(path)
    run1 = rows[1][1] if len(rows) > 1 else "(none)"
    check([r[2:] for r in rows[1:]] == [
        ["dev build", "xterm-256color", "25x80", "10-sgr", "right", ""],
        ["dev build", "xterm-256color", "25x80", "15-colour-forms", "wrong", "swatch 3 differs\\nrows 2\\\\4 too"],
        ["dev build", "xterm-256color", "25x80", "20-underline-styles", "unsure", ""],
        ["dev build", "xterm-256color", "25x80", "20-underline-styles", "none", "curly ok"],
        ["dev build", "xterm-256color", "25x80", "10-sgr", "right", ""],
    ], "run 1 recorded five answers; comments escaped; the tab in the label made a space",
        "\n".join(map(repr, rows[1:])))
    check(all(ISO.match(r[0]) for r in rows[1:]), "each answer carries an ISO timestamp")
    check(RUN.match(run1) and all(r[1] == run1 for r in rows[1:5]), "one run id for the first pass")
    run1b = rows[-1][1]
    check(RUN.match(run1b) and run1b != run1, "the pass after r has a run id of its own")
    check(oct(os.stat(path).st_mode & 0o777) == "0o644"
          and oct(os.stat(os.path.dirname(path)).st_mode & 0o777) == "0o755",
          "the file is 0644 in a directory of 0755")
    status, out = feedback(root, "--file", path)
    check(status == 0 and out == (
        f"run       {run1b}\nlabel     dev build\nterminal  TERM=xterm-256color, 25x80\n"
        f"started   {rows[-1][0]}\nanswers   1 for 1 sections\n\n"
        "section                   rating\n"
        "10-sgr                    right\n"), "feedback.sh shows the pass after r as the latest run", out)

    # Run 2, another label, into the same file: a comment rated 2, then q, q.
    d = Demo(root, ["sgr", "colour-forms"], {**base, "DEMO_LABEL": "stock"})
    d.expect("press a key for the first section")
    d.send(" ")
    d.expect(PROMPT)
    d.send("c")
    d.expect(COMMENT)
    for line in ("a", "b", "."):
        d.expect("> ")
        d.send(line + "\r")
    d.expect(RATE)
    d.send("2")
    d.expect("[2/2] colour-forms")
    d.expect(PROMPT)
    d.send("q")
    d.expect(RERUN)
    d.send("q")
    check(d.wait() == 0, "run 2 ends with status 0")
    rows = rows_of(path)
    run2 = rows[-1][1]
    check(len(rows) == 7 and rows[0][0] == "time" and rows[6][2:] ==
          ["stock", "xterm-256color", "25x80", "10-sgr", "wrong", "a\\nb"] and run2 not in (run1, run1b),
          "run 2 appended one answer under a run id of its own, no second header")
    if len(rows) != 7:
        print(f"{len(failures)} check(s) failed; the files are in {tmp}")
        return 1

    status, out = feedback(root, "--file", path)
    check(status == 0 and out == (
        f"run       {run2}\nlabel     stock\nterminal  TERM=xterm-256color, 25x80\n"
        f"started   {rows[6][0]}\nanswers   1 for 1 sections\n\n"
        "section                   rating\n"
        "10-sgr                    wrong\n"
        "    a\n"
        "    b\n"), "feedback.sh prints the latest run, its comment one line per line", out)
    status, out = feedback(root, "--file", path, "--run", run1)
    check(status == 0 and out == (
        f"run       {run1}\nlabel     dev build\nterminal  TERM=xterm-256color, 25x80\n"
        f"started   {rows[1][0]}\nanswers   4 for 3 sections\n\n"
        "section                   rating\n"
        "10-sgr                    right\n"
        "15-colour-forms           wrong\n"
        "    swatch 3 differs\n"
        "    rows 2\\4 too\n"
        "20-underline-styles       none\n"
        "    curly ok\n"), "feedback.sh --run prints run 1", out)
    status, out = feedback(root, "--file", path, "--compare")
    check(status == 0 and out == (
        f"A  dev build\n   run {run1b}, TERM=xterm-256color, 25x80\n"
        f"B  stock\n   run {run2}, TERM=xterm-256color, 25x80\n\n"
        "section                   A        B\n"
        "10-sgr                    right    wrong\n\n"
        "comments\n"
        "B  10-sgr\n"
        "    a\n"
        "    b\n"), "feedback.sh --compare", out)
    status, out = feedback(root, "--file", path, "--run", "nope")
    check(status == 1 and "no run nope" in out, "feedback.sh --run with an unknown id fails")

    # Run 3, a third label: s skips page 1; s on page 2 is replaced after b by
    # 1; a comment on page 3 is followed by s; page 4 is rated 3.
    d = Demo(root, ["sgr", "colour-forms", "underline-styles", "strike"], {**base, "DEMO_LABEL": "review"})
    page = d.expect("press a key for the first section")
    check("s skips it for now\r\n  (still to review)" in page, "the title page explains s")
    d.send(" ")
    page = d.expect(PROMPT)
    check("s skip  c comment" in page, "the prompt offers s")
    d.send("s")
    d.expect("[2/4] colour-forms")
    d.expect(PROMPT)
    d.send("S")
    d.expect("[3/4] underline-styles")
    d.expect(PROMPT)
    d.send("b")
    d.expect("[2/4] colour-forms")
    d.expect(PROMPT)
    d.send("1")
    d.expect("[3/4] underline-styles")
    d.expect(PROMPT)
    d.send("c")
    d.expect(COMMENT)
    d.expect("> ")
    d.send("next time\r")
    d.expect("> ")
    d.send(".\r")
    page = d.expect(RATE)
    check("s skip for now" in page, "the rating after a comment offers s")
    d.send("s")
    d.expect("[4/4] strike")
    d.expect(PROMPT)
    d.send("3")
    page = d.expect(RERUN)
    check(summary_rows(page) == [
        "  10-sgr                 skipped",
        "  15-colour-forms        right",
        "  20-underline-styles    skipped next time",
        "  30-strike              unsure"] and "skipped: 2, still to review" in page,
        "the summary shows the skipped pages and counts them", page)
    d.send("q")
    check(d.wait() == 0, "run 3 ends with status 0")
    rows = rows_of(path)
    run3 = rows[-1][1]
    check([r[5:] for r in rows[7:]] == [
        ["10-sgr", "skipped", ""],
        ["15-colour-forms", "skipped", ""],
        ["15-colour-forms", "right", ""],
        ["20-underline-styles", "skipped", "next time"],
        ["30-strike", "unsure", ""],
    ], "s is recorded as skipped, apart from unsure", "\n".join(map(repr, rows[7:])))
    status, out = feedback(root, "--file", path)
    check(status == 0 and out == (
        f"run       {run3}\nlabel     review\nterminal  TERM=xterm-256color, 25x80\n"
        f"started   {rows[7][0]}\nanswers   5 for 4 sections\n\n"
        "section                   rating\n"
        "10-sgr                    skipped\n"
        "15-colour-forms           right\n"
        "20-underline-styles       skipped\n"
        "    next time\n"
        "30-strike                 unsure\n\n"
        "skipped, still to review\n"
        "  10-sgr\n"
        "  20-underline-styles\n"), "feedback.sh lists the skipped sections as still to review", out)
    status, out = feedback(root, "--file", path, "--compare")
    check(status == 0 and "10-sgr                    right    wrong    skipped\n" in out
          and "\nskipped, still to review\nC  10-sgr\nC  20-underline-styles\n\ncomments\n" in out,
          "feedback.sh --compare lists the skipped sections by column", out)

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
