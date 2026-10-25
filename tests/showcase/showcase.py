#!/usr/bin/env python3
"""Checks fork/showcase/bad-apple.sh without a video, a terminal or any of
the media tools: its act and effect tables (--list), its plan (--dry-run),
its refusals, that every act listed has its function, and the pure helpers
the acts draw with (screen.py, frames.py, effects.py and act 5's sizing in
bad-apple.sh), and, on a pty, that quitting turns off every mode a player
may have left on (#88). Act 7's kitty player (kitty.py) runs on a pty with
stand-ins for ffmpeg and ffprobe, its output replayed on a model of the
kitty graphics protocol's image store.

    showcase.py --root SOURCE_ROOT
"""

import argparse
import base64
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
        check("every act plays on kmscon (act 7 through kitty graphics since #169)",
              all("placeholder" not in l for l in rows), p.stdout)

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
                            ("\x1b[4;2128;7676t\x1b[?62;4c", "19 38"), ("\x1b[?62;4c", "0 0"),
                            ("\x1b[6;038;019t", "19 38"), ("\x1b[6;99999;7t", "7 1000"),
                            ("\x1b[6;0;19t", "0 0")):
            p = shell('screen_cols=404 screen_rows=56 cell_w=0 cell_h=0; winops_cell "$REPLY"; echo $cell_w $cell_h',
                      REPLY=reply)
            check(f"XTWINOPS answers {reply.replace(chr(27), 'ESC')} give the cell {want}",
                  p.stdout.strip() == want, p.stdout + p.stderr)

        # While a player runs, keys are read through a filter that drops the
        # mouse reports mpv's tracking sends, whole or split across reads,
        # and keeps q as quit; after it, the modes go off and what the
        # terminal had already sent is read through the same filter (#88).
        def keys(code, feed):
            pid, fd = pty.fork()
            if pid == 0:
                os.execve("/bin/bash", ["bash", "-c", 'source "$1"; fx_py=$here/fx.py; '
                                        "stty -echo -icanon min 1 time 0; " + code + "; echo SURVIVED",
                                        "sh", script], env)
            time.sleep(0.3)
            for chunk in feed:
                os.write(fd, chunk)
                time.sleep(0.08)
            got = b""
            while True:
                r, _, _ = select.select([fd], [], [], 5)
                try:
                    d = os.read(fd, 4096) if r else b""
                except OSError:
                    break
                if not d:
                    break
                got += d
            os.waitpid(pid, 0)
            return got
        report = b"\x1b[M#q5"  # a press at column 81 (q), row 21
        for name, feed, survives in (
                ("a mouse report at column 81 is dropped", [report], True),
                ("a report split across reads is dropped", [b"\x1b[", b"M#", b"q5"], True),
                ("an SGR report is dropped", [b"\x1b[<0;81;21M"], True),
                ("q quits", [b"q"], False),
                ("q right after Esc quits", [b"\x1bq"], False),
                ("q after an arrow key quits", [b"\x1b[A", b"q"], False)):
            got = keys("sleep 1.5 & wait_bg $!", feed)
            check(f"while a player runs, {name}", (b"SURVIVED" in got) == survives, repr(got[-120:]))
        got = keys("sleep 0.6; player_done; IFS= read -rsn1 -t 0.3 k && echo LEFT", [report])
        check("after a player, tracking goes off and a report already sent is read and dropped",
              b"SURVIVED" in got and b"\x1b[?1003l" in got and b"LEFT" not in got, repr(got[-200:]))

        # mpv's video outputs show the cursor when they exit; act 5 hides it
        # again, or the next acts draw with it on, and a frame caught half
        # written shows it as a block in the cells' colours.
        stubs = os.path.join(tmp, "stubs")
        os.mkdir(stubs)
        for name, body in (("mpv", r"printf '\033[?25l\033[?1003h'; sleep 0.2; printf '\033[?25h\033[?1003l\033[?25h'"),
                           ("ffprobe", "echo 960,718")):
            with open(os.path.join(stubs, name), "w") as f:
                f.write(f"#!/bin/sh\n{body}\n")
            os.chmod(os.path.join(stubs, name), 0o755)
        got = keys(f"PATH={stubs}:$PATH; have_sixel=1 screen_cols=136 screen_rows=40 cell_w=10 cell_h=19; "
                   "act5_clip() { echo clip.mp4; }; pause() { :; }; act_5; echo NEXT-ACT", [])
        shown = re.findall(rb"\x1b\[\?25[hl]", got[:got.find(b"NEXT-ACT")])
        check("after act 5's player, the cursor is hidden again",
              b"NEXT-ACT" in got and shown[-1:] == [b"\x1b[?25l"], repr(shown))

        # With half a report buffered and stdin closed, the drain stops at
        # once; it used to spin for its second, writing read errors.
        start = time.monotonic()
        p = shell('keybuf=$E[; player_done 0<&- 2>&1 | wc -c')
        took = time.monotonic() - start
        check("after a player, a closed stdin ends the drain at once",
              p.returncode == 0 and int(p.stdout.strip() or 0) < 1000 and took < 0.9,
              f"{p.stdout.strip()} bytes in {took:.2f} s {p.stderr}")

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

    picker_checks(root)
    kitty_checks(root)

    if failures:
        print(f"{len(failures)} failed")
        return 1
    return 0


