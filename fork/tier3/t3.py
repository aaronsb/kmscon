#!/usr/bin/env python3
"""Tier 3 driver: provision the guest, run the scenario under QEMU, compare.

Stdlib only. QEMU runs headless as the invoking user and is driven over QMP on
a unix socket: keys go in with send-key, screens come back with screendump.
Every VM this script starts is shut down by the PID it was started with.

The GL variant is the exception for captures: once virgl owns the scanout,
QEMU's screendump reports "no surface", so that variant pairs egl-headless
with a VNC server on a unix socket and reads the screen over RFB instead.

Variants run one at a time, or with --jobs N up to N at once, each in its own
worker process. GL variants share the host's render node and never run
together. A lock on fork/build/tier3.lock refuses a second concurrent run.

    t3.py provision                       build fork/vm/base.qcow2 from the pinned image
    t3.py run [--jobs N] [VARIANT ...]    install the package, run the scenario, compare
                                          (default: the 2D variants)
    t3.py bless [VARIANT ...]             copy the last run's captures over the goldens

Paths are relative to the repository root, which is two levels above this file.
"""

import argparse
import base64
import contextlib
import fcntl
import glob
import hashlib
import json
import multiprocessing
import multiprocessing.connection
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t3img  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HERE = os.path.join(ROOT, "fork", "tier3")
GUEST = os.path.join(HERE, "guest")
GOLDEN = os.path.join(HERE, "golden")
VM = os.path.join(ROOT, "fork", "vm")
PKGOUT = os.path.join(ROOT, "fork", "build", "pkg")
RUNDIR = os.path.join(ROOT, "fork", "build", "tier3")
REPORT = os.path.join(ROOT, "fork", "build", "tier3-report.json")
LOCK = os.path.join(ROOT, "fork", "build", "tier3.lock")
BASE = os.path.join(VM, "base.qcow2")

MEMORY = "4096"
CPUS = "4"

# Display variants. Each boots its own guest from the base image; the 2D
# variants draw through drm2d and bbulk, the GL variant through drm3d and
# gltex on virgl.
VARIANTS = {
    "2d-1280x800": {
        "device": "virtio-vga,xres=1280,yres=800,edid=on",
        "display": "none",
        "conf": "kmscon.conf",
        "renderer": "bbulk",
    },
    "2d-7680x2160": {
        # Each 7680x2160 XRGB buffer is 63 MiB; the console, two kmscon
        # instances and their double buffers exceed the 256 MiB default.
        "device": "virtio-vga,xres=7680,yres=2160,edid=on,max_hostmem=1G",
        "display": "none",
        "conf": "kmscon.conf",
        "renderer": "bbulk",
    },
    "gl-1280x800": {
        "device": "virtio-vga-gl,xres=1280,yres=800,edid=on",
        "display": "egl-headless,rendernode=%s"
                   % os.environ.get("T3_RENDERNODE", "/dev/dri/renderD128"),
        "conf": "kmscon-gl.conf",
        "renderer": "gltex",
        "capture": "vnc",
    },
}

# What `t3.py run` runs when no variant is named. gl-1280x800 runs only when
# named (`make tier3-gl`) until its intermittent black screen is resolved:
# aaronsb/kmscon#17.
DEFAULT_VARIANTS = ["2d-1280x800", "2d-7680x2160"]

# Screens drawn by `t3 NAME` in the guest (fork/tier3/guest/screens/NAME.sh).
SCREENS = ["sgr", "boxdraw", "cjk"]
# Every case of a variant: each screen, then the VT switch (a check without a
# golden) and the restore after it (compared with the last screen's golden).
CASES = SCREENS + ["vt-switch", "vt-restore"]


# Every log line carries the seconds since the run started and, inside a
# variant's job, the variant's name, so lines from concurrent variants can be
# told apart and phases timed from the log alone.
_T0 = time.time()
_LABEL = ""

# A unix socket path, with its terminating NUL, must fit sun_path (108 bytes).
SUN_PATH_MAX = 107

# Seconds a QMP command may take before QEMU counts as wedged.
QMP_TIMEOUT = 30
# Seconds a variant's worker may run before it is stopped. Every wait in the
# scenario has its own timeout well inside this; it catches a hang nothing
# else does.
VARIANT_DEADLINE = 1800

# Called with the PID of each QEMU this process starts; a worker reports them
# to the parent, which kills any that outlive the worker.
_on_qemu_start = None

# CSI and OSC sequences, which systemd's colored status lines carry.
_ESCAPES = re.compile(r"\x1b\[[0-9;?!]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")


