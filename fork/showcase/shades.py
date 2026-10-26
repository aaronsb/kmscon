"""The interlude between act 6's cats and act 7 (#75): a row of big cat
faces in kitty graphics, pixel sunglasses dropping onto them one a beat
from left to right, then the row sliding off the left edge.

The faces are Noto Emoji's vector cats and the sunglasses a pixel-art SVG
(fork/showcase/cats/), rendered by rsvg-convert at the size the screen
asks for, a quarter of its height at most, into the cache, where the next
run finds them. Each is placed at its own pixel size, so nothing is scaled
by the terminal; the sunglasses sit above the faces (z) and fall by being
placed again lower each frame, at a pixel offset within the cell (X, Y);
leaving the screen, an image is placed at the left edge cropped (x, w).
Without kitty graphics, or when the images cannot be rendered or cached,
the faces are emoji and the sunglasses ⌐■-■.

Times are in beats of the interlude's music cue (music/act6-shades.js):
the faces pop in on the first six eighths, the sunglasses land on beats 4
to 9, falling for three quarters of a beat, and the row slides off over
beats 12 to 16.
"""

import os
import signal
import subprocess
import sys

from kitty import apc, b64, chunked, hang_up
from screen import ESC, cup, frames, geom, out, resized

CATS = ["1f431", "1f63a", "1f638", "1f63c", "1f63b", "1f63d"]
EMOJI = ["🐱", "😺", "😸", "😼", "😻", "😽"]
CAT_ID = 101               # image ids 101 to 106; the sunglasses 110, placed once per cat
SHADES_ID = 110
POP, LAND, FALL, SLIDE, END = 0.5, 4, 0.75, 12, 16

# Where the sunglasses go on a face, as shares of its size: their SVG is 39
# units wide and 7 tall (a unit of margin round it), its lenses in rows 1
# to 4 with their centres at x 6 and 20. The eyes sit at 0.375 and 0.62 of
# a face's width, from 0.45 to 0.58 of its height; a unit of 0.028 of the
# face makes the sunglasses oversized, wider than the face with the arm
# out past it, each lens over its eye, centred between them, from 0.45
# down.
UNIT = 0.028
SHADES_W, SHADES_H = 39 * UNIT, 7 * UNIT
SHADES_X, SHADES_Y = (0.375 + 0.62) / 2 - 14 * UNIT, 0.45 - 2 * UNIT


def render(svg, png, w, h):
    """svg rendered to png at w x h pixels, unless it already is; a failed
    render leaves no .part behind."""
    if not os.path.exists(png):
        os.makedirs(os.path.dirname(png), exist_ok=True)
        try:
            subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h), "-o", png + ".part", svg], check=True)
            os.replace(png + ".part", png)
        except BaseException:
            try:
                os.unlink(png + ".part")
            except OSError:
                pass
            raise
    with open(png, "rb") as f:
        return f.read()