PULSE_LIST = """List of detected audio devices:
  'auto' (Autoselect device)
  'pulse' (Default (pulseaudio))
  'pulse/alsa_output.pci-0000_00_1f.3.analog-stereo' (Built-in Audio Analog Stereo)
  'alsa' (Default (alsa))
"""


def picker_checks(root):
    """fork/demo/pick-audio.sh, the showcase's output picker, with stand-ins
    for mpv (a PulseAudio desktop's list), pgrep (no PipeWire running) and
    getent, and a sound card directory of its own."""
    picker = os.path.join(root, "fork/demo/pick-audio.sh")
    with tempfile.TemporaryDirectory() as tmp:
        stubs = os.path.join(tmp, "bin")
        os.mkdir(stubs)
        stub = {"mpv": "#!/bin/sh\nfor a; do [ \"$a\" = --audio-device=help ] && { cat \"$LIST\"; exit 0; }; done\nexit 0\n",
                "pgrep": "#!/bin/sh\nexit 1\n",
                "getent": "#!/bin/sh\n[ -n \"$GROUP\" ] || exit 2\necho \"$GROUP\"\n"}
        for name, body in stub.items():
            with open(os.path.join(stubs, name), "w") as f:
                f.write(body)
            os.chmod(os.path.join(stubs, name), 0o755)
        listing = os.path.join(tmp, "list")
        with open(listing, "w") as f:
            f.write(PULSE_LIST)
        snd = os.path.join(tmp, "snd")
        os.mkdir(snd)
        open(os.path.join(snd, "controlC0"), "w").close()
        # A directory holding pulse/cookie, as ~/.config does, must not turn
        # the pattern pulse/* into a file name.
        cwd = os.path.join(tmp, "cwd")
        os.makedirs(os.path.join(cwd, "pulse"))
        open(os.path.join(cwd, "pulse", "cookie"), "w").close()

        def pick(stdin, *args, group="audio:x:995:", **extra):
            env = {"PATH": stubs + ":/usr/bin:/bin", "HOME": tmp, "LIST": listing,
                   "GROUP": group, "PICK_AUDIO_SND": snd, **extra}
            return subprocess.run(["bash", picker, *args], input=stdin, env=env, cwd=cwd,
                                  capture_output=True, text=True, timeout=30)

        p = pick("0\n")
        check("the picker lists a PulseAudio desktop's sinks with no PipeWire running, and 0 is no sound",
              p.returncode == 0 and p.stdout == "none\n" and "Built-in Audio Analog Stereo" in p.stderr
              and "Default (pulseaudio)" in p.stderr, p.stdout + p.stderr)
        p = pick("0\n", group="")
        check("the picker runs on a host with no audio group",
              p.returncode == 0 and p.stdout == "none\n" and "Built-in Audio" in p.stderr, p.stdout + p.stderr)
        p = pick("2\ny\n")
        check("a number and y keep that output",
              p.returncode == 0 and p.stdout == "pulse/alsa_output.pci-0000_00_1f.3.analog-stereo\n",
              p.stdout + p.stderr)
        p = pick("", "--no-wait")
        check("--no-wait asks nothing and plays through mpv's default output",
              p.returncode == 0 and p.stdout == "auto\n" and "--no-wait" in p.stderr, p.stdout + p.stderr)
        p = pick("", AUDIO_DEVICE="pulse/x")
        check("AUDIO_DEVICE answers without a question", p.returncode == 0 and p.stdout == "pulse/x\n",
              p.stdout + p.stderr)
        # Runs with no act that plays sound ask nothing.
        script = os.path.join(root, "fork/showcase/bad-apple.sh")
        for first, last, banner, why in (("2", "2", "", "act 2 alone"), ("7", "7", "1", "act 7 --banner-only")):
            p = subprocess.run(["bash", "-c", 'source "$1"; first=$2 last=$3 banner_only=$4 audio=1 wait_key=1; '
                                'pick_audio; echo "audio=$audio"', "sh", script, first, last, banner],
                               env={"PATH": stubs + ":/usr/bin:/bin", "HOME": tmp, "LIST": listing,
                                    "PICK_AUDIO_SND": snd},
                               stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
            check(f"{why} asks for no output", p.returncode == 0 and p.stdout == "audio=0\n"
                  and "Where should" not in p.stderr, p.stdout + p.stderr)
        if os.geteuid() != 0:
            os.chmod(os.path.join(snd, "controlC0"), 0)
            p = pick("0\n")
            check("unreachable sound cards are named, with the audio group's fix",
                  p.returncode == 0 and "out of reach" in p.stderr and "usermod -aG audio" in p.stderr,
                  p.stdout + p.stderr)


class KittyStore:
    """The image store of the kitty graphics protocol's spec, as far as the
    player uses it, fed the player's output: a transmission (a=t) of an id
    replaces the image and deletes its placements, a put (a=p) adds or
    replaces placement p of an image, and d=I deletes an image with its
    placements. Records what breaks the spec's rules or the player's own:
    a command without q=2, a put without C=1 or with a placement id outside
    1 to 3, a put of an image not stored, a chunk over 4096 bytes or, but
    for the last, not a multiple of 4, a later chunk with keys other than m
    and q, and more than two images stored at once."""

    def __init__(self):
        self.images = {}
        self.errors = []
        self.peak = 0
        self.transmits = 0
        self.puts = 0
        self.held = None  # the keys of a chunked command until its last chunk

    def feed(self, stream):
        for m in re.finditer(rb"\x1b_G([^;]*);(.*?)\x1b\\", stream, re.S):
            keys = dict(kv.split("=", 1) for kv in m.group(1).decode().split(",") if kv)
            data = m.group(2)
            if len(data) > 4096:
                self.errors.append(f"a chunk of {len(data)} bytes")
            if self.held is not None:
                if set(keys) - {"m", "q"}:
                    self.errors.append(f"a later chunk with keys {sorted(keys)}")
                if keys.get("m") == "1" and len(data) % 4:
                    self.errors.append("a chunk not a multiple of 4")
                if keys.get("m") != "1":
                    keys, self.held = self.held, None
                    self.apply(keys)
                continue
            if keys.get("m") == "1":
                if len(data) % 4:
                    self.errors.append("a first chunk not a multiple of 4")
                self.held = keys
                continue
            self.apply(keys)

    def apply(self, k):
        if k.get("q") != "2":
            self.errors.append(f"a command without q=2: {k}")
        a, i = k.get("a"), int(k.get("i", 0))
        if a == "t":
            self.images[i] = set()
            self.transmits += 1
        elif a == "p":
            self.puts += 1
            if k.get("C") != "1" or k.get("p") not in ("1", "2", "3"):
                self.errors.append(f"a put without C=1 or with p={k.get('p')}")
            if i not in self.images:
                self.errors.append(f"a put of image {i}, not stored")
            else:
                self.images[i].add(int(k["p"]))
        elif a == "d" and k.get("d") == "I":
            self.images.pop(i, None)
        else:
            self.errors.append(f"an unexpected command {k}")
        self.peak = max(self.peak, len(self.images))

    def placed(self):
        return {i: sorted(p) for i, p in self.images.items() if p}


def kitty_checks(root):
    """Act 7's player: its layout, easing and moves, and its output on a
    model of the image store, from the player alone and from fx.py kitty on
    a pty with ffmpeg and ffprobe stood in for."""
    import kitty  # noqa: E402  (on the path main set)

    # At the operator's 7680x2160 with a 19x38 cell, 44 rows above the
    # scroller: three copies the full height fit side by side.
    lay = kitty.Layout(404, 44, 19, 38, 1444 / 1080)
    check("three copies fit across 404x44 cells of 19x38 at full height",
          lay.r == 44 and lay.c == 118 and 3 * lay.c + 4 <= 404 - 2 and lay.slots[0] >= 1
          and lay.slots[2] + lay.c - 1 <= 404, f"r={lay.r} c={lay.c} slots={lay.slots}")
    lay = kitty.Layout(128, 24, 10, 22, 1444 / 1080)
    check("in 128x24 cells of 10x22 the copies shrink to fit three across",
          3 * lay.c + 4 <= 126 and lay.r < 24 and lay.top == 1 + (24 - lay.r) // 2,
          f"r={lay.r} c={lay.c} top={lay.top}")
    peak = max(kitty.ease(u / 1000, 0.12) for u in range(1001))
    check("a slide starts at 0, overshoots by about the bounce and settles on 1",
          kitty.ease(0, 0.12) == 0 and abs(kitty.ease(1, 0.12) - 1) < 1e-9 and 1.09 < peak < 1.13
          and max(kitty.ease(u / 100, 0) for u in range(101)) <= 1, f"peak {peak}")

    lay = kitty.Layout(404, 44, 19, 38, 1444 / 1080)
    chor = kitty.Choreography(slide=5, copy2=10, copy3=15, move=1, bounce=0.12)
    pos = {t: chor.positions(lay, t) for t in (0, 4.9, 5.4, 7, 10.4, 12, 17)}
    left = min(chor.positions(lay, 5 + u / 100)[0][1] for u in range(101))
    check("the moves: one copy in the middle, it slides left past its slot and back, then two, then three",
          pos[0] == pos[4.9] == [(1, lay.centre)] and pos[7] == [(1, lay.slots[0])]
          and left < lay.slots[0] and len(pos[10.4]) == 2 and pos[12] == [(1, lay.slots[0]), (2, lay.slots[1])]
          and pos[17] == [(1, lay.slots[0]), (2, lay.slots[1]), (3, lay.slots[2])],
          f"{pos} leftmost {left}")
    check("a copy slides out from behind the one before it",
          chor.positions(lay, 10)[1][1] == lay.slots[0] and chor.positions(lay, 15)[2][1] == lay.slots[1],
          repr(chor.positions(lay, 15)))

    # The player alone, on a clock that moves a tenth of a frame a read: every
    # frame shows, each write leaves one image placed, and at the end none.
    for transport in kitty.TRANSPORTS:
        store = KittyStore()
        writes = []
        t = [0.0]

        def clock_now():
            t[0] += 0.1 / 30
            return t[0]

        def write(s):
            writes.append(s)
            store.feed(s.encode())
        enc = kitty.Encoder(transport, 64, 48)
        clock = kitty.Clock(now=clock_now)
        player = kitty.Player(enc, lay, kitty.Choreography(slide=0.3, copy2=0.6, copy3=0.9, move=0.2), 30, 1.5,
                              write=write, clock=clock)
        player.sleep = lambda sec: None
        after = []
        frames = (bytes([n % 256]) * enc.frame_bytes for n in range(1000))
        orig_show = player.show

        def show(frame, now):
            orig_show(frame, now)
            after.append(store.placed())
        player.show = show
        player.run(frames)
        names = [n for n in re.findall(r"t=s,S=\d+;([A-Za-z0-9+/=]+)", "".join(writes))]
        enc.close()
        shm_ok = all(base64.b64decode(n).decode().count("/") == 1 and base64.b64decode(n).startswith(b"/")
                     for n in names) and len(names) == player.stats.frames if transport == "shm" else True
        check(f"{transport}: 45 frames on the soundtrack's clock, each transmitted once and placed once a copy",
              player.stats.frames == 45 and store.transmits == 45 and not store.errors
              and store.puts >= 45 + 2 * 15, f"{player.stats.line(transport)} {store.errors[:3]}")
        check(f"{transport}: after each frame one image holds every copy, and the store never holds more than two",
              all(len(p) == 1 for p in after) and store.peak <= 2 and after[-1] and list(after[-1].values())[0] == [1, 2, 3],
              f"peak {store.peak}, {after[:3]} ... {after[-1:]}")
        check(f"{transport}: the player stops with no image left", store.images == {}, repr(store.images))
        if transport == "shm":
            check("shm: each frame a new object named as the spec asks, / and no other", shm_ok and len(set(names)) == len(names),
                  repr(names[:3]))
            check("shm: objects the terminal did not read are removed",
                  not any(os.path.exists("/dev/shm" + base64.b64decode(n).decode()) for n in names))

    # fx.py kitty on a pty, with ffprobe and ffmpeg stood in for: 64x48 raw
    # frames, chunked; the moves at 0.2, 0.4 and 0.6 seconds.
    with tempfile.TemporaryDirectory() as tmp:
        stubs = os.path.join(tmp, "bin")
        os.mkdir(stubs)
        with open(os.path.join(stubs, "ffprobe"), "w") as f:
            f.write('#!/bin/sh\necho \'{"streams": [{"width": 1444, "height": 1080, "r_frame_rate": "30/1", '
                    '"sample_aspect_ratio": "1:1"}]}\'\n')
        with open(os.path.join(stubs, "ffmpeg"), "w") as f:
            f.write("#!/usr/bin/env python3\nimport re, sys\n"
                    "w, h = map(int, re.search(r'scale=(\\d+):(\\d+)', ' '.join(sys.argv)).groups())\n"
                    "bpp = 1 if 'gray' in sys.argv else 3\n"
                    "for n in range(60):\n    sys.stdout.buffer.write(bytes([n * 4 % 256]) * (w * h * bpp))\n")
        for name in ("ffprobe", "ffmpeg"):
            os.chmod(os.path.join(stubs, name), 0o755)
        report = os.path.join(tmp, "report")
        pid, fd = pty.fork()
        if pid == 0:
            os.environ["PATH"] = stubs + ":" + os.environ["PATH"]
            os.execvp("python3", ["python3", "-B", os.path.join(root, "fork/showcase/fx.py"), "kitty",
                                  "--video", "clip.mp4", "--seconds", "1", "--transport", "raw", "--width", "64",
                                  "--banner-rows", "4", "--slide-at", "0.2", "--copy2-at", "0.4",
                                  "--copy3-at", "0.6", "--move", "0.2", "--report", report])
        got = b""
        while True:
            r, _, _ = select.select([fd], [], [], 10)
            try:
                d = os.read(fd, 65536) if r else b""
            except OSError:
                break
            if not d:
                break
            got += d
        _, st = os.waitpid(pid, 0)
        store = KittyStore()
        store.feed(got)
        rep = open(report).read() if os.path.exists(report) else ""
        check("fx.py kitty on a pty: frames chunked within the spec, three copies, nothing left stored",
              os.waitstatus_to_exitcode(st) == 0 and not store.errors and store.transmits >= 25
              and store.images == {} and store.peak <= 2 and b"m=1" in got and "copies=moves" in rep,
              f"status {st}, {store.errors[:3]}, {store.transmits} frames, {rep}")
        check("fx.py kitty sends no query when its transport needs none, and leaves the cursor where it was",
              b"a=q" not in got and got.count(b"\x1b7") == got.count(b"\x1b8") > 0, repr(got[:120]))


if __name__ == "__main__":
    sys.exit(main())
