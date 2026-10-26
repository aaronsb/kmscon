"""The showcase's scenes made of text and effects (#75): act 2's big letters
over the plasma and shadebobs, act 7's scroller, the typed lines, the
placeholders, the cats and the end card.
"""

import math
import random
import time

from effects import Plasma, Shadebobs, Wireframe, copper_rows, run_dots
from letters import TextMask, banner_strip
from screen import (ESC, Painter, Stats, any_key_ends, box, clear, cup, frames, geom, hsv, out,
                    placement, resized, wait)


# ------------------------------------------------------------------ act 2

class BigText:
    """The letters of act 2 on a cols x rows screen, and when each appears:
    one every `letter` seconds, a space at once."""

    def __init__(self, text, font, cols, rows, letter):
        self.cols, self.h2 = cols, rows * 2
        self.mask = TextMask(text, font, cols, self.h2)
        self.text = text
        self.hues = [(x - self.mask.x0) / max(1, self.mask.width) for x in range(cols)] if self.mask.ok else []
        self.at = []
        t = 0.0
        for ch in text if self.mask.spans else ("x",):
            self.at.append(t)
            if ch != " ":
                t += letter
        self.done = t

    def draw_onto(self, px, t, letter):
        """Lays the letters shown at t over px, each new one white for the
        first part of its slot, with a one-pixel shadow."""
        h2, cols = self.h2, self.cols
        for x, y, li in self.mask.pixels:
            if self.at[li] > t or not (0 <= y < h2 - 1 and x + 1 < cols):
                continue
            px[y + 1][x + 1] = 0
            px[y][x] = 0xFFFFFF if t - self.at[li] < letter * 0.4 else hsv(self.hues[x] * 0.8 + t * 0.15, 0.75, 1.0)


def cmd_big(a):
    """Act 2: a black pause, then the letters slam in one by one and hold,
    then a plasma fades in behind them, then shadebobs. The pause counts
    from the command's start and the letters are drawn during it, so the
    first letter lands --pause seconds after the start, in time with the
    act's music cue started just before."""
    start = time.monotonic()
    cols, rows = geom()[:2]
    big = BigText(a.text, a.font, cols, rows, a.letter)
    wait(a.pause - (time.monotonic() - start))
    if not big.mask.ok:
        out(f"{cup(rows // 2, max(1, (cols - len(a.text)) // 2 + 1))}{ESC}[0;1;95m{a.text}{ESC}[0m")
        wait(a.letter * len(a.text) + a.hold + a.plasma + a.bobs)
        return
    plasma_at = big.done + a.hold
    bobs_at = plasma_at + a.plasma
    end = bobs_at + a.bobs
    plasma = Plasma(cols, rows) if a.plasma > 0 else None
    bobs = Shadebobs(cols, rows) if a.bobs > 0 else None
    painter = Painter()
    stats = Stats("big")
    clear()
    for _n, t in frames(end, a.fps):
        if resized():
            cols, rows = geom()[:2]
            big = BigText(a.text, a.font, cols, rows, a.letter)
            plasma = Plasma(cols, rows) if plasma else None
            bobs = Shadebobs(cols, rows) if bobs else None
            painter.reset()
            clear()
            if not big.mask.ok:
                return
        if t < plasma_at or not (plasma or bobs):
            px = [[0] * cols for _ in range(rows * 2)]
        elif t < bobs_at:
            px = plasma.frame(t, min(1.0, (t - plasma_at) / max(0.01, a.fade), (bobs_at - t) / 0.4 if bobs else 1.0))
        else:
            px = bobs.frame(t, min(1.0, (t - bobs_at) / 0.6))
        big.draw_onto(px, t, a.letter)
        s = painter.draw(px)
        stats.add(s)
        out(s)
    stats.report(a.report)


# ------------------------------------------------------------------ act 7