def log(msg):
    tag = "t3 +%.1fs" % (time.time() - _T0)
    if _LABEL:
        tag += " " + _LABEL
    print("[%s] %s" % (tag, msg), file=sys.stderr, flush=True)


# --- QEMU and QMP -----------------------------------------------------------

class Qemu:
    """One headless QEMU process and its QMP connection."""

    def __init__(self, workdir, disks, device, display="none", net=False,
                 capture="screendump"):
        os.makedirs(workdir, exist_ok=True)
        self.workdir = workdir
        self.capture = capture
        self.sock_path = os.path.join(workdir, "qmp.sock")
        self.vnc_path = os.path.join(workdir, "vnc.sock")
        # ttyS0 is the guest's console. ttyS1 carries the scenario's events
        # (guest/setup), so the driver can wait for a login or a drawn screen.
        self.serial = os.path.join(workdir, "serial.log")
        self.events = os.path.join(workdir, "events.log")
        for p in (self.sock_path, self.vnc_path):
            if len(os.fsencode(p)) > SUN_PATH_MAX:
                raise RuntimeError("socket path is %d bytes, over the %d a unix "
                                   "socket allows; use a checkout with a shorter "
                                   "path: %s" % (len(os.fsencode(p)), SUN_PATH_MAX, p))
        for p in (self.sock_path, self.vnc_path, self.serial, self.events):
            if os.path.exists(p):
                os.unlink(p)
        args = ["qemu-system-x86_64", "-machine", "q35,accel=kvm", "-cpu", "host",
                "-smp", CPUS, "-m", MEMORY, "-no-reboot",
                "-display", display, "-device", device,
                "-qmp", "unix:%s,server=on,wait=off" % self.sock_path,
                "-serial", "file:%s" % self.serial,
                "-serial", "file:%s" % self.events]
        for path, fmt, ro in disks:
            args += ["-drive", "file=%s,if=virtio,format=%s%s"
                     % (path, fmt, ",readonly=on" if ro else "")]
        args += ["-nic", "user,model=virtio-net-pci"] if net else ["-nic", "none"]
        if capture == "vnc":
            args += ["-vnc", "unix:%s" % self.vnc_path]
        log("starting: " + " ".join(args))
        self.qemu_log = open(os.path.join(workdir, "qemu.log"), "w")
        self.proc = subprocess.Popen(args, stdout=self.qemu_log,
                                     stderr=subprocess.STDOUT)
        log("qemu pid %d" % self.proc.pid)
        if _on_qemu_start:
            _on_qemu_start(self.proc.pid)
        self.sock = None
        self.buf = b""
        try:
            self._connect()
        except BaseException:  # a QEMU that cannot be driven is not left running
            self._kill()
            self.qemu_log.close()
            raise

    def _connect(self):
        deadline = time.time() + 30
        while self.sock is None:
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited with %d; see %s"
                                   % (self.proc.returncode, self.qemu_log.name))
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.sock_path)
                self.sock = s
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        # A wedged QEMU makes a QMP command raise instead of blocking forever.
        self.sock.settimeout(QMP_TIMEOUT)
        self._recv()  # greeting
        self.cmd("qmp_capabilities")

    def _recv(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("QMP connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **arguments):
        msg = {"execute": name}
        if arguments:
            msg["arguments"] = arguments
        try:
            self.sock.sendall(json.dumps(msg).encode() + b"\n")
            while True:
                reply = self._recv()
                if "event" not in reply:
                    break
        except TimeoutError:
            raise RuntimeError("QMP %s: no reply within %d s; QEMU pid %d is wedged"
                               % (name, QMP_TIMEOUT, self.proc.pid)) from None
        if "error" in reply:
            raise RuntimeError("QMP %s: %s" % (name, reply["error"]))
        return reply.get("return")

    def keys(self, *qcodes):
        """Press qcodes together, as one chord."""
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes])
        # A fixed pause, kept: QEMU queues each chord's release behind a hold
        # time and drops key events while its bounded input queue is full,
        # and nothing outside QEMU shows how full the queue is. Pacing the
        # chords keeps it short. A dropped key shows on the screen, so it
        # fails the case.
        time.sleep(0.05)

    def type(self, text):
        for ch in text:
            self.keys(*char_qcodes(ch))

    def screendump(self, path):
        """Capture the screen to a PPM at `path` and return it."""
        if self.capture == "vnc":
            img = vnc_capture(self.vnc_path)
            write_ppm(path, img)
            return img
        tmp = path + ".tmp"
        self.cmd("screendump", filename=tmp, format="ppm")
        # screendump returns before the file is complete on some backends.
        for _ in range(100):
            try:
                img = t3img.read(tmp)
                os.replace(tmp, path)
                return img
            except (ValueError, IndexError, OSError):
                time.sleep(0.1)
        raise RuntimeError("screendump did not produce a readable image")

    def wait_serial(self, marker, timeout, log_path=None):
        """Wait for a line holding `marker` on the console log (or on
        `log_path`), terminal escape sequences left out, and return it."""
        log_path = log_path or self.serial
        deadline = time.time() + timeout
        while True:
            with open(log_path, "rb") as f:
                text = _ESCAPES.sub("", f.read().decode(errors="replace"))
            for line in text.splitlines():
                if marker in line:
                    return line.strip()
            if self.proc.poll() is not None or time.time() > deadline:
                break
            time.sleep(0.2)
        raise RuntimeError("no %r within %d s; see %s" % (marker, timeout, rel(log_path)))

    def wait_event(self, marker, timeout):
        """Wait for the guest to report `marker` on its event port."""
        return self.wait_serial(marker, timeout, log_path=self.events)

    def wait_exit(self, timeout):
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            return None

    def _kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()

    def stop(self):
        """Power the guest off, then make sure the process we started is gone."""
        if self.proc.poll() is None:
            try:
                self.cmd("quit")
                quit_sent = True
            except (OSError, RuntimeError):  # QMP is gone or wedged
                quit_sent = False
            if not quit_sent or self.wait_exit(15) is None:
                log("qemu pid %d did not quit; sending SIGTERM" % self.proc.pid)
                self.proc.terminate()
                if self.wait_exit(10) is None:
                    log("qemu pid %d did not exit; sending SIGKILL" % self.proc.pid)
                    self._kill()
        if self.sock:
            self.sock.close()
        self.qemu_log.close()


