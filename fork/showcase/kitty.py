"""Act 7's video player (#75): Bad Apple!! through the kitty graphics
protocol, written from its published spec alone
(https://sw.kovidgoyal.net/kitty/graphics-protocol/, ADR-300's clean room).

ffmpeg decodes the clip small, and each frame is transmitted once and placed
up to three times, each copy a placement (p=1, 2, 3) scaled up by the
terminal into c x r cells. A copy moves by being placed again under its own
placement id at another column, which the spec says replaces the placement
"without flicker"; nothing is transmitted again.

Re-transmitting an image id deletes the image's placements (the spec's note
under "Display images on screen"), so the frames alternate between two
image ids: frame n goes to one id, its copies are placed, and then the other
id is deleted (d=I, which frees its data), all in one write, so the picture
is never without its copies and the terminal never holds more than two
images of the player's.

The transports (--transport; png by default, the cheapest inline one):
  raw   f=24 RGB inline (t=d), base64 in chunks of 4096 (m=1)
  zlib  the same compressed with zlib (o=z): Bad Apple is nearly all black
        and white, so a frame shrinks many times
  png   f=100, an 8-bit greyscale PNG made here; the spec has no grey raw
        format (f is 24, 32 or 100), and PNG is its compact form
  shm   f=24 RGB in a POSIX shared memory object (t=s), a new name each
        frame, starting with / as the spec requires; the terminal reads
        and unlinks it

Every command carries q=2, so the terminal sends nothing back into the
show's input, and C=1, so the cursor stays where it was. Frames follow the
soundtrack's clock (mpv's time-pos over its IPC socket, --audio-ipc), or the
monotonic clock without one, and are dropped, never delayed, when the
terminal falls behind.
"""

import base64
import functools
import json
import math
import os
import select
import socket
import struct
import subprocess
import time
import zlib

from screen import ESC, cup, geom, out, poll_keys, resized, wait

TRANSPORTS = ("raw", "zlib", "png", "shm")
IMAGE_IDS = (7501, 7502)    # the two ids the frames alternate between
QUERY_ID = 7500             # the transport probe's a=q
CHUNK = 4096                # the spec's largest chunk of base64
COPIES = 3
TICK = 1 / 60               # how often a moving copy is placed again
ST = ESC + "\\"


# ------------------------------------------------------------- commands

def apc(keys, payload=""):
    return f"{ESC}_G{keys};{payload}{ST}"


def chunked(keys, data):
    """One command with base64 payload data, in chunks of CHUNK with m=1 on
    all but the last; later chunks carry m and q only, as the spec asks."""
    if len(data) <= CHUNK:
        return apc(keys, data)
    parts = [apc(f"{keys},m=1", data[:CHUNK])]
    for i in range(CHUNK, len(data), CHUNK):
        parts.append(apc(f"m={int(i + CHUNK < len(data))},q=2", data[i:i + CHUNK]))
    return "".join(parts)


def b64(b):
    return base64.b64encode(b).decode()


