#!/usr/bin/env python3
"""Checks fork/demo/vt-demo.sh, the script behind `make vt-demo`, without a
VT, sudo or logind: the VT on screen is a file, loginctl a script that lists
the sessions each case plants, and run-vt.sh a stand-in that prints kmscon log
lines and appends answers to a feedback file in a temporary directory.

    vt_demo.py --root SOURCE_ROOT
"""

import argparse
import os
import subprocess
import sys
import tempfile

FAKE_LOGINCTL = """#!/bin/sh
# list-sessions prints one line per session; show-session -p VTNr gives its VT.
case $1 in
list-sessions) for s in $SESSIONS; do echo "c${s} 1000 tester seat0"; done ;;
show-session)
	for id; do :; done
	case "$*" in
	*VTNr*) echo "${id#c}" ;;
	*) printf 'tester\\ntty\\n' ;;
	esac ;;
esac
"""

# Appends the lines in $APPEND to the feedback file, prints $LOG, records its
# arguments and exits with $STATUS, as run-vt.sh would around a real demo.
FAKE_RUN_VT = """#!/bin/bash
printf '%s\\n' "$@" >"$ARGS_OUT"
printf '%b' "${LOG:-}"
mkdir -p "${FEEDBACK%/*}"
printf '%b' "${APPEND:-}" >>"$FEEDBACK"
exit "${STATUS:-0}"
"""

HEADER = "time\trun\tlabel\tterm\tsize\tsection\trating\tnote\n"


def line(run, label, section, rating, note=""):
    return f"2026-10-22T10:00:00+00:00\t{run}\t{label}\tlinux\t25x80\t{section}\t{rating}\t{note}\n"


class Case:
    def __init__(self, root, tmp):
        self.root = root
        self.tmp = tmp
        self.loginctl = os.path.join(tmp, "loginctl")
        self.run_vt = os.path.join(tmp, "run-vt.sh")
        for path, text in ((self.loginctl, FAKE_LOGINCTL), (self.run_vt, FAKE_RUN_VT)):
            with open(path, "w") as f:
                f.write(text)
            os.chmod(path, 0o755)
        self.active = os.path.join(tmp, "active")
        self.feedback = os.path.join(tmp, "out", "feedback.tsv")
        self.args_out = os.path.join(tmp, "args")

    def run(self, args, active="tty7", sessions=(), dry=True, **fake):
        with open(self.active, "w") as f:
            f.write(active + "\n")
        env = dict(os.environ)
        env.update(
            VT_DEMO_ACTIVE=self.active,
            VT_DEMO_LOGINCTL=self.loginctl,
            VT_DEMO_SUDO="",
            VT_DEMO_RUN_VT=self.run_vt,
            VT_DEMO_FEEDBACK=self.feedback,
            VT_DEMO_DRY_RUN="1" if dry else "",
            SESSIONS=" ".join(str(s) for s in sessions),
            FEEDBACK=self.feedback,
            ARGS_OUT=self.args_out,
            **fake,
        )
        return subprocess.run(
            ["bash", os.path.join(self.root, "fork/demo/vt-demo.sh"), *args],
            env=env, capture_output=True, text=True, timeout=30,
        )


failures = []


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name)
    if not ok:
        failures.append(name)
        if detail:
            print("      " + detail.replace("\n", "\n      "))


