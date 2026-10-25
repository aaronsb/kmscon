"""Tier 3's variants and the scenario each one runs: install the package,
log in, draw the screens, switch VTs, then evaluate every case."""

import glob
import os
import re
import shutil
import subprocess
import time

import t3_bench
from t3_common import BASE, GUEST, RUNDIR, rel
from t3_evaluate import BENCHMARKS, CASES, KEYED, KEYS_AFTER, SCREENS, evaluate
from t3_qemu import Qemu, make_iso, not_blank, wait_stable

# Display variants. Each boots its own guest from the base image, with its
# own vCPUs and memory; the 2D variants draw through drm2d and bbulk, the GL
# variant through drm3d and gltex on virgl. Each draws every screen, the image
# ones included (t3_evaluate.IMAGE_SCREENS).
VARIANTS = {
    "2d-1280x800": {
        "device": "virtio-vga,xres=1280,yres=800,edid=on",
        "display": "none",
        "conf": "kmscon.conf",
        "renderer": "bbulk",
        "cpus": 4,
        "memory_mb": 4096,
    },
    # ADR-103's small machine: one vCPU and 512 MB at a common laptop size.
    # The vCPU is a core of the host, so it bounds parallelism and memory
    # rather than reproducing slow hardware.
    "2d-1366x768-potato": {
        "device": "virtio-vga,xres=1366,yres=768,edid=on",
        "display": "none",
        "conf": "kmscon.conf",
        "renderer": "bbulk",
        "cpus": 1,
        "memory_mb": 512,
    },
    "2d-7680x2160": {
        # Each 7680x2160 XRGB buffer is 63 MiB; the console, two kmscon
        # instances and their double buffers exceed the 256 MiB default.
        "device": "virtio-vga,xres=7680,yres=2160,edid=on,max_hostmem=1G",
        "display": "none",
        "conf": "kmscon.conf",
        "renderer": "bbulk",
        "cpus": 4,
        "memory_mb": 4096,
    },
    # The 7680x2160 display with DejaVu Sans Mono at 32 points, a 19x38 cell,
    # near the 20x40 of Monoid at 30 the operator runs: pictures sized from
    # the cell (sixel) are checked at a cell other than the 10x19 of the rest.
    "2d-7680x2160-font32": {
        "device": "virtio-vga,xres=7680,yres=2160,edid=on,max_hostmem=1G",
        "display": "none",
        "conf": "kmscon-font32.conf",
        "renderer": "bbulk",
        "cpus": 4,
        "memory_mb": 4096,
    },
    "gl-1280x800": {
        "device": "virtio-vga-gl,xres=1280,yres=800,edid=on",
        "display": "egl-headless,rendernode=%s"
                   % os.environ.get("T3_RENDERNODE", "/dev/dri/renderD128"),
        "conf": "kmscon-gl.conf",
        "renderer": "gltex",
        "capture": "vnc",
        "cpus": 4,
        "memory_mb": 4096,
    },
}

# What `t3.py run` runs when no variant is named, in the order workers take
# them: with JOBS=2 the longest, 2d-7680x2160, starts at once, and the
# potato and then 2d-7680x2160-font32 follow 2d-1280x800 as workers free.
# gl-1280x800 runs only when named (`make tier3-gl`) until its goldens are
# blessed; its black screen, aaronsb/kmscon#17, was gltex's and is fixed.
DEFAULT_VARIANTS = ["2d-1280x800", "2d-7680x2160", "2d-1366x768-potato",
                    "2d-7680x2160-font32"]

# Seconds a screen may take to draw; a benchmark scrolls for longer.
SCREEN_TIMEOUT = 60
BENCH_TIMEOUT = 300


def uses_virgl(variant):
    return VARIANTS[variant]["device"].startswith("virtio-vga-gl")


def evaluate_variant(variant, caps, meta, log):
    """The report entries of `variant`'s cases from their captures."""
    return [evaluate(variant, case, caps.get(case), meta, log) for case in CASES]


def newest_packages(pkgdir, name):
    """The newest build in `pkgdir` of each of the two split packages, `name`
    and `name`-terminfo. A run resolves them once, so every variant installs
    the same build."""
    pkgs = sorted(glob.glob(os.path.join(pkgdir, name + "-*.pkg.tar.zst")),
                  key=os.path.getmtime)
    newest = {}
    for p in pkgs:
        rest = os.path.basename(p)[len(name) + 1:]
        kind = "terminfo" if rest.startswith("terminfo-") else "main"
        newest[kind] = p
    if "main" not in newest:
        raise SystemExit("no %s package in %s; run its package target" % (name, rel(pkgdir)))
    return sorted(newest.values())