def _recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(min(n - len(buf), 1 << 20))
        if not chunk:
            raise RuntimeError("VNC connection closed")
        buf += chunk
    return bytes(buf)


def vnc_capture(path):
    """Read one full frame from QEMU's VNC server (RFB 3.8, no auth, raw)."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(path)
    try:
        _recv_exact(s, 12)
        s.sendall(b"RFB 003.008\n")
        types = _recv_exact(s, _recv_exact(s, 1)[0])
        if 1 not in types:
            raise RuntimeError("VNC server offers no unauthenticated access")
        s.sendall(b"\x01")
        if struct.unpack(">I", _recv_exact(s, 4))[0] != 0:
            raise RuntimeError("VNC security handshake failed")
        s.sendall(b"\x01")  # shared
        w, h = struct.unpack(">HH", _recv_exact(s, 4))
        _recv_exact(s, 16)
        _recv_exact(s, struct.unpack(">I", _recv_exact(s, 4))[0])
        # 32 bpp little-endian XRGB, so each pixel arrives as B, G, R, X.
        s.sendall(struct.pack(">B3xBBBBHHHBBB3x", 0, 32, 24, 0, 1,
                              255, 255, 255, 16, 8, 0))
        s.sendall(struct.pack(">BxHi", 2, 1, 0))  # raw encoding only
        s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
        frame = bytearray(w * h * 3)
        covered = 0
        while covered < w * h:
            kind = _recv_exact(s, 1)[0]
            if kind == 2:  # bell
                continue
            if kind == 3:  # cut text
                _recv_exact(s, 3)
                _recv_exact(s, struct.unpack(">I", _recv_exact(s, 4))[0])
                continue
            if kind != 0:
                raise RuntimeError("unexpected VNC message %d" % kind)
            _recv_exact(s, 1)
            (nrects,) = struct.unpack(">H", _recv_exact(s, 2))
            for _ in range(nrects):
                x, y, rw, rh, enc = struct.unpack(">HHHHi", _recv_exact(s, 12))
                if enc != 0:
                    raise RuntimeError("unexpected VNC encoding %d" % enc)
                raw = _recv_exact(s, rw * rh * 4)
                for row in range(rh):
                    src = raw[row * rw * 4:(row + 1) * rw * 4]
                    line = bytearray(rw * 3)
                    line[0::3], line[1::3], line[2::3] = src[2::4], src[1::4], src[0::4]
                    off = ((y + row) * w + x) * 3
                    frame[off:off + rw * 3] = line
                covered += rw * rh
        return t3img.Image(w, h, bytes(frame))
    finally:
        s.close()


def write_ppm(path, img):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (img.width, img.height))
        f.write(img.data)


_SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
            '*': '8', '(': '9', ')': '0', '_': 'minus', '+': 'equal',
            '{': 'bracket_left', '}': 'bracket_right', '|': 'backslash',
            ':': 'semicolon', '"': 'apostrophe', '~': 'grave_accent',
            '<': 'comma', '>': 'dot', '?': 'slash'}
_PLAIN = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal',
          '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash',
          ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent',
          ',': 'comma', '.': 'dot', '/': 'slash', '\t': 'tab'}


def char_qcodes(ch):
    """The qcodes that type `ch` on a US layout."""
    if ch.isascii() and (ch.islower() or ch.isdigit()):
        return [ch]
    if ch.isascii() and ch.isupper():
        return ["shift", ch.lower()]
    if ch in _PLAIN:
        return [_PLAIN[ch]]
    if ch in _SHIFTED:
        return ["shift", _SHIFTED[ch]]
    raise ValueError("cannot type %r" % ch)


def wait_stable(vm, path, settle=1.0, timeout=60, want=None):
    """Capture until two captures `settle` seconds apart are identical.

    `want`, if given, is a predicate the stable image must also satisfy.
    Returns the stable image, saved at `path`.
    """
    deadline = time.time() + timeout
    last = None
    while True:
        img = vm.screendump(path)
        digest = hashlib.sha256(img.data).digest()
        if digest == last and (want is None or want(img)):
            return img
        if time.time() > deadline:
            if digest == last:
                raise RuntimeError("screen settled but not in the expected state; "
                                   "last capture at %s" % rel(path))
            raise RuntimeError("screen did not settle; last capture at %s" % rel(path))
        last = digest
        time.sleep(settle)


def not_blank(img):
    return img.data.count(img.data[:3]) * 3 != len(img.data)


def make_iso(srcdir, label, out):
    # xorriso prints a banner even with -quiet; it is kept out of the log,
    # whose lines each name their variant, and shown only on failure.
    r = subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-V", label, "-J", "-r",
                        "-o", out, srcdir], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("xorriso exited with %d: %s" % (r.returncode, r.stderr.strip()))


# --- provisioning -----------------------------------------------------------

def provision(args):
    with run_lock():
        return _provision(args)


def _provision(args):
    pristine = os.path.join(VM, "images", args.image)
    work = os.path.join(VM, "provision")
    shutil.rmtree(work, ignore_errors=True)
    seed = os.path.join(work, "seed")
    os.makedirs(seed)

    def b64(name):
        with open(os.path.join(GUEST, name), "rb") as f:
            return base64.b64encode(f.read()).decode()

    with open(os.path.join(GUEST, "user-data.in")) as f:
        ud = f.read()
    ud = (ud.replace("@ARCHIVE@", args.archive)
            .replace("@INSTALL_SCRIPT@", b64("tier3-install"))
            .replace("@INSTALL_UNIT@", b64("tier3-install.service")))
    with open(os.path.join(seed, "user-data"), "w") as f:
        f.write(ud)
    shutil.copy(os.path.join(GUEST, "meta-data"), seed)
    iso = os.path.join(work, "seed.iso")
    make_iso(seed, "cidata", iso)

    tmp = BASE + ".tmp"
    if os.path.exists(tmp):
        os.unlink(tmp)
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "qcow2",
                    "-b", os.path.abspath(pristine), tmp, "16G"], check=True)
    vm = Qemu(work, [(tmp, "qcow2", False), (iso, "raw", True)],
              "virtio-vga", net=True)
    try:
        vm.wait_serial("TIER3-PROVISION-OK", timeout=1200)
        log("provisioned; waiting for poweroff")
        if vm.wait_exit(300) is None:
            raise RuntimeError("guest did not power off after provisioning")
    finally:
        vm.stop()
    os.replace(tmp, BASE)
    log("base image ready: %s" % BASE)
    return 0


# --- the scenario -----------------------------------------------------------

def newest_packages():
    """The newest build of each of the two split packages. A run resolves
    them once, so every variant installs the same build."""
    pkgs = sorted(glob.glob(os.path.join(PKGOUT, "kmscon-aaronsb-*.pkg.tar.zst")),
                  key=os.path.getmtime)
    if not pkgs:
        raise SystemExit("no package in %s; run `make pkg`" % rel(PKGOUT))
    newest = {}
    for p in pkgs:
        name = "terminfo" if "-terminfo-" in os.path.basename(p) else "main"
        newest[name] = p
    return sorted(newest.values())


def build_pkg_disc(variant, work, pkgs):
    disc = os.path.join(work, "disc")
    shutil.rmtree(disc, ignore_errors=True)
    os.makedirs(os.path.join(disc, "pkg"))
    for p in pkgs:
        shutil.copy(p, os.path.join(disc, "pkg"))
    shutil.copy(os.path.join(GUEST, VARIANTS[variant]["conf"]),
                os.path.join(disc, "kmscon.conf"))
    shutil.copytree(os.path.join(GUEST, "screens"), os.path.join(disc, "screens"))
    shutil.copy(os.path.join(GUEST, "t3"), disc)
    shutil.copy(os.path.join(GUEST, "setup"), disc)
    iso = os.path.join(work, "t3pkg.iso")
    make_iso(disc, "T3PKG", iso)
    return iso


def kmscon_displays(serial):
    """kmscon's "Display ... with backend [..] text renderer [..]" log lines."""
    with open(serial, "rb") as f:
        text = f.read().decode(errors="replace")
    return sorted({line[line.index("Display "):].strip()
                   for line in text.splitlines()
                   if "Display " in line and "text renderer [" in line})


