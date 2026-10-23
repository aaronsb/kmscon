#!/usr/bin/env python3
"""Drawing and rendering helpers for fork/showcase/bad-apple.sh (#75).

Everything that draws more than a line of text lives here: the frame caches
the video acts play (prep-outline, prep-caca), their player (play), the big
letters, the typed lines, the placeholders, the cats, the scrolling banner and
the half-block effects (plasma, fire, copper bars, starfield). Standard library
only; ffmpeg, img2txt and toilet are run as programs.

Each frame is one os.write(), so a frame never interleaves with another
process writing to the same terminal (mpv, in act 7): the kernel holds the
tty's write lock for the whole call.

Keys: q ends the show (exit status 10, which bad-apple.sh takes as "quit");
Ctrl-C exits 130. The terminal's mode is left as the caller set it, except
that a run on its own puts it in cbreak mode and restores it.

    fx.py COMMAND [OPTIONS]      (fx.py COMMAND --help for each)
"""

import argparse
import colorsys
import fcntl
import json
import math
import os
import random
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty

QUIT = 10
ESC = "\x1b"


class Quit(Exception):
    pass


# ---------------------------------------------------------------- terminal

def out(s):
    """Write s in one call: one frame, one write()."""
    b = s.encode()
    while b:
        n = os.write(1, b)
        b = b[n:]


def geom():
    """(cols, rows, xpixel, ypixel) of the terminal; pixels are 0 when unknown."""
    for fd in (1, 0, 2):
        try:
            rows, cols, xp, yp = struct.unpack("HHHH", fcntl.ioctl(fd, termios.TIOCGWINSZ, b"\0" * 8))
            if cols and rows:
                return cols, rows, xp, yp
        except OSError:
            pass
    c, r = shutil.get_terminal_size((80, 25))
    return c, r, 0, 0


def poll_keys():
    """Reads pending keys without waiting; raises Quit on q."""
    try:
        if not os.isatty(0):
            return
        while select.select([0], [], [], 0)[0]:
            b = os.read(0, 256)
            if not b:
                return
            if b"q" in b or b"Q" in b:
                raise Quit
    except OSError:
        return


def wait(sec):
    """Sleeps sec seconds, reading keys as they come."""
    end = time.monotonic() + max(0.0, sec)
    tty_in = os.isatty(0)
    while True:
        left = end - time.monotonic()
        if left <= 0:
            poll_keys()
            return
        if tty_in:
            if select.select([0], [], [], left)[0]:
                poll_keys()
        else:
            time.sleep(left)


def cup(row, col):
    return f"{ESC}[{row};{col}H"


def hsv(h, s=1.0, v=1.0):
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, s, v)
    return (int(r * 255) << 16) | (int(g * 255) << 8) | int(b * 255)


def scale(c, f):
    return (min(255, int((c >> 16) * f)) << 16) | (min(255, int(((c >> 8) & 255) * f)) << 8) | min(255, int((c & 255) * f))


def rgb_fg(c):
    return f"{ESC}[38;2;{c >> 16};{(c >> 8) & 255};{c & 255}m"


def rgb_bg(c):
    return f"{ESC}[48;2;{c >> 16};{(c >> 8) & 255};{c & 255}m"


# --------------------------------------------------------------- half blocks

DEFAULT = -1  # the terminal's own background

_fg_codes = {}
_bg_codes = {}


def _fg(c):
    s = _fg_codes.get(c)
    if s is None:
        s = _fg_codes[c] = f"38;2;{c >> 16};{(c >> 8) & 255};{c & 255}"
    return s


def _bg(c):
    s = _bg_codes.get(c)
    if s is None:
        s = _bg_codes[c] = "49" if c == DEFAULT else f"48;2;{c >> 16};{(c >> 8) & 255};{c & 255}"
    return s


