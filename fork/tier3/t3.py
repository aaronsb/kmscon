#!/usr/bin/env python3
"""Tier 3 driver: provision the guest, run the scenario under QEMU, compare.

Stdlib only. The work is split across the modules beside this one:

    t3_common.py     paths and the logger
    t3_qemu.py       one headless QEMU: QMP, VNC captures, key mapping
    t3_scenario.py   the variants and the scenario each runs
    t3_evaluate.py   the cases, their screen text and the compare with a golden
    t3_runner.py     the worker pool, the run lock and the report
    t3img.py         PNG/PPM reading and writing and the image compare

Variants run one at a time, or with --jobs N up to N at once, each in its own
worker process.

    t3.py provision                       build fork/vm/base.qcow2 from the pinned image
    t3.py run [--jobs N] [VARIANT ...]    install the package, run the scenario, compare
                                          (default: the 2D variants)
    t3.py run --pkg-dir DIR --pkg-name NAME --compare-only
                                          the same for another build, such as
                                          upstream's (fork/upstream.mk)
    t3.py bless [VARIANT ...]             copy the last run's captures over the goldens

Paths are relative to the repository root, which is two levels above this file.
"""

import argparse
import base64
import json
import os
import shutil
import signal
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t3_runner  # noqa: E402
import t3_scenario  # noqa: E402
from t3_common import BASE, GUEST, PKGOUT, REPORT, ROOT, VM, Logger, rel  # noqa: E402
from t3_evaluate import SAME_AS, golden_for  # noqa: E402
from t3_qemu import Qemu, make_iso  # noqa: E402


# --- provisioning -----------------------------------------------------------

def provision(args, log):
    with t3_runner.run_lock():
        return _provision(args, log)


def _provision(args, log):
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
              "virtio-vga", log, net=True)
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


# --- a run ------------------------------------------------------------------

def run(args, log):
    if not os.path.exists(BASE):
        raise SystemExit("no base image at %s; run `make tier3-image`" % BASE)
    variants = args.variants or t3_scenario.DEFAULT_VARIANTS
    t3_runner.raise_on_sigterm(log)
    with t3_runner.run_lock():
        if args.pkg_target:
            log("building the package: make %s" % args.pkg_target)
            r = subprocess.run(["make", "--no-print-directory", args.pkg_target], cwd=ROOT)
            if r.returncode:
                raise SystemExit("package build failed with exit code %d" % r.returncode)
        pkgs = t3_scenario.newest_packages(args.pkg_dir, args.pkg_name)
        log("packages: %s" % ", ".join(map(os.path.basename, pkgs)))
        results, interrupted = t3_runner.run_variants(variants, args.jobs, pkgs,
                                                   args.deadline, log)
        cases = t3_runner.write_report(variants, results)
    for c in cases:
        print("%-8s %s  (%d px)" % (c["status"], c["name"], c["stats"]["pixels"]))
    print("report: %s" % rel(REPORT))
    if interrupted is not None:
        return 130 if isinstance(interrupted, KeyboardInterrupt) else 128 + signal.SIGTERM
    if args.compare_only:
        # Another build measured against the fork's goldens: a difference is
        # what the comparison shows, and only a missing capture fails.
        return 0 if all(c["actual"] for c in cases) else 1
    # "near" is a difference within the case's reviewed tolerance: it passes.
    return 0 if all(c["status"] in ("pass", "near") for c in cases) else 1


def bless(args, log):
    with t3_runner.run_lock():
        return _bless(args, log)


def _bless(args, log):
    with open(REPORT) as f:
        report = json.load(f)
    for c in report["cases"]:
        variant, case = c["name"].split("/", 1)
        # Only a case with a golden of its own: not vt-switch, a check, nor
        # a case compared with another's golden.
        golden, _ = golden_for(variant, case)
        if golden is None or case in SAME_AS or not c["actual"]:
            continue
        if args.variants and variant not in args.variants:
            continue
        os.makedirs(os.path.dirname(golden), exist_ok=True)
        shutil.copy(os.path.join(ROOT, c["actual"]), golden)
        log("blessed %s" % rel(golden))
    return 0


def main(argv=None):
    log = Logger()
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("provision")
    p.add_argument("--image", required=True, help="pristine image file in fork/vm/images")
    p.add_argument("--archive", required=True, help="Arch Linux Archive date, YYYY/MM/DD")
    r = sub.add_parser("run")
    r.add_argument("--jobs", type=int, default=1, metavar="N",
                   help="run up to N variants at once (default 1)")
    r.add_argument("--pkg-target", metavar="TARGET",
                   help="make target that builds the package, run under the lock first")
    r.add_argument("--deadline", type=int, default=t3_runner.VARIANT_DEADLINE, metavar="SECONDS",
                   help="with --jobs above 1, stop a variant still running after this "
                        "(default %d)" % t3_runner.VARIANT_DEADLINE)
    r.add_argument("--pkg-dir", default=PKGOUT, metavar="DIR",
                   help="where the package target puts the packages (default %s)" % rel(PKGOUT))
    r.add_argument("--pkg-name", default="kmscon-aaronsb", metavar="NAME",
                   help="the console's package; its terminfo is NAME-terminfo "
                        "(default kmscon-aaronsb)")
    r.add_argument("--compare-only", action="store_true",
                   help="fail only a case without a capture, for a build the goldens "
                        "do not describe, such as upstream's")
    r.add_argument("variants", nargs="*", help="any of: " + ", ".join(t3_scenario.VARIANTS))
    b = sub.add_parser("bless")
    b.add_argument("variants", nargs="*")
    args = ap.parse_args(argv)
    unknown = set(getattr(args, "variants", [])) - set(t3_scenario.VARIANTS)
    if unknown:
        ap.error("unknown variant(s): " + ", ".join(sorted(unknown)))
    named = getattr(args, "variants", [])
    if len(set(named)) != len(named):
        ap.error("a variant is named twice; each has one work directory")
    if getattr(args, "jobs", 1) < 1:
        ap.error("--jobs must be at least 1")
    return {"provision": provision, "run": run, "bless": bless}[args.cmd](args, log)


if __name__ == "__main__":
    sys.exit(main())