def run_variant(variant, timing, pkgs):
    """Boot one variant with the packages `pkgs` and drive the scenario.

    Returns ({case: capture path}, metadata). A scenario that fails part way
    keeps the captures taken before the failure; metadata carries the error,
    and per-case errors under "errors". If kmscon drew with a renderer other
    than the one the variant tests, no capture is returned: they show the
    wrong path. `timing` gets the seconds from the start of the variant to the
    end of each phase.

    Each step waits for state it can observe, never for a fixed time: lines
    on the console, events the guest reports on its event port (guest/setup),
    and a screen that has changed and then settled. Every wait has a timeout
    that fails the case. On a slow host a capture taken too early can fail
    its case; it cannot pass it.
    """
    start = time.time()

    def phase(name):
        timing[name] = round(time.time() - start, 1)
        log("phase %s done at %.1f s" % (name, timing[name]))

    work = os.path.join(RUNDIR, variant)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    iso = build_pkg_disc(variant, work, pkgs)
    disk = os.path.join(work, "run.qcow2")
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "qcow2",
                    "-b", BASE, disk], check=True)
    v = VARIANTS[variant]
    vm = Qemu(work, [(disk, "qcow2", False), (iso, "raw", True)],
              v["device"], display=v["display"],
              capture=v.get("capture", "screendump"))
    caps, errors, error = {}, {}, None
    try:
        log(vm.wait_serial("TIER3-INSTALL-OK", timeout=180))
        phase("install")
        # The VT switch spawns kmscon only once logind is up and the boot
        # has reached its login prompts; systemd reports both on the console.
        vm.wait_serial("Started User Login Management", timeout=120)
        vm.wait_serial("Reached target Login Prompts", timeout=120)
        phase("boot")
        # tty1 runs getty; switching to tty2 makes logind spawn autovt@tty2,
        # which is kmsconvt@tty2. Nothing starts it earlier: a boot without
        # the keypress has no kmscon on tty2.
        vm.keys("ctrl", "alt", "f2")
        vm.wait_serial("vt: using tty /dev/tty2", timeout=60)
        tmp = os.path.join(work, "wait.ppm")
        prompt = wait_stable(vm, tmp, settle=2.0, timeout=90, want=not_blank)
        phase("kmscon")
        vm.type("tester\n")
        # The user name echoes; then login asks for the password.
        wait_stable(vm, tmp, want=lambda img: img.data != prompt.data)
        vm.type("tester\n")
        try:
            vm.wait_event("T3-LOGIN", timeout=20)
        except RuntimeError:
            # PAM discards typed-ahead input when it turns echo off for the
            # password, so a password typed before the prompt is lost. Typing
            # it again is harmless otherwise: the shell runs it as a command,
            # and every screen clears before it draws.
            log("no login yet; typing the password again")
            vm.type("tester\n")
            vm.wait_event("T3-LOGIN", timeout=60)
        last = wait_stable(vm, tmp)
        phase("login")
        for name in SCREENS:
            vm.type("t3 %s\n" % name)
            vm.wait_event("T3-DONE %s" % name, timeout=60)
            path = os.path.join(work, name + ".ppm")
            # Each screen differs from the one before it, so a capture of the
            # previous screen is never taken for this one.
            last = wait_stable(vm, path, want=lambda img, prev=last: img.data != prev.data)
            caps[name] = path
            phase(name)
        # VT switch away and back. vt-switch: logind starts a second kmscon
        # on tty3, which must draw a screen (its login prompt). vt-restore:
        # back on tty2, the screen must be exactly as it was left. A switch
        # that does not visibly happen fails vt-switch, so vt-restore cannot
        # pass alone without testing anything.
        vm.keys("ctrl", "alt", "f3")
        path = os.path.join(work, "vt-switch.ppm")
        try:
            vm.wait_serial("vt: using tty /dev/tty3", timeout=60)
            wait_stable(vm, path, settle=2.0, timeout=60,
                        want=lambda img: not_blank(img) and img.data != last.data)
            caps["vt-switch"] = path
        except RuntimeError as e:
            errors["vt-switch"] = "no new kmscon screen on tty3: %s" % e
        phase("vt-switch")
        # The restored screen must differ from the one shown before the
        # switch back, so tty3's screen, drawn or blank, is never taken for
        # tty2's. A switch to tty3 that never happened fails vt-restore too.
        away = vm.screendump(tmp)
        vm.keys("ctrl", "alt", "f2")
        path = os.path.join(work, "vt-restore.ppm")
        wait_stable(vm, path, want=lambda img: img.data != away.data)
        caps["vt-restore"] = path
        phase("vt-restore")
    except Exception as e:  # reported per case; the captures so far still count
        error = str(e)
        log(error)
    finally:
        vm.stop()
        if os.path.exists(disk):
            os.unlink(disk)
    displays = kmscon_displays(vm.serial)
    meta = {"packages": [os.path.basename(p) for p in pkgs], "kmscon": displays,
            "errors": errors}
    want = "text renderer [%s]" % v["renderer"]
    if not displays or not all(want in d for d in displays):
        caps = {}
        error = "kmscon did not draw with %s: %s" % (v["renderer"], displays)
    if error:
        meta["error"] = error
    return caps, meta