def render(px, top=1, left=1, mask=None, overlay=None):
    """The escape string drawing pixel rows px (two per cell row, colours as
    0xRRGGBB or DEFAULT) with ▀/▄, from cell (top, left). SGR is sent only when
    a colour changes. mask[r][x], when given, says which cells to draw; the
    rest are left as they are. overlay[r] maps a column to a two-cell glyph
    (an emoji) drawn on that cell's top colour."""
    parts = []
    w = len(px[0])
    for r in range(len(px) // 2):
        up = px[2 * r]
        lo = px[2 * r + 1]
        mrow = mask[r] if mask is not None else None
        orow = overlay.get(r) if overlay else None
        fg = bg = None
        x = 0
        moved = False
        parts.append(cup(top + r, left))
        while x < w:
            if mrow is not None and not mrow[x]:
                x += 1
                moved = True
                continue
            if moved:
                parts.append(cup(top + r, left + x))
                moved = False
            if orow and x in orow and x + 1 < w:
                c = up[x]
                parts.append(f"{ESC}[0;{_bg(c)}m{orow[x]}")
                fg, bg = None, c
                x += 2
                continue
            t = up[x]
            b = lo[x]
            codes = []
            if t == b:
                if fg == t and t != DEFAULT:
                    ch = "█"
                else:
                    ch = " "
                    if bg != t:
                        codes.append(_bg(t))
                        bg = t
            elif t == DEFAULT or b == DEFAULT:
                if t == DEFAULT:
                    ch, f, g = "▄", b, DEFAULT
                else:
                    ch, f, g = "▀", t, DEFAULT
                if fg != f:
                    codes.append(_fg(f))
                    fg = f
                if bg != g:
                    codes.append(_bg(g))
                    bg = g
            else:
                upper = (fg != t) + (bg != b)
                lower = (fg != b) + (bg != t)
                if lower < upper:
                    ch, f, g = "▄", b, t
                else:
                    ch, f, g = "▀", t, b
                if fg != f:
                    codes.append(_fg(f))
                    fg = f
                if bg != g:
                    codes.append(_bg(g))
                    bg = g
            if codes:
                parts.append(f"{ESC}[{';'.join(codes)}m{ch}")
            else:
                parts.append(ch)
            x += 1
    parts.append(f"{ESC}[0m")
    return "".join(parts)


class Stats:
    """Frames drawn, bytes sent and time taken, for --report."""

    def __init__(self, name):
        self.name = name
        self.frames = 0
        self.bytes = 0
        self.t0 = time.monotonic()

    def add(self, s):
        self.frames += 1
        self.bytes += len(s.encode())

    def line(self):
        dt = max(1e-6, time.monotonic() - self.t0)
        per = self.bytes // max(1, self.frames)
        return (f"{self.name}: {self.frames} frames in {dt:.1f} s = {self.frames / dt:.1f} fps, "
                f"{per} bytes per frame, {self.bytes / dt / 1e6:.1f} MB/s")


def report(stats, path):
    if path:
        with open(path, "a") as f:
            f.write(stats.line() + "\n")


# ------------------------------------------------------------------ big text

def toilet_rows(text, font, trim=True):
    """Pixel rows (lists of 0/1) of text set in a toilet font that draws
    with ▀ ▄ █, two per line of toilet's output; None when toilet or the font
    is missing. Untrimmed, every glyph of a font has the same height."""
    if not shutil.which("toilet"):
        return None
    try:
        p = subprocess.run(["toilet", "-W", "-w", "4000", "-f", font, "--", text],
                           capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.returncode != 0:
        return None
    lines = p.stdout.rstrip("\n").split("\n")
    w = max((len(l) for l in lines), default=0)
    top = {"▀": 1, "█": 1, "▄": 0, " ": 0}
    bot = {"▀": 0, "█": 1, "▄": 1, " ": 0}
    rows = []
    for l in lines:
        l = l.ljust(w)
        rows.append([top.get(ch, 1) for ch in l])
        rows.append([bot.get(ch, 1) for ch in l])
    return trim_rows(rows) if trim else rows


def trim_rows(rows):
    while rows and not any(rows[0]):
        rows.pop(0)
    while rows and not any(rows[-1]):
        rows.pop()
    return rows


def figlet_rows(text):
    """Fallback: figlet's banner font, each mark a full cell (two pixels)."""
    if not shutil.which("figlet"):
        return None
    try:
        p = subprocess.run(["figlet", "-f", "banner", "-w", "4000", "--", text],
                           capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.returncode != 0:
        return None
    lines = p.stdout.rstrip("\n").split("\n")
    w = max((len(l) for l in lines), default=0)
    rows = []
    for l in lines:
        r = [0 if ch == " " else 1 for ch in l.ljust(w)]
        rows += [r, list(r)]
    return trim_rows(rows)


def glyph_strip(text, font):
    """The bitmap of text set one character at a time, so each letter's
    columns are known: (rows, [(x0, x1) per character]), or (None, None)."""
    glyphs = []
    for ch in text:
        g = toilet_rows("i" if ch == " " else ch, font, trim=False)
        if g is None:
            return None, None
        if ch == " ":
            g = [[0] * len(g[0]) for _ in g]
        glyphs.append(g)
    h = max(len(g) for g in glyphs)
    rows, spans, x = [[] for _ in range(h)], [], 0
    for g in glyphs:
        gw = len(g[0])
        for y in range(h):
            rows[y] += g[y] if y < len(g) else [0] * gw
        spans.append((x, x + gw))
        x += gw
    return trim_rows(rows), spans


# ------------------------------------------------------------ frame caches

def video_size(video):
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                        "-show_entries", "stream=width,height", "-of", "csv=p=0", video],
                       capture_output=True, text=True, check=True)
    w, h = p.stdout.strip().split("\n")[0].split(",")[:2]
    return int(w), int(h)


def ffmpeg_gray(video, start, seconds, fps, w, h):
    """Yields frames of w*h grey bytes."""
    cmd = ["ffmpeg", "-v", "error", "-nostdin", "-ss", str(start)]
    if seconds > 0:
        cmd += ["-t", str(seconds)]
    cmd += ["-i", video, "-an", "-vf", f"fps={fps},scale={w}:{h}:flags=area,format=gray",
            "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = w * h
    try:
        while True:
            b = p.stdout.read(n)
            if len(b) < n:
                break
            yield b
    finally:
        p.stdout.close()
        p.wait()


# Marching squares: the four sub-pixels of a cell (top-left 8, top-right 4,
# bottom-left 2, bottom-right 1; set where the picture is dark) give the
# outline's direction through it.
OUTLINE = {
    0b0000: " ", 0b1111: " ",
    0b1100: "-", 0b0011: "-",
    0b1010: "|", 0b0101: "|",
    0b1000: "/", 0b0111: "/", 0b0001: "/", 0b1110: "/",
    0b0100: "\\", 0b1011: "\\", 0b0010: "\\", 0b1101: "\\",
    0b1001: "\\", 0b0110: "/",
}


def write_frames(path, meta, frames):
    tmp = path + ".part"
    with open(tmp, "w") as f:
        meta = dict(meta, frames=len(frames))
        f.write(json.dumps(meta) + "\n")
        for fr in frames:
            for line in fr:
                f.write(line + "\n")
    os.replace(tmp, path)


def read_frames(path):
    with open(path) as f:
        meta = json.loads(f.readline())
        lines = f.read().split("\n")
    h = meta["rows"]
    frames = [lines[i * h:(i + 1) * h] for i in range(meta["frames"])]
    return meta, frames


def cmd_prep_outline(a):
    vw, vh = video_size(a.video)
    rows = a.rows
    cols = a.cols or max(8, round(rows * vw / vh * a.cell_ratio))
    w2, h2 = cols * 2, rows * 2
    frames = []
    for b in ffmpeg_gray(a.video, a.start, a.seconds, a.fps, w2, h2):
        fr = []
        for y in range(rows):
            r0 = (2 * y) * w2
            r1 = r0 + w2
            line = []
            for x in range(cols):
                i = 2 * x
                bits = ((b[r0 + i] < 128) << 3 | (b[r0 + i + 1] < 128) << 2
                        | (b[r1 + i] < 128) << 1 | (b[r1 + i + 1] < 128))
                line.append(OUTLINE[bits] if bits != 15 else a.fill)
            fr.append("".join(line))
        frames.append(fr)
    if not frames:
        sys.exit(f"fx: no frames from {a.video} at {a.start}s")
    write_frames(a.out, {"fps": a.fps, "cols": cols, "rows": rows, "style": "outline"}, frames)


def caca_sgr(line):
    """img2txt's line with blink (its trick for bright backgrounds) removed
    and its colours folded to greys, as aalib drew them."""
    fg_map = {"31": "37", "32": "37", "33": "37", "34": "37", "35": "37", "36": "37",
              "91": "97", "92": "97", "93": "97", "94": "97", "95": "97", "96": "97"}
    bg_map = {"41": "40", "42": "40", "43": "40", "44": "40", "45": "40", "46": "40",
              "101": "100", "102": "100", "103": "100", "104": "100", "105": "100", "106": "100"}
    outp, i = [], 0
    while i < len(line):
        if line.startswith(ESC + "[", i):
            j = line.find("m", i)
            if j < 0:
                break
            params = [p for p in line[i + 2:j].split(";") if p != "5"]
            params = [fg_map.get(p, bg_map.get(p, p)) for p in params]
            outp.append(f"{ESC}[{';'.join(params)}m")
            i = j + 1
        else:
            outp.append(line[i])
            i += 1
    return "".join(outp)


def cmd_prep_caca(a):
    frames = []
    with tempfile.TemporaryDirectory(prefix="showcase-caca-") as tmp:
        cmd = ["ffmpeg", "-v", "error", "-nostdin", "-ss", str(a.start), "-t", str(a.seconds),
               "-i", a.video, "-an", "-vf", f"fps={a.fps},scale=320:-2,format=gray",
               os.path.join(tmp, "f%05d.pgm")]
        subprocess.run(cmd, check=True)
        names = sorted(n for n in os.listdir(tmp) if n.endswith(".pgm"))
        for n in names:
            p = subprocess.run(["img2txt", "-W", str(a.cols), "-H", str(a.rows), "-d", a.dither,
                                "-f", "utf8", os.path.join(tmp, n)],
                               capture_output=True, text=True, check=True)
            lines = p.stdout.rstrip("\n").split("\n")[:a.rows]
            lines += [""] * (a.rows - len(lines))
            frames.append([caca_sgr(l) for l in lines])
    if not frames:
        sys.exit(f"fx: no frames from {a.video} at {a.start}s")
    write_frames(a.out, {"fps": a.fps, "cols": a.cols, "rows": a.rows, "style": "caca"}, frames)


# -------------------------------------------------------------------- play

BOX = {"double": "╔═╗║╚╝", "single": "┌─┐│└┘"}


def box(top, left, w, h, title="", style="double", colour=None):
    """A frame around the h x w cells from (top, left); title on its top edge."""
    tl, hz, tr, vt, bl, br = BOX[style]
    c = colour or f"{ESC}[0;37m"
    s = [c, cup(top - 1, left - 1), tl, hz * w, tr]
    for r in range(h):
        s += [cup(top + r, left - 1), vt, cup(top + r, left + w), vt]
    s += [cup(top + h, left - 1), bl, hz * w, br]
    if title:
        t = f" {title} "[:w]
        s += [cup(top - 1, left + max(0, (w - len(t)) // 2)), t]
    s.append(f"{ESC}[0m")
    return "".join(s)


def placement(cols, rows, w, h):
    return max(1, (rows - h) // 2 + 1), max(1, (cols - w) // 2 + 1)


def cmd_play(a):
    meta, frames = read_frames(a.frames)
    cols, rows, _, _ = geom()
    w, h = meta["cols"], meta["rows"]
    top, left = placement(cols, rows, w, h)
    colour = {"outline": f"{ESC}[0;37m", "caca": ""}.get(meta.get("style"), "")
    if a.box is not None:
        out(box(top, left, w, h, a.box))
    if a.caption:
        out(f"{cup(min(rows, top + h + (1 if a.box is None else 2)), left)}{ESC}[0;2m{a.caption[:w]}{ESC}[0m")
    fps = meta["fps"]
    t0 = time.monotonic()
    stats = Stats("play")
    for i, fr in enumerate(frames):
        due = t0 + i / fps
        now = time.monotonic()
        if now > due + 1.0 / fps and i + 1 < len(frames):
            poll_keys()
            continue  # late: drop it and keep time with the music
        wait(due - now)
        s = "".join(cup(top + y, left) + colour + line + f"{ESC}[0m" for y, line in enumerate(fr))
        stats.add(s)
        out(s)
    wait(t0 + len(frames) / fps - time.monotonic())
    if a.broken:
        out(broken(top, left, w, h, frames[-1], a.box))
    report(stats, a.report)


def broken(top, left, w, h, last, title):
    """The player freezing: bands of the frame torn sideways, a few rows of
    attribute garbage, and the box's title saying so."""
    rnd = random.Random(75)
    s = []
    junk = "▒░▓█▄▀■≡±÷°∙·¬½¼«»αßΓπΣσµτΦΘΩδ∞φε∩"
    for band in (rnd.randrange(h // 5, h // 2), rnd.randrange(h // 2, h - 2)):
        for y in range(band, min(h, band + rnd.randrange(1, 3))):
            shift = rnd.randrange(3, 12)
            plain = strip_sgr(last[y])
            plain = (plain[shift:] + plain[:shift]).ljust(w)[:w]
            s.append(f"{cup(top + y, left)}{ESC}[0;1;37;100m{plain}{ESC}[0m")
    for _ in range(3):
        y = rnd.randrange(h)
        x = rnd.randrange(w - 12)
        run = "".join(rnd.choice(junk) for _ in range(rnd.randrange(4, 12)))
        s.append(f"{cup(top + y, left + x)}{ESC}[0;{rnd.choice((31, 32, 33, 35, 36))};{rnd.choice((40, 44))}m{run}{ESC}[0m")
    if title is not None:
        t = " FROZEN "
        s.append(f"{cup(top - 1, left + max(0, (w - len(t)) // 2))}{ESC}[0;5;1;31m{t}{ESC}[0m")
    return "".join(s)


def strip_sgr(s):
    o, i = [], 0
    while i < len(s):
        if s.startswith(ESC + "[", i):
            j = s.find("m", i)
            i = len(s) if j < 0 else j + 1
        else:
            o.append(s[i])
            i += 1
    return "".join(o)


# ----------------------------------------------------------------- typing

def cmd_type(a):
    """Types the lines centred on the rows from --row, CAPITALISED words in
    bold yellow, with a pause after each '...'."""
    cols, rows, _, _ = geom()
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


# ------------------------------------------------------------- big letters

def text_mask(text, font, cols, rows2, max_scale=4):
    """(pixels, spans, scale, x0, y0, width): the lit pixels of text centred
    on a cols x rows2 pixel canvas, as (x, y, letter index), set in font or,
    where that is too wide, in mono9 or figlet's banner font. None when no
    font fits or none is installed."""
    for f in dict.fromkeys((font, "mono9", None)):
        if f is None:
            bm, spans = figlet_rows(text), None
        else:
            bm, spans = glyph_strip(text, f)
        if bm and len(bm[0]) <= cols - 2 and len(bm) <= rows2 - 2:
            break
    else:
        return None
    w, h = len(bm[0]), len(bm)
    k = max(1, min(max_scale, (cols - 4) // w, (rows2 - 4) // h))
    x0 = (cols - w * k) // 2
    y0 = (rows2 - h * k) // 2
    letter_of = [0] * w
    if spans:
        for li, (s0, s1) in enumerate(spans):
            for x in range(s0, min(s1, w)):
                letter_of[x] = li
    pix = []
    for y in range(h):
        for x in range(w):
            if bm[y][x]:
                for dy in range(k):
                    for dx in range(k):
                        pix.append((x0 + x * k + dx, y0 + y * k + dy, letter_of[x]))
    return pix, spans, k, x0, y0, w * k


def plasma_field(w, h):
    f = [[int(64 * math.sin(x / 19.0) + 64 * math.sin(math.hypot(x - w / 2, (y - h / 2) * 1.6) / 11.0))
          for x in range(w)] for y in range(h)]
    hx = [int(48 * math.sin(x / 31.0)) for x in range(2 * w + 512)]
    gy = [int(48 * math.sin(y / 13.0)) for y in range(2 * h + 512)]
    return f, hx, gy


def plasma_palette(level):
    pal = []
    for i in range(256):
        q = i & ~3
        r = 0.5 + 0.5 * math.sin(math.pi * q / 64.0)
        g = 0.5 + 0.5 * math.sin(math.pi * q / 128.0 + 2.0)
        b = 0.5 + 0.5 * math.sin(math.pi * q / 32.0 + 4.0)
        pal.append((int(r * 200 * level) << 16) | (int(g * 120 * level) << 8) | int(b * 230 * level))
    return pal


def plasma_rows(field, t, pal):
    f, hx, gy = field
    w = len(f[0])
    sx = int(t * 37) % 512
    sy = int(t * 23) % 512
    p = int(t * 60)
    hs = hx[sx:sx + w]
    rows = []
    for y, fy in enumerate(f):
        c = gy[y + sy] + p
        rows.append([pal[(a + b + c) & 255] for a, b in zip(fy, hs)])
    return rows


def blank_rows(w, h, colour=DEFAULT):
    return [[colour] * w for _ in range(h)]


def cmd_big(a):
    """Act 2: the letters slam in one by one on a black screen, hold, then a
    plasma fades in behind them."""
    cols, rows, _, _ = geom()
    h2 = rows * 2
    m = text_mask(a.text, a.font, cols, h2)
    if m is None:
        out(f"{cup(rows // 2, max(1, (cols - len(a.text)) // 2 + 1))}{ESC}[0;1;95m{a.text}{ESC}[0m")
        wait(a.letter * len(a.text) + a.hold + a.plasma)
        return
    pix, spans, k, x0, y0, tw = m
    hues = [((x - x0) / max(1, tw)) for x in range(cols)]
    nletters = len(spans) if spans else 1
    stats = Stats("big")

    def frame(shown, flash, t, level):
        px = plasma_rows(field, t, pals[level]) if level > 0 else blank_rows(cols, h2, 0)
        for x, y, li in pix:
            if li < shown and 0 <= y < h2:
                if y + 1 < h2 and x + 1 < cols:
                    px[y + 1][x + 1] = 0
                px[y][x] = 0xFFFFFF if li == flash else hsv(hues[x] * 0.8 + t * 0.15, 0.75, 1.0)
        return px

    field = plasma_field(cols, h2) if a.plasma > 0 else None
    pals = [plasma_palette(i / 16.0) for i in range(17)] if a.plasma > 0 else None
    out(f"{ESC}[0m{ESC}[2J")
    t0 = time.monotonic()
    for li in range(nletters):
        if spans and a.text[li] == " ":
            continue
        s = render(frame(li + 1, li, time.monotonic() - t0, 0))
        stats.add(s)
        out(s)
        wait(a.letter * 0.4)
        s = render(frame(li + 1, -1, time.monotonic() - t0, 0))
        out(s)
        wait(a.letter * 0.6)
    end = time.monotonic() + a.hold
    while time.monotonic() < end:
        s = render(frame(nletters, -1, time.monotonic() - t0, 0))
        stats.add(s)
        out(s)
        wait(1 / 30)
    if a.plasma > 0:
        start = time.monotonic()
        end = start + a.plasma
        while time.monotonic() < end:
            t = time.monotonic() - start
            level = min(16, int(16 * t / max(0.01, a.fade)))
            s = render(frame(nletters, -1, time.monotonic() - t0, level))
            stats.add(s)
            out(s)
            poll_keys()
    report(stats, a.report)


# ------------------------------------------------------------------ effects

def fire_palette():
    stops = [(0, 0x000000), (6, 0x1f0707), (12, 0x8f2707), (18, 0xc74707), (24, 0xdf6f0f),
             (30, 0xcfa717), (33, 0xc7c72f), (36, 0xffffff)]
    pal = []
    for i in range(37):
        for (a0, c0), (a1, c1) in zip(stops, stops[1:]):
            if a0 <= i <= a1:
                f = (i - a0) / (a1 - a0)
                pal.append(sum(int(((c0 >> s) & 255) * (1 - f) + ((c1 >> s) & 255) * f) << s for s in (16, 8, 0)))
                break
    return pal


def cmd_fire(a):
    """The wipe into act 6: a burning edge climbs the screen. Flames lick up
    from it into what was there, every cell they touch is charred black when
    it cools, and once the edge reaches the top the fire dies out, leaving
    the screen black. Cells the fire never reached keep what was there."""
    cols, rows, _, _ = geom()
    h2 = rows * 2
    w = (cols + 1) // 2  # the fire runs at half the width, each pixel drawn twice
    heat = [[0] * w for _ in range(h2)]
    touched = [[False] * cols for _ in range(rows)]
    passed = [False] * rows  # rows the edge has passed: drawn whole
    pal = fire_palette()
    rnd = random.Random(6)
    noise = [rnd.randrange(4) for _ in range(65536)]
    ni = 0
    stats = Stats("fire")
    t0 = time.monotonic()
    climb = a.seconds * 0.75
    while True:
        t = time.monotonic() - t0
        if t >= a.seconds:
            break
        # The classic fire: each pixel takes the heat of the one below it,
        # drifting sideways at random and cooling by one three times in four. The source
        # is the row at the edge, which climbs; below it the fire goes out.
        edge = h2 - 1 - int((h2 - 1) * min(1.0, t / max(0.01, climb)))
        if t < climb:
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
        px = []
        for y in range(h2):
            line = []
            for v in heat[y]:
                c = pal[v]
                line += (c, c)
            px.append(line[:cols])
        for r in range(rows):
            if passed[r]:
                continue
            if 2 * r + 1 >= edge:
                touched[r] = [True] * cols
                passed[r] = True
                continue
            tr = touched[r]
            up, lo = heat[2 * r], heat[2 * r + 1]
            for x in range(cols):
                if not tr[x] and (up[x >> 1] or lo[x >> 1]):
                    tr[x] = True
        s = render(px, mask=touched)
        stats.add(s)
        out(s)
        wait(1 / 40)
    out(f"{ESC}[0m{ESC}[2J")
    report(stats, a.report)


def cmd_plasma(a):
    cols, rows, _, _ = geom()
    h2 = rows * 2
    field = plasma_field(cols, h2)
    pal = plasma_palette(1.0)
    stats = Stats("plasma")
    t0 = time.monotonic()
    while a.seconds <= 0 or time.monotonic() - t0 < a.seconds:
        s = render(plasma_rows(field, time.monotonic() - t0, pal))
        stats.add(s)
        out(s)
        poll_keys()
    report(stats, a.report)


def cmd_starfield(a):
    """Stars flying out of the centre, behind the title in big letters."""
    cols, rows, _, _ = geom()
    h2 = rows * 2
    rnd = random.Random(1)
    stars = [[rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(0.05, 1)] for _ in range(max(200, cols * rows // 40))]
    m = text_mask(a.title, a.font, cols, h2, max_scale=3) if a.title else None
    sub = a.subtitle
    stats = Stats("starfield")
    t0 = last = time.monotonic()
    while a.seconds <= 0 or time.monotonic() - t0 < a.seconds:
        now = time.monotonic()
        dt, last = now - last, now
        px = blank_rows(cols, h2, 0)
        for s in stars:
            s[2] -= dt * 0.35
            if s[2] <= 0.02:
                s[0], s[1], s[2] = rnd.uniform(-1, 1), rnd.uniform(-1, 1), 1.0
            x = int(cols / 2 + s[0] / s[2] * cols / 4)
            y = int(h2 / 2 + s[1] / s[2] * h2 / 4)
            if 0 <= x < cols and 0 <= y < h2:
                v = min(1.0, 1.2 - s[2])
                px[y][x] = scale(0xFFFFFF, v)
        if m:
            t = now - t0
            for x, y, _ in m[0]:
                if 0 <= y < h2:
                    if y + 1 < h2 and x + 1 < cols:
                        px[y + 1][x + 1] = 0x202040
                    px[y][x] = hsv(0.55 + 0.1 * math.sin(t + x / 40.0), 0.5, 1.0)
        s = render(px)
        if sub:
            s += f"{cup(rows - 3, max(1, (cols - len(sub)) // 2 + 1))}{ESC}[0;37;48;2;0;0;0m{sub}{ESC}[0m"
        stats.add(s)
        out(s)
        wait(1 / 40)
    report(stats, a.report)


COPPER = [0xff2040, 0xffa020, 0x20c0ff, 0xa040ff]


def copper_rows(cols, h2, t):
    """Raster bars bouncing over a dark gradient, one colour per pixel row."""
    base = [(0 << 16) | (0 << 8) | int(30 + 40 * y / max(1, h2)) for y in range(h2)]
    for i, c in enumerate(COPPER):
        centre = h2 / 2 + (h2 / 2 - 2) * math.sin(t * 1.7 + i * 1.3)
        for y in range(h2):
            d = abs(y - centre)
            if d < 3.5:
                base[y] = scale(c, 0.35 + 0.65 * (1 - d / 3.5))
    return [[c] * cols for c in base]


def cmd_copper(a):
    cols, rows, _, _ = geom()
    stats = Stats("copper")
    t0 = time.monotonic()
    while a.seconds <= 0 or time.monotonic() - t0 < a.seconds:
        s = render(copper_rows(cols, rows * 2, time.monotonic() - t0))
        stats.add(s)
        out(s)
        wait(1 / 40)
    report(stats, a.report)


# ------------------------------------------------------------------ banner

def banner_strip(text, font):
    """The scroller's strip: per column, the lit pixel rows of the text's
    words set in font; each non-ASCII word (an emoji) is a two-column slot.
    Returns (columns, {slot start: emoji}, text height in pixels)."""
    colsets, emoji, height = [], {}, 2
    gap = 6

    def flush(words):
        nonlocal height
        if not words:
            return
        phrase = " ".join(words)
        bm = toilet_rows(phrase, font) or figlet_rows(phrase)
        if not bm:
            return
        height = max(height, len(bm))
        for x in range(len(bm[0])):
            colsets.append([y for y in range(len(bm)) if bm[y][x]])
        colsets.extend([] for _ in range(gap))

    words = []
    for word in text.split():
        if word.isascii():
            words.append(word)
            continue
        flush(words)
        words = []
        emoji[len(colsets)] = word
        colsets.extend([] for _ in range(2 + gap))
    flush(words)
    return colsets, emoji, height


def cmd_banner(a):
    """The demoscene scroller along the bottom rows: big letters and colour
    emoji riding a sine wave over copper bars. Drawn with the cursor saved
    and restored around each frame, so a player drawing above it (mpv) keeps
    its position. With --placeholder, the rows above show it instead of a
    video."""
    cols, rows, _, _ = geom()
    brows = min(a.rows, rows)
    h2 = brows * 2
    top = rows - brows + 1
    strip, emoji, fh = banner_strip(a.text, a.font)
    if not strip:
        return
    n = len(strip)
    amp = max(0, (h2 - fh) // 2 - 1)
    y0 = (h2 - fh) // 2
    stats = Stats("banner")
    if a.placeholder:
        # The panel stands where a 4:3 video would play above the scroller.
        vh = rows - brows - 2
        out(placeholder_screen(cols, rows - brows, a.placeholder, a.title, w=vh * 8 // 3, h=vh))
    t0 = time.monotonic()
    while a.seconds <= 0 or time.monotonic() - t0 < a.seconds:
        t = time.monotonic() - t0
        off = int(t * a.speed)
        px = copper_rows(cols, h2, t)
        overlay = {}
        for c in range(cols):
            si = (c + off) % n
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
        s = f"{ESC}7" + render(px, top=top, overlay=overlay) + f"{ESC}8"
        stats.add(s)
        out(s)
        if a.keys:
            wait(1 / 30)
        else:
            time.sleep(1 / 30)
    report(stats, a.report)


# ------------------------------------------------------------- placeholders

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
    t = f"  {text}  "
    r = top + h // 2
    c = left + max(0, (w - len(t)) // 2)
    s.append(f"{cup(r - 1, c)}{ESC}[0;40m{' ' * len(t)}{cup(r, c)}{ESC}[0;1;97;40m{t}{cup(r + 1, c)}{ESC}[0;40m{' ' * len(t)}{ESC}[0m")
    return "".join(s)


def cmd_placeholder(a):
    cols, rows, xp, yp = geom()
    w, h = a.cols or None, a.rows or None
    end = time.monotonic() + a.seconds
    phase = 0
    while time.monotonic() < end:
        out(placeholder_screen(cols, rows, a.text, a.title, w, h, phase))
        phase += 1
        wait(0.12)


# --------------------------------------------------------------------- cats

CATS = ["🐱", "🐈", "😺", "😸", "🙀", "🐈‍⬛", "😻", "😹", "😼", "😽", "😿", "😾"]


def cmd_cats(a):
    """A crowd of colour cat emoji popping in below row --top, faster and
    faster, then a few changing faces."""
    cols, rows, _, _ = geom()
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


# --------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("geom", help="print cols rows xpixel ypixel")

    p = sub.add_parser("pause", help="wait, reading q")
    p.add_argument("seconds", type=float)

    p = sub.add_parser("prep-outline", help="act 1's frames: ASCII outlines")
    p.add_argument("--video", required=True)
    p.add_argument("--start", type=float, default=0)
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--fps", type=int, default=12)
    p.add_argument("--rows", type=int, default=27)
    p.add_argument("--cols", type=int, default=0)
    p.add_argument("--cell-ratio", type=float, default=2.0)
    p.add_argument("--fill", default=" ")
    p.add_argument("--out", required=True)

    p = sub.add_parser("prep-caca", help="act 3's frames: libcaca's img2txt")
    p.add_argument("--video", required=True)
    p.add_argument("--start", type=float, default=30)
    p.add_argument("--seconds", type=float, default=8)
    p.add_argument("--fps", type=int, default=15)
    p.add_argument("--cols", type=int, default=80)
    p.add_argument("--rows", type=int, default=25)
    p.add_argument("--dither", default="none")
    p.add_argument("--out", required=True)

    p = sub.add_parser("play", help="play a frame cache, centred")
    p.add_argument("frames")
    p.add_argument("--box", default=None)
    p.add_argument("--caption", default="")
    p.add_argument("--broken", action="store_true")
    p.add_argument("--report", default="")

    p = sub.add_parser("type", help="type lines centred")
    p.add_argument("lines", nargs="+")
    p.add_argument("--row", type=int, default=0)
    p.add_argument("--cps", type=float, default=14)
    p.add_argument("--dots", type=float, default=0.9)
    p.add_argument("--hold", type=float, default=0)
    p.add_argument("--bar", action="store_true")

    p = sub.add_parser("big", help="act 2: big letters, then plasma")
    p.add_argument("text")
    p.add_argument("--font", default="mono12")
    p.add_argument("--letter", type=float, default=0.12)
    p.add_argument("--hold", type=float, default=1.5)
    p.add_argument("--plasma", type=float, default=8)
    p.add_argument("--fade", type=float, default=2)
    p.add_argument("--report", default="")

    for name, f in (("plasma", cmd_plasma), ("copper", cmd_copper)):
        p = sub.add_parser(name, help=f"{name} alone")
        p.add_argument("--seconds", type=float, default=10)
        p.add_argument("--report", default="")
        p.set_defaults(func=f)

    p = sub.add_parser("fire", help="the fire wipe")
    p.add_argument("--seconds", type=float, default=4)
    p.add_argument("--report", default="")

    p = sub.add_parser("starfield", help="stars behind a title")
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--title", default="")
    p.add_argument("--subtitle", default="")
    p.add_argument("--font", default="mono12")
    p.add_argument("--report", default="")

    p = sub.add_parser("banner", help="act 7's scroller over copper bars")
    p.add_argument("--text", required=True)
    p.add_argument("--rows", type=int, default=12)
    p.add_argument("--font", default="mono9")
    p.add_argument("--speed", type=float, default=60)
    p.add_argument("--seconds", type=float, default=0)
    p.add_argument("--placeholder", default="")
    p.add_argument("--title", default="")
    p.add_argument("--keys", action="store_true", help="read q from the terminal")
    p.add_argument("--report", default="")

    p = sub.add_parser("placeholder", help="a framed panel of static with text")
    p.add_argument("--text", required=True)
    p.add_argument("--title", default="")
    p.add_argument("--cols", type=int, default=0)
    p.add_argument("--rows", type=int, default=0)
    p.add_argument("--seconds", type=float, default=4)

    p = sub.add_parser("cats", help="feed the cats")
    p.add_argument("--top", type=int, default=6)
    p.add_argument("--seconds", type=float, default=8)
    p.add_argument("--density", type=float, default=0.6)

    a = ap.parse_args()
    funcs = {
        "geom": lambda a: print(*geom()),
        "pause": lambda a: wait(a.seconds),
        "prep-outline": cmd_prep_outline, "prep-caca": cmd_prep_caca, "play": cmd_play,
        "type": cmd_type, "big": cmd_big, "fire": cmd_fire, "starfield": cmd_starfield,
        "banner": cmd_banner, "placeholder": cmd_placeholder, "cats": cmd_cats,
    }
    func = getattr(a, "func", None) or funcs[a.cmd]

    saved = None
    if a.cmd not in ("geom", "prep-outline", "prep-caca") and os.isatty(0):
        try:
            saved = termios.tcgetattr(0)
            tty.setcbreak(0)
        except termios.error:
            saved = None
    try:
        func(a)
    except Quit:
        out(f"{ESC}[0m")
        return QUIT
    except KeyboardInterrupt:
        out(f"{ESC}[0m")
        return 130
    except BrokenPipeError:
        return 141
    finally:
        if saved is not None:
            termios.tcsetattr(0, termios.TCSADRAIN, saved)
    return 0


if __name__ == "__main__":
    sys.exit(main())
