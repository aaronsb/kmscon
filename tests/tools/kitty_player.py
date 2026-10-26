#!/usr/bin/env python3
"""Checks the kitty graphics player, fork/tools/kitty-player/kitty.py, the
spec-written client of the kitty acceptance runs (ADR-103, #177), without a
video, a terminal or ffmpeg: its layout, easing and moves, its output
replayed on a model of the kitty graphics protocol's image store from the
player alone in every transport and from kitty.py on a pty with stand-ins
for ffmpeg and ffprobe, the shared memory probe and cleanup, and q and the
signals ending it. These checks came with the player from the Bad Apple!!
showcase's tests when the show moved to aaronsb/kmscon-showcase (#239).

    kitty_player.py --root SOURCE_ROOT
"""

import argparse
import base64
import os
import pty
import re
import select
import signal
import struct
import sys
import tempfile
import time
import zlib

failures = []


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name)
    if not ok:
        failures.append(name)
        if detail:
            print("      " + detail.replace("\n", "\n      "))


class KittyStore:
    """The image store of the kitty graphics protocol's spec, as far as the
    player uses it, fed the player's output: a transmission (a=t) of an id
    replaces the image and deletes its placements, a put (a=p) adds or
    replaces placement p of an image, and d=I deletes an image with its
    placements. Each transmission is decoded as a terminal would (base64,
    zlib, the PNG's chunks, CRCs and rows, or the shared memory object),
    its pixels kept in pixels. Records what breaks the spec's rules or the
    player's own: a command without q=2; a put without C=1, with a
    placement id outside 1 to 3, a z other than 4 - p, or c and r other
    than layout's; a put of an image not stored; a chunk over 4096 bytes
    or, but for the last, not a multiple of 4; a later chunk with keys
    other than m and q; data that does not decode to s x v; and a gap: a
    frame's copies deleted before the next frame's are placed."""

    def __init__(self, layout=None):
        self.layout = layout
        self.images = {}
        self.errors = []
        self.pixels = []
        self.peak = 0
        self.transmits = 0
        self.puts = 0
        self.held = None  # the keys of a chunked command until its last chunk
        self.data = b""
        self.gap = False  # nothing placed since a placed image was deleted

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
                self.data += data
                if keys.get("m") != "1":
                    keys, self.held = self.held, None
                    self.apply(keys, self.data)
                continue
            if keys.get("m") == "1":
                if len(data) % 4:
                    self.errors.append("a first chunk not a multiple of 4")
                self.held, self.data = keys, data
                continue
            self.apply(keys, data)

    def decode(self, k, data):
        raw = base64.b64decode(data)
        if k.get("t") == "s":
            name = raw.decode()
            with open("/dev/shm" + name, "rb") as f:
                raw = f.read()
        if k.get("o") == "z":
            raw = zlib.decompress(raw)
        if k.get("f") == "100":
            return png_pixels(raw)
        w, h = int(k["s"]), int(k["v"])
        if len(raw) < w * h * 3:
            raise ValueError(f"{len(raw)} bytes for {w}x{h} RGB")
        return raw[:w * h * 3]

    def apply(self, k, data=b""):
        if k.get("q") != "2":
            self.errors.append(f"a command without q=2: {k}")
        a, i = k.get("a"), int(k.get("i", 0))
        if a == "t":
            try:
                self.pixels.append(self.decode(k, data))
            except (ValueError, OSError, zlib.error) as e:
                self.errors.append(f"image {i} does not decode: {e}")
            self.images[i] = set()
            self.transmits += 1
        elif a == "p":
            self.puts += 1
            p = k.get("p")
            if k.get("C") != "1" or p not in ("1", "2", "3"):
                self.errors.append(f"a put without C=1 or with p={p}")
            elif k.get("z") != str(4 - int(p)):
                self.errors.append(f"placement {p} at z={k.get('z')}, not {4 - int(p)}")
            lay = self.layout
            if lay and (k.get("c"), k.get("r")) != (str(lay.c), str(lay.r)):
                self.errors.append(f"a put of c={k.get('c')},r={k.get('r')}, not c={lay.c},r={lay.r}")
            if i not in self.images:
                self.errors.append(f"a put of image {i}, not stored")
            else:
                if self.gap:
                    self.errors.append("a gap: the copies were deleted before the next frame's were placed")
                    self.gap = False
                self.images[i].add(int(p))
        elif a == "d" and k.get("d") == "I":
            had = bool(self.images.pop(i, None))
            if had and not self.placed():
                self.gap = True
        else:
            self.errors.append(f"an unexpected command {k}")
        self.peak = max(self.peak, len(self.images))

    def placed(self):
        return {i: sorted(p) for i, p in self.images.items() if p}


