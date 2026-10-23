"""Big letters as pixels (#75): toilet's block fonts (drawn with ▀ ▄ █) read
back into half-block pixel rows, so letters can be scaled, coloured and laid
over an effect, with figlet's banner font as the fallback. toilet and figlet
are run as programs; without either, the callers fall back to plain text.
"""

import shutil
import subprocess

_TOP = {"▀": 1, "█": 1, "▄": 0, " ": 0}
_BOTTOM = {"▀": 0, "█": 1, "▄": 1, " ": 0}


def _run(cmd):
    if not shutil.which(cmd[0]):
        return None
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.returncode != 0:
        return None
    lines = p.stdout.rstrip("\n").split("\n")
    w = max((len(l) for l in lines), default=0)
    return [l.ljust(w) for l in lines]


def trim_rows(rows):
    while rows and not any(rows[0]):
        rows.pop(0)
    while rows and not any(rows[-1]):
        rows.pop()
    return rows


def toilet_rows(text, font, trim=True):
    """Pixel rows (lists of 0/1) of text set in a toilet font that draws with
    ▀ ▄ █, two per line of toilet's output; None when toilet or the font is
    missing. Untrimmed, every glyph of a font has the same height."""
    lines = _run(["toilet", "-W", "-w", "4000", "-f", font, "--", text])
    if lines is None:
        return None
    rows = []
    for l in lines:
        rows.append([_TOP.get(ch, 1) for ch in l])
        rows.append([_BOTTOM.get(ch, 1) for ch in l])
    return trim_rows(rows) if trim else rows


def figlet_rows(text):
    """Fallback: figlet's banner font, each mark a full cell (two pixels)."""
    lines = _run(["figlet", "-f", "banner", "-w", "4000", "--", text])
    if lines is None:
        return None
    rows = []
    for l in lines:
        r = [0 if ch == " " else 1 for ch in l]
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


class TextMask:
    """The lit pixels of text centred on a cols x rows2 pixel canvas, as
    (x, y, letter index), set in font or, where that is too wide, in mono9 or
    figlet's banner font, scaled up to max_scale. ok is False when no font
    fits or none is installed."""

    def __init__(self, text, font, cols, rows2, max_scale=4):
        self.ok = False
        self.pixels = []
        self.spans = None
        for f in dict.fromkeys((font, "mono9", None)):
            if f is None:
                bm, spans = figlet_rows(text), None
            else:
                bm, spans = glyph_strip(text, f)
            if bm and len(bm[0]) <= cols - 2 and len(bm) <= rows2 - 2:
                break
        else:
            return
        w, h = len(bm[0]), len(bm)
        k = max(1, min(max_scale, (cols - 4) // w, (rows2 - 4) // h))
        self.ok = True
        self.spans = spans
        self.x0 = (cols - w * k) // 2
        self.y0 = (rows2 - h * k) // 2
        self.width = w * k
        letter_of = [0] * w
        for li, (s0, s1) in enumerate(spans or ()):
            for x in range(s0, min(s1, w)):
                letter_of[x] = li
        for y in range(h):
            for x in range(w):
                if bm[y][x]:
                    for dy in range(k):
                        for dx in range(k):
                            self.pixels.append((self.x0 + x * k + dx, self.y0 + y * k + dy, letter_of[x]))


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