def png_grey(grey, w, h, level=1):
    """An 8-bit greyscale PNG (colour type 0) of w x h bytes, each row with
    filter 0."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    rows = b"".join(b"\0" + grey[y * w:(y + 1) * w] for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, level)) + chunk(b"IEND", b""))


class Encoder:
    """Turns a decoded frame into the transmit command of one transport.
    shm writes each frame to /dev/shm (where Linux keeps POSIX shared memory
    objects; shm_open("/name") opens /dev/shm/name) under a new name."""

    def __init__(self, transport, w, h, level=1):
        if transport not in TRANSPORTS:
            raise ValueError(f"no transport {transport!r}: {', '.join(TRANSPORTS)}")
        self.transport, self.w, self.h, self.level = transport, w, h, level
        self.pix = "gray" if transport == "png" else "rgb24"
        self.frame_bytes = w * h * (1 if transport == "png" else 3)
        self.names = []
        self.serial = 0

    def transmit(self, image_id, frame):
        keys = f"a=t,i={image_id},q=2"
        if self.transport == "raw":
            return chunked(f"{keys},f=24,s={self.w},v={self.h}", b64(frame))
        if self.transport == "zlib":
            return chunked(f"{keys},f=24,s={self.w},v={self.h},o=z", b64(zlib.compress(frame, self.level)))
        if self.transport == "png":
            return chunked(f"{keys},f=100", b64(png_grey(frame, self.w, self.h, self.level)))
        name = self.shm_write(frame)
        return apc(f"{keys},f=24,s={self.w},v={self.h},t=s,S={len(frame)}", b64(name.encode()))

    def shm_write(self, data):
        self.serial += 1
        name = f"/kmscon-showcase-{os.getpid()}-{self.serial}"
        fd = os.open("/dev/shm" + name, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        try:
            os.write(fd, data)
        finally:
            os.close(fd)
        self.names.append(name)
        if len(self.names) > 64:
            del self.names[0]
        return name

    def close(self):
        """Removes the objects the terminal has not read: those it refused,
        or had not reached when the player stopped."""
        for name in self.names:
            try:
                os.unlink("/dev/shm" + name)
            except OSError:
                pass
        self.names = []


def query(transport):
    """The spec's a=q of one pixel in this transport, which loads and checks
    the data without storing it."""
    e = Encoder(transport, 1, 1)
    frame = b"\0" if transport == "png" else b"\0\0\0"
    cmd = e.transmit(QUERY_ID, frame).replace(f"a=t,i={QUERY_ID},q=2", f"a=q,i={QUERY_ID}", 1)
    return cmd, e


def probe(transport, timeout=1.0):
    """True when the terminal loads a pixel through this transport: an a=q
    answered OK before its answer to DA1. A terminal that refuses the medium
    (kmscon reads no shared memory for a session outside logind) answers
    with an error, and one without the protocol answers DA1 alone."""
    cmd, e = query(transport)
    try:
        out(cmd + f"{ESC}[c")
        reply = b""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if select.select([0], [], [], max(0, end - time.monotonic()))[0]:
                d = os.read(0, 1024)
                if not d:
                    break
                reply += d
                if b"\x1b[?" in reply and reply.rstrip().endswith(b"c"):
                    break
        return f"{ESC}_Gi={QUERY_ID};OK".encode() in reply
    finally:
        e.close()


# --------------------------------------------------------------- layout

class Layout:
    """Where the copies go in the cols x rows above the scroller: each r rows
    tall and c columns wide (the clip's aspect in the cell's), as tall as
    fits with three side by side and gap columns between, and centred down
    the rows. slots are the three copies' columns side by side, centred
    across; centre is one copy's column alone."""

    def __init__(self, cols, rows, cell_w, cell_h, aspect, gap=2, copies=COPIES):
        self.cols, self.rows = cols, rows
        r = max(1, rows)
        while r > 1 and copies * self.width(r, cell_w, cell_h, aspect) + (copies - 1) * gap > cols - 2:
            r -= 1
        self.r = r
        self.c = max(1, min(cols, self.width(r, cell_w, cell_h, aspect)))
        self.top = 1 + max(0, (rows - r) // 2)
        span = copies * self.c + (copies - 1) * gap
        left = 1 + max(0, (cols - span) // 2)
        self.slots = [left + k * (self.c + gap) for k in range(copies)]
        self.centre = 1 + max(0, (cols - self.c) // 2)

    @staticmethod
    def width(r, cell_w, cell_h, aspect):
        return max(1, round(r * cell_h * aspect / cell_w))

    def clamp(self, x):
        return max(1, min(self.cols - self.c + 1, x))


SPRING = 2.5 * math.pi  # the spring's frequency over a move: cos(2.5 pi) = 0 lands it on 1


def overshoot(a):
    """How far past 1 the spring 1 - exp(-a u) cos(SPRING u) first swings:
    its peak, where tan(SPRING u) = -a / SPRING."""
    b = SPRING
    return math.exp(-a * (math.pi - math.atan(a / b)) / b) * b / math.hypot(a, b)


@functools.lru_cache(maxsize=None)
def damping(bounce):
    """The damping whose first swing passes 1 by bounce, by bisection."""
    lo, hi = 0.0, 100.0
    for _ in range(60):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if overshoot(mid) > bounce else (lo, mid)
    return hi


def ease(u, bounce):
    """0 at u=0 to 1 at u=1. With bounce above 0, a damped spring that
    swings past 1 by bounce, comes back a little short, and lands exactly on
    1 at u=1; with 0, ease-out."""
    u = max(0.0, min(1.0, u))
    if bounce <= 0:
        return 1 - (1 - u) ** 3
    return 1 - math.exp(-damping(min(bounce, 0.9)) * u) * math.cos(SPRING * u)


class Choreography:
    """Act 7's moves, in seconds from the start of the act: one copy in the
    middle; at slide it moves left to the first slot; at copy2 a second
    copy slides out from behind it to the second slot, and at copy3 a third
    from behind that to the third. Each move takes move seconds and
    overshoots by bounce of its distance before it settles. A negative time
    never comes. copies, when set, holds that many copies still, side by
    side (one in the middle), for measuring."""

    def __init__(self, slide=-1.0, copy2=-1.0, copy3=-1.0, move=1.2, bounce=0.12, copies=0):
        self.slide, self.copy2, self.copy3 = slide, copy2, copy3
        self.move, self.bounce, self.copies = move, bounce, copies

    def _at(self, lay, start, a, b, t):
        if start < 0 or t < start:
            return a
        u = (t - start) / self.move if self.move > 0 else 1.0
        return lay.clamp(round(a + (b - a) * ease(u, self.bounce)))

    def positions(self, lay, t):
        """[(placement id, column)] of the copies showing at t, front first."""
        if self.copies:
            if self.copies == 1:
                return [(1, lay.centre)]
            return [(k + 1, lay.slots[k]) for k in range(min(self.copies, len(lay.slots)))]
        s = lay.slots
        pos = [(1, self._at(lay, self.slide, lay.centre, s[0], t))]
        if 0 <= self.copy2 <= t:
            pos.append((2, self._at(lay, self.copy2, s[0], s[1], t)))
        if 0 <= self.copy3 <= t:
            pos.append((3, self._at(lay, self.copy3, s[1], s[2], t)))
        return pos


def placements(image_id, lay, pos):
    """Places image_id at each (placement id, column): copy 1 in front
    (z=3), each later one behind the one before, so a copy sliding out from
    behind another starts hidden."""
    return "".join(cup(lay.top, x) + apc(f"a=p,i={image_id},p={p},c={lay.c},r={lay.r},z={4 - p},C=1,q=2")
                   for p, x in pos)


def delete(image_id):
    """Deletes the image and its placements, and frees its data (d=I)."""
    return apc(f"a=d,d=I,i={image_id},q=2")


# ---------------------------------------------------------------- clock

class Clock:
    """Seconds since the act started. With an mpv IPC socket, the
    soundtrack's own position (time-pos, less start), read every quarter
    second and followed between reads on the monotonic clock, so a frame
    shows when its sound plays; until mpv answers (up to wait seconds), the
    clock holds at 0. Without one, or if mpv never answers, the monotonic
    clock from the first read."""

    def __init__(self, ipc="", start=0.0, wait=3.0, now=time.monotonic):
        self.now_fn = now
        self.start = start
        self.sock = None
        self.buf = b""
        self.offset = None  # time-pos less start, less the monotonic clock
        self.asked = 0.0
        self.waited_until = now() + wait
        self.ipc = ipc
        self.synced = False

    def _connect(self):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(self.ipc)
            s.setblocking(False)
            self.sock = s
        except OSError:
            self.sock = None

    def _poll(self, mono):
        if self.sock is None:
            self._connect()
            if self.sock is None:
                return
        if mono - self.asked >= 0.25:
            self.asked = mono
            try:
                self.sock.send(b'{"command":["get_property","time-pos"],"request_id":75}\n')
            except OSError:
                self.sock = None
                return
        try:
            while True:
                d = self.sock.recv(4096)
                if not d:
                    self.sock = None
                    return
                self.buf += d
        except BlockingIOError:
            pass
        except OSError:
            self.sock = None
            return
        *lines, self.buf = self.buf.split(b"\n")
        for line in lines:
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if msg.get("request_id") != 75 or not isinstance(msg.get("data"), (int, float)):
                continue
            off = msg["data"] - self.start - mono
            if self.offset is None or abs(off - self.offset) > 0.05:
                self.offset = off
            else:
                self.offset += 0.2 * (off - self.offset)
            self.synced = True

    def now(self):
        mono = self.now_fn()
        if self.ipc:
            self._poll(mono)
        if self.offset is None:
            if self.ipc and mono < self.waited_until:
                return 0.0
            self.offset = -mono
        return mono + self.offset


# --------------------------------------------------------------- player

class Stats:
    def __init__(self):
        self.frames = self.dropped = self.moves = self.bytes = 0
        self.encode = self.write = 0.0
        self.t0 = time.monotonic()

    def line(self, name):
        dt = max(1e-6, time.monotonic() - self.t0)
        n = max(1, self.frames)
        return (f"{name}: {self.frames} frames in {dt:.1f} s = {self.frames / dt:.1f} fps, "
                f"{self.dropped} dropped, {self.moves} moves, {self.bytes // n} bytes per frame, "
                f"encode {self.encode / n * 1000:.1f} ms, write {self.write / n * 1000:.1f} ms a frame")


class Player:
    """Sends frames and moves to write(), from frames(), an iterator of
    decoded frames at fps, on clock(). Kept free of the terminal and ffmpeg
    so the tests drive it with their own."""

    def __init__(self, enc, lay, chor, fps, seconds, write=out, clock=None, unpaced=False):
        self.enc, self.lay, self.chor = enc, lay, chor
        self.fps, self.seconds = fps, seconds
        self.write = write
        self.clock = clock or Clock()
        self.unpaced = unpaced
        self.stats = Stats()
        self.current = None   # the image id showing
        self.pos = None       # the positions it was placed at
        self.finished = False

    def show(self, frame, t):
        n = self.stats.frames
        image_id = IMAGE_IDS[n % 2]
        t0 = time.monotonic()
        tx = self.enc.transmit(image_id, frame)
        t1 = time.monotonic()
        self.pos = self.chor.positions(self.lay, t)
        s = ESC + "7" + tx + placements(image_id, self.lay, self.pos)
        if self.current is not None:
            s += delete(self.current)
        s += ESC + "8"
        self.write(s)
        self.current = image_id
        st = self.stats
        st.frames += 1
        st.bytes += len(s)
        st.encode += t1 - t0
        st.write += time.monotonic() - t1

    def move(self, t):
        pos = self.chor.positions(self.lay, t)
        if self.current is None or pos == self.pos:
            return
        self.pos = pos
        self.write(ESC + "7" + placements(self.current, self.lay, pos) + ESC + "8")
        self.stats.moves += 1

    def relayout(self, lay, t):
        self.lay = lay
        if self.current is not None:
            self.pos = None
            self.move(t)

    def run(self, frames, on_tick=None):
        """Plays until seconds pass or the frames run out. Returns the
        frames shown."""
        index = 0  # the number of the next frame frames() yields
        frames = iter(frames)
        done = False
        while not done:
            t = self.clock.now()
            if self.seconds > 0 and t >= self.seconds:
                break
            if on_tick:
                on_tick(self, t)
            want = index if self.unpaced else int(t * self.fps)
            if want >= index:
                frame = None
                while index <= want:
                    frame = next(frames, None)
                    if frame is None:
                        done = True
                        break
                    if index < want:
                        self.stats.dropped += 1
                    index += 1
                if frame is not None:
                    self.show(frame, t)
                continue
            self.move(t)
            if not self.unpaced:
                nxt = index / self.fps - t
                self.sleep(max(0.0, min(TICK, nxt)))
        self.finish()
        return self.stats.frames

    def sleep(self, sec):
        wait(sec)

    def finish(self):
        """Deletes both image ids, once, however the player stopped."""
        if not self.finished:
            self.finished = True
            self.write("".join(delete(i) for i in IMAGE_IDS))
        self.current = None


def ffprobe(video):
    """(width, height, fps) of the video's first stream; the display aspect
    is width over height."""
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "stream=width,height,r_frame_rate,sample_aspect_ratio", "-of", "json", video],
                       capture_output=True, text=True, check=True)
    s = json.loads(p.stdout)["streams"][0]
    num, den = (int(x) for x in s.get("r_frame_rate", "30/1").split("/"))
    sar = s.get("sample_aspect_ratio", "1:1")
    sn, sd = (int(x) for x in sar.split(":")) if ":" in sar and not sar.startswith("0") else (1, 1)
    return s["width"] * sn / sd, s["height"], num / den if den else 30.0


def decode(video, start, seconds, w, h, pix):
    """ffmpeg's frames of the clip at w x h, raw, from start for seconds."""
    size = w * h * (1 if pix == "gray" else 3)
    cmd = ["ffmpeg", "-v", "error", "-nostdin", "-ss", str(start)]
    if seconds > 0:
        cmd += ["-t", str(seconds)]
    cmd += ["-i", video, "-an", "-vf", f"scale={w}:{h}:flags=area", "-f", "rawvideo", "-pix_fmt", pix, "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
    try:
        while True:
            b = p.stdout.read(size)
            if len(b) < size:
                return
            yield b
    finally:
        p.kill()
        p.wait()


def cmd_kitty(a):
    vw, vh, fps = ffprobe(a.video)
    aspect = vw / vh
    w = a.width
    h = max(2, round(w / aspect / 2) * 2)
    transport = a.transport
    for t in [transport] + [x for x in a.fallback.split(",") if x and x != transport]:
        if t not in ("shm",) or probe(t, a.probe_timeout):
            transport = t
            break
    enc = Encoder(transport, w, h, a.level)

    def layout():
        cols, rows = geom()[:2]
        return Layout(cols, max(1, rows - a.banner_rows), a.cell_w, a.cell_h, aspect, a.gap)

    chor = Choreography(a.slide_at, a.copy2_at, a.copy3_at, a.move, a.bounce, a.copies)
    clock = Clock(a.audio_ipc, a.start) if a.audio_ipc else Clock()
    player = Player(enc, layout(), chor, fps, a.seconds, clock=clock, unpaced=a.unpaced)

    def tick(p, t):
        if resized():
            p.relayout(layout(), t)
        if a.unpaced:
            poll_keys()

    try:
        player.run(decode(a.video, a.start, a.seconds, w, h, enc.pix), on_tick=tick)
    finally:
        player.finish()
        enc.close()
        if a.report:
            with open(a.report, "a") as f:
                clock_name = "mpv" if clock.synced else "monotonic"
                f.write(player.stats.line(f"kitty {transport} {w}x{h} copies={a.copies or 'moves'} clock={clock_name}")
                        + "\n")
