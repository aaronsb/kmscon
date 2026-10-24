#!/usr/bin/env python3
"""Runs the VT demo's sixel pages as demo.sh draws them, below its header,
and as tier 3's t3 draws them, on a clear screen. Below the header a page
must draw exactly what it draws on a clear screen, two rows lower: every
text cell and every picture. A page whose pictures stand apart from its
text (APART) also fails when a picture is drawn over text already on the
screen or text is printed over a picture.

On the VT demo (2026-10-24, 7680x2160 with a 20x40 cell) the operator saw
the photographs over the text of photos and photo-scroll: the pages placed
their pictures at fixed rows, which hold the page's title once demo.sh's
two-line header has pushed it down. Tier 3 runs the pages on a clear screen
and could not see it.

Each page runs as the session leader of a pty sized as kmscon sizes it
(TIOCSWINSZ with the text area in pixels) and is answered as kmscon answers:
DA1 with attribute 4, XTWINOPS 16 and 18, XTSMGRAPHICS's colour registers,
and DSR 6 with the cursor's row in the model below. chafa and mpv are
stand-ins that draw an opaque sixel picture filling the box the page asked
for, chafa at the cursor and mpv at --vo-sixel-left and --vo-sixel-top, so
the check needs neither program and judges the page's layout alone.

The model keeps the screen's text cells and its pictures' boxes, scrolls
both, and moves the cursor after a picture as ADR-103 has kmscon do (foot's
rule): to the text row holding the picture's last pixel row, at the
picture's left column. A box is the picture's raster size over the cell
size, rounded up.

    sixel_placement.py --root SOURCE_ROOT
"""

import argparse
import fcntl
import os
import pty
import re
import select
import struct
import sys
import tempfile
import termios
import time

# The sections of the sixel kit that draw only through the stand-ins. The
# others draw with mpv's video output or tmux, which the stand-ins do not
# model, and are judged by tier 3's mpv-sixel and tmux cases; the audio page
# draws no picture.
UNMODELLED = {"85-audio", "85-mpv-sixel", "86-tmux-2x2", "87-tmux-3x3"}

# The sections whose pictures stand apart from their text. The others lay
# pictures over text on purpose (sixel's ring and lettered bars, the ramps'
# ring), which their tier 3 goldens judge.
APART = {"82-sixel-article", "83-photos", "83-sixel-evict", "84-photo-scroll"}

# (columns, rows, cell width, cell height): the demo's 80x25 at a 10x20
# cell, and the operator's 7680x2160 at a 20x40 cell.
SIZES = [(80, 25, 10, 20), (384, 54, 20, 40)]

CHAFA = r'''# chafa's stand-in: an opaque picture filling --size=COLSxROWS cells at the
# cursor, for the cell size TIOCGWINSZ gives.
import fcntl, struct, sys, termios
rows, cols, xp, yp = struct.unpack("HHHH", fcntl.ioctl(1, termios.TIOCGWINSZ, b"\0" * 8))
size = [a for a in sys.argv[1:] if a.startswith("--size=")][0][7:]
w, h = (int(n) for n in size.split("x"))
sys.stdout.write(SIXEL(w * (xp // cols), h * (yp // rows)))
'''

MPV = r'''# mpv's stand-in for --vo=sixel: an opaque picture --vo-sixel-width by
# --vo-sixel-height pixels at --vo-sixel-left and --vo-sixel-top, placed
# with CUP as mpv places it.
import sys
opt = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
sys.stdout.write("\033[%s;%sf" % (opt["vo-sixel-top"], opt["vo-sixel-left"]))
sys.stdout.write(SIXEL(int(opt["vo-sixel-width"]), int(opt["vo-sixel-height"])))
'''

SIXEL_FN = r'''
def SIXEL(w, h):
    band = "#1!%d~" % w
    return "\033P0;1;0q\"1;1;%d;%d#1;2;50;50;50" % (w, h) + "-".join([band] * (-(-h // 6))) + "\033\\"
'''

# demo.sh's own draw_page, with the helpers it calls, run for one section.
PAGE = r'''
eval "$(sed -n '/^clear_screen() {/,/^}/p; /^name_of() {/,/^}/p; /^draw_page() {/,/^}/p' "$1")"
sections=("$2")
draw_page 0
'''

# tier 3's t3: clear, ED 3, the section.
T3 = r'''
printf '\033[H\033[2J\033[3J'
bash "$2"
'''

CSI = re.compile(rb"\x1b\[([?0-9;]*)([ -/]*)([@-~])")