def picked(p):
    """The VT the dry run would hand to run-vt.sh."""
    for l in p.stdout.splitlines():
        if l.startswith("+ "):
            return l.split()[2]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = ap.parse_args().root

    with tempfile.TemporaryDirectory() as tmp:
        c = Case(root, tmp)

        # VT= is honoured, and the banner names it and the way back.
        p = c.run(["3", "--", "--font-size=30"])
        check("VT=3 is used", p.returncode == 0 and picked(p) == "3", p.stdout + p.stderr)
        check("banner names the VT and the desktop's key",
              "Switch to VT 3 now: Ctrl-Alt-F3" in p.stdout
              and "Ctrl-Alt-F7 returns to this desktop" in p.stdout, p.stdout)
        check("kmscon options follow --",
              any(l.endswith("run-vt.sh 3 -- --font-size=30") for l in p.stdout.splitlines()), p.stdout)

        p = c.run(["5"], active="tty5")
        check("VT= on screen is refused",
              p.returncode != 0 and "VT 5 refused: it is on screen" in p.stderr, p.stderr)
        p = c.run(["4"], sessions=[4])
        check("VT= with a session is refused",
              p.returncode != 0 and "VT 4 refused: logind session c4" in p.stderr, p.stderr)
        p = c.run(["13"])
        check("VT= out of range is refused", p.returncode != 0 and "2 to 12" in p.stderr, p.stderr)

        # Without VT=, the first free of 5, 6, 4, 3, 2.
        for active, sessions, want in (
            ("tty7", [], "5"),
            ("tty5", [], "6"),
            ("tty5", [6], "4"),
            ("tty7", [5, 6, 4], "3"),
            ("tty3", [5, 6, 4], "2"),
            ("tty1", [2, 7], "5"),
        ):
            p = c.run([], active=active, sessions=sessions)
            check(f"auto-pick with {active} on screen and sessions on {sessions} -> {want}",
                  p.returncode == 0 and picked(p) == want, p.stdout + p.stderr)

        p = c.run([], active="tty5", sessions=[6, 4, 3, 2])
        want = ["VT 5: it is on screen"] + [f"VT {v}: logind session c{v}" for v in (6, 4, 3, 2)]
        check("all busy fails naming each VT and why",
              p.returncode != 0 and "no free VT" in p.stderr and all(w in p.stderr for w in want),
              p.stderr)

        # A real run against the stand-in: an older dev run already in the
        # file, then this run's answers and a stock run's after them.
        os.makedirs(os.path.dirname(c.feedback), exist_ok=True)
        with open(c.feedback, "w") as f:
            f.write(HEADER + line("20261022T090000-1", "dev build old", "10-sgr", "wrong"))
        append = (line("20261022T100000-42", "dev build new", "10-sgr", "right")
                  + line("20261022T100000-42", "dev build new", "20-underline-styles", "unsure", "frayed")
                  + line("20261022T100500-77", "stock kmscon 10.0.4-1", "10-sgr", "wrong"))
        log = ("drm_shared: test-only atomic commit failed, -22\\n" * 2
               + "vt_timeout: failed to wake up after 20 retries\\n")
        p = c.run(["6", "--", "--font-name=Monoid Nerd Font Mono"], dry=False,
                  APPEND=append.replace("\t", "\\t").replace("\n", "\\n"), LOG=log)
        out = p.stdout
        check("run exits 0", p.returncode == 0, out + p.stderr)
        check("summary is this run's, by its run id",
              "run       20261022T100000-42" in out and "frayed" in out
              and "20261022T100500-77" not in out and "20261022T090000-1" not in out, out)
        check("results file printed", f"results: {c.feedback}" in out, out)
        check("health counts the freeze signature",
              "health: 2 'atomic commit failed', 1 'vt_timeout'" in out, out)
        logs = [l.split(": ", 1)[1] for l in out.splitlines() if l.startswith("kmscon log: ")]
        check("kmscon log kept at the printed path",
              len(logs) == 1 and "vt_timeout" in open(logs[0]).read(), out)
        with open(c.args_out) as f:
            args = f.read().splitlines()
        check("run-vt.sh gets N -- options, spaces kept",
              args == ["6", "--", "--font-name=Monoid Nerd Font Mono"], repr(args))

        p = c.run(["6"], dry=False, STATUS="1", APPEND="")
        check("run-vt.sh failing fails the run",
              p.returncode != 0 and "exited with status 1" in p.stderr, p.stdout + p.stderr)
        p = c.run(["6"], dry=False, APPEND=line("20261022T110000-9", "stock x", "10-sgr", "right")
                  .replace("\t", "\\t").replace("\n", "\\n"))
        check("no dev-build answers fails the run",
              p.returncode != 0 and "no answers from this run" in p.stderr, p.stdout + p.stderr)
        p = c.run(["6"], dry=False, APPEND="")
        check("a clean log reports zero",
              "health: 0 'atomic commit failed', 0 'vt_timeout'" in p.stdout, p.stdout)

    if failures:
        print(f"{len(failures)} failed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
