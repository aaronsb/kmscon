"""Tier 3's variants and the scenario each one runs: install the package,
log in, draw the screens, switch VTs, then evaluate every case."""

import glob
import os
import re
import shutil
import subprocess
import time

from t3_common import BASE, GUEST, PKGOUT, RUNDIR, rel
from t3_evaluate import CASES, SCREENS, evaluate
from t3_qemu import Qemu, make_iso, not_blank, wait_stable

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


def uses_virgl(variant):
    return VARIANTS[variant]["device"].startswith("virtio-vga-gl")


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


def kmscon_cell(serial):
    """The [width, height] of a character cell, from the font engine's
    "cell size WxH" log line, or None when kmscon logged none."""
    with open(serial, "rb") as f:
        m = re.search(rb"cell size (\d+)x(\d+)", f.read())
    return [int(m.group(1)), int(m.group(2))] if m else None


def run_variant(variant, timing, pkgs, log):
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
              v["device"], log, display=v["display"],
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
    with open(os.path.join(GUEST, v["conf"])) as f:
        conf = [line.strip() for line in f if line.strip() and not line.startswith("#")]
    meta = {"packages": [os.path.basename(p) for p in pkgs], "kmscon": displays,
            "errors": errors, "device": v["device"], "config": conf,
            "cell": kmscon_cell(vm.serial)}
    want = "text renderer [%s]" % v["renderer"]
    if not displays or not all(want in d for d in displays):
        caps = {}
        error = "kmscon did not draw with %s: %s" % (v["renderer"], displays)
    if error:
        meta["error"] = error
    return caps, meta


def failure_meta(error, pkgs):
    """A variant's metadata when it produced no captures, in the same shape
    as a variant that ran."""
    return {"packages": [os.path.basename(p) for p in pkgs], "kmscon": [],
            "errors": {}, "error": error, "timing": {}}


def failed_variant(variant, error, pkgs, log):
    log("%s: %s" % (variant, error))
    meta = failure_meta(error, pkgs)
    return [evaluate(variant, case, None, meta, log) for case in CASES], meta


def variant_job(variant, pkgs, log):
    """Drive and evaluate one variant: (its case entries, its metadata).

    `log` is the run's logger; the variant's lines carry its name. A variant
    that cannot run is reported, not fatal: each of its cases fails with the
    error.
    """
    log = log.labelled(variant)
    timing = {}
    try:
        caps, meta = run_variant(variant, timing, pkgs, log)
    except Exception as e:
        log(str(e))
        caps, meta = {}, failure_meta(str(e), pkgs)
    start = time.time()
    cases = [evaluate(variant, case, caps.get(case), meta, log) for case in CASES]
    timing["evaluate"] = round(time.time() - start, 1)
    log("phase evaluate took %.1f s" % timing["evaluate"])
    meta["timing"] = timing
    return cases, meta
