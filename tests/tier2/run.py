#!/usr/bin/env python3
"""Tier 2 driver (ADR-301): render the screens, compare each with its golden.

  run.py --render EXE --fonts DIR --goldens DIR --out DIR --report FILE
         [--root DIR] [--update]

Runs the render executable into OUT/actual, compares every golden-named case
with pixcmp, writes diffs to OUT/diff and the machine-readable report to
FILE, and exits 1 if any case fails or is missing (a case missing a golden,
or a golden no screen produced). Statuses: pass (identical), near
(different, within the case's NAME.tol sidecar; the run still passes), fail,
missing, error (a case that could not be compared; counts as failing). With --update the rendered images are
copied over the goldens instead and nothing is compared.

Report paths are relative to --root (default: the current directory).
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pixcmp  # noqa: E402


# Packages whose version can move a rendered pixel or a replayed cell.
TOOL_PACKAGES = ("freetype2", "fontconfig", "harfbuzz", "pixman", "gcc", "meson")


def tool_versions():
    """The dev image's pins and package versions, read inside the container.

    Outside the image (no DEV_* variables, no pacman) the fields are null, so a
    report from a bare host says it does not know rather than guessing.
    """
    packages = {}
    for pkg in TOOL_PACKAGES:
        try:
            out = subprocess.run(["pacman", "-Q", pkg], capture_output=True, text=True)
        except OSError:
            break
        if out.returncode == 0:
            packages[pkg] = out.stdout.split()[1]
    return {
        "image_base": os.environ.get("DEV_IMAGE_BASE"),
        "archive_date": os.environ.get("DEV_ARCHIVE_DATE"),
        "packages": packages,
    }


def write_report(path, cases, libtsm=None):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w") as f:
        tools = tool_versions()
        tools["libtsm"] = libtsm
        json.dump({"tier": 2, "tools": tools, "cases": cases}, f, indent=2)
        f.write("\n")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--render")
    p.add_argument("--fonts")
    p.add_argument("--goldens")
    p.add_argument("--out")
    p.add_argument("--report")
    p.add_argument("--libtsm-rev", help="commit of the libtsm the renderer was built against")
    p.add_argument("--root", default=os.getcwd())
    p.add_argument("--update", action="store_true")
    p.add_argument("--skip", metavar="REASON", help="exit 77 (meson: skip) with this reason")
    args = p.parse_args()

    if args.skip:
        print("tier2: SKIPPED: " + args.skip)
        return 77
    for name in ("render", "fonts", "goldens", "out", "report"):
        if not getattr(args, name):
            p.error("--%s is required" % name)

    # A stale report from an earlier run must never stand in for this one.
    if os.path.exists(args.report):
        os.remove(args.report)

    actual_dir = os.path.join(args.out, "actual")
    diff_dir = os.path.join(args.out, "diff")
    for d in (actual_dir, diff_dir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

    cases = []

    def error_case(name, note):
        print("ERROR   %s: %s" % (name, note), file=sys.stderr)
        cases.append({"name": name, "status": "error", "note": note, "golden": None, "actual": None,
                      "diff": None, "tolerance": None, "stats": {"pixels": 0, "bbox": [], "max_delta": 0}})

    try:
        r = subprocess.run([args.render, args.fonts, actual_dir])
        render_rc = r.returncode
    except OSError as e:
        render_rc = -1
        print("tier2: cannot run renderer: %s" % e, file=sys.stderr)
    if render_rc != 0:
        error_case("renderer", "renderer exited %d" % render_rc)
        write_report(args.report, cases, args.libtsm_rev)
        return 1

    rendered = sorted(os.path.basename(f)[:-4] for f in glob.glob(os.path.join(actual_dir, "*.ppm")))

    if args.update:
        os.makedirs(args.goldens, exist_ok=True)
        for name in rendered:
            shutil.copyfile(os.path.join(actual_dir, name + ".ppm"), os.path.join(args.goldens, name + ".ppm"))
        print("tier2: wrote %d golden(s) to %s" % (len(rendered), args.goldens))
        return 0

    golden_names = sorted(os.path.basename(f)[:-4] for f in glob.glob(os.path.join(args.goldens, "*.ppm")))
    names = sorted(set(rendered) | set(golden_names))
    rel = lambda path: os.path.relpath(path, args.root)  # noqa: E731

    for name in names:
        golden = os.path.join(args.goldens, name + ".ppm")
        actual = os.path.join(actual_dir, name + ".ppm")
        case = {"name": name, "golden": rel(golden), "actual": rel(actual), "diff": None, "tolerance": None,
                "stats": {"pixels": 0, "bbox": [], "max_delta": 0}}
        # The screen's caption, which the renderer writes beside its image.
        desc = os.path.join(actual_dir, name + ".txt")
        if os.path.exists(desc):
            with open(desc) as f:
                case["description"] = f.read().strip()
        cases.append(case)
        if not os.path.exists(golden) or not os.path.exists(actual):
            case["status"] = "missing"
            case["note"] = "no golden" if not os.path.exists(golden) else "screen was not rendered"
            print("MISSING %s: %s" % (name, case["note"]))
            continue
        diff = os.path.join(diff_dir, name + ".ppm")
        try:
            tolfile = os.path.join(args.goldens, name + ".tol")
            tol = pixcmp.read_tol(tolfile) if os.path.exists(tolfile) else None
            case["tolerance"] = tol
            res = pixcmp.compare(golden, actual, tol, diff_path=diff)
        except (OSError, ValueError) as e:
            case["status"] = "error"
            case["note"] = str(e)
            print("ERROR   %s: %s" % (name, e), file=sys.stderr)
            continue
        case["stats"] = {"pixels": res["pixels"], "bbox": res["bbox"], "max_delta": res["max_delta"]}
        if res.get("note"):
            case["note"] = res["note"]
        if res["status"] == "identical":
            case["status"] = "pass"
            if os.path.exists(diff):
                os.remove(diff)
            print("pass    %s" % name)
            continue
        # a size mismatch writes no diff image
        case["diff"] = rel(diff) if os.path.exists(diff) else None
        if res["match"]:
            case["status"] = "near"
            print("near    %s: %d pixel(s) differ, max channel delta %d, within tolerance"
                  % (name, res["pixels"], res["max_delta"]))
        else:
            case["status"] = "fail"
            print("FAIL    %s: %d pixel(s) differ, max channel delta %d, regions %s"
                  % (name, res["pixels"], res["max_delta"], res["bbox"]))

    write_report(args.report, cases, args.libtsm_rev)
    bad = sum(c["status"] not in ("pass", "near") for c in cases)
    if not cases:
        print("tier2: no cases", file=sys.stderr)
        return 1
    print("tier2: %d case(s), %d not passing" % (len(cases), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