class Row:
    """The row's layout on a cols x rows screen of cell_w x cell_h cells:
    face size s pixels, and each face's top-left pixel."""

    def __init__(self, cols, rows, cell_w, cell_h, s=0):
        self.cell_w, self.cell_h = cell_w, cell_h
        n = len(CATS)
        self.s = s or max(2 * cell_h, min(rows * cell_h // 4, cols * cell_w // (n + 1)))
        wc = -(-self.s // cell_w)
        gap = max(0, (cols - n * wc) // (n + 1))
        top = max(0, (rows - -(-self.s // cell_h)) // 2)
        self.faces = [((gap + i * (wc + gap)) * cell_w, top * cell_h) for i in range(n)]
        self.shades = (round(self.s * SHADES_W), round(self.s * SHADES_H))
        self.gone = set()  # the placements deleted past the left edge

    def glasses(self, i):
        """Where the sunglasses rest on face i, in pixels."""
        x, y = self.faces[i]
        return x + round(self.s * SHADES_X), y + round(self.s * SHADES_Y)

    def place(self, image_id, pid, x, y, z, width):
        """Places image_id (pid) with its top-left at pixel x, y; past the
        left edge, cropped to what is left of it, or deleted once gone,
        and then nothing more."""
        if x + width <= 0:
            if (image_id, pid) in self.gone:
                return ""
            self.gone.add((image_id, pid))
            return apc(f"a=d,d=i,i={image_id},p={pid},q=2")
        crop = f",x={-x},w={width + x}" if x < 0 else ""
        x = max(0, x)
        return (cup(y // self.cell_h + 1, x // self.cell_w + 1)
                + apc(f"a=p,i={image_id},p={pid},X={x % self.cell_w},Y={y % self.cell_h},z={z}{crop},C=1,q=2"))


def slide(row, beat):
    """How far left the row has slid at beat: from nothing at SLIDE to past
    the right-most face at END, faster and faster."""
    u = min(1.0, max(0.0, (beat - SLIDE) / (END - SLIDE)))
    return int(u * u * (row.faces[-1][0] + row.s + 1))


def drop(row, i, beat):
    """The sunglasses' height over face i at beat, falling from the top of
    the screen with gravity to land on beat LAND + i; None before they
    start falling."""
    start = LAND + i - FALL
    if beat < start:
        return None
    rest = row.glasses(i)[1]
    u = min(1.0, (beat - start) / FALL)
    return int(rest * u * u)


def images(row, cache):
    """The faces and the sunglasses as PNGs at row's sizes, rendered into
    cache the first time."""
    sw, sh = row.shades
    here = os.path.dirname(os.path.abspath(__file__))
    cache = os.path.join(cache, f"cats-{row.s}")
    faces = [render(os.path.join(here, "cats", f"emoji_u{c}.svg"), os.path.join(cache, f"{c}.png"), row.s, row.s)
             for c in CATS]
    shades = render(os.path.join(here, "cats", "sunglasses.svg"), os.path.join(cache, f"shades-{sw}x{sh}.png"), sw, sh)
    return faces, shades


def run_kitty(a, beat_s, row, faces, shades):
    sent = [chunked(f"a=t,f=100,i={CAT_ID + i},q=2", b64(png)) for i, png in enumerate(faces)]
    out("".join(sent) + chunked(f"a=t,f=100,i={SHADES_ID},q=2", b64(shades)))
    sw = row.shades[0]
    shown = set()
    try:
        for _n, t in frames(END * beat_s, 60):
            if resized():
                cols, rows = geom()[:2]
                row = Row(cols, rows, a.cell_w, a.cell_h, row.s)
                shown.clear()
            beat = t / beat_s
            dx = slide(row, beat)
            s = []
            for i, (x, y) in enumerate(row.faces):
                if beat >= i * POP and (i not in shown or dx):
                    s.append(row.place(CAT_ID + i, 1, x - dx, y, 1, row.s))
                    shown.add(i)
                height = drop(row, i, beat)
                if height is not None and (beat < LAND + i + 0.1 or dx):
                    s.append(row.place(SHADES_ID, i + 1, row.glasses(i)[0] - dx, height, 2, sw))
            if any(s):
                out("".join(s))
    finally:
        out("".join(apc(f"a=d,d=I,i={image_id},q=2") for image_id in list(range(CAT_ID, CAT_ID + len(CATS))) + [SHADES_ID]))


def run_text(beat_s):
    """The same timeline in text: the faces as emoji, the sunglasses ⌐■-■
    dropping onto the row above each, then the row cleared from the left."""
    cols, rows = geom()[:2]
    n = len(EMOJI)
    gap = max(2, (cols - 2 * n) // (n + 1))
    xs = [1 + gap + i * (gap + 2) for i in range(n)]
    face_row = rows // 2 + 1
    at = [None] * n
    for _n, t in frames(END * beat_s, 40):
        beat = t / beat_s
        s = []
        for i in range(n):
            if beat >= i * POP:
                s.append(f"{cup(face_row, xs[i])}{EMOJI[i]}")
            start = LAND + i - FALL
            if beat >= start:
                r = 1 + int((face_row - 2) * min(1.0, (beat - start) / FALL) ** 2)
                if r != at[i]:
                    if at[i]:
                        s.append(f"{cup(at[i], xs[i] - 1)}    ")
                    s.append(f"{cup(r, xs[i] - 1)}{ESC}[0;1;97m⌐■-■{ESC}[0m")
                    at[i] = r
        cleared = int(min(1.0, max(0.0, (beat - SLIDE) / (END - SLIDE))) * cols)
        if cleared:
            s.append(f"{cup(face_row - 1, 1)}{' ' * cleared}{cup(face_row, 1)}{' ' * cleared}")
        if s:
            out("".join(s))


def cmd_shades(a):
    """The interlude: the cats in sunglasses, in kitty graphics or text.
    Images that cannot be rendered or cached (no rsvg-convert, a read-only
    or full cache) fall back to text, so the show goes on to act 7. TERM
    and HUP end it as Ctrl-C does, through its cleanup."""
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, hang_up)
    beat_s = 60.0 / a.bpm
    if a.kitty:
        cols, rows = geom()[:2]
        row = Row(cols, rows, a.cell_w, a.cell_h)
        try:
            faces, shades = images(row, a.cache)
        except (OSError, subprocess.CalledProcessError) as e:
            print(f"shades: no cat images ({e}); emoji instead", file=sys.stderr, flush=True)
        else:
            run_kitty(a, beat_s, row, faces, shades)
            return
    run_text(beat_s)
