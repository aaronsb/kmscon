#!/usr/bin/env python3
"""Capability probes: what kmscon's terminal does and what it is missing.

  run.py --list FILE --probe EXE [--font-probe EXE --fonts DIR]
         [--terminfo SRC --tic PATH --infocmp PATH]

Every probe named in FILE (capabilities.list) is run and judged against the
status recorded there:

  works    the probe must pass
  missing  the probe must fail; the line names the issue that tracks the gap

A missing probe that passes fails the run too, so the list is updated in the
change that adds the feature. A probe one of the programs knows but the list
does not name, or a name in the list no program knows, fails the run.

Probes come from three places: test_capabilities (libtsm, through its public
API), test_capabilities_font (the freetype backend) and the terminfo entry,
compiled with tic and read back with infocmp, for the capabilities listed in
TERMINFO below. A source that was not built or has no tool is reported as
skipped, probe by probe.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

# probe -> alternatives, each a set of capability names that must all be present
TERMINFO = {
    "terminfo-cursor-shape": [{"Ss", "Se"}],
    "terminfo-italic": [{"sitm", "ritm"}],
    "terminfo-dim": [{"dim"}],
    "terminfo-strike": [{"smxx", "rmxx"}],
    "terminfo-styled-underline": [{"Smulx"}],
    "terminfo-underline-colour": [{"Setulc"}],
    "terminfo-truecolor": [{"RGB"}, {"Tc"}],
    "terminfo-bracketed-paste": [{"BD", "BE", "PS", "PE"}],
    "terminfo-sgr-mouse": [{"XM"}],
    "terminfo-sync": [{"Sync"}],
    "terminfo-focus": [{"fe", "fd"}],
    "terminfo-clipboard": [{"Ms"}],
}

STATUSES = ("works", "missing")
# #N is an aaronsb/kmscon issue, libtsm#N an aaronsb/libtsm one
ISSUE = re.compile(r"(libtsm)?#[0-9]+")


def read_list(path):
    """[(name, status, issue, note, line number)] from capabilities.list."""
    entries = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            fields = line.split(None, 3)
            if len(fields) < 3:
                sys.exit(f"{path}:{n}: want NAME STATUS ISSUE NOTE")
            name, status, issue = fields[:3]
            note = fields[3].strip() if len(fields) > 3 else ""
            if status not in STATUSES:
                sys.exit(f"{path}:{n}: status {status!r} is not one of {', '.join(STATUSES)}")
            if status == "missing" and not ISSUE.fullmatch(issue):
                sys.exit(f"{path}:{n}: a missing probe names its issue (#N or libtsm#N)")
            if status == "works" and issue != "-":
                sys.exit(f"{path}:{n}: a working probe has no issue (-)")
            entries.append((name, status, issue, note, n))
    return entries


def program_probes(cmd):
    """The probe names a probe program prints for --list."""
    out = subprocess.run(cmd + ["--list"], capture_output=True, text=True, check=True)
    return out.stdout.split()


def run_program(cmd, name):
    """(passed, reason) for one probe of a probe program."""
    res = subprocess.run(cmd + [name], capture_output=True, text=True)
    if res.returncode not in (0, 1):
        return None, f"probe exited {res.returncode}: {res.stderr.strip()}"
    return res.returncode == 0, res.stdout.strip()


def terminfo_caps(src, tic, infocmp):
    """The capability names of the entry compiled from @src."""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([tic, "-x", "-o", tmp, src], check=True, capture_output=True)
        out = subprocess.run([infocmp, "-x", "-1", "-A", tmp, "kmscon"], check=True,
                             capture_output=True, text=True).stdout
    caps = set()
    for line in out.splitlines():
        if not line.startswith("\t"):
            continue
        cap = line.strip().rstrip(",")
        caps.add(cap.split("=", 1)[0].split("#", 1)[0].split("@", 1)[0])
    return caps


def run_terminfo(caps, name):
    for alt in TERMINFO[name]:
        if alt <= caps:
            return True, ""
    want = " or ".join(",".join(sorted(alt)) for alt in TERMINFO[name])
    return False, f"the kmscon entry lacks {want}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", required=True)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--font-probe")
    ap.add_argument("--fonts")
    ap.add_argument("--terminfo")
    ap.add_argument("--tic")
    ap.add_argument("--infocmp")
    args = ap.parse_args()
    args.list = os.path.normpath(args.list)

    runners = {}  # probe name -> callable returning (passed, reason)
    skipped = {}  # probe name -> why it cannot run here

    probe = [args.probe]
    for name in program_probes(probe):
        runners[name] = lambda name=name: run_program(probe, name)

    if args.font_probe:
        font = [args.font_probe, args.fonts]
        for name in program_probes(font):
            runners[name] = lambda name=name: run_program(font, name)

    if args.terminfo and args.tic and args.infocmp:
        caps = terminfo_caps(args.terminfo, args.tic, args.infocmp)
        for name in TERMINFO:
            runners[name] = lambda name=name: run_terminfo(caps, name)
    else:
        for name in TERMINFO:
            skipped[name] = "tic or infocmp not found"

    entries = read_list(args.list)
    listed = {e[0] for e in entries}
    failures = 0

    for name in sorted(set(runners) - listed):
        print(f"FAIL {name}: a probe the list does not name; add it to {args.list}")
        failures += 1

    for name, status, issue, note, n in entries:
        if name in skipped:
            print(f"skip {name}: {skipped[name]}")
            continue
        if name not in runners:
            if name.startswith("font-") and not args.font_probe:
                print(f"skip {name}: the freetype font backend is not built")
                continue
            print(f"FAIL {name}: {args.list}:{n} names a probe nothing defines")
            failures += 1
            continue
        passed, reason = runners[name]()
        if passed is None:
            print(f"FAIL {name}: {reason}")
            failures += 1
        elif status == "works" and not passed:
            print(f"FAIL {name}: listed as working, but {reason}")
            failures += 1
        elif status == "missing" and passed:
            print(f"FAIL {name}: listed as missing ({issue}) but passes; "
                  f"set it to works in {args.list} and close {issue}")
            failures += 1
        elif status == "works":
            print(f"ok   {name}: works")
        else:
            print(f"ok   {name}: missing, {issue}: {reason}")

    counts = {s: sum(1 for e in entries if e[1] == s) for s in STATUSES}
    print(f"{len(entries)} probes: {counts['works']} work, {counts['missing']} missing; "
          f"{failures} disagree with the list")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
