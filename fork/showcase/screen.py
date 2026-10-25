"""The terminal side of the showcase's drawing (#75): output, keys, the
window size, colours, and the two painters every effect draws through.

Each frame is one os.write(), so a frame never interleaves with another
process writing to the same terminal (mpv, in act 7): the kernel holds the
tty's write lock for the whole call. The painters keep the last frame and
send only the cells that changed, with a cursor move only where they skip,
and SGR only where the colour changes.
"""

import colorsys
import fcntl
import os
import select
import shutil
import signal
import struct
import termios
import time

ESC = "\x1b"
DEFAULT = -1  # the terminal's own background


class Quit(Exception):
    """q was pressed: the show ends."""


class AnyKey(Exception):
    """A key was pressed where any key ends the scene."""


# ---------------------------------------------------------------- terminal

def out(s):
    """Write s in one call: one frame, one write(). SIGWINCH is held off
    while it writes, so a resize cannot cut the write short and let another
    process's frame land inside this one; the loop is for what remains."""
    b = s.encode()
    held = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGWINCH})
    try:
        while b:
            n = os.write(1, b)
            b = b[n:]
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, held)


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


_resized = False


def _on_winch(_sig, _frame):
    global _resized
    _resized = True


def watch_resize():
    signal.signal(signal.SIGWINCH, _on_winch)


def resized():
    """True once after each SIGWINCH: the caller reads geom() again."""
    global _resized
    r, _resized = _resized, False
    return r


_any_key = False


def any_key_ends(on=True):
    """From now on any key raises AnyKey (q still raises Quit)."""
    global _any_key
    _any_key = on


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
            if _any_key:
                raise AnyKey
    except (OSError, InterruptedError):
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
            try:
                if select.select([0], [], [], left)[0]:
                    poll_keys()
            except InterruptedError:
                pass
        else:
            time.sleep(left)


def cup(row, col):
    return f"{ESC}[{row};{col}H"


def clear():
    out(f"{ESC}[0m{ESC}[2J")


# ------------------------------------------------------------------ colour

def hsv(h, s=1.0, v=1.0):
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, s, v)
    return (int(r * 255) << 16) | (int(g * 255) << 8) | int(b * 255)


def scale(c, f):
    return ((min(255, int((c >> 16) * f)) << 16) | (min(255, int(((c >> 8) & 255) * f)) << 8)
            | min(255, int((c & 255) * f)))


def gradient(stops, n):
    """n colours through stops [(position 0..1, 0xRRGGBB), ...]."""
    pal = []
    for i in range(n):
        p = i / max(1, n - 1)
        for (a0, c0), (a1, c1) in zip(stops, stops[1:]):
            if a0 <= p <= a1:
                f = (p - a0) / max(1e-9, a1 - a0)
                pal.append(sum(int(((c0 >> s) & 255) * (1 - f) + ((c1 >> s) & 255) * f) << s for s in (16, 8, 0)))
                break
    return pal


_fg_codes = {}
_bg_codes = {}


def fg_code(c):
    s = _fg_codes.get(c)
    if s is None:
        s = _fg_codes[c] = "39" if c == DEFAULT else f"38;2;{c >> 16};{(c >> 8) & 255};{c & 255}"
    return s


def bg_code(c):
    s = _bg_codes.get(c)
    if s is None:
        s = _bg_codes[c] = "49" if c == DEFAULT else f"48;2;{c >> 16};{(c >> 8) & 255};{c & 255}"
    return s


# --------------------------------------------------------------- half blocks

_STALE = -2  # a cell the painter did not draw, or drew something unknown into