def golden_for(variant, case):
    """(golden, tolerance sidecar) paths for a case, or (None, None) for
    vt-switch, which is a check without a golden."""
    if case == "vt-switch":
        return None, None
    # vt-restore must reproduce the last screen drawn before the switch.
    base = SCREENS[-1] if case == "vt-restore" else case
    return (os.path.join(GOLDEN, variant, base + ".png"),
            os.path.join(GOLDEN, variant, base + ".tol"))


def write_thumb(img, path):
    t3img.write_png(path, img)
    return rel(path)


def rel(path):
    return os.path.relpath(path, ROOT) if path else None


def evaluate(variant, case, ppm, meta):
    """The report entry for one case, from its capture (or None)."""
    golden, tolpath = golden_for(variant, case)
    stem = os.path.join(RUNDIR, variant, case)
    entry = {"name": "%s/%s" % (variant, case), "golden": rel(golden),
             "actual": None, "diff": None, "tolerance": None,
             "thumb_actual": None, "thumb_golden": None, "thumb_diff": None,
             "stats": {"pixels": 0, "bbox": [], "max_delta": 0}}
    # The golden's thumbnail comes first, so a case that fails without a
    # capture still shows on the contact sheet what it should have drawn.
    gimg = None
    if golden is not None and os.path.exists(golden):
        gimg = t3img.read(golden)
        os.makedirs(os.path.dirname(stem), exist_ok=True)
        entry["thumb_golden"] = write_thumb(t3img.thumbnail(gimg), stem + ".golden.thumb.png")
    try:
        tol = entry["tolerance"] = t3img.load_tol(tolpath)
    except ValueError as e:  # a malformed sidecar fails its case, loudly
        entry["status"], entry["error"] = "fail", str(e)
        return entry
    if ppm is None:
        entry["status"] = "fail"
        entry["error"] = meta.get("errors", {}).get(case) or meta.get("error", "no capture")
        return entry
    img = t3img.read(ppm)
    t3img.write_png(stem + ".png", img)
    entry["actual"] = rel(stem + ".png")
    entry["thumb_actual"] = write_thumb(t3img.thumbnail(img), stem + ".thumb.png")
    if golden is None:  # a check, passed when the capture was taken
        entry["status"] = "pass"
        return entry
    if gimg is None:
        entry["status"] = "missing"
        return entry
    stats, diff, points = t3img.compare(img, gimg, tol["masks"] if tol else ())
    entry["stats"] = stats
    if diff is not None:
        t3img.write_png(stem + ".diff.png", diff)
        entry["diff"] = rel(stem + ".diff.png")
        entry["thumb_diff"] = write_thumb(t3img.diff_thumbnail(img, points),
                                          stem + ".diff.thumb.png")
    entry["status"] = t3img.verdict(stats, tol)
    return entry


