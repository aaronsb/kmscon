#!/usr/bin/env python3
"""Checks fork/showcase/bad-apple.sh without a video, a terminal or any of
the media tools: its act and effect tables (--list), its plan (--dry-run),
its refusals, that every act listed has its function, and the pure helpers
the acts draw with (screen.py, frames.py, effects.py and act 5's sizing in
bad-apple.sh), and, on a pty, that quitting turns off every mode a player
may have left on (#88).

    showcase.py --root SOURCE_ROOT
"""

import argparse
import os
import pty
import re
import select
import subprocess
import sys
import tempfile
import time

EFFECTS = ["plasma", "shadebobs", "fire", "copper", "stars", "braille", "banner"]

ACTS = [
    ("1", "stick figures"),
    ("2", "we can do better"),
    ("3", "aalib"),
    ("4", "don't we have libsixel?"),
    ("5", "sixel"),
    ("6", "kitty and the cats"),
    ("7", "kitty, full size"),
]

failures = []


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name)
    if not ok:
        failures.append(name)
        if detail:
            print("      " + detail.replace("\n", "\n      "))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = os.path.abspath(ap.parse_args().root)
    script = os.path.join(root, "fork/showcase/bad-apple.sh")

    with tempfile.TemporaryDirectory() as tmp:
        env = {k: v for k, v in os.environ.items() if k != "BAD_APPLE" and not k.startswith("ACT")}
        env["SHOWCASE_CACHE"] = os.path.join(tmp, "cache")

        def run(*args, **extra):
            e = dict(env, **extra)
            return subprocess.run(["bash", script, *args], env=e, capture_output=True, text=True,
                                  timeout=30, stdin=subprocess.DEVNULL, cwd=tmp)

        # --list: one row per act, in order, with what it needs and how it
        # plays on kmscon today.
        p = run("--list")
        blocks = p.stdout.split("\n\n")
        rows = [l for l in blocks[0].splitlines()[1:] if l.strip()]
        got = [(l[:4].strip(), l[4:29].strip()) for l in rows]
        check("--list prints the seven acts in order", p.returncode == 0 and got == ACTS,
              f"{got!r}\n{p.stderr}")
        needs = {l[:4].strip(): l[29:60].split() for l in rows}
        check("each act lists what it needs", all(needs.get(n) for n, _ in ACTS), repr(needs))
        check("act 7 is a placeholder on kmscon today, the rest play",
              all(("placeholder" in l) == (l[:4].strip() == "7") for l in rows), p.stdout)

        effects = [l.split()[0] for l in blocks[1].splitlines()[1:] if l.strip()] if len(blocks) > 1 else []
        check("--list names the effects --fx runs", effects == EFFECTS, repr(effects))

        # Every act in the table has its function.
        fns = subprocess.run(["bash", "-c", 'source "$1"; for n in 1 2 3 4 5 6 7; do declare -F "act_$n" >/dev/null || echo "missing act_$n"; done; echo "$LAST_ACT"', "sh", script],
                             env=env, capture_output=True, text=True, timeout=30)
        check("act_1 to act_7 are defined and LAST_ACT is 7",
              fns.returncode == 0 and fns.stdout.strip() == "7", fns.stdout + fns.stderr)

        # --dry-run: no video, no terminal, no tools needed; each act with
        # what it does and the tools it would use.
        p = run("--dry-run")
        acts = [l for l in p.stdout.splitlines() if l.startswith("act ")]
        check("--dry-run runs without BAD_APPLE and plans every act",
              p.returncode == 0 and acts == [f"act {n}: {name}" for n, name in ACTS], p.stdout + p.stderr)
        check("--dry-run says BAD_APPLE is unset", "BAD_APPLE is unset" in p.stdout, p.stdout)
        uses = [re.sub(r"\([^)]*\)", "", l.split(":", 1)[1]).split()
                for l in p.stdout.splitlines() if l.strip().startswith("uses:")]
        check("--dry-run's tools per act are --list's", uses == [needs[n] for n, _ in ACTS], p.stdout)
        check("--dry-run touches no terminal", "\x1b" not in p.stdout, repr(p.stdout[:200]))

        p = run("--dry-run", "--from", "5")
        acts = [l for l in p.stdout.splitlines() if l.startswith("act ")]
        check("--from 5 plans acts 5 to 7", p.returncode == 0 and acts == ["act 5: sixel", "act 6: kitty and the cats", "act 7: kitty, full size"],
              p.stdout + p.stderr)
        p = run("--dry-run", "--act=3")
        acts = [l for l in p.stdout.splitlines() if l.startswith("act ")]
        check("--act 3 plans act 3 alone", p.returncode == 0 and acts == ["act 3: aalib"], p.stdout + p.stderr)

        video = os.path.join(tmp, "bad apple.mp4")
        open(video, "w").close()
        p = run("--dry-run", BAD_APPLE=video, ACT1_SECONDS="12", ACT3_START="41")
        check("timings come from the environment",
              "video: 0s for 12s" in p.stdout and "video: 41s for 8s" in p.stdout, p.stdout)
        check("--dry-run names the video", f"video: BAD_APPLE={video}" in p.stdout, p.stdout)

        # Refusals, before anything touches the terminal.
        for args in (["--act", "0"], ["--act", "8"], ["--from", "x"], ["--act=07"]):
            p = run(*args)
            check(f"{' '.join(args)} is refused", p.returncode != 0 and "no act" in p.stderr, p.stderr)
        p = run("--frobnicate")
        check("an unknown option is refused", p.returncode == 2, p.stderr)

        p = run("--act", "1")
        check("a video act without BAD_APPLE fails, saying so",
              p.returncode != 0 and "BAD_APPLE is unset" in p.stderr and "never downloaded" in p.stderr
              and "\x1b" not in p.stdout, p.stdout + p.stderr)
        p = run("--act", "3", BAD_APPLE=os.path.join(tmp, "nothing.mp4"))
        check("a missing video fails, naming it", p.returncode != 0 and "nothing.mp4 is not a readable file" in p.stderr,
              p.stderr)
        p = run("--act", "7", "--banner-only")
        check("act 7 with --banner-only needs no video", p.returncode != 0 and "BAD_APPLE" not in p.stderr
              and ("needs a terminal" in p.stderr or "missing:" in p.stderr), p.stderr)
        p = run("--fx", "lava")
        check("an unknown effect is refused, naming the effects",
              p.returncode != 0 and "no effect 'lava'" in p.stderr and all(e in p.stderr for e in EFFECTS), p.stderr)
        p = run("--dry-run")
        check("--dry-run lists the effects",
              all(f"  {e}: " in p.stdout for e in EFFECTS), p.stdout)
        p = run("--prepare")
        check("--prepare without BAD_APPLE fails", p.returncode != 0 and "BAD_APPLE is unset" in p.stderr, p.stderr)
        # Act 2 needs no video: it gets past that check and stops only for
        # want of a terminal (or of a tool this machine lacks).
        p = run("--act", "2")
        check("act 2 needs no video", p.returncode != 0 and "BAD_APPLE" not in p.stderr
              and ("needs a terminal" in p.stderr or "missing:" in p.stderr), p.stderr)

        # Act 5's picture: half the rows tall, centred in whole cells, its
        # height a multiple of 6; narrower where the columns run out.
        def shell(code, **extra):
            return subprocess.run(["bash", "-c", 'source "$1"; ' + code, "sh", script], env=dict(env, **extra),
                                  capture_output=True, text=True, timeout=30)
        for (cols, rows, cw, ch), want in (((404, 56, 19, 38), "1419 28 165 15"),
                                           ((136, 40, 10, 19), "505 20 43 11"),
                                           ((40, 40, 10, 20), "377 15 2 13")):
            p = shell(f"screen_cols={cols} screen_rows={rows} cell_w={cw} cell_h={ch}; act5_box 960 718")
            check(f"act 5's picture of a 960x718 clip at {cols}x{rows} cells of {cw}x{ch} is {want}",
                  p.stdout.strip() == want, p.stdout + p.stderr)
        # The cell size from XTWINOPS 16, else 14 over the cells.
        for reply, want in (("\x1b[4;2128;7676t\x1b[6;38;19t\x1b[?62;4c", "19 38"),
                            ("\x1b[4;2128;7676t\x1b[?62;4c", "19 38"), ("\x1b[?62;4c", "0 0")):
            p = shell('screen_cols=404 screen_rows=56 cell_w=0 cell_h=0; winops_cell "$REPLY"; echo $cell_w $cell_h',
                      REPLY=reply)
            check(f"XTWINOPS answers {reply.replace(chr(27), 'ESC')} give the cell {want}",
                  p.stdout.strip() == want, p.stdout + p.stderr)

        # Quitting turns off mouse tracking and the other report modes a
        # killed mpv leaves on (#88). Act 4 needs python3 alone.
        pid, fd = pty.fork()
        if pid == 0:
            os.execve("/bin/bash", ["bash", script, "--act", "4", "--no-wait"], env)
        got, start, sent = b"", time.time(), False
        while time.time() - start < 20:
            if not sent and b"wait" in got:
                os.write(fd, b"q")
                sent = True
            r, _, _ = select.select([fd], [], [], 0.1)
            try:
                d = os.read(fd, 65536) if r else b""
            except OSError:
                break
            if r and not d:
                break
            got += d
        _, st = os.waitpid(pid, 0)
        tail = got[got.rfind(b"\x1b[?1049l"):]
        check("q ends the show with every mouse mode, focus events and bracketed paste off",
              os.waitstatus_to_exitcode(st) == 0
              and all(b"\x1b[?%dl" % m in tail for m in (1000, 1002, 1003, 1005, 1006, 1015, 1004, 2004)),
              repr(got[-200:]))

    # The helpers, imported without leaving bytecode in the tree.
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.join(root, "fork/showcase"))
    import effects  # noqa: E402
    import frames  # noqa: E402
    import fx  # noqa: E402,F401  (the command line imports every module)
    import screen  # noqa: E402

    check("the outline table covers all 16 cell patterns", sorted(frames.OUTLINE) == list(range(16)),
          repr(sorted(frames.OUTLINE)))
    check("every effect --list names has its code",
          sorted(list(effects.EFFECTS) + list(effects.DOT_EFFECTS) + ["banner"]) == sorted(EFFECTS))
    r, w, b, d = 0xFF0000, 0xFFFFFF, 0x0000FF, screen.DEFAULT
    s = screen.render([[r, w, d], [r, b, b]])
    want = ("\x1b[1;1H\x1b[48;2;255;0;0m \x1b[38;2;255;255;255;48;2;0;0;255m▀"
            "\x1b[38;2;0;0;255;49m▄\x1b[0m")
    check("render draws a cell row as ▀/▄ with SGR only on change", s == want, repr(s))
    s = screen.render([[r, r, r], [r, r, r]], top=3, left=2, mask=[[True, False, True]])
    check("render skips masked cells with a cursor move, keeping its colours",
          s == "\x1b[3;2H\x1b[48;2;255;0;0m \x1b[3;4H \x1b[0m", repr(s))
    p = screen.Painter()
    p.draw([[r, w, b], [r, w, b]])
    s = p.draw([[r, b, b], [r, w, b]])
    check("a painter sends only the cells that changed",
          s == "\x1b[1;2H\x1b[38;2;0;0;255;48;2;255;255;255m▀\x1b[0m", repr(s))
    dots = screen.Dots(2, 1)
    dots.line(0, 0, 3, 3, w)
    check("Braille dots: a diagonal sets one dot per row, two cells",
          [chr(0x2800 + c) for c in dots.code] == ["⠑", "⢄"], repr(dots.code))
    s = frames.caca_sgr("\x1b[0;37;5;47;107mX\x1b[0;36;5;40;100m.\x1b[0m")
    check("img2txt's blink is dropped and its colours folded to greys",
          s == "\x1b[0;37;47;107mX\x1b[0;37;40;100m.\x1b[0m", repr(s))

    if failures:
        print(f"{len(failures)} failed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
