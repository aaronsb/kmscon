"""The classic demo effects of the showcase (#75), in truecolor glyphs.

Half blocks (▀ with a 24-bit foreground and background) make each cell two
pixels: 384x108 at the operator's 384x54 cells. Braille (U+2800 to U+28FF)
makes each cell 2x4 dots in one colour: 768x216. Each effect is a class built
for a screen size, whose frame(t) returns the pixel rows (or the dots) at t
seconds; run() and run_dots() draw one alone, rebuild it when the window is
resized, and report frames per second and bytes per frame.
"""

import math
import random

from letters import TextMask
from screen import (DEFAULT, BraillePainter, Dots, Painter, Stats, clear, cup, frames, geom,
                    gradient, hsv, out, resized, scale, ESC)


# ------------------------------------------------------------------ plasma

class Plasma:
    """Two moving sine fields and a cycling palette; level (0..1) dims it,
    for a fade in."""

    LEVELS = 16

    def __init__(self, cols, rows):
        w, h = cols, rows * 2
        self.f = [[int(64 * math.sin(x / 19.0) + 64 * math.sin(math.hypot(x - w / 2, (y - h / 2) * 1.6) / 11.0))
                   for x in range(w)] for y in range(h)]
        self.hx = [int(48 * math.sin(x / 31.0)) for x in range(2 * w + 512)]
        self.gy = [int(48 * math.sin(y / 13.0)) for y in range(2 * h + 512)]
        self.w = w
        self.pals = [self.palette(i / self.LEVELS) for i in range(self.LEVELS + 1)]

    @staticmethod
    def palette(level):
        pal = []
        for i in range(256):
            q = i & ~3  # 64 colours, so neighbouring cells repeat theirs
            r = 0.5 + 0.5 * math.sin(math.pi * q / 64.0)
            g = 0.5 + 0.5 * math.sin(math.pi * q / 128.0 + 2.0)
            b = 0.5 + 0.5 * math.sin(math.pi * q / 32.0 + 4.0)
            pal.append((int(r * 200 * level) << 16) | (int(g * 120 * level) << 8) | int(b * 230 * level))
        return pal

    def frame(self, t, level=1.0):
        pal = self.pals[max(0, min(self.LEVELS, int(level * self.LEVELS)))]
        sx = int(t * 37) % 512
        sy = int(t * 23) % 512
        p = int(t * 60)
        hs = self.hx[sx:sx + self.w]
        gy = self.gy
        return [[pal[(a + b + c) & 255] for a, b in zip(fy, hs)]
                for fy, c in zip(self.f, (gy[y + sy] + p for y in range(len(self.f))))]


# --------------------------------------------------------------- shadebobs