def failure_meta(error, pkgs):
    """A variant's metadata when it produced no captures, in the same shape
    as a variant that ran."""
    return {"packages": [os.path.basename(p) for p in pkgs], "kmscon": [],
            "errors": {}, "error": error, "timing": {}}


def failed_variant(variant, error, pkgs):
    log("%s: %s" % (variant, error))
    meta = failure_meta(error, pkgs)
    return [evaluate(variant, case, None, meta) for case in CASES], meta


def variant_job(variant, t0, pkgs):
    """Drive and evaluate one variant: (its case entries, its metadata).

    A variant that cannot run is reported, not fatal: each of its cases fails
    with the error.
    """
    global _T0, _LABEL
    _T0, _LABEL = t0, variant
    timing = {}
    try:
        caps, meta = run_variant(variant, timing, pkgs)
    except Exception as e:
        log(str(e))
        caps, meta = {}, failure_meta(str(e), pkgs)
    start = time.time()
    cases = [evaluate(variant, case, caps.get(case), meta) for case in CASES]
    timing["evaluate"] = round(time.time() - start, 1)
    log("phase evaluate took %.1f s" % timing["evaluate"])
    meta["timing"] = timing
    return cases, meta


def _exit_on_sigterm(signum, frame):
    # The default SIGTERM action skips finally blocks, which stop the VM.
    raise SystemExit("terminated by signal %d" % signum)


