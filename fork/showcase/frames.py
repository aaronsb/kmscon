"""The video acts' frame caches and their player (#75).

prep-outline (act 1's ASCII line art), prep-dots (act 1's Braille pass) and
prep-caca (act 3's libcaca frames) render a stretch of the operator's video
into a text file: a JSON header line, then each frame's rows. ffmpeg decodes
it and img2txt draws act 3; both are run as programs. play draws a cache
centred on the screen at its frame rate, sending only the rows that changed.
"""

import json
import os
import random
import subprocess
import sys
import tempfile
import time

from screen import ESC, Stats, box, cup, geom, out, placement, poll_keys, wait


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


def write_frames(path, meta, frames):
    tmp = path + ".part"
    with open(tmp, "w") as f:
        f.write(json.dumps(dict(meta, frames=len(frames))) + "\n")
        for fr in frames:
            for line in fr:
                f.write(line + "\n")
    os.replace(tmp, path)


def read_frames(path):
    with open(path) as f:
        meta = json.loads(f.readline())
        lines = f.read().split("\n")
    h = meta["rows"]
    return meta, [lines[i * h:(i + 1) * h] for i in range(meta["frames"])]


def grid_cols(video, rows, cell_ratio):
    """Columns that keep the video's shape on rows rows of cells cell_ratio
    times taller than wide."""
    vw, vh = video_size(video)
    return max(8, round(rows * vw / vh * cell_ratio))


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


def cmd_prep_outline(a):
    rows = a.rows
    cols = a.cols or grid_cols(a.video, rows, a.cell_ratio)
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


# Braille: a dot for each light sample, 2x4 samples a cell. Each of a cell's
# eight dots is its own bit, so a row of cells is built by OR-ing whole rows
# of samples, each translated to its dot's bit, as big integers: no bit
# carries between bytes.
_DOT_BIT = ((0x01, 0x08), (0x02, 0x10), (0x04, 0x20), (0x40, 0x80))
_CELL = [" "] + [chr(0x2800 + c) for c in range(1, 256)]


def cmd_prep_dots(a):
    rows = a.rows
    cols = a.cols or grid_cols(a.video, rows, a.cell_ratio)
    w, h = cols * 2, rows * 4
    lit = [[bytes(bit if v >= 128 else 0 for v in range(256)) for bit in row] for row in _DOT_BIT]
    frames = []
    for b in ffmpeg_gray(a.video, a.start, a.seconds, a.fps, w, h):
        fr = []
        for y in range(rows):
            acc = 0
            for dy in range(4):
                line = b[(4 * y + dy) * w:(4 * y + dy + 1) * w]
                for dx in range(2):
                    acc |= int.from_bytes(line[dx::2].translate(lit[dy][dx]), "big")
            fr.append("".join(_CELL[c] for c in acc.to_bytes(cols, "big")))
        frames.append(fr)
    if not frames:
        sys.exit(f"fx: no frames from {a.video} at {a.start}s")
    write_frames(a.out, {"fps": a.fps, "cols": cols, "rows": rows, "style": "dots"}, frames)


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
        for n in sorted(n for n in os.listdir(tmp) if n.endswith(".pgm")):
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

STYLE = {"outline": f"{ESC}[0;37m", "dots": f"{ESC}[0;97m", "caca": ""}


def cmd_play(a):
    """Plays a cache centred on the screen, in a frame titled --box when
    given. Late frames are dropped, to keep time with the soundtrack. With
    --broken the last frame then freezes, broken; --still draws only that."""
    meta, frames = read_frames(a.frames)
    cols, rows = geom()[:2]
    w, h = meta["cols"], meta["rows"]
    top, left = placement(cols, rows, w, h)
    colour = STYLE.get(meta.get("style"), "")
    if a.box is not None:
        out(box(top, left, w, h, a.box))
    if a.still:
        out("".join(cup(top + y, left) + colour + line + f"{ESC}[0m" for y, line in enumerate(frames[-1])))
        out(broken(top, left, w, h, frames[-1], a.box))
        return
    fps = meta["fps"]
    t0 = time.monotonic()
    stats = Stats("play")
    prev = [None] * h
    for i, fr in enumerate(frames):
        due = t0 + i / fps
        now = time.monotonic()
        if now > due + 1.0 / fps and i + 1 < len(frames):
            poll_keys()
            continue
        wait(due - now)
        s = "".join(cup(top + y, left) + colour + line + f"{ESC}[0m"
                    for y, line in enumerate(fr) if line != prev[y])
        prev = fr
        stats.add(s)
        out(s)
    wait(t0 + len(frames) / fps - time.monotonic())
    if a.broken:
        out(broken(top, left, w, h, frames[-1], a.box))
    stats.report(a.report)


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
        x = rnd.randrange(max(1, w - 12))
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