def png_pixels(png):
    """The grey bytes of an 8-bit greyscale PNG whose rows all use filter 0,
    as the player writes it, checking the signature, each chunk's CRC and
    the size; anything else raises ValueError."""
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("no PNG signature")
    pos, idat, w = 8, b"", None
    while pos < len(png):
        n, kind = struct.unpack(">I4s", png[pos:pos + 8])
        body = png[pos + 8:pos + 8 + n]
        if struct.unpack(">I", png[pos + 8 + n:pos + 12 + n])[0] != zlib.crc32(kind + body):
            raise ValueError(f"{kind!r} fails its CRC")
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if (depth, colour) != (8, 0):
                raise ValueError(f"depth {depth}, colour type {colour}")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    rows = zlib.decompress(idat)
    if w is None or len(rows) != (w + 1) * h or any(rows[y * (w + 1)] for y in range(h)):
        raise ValueError("rows not w + 1 bytes each with filter 0")
    return b"".join(rows[y * (w + 1) + 1:(y + 1) * (w + 1)] for y in range(h))


def kitty_checks(root):
    """The player: its layout, easing and moves, and its output on a model
    of the image store, from the player alone and from kitty.py on a pty
    with ffmpeg and ffprobe stood in for."""
    sys.path.insert(0, os.path.join(root, "fork/tools/kitty-player"))
    import kitty  # noqa: E402

    # At the operator's 7680x2160 with a 19x38 cell, 44 rows above the
    # rows left free: three copies the full height fit side by side.
    lay = kitty.Layout(404, 44, 19, 38, 1444 / 1080)
    check("three copies fit across 404x44 cells of 19x38 at full height",
          lay.r == 44 and lay.c == 118 and 3 * lay.c + 4 <= 404 - 2 and lay.slots[0] >= 1
          and lay.slots[2] + lay.c - 1 <= 404, f"r={lay.r} c={lay.c} slots={lay.slots}")
    lay = kitty.Layout(128, 24, 10, 22, 1444 / 1080)
    check("in 128x24 cells of 10x22 the copies shrink to fit three across",
          3 * lay.c + 4 <= 126 and lay.r < 24 and lay.top == 1 + (24 - lay.r) // 2,
          f"r={lay.r} c={lay.c} top={lay.top}")
    tall = kitty.Layout(1000, 200, 10, 20, 1444 / 1080, copies=1)
    check("frames are a copy's own size, at most 2160 pixels tall, or as wide as asked",
          kitty.frame_size(kitty.Layout(404, 44, 19, 38, 1444 / 1080), 38, 1444 / 1080) == (2236, 1672)
          and kitty.frame_size(tall, 20, 1444 / 1080) == (2888, 2160)
          and kitty.frame_size(tall, 20, 1444 / 1080, 480) == (480, 360),
          repr(kitty.frame_size(tall, 20, 1444 / 1080)))
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
    # frame shows and decodes to what was sent, each frame's copies are placed
    # before the last frame's are deleted, and at the end nothing is left.
    for transport in kitty.TRANSPORTS:
        store = KittyStore(layout=lay)
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
        sent = [bytes([n]) * enc.frame_bytes for n in range(45)]
        check(f"{transport}: 45 frames on the player's clock, each transmitted once and placed once a copy",
              player.stats.frames == 45 and store.transmits == 45 and not store.errors
              and store.puts >= 45 + 2 * 15, f"{player.stats.line(transport)} {store.errors[:3]}")
        check(f"{transport}: every frame decodes to the pixels sent",
              store.pixels == sent, f"{len(store.pixels)} decoded, first {store.pixels[:1]!r:.60}")
        check(f"{transport}: after each frame one image holds every copy, and the store never holds more than two",
              all(len(p) == 1 for p in after) and store.peak <= 2 and after[-1] and list(after[-1].values())[0] == [1, 2, 3],
              f"peak {store.peak}, {after[:3]} ... {after[-1:]}")
        check(f"{transport}: the player stops with no image left", store.images == {}, repr(store.images))
        if transport == "shm":
            shm_ok = all(base64.b64decode(n).decode().count("/") == 1 and base64.b64decode(n).startswith(b"/")
                         for n in names) and len(names) == player.stats.frames
            check("shm: each frame a new object named as the spec asks, / and no other",
                  shm_ok and len(set(names)) == len(names), repr(names[:3]))
            check("shm: objects the terminal did not read are removed",
                  not any(os.path.exists("/dev/shm" + base64.b64decode(n).decode()) for n in names))
    # Frames no terminal reads: /dev/shm holds at most SHM_BUDGET of them
    # (here ten frames' worth). The player stops sending until the terminal
    # reads one, the encoder alone unlinks the oldest past it, and none
    # outlive the player.
    budget, kitty.SHM_BUDGET = kitty.SHM_BUDGET, 10 * 4 * 4 * 3
    try:
        enc = kitty.Encoder("shm", 4, 4)
        player = kitty.Player(enc, lay, kitty.Choreography(copies=1), 30, 0, write=lambda s: None,
                              clock=kitty.Clock(now=lambda: 0.0))
        for n in range(25):
            player.show(b"\0" * enc.frame_bytes, 0.0)

        def unread():
            return sum(os.path.exists(f"/dev/shm{enc.prefix}{k}") for k in range(1, enc.serial + 1))
        held, sent = unread(), player.stats.frames
        kitty.unlink_shm(f"{enc.prefix}1")  # the terminal reads the oldest
        player.show(b"\0" * enc.frame_bytes, 0.0)
        check("shm: unread frames stop at the budget, the rest dropped, and one read lets the next through",
              enc.keep == 10 and held == 10 and sent == 10 and player.stats.dropped == 15
              and player.stats.frames == 11, f"held {held}, sent {sent}, {player.stats.line('shm')}")
        most = 0
        for n in range(25):
            enc.transmit(kitty.IMAGE_IDS[0], b"\0" * enc.frame_bytes)
            most = max(most, unread())
        enc.close()
        left = unread()
        check("shm: the encoder alone unlinks the oldest past the budget, and none outlive the player",
              most == enc.keep and left == 0, f"most {most}, left {left}")
        for k in range(1, enc.serial + 1):  # what a failing run left
            kitty.unlink_shm(f"{enc.prefix}{k}")
    finally:
        kitty.SHM_BUDGET = budget
    stale = f"/dev/shm/kmscon-kitty-player-{os.getpid()}-1"
    open(stale, "wb").close()
    enc = kitty.Encoder("shm", 4, 4)
    cmd = enc.transmit(kitty.IMAGE_IDS[0], b"\0" * enc.frame_bytes)
    enc.close()
    os.unlink(stale)
    check("shm: a name a crashed run left is skipped", f"-{os.getpid()}-2".encode() in base64.b64decode(
        cmd.rsplit(";", 1)[1][:-2]), cmd)
    cmd, e = kitty.query("shm")
    e.close()
    check("shm: the probe's object has a name of its own",
          b"kmscon-kitty-player-probe-" in base64.b64decode(cmd.rsplit(";", 1)[1][:-2]), cmd)

    # kitty.py on a pty, with ffprobe and ffmpeg stood in for.
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
                    "for n in range(900):\n    sys.stdout.buffer.write(bytes([n * 4 % 256]) * (w * h * bpp))\n")
        for name in ("ffprobe", "ffmpeg"):
            os.chmod(os.path.join(stubs, name), 0o755)
        report = os.path.join(tmp, "report")

        def run_player(*args, answer=None, keys=b"", pace=0.0, timeout=10, kill=None, **env):
            """Runs kitty.py on a pty and returns its status, its output
            and its pid. answer is written back once the output holds an
            a=q; keys are typed at once; with pace, the output is read 4 KB
            at a time that many seconds apart, a terminal falling behind;
            kill is a signal sent once a second has passed."""
            pid, fd = pty.fork()
            if pid == 0:
                os.environ["PATH"] = stubs + ":" + os.environ["PATH"]
                os.environ.update(env)
                os.execvp("python3", ["python3", "-B", os.path.join(root, "fork/tools/kitty-player/kitty.py"),
                                      "--video=clip.mp4", *args])
            if keys:
                time.sleep(0.3)
                os.write(fd, keys)
            got = b""
            start = time.monotonic()
            end = start + timeout
            while time.monotonic() < end:
                if kill and time.monotonic() - start > 1:
                    os.kill(pid, kill)
                    kill = None
                r, _, _ = select.select([fd], [], [], max(0, end - time.monotonic()))
                try:
                    d = os.read(fd, 4096 if pace else 65536) if r else b""
                except OSError:
                    break
                if not d:
                    break
                got += d
                if answer and b"a=q" in got:
                    os.write(fd, answer)
                    answer = None
                if pace:
                    time.sleep(pace)
            else:
                os.kill(pid, 9)
            _, st = os.waitpid(pid, 0)
            os.close(fd)
            return st, got, pid

        # 64x48 raw frames, chunked; the moves at 0.2, 0.4 and 0.6 seconds.
        st, got, _ = run_player("--seconds", "1", "--transport", "raw", "--width", "64", "--banner-rows", "4",
                              "--slide-at", "0.2", "--copy2-at", "0.4", "--copy3-at", "0.6", "--move", "0.2",
                              "--report", report)
        store = KittyStore()
        store.feed(got)
        rep = open(report).read() if os.path.exists(report) else ""
        check("kitty.py on a pty: frames chunked within the spec, decoded, three copies, nothing left stored",
              os.waitstatus_to_exitcode(st) == 0 and not store.errors and store.transmits >= 25
              and store.images == {} and store.peak <= 2 and b"m=1" in got and "copies=moves" in rep,
              f"status {st}, {store.errors[:3]}, {store.transmits} frames, {rep}")
        check("kitty.py sends no query when its transport needs none, and leaves the cursor where it was",
              b"a=q" not in got and got.count(b"\x1b7") == got.count(b"\x1b8") > 0, repr(got[:120]))
        # Width 0: frames as tall as a copy's rows in pixels, so the terminal
        # shows them at their size. 100x40 cells of 10x20, 4 rows for the
        # scroller: three copies 23 rows tall.
        st, got, _ = run_player("--seconds", "0.2", "--transport", "raw", "--width", "0", "--banner-rows", "4",
                              "--cell-w", "10", "--cell-h", "20", "--copies", "3", COLUMNS="100", LINES="40")
        want = b"s=294,v=220"  # 11 rows of 20 pixels, 1444:1080 across
        check(f"kitty.py --width 0 sends frames of a copy's own size ({want.decode()})",
              os.waitstatus_to_exitcode(st) == 0 and want in got, repr(re.findall(rb"s=\d+,v=\d+", got)[:1]))
        # No rows above the free rows: nothing is placed over them.
        st, got, _ = run_player("--seconds", "0.2", "--transport", "png", "--banner-rows", "12", COLUMNS="100",
                              LINES="12")
        check("with no rows above the free rows, kitty.py places nothing",
              os.waitstatus_to_exitcode(st) == 0 and b"a=t" in got and b"a=p" not in got, repr(got[:120]))

        # shm, refused and taken: the probe's answer decides.
        st, got, pid = run_player("--seconds", "0.3", "--transport", "shm", "--width", "64",
                                answer=b"\x1b_Gi=7500;EBADF:refused\x1b\\\x1b[?62;4c")
        check("shm refused by the probe: kitty.py falls back to png",
              os.waitstatus_to_exitcode(st) == 0 and b"a=q" in got and b"t=s" not in got.split(b"\x1b[c", 1)[-1]
              and b"f=100" in got, repr(got[:200]))
        st, got, pid = run_player("--seconds", "0.3", "--transport", "shm", "--width", "64",
                                answer=b"\x1b_Gi=7500;OK\x1b\\\x1b[?62;4c")
        left = [n for n in os.listdir("/dev/shm") if n.startswith(f"kmscon-kitty-player-{pid}-")
                or n.startswith(f"kmscon-kitty-player-probe-{pid}-")]
        check("shm taken by the probe: frames go by t=s, and no object outlives the player",
              os.waitstatus_to_exitcode(st) == 0 and got.count(b"t=s") > 3 and not left, f"{left} {got[:200]!r}")

        # SIGTERM and SIGHUP: the player stops through its cleanup, its
        # images deleted and its shared memory unlinked, as on Ctrl-C.
        for sig in (signal.SIGTERM, signal.SIGHUP):
            st, got, pid = run_player("--seconds", "20", "--transport", "shm", "--width", "64", kill=sig,
                                    answer=b"\x1b_Gi=7500;OK\x1b\\\x1b[?62;4c")
            left = [n for n in os.listdir("/dev/shm") if n.startswith(f"kmscon-kitty-player-{pid}-")]
            for n in left:
                os.unlink("/dev/shm/" + n)
            check(f"{sig.name} ends kitty.py through its cleanup: images deleted, no shared memory left",
                  os.waitstatus_to_exitcode(st) == 128 + sig and not left
                  and got.rstrip().endswith(b"a=d,d=I,i=7502,q=2;\x1b\\"),
                  f"status {st}, {len(left)} objects left, {got[-80:]!r}")

        # q while the terminal is behind: read slowly, the player never
        # catches up to sleep, yet q ends it at once (status 10).
        start = time.monotonic()
        st, got, _ = run_player("--seconds", "20", "--transport", "raw", "--width", "320", keys=b"q", pace=0.02,
                              timeout=15)
        took = time.monotonic() - start
        check("q ends kitty.py while the terminal is behind",
              os.waitstatus_to_exitcode(st) == 10 and took < 5, f"status {st}, {took:.1f} s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = os.path.abspath(ap.parse_args().root)
    kitty_checks(root)
    if failures:
        print(f"{len(failures)} failed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