def _worker(variant, t0, pkgs, conn):
    """A worker process: reports each QEMU it starts, then its result."""
    global _on_qemu_start
    signal.signal(signal.SIGTERM, _exit_on_sigterm)
    _on_qemu_start = lambda pid: conn.send(("qemu", pid))
    conn.send(("result", variant_job(variant, t0, pkgs)))
    conn.close()


def uses_virgl(variant):
    return VARIANTS[variant]["device"].startswith("virtio-vga-gl")


def kill_leftover_qemu(pids):
    """SIGKILL each QEMU in `pids` that outlived the worker that started it."""
    for pid in pids:
        try:
            with open("/proc/%d/cmdline" % pid, "rb") as f:
                if not f.read().startswith(b"qemu-system"):
                    continue  # gone, or the PID now names another process
            log("killing qemu pid %d, left by its worker" % pid)
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


class Job:
    """One variant's worker process and what it has reported."""

    def __init__(self, ctx, variant, pkgs):
        self.variant = variant
        self.conn, child = ctx.Pipe(duplex=False)
        self.proc = ctx.Process(target=_worker, args=(variant, _T0, pkgs, child),
                                name="t3-" + variant)
        self.proc.start()
        child.close()
        self.start = time.time()
        self.qemu = []
        self.result = None

    def drain(self):
        try:
            while self.conn.poll():
                kind, value = self.conn.recv()
                if kind == "qemu":
                    self.qemu.append(value)
                else:
                    self.result = value
        except (EOFError, OSError):  # the worker is gone
            pass

    def stop(self):
        """Stop an overdue worker: SIGTERM lets it stop its VM first."""
        self.proc.terminate()
        self.proc.join(60)
        if self.proc.is_alive():
            self.proc.kill()

    def finish(self):
        self.proc.join()
        self.drain()
        self.conn.close()
        kill_leftover_qemu(self.qemu)


def run_concurrently(variants, jobs, pkgs):
    """{variant: variant_job's result}, at most `jobs` variants at once.

    Each variant runs in its own process, so one variant's compare (pure
    Python, holding the GIL) never stalls another's captures, and a worker
    that dies fails its own variant only. Variants on virgl share the host's
    render node, so at most one of them runs at a time. They start in the
    requested order.
    """
    ctx = multiprocessing.get_context("spawn")
    results, pending, running = {}, list(variants), []
    while pending or running:
        for variant in list(pending):
            if len(running) >= jobs:
                break
            if uses_virgl(variant) and any(uses_virgl(j.variant) for j in running):
                continue
            pending.remove(variant)
            running.append(Job(ctx, variant, pkgs))
        multiprocessing.connection.wait(
            [j.conn for j in running] + [j.proc.sentinel for j in running], timeout=5)
        for job in list(running):
            alive = job.proc.is_alive()
            job.drain()
            overdue = alive and time.time() - job.start > VARIANT_DEADLINE
            if alive and not overdue:
                continue
            if overdue:
                job.stop()
            job.finish()
            running.remove(job)
            if job.result is not None:
                results[job.variant] = job.result
            elif overdue:
                results[job.variant] = failed_variant(
                    job.variant, "no result within %d s; worker stopped" % VARIANT_DEADLINE,
                    pkgs)
            else:
                results[job.variant] = failed_variant(
                    job.variant, "worker exited with code %s before reporting"
                    % job.proc.exitcode, pkgs)
    return results