class Painter:
    """Draws pixel rows (two per cell row, colours as 0xRRGGBB or DEFAULT)
    with ▀/▄ from cell (top, left), sending only the cells that changed since
    its last frame. reset() forgets that frame (after a clear or a resize)."""

    def __init__(self, top=1, left=1):
        self.top = top
        self.left = left
        self.prev = None

    def reset(self):
        self.prev = None

    def draw(self, px, mask=None, overlay=None):
        """The escape string for this frame. mask[r][x], when given, says which
        cells may be drawn; the rest keep what the terminal shows. overlay[r]
        maps a column to a two-cell glyph (an emoji) drawn on that cell's top
        colour; a row with an overlay is drawn whole."""
        w = len(px[0])
        nrows = len(px) // 2
        if self.prev is None or len(self.prev) != nrows or len(self.prev[0][0]) != w:
            self.prev = [([_STALE] * w, [_STALE] * w) for _ in range(nrows)]
        parts = []
        fg = bg = None
        top, left = self.top, self.left
        for r in range(nrows):
            up = px[2 * r]
            lo = px[2 * r + 1]
            pu, pl = self.prev[r]
            orow = overlay.get(r) if overlay else None
            mrow = mask[r] if mask is not None else None
            if orow is None and mrow is None and up == pu and lo == pl:
                continue
            x = 0
            at = -1  # the column the cursor is at, -1: unknown
            while x < w:
                if orow is None and (mrow is not None and not mrow[x] or up[x] == pu[x] and lo[x] == pl[x]):
                    if mrow is not None and not mrow[x]:
                        pu[x] = pl[x] = _STALE
                    x += 1
                    continue
                if at != x:
                    parts.append(cup(top + r, left + x))
                if orow and x in orow and x + 1 < w:
                    c = up[x]
                    parts.append(f"{ESC}[0;{bg_code(c)}m{orow[x]}")
                    fg, bg = None, c
                    pu[x] = pl[x] = pu[x + 1] = pl[x + 1] = _STALE
                    x += 2
                    at = x
                    continue
                t = up[x]
                b = lo[x]
                pu[x], pl[x] = t, b
                codes = []
                if t == b:
                    if fg == t and t != DEFAULT:
                        ch = "█"
                    else:
                        ch = " "
                        if bg != t:
                            codes.append(bg_code(t))
                            bg = t
                else:
                    if t == DEFAULT:
                        ch, f, g = "▄", b, DEFAULT
                    elif b == DEFAULT:
                        ch, f, g = "▀", t, DEFAULT
                    elif (fg != b) + (bg != t) < (fg != t) + (bg != b):
                        ch, f, g = "▄", b, t
                    else:
                        ch, f, g = "▀", t, b
                    if fg != f:
                        codes.append(fg_code(f))
                        fg = f
                    if bg != g:
                        codes.append(bg_code(g))
                        bg = g
                parts.append(f"{ESC}[{';'.join(codes)}m{ch}" if codes else ch)
                x += 1
                at = x
        if parts:
            parts.append(f"{ESC}[0m")
        return "".join(parts)


def render(px, top=1, left=1, mask=None, overlay=None):
    """One frame drawn whole: a Painter with no previous frame."""
    return Painter(top, left).draw(px, mask, overlay)


# ----------------------------------------------------------------- Braille

# The bit of each dot of a Braille cell (U+2800 + bits), by (y, x) in the cell.
BRAILLE_BIT = ((0x01, 0x08), (0x02, 0x10), (0x04, 0x20), (0x40, 0x80))


class Dots:
    """A cols x rows grid of Braille cells: 2 x 4 dots each, one colour per
    cell (the colour of the last dot set in it)."""

    def __init__(self, cols, rows):
        self.cols = cols
        self.rows = rows
        self.w = cols * 2
        self.h = rows * 4
        self.code = [0] * (cols * rows)
        self.colour = [DEFAULT] * (cols * rows)

    def dot(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y >> 2) * self.cols + (x >> 1)
            self.code[i] |= BRAILLE_BIT[y & 3][x & 1]
            self.colour[i] = c

    def line(self, x0, y0, x1, y1, c):
        x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        for _ in range(dx - dy + 1):
            self.dot(x0, y0, c)
            if x0 == x1 and y0 == y1:
                return
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy


class BraillePainter:
    """Draws a Dots grid from cell (top, left), sending only the rows that
    changed. Rows in skip are left alone (text drawn over the grid)."""

    def __init__(self, top=1, left=1):
        self.top = top
        self.left = left
        self.prev = {}

    def reset(self):
        self.prev = {}

    def draw(self, d, skip=()):
        parts = []
        cols = d.cols
        for r in range(d.rows):
            if r in skip:
                continue
            codes = d.code[r * cols:(r + 1) * cols]
            colours = d.colour[r * cols:(r + 1) * cols]
            key = (codes, colours)
            if self.prev.get(r) == key:
                continue
            self.prev[r] = key
            line = [cup(self.top + r, self.left)]
            fg = None
            for code, c in zip(codes, colours):
                if code:
                    if c != fg:
                        line.append(f"{ESC}[{fg_code(c)}m")
                        fg = c
                    line.append(chr(0x2800 + code))
                else:
                    line.append(" ")
            parts.append("".join(line))
        if parts:
            parts.append(f"{ESC}[0m")
        return "".join(parts)


# ------------------------------------------------------------------- stats

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

    def report(self, path):
        if path:
            with open(path, "a") as f:
                f.write(self.line() + "\n")


def frames(seconds, fps):
    """Yields (frame number, seconds since the start) at fps until seconds
    have passed (forever when seconds <= 0), reading keys between frames;
    fps 0 runs as fast as the terminal takes it."""
    t0 = time.monotonic()
    n = 0
    while True:
        t = time.monotonic() - t0
        if seconds > 0 and t >= seconds:
            return
        yield n, t
        n += 1
        if fps > 0:
            wait(t0 + n / fps - time.monotonic())
        else:
            poll_keys()


# ------------------------------------------------------------------ frames

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
    """The top-left cell of a w x h region centred on the screen."""
    return max(1, (rows - h) // 2 + 1), max(1, (cols - w) // 2 + 1)