def package_name(path):
    """The package name in a package file's name, NAME-VERSION-REL-ARCH.pkg.tar.zst."""
    return os.path.basename(path).rsplit("-", 3)[0]


def check_install(line, pkgs):
    """The packages and libtsm commit the guest reported installing
    (guest/setup's TIER3-INSTALL-OK line), as metadata. Raises when the
    guest holds a kmscon or libtsm package that is not on the disc, or the
    package was built from another libtsm than the run names, an unknown one,
    or one with changes the run does not declare: each would let one build's
    results stand for another's."""
    m = re.search(r"TIER3-INSTALL-OK packages: (.*?); libtsm: (\S+)", line)
    if not m:
        raise RuntimeError("cannot read the installed packages from %r" % line)
    installed = m.group(1).split()
    names = installed[::2]
    if sorted(names) != sorted(package_name(p) for p in pkgs):
        raise RuntimeError("the guest has %s installed, not only the run's packages %s"
                           % (" ".join(names), " ".join(map(os.path.basename, pkgs))))
    libtsm, want = m.group(2), os.environ.get("T3_LIBTSM_REV") or ""
    if not want:
        raise RuntimeError("the run names no libtsm commit (T3_LIBTSM_REV), so the package's "
                           "cannot be checked; `make build` fetches subprojects/libtsm")
    got_rev, _, got_dirty = libtsm.partition("+")
    want_rev, _, want_dirty = want.partition("+")
    if not re.fullmatch(r"[0-9a-f]{40}", got_rev) or not got_rev.startswith(want_rev) \
            or got_dirty != want_dirty:
        raise RuntimeError("the package was built with libtsm %s, the run names %s" % (libtsm, want))
    return {"packages": [" ".join(installed[i:i + 2]) for i in range(0, len(installed), 2)],
            "libtsm": libtsm}


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


def bench_figures(vm, caps, conf):
    """Each drawn benchmark screen's figures (t3_bench.measure), by name."""
    with open(vm.serial, "rb") as f:
        serial = f.read().decode(errors="replace")
    with open(vm.events, "rb") as f:
        events = f.read().decode(errors="replace").splitlines()
    out = {}
    for name in BENCHMARKS:
        if name in caps:
            line = next((e for e in events if e.startswith("T3-BENCH %s " % name)), None)
            out[name] = t3_bench.measure(serial, line, conf)
    return out


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
              capture=v.get("capture", "screendump"), cpus=v["cpus"], memory_mb=v["memory_mb"])
    caps, errors, error, installed = {}, {}, None, None
    try:
        line = vm.wait_serial("TIER3-INSTALL-OK", timeout=180)
        log(line)
        installed = check_install(line, pkgs)
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
            vm.wait_event("T3-DONE %s" % name,
                          timeout=BENCH_TIMEOUT if name in BENCHMARKS else SCREEN_TIMEOUT)
            path = os.path.join(work, name + ".ppm")
            # Each screen differs from the one before it, so a capture of the
            # previous screen is never taken for this one.
            last = wait_stable(vm, path, want=lambda img, prev=last: img.data != prev.data)
            caps[name] = path
            phase(name)
            # The keyed cases of this screen, each captured once the screen
            # has changed from the capture before it: a key that changes
            # nothing fails its case.
            for screen, case, chords in KEYED:
                if screen != name:
                    continue
                for chord in chords:
                    vm.keys(*chord)
                path = os.path.join(work, case + ".ppm")
                last = wait_stable(vm, path, want=lambda img, prev=last: img.data != prev.data)
                caps[case] = path
                phase(case)
            for chord in KEYS_AFTER.get(name, []):
                vm.keys(*chord)
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
    meta = {"packages": [os.path.basename(p) for p in pkgs], "installed": installed,
            "kmscon": displays, "errors": errors, "device": v["device"], "config": conf,
            "cell": kmscon_cell(vm.serial), "cpus": v["cpus"], "memory_mb": v["memory_mb"],
            "bench": bench_figures(vm, caps, conf)}
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
    return evaluate_variant(variant, {}, meta, log), meta


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
    cases = evaluate_variant(variant, caps, meta, log)
    timing["evaluate"] = round(time.time() - start, 1)
    log("phase evaluate took %.1f s" % timing["evaluate"])
    meta["timing"] = timing
    return cases, meta