@contextlib.contextmanager
def run_lock():
    """Hold fork/build/tier3.lock, or exit if another run holds it: two runs
    would share the package, the work directory, the report and the render
    node."""
    os.makedirs(os.path.dirname(LOCK), exist_ok=True)
    with open(LOCK, "a+") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            f.seek(0)
            raise SystemExit("another tier 3 run (pid %s) holds %s"
                             % (f.read().strip() or "unknown", rel(LOCK)))
        f.truncate(0)
        f.write("%d\n" % os.getpid())
        f.flush()
        yield


def run(args):
    if not os.path.exists(BASE):
        raise SystemExit("no base image at %s; run `make tier3-image`" % BASE)
    variants = args.variants or DEFAULT_VARIANTS
    signal.signal(signal.SIGTERM, _exit_on_sigterm)
    with run_lock():
        if args.pkg_cmd:
            log("building the package: %s" % args.pkg_cmd)
            r = subprocess.run(args.pkg_cmd, shell=True)
            if r.returncode:
                raise SystemExit("package build failed with exit code %d" % r.returncode)
        pkgs = newest_packages()
        log("packages: %s" % ", ".join(map(os.path.basename, pkgs)))
        if args.jobs == 1:
            results = {v: variant_job(v, _T0, pkgs) for v in variants}
        else:
            results = run_concurrently(variants, args.jobs, pkgs)
        global _LABEL
        _LABEL = ""
        log("%d variant(s) done, %d at a time" % (len(variants), args.jobs))
        # The report lists variants in the requested order, whatever order
        # they finished in, so a concurrent run's report matches a serial one's.
        cases = [c for v in variants for c in results[v][0]]
        meta = {v: results[v][1] for v in variants}
        report = {"tier": 3, "cases": cases, "variants": meta}
        os.makedirs(os.path.dirname(REPORT), exist_ok=True)
        with open(REPORT, "w") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
    for c in cases:
        print("%-8s %s  (%d px)" % (c["status"], c["name"], c["stats"]["pixels"]))
    print("report: %s" % rel(REPORT))
    # "near" is a difference within the case's reviewed tolerance: it passes.
    return 0 if all(c["status"] in ("pass", "near") for c in cases) else 1


def bless(args):
    with run_lock():
        return _bless(args)


def _bless(args):
    with open(REPORT) as f:
        report = json.load(f)
    for c in report["cases"]:
        variant, case = c["name"].split("/", 1)
        if case not in SCREENS or not c["actual"]:
            continue
        if args.variants and variant not in args.variants:
            continue
        golden, _ = golden_for(variant, case)
        os.makedirs(os.path.dirname(golden), exist_ok=True)
        shutil.copy(os.path.join(ROOT, c["actual"]), golden)
        log("blessed %s" % rel(golden))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("provision")
    p.add_argument("--image", required=True, help="pristine image file in fork/vm/images")
    p.add_argument("--archive", required=True, help="Arch Linux Archive date, YYYY/MM/DD")
    r = sub.add_parser("run")
    r.add_argument("--jobs", type=int, default=1, metavar="N",
                   help="run up to N variants at once (default 1)")
    r.add_argument("--pkg-cmd", metavar="CMD",
                   help="shell command that builds the package, run under the lock first")
    r.add_argument("variants", nargs="*", help="any of: " + ", ".join(VARIANTS))
    b = sub.add_parser("bless")
    b.add_argument("variants", nargs="*")
    args = ap.parse_args(argv)
    unknown = set(getattr(args, "variants", [])) - set(VARIANTS)
    if unknown:
        ap.error("unknown variant(s): " + ", ".join(sorted(unknown)))
    named = getattr(args, "variants", [])
    if len(set(named)) != len(named):
        ap.error("a variant is named twice; each has one work directory")
    if getattr(args, "jobs", 1) < 1:
        ap.error("--jobs must be at least 1")
    return {"provision": provision, "run": run, "bless": bless}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
