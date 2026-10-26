#!/usr/bin/env python3
"""Capability probes: what kmscon's terminal does and what it is missing.

  run.py --list FILE --probe EXE [--image-probe EXE] [--build FEATURE=true|false]...
         [--font-probe EXE --fonts DIR]
         [--terminfo SRC --tic PATH --infocmp PATH [--tput PATH]]

Every probe named in FILE (capabilities.list) is run and judged against the
status recorded there:

  works    the probe must pass
  missing  the probe must fail; the line names the issue that tracks the gap

A missing probe that passes fails the run too, so the list is updated in the
change that adds the feature. A working probe may name a build feature in
place of an issue, -Dimages: it must pass in a build with that feature and
fail in one without, where a pass is as red as a missing probe's. --build
says which features this build has; a feature the list names and --build
does not fails the run. A probe one of the programs knows but the list
does not name, or a name in the list no program knows, fails the run.

Probes come from four places: test_capabilities (libtsm, through its public
API), test_capabilities_images (sixel, kitty graphics, XTVERSION and XTGETTCAP
through kmscon's own configuration and vte), test_capabilities_font (the
freetype backend) and the terminfo entry, compiled with tic and read back with
infocmp, for the capabilities listed in TERMINFO below, and expanded with
tput, for the strings listed in TERMINFO_STRINGS. A source that was not built or has no tool is reported as
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
    "terminfo-truecolor": [{"RGB"}, {"Tc"}],
    "terminfo-sync": [{"Sync"}],
    "terminfo-focus": [{"fe", "fd"}],
    "terminfo-clipboard": [{"Ms"}],
}

# probe -> [(capability, parameters, the bytes tput must print)]
TERMINFO_STRINGS = {
    # no empty final parameter before 'm': xterm reads one as SGR 0
    "terminfo-setrgb": [
        ("setrgbf", ("17", "34", "51"), "\033[38;2;17;34;51m"),
        ("setrgbb", ("68", "85", "102"), "\033[48;2;68;85;102m"),
    ],
    # ED 3, which clear(1) sends after clear when the entry has it
    "terminfo-e3": [("E3", (), "\033[3J")],
    "terminfo-italic": [("sitm", (), "\033[3m"), ("ritm", (), "\033[23m")],
    "terminfo-dim": [("dim", (), "\033[2m")],
    "terminfo-strike": [("smxx", (), "\033[9m"), ("rmxx", (), "\033[29m")],
    "terminfo-styled-underline": [
        ("Smulx", ("3",), "\033[4:3m"),
        ("Smulx", ("0",), "\033[4:0m"),
    ],
    # one colour packed as r*65536 + g*256 + b, written in T.416's colon form
    "terminfo-underline-colour": [
        ("Setulc", ("1122867",), "\033[58:2::17:34:51m"),
        ("Setulc", ("16777215",), "\033[58:2::255:255:255m"),
    ],
    "terminfo-bracketed-paste": [
        ("BE", (), "\033[?2004h"),
        ("BD", (), "\033[?2004l"),
        ("PS", (), "\033[200~"),
        ("PE", (), "\033[201~"),
    ],
    # ncurses decodes SGR reports when XM names 1006, and matches them by
    # kmous: XM with kmscon's old kmous, X10's CSI M, loses every click
    "terminfo-sgr-mouse": [
        ("XM", ("1",), "\033[?1006;1000h"),
        ("XM", ("0",), "\033[?1006;1000l"),
        ("kmous", (), "\033[<"),
    ],
}

# test_capabilities_font's probes, named here so that a build without the
# freetype backend skips exactly these and still fails on a misspelt line
FONT_PROBES = ("font-freetype-italic", "font-freetype-synthetic-bold")

STATUSES = ("works", "missing")
# #N is an aaronsb/kmscon issue, libtsm#N an aaronsb/libtsm one
ISSUE = re.compile(r"(libtsm)?#[0-9]+")
# a meson feature a working probe needs (meson.options)
FEATURE = re.compile(r"-D([a-z_]+)")


def read_list(path):
    """[(name, status, issue, note, line number)] from capabilities.list."""
    entries = []
    seen = {}
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
            if status == "works" and issue != "-" and not FEATURE.fullmatch(issue):
                sys.exit(f"{path}:{n}: a working probe has no issue (-), or names the "
                         f"build feature it needs (-Dname)")
            if name in seen:
                sys.exit(f"{path}:{n}: {name} is already listed on line {seen[name]}")
            seen[name] = n
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


def read_terminfo(src, tic, infocmp, tput):
    """(capability names, {(capability, parameters): tput's CompletedProcess})
    of the entry compiled from @src; the second is empty without @tput."""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([tic, "-x", "-o", tmp, src], check=True, capture_output=True)
        out = subprocess.run([infocmp, "-x", "-1", "-A", tmp, "kmscon"], check=True,
                             capture_output=True, text=True).stdout
        strings = {}
        if tput:
            env = dict(os.environ, TERMINFO=tmp)
            for checks in TERMINFO_STRINGS.values():
                for cap, params, _ in checks:
                    strings[cap, params] = subprocess.run(
                        [tput, "-T", "kmscon", cap, *params], env=env,
                        capture_output=True, text=True)
    caps = set()
    for line in out.splitlines():
        if not line.startswith("\t"):
            continue
        cap = line.strip().rstrip(",")
        caps.add(cap.split("=", 1)[0].split("#", 1)[0].split("@", 1)[0])
    return caps, strings


def run_terminfo(caps, name):
    for alt in TERMINFO[name]:
        if alt <= caps:
            return True, ""
    want = " or ".join(",".join(sorted(alt)) for alt in TERMINFO[name])
    return False, f"the kmscon entry lacks {want}"


# tput's exits for a capability the entry does not define: 1 for a standard
# one, 4 for an extended one, whose name is unknown unless the entry has it
TPUT_UNDEFINED = (1, 4)


def run_terminfo_strings(strings, name):
    for cap, params, want in TERMINFO_STRINGS[name]:
        res = strings[cap, params]
        call = " ".join(("tput", cap) + params)
        if res.returncode in TPUT_UNDEFINED:
            return False, f"the kmscon entry lacks {cap}"
        if res.returncode != 0:
            return None, f"{call} exited {res.returncode}: {res.stderr.strip()}"
        if res.stdout != want:
            return False, f"{call} prints {res.stdout!r}, want {want!r}"
    return True, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", required=True)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--image-probe")
    ap.add_argument("--build", action="append", default=[])
    ap.add_argument("--font-probe")
    ap.add_argument("--fonts")
    ap.add_argument("--terminfo")
    ap.add_argument("--tic")
    ap.add_argument("--infocmp")
    ap.add_argument("--tput")
    args = ap.parse_args()
    args.list = os.path.normpath(args.list)

    runners = {}  # probe name -> callable returning (passed, reason)
    skipped = {}  # probe name -> why it cannot run here

    probe = [args.probe]
    for name in program_probes(probe):
        runners[name] = lambda name=name: run_program(probe, name)

    if args.image_probe:
        image = [args.image_probe]
        for name in program_probes(image):
            runners[name] = lambda name=name: run_program(image, name)

    build = {}
    for b in args.build:
        feature, _, value = b.partition("=")
        if value not in ("true", "false"):
            sys.exit(f"--build {b}: want FEATURE=true or FEATURE=false")
        build[feature] = value == "true"

    if args.font_probe:
        font = [args.font_probe, args.fonts]
        names = program_probes(font)
        if sorted(names) != sorted(FONT_PROBES):
            sys.exit(f"test_capabilities_font lists {names}; FONT_PROBES in run.py says "
                     f"{list(FONT_PROBES)}")
        for name in names:
            runners[name] = lambda name=name: run_program(font, name)
    else:
        for name in FONT_PROBES:
            skipped[name] = "the freetype font backend is not built"

    if args.terminfo and args.tic and args.infocmp:
        caps, strings = read_terminfo(args.terminfo, args.tic, args.infocmp, args.tput)
        for name in TERMINFO:
            runners[name] = lambda name=name: run_terminfo(caps, name)
        for name in TERMINFO_STRINGS:
            if args.tput:
                runners[name] = lambda name=name: run_terminfo_strings(strings, name)
            else:
                skipped[name] = "tput not found"
    else:
        for name in list(TERMINFO) + list(TERMINFO_STRINGS):
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
            print(f"FAIL {name}: {args.list}:{n} names a probe nothing defines")
            failures += 1
            continue
        feature = FEATURE.fullmatch(issue)
        if feature and feature.group(1) not in build:
            print(f"FAIL {name}: {args.list}:{n} needs {issue}, which --build does not name")
            failures += 1
            continue
        passed, reason = runners[name]()
        if passed is None:
            print(f"FAIL {name}: {reason}")
            failures += 1
        elif feature and not build[feature.group(1)]:
            if passed:
                print(f"FAIL {name}: passes in a build without {issue}")
                failures += 1
            else:
                print(f"ok   {name}: absent, a build without {issue}: {reason}")
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