class Screen:
    """Text cells and picture boxes on a screen of rows by cols cells."""

    def __init__(self, cols, rows, cw, ch):
        self.cols, self.rows, self.cw, self.ch = cols, rows, cw, ch
        self.r = self.c = 1
        self.text = {}
        self.boxes = []  # [top, left, bottom, right], inclusive
        self.faults = []
        self.overlaps = []
        self.events = []  # ("text", row, col, char) and ("picture", row, col, w, h)
        self.scrolled = 0
        self.saved = (1, 1)
        self.buf = b""

    def scroll(self):
        self.scrolled += 1
        self.text = {(r - 1, c): v for (r, c), v in self.text.items() if r > 1}
        self.boxes = [[t - 1, l, b - 1, rt] for t, l, b, rt in self.boxes if b > 1]

    def newline(self):
        if self.r == self.rows:
            self.scroll()
        else:
            self.r += 1

    def put(self, ch):
        if self.c > self.cols:
            self.c = 1
            self.newline()
        for t, l, b, rt in self.boxes:
            if t <= self.r <= b and l <= self.c <= rt:
                self.overlaps.append("text %r printed over a picture at row %d, column %d"
                                   % (ch, self.r, self.c))
                break
        self.text[(self.r, self.c)] = ch
        self.events.append(("text", self.r + self.scrolled, self.c, ch))
        self.c += 1

    def picture(self, w, h):
        rows, cols = -(-h // self.ch), -(-w // self.cw)
        # the picture scrolls the screen until its last row fits
        while self.r + rows - 1 > self.rows:
            self.scroll()
            self.r -= 1
        top, left = self.r, self.c
        bottom, right = top + rows - 1, min(left + cols - 1, self.cols)
        under = sorted({r for (r, c) in self.text if top <= r <= bottom and left <= c <= right})
        if under:
            self.overlaps.append("a %dx%d picture at row %d, column %d covers text on rows %s"
                               % (w, h, top, left, ", ".join(map(str, under))))
        self.boxes.append([top, left, bottom, right])
        self.events.append(("picture", top + self.scrolled, left, w, h))
        self.r = bottom

    def feed(self, data):
        """Consumes what it can of data; returns the replies it owes."""
        self.buf += data
        out = b""
        d = self.buf
        i = 0
        while i < len(d):
            b = d[i]
            if b == 0x1b:
                if i + 1 >= len(d):
                    break
                if d[i + 1:i + 2] == b"P":
                    end = d.find(b"\x1b\\", i)
                    if end < 0:
                        break
                    ra = re.search(rb'q"\d+;\d+;(\d+);(\d+)', d[i:end])
                    if ra:
                        self.picture(int(ra.group(1)), int(ra.group(2)))
                    i = end + 2
                    continue
                if d[i + 1:i + 2] == b"[":
                    m = CSI.match(d, i)
                    if not m:
                        if len(d) - i < 32:
                            break
                        i += 2
                        continue
                    out += self.csi(m.group(1), m.group(3))
                    i = m.end()
                    continue
                if d[i + 1:i + 2] == b"7":
                    self.saved = (self.r, self.c)
                elif d[i + 1:i + 2] == b"8":
                    self.r, self.c = self.saved
                i += 2
                continue
            if b == 10:
                self.newline()
            elif b == 13:
                self.c = 1
            elif b == 8:
                self.c = max(1, self.c - 1)
            elif b >= 0x20 and not 0x80 <= b < 0xc0:
                self.put(chr(b) if b < 0x80 else "?")
            i += 1
        self.buf = d[i:]
        return out

    def csi(self, params, final):
        p = [int(x) if x.isdigit() else 0 for x in params.lstrip(b"?").split(b";")]
        n = max(p[0], 1)
        if final in b"Hf":
            self.r = min(max(p[0], 1), self.rows)
            self.c = min(max(p[1] if len(p) > 1 else 1, 1), self.cols)
        elif final == b"A":
            self.r = max(1, self.r - n)
        elif final == b"B":
            self.r = min(self.rows, self.r + n)
        elif final == b"C":
            self.c = min(self.cols, self.c + n)
        elif final == b"D":
            self.c = max(1, self.c - n)
        elif final == b"J" and p[0] == 2:
            self.text, self.boxes = {}, []
        elif final == b"c" and not params.startswith(b">"):
            return b"\x1b[?62;4;22c"
        elif final == b"n" and p[0] == 6:
            return b"\x1b[%d;%dR" % (self.r, self.c)
        elif final == b"t" and p[0] == 16:
            return b"\x1b[6;%d;%dt" % (self.ch, self.cw)
        elif final == b"t" and p[0] == 18:
            return b"\x1b[8;%d;%dt" % (self.rows, self.cols)
        elif final == b"S" and params == b"?1;1;0":
            return b"\x1b[?1;0;256S"
        return b""


def run(script, args, size, env):
    cols, rows, cw, ch = size
    screen = Screen(cols, rows, cw, ch)
    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, cols * cw, rows * ch))
        try:
            os.execve("/bin/bash", ["bash", "-c", script, "page", *args], env)
        finally:
            os._exit(127)
    deadline = time.monotonic() + 120
    status = None
    while status is None:
        if time.monotonic() > deadline:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
            screen.faults.append("did not finish within 120 seconds")
            break
        ready, _, _ = select.select([fd], [], [], 0.5)
        data = b""
        if ready:
            try:
                data = os.read(fd, 65536)
            except OSError:
                pass
        if data:
            reply = screen.feed(data)
            if reply:
                os.write(fd, reply)
            continue
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            status = st
    os.close(fd)
    if status:
        screen.faults.append("exited with status %d" % os.waitstatus_to_exitcode(status))
    return screen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = ap.parse_args().root
    sections = os.path.join(root, "fork/demo/sections")
    demo = os.path.join(root, "fork/demo/demo.sh")

    kit = []
    for name in sorted(os.listdir(sections)):
        path = os.path.join(sections, name)
        with open(path) as f:
            if "# kit: sixel\n" in f.read():
                kit.append(name[:-3])
    missing = UNMODELLED - set(kit)
    if missing:
        print("not in the sixel kit any more: %s" % ", ".join(sorted(missing)))
        return 1
    pages = [n for n in kit if n not in UNMODELLED]

    with tempfile.TemporaryDirectory() as tmp:
        for tool, body in (("chafa", CHAFA), ("mpv", MPV)):
            path = os.path.join(tmp, tool)
            with open(path, "w") as f:
                f.write("#!/usr/bin/env python3\n" + SIXEL_FN + body)
            os.chmod(path, 0o755)
        env = {"PATH": tmp + ":/usr/bin:/bin", "TERM": "xterm-256color",
               "LANG": "C.UTF-8", "HOME": tmp}

        failed = runs = 0
        for name in pages:
            path = os.path.join(sections, name + ".sh")
            header = "[1/1] " + name[3:]
            for size in SIZES:
                label = "%s at %dx%d, %dx%d cell" % (name, *size)
                t3 = run(T3, ["", path], size, env)
                page = run(PAGE, [demo, path], size, env)
                faults = ["on t3's clear screen: " + f for f in t3.faults]
                faults += ["below demo.sh's header: " + f for f in page.faults]
                if name in APART:
                    faults += ["on t3's clear screen: " + f for f in t3.overlaps]
                    faults += ["below demo.sh's header: " + f for f in page.overlaps]
                # Below the header, the page draws what it draws on a clear
                # screen, two rows lower: every cell and picture, counting
                # the rows scrolled away.
                drawn = "".join(e[3] for e in page.events[:len(header)])
                if drawn != header:
                    faults.append("demo.sh's header reads %r, not %r" % (drawn, header))
                lower = [(e[0], e[1] + 2) + e[2:] for e in t3.events]
                below = page.events[len(header):]
                if not any(e[0] == "picture" for e in t3.events):
                    faults.append("the page drew no picture: %r"
                                  % "".join(e[3] for e in t3.events if e[0] == "text")[:160])
                elif below != lower:
                    k = next((i for i, (a, b) in enumerate(zip(below, lower)) if a != b),
                             min(len(below), len(lower)))
                    faults.append("below demo.sh's header, the page draws %s where it should "
                                  "draw %s, two rows below where it draws on a clear screen"
                                  % (describe(below, k), describe(lower, k)))
                runs += 1
                if faults:
                    failed += 1
                    print("FAIL %s:" % label)
                    for fault in faults:
                        print("    " + fault)
                else:
                    print("ok   %s: the same %d picture(s) below demo.sh's header"
                          % (label, sum(e[0] == "picture" for e in t3.events)))
        print("%d of %d failed" % (failed, runs))
        return 1 if failed else 0


def describe(events, k):
    if k >= len(events):
        return "nothing more"
    e = events[k]
    if e[0] == "text":
        return "%r at row %d, column %d" % (e[3], e[1], e[2])
    return "a %dx%d picture at row %d, column %d" % (e[3], e[4], e[1], e[2])


if __name__ == "__main__":
    sys.exit(main())
