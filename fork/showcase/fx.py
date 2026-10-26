#!/usr/bin/env python3
"""The drawing commands of fork/showcase/bad-apple.sh (#75).

Everything that draws more than a line of text runs through here: the video
acts' frame caches and their player (frames.py), act 7's kitty graphics
player (kitty.py), the effects (effects.py), the scenes of letters, typing,
placeholders and cats (scenes.py), all drawn through the painters of
screen.py. Standard library only; ffmpeg, img2txt and toilet are run as
programs.

Keys: q ends the show (exit status 10, which bad-apple.sh takes as "quit");
Ctrl-C exits 130. The terminal's mode is left as the caller set it, except
that a run on its own puts it in cbreak mode and restores it.

    fx.py COMMAND [OPTIONS]      (fx.py COMMAND --help for each)
"""

import argparse
import os
import sys
import termios
import tty

import effects
import frames
import kitty
import scenes
from screen import ESC, AnyKey, Quit, geom, out, wait, watch_resize

QUIT = 10


def parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def cmd(name, func, help):
        p = sub.add_parser(name, help=help)
        p.set_defaults(func=func)
        return p

    cmd("geom", lambda a: print(*geom()), "print cols rows xpixel ypixel")
    p = cmd("pause", lambda a: wait(a.seconds), "wait, reading q")
    p.add_argument("seconds", type=float)

    for name, func, help, fps, rows in (
        ("prep-outline", frames.cmd_prep_outline, "act 1's frames: ASCII outlines", 12, 27),
        ("prep-dots", frames.cmd_prep_dots, "act 1's Braille pass", 24, 52),
    ):
        p = cmd(name, func, help)
        p.add_argument("--video", required=True)
        p.add_argument("--start", type=float, default=0)
        p.add_argument("--seconds", type=float, default=20)
        p.add_argument("--fps", type=int, default=fps)
        p.add_argument("--rows", type=int, default=rows)
        p.add_argument("--cols", type=int, default=0)
        p.add_argument("--cell-ratio", type=float, default=2.0)
        p.add_argument("--fill", default=" ")
        p.add_argument("--out", required=True)

    p = cmd("prep-caca", frames.cmd_prep_caca, "act 3's frames: libcaca's img2txt")
    p.add_argument("--video", required=True)
    p.add_argument("--start", type=float, default=30)
    p.add_argument("--seconds", type=float, default=8)
    p.add_argument("--fps", type=int, default=15)
    p.add_argument("--cols", type=int, default=80)
    p.add_argument("--rows", type=int, default=25)
    p.add_argument("--dither", default="none")
    p.add_argument("--out", required=True)

    p = cmd("play", frames.cmd_play, "play a frame cache, centred")
    p.add_argument("frames")
    p.add_argument("--box", default=None)
    p.add_argument("--broken", action="store_true")
    p.add_argument("--still", action="store_true", help="draw the last frame, broken, and stop")
    p.add_argument("--report", default="")

    p = cmd("type", scenes.cmd_type, "type lines centred")
    p.add_argument("lines", nargs="+")
    p.add_argument("--row", type=int, default=0)
    p.add_argument("--cps", type=float, default=14)
    p.add_argument("--dots", type=float, default=0.9)
    p.add_argument("--hold", type=float, default=0)
    p.add_argument("--bar", action="store_true")

    p = cmd("big", scenes.cmd_big, "act 2: big letters, then plasma and shadebobs")
    p.add_argument("text")
    p.add_argument("--font", default="mono12")
    p.add_argument("--pause", type=float, default=0)
    p.add_argument("--letter", type=float, default=0.14)
    p.add_argument("--hold", type=float, default=1.5)
    p.add_argument("--plasma", type=float, default=6)
    p.add_argument("--fade", type=float, default=2)
    p.add_argument("--bobs", type=float, default=5)
    p.add_argument("--fps", type=float, default=60)
    p.add_argument("--report", default="")

    p = cmd("effect", effects.cmd_effect, "one effect alone")
    p.add_argument("name", choices=sorted(list(effects.EFFECTS) + list(effects.DOT_EFFECTS) + ["starfield"]))
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--title", default="")
    p.add_argument("--subtitle", default="")
    p.add_argument("--font", default="mono12")
    p.add_argument("--cjk", type=float, default=0)
    p.add_argument("--report", default="")

    p = cmd("banner", scenes.cmd_banner, "act 7's scroller over copper bars")
    p.add_argument("--text", required=True)
    p.add_argument("--rows", type=int, default=12)
    p.add_argument("--font", default="mono9")
    p.add_argument("--fps", type=float, default=40, help="frames, and so columns, a second")
    p.add_argument("--seconds", type=float, default=0)
    p.add_argument("--placeholder", default="")
    p.add_argument("--title", default="")
    p.add_argument("--report", default="")

    p = cmd("kitty", kitty.cmd_kitty, "act 7's video through kitty graphics, up to three copies")
    p.add_argument("--video", required=True)
    p.add_argument("--start", type=float, default=0)
    p.add_argument("--seconds", type=float, default=0, help="0: to the end")
    p.add_argument("--transport", default="png", choices=kitty.TRANSPORTS)
    p.add_argument("--fallback", default="png,raw", help="transports to try when the first is refused")
    p.add_argument("--width", type=int, default=0, help="frames are decoded this many pixels wide (0: a copy's own size, at most 2160 tall)")
    p.add_argument("--level", type=int, default=1, help="zlib's level for zlib and png")
    p.add_argument("--cell-w", type=int, default=10)
    p.add_argument("--cell-h", type=int, default=20)
    p.add_argument("--banner-rows", type=int, default=0, help="rows left free at the bottom")
    p.add_argument("--gap", type=int, default=2, help="columns between copies")
    p.add_argument("--slide-at", type=float, default=-1, help="seconds into the act copy 1 slides left")
    p.add_argument("--copy2-at", type=float, default=-1)
    p.add_argument("--copy3-at", type=float, default=-1)
    p.add_argument("--move", type=float, default=1.2, help="seconds each slide takes")
    p.add_argument("--bounce", type=float, default=0.12, help="overshoot, a share of the distance")
    p.add_argument("--copies", type=int, default=0, help="hold this many copies still (measuring)")
    p.add_argument("--audio-ipc", default="", help="mpv's IPC socket: follow its time-pos")
    p.add_argument("--audio-wait", type=float, default=3.0, help="seconds to wait for mpv's first answer")
    p.add_argument("--audio-started", type=float, default=None,
                   help="when the soundtrack started (seconds since the epoch): the clock until, or if never, mpv answers")
    p.add_argument("--unpaced", action="store_true", help="every frame, as fast as the terminal takes them")
    p.add_argument("--probe-timeout", type=float, default=1.0)
    p.add_argument("--report", default="")

    p = cmd("placeholder", scenes.cmd_placeholder, "a framed panel of static with text")
    p.add_argument("--text", required=True)
    p.add_argument("--title", default="")
    p.add_argument("--cols", type=int, default=0)
    p.add_argument("--rows", type=int, default=0)
    p.add_argument("--seconds", type=float, default=4)

    p = cmd("cats", scenes.cmd_cats, "feed the cats")
    p.add_argument("--top", type=int, default=6)
    p.add_argument("--seconds", type=float, default=8)
    p.add_argument("--density", type=float, default=0.6)

    p = cmd("endcard", scenes.cmd_endcard, "the credits over a Braille wireframe, until a key")
    p.add_argument("--title", required=True)
    p.add_argument("--text", default="")
    return ap


def main():
    a = parser().parse_args()
    saved = None
    if os.isatty(1):
        watch_resize()
    if a.cmd not in ("geom", "prep-outline", "prep-dots", "prep-caca") and os.isatty(0):
        try:
            saved = termios.tcgetattr(0)
            tty.setcbreak(0)
        except termios.error:
            saved = None
    try:
        a.func(a)
    except AnyKey:
        return 0
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