def placeholder_screen(cols, rows, text, title, w=None, h=None, phase=0):
    """A framed panel with static in it and text in its middle: what a
    graphics act shows where the terminal lacks the protocol."""
    w = min(w or cols - 6, cols - 4)
    h = min(h or rows - 4, rows - 2)
    top, left = placement(cols, rows, w, h)
    rnd = random.Random(phase)
    s = [box(top, left, w, h, title, "single", f"{ESC}[0;90m")]
    shades = " ░░▒"
    for y in range(h):
        line = "".join(rnd.choice(shades) for _ in range(w))
        s.append(f"{cup(top + y, left)}{ESC}[0;90m{line}")
    t = f"  {text}  "[:w]
    r = top + h // 2
    c = left + max(0, (w - len(t)) // 2)
    s.append(f"{cup(r - 1, c)}{ESC}[0;40m{' ' * len(t)}{cup(r, c)}{ESC}[0;1;97;40m{t}"
             f"{cup(r + 1, c)}{ESC}[0;40m{' ' * len(t)}{ESC}[0m")
    return "".join(s)


def cmd_banner(a):
    """The demoscene scroller along the bottom rows: big letters and colour
    emoji riding a sine wave over copper bars, one column further each
    frame. Drawn with the cursor saved and restored around each frame, so a
    player drawing above it (mpv) keeps its position. With --placeholder,
    the rows above show it instead of a video."""
    strip, emoji, fh = banner_strip(a.text, a.font)
    if not strip:
        return
    n = len(strip)
    stats = Stats("banner")
    painter = Painter()
    cols = None
    for frame_no, t in frames(a.seconds, a.fps):
        if cols is None or resized():
            cols, rows = geom()[:2]
            brows = min(a.rows, rows)
            h2 = brows * 2
            painter = Painter(top=rows - brows + 1)
            amp = max(0, (h2 - fh) // 2 - 1)
            y0 = (h2 - fh) // 2
            if a.placeholder:
                clear()
                vh = rows - brows - 2  # a 4:3 panel where the video would play
                out(placeholder_screen(cols, rows - brows, a.placeholder, a.title, w=vh * 8 // 3, h=vh))
        px = copper_rows(cols, h2, t)
        overlay = {}
        for c in range(cols):
            si = (c + frame_no) % n
            dy = int(amp * math.sin(c / 18.0 + t * 3.0))
            col = strip[si]
            if col:
                hue = (c / cols + t * 0.2) % 1.0
                for py in col:
                    y = y0 + py + dy
                    if 0 <= y < h2:
                        if y + 1 < h2 and c + 1 < cols:
                            px[y + 1][c + 1] = 0x101010
                        px[y][c] = 0xFFFFFF if py < 2 else hsv(hue, 0.35, 1.0)
            e = emoji.get(si)
            if e is not None and c + 1 < cols:
                r = max(0, min(brows - 1, (y0 + fh // 2 + dy) // 2))
                overlay.setdefault(r, {})[c] = e
        s = painter.draw(px, overlay=overlay)
        if s:
            s = f"{ESC}7{s}{ESC}8"
        stats.add(s)
        out(s)
    stats.report(a.report)


def cmd_placeholder(a):
    cols, rows = geom()[:2]
    phase = 0
    end = time.monotonic() + a.seconds
    while time.monotonic() < end:
        if resized():
            cols, rows = geom()[:2]
            clear()
        out(placeholder_screen(cols, rows, a.text, a.title, a.cols or None, a.rows or None, phase))
        phase += 1
        wait(0.12)


# ----------------------------------------------------------------- typing

def cmd_type(a):
    """Types the lines centred on the rows from --row, CAPITALISED words in
    bold yellow, with a pause after each '...'."""
    cols, rows = geom()[:2]
    rnd = random.Random(4)
    row = a.row if a.row > 0 else rows // 2 - len(a.lines) // 2
    for n, text in enumerate(a.lines):
        width = len(text)
        col = max(1, (cols - width) // 2 + 1)
        r = row + n * 2
        if a.bar:
            out(f"{cup(r, max(1, col - 2))}{ESC}[0;40m{' ' * (width + 4)}{ESC}[0m")
        out(cup(r, col))
        words = text.split(" ")
        i = 0
        for wi, word in enumerate(words):
            loud = len(word) > 2 and word.strip("?!.,").isupper()
            style = f"{ESC}[0;1;93;40m" if loud else f"{ESC}[0;97;40m"
            for ch in word + (" " if wi + 1 < len(words) else ""):
                out(f"{style}{ch}{ESC}[0;97;40m▌{ESC}[D")
                i += 1
                d = 1.0 / a.cps * (0.6 + 0.8 * rnd.random())
                if text[:i].endswith("..."):
                    d += a.dots
                wait(d)
        out(f"{ESC}[0;40m {ESC}[0m")
        wait(a.dots / 2)
    wait(a.hold)


# ------------------------------------------------------------------- cats

CATS = ["🐱", "🐈", "😺", "😸", "🙀", "🐈‍⬛", "😻", "😹", "😼", "😽", "😿", "😾"]


def cmd_cats(a):
    """A crowd of colour cat emoji popping in below row --top, faster and
    faster, then a few changing faces."""
    cols, rows = geom()[:2]
    rnd = random.Random(7)
    spots = [(r, c) for r in range(a.top, rows) for c in range(1, cols - 1, 4)]
    rnd.shuffle(spots)
    spots = spots[:max(1, int(len(spots) * a.density))]
    end = time.monotonic() + a.seconds
    n = len(spots)
    placed = []
    t0 = time.monotonic()
    pop = a.seconds * 0.7
    i = 0
    while time.monotonic() < end:
        t = time.monotonic() - t0
        want = n if t >= pop else int(n * (t / pop) ** 2)
        s = []
        while i < want:
            r, c = spots[i]
            c += rnd.randrange(2)
            s.append(f"{cup(r, c)}{rnd.choice(CATS)}")
            placed.append((r, c))
            i += 1
        if placed and t >= pop:
            for _ in range(max(1, len(placed) // 20)):
                r, c = rnd.choice(placed)
                s.append(f"{cup(r, c)}{rnd.choice(CATS)}")
        if s:
            out("".join(s))
        wait(1 / 30)


# --------------------------------------------------------------- end card

def cmd_endcard(a):
    """The credits over the Braille wireframe, until a key."""

    def text(cols, rows):
        r = rows // 2
        lines = [(r - 1, f"{ESC}[0;1;97m", a.title), (r + 1, f"{ESC}[0;37m", a.text)]
        s = "".join(f"{cup(row, max(1, (cols - len(t[:cols])) // 2 + 1))}{style}{t[:cols]}{ESC}[0m"
                    for row, style, t in lines)
        return s, {row - 1 for row, _, _ in lines}

    any_key_ends()
    run_dots(lambda c, r: Wireframe(c, r), 0, 40, "endcard", text=text)
