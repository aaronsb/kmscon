#!/usr/bin/env python3
"""Tier 2 driver (ADR-301): render the screens, compare each with its golden.

  run.py --render EXE --fonts DIR --goldens DIR --out DIR --report FILE
         [--root DIR] [--update]

Runs the render executable into OUT/actual, compares every golden-named case
with pixcmp, writes diffs to OUT/diff and the machine-readable report to
FILE, and exits 1 if any case fails or is missing (a case missing a golden,
or a golden no screen produced). Statuses: pass (identical), near
(different, within the case's NAME.tol sidecar; the run still passes), fail,
missing. With --update the rendered images are
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


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--render", required=True)
    p.add_argument("--fonts", required=True)
    p.add_argument("--goldens", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--report", required=True)
    p.add_argument("--root", default=os.getcwd())
    p.add_argument("--update", action="store_true")
    args = p.parse_args()

    actual_dir = os.path.join(args.out, "actual")
    diff_dir = os.path.join(args.out, "diff")
    for d in (actual_dir, diff_dir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

    r = subprocess.run([args.render, args.fonts, actual_dir])
    if r.returncode != 0:
        print("tier2: renderer exited %d" % r.returncode, file=sys.stderr)
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

    cases = []
    bad = 0
    for name in names:
        golden = os.path.join(args.goldens, name + ".ppm")
        actual = os.path.join(actual_dir, name + ".ppm")
        case = {"name": name, "golden": rel(golden), "actual": rel(actual), "diff": None, "tolerance": None,
                "stats": {"pixels": 0, "bbox": [], "max_delta": 0}}
        if not os.path.exists(golden) or not os.path.exists(actual):
            case["status"] = "missing"
            case["note"] = "no golden" if not os.path.exists(golden) else "screen was not rendered"
            print("MISSING %s: %s" % (name, case["note"]))
        else:
            diff = os.path.join(diff_dir, name + ".ppm")
            tolfile = os.path.join(args.goldens, name + ".tol")
            tol = pixcmp.read_tol(tolfile) if os.path.exists(tolfile) else None
            case["tolerance"] = tol
            res = pixcmp.compare(golden, actual, tol, diff_path=diff)
            case["stats"] = {"pixels": res["pixels"], "bbox": res["bbox"], "max_delta": res["max_delta"]}
            if res["status"] == "identical":
                case["status"] = "pass"
                os.remove(diff)
                print("pass    %s" % name)
            elif res["match"]:
                case["status"] = "near"
                case["diff"] = rel(diff)
                print("near    %s: %d pixel(s) differ, max channel delta %d, within tolerance"
                      % (name, res["pixels"], res["max_delta"]))
            else:
                case["status"] = "fail"
                case["diff"] = rel(diff)
                if res.get("note"):
                    case["note"] = res["note"]
                print("FAIL    %s: %d pixel(s) differ, max channel delta %d, regions %s"
                      % (name, res["pixels"], res["max_delta"], res["bbox"]))
        bad += case["status"] not in ("pass", "near")
        cases.append(case)

    os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
    with open(args.report, "w") as f:
        json.dump({"tier": 2, "cases": cases}, f, indent=2)
        f.write("\n")
    if not cases:
        print("tier2: no cases", file=sys.stderr)
        return 1
    print("tier2: %d case(s), %d not passing" % (len(cases), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