class Shadebobs:
    """Blurred blobs on Lissajous paths, added into a buffer that fades a
    little each frame: where they cross, the brightness piles up."""

    def __init__(self, cols, rows, bobs=7):
        self.w, self.h = cols, rows * 2
        self.buf = [[0] * self.w for _ in range(self.h)]
        r = max(4, self.h // 8)
        self.r = r
        self.kernel = [(dx, dy, int(64 * (1 - math.hypot(dx, dy) / r) ** 2))
                       for dy in range(-r, r + 1) for dx in range(-r, r + 1) if math.hypot(dx, dy) < r]
        rnd = random.Random(3)
        self.paths = [(rnd.uniform(0.3, 0.9), rnd.uniform(0.4, 1.1), rnd.uniform(0, 6.3), rnd.uniform(0, 6.3))
                      for _ in range(bobs)]
        self.pal = gradient([(0, 0x000000), (0.25, 0x10106a), (0.5, 0xa02090), (0.75, 0xff8020), (1, 0xffffff)], 256)

    def frame(self, t, level=1.0):
        w, h, r, buf = self.w, self.h, self.r, self.buf
        for y in range(h):
            buf[y] = [v - (v >> 4) - (v > 0) for v in buf[y]]
        for a, b, p, q in self.paths:
            cx = int(w / 2 + (w / 2 - r) * math.sin(t * a + p))
            cy = int(h / 2 + (h / 2 - r) * math.sin(t * b + q))
            for dx, dy, v in self.kernel:
                x, y = cx + dx, cy + dy
                if 0 <= x < w and 0 <= y < h:
                    row = buf[y]
                    row[x] = min(255, row[x] + v)
        pal = self.pal if level >= 1 else [scale(c, level) for c in self.pal]
        return [[pal[v] for v in row] for row in buf]


# -------------------------------------------------------------------- fire

class Fire:
    """The wipe into act 6: a burning edge climbs the screen over seconds.
    Flames lick up from it into what was there, every cell they touch is
    charred black when it cools, and once the edge reaches the top the fire
    dies out, leaving the screen black. frame() returns the pixels and the
    mask of cells the fire has reached; the rest keep what was there."""

    def __init__(self, cols, rows, seconds=5.0):
        self.cols, self.rows = cols, rows
        self.h2 = rows * 2
        self.w = (cols + 1) // 2  # half the width, each pixel drawn twice
        self.heat = [[0] * self.w for _ in range(self.h2)]
        self.touched = [[False] * cols for _ in range(rows)]
        self.passed = [False] * rows  # rows the edge has passed: drawn whole
        self.pal = gradient([(0, 0x000000), (6 / 36, 0x1f0707), (12 / 36, 0x8f2707), (18 / 36, 0xc74707),
                             (24 / 36, 0xdf6f0f), (30 / 36, 0xcfa717), (33 / 36, 0xc7c72f), (1, 0xffffff)], 37)
        rnd = random.Random(6)
        self.noise = [rnd.randrange(4) for _ in range(65536)]
        self.ni = 0
        self.climb = seconds * 0.75

    def frame(self, t):
        h2, w, heat, noise = self.h2, self.w, self.heat, self.noise
        ni = self.ni
        # The classic fire: each pixel takes the heat of the one below it,
        # drifting sideways at random and cooling by one three times in four.
        # The source is the row at the edge, which climbs; below it the fire
        # goes out.
        edge = h2 - 1 - int((h2 - 1) * min(1.0, t / max(0.01, self.climb)))
        if t < self.climb:
            heat[edge] = [36] * w
        for _ in range(2):  # twice a frame, so the flames outrun the edge
            heat[h2 - 1] = [0] * w
            for y in range(1, h2):
                src = heat[y]
                dst = heat[y - 1]
                for x in range(w):
                    n = noise[ni]
                    ni = (ni + 1) & 65535
                    v = src[x]
                    if v and noise[ni]:
                        v -= 1
                    dst[min(w - 1, max(0, x - n + 1))] = v
        self.ni = ni
        pal, cols = self.pal, self.cols
        px = []
        for row in heat:
            line = []
            for v in row:
                c = pal[v]
                line += (c, c)
            px.append(line[:cols])
        for r in range(self.rows):
            if self.passed[r]:
                continue
            if 2 * r + 1 >= edge:
                self.touched[r] = [True] * cols
                self.passed[r] = True
                continue
            tr = self.touched[r]
            up, lo = heat[2 * r], heat[2 * r + 1]
            for x in range(cols):
                if not tr[x] and (up[x >> 1] or lo[x >> 1]):
                    tr[x] = True
        return px, self.touched


# ------------------------------------------------------------------ copper

COPPER = [0xff2040, 0xffa020, 0x20c0ff, 0xa040ff]


def copper_rows(cols, h2, t):
    """Raster bars bouncing over a dark gradient, one colour per pixel row."""
    base = [int(30 + 40 * y / max(1, h2)) for y in range(h2)]
    for i, c in enumerate(COPPER):
        centre = h2 / 2 + (h2 / 2 - 2) * math.sin(t * 1.7 + i * 1.3)
        for y in range(h2):
            d = abs(y - centre)
            if d < 3.5:
                base[y] = scale(c, 0.35 + 0.65 * (1 - d / 3.5))
    return [[c] * cols for c in base]


class Copper:
    def __init__(self, cols, rows):
        self.cols, self.h2 = cols, rows * 2

    def frame(self, t):
        return copper_rows(self.cols, self.h2, t)


# ------------------------------------------------------------------- stars

class Stars:
    """Stars flying out of the centre, behind a title in big letters and a
    subtitle on its own row, which text() draws once."""

    def __init__(self, cols, rows, title="", subtitle="", font="mono12"):
        self.cols, self.rows, self.h2 = cols, rows, rows * 2
        self.rnd = random.Random(1)
        self.stars = [[self.rnd.uniform(-1, 1), self.rnd.uniform(-1, 1), self.rnd.uniform(0.05, 1)]
                      for _ in range(max(200, cols * rows // 40))]
        self.mask = TextMask(title, font, cols, self.h2 - 8, max_scale=3) if title else None
        self.subtitle = subtitle[:cols]
        self.sub_row = rows - 3
        self.last = 0.0

    def frame(self, t):
        dt, self.last = t - self.last, t
        cols, h2 = self.cols, self.h2
        px = [[0] * cols for _ in range(h2)]
        for s in self.stars:
            s[2] -= dt * 0.35
            if s[2] <= 0.02:
                s[0], s[1], s[2] = self.rnd.uniform(-1, 1), self.rnd.uniform(-1, 1), 1.0
            x = int(cols / 2 + s[0] / s[2] * cols / 4)
            y = int(h2 / 2 + s[1] / s[2] * h2 / 4)
            if 0 <= x < cols and 0 <= y < h2 and not (self.subtitle and y // 2 == self.sub_row - 1):
                px[y][x] = scale(0xFFFFFF, min(1.0, 1.2 - s[2]))
        if self.mask and self.mask.ok:
            for x, y, _ in self.mask.pixels:
                if 0 <= y < h2 - 1 and x + 1 < cols:
                    px[y + 1][x + 1] = 0x202040
                    px[y][x] = hsv(0.55 + 0.1 * math.sin(t + x / 40.0), 0.5, 1.0)
        return px

    def text(self):
        if not self.subtitle:
            return ""
        return f"{cup(self.sub_row, max(1, (self.cols - len(self.subtitle)) // 2 + 1))}{ESC}[0;37;48;2;0;0;0m{self.subtitle}{ESC}[0m"


# -------------------------------------------------------- Braille wireframe

class Wireframe:
    """A torus turning in 3D, drawn as lines of Braille dots coloured by
    depth, over an oscilloscope trace across the whole width. Lines and a
    trace still read when a font draws the dots slightly off their grid."""

    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        nu, nv = 28, 12
        self.points = [[((2 + math.cos(v)) * math.cos(u), (2 + math.cos(v)) * math.sin(u), math.sin(v))
                        for v in (2 * math.pi * j / nv for j in range(nv))]
                       for u in (2 * math.pi * i / nu for i in range(nu))]

    def frame(self, t):
        d = Dots(self.cols, self.rows)
        w, h = d.w, d.h
        # The trace: a sine beating against a slower one, the full width.
        prev = None
        for x in range(0, w, 2):
            y = h / 2 + h * 0.38 * math.sin(x * 0.021 + t * 2.3) * math.sin(x * 0.0037 - t * 0.7)
            if prev:
                d.line(prev[0], prev[1], x, y, 0x1f6f3f)
            prev = (x, y)
        a, b = t * 0.9, t * 0.6
        ca, sa, cb, sb = math.cos(a), math.sin(a), math.cos(b), math.sin(b)
        size = min(w * 0.22, h * 0.95)
        proj = []
        for ring in self.points:
            pr = []
            for x, y, z in ring:
                y, z = y * ca - z * sa, y * sa + z * ca
                x, z = x * cb + z * sb, -x * sb + z * cb
                k = size / (z + 7.5) * 2.2
                pr.append((w / 2 + x * k, h / 2 + y * k, z))
            proj.append(pr)
        for i, ring in enumerate(proj):
            nxt = proj[(i + 1) % len(proj)]
            for j, (x, y, z) in enumerate(ring):
                c = hsv(0.5 + z * 0.08, 0.6, 0.55 + 0.15 * (3 - z) / 3)
                x2, y2, _ = ring[(j + 1) % len(ring)]
                d.line(x, y, x2, y2, c)
                x3, y3, _ = nxt[j]
                d.line(x, y, x3, y3, c)
        return d


# ------------------------------------------------------------------ runners

def run(make, seconds, fps, name, report=""):
    """Draws the effect make(cols, rows) alone for seconds (forever when 0),
    rebuilt at the new size after a resize."""
    stats = Stats(name)
    cols, rows = geom()[:2]
    eff = make(cols, rows)
    painter = Painter()
    text = getattr(eff, "text", None)
    fresh = True
    for _n, t in frames(seconds, fps):
        if resized():
            cols, rows = geom()[:2]
            eff = make(cols, rows)
            text = getattr(eff, "text", None)
            painter.reset()
            clear()
            fresh = True
        res = eff.frame(t)
        px, mask = res if isinstance(res, tuple) else (res, None)
        s = painter.draw(px, mask)
        if fresh and text:
            s += text()
            fresh = False
        stats.add(s)
        out(s)
    stats.report(report)


def run_dots(make, seconds, fps, name, report="", text=None):
    """run() for a Braille effect. text(cols, rows) returns (the escape
    string of any lines drawn over it, the rows they take)."""
    stats = Stats(name)
    painter = BraillePainter()
    cols = rows = None
    eff = None
    skip = ()
    for _n, t in frames(seconds, fps):
        if eff is None or resized():
            cols, rows = geom()[:2]
            words, skip = text(cols, rows) if text else ("", ())
            eff = make(cols, rows)
            painter.reset()
            clear()
            out(words)
        s = painter.draw(eff.frame(t), skip)
        stats.add(s)
        out(s)
    stats.report(report)


EFFECTS = {
    "plasma": ("the plasma behind act 2's letters", lambda c, r, a: Plasma(c, r), 0),
    "shadebobs": ("shadebobs behind act 2's letters, after the plasma", lambda c, r, a: Shadebobs(c, r), 40),
    "fire": ("the fire wipe into act 6", lambda c, r, a: Fire(c, r, a.seconds or 5), 40),
    "copper": ("the copper bars under act 7's scroller", lambda c, r, a: Copper(c, r), 40),
    "stars": ("the starfield title card before act 1 (TITLE_SECONDS)",
              lambda c, r, a: Stars(c, r, a.title, a.subtitle, a.font), 40),
}
DOT_EFFECTS = {
    "braille": ("a Braille wireframe behind the end card", lambda c, r, a: Wireframe(c, r), 40),
}


def cmd_effect(a):
    """--fx NAME: one effect alone. The fire, being a wipe, clears the
    screen when it has burnt out."""
    name = "stars" if a.name == "starfield" else a.name
    if name in DOT_EFFECTS:
        _, make, fps = DOT_EFFECTS[name]
        run_dots(lambda c, r: make(c, r, a), a.seconds, fps, name, a.report)
        return
    _, make, fps = EFFECTS[name]
    run(lambda c, r: make(c, r, a), a.seconds, fps, name, a.report)
    if name == "fire":
        clear()
