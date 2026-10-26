#!/usr/bin/env python3
"""Checks fork/demo/vt-demo.sh, the script behind `make vt-demo`, without a
VT, sudo or logind: the VT on screen is a file, loginctl a script that lists
the sessions each case plants, and run-vt.sh a stand-in that prints kmscon log
lines and appends answers to a feedback file in a temporary directory.

    vt_demo.py --root SOURCE_ROOT
"""

import argparse
import os
import pwd
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile

FAKE_LOGINCTL = """#!/bin/sh
# list-sessions prints one line per session; show-session -p VTNr gives its VT.
# A session in $GONE ends after the list, so show-session fails for it.
case $1 in
list-sessions) for s in $SESSIONS $GONE; do echo "c${s} 1000 tester seat0"; done ;;
show-session)
	for id; do :; done
	case " $GONE " in *" ${id#c} "*) echo "Failed to get session" >&2; exit 1 ;; esac
	case "$*" in
	*VTNr*) echo "${id#c}" ;;
	*) printf 'tester\\ntty\\n' ;;
	esac ;;
esac
"""

# Appends the lines in $APPEND to the feedback file and those in $CHECKS to
# the guided checks' results log, prints $LOG, records its arguments and exits
# with $STATUS, as run-vt.sh would around a real demo or guide.
FAKE_RUN_VT = """#!/bin/bash
printf '%s\\n' "$@" >"$ARGS_OUT"
printf '%b' "${LOG:-}"
mkdir -p "${FEEDBACK%/*}"
printf '%b' "${APPEND:-}" >>"$FEEDBACK"
printf '%b' "${CHECKS:-}" >>"$CHECKLOG"
exit "${STATUS:-0}"
"""

# run-vt.sh's loginctl for a dry run's session checks and its watch of a login
# (RUN_VT_LOGINCTL): list-sessions call k prints word k of $SEQ (the last word
# past the end): - no session, F a failure, N one session on VT N, N:STATE one
# in that state (active by default), and N! (or N:STATE!) one that show-session
# fails for. Each session's id carries its VT and state, which show-session
# reads back.
FAKE_RUN_VT_LOGINCTL = """#!/bin/bash
case $1 in
list-sessions)
	n=$(( $(cat "$COUNT" 2>/dev/null || echo 0) + 1 )); echo "$n" >"$COUNT"
	read -ra seq <<<"$SEQ"; w=${seq[n - 1]:-${seq[-1]}}
	case $w in
	-) ;;
	F) echo "Failed to list sessions" >&2; exit 1 ;;
	*) bad=; [[ $w != *! ]] || bad=_x; w=${w%!}
		vt=${w%%:*}; st=active; [[ $w != *:* ]] || st=${w#*:}; echo "c${vt}_${st}${bad} 1000 tester seat0" ;;
	esac ;;
show-session)
	for id; do :; done; id=${id#c}
	[[ $id != *_x ]] || { echo "Failed to get session" >&2; exit 1; }
	case "$*" in
	*VTNr*) printf 'VTNr=%s\nState=%s\n' "${id%%_*}" "${id#*_}" ;;
	*) printf 'tester\ntty\n' ;;
	esac ;;
esac
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
        self.checklog = os.path.join(tmp, "out", "vt-check.log")
        self.args_out = os.path.join(tmp, "args")

    def run(self, args, active="tty7", sessions=(), dry=True, gone=(), **fake):
        with open(self.active, "w") as f:
            f.write(active + "\n")
        env = dict(os.environ)
        env.update(
            VT_DEMO_ACTIVE=self.active,
            VT_DEMO_LOGINCTL=self.loginctl,
            VT_DEMO_TEST="1",
            VT_DEMO_SUDO="",
            VT_DEMO_RUN_VT=self.run_vt,
            VT_DEMO_FEEDBACK=self.feedback,
            VT_DEMO_CHECKLOG=self.checklog,
            VT_DEMO_DRY_RUN="1" if dry else "",
            SESSIONS=" ".join(str(s) for s in sessions),
            GONE=" ".join(str(s) for s in gone),
            FEEDBACK=self.feedback,
            CHECKLOG=self.checklog,
            ARGS_OUT=self.args_out,
        )
        env.update(fake)
        return subprocess.run(
            ["bash", os.path.join(self.root, "fork/demo/vt-demo.sh"), *args],
            env=env, capture_output=True, text=True, timeout=30, cwd=self.tmp,
        )


failures = []


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name)
    if not ok:
        failures.append(name)
        if detail:
            print("      " + detail.replace("\n", "\n      "))


def read(path):
    with open(path) as f:
        return f.read()


def esc(text):
    """APPEND's form: printf %b turns these escapes back."""
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def picked(p):
    """The VT the dry run would hand to run-vt.sh."""
    for l in p.stdout.splitlines():
        if l.startswith("+ "):
            return l.split()[2]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = os.path.abspath(ap.parse_args().root)

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
        p = c.run(["05"])
        check("VT= with a leading zero is refused", p.returncode != 0 and "not '05'" in p.stderr, p.stderr)

        # make's path: VT and DEMO_ARGS arrive in the environment and are split
        # like shell words, with nothing run or expanded.
        p = c.run([], VT_DEMO_VT="4",
                  VT_DEMO_ARGS="""--font-name="Monoid Nerd Font Mono" --x='a b' c\\ d ; touch planted $HOME""")
        cmd = next((l for l in p.stdout.splitlines() if l.startswith("+ ")), "")
        check("VT_DEMO_VT and VT_DEMO_ARGS are taken from the environment",
              p.returncode == 0 and picked(p) == "4"
              and cmd.endswith("4 -- --font-name=Monoid\\ Nerd\\ Font\\ Mono --x=a\\ b c\\ d \\; touch planted \\$HOME")
              and not os.path.exists(os.path.join(tmp, "planted")), p.stdout + p.stderr)
        p = c.run([], VT_DEMO_ARGS="--font-name=\"Monoid")
        check("DEMO_ARGS with an open quote is refused",
              p.returncode != 0 and "cannot be split" in p.stderr, p.stderr)

        # KIT reaches run-vt.sh as --kit before the VT (#123); without it,
        # run-vt.sh's default, the text kit.
        p = c.run(["6"], VT_DEMO_KIT="sixel")
        cmd = next((l for l in p.stdout.splitlines() if l.startswith("+ ")), "")
        check("VT_DEMO_KIT reaches run-vt.sh before the VT",
              p.returncode == 0 and cmd.endswith("run-vt.sh --kit=sixel 6 --"), p.stdout + p.stderr)
        p = c.run(["6"])
        check("no KIT passes no --kit", p.returncode == 0 and "--kit" not in p.stdout, p.stdout)
        p = c.run(["6"], VT_DEMO_KIT="bogus")
        check("KIT=bogus is refused",
              p.returncode != 0 and "KIT=bogus: text, sixel, kitty, glyphs or all" in p.stderr, p.stderr)

        p = c.run([], active="tty7", sessions=[6], gone=[5])
        check("a session gone between list and query is skipped",
              p.returncode == 0 and picked(p) == "5", p.stdout + p.stderr)

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
                  + line("20261022T100000-42", "dev build new", "30-strike", "skipped")
                  + line("20261022T100500-77", "stock kmscon 10.0.4-1", "10-sgr", "wrong"))
        log = ("drm_shared: test-only atomic commit failed, -22\\n" * 2
               + "vt_timeout: failed to wake up after 20 retries\\n"
               + "main: 1 screens still in the image budget at exit\\n")
        p = c.run(["6", "--", "--font-name=Monoid Nerd Font Mono"], dry=False,
                  APPEND=esc(append), LOG=log)
        out = p.stdout
        check("run exits 0", p.returncode == 0, out + p.stderr)
        check("summary is this run's, by its run id",
              "run       20261022T100000-42" in out and "frayed" in out
              and "20261022T100500-77" not in out and "20261022T090000-1" not in out, out)
        check("results file printed", f"results: {c.feedback}" in out, out)
        check("skipped pages listed apart from unsure, as still to review",
              "30-strike                 skipped\n" in out
              and "\nskipped, still to review\n  30-strike\n" in out and "  20-underline-styles\n" not in out, out)
        check("health counts the freeze signature",
              "health: 2 'atomic commit failed', 1 'vt_timeout'" in out, out)
        check("health counts screens left in the image budget",
              "health: 1 'still in the image budget' lines in kmscon's log: " in out, out)
        logs = [l.split(": ", 1)[1] for l in out.splitlines() if l.startswith("kmscon log: ")]
        check("kmscon log kept at the printed path",
              len(logs) == 1 and "vt_timeout" in read(logs[0]), out)
        with open(c.args_out) as f:
            args = f.read().splitlines()
        check("run-vt.sh gets N -- options, spaces kept",
              args == ["6", "--", "--font-name=Monoid Nerd Font Mono"], repr(args))

        check("the log is new, with a random name and mode 0600",
              len(logs) == 1 and re.search(r"/vt-demo-\d{8}T\d{6}-tty6-\w{6}\.log$", logs[0])
              and stat.S_IMODE(os.stat(logs[0]).st_mode) == 0o600, out)

        p = c.run(["6"], dry=False, STATUS="1", APPEND="")
        check("run-vt.sh failing fails the run",
              p.returncode == 1 and "exited with status 1" in p.stderr, p.stdout + p.stderr)
        p = c.run(["6"], dry=False, STATUS="130", APPEND="")
        check("run-vt.sh's 130 (Ctrl-C) is kept", p.returncode == 130, p.stdout + p.stderr)
        p = c.run(["6"], dry=False, APPEND=esc(line("20261022T110000-9", "stock x", "10-sgr", "right")))
        check("no dev-build answers fails the run, naming the results file",
              p.returncode != 0 and "no answers from this run" in p.stderr
              and f"results: {c.feedback}" in p.stdout, p.stdout + p.stderr)
        p = c.run(["6"], dry=False, APPEND="")
        check("a clean log reports zero",
              "health: 0 'atomic commit failed', 0 'vt_timeout'" in p.stdout
              and "health: 0 'still in the image budget' lines in kmscon's log\n" in p.stdout, p.stdout)

        # The log's directory: created 0700, never a symlink or someone else's.
        new = os.path.join(tmp, "fresh", "feedback.tsv")
        p = c.run(["6"], dry=False, VT_DEMO_FEEDBACK=new, FEEDBACK=new,
                  APPEND=esc(line("20261022T120000-5", "dev build x", "10-sgr", "right")))
        check("a missing log directory is created with mode 0700",
              p.returncode == 0 and stat.S_IMODE(os.stat(os.path.dirname(new)).st_mode) == 0o700,
              p.stdout + p.stderr)
        os.symlink(os.path.join(tmp, "out"), os.path.join(tmp, "link"))
        link = os.path.join(tmp, "link", "feedback.tsv")
        p = c.run(["6"], dry=False, VT_DEMO_FEEDBACK=link, FEEDBACK=link, APPEND="")
        check("a symlinked log directory is refused",
              p.returncode != 0 and "is a symlink, or not a directory of yours" in p.stderr
              , p.stderr)
        if os.getuid() != 0:
            p = c.run(["6"], dry=False, VT_DEMO_FEEDBACK="/usr/feedback.tsv", APPEND="")
            check("a log directory owned by someone else is refused",
                  p.returncode != 0 and "/usr is a symlink, or not a directory of yours" in p.stderr,
                  p.stderr)

        # make vt-login (#207): run-vt.sh --login before the VT, with the
        # kmscon options; a login records answers only if a demo was run in
        # it, so a run without any ends with the health check and status 0.
        p = c.run([], VT_DEMO_LOGIN="1", VT_DEMO_VT="8", VT_DEMO_ARGS="--font-size=30")
        cmd = next((l for l in p.stdout.splitlines() if l.startswith("+ ")), "")
        check("VT_DEMO_LOGIN reaches run-vt.sh as --login before the VT",
              p.returncode == 0 and cmd.endswith("run-vt.sh --login 8 -- --font-size=30"),
              p.stdout + p.stderr)
        p = c.run([], VT_DEMO_LOGIN="1", sessions=[5])
        check("VT_DEMO_LOGIN without VT= picks a free VT",
              p.returncode == 0 and "run-vt.sh --login 6 --" in p.stdout, p.stdout + p.stderr)
        p = c.run(["8"], VT_DEMO_LOGIN="1", sessions=[8])
        check("VT_DEMO_LOGIN on a VT with a session is refused",
              p.returncode != 0 and "VT 8 refused: logind session c8" in p.stderr, p.stderr)
        for extra, why in (({"VT_DEMO_KIT": "sixel"}, "KIT goes with the demo"),
                           ({"VT_DEMO_LOGIN": "maybe"}, "VT_DEMO_LOGIN=maybe")):
            p = c.run(["8"], **{"VT_DEMO_LOGIN": "1", **extra})
            check(f"VT_DEMO_LOGIN with {' '.join(f'{k}={v}' for k, v in extra.items())} is refused",
                  p.returncode != 0 and why in p.stderr, p.stdout + p.stderr)
        p = c.run(["8"], dry=False, VT_DEMO_LOGIN="1", APPEND="")
        with open(c.args_out) as f:
            args = f.read().splitlines()
        check("a login run without answers ends with the health check and status 0",
              p.returncode == 0 and "health: 0" in p.stdout and "results:" not in p.stdout
              and "no answers" not in p.stderr and args == ["--login", "8", "--"],
              p.stdout + p.stderr + repr(args))
        p = c.run(["8"], dry=False, VT_DEMO_LOGIN="1",
                  APPEND=esc(line("20261025T200000-3", "dev build v1", "10-sgr", "wrong", "smeared")))
        check("a login run prints the answers of a demo run in it",
              p.returncode == 0 and "run       20261025T200000-3" in p.stdout and "smeared" in p.stdout
              and "results: " in p.stdout, p.stdout + p.stderr)
        # A login ends at its logout or at Ctrl-C; run-vt.sh's 130 is a normal end.
        p = c.run(["8"], dry=False, VT_DEMO_LOGIN="1", STATUS="130", APPEND="")
        check("a login run ended with Ctrl-C is a normal end",
              p.returncode == 0 and "vt-login: ended with Ctrl-C" in p.stdout
              and "kmscon failed" not in p.stderr, p.stdout + p.stderr)
        p = c.run(["8"], dry=False, VT_DEMO_LOGIN="1", STATUS="1", APPEND="")
        check("a login run whose kmscon failed fails",
              p.returncode == 1 and "exited with status 1" in p.stderr, p.stdout + p.stderr)

        # make vt-check (#238): run-vt.sh --guide with the guide's absolute
        # path, and --steps, before the VT. A guide is named or a path, and is
        # checked here, before sudo; it runs no demo and no login.
        checks = os.path.join(root, "fork/demo/checks")
        guide = os.path.join(checks, "focus-decarm-mouse.guide")

        def plus(p):
            cmd = next((l for l in p.stdout.splitlines() if l.startswith("+ ")), "")
            return shlex.split(cmd[2:]) if cmd else []

        p = c.run([], VT_DEMO_GUIDE="focus-decarm-mouse", VT_DEMO_GUIDE_STEPS="4 5", VT_DEMO_VT="8",
                  VT_DEMO_ARGS="--dpms-timeout=15")
        check("VT_DEMO_GUIDE reaches run-vt.sh as --guide and --steps before the VT",
              p.returncode == 0
              and plus(p)[1:] == [f"--guide={guide}", "--steps=4 5", "8", "--", "--dpms-timeout=15"]
              and f"the answers are read from {c.checklog})\n" in p.stdout, p.stdout + p.stderr)
        p = c.run(["6"], VT_DEMO_GUIDE=os.path.relpath(guide, tmp))
        check("VT_DEMO_GUIDE takes a path, made absolute",
              p.returncode == 0 and plus(p)[1:] == [f"--guide={guide}", "6", "--"], p.stdout + p.stderr)
        bad = os.path.join(tmp, "bad.guide")
        with open(bad, "w") as f:
            f.write("title: t\nstep: s\ndo: Ctrl+Logo+Return\nexpect: e\n")
        for env, why in (({"VT_DEMO_GUIDE": "nosuch"}, "no guide " + os.path.join(checks, "nosuch.guide")
                          + "; the guides in fork/demo/checks: focus-decarm-mouse"),
                         ({"VT_DEMO_GUIDE": bad}, "name the Super (Windows) key, not Logo"),
                         ({"VT_DEMO_GUIDE": "focus-decarm-mouse", "VT_DEMO_GUIDE_STEPS": "4 9"},
                          "'9' is not a step of this guide (1 to 7)"),
                         ({"VT_DEMO_GUIDE_STEPS": "4"}, "GUIDE_STEPS goes with GUIDE"),
                         ({"VT_DEMO_GUIDE": "focus-decarm-mouse", "VT_DEMO_KIT": "sixel"}, "with no KIT and no login"),
                         ({"VT_DEMO_GUIDE": "focus-decarm-mouse", "VT_DEMO_LOGIN": "1"}, "with no KIT and no login")):
            p = c.run(["6"], **env)
            check(f"{' '.join(f'{k}={v}' for k, v in env.items())} is refused",
                  p.returncode != 0 and why in p.stderr and "+ " not in p.stdout, p.stdout + p.stderr)

        # A guided run prints the lines it added to the results log, after
        # the health check, and fails when it added none.
        with open(c.checklog, "w") as f:
            f.write("=== an older run\n-- step 1 of 7: VT switch\n  answer: no\n")
        checks_added = ("=== Focus reports, DECARM and mouse pixels (#233), 2026-10-26T10:00:00-05:00\n"
                        "-- step 4 of 7: Mouse 1016: pixel reports from 1\n"
                        "  [mouse]   1.538  #1               b'\\x1b[<0;1;1M'\n"
                        "  answer: yes\n=== done 2026-10-26T10:01:00-05:00\n")
        p = c.run(["6"], dry=False, VT_DEMO_GUIDE="focus-decarm-mouse", VT_DEMO_GUIDE_STEPS="4",
                  CHECKS=esc(checks_added), APPEND="")
        check("a guided run prints this run's lines and the results log",
              p.returncode == 0 and checks_added in p.stdout and "an older run" not in p.stdout
              and f"results: {c.checklog}" in p.stdout and "health: 0" in p.stdout
              and p.stdout.find("health: 0") < p.stdout.find("=== Focus reports")
              and read(c.args_out).splitlines() == [f"--guide={guide}", "--steps=4", "6", "--"],
              p.stdout + p.stderr)
        p = c.run(["6"], dry=False, VT_DEMO_GUIDE="focus-decarm-mouse", CHECKS="", APPEND="")
        check("a guided run that added nothing fails, naming the results log",
              p.returncode != 0 and f"no lines from this run in {c.checklog}" in p.stderr, p.stdout + p.stderr)
        p = c.run(["6"], dry=False, VT_DEMO_GUIDE="focus-decarm-mouse", STATUS="1",
                  CHECKS=esc("=== cut short\n"), APPEND="")
        check("a guided run whose kmscon failed fails, after its lines",
              p.returncode == 1 and "=== cut short" in p.stdout and "exited with status 1" in p.stderr,
              p.stdout + p.stderr)

        # sudo and run-vt.sh come from the environment only in a dry run or
        # under VT_DEMO_TEST: a stale export must not decide what runs as root.
        stubs = os.path.join(tmp, "stubs")
        os.makedirs(stubs, exist_ok=True)
        sudo_log = os.path.join(tmp, "sudo-args")
        with open(os.path.join(stubs, "sudo"), "w") as f:
            f.write(f'#!/bin/sh\nprintf "%s\\n" "$@" >"{sudo_log}"\nexit 1\n')
        os.chmod(os.path.join(stubs, "sudo"), 0o755)
        if os.path.exists(c.args_out):
            os.unlink(c.args_out)
        p = c.run(["6"], dry=False, VT_DEMO_TEST="", PATH=stubs + ":" + os.environ["PATH"], APPEND="")
        check("a stray VT_DEMO_SUDO and VT_DEMO_RUN_VT are ignored outside a dry run or the tests",
              p.returncode != 0 and "sudo failed" in p.stderr and os.path.exists(sudo_log)
              and read(sudo_log).splitlines() == ["-v"] and not os.path.exists(c.args_out),
              p.stdout + p.stderr)
        p = c.run(["6"], VT_DEMO_TEST="")
        check("a dry run takes VT_DEMO_RUN_VT, since it runs nothing",
              p.returncode == 0 and f"+ {c.run_vt} 6 --" in p.stdout, p.stdout + p.stderr)

        # make vt-login hands KIT on, to be refused; make's demo step is skipped.
        make = shutil.which("make")
        base = dict(os.environ, VT_DEMO_DRY_RUN="1", VT_DEMO_TEST="1", VT_DEMO_ACTIVE=c.active,
                    VT_DEMO_LOGINCTL=c.loginctl, VT_DEMO_SUDO="", VT_DEMO_RUN_VT=c.run_vt,
                    SESSIONS="", GONE="")
        for k in ("MAKEFLAGS", "MAKELEVEL", "MFLAGS"):
            base.pop(k, None)
        for args, ok, why in ((["KIT=sixel"], False, "KIT goes with the demo"),
                              ([], True, "run-vt.sh --login 8 --")):
            p = subprocess.run([make or "make", "-s", "-C", root, "-o", "demo", "-o", "pkg", "vt-login", "VT=8", *args],
                               env=base, capture_output=True, text=True, timeout=60) if make else None
            out = (p.stdout + p.stderr) if p else "make not found"
            check(f"make vt-login {' '.join(args) or '(no KIT)'} {'runs' if ok else 'is refused'}",
                  p is not None and (p.returncode == 0) == ok and why in out, out)
        # A GUIDE exported in the operator's shell does not reach vt-demo or
        # vt-login: they run the demo, or the login, as they say.
        for target, want in (("vt-demo", "run-vt.sh 8 --"), ("vt-login", "run-vt.sh --login 8 --")):
            p = subprocess.run([make or "make", "-s", "-C", root, "-o", "demo", "-o", "pkg", target, "VT=8"],
                               env=dict(base, VT_DEMO_GUIDE="focus-decarm-mouse", VT_DEMO_GUIDE_STEPS="4"),
                               capture_output=True, text=True, timeout=60) if make else None
            out = (p.stdout + p.stderr) if p else "make not found"
            check(f"make {target} ignores a VT_DEMO_GUIDE in the environment",
                  p is not None and p.returncode == 0 and want in out and "--guide" not in out, out)
        # make vt-check hands GUIDE and GUIDE_STEPS on, and KIT to be refused;
        # without GUIDE it fails before the build, naming the guides.
        for args, ok, why in ((["GUIDE=focus-decarm-mouse", "GUIDE_STEPS=4 5"], True,
                               f"run-vt.sh --guide={guide} --steps=4\\ 5 8 --"),
                              (["GUIDE=focus-decarm-mouse", "KIT=sixel"], False, "with no KIT and no login"),
                              ([], False, "vt-check: GUIDE=NAME, a guide in fork/demo/checks: focus-decarm-mouse")):
            opts = ["-o", "demo", "-o", "pkg"] if args else []
            p = subprocess.run([make or "make", "-s", "-C", root, *opts, "vt-check", "VT=8", *args],
                               env=base, capture_output=True, text=True, timeout=60) if make else None
            out = (p.stdout + p.stderr) if p else "make not found"
            check(f"make vt-check {' '.join(args) or '(no GUIDE)'} {'runs' if ok else 'is refused'}",
                  p is not None and (p.returncode == 0) == ok and why in out, out)

        # run-vt.sh lets through only allowlisted kmscon options, by exact name
        # (getopt_long takes unique prefixes), each one word: a separate word
        # would be taken as a value or moved into the login command.
        run_vt = os.path.join(root, "fork/demo/run-vt.sh")

        def run_vt_with(opts):
            return subprocess.run(["bash", run_vt, "--dry-run", "3", "--", *opts],
                                  capture_output=True, text=True, timeout=30)

        for opts in (["--"], ["-l"], ["-vl"], ["--login"], ["--login=/bin/sh"], ["--lo"],
                     ["--vt=tty2"], ["--configdir=/tmp"], ["--conf=/x"], ["-c"],
                     ["--switchvt"], ["--switch"], ["--oneshot"], ["--no-oneshot"],
                     ["--asciicast=/x"], ["--grab-reboot=<Ctrl>r"], ["--blink=1"],
                     ["--render-timer", "log"], ["--image-storage-limit", "512M"],
                     ["--sync-timeout", "1000"],
                     ["Nerd"], ["/path/x"], ["--font-name", "Monoid"],
                     ["--font-name=Monoid", "Nerd", "Font", "Mono"]):
            p = run_vt_with(opts)
            bad = next(o for o in opts if not o.startswith("--font-name=")) if len(opts) > 1 else opts[0]
            check(f"run-vt.sh refuses {' '.join(opts)}",
                  p.returncode != 0 and f"kmscon option '{bad}' is refused" in p.stderr
                  and '--font-name="Monoid Nerd Font Mono"' in p.stderr, p.stderr)
        # the image budget is capped for a root kmscon: auto or up to 4G
        for size in ("5G", "1024G", "4097M", "4294967297", "99999999999", "-1", ""):
            p = run_vt_with([f"--image-storage-limit={size}"])
            check(f"run-vt.sh refuses --image-storage-limit={size}",
                  p.returncode != 0 and "up to 4G here" in p.stderr, p.stderr)
        # 09M: a leading zero is decimal, not an octal error
        for size in ("auto", "4G", "4096M", "1G", "09M"):
            p = run_vt_with([f"--image-storage-limit={size}"])
            check(f"run-vt.sh accepts --image-storage-limit={size}",
                  "is refused" not in p.stderr and "up to 4G" not in p.stderr
                  and (p.returncode == 0 or "nothing staged" in p.stderr
                       or "does not exist" in p.stderr), p.stderr)
        # sync-timeout names no file and runs nothing; kmscon holds it to 10 s
        for value in ("0", "200", "1000", "10000"):
            p = run_vt_with([f"--sync-timeout={value}"])
            check(f"run-vt.sh accepts --sync-timeout={value}",
                  "is refused" not in p.stderr
                  and (p.returncode == 0 or "nothing staged" in p.stderr
                       or "does not exist" in p.stderr), p.stderr)
        operator = ["--font-name=Monoid Nerd Font Mono", "--font-size=30", "--mode=7680x2160",
                    "--no-use-original-mode"]
        p = run_vt_with(operator + ["--no-blink", "--palette-red=255,0,0", "--hwaccel",
                                    "--render-timer=log", "--image-storage-limit=512M"])
        # Past the option checks, a dry run stops at the staging check (no
        # make demo here) or prints its commands.
        check("run-vt.sh accepts the operator's options",
              "is refused" not in p.stderr
              and (p.returncode == 0 or "nothing staged" in p.stderr or "does not exist" in p.stderr),
              p.stderr)
        # The showcase moved to aaronsb/kmscon-showcase (#239): its options
        # are gone, and an old command fails at the usage, which is the
        # header's option list and stops where it ends.
        for opts in (["--showcase=/v.mkv"], ["--showcase-from=5"], ["--showcase-audio"]):
            p = subprocess.run(["bash", run_vt, "--dry-run", *opts, "3"], capture_output=True, text=True, timeout=30)
            check(f"run-vt.sh no longer takes {' '.join(opts)}",
                  p.returncode == 2 and "--showcase" not in p.stderr
                  and p.stderr.rstrip().endswith("As root, any of the four set is refused."), p.stderr)
        for opts, why in ((["--kit=bogus"], "--kit takes text, sixel, kitty, glyphs or all"),
                          (["--kit="], "--kit takes text, sixel, kitty, glyphs or all")):
            p = subprocess.run(["bash", run_vt, "--dry-run", *opts, "3"], capture_output=True, text=True, timeout=30)
            check(f"run-vt.sh refuses {' '.join(opts)}", p.returncode != 0 and why in p.stderr, p.stderr)
        # The demo's login command, read from a dry run in a copy of the
        # tree with a stand-in staged build: the session runs after setpriv
        # has dropped to the user and cleared the environment, and its label
        # and kit are words of env's.
        copy = os.path.join(tmp, "tree")
        shutil.copytree(os.path.join(root, "fork/demo"), os.path.join(copy, "fork/demo"))
        stage = os.path.join(copy, "fork/build/demo-root")
        os.makedirs(os.path.join(stage, "usr/bin"))
        os.makedirs(os.path.join(stage, "usr/lib/kmscon"))
        with open(os.path.join(stage, "usr/bin/kmscon"), "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(os.path.join(stage, "usr/bin/kmscon"), 0o755)
        with open(os.path.join(stage, "BUILD"), "w") as f:
            f.write("v10-test kmscon-aaronsb 1\n")
        os.makedirs(os.path.join(tmp, "moduledir"))
        p = subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run",
                            "--kit=glyphs", "4", "--", "--font-size=30"],
                           capture_output=True, text=True, timeout=30,
                           env=dict(os.environ, SUDO_USER="nobody",
                                    RUN_VT_MODULEDIR=os.path.join(tmp, "moduledir")))
        unshare = next((l for l in p.stdout.splitlines() if l.startswith("+ unshare ")), "")
        words = shlex.split(unshare[2:]) if unshare else []

        def at(w):
            return words.index(w) if w in words else -1

        login, setpriv, env_at = at("--login"), at("/usr/bin/setpriv"), at("/usr/bin/env")
        drop = words[setpriv:env_at]
        session = words[env_at:]
        nobody = pwd.getpwnam("nobody")
        check("run-vt.sh runs the demo as the user, after setpriv has cleared the environment",
              p.returncode == 0 and 0 < login < setpriv < env_at
              and drop == ["/usr/bin/setpriv", f"--reuid={nobody.pw_uid}", f"--regid={nobody.pw_gid}",
                           "--init-groups", "--reset-env", "--"]
              and "DEMO_KIT=glyphs" in session
              and session[-2:] == ["/bin/bash", os.path.join(copy, "fork/demo/demo.sh")]
              and not any(w.startswith("DEMO_") for w in words[:env_at])
              and "--font-size=30" in words[:login],
              p.stdout + p.stderr)

        # The demo's kit is one word of env's, after setpriv, like the label.
        p = subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run",
                            "--kit=sixel", "4"],
                           capture_output=True, text=True, timeout=30,
                           env=dict(os.environ, SUDO_USER="nobody",
                                    RUN_VT_MODULEDIR=os.path.join(tmp, "moduledir")))
        unshare = next((l for l in p.stdout.splitlines() if l.startswith("+ unshare ")), "")
        words = shlex.split(unshare[2:]) if unshare else []
        env_at = words.index("/usr/bin/env") if "/usr/bin/env" in words else -1
        check("run-vt.sh hands the demo its kit in env's words, and no runtime directory",
              p.returncode == 0 and env_at > 0 and "DEMO_KIT=sixel" in words[env_at:]
              and not any(w.startswith("XDG_RUNTIME_DIR=") for w in words)
              and words[-2:] == ["/bin/bash", os.path.join(copy, "fork/demo/demo.sh")],
              p.stdout + p.stderr)

        # --guide (#238): the guided checks as the session, as the user after
        # setpriv, with --oneshot, so the run ends after the last step. The
        # guide's path, the VT, the desktop's VT and the steps are words of
        # env's and guide.sh's; no demo variable is set.
        cguide = os.path.join(copy, "fork/demo/checks/focus-decarm-mouse.guide")
        p = subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run",
                            f"--guide={cguide}", "--steps=4 5", "6", "--", "--dpms-timeout=15"],
                           capture_output=True, text=True, timeout=30,
                           env=dict(os.environ, SUDO_USER="nobody",
                                    RUN_VT_MODULEDIR=os.path.join(tmp, "moduledir")))
        unshare = next((l for l in p.stdout.splitlines() if l.startswith("+ unshare ")), "")
        words = shlex.split(unshare[2:]) if unshare else []
        login, setpriv, env_at = at("--login"), at("/usr/bin/setpriv"), at("/usr/bin/env")
        check("run-vt.sh --guide runs guide.sh on the guide as the user, with --oneshot",
              p.returncode == 0 and 0 < login < setpriv < env_at
              and "--oneshot" in words[:login] and "--dpms-timeout=15" in words[:login]
              and words[env_at:] == ["/usr/bin/env", "GUIDE_VT=6", "GUIDE_DESKTOP=N", "GUIDE_STEPS=4 5",
                                     "/bin/bash", os.path.join(copy, "fork/demo/checks/guide.sh"), cguide]
              and "follow each step of " + cguide + " (steps 4 5)" in p.stdout
              and "when the checks end" in p.stdout,
              p.stdout + p.stderr)
        p = subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run",
                            "--guide=fork/demo/checks/focus-decarm-mouse.guide", "6"],
                           capture_output=True, text=True, timeout=30, cwd=copy,
                           env=dict(os.environ, SUDO_USER="nobody",
                                    RUN_VT_MODULEDIR=os.path.join(tmp, "moduledir")))
        unshare = next((l for l in p.stdout.splitlines() if l.startswith("+ unshare ")), "")
        words = shlex.split(unshare[2:]) if unshare else []
        check("run-vt.sh --guide makes a relative path absolute, and without --steps sets no GUIDE_STEPS",
              p.returncode == 0 and words[-1] == os.path.realpath(cguide)
              and not any(w.startswith("GUIDE_STEPS=") for w in words), p.stdout + p.stderr)
        for opts, why in ((["--guide=/x.guide", "--kit=sixel"], "--guide runs the guided checks"),
                          (["--guide=/x.guide", "--login"], "--guide runs the guided checks"),
                          (["--guide="], "--guide needs the guide's path"),
                          (["--steps=4"], "--steps goes with --guide=FILE"),
                          (["--guide=/x.guide", "--steps=0"], "--steps takes step numbers"),
                          (["--guide=/x.guide", "--steps=4;5"], "--steps takes step numbers"),
                          (["--guide=/x.guide", "--steps=4 $(id)"], "--steps takes step numbers")):
            p = subprocess.run(["bash", run_vt, "--dry-run", *opts, "8"], capture_output=True, text=True, timeout=30)
            check(f"run-vt.sh refuses {' '.join(opts)}", p.returncode != 0 and why in p.stderr, p.stderr)

        def guide_words(*opts):
            p = subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run",
                                f"--guide={cguide}", *opts, "6"],
                               capture_output=True, text=True, timeout=30,
                               env=dict(os.environ, SUDO_USER="nobody",
                                        RUN_VT_MODULEDIR=os.path.join(tmp, "moduledir")))
            unshare = next((l for l in p.stdout.splitlines() if l.startswith("+ unshare ")), "")
            return p, shlex.split(unshare[2:]) if unshare else []

        p, words = guide_words("--steps=")
        check("run-vt.sh takes an empty --steps= as all steps, with no GUIDE_STEPS",
              p.returncode == 0 and words[-1] == cguide
              and not any(w.startswith("GUIDE_STEPS") for w in words), p.stdout + p.stderr)

        # GUIDE_STEPS as an operator types it, with stray spaces, a tab or a
        # comma and a space, which guide.sh --check takes: vt-demo.sh hands
        # run-vt.sh the words, one space apart, and run-vt.sh takes them.
        for typed, want in (("4, 5", "4 5"), ("4  5", "4 5"), (" 4", "4"), ("4 ", "4"), ("4\t5", "4 5")):
            p = c.run(["6"], VT_DEMO_GUIDE="focus-decarm-mouse", VT_DEMO_GUIDE_STEPS=typed)
            handed = [w for w in plus(p) if w.startswith("--steps=")]
            r, words = guide_words(*handed)
            check(f"GUIDE_STEPS={typed!r} reaches run-vt.sh as --steps={want!r}, and run-vt.sh takes it",
                  p.returncode == 0 and handed == [f"--steps={want}"]
                  and r.returncode == 0 and f"GUIDE_STEPS={want}" in words,
                  p.stdout + p.stderr + r.stderr)

        # --login (#207): kmscon's own login, /bin/login through PAM, as the
        # session, in the same mount namespace; no setpriv and no env, since
        # the login decides who runs, and no --oneshot, so a logout or login's
        # timeout brings a new prompt. The kmscon options keep their allowlist.
        def run_vt_copy(*args, **env):
            return subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run", *args],
                                  capture_output=True, text=True, timeout=30,
                                  env=dict(os.environ, SUDO_USER="nobody",
                                           RUN_VT_MODULEDIR=os.path.join(tmp, "moduledir"), **env))

        p = run_vt_copy("--login", "8", "--", "--font-size=30", "--hwaccel")
        unshare = next((l for l in p.stdout.splitlines() if l.startswith("+ unshare ")), "")
        words = shlex.split(unshare[2:]) if unshare else []
        kmscon = os.path.join(copy, "fork/build/demo-root/usr/bin/kmscon")
        check("run-vt.sh --login runs kmscon's login in the private mount namespace",
              p.returncode == 0
              and words[:9] == ["unshare", "--mount", "--propagation", "private", "--", "/bin/sh", "-c",
                                'mount --bind "$1" "$2" && shift 2 && exec "$@"', "sh"]
              and words[9:11] == [os.path.join(copy, "fork/build/demo-root/usr/lib/kmscon"),
                                  os.path.join(tmp, "moduledir")]
              and words[11:] == [kmscon, "--vt=tty8", "--no-switchvt", "--configdir",
                                 os.path.join(copy, "fork/demo"), "--font-size=30", "--hwaccel",
                                 "--login", "--", "/bin/login", "-p"],
              p.stdout + p.stderr)
        tell = p.stdout[:p.stdout.find("+ unshare ")]
        check("run-vt.sh --login says what to run once logged in, before kmscon starts",
              "Ctrl-Alt-F8" in tell and "log in" in tell
              and "DEMO_LABEL='dev build v10-test' fork/demo/demo.sh" in tell
              and "DEMO_KIT=sixel" in tell
              and "github.com/aaronsb/kmscon-showcase" in tell
              and not any(w in tell.lower() for w in ("audio", "sound"))
              and "log out to end it" in tell and "Ctrl-C here ends it" in tell
              and os.path.realpath(copy) in tell, tell)
        # The VT's console units, whichever of kmsconvt@, getty@ or autovt@
        # runs there, are checked, stopped and started again on exit.
        for name in ("kmsconvt", "getty", "autovt"):
            check(f"run-vt.sh --login checks {name}@tty8",
                  f"+ systemctl is-active --quiet {name}@tty8.service" in p.stdout, p.stdout)
        order = [p.stdout.find(s) for s in ("+ systemctl stop kmsconvt@tty8.service", "+ unshare ",
                                            "+ systemctl start kmsconvt@tty8.service")]
        check("run-vt.sh stops the VT's active units before kmscon and starts them after",
              0 <= order[0] < order[1] < order[2], p.stdout)
        # Whichever units run are stopped before kmscon and started after it.
        for active in ("getty@tty8.service autovt@tty8.service", "kmsconvt@tty8.service getty@tty8.service"):
            p = run_vt_copy("--login", "8", RUN_VT_ACTIVE=active)
            order = [p.stdout.find(s) for s in (f"+ systemctl stop {active}\n", "+ unshare ",
                                                f"+ systemctl start {active}\n")]
            check(f"run-vt.sh stops {active} before kmscon and starts them after",
                  p.returncode == 0 and 0 <= order[0] < order[1] < order[2]
                  and f"when it ends, {active} is started again" in p.stdout, p.stdout + p.stderr)
        p = run_vt_copy("--login", "8", RUN_VT_ACTIVE="")
        check("run-vt.sh with no unit active stops and starts none",
              p.returncode == 0 and "systemctl stop" not in p.stdout and "systemctl start" not in p.stdout
              and "nothing to restore" in p.stdout, p.stdout + p.stderr)

        # The session check fails closed: no list, no run.
        fake = os.path.join(tmp, "run-vt-loginctl")
        with open(fake, "w") as f:
            f.write(FAKE_RUN_VT_LOGINCTL)
        os.chmod(fake, 0o755)
        count = os.path.join(tmp, "loginctl-count")

        def run_vt_seq(seq, *args, **env):
            if os.path.exists(count):
                os.unlink(count)
            return run_vt_copy(*args, RUN_VT_LOGINCTL=fake, SEQ=seq, COUNT=count, **env)

        p = run_vt_seq("F", "--login", "8")
        check("run-vt.sh refuses when loginctl cannot list the sessions",
              p.returncode != 0 and "whether a session is on VT 8 is unknown" in p.stderr
              and "+ unshare" not in p.stdout, p.stdout + p.stderr)
        # A session show-session fails for is gone only when a second list no
        # longer has it; one still listed leaves the VT unknown, so no run.
        p = run_vt_seq("8! 8!", "8")
        check("run-vt.sh refuses when loginctl cannot read a session still listed",
              p.returncode != 0 and "whether a session is on VT 8 is unknown" in p.stderr
              and "+ unshare" not in p.stdout and int(read(count)) == 2, p.stdout + p.stderr)
        p = run_vt_seq("8! -", "8")
        check("run-vt.sh takes a session it cannot read, gone from a second list, as ended",
              p.returncode == 0 and "+ unshare" in p.stdout and int(read(count)) == 2, p.stdout + p.stderr)
        for seq in ("8", "8:closing"):
            p = run_vt_seq(seq, "8")
            check(f"run-vt.sh refuses VT 8 with a session there ({seq})",
                  p.returncode != 0 and "logind session c8_" in p.stderr and "is on VT 8" in p.stderr
                  and "+ unshare" not in p.stdout, p.stdout + p.stderr)
        p = run_vt_seq("4", "8")
        check("run-vt.sh takes VT 8 with a session on another VT",
              p.returncode == 0 and "+ unshare" in p.stdout, p.stdout + p.stderr)

        # With --login, the first logout ends the run: the watch sees no
        # session, then one, then none open (or only a closing one), and stops
        # kmscon (here a stand-in) and restarts the unit. Before any login, and
        # through a poll whose list fails or whose session cannot be read while
        # still listed, it keeps going.
        for seq, name in (("- - 8 F 8 -", "a logout"), ("- 8 8:online 8:closing", "a session left closing"),
                          ("- 8 8! 8! 8 -", "a logout after a session it could not read"),
                          ("- 8 8! -", "a logout seen as a session gone from a second list")):
            p = run_vt_seq(seq, "--login", "8", RUN_VT_CHILD="exec sleep 60")
            after = p.stdout[p.stdout.find("+ unshare "):]
            check(f"run-vt.sh --login ends at {name} and restores the unit",
                  p.returncode == 0 and "vt-login: session ended" in after
                  and after.find("vt-login: session ended") < after.find("+ kill -TERM ")
                  < after.find("+ systemctl start kmsconvt@tty8.service")
                  and int(read(count)) == len(seq.split()), p.stdout + p.stderr)
        p = run_vt_seq("-", "--login", "8", RUN_VT_CHILD="sleep 3")
        check("run-vt.sh --login keeps going while nobody has logged in",
              "session ended" not in p.stdout and int(read(count)) >= 3, p.stdout + p.stderr)
        # kmscon ending on its own is a failure in a login run, status 0
        # included: that is its limit on logins failing in a row.
        check("run-vt.sh --login fails when kmscon ends on its own with status 0",
              p.returncode == 1 and "kmscon exited with status 0" in p.stderr, p.stdout + p.stderr)
        # A kmscon crash after the login is no logout, even when the poll
        # after it finds the session gone.
        p = run_vt_seq("- 8 -", "--login", "8", RUN_VT_CHILD="sleep 1.5; exit 3")
        check("run-vt.sh --login takes a kmscon crash after a login for a crash",
              p.returncode == 3 and "kmscon exited with status 3" in p.stderr
              and "session ended" not in p.stdout, p.stdout + p.stderr)
        # A watch whose polls keep failing says so, once, and keeps going.
        p = run_vt_seq("- F", "--login", "8", RUN_VT_CHILD="sleep 12.5")
        check("run-vt.sh --login warns once after 10 failed polls in a row",
              p.stderr.count("loginctl failed 10 polls in a row") == 1 and "session ended" not in p.stdout
              and int(read(count)) >= 12, p.stdout + p.stderr)
        p = run_vt_seq("-", "8", RUN_VT_CHILD="exit 0")
        check("run-vt.sh without --login does not watch the sessions",
              p.returncode == 0 and int(read(count)) == 1, p.stdout + p.stderr)

        # The dry run's stand-ins are for the tests: as root, each is refused,
        # so a root dry run runs nothing its caller chose. id -u is the check.
        rootbin = os.path.join(tmp, "rootbin")
        os.makedirs(rootbin, exist_ok=True)
        with open(os.path.join(rootbin, "id"), "w") as f:
            f.write(f'#!/bin/sh\n[ "$*" = -u ] && echo 0 || exec {shutil.which("id")} "$@"\n')
        os.chmod(os.path.join(rootbin, "id"), 0o755)
        planted = os.path.join(tmp, "planted-by-child")
        for var, value in (("RUN_VT_CHILD", f"touch {planted}"), ("RUN_VT_LOGINCTL", fake),
                           ("RUN_VT_ACTIVE", ""), ("RUN_VT_MODULEDIR", os.path.join(tmp, "moduledir"))):
            env = dict(os.environ, SUDO_USER="nobody", PATH=rootbin + ":" + os.environ["PATH"],
                       SEQ="-", COUNT=count, **{var: value})
            p = subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run", "--login", "8"],
                               capture_output=True, text=True, timeout=30, env=env)
            check(f"run-vt.sh as root refuses {var}",
                  p.returncode != 0 and f"{var} is for the tests" in p.stderr
                  and "+ unshare" not in p.stdout and not os.path.exists(planted), p.stdout + p.stderr)
        p = subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run", "--login", "8"],
                           capture_output=True, text=True, timeout=30,
                           env=dict(os.environ, SUDO_USER="nobody", PATH=rootbin + ":" + os.environ["PATH"]))
        check("run-vt.sh as root takes a dry run without the stand-ins",
              "is for the tests" not in p.stderr
              and (p.returncode == 0 or "does not exist" in p.stderr), p.stdout + p.stderr)

        # restore() stops kmscon only while it is this script's running job:
        # a PID that kmscon left and another process took is not killed. A
        # private PID namespace makes the reuse happen: once the stand-in
        # kmscon has exited, the stand-in loginctl starts a process on its PID
        # (ns_last_pid) and stops the script, whose PID there is 1.
        reuse = os.path.join(tmp, "reuse-loginctl")
        with open(reuse, "w") as f:
            f.write("""#!/bin/bash
[ "$1" = list-sessions ] || exit 0
[ -s "$PIDFILE" ] && [ ! -e "$TAKEN" ] && ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null || exit 0
echo $(( $(cat "$PIDFILE") - 1 )) >/proc/sys/kernel/ns_last_pid
setsid sleep 30 </dev/null >/dev/null 2>&1 &
echo "$!" >"$TAKEN"
kill -TERM 1
""")
        os.chmod(reuse, 0o755)
        pidfile, taken = os.path.join(tmp, "child-pid"), os.path.join(tmp, "taken-pid")
        ns = ["unshare", "--user", f"--map-user={os.getuid()}", f"--map-group={os.getgid()}", "--keep-caps",
              "--pid", "--fork", "--mount-proc", "--"]
        if subprocess.run([*ns, "true"], capture_output=True).returncode != 0:
            print("skip  run-vt.sh leaves a PID kmscon left to its new owner: no user and PID namespace here")
        else:
            p = subprocess.run([*ns, "bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run", "--login", "8"],
                               capture_output=True, text=True, timeout=30,
                               env=dict(os.environ, SUDO_USER="nobody", RUN_VT_MODULEDIR=os.path.join(tmp, "moduledir"),
                                        RUN_VT_LOGINCTL=reuse, PIDFILE=pidfile, TAKEN=taken,
                                        RUN_VT_CHILD='echo $$ >"$PIDFILE"'))
            same = os.path.exists(taken) and read(taken).strip() == read(pidfile).strip()
            check("run-vt.sh leaves a PID kmscon left to its new owner",
                  p.returncode == 143 and same and "+ kill -TERM" not in p.stdout,
                  p.stdout + p.stderr + f"\nchild {read(pidfile) if os.path.exists(pidfile) else '?'}"
                  f" taken {read(taken) if os.path.exists(taken) else '?'}")

        for opts, why in ((["--login", "--kit=sixel"], "--login runs no demo"),):
            p = subprocess.run(["bash", run_vt, "--dry-run", *opts, "8"], capture_output=True, text=True, timeout=30)
            check(f"run-vt.sh refuses {' '.join(opts)}", p.returncode != 0 and why in p.stderr, p.stderr)
        for bad in ("--login=/bin/sh", "--lo", "Nerd"):
            p = run_vt_copy("--login", "8", "--", bad)
            check(f"run-vt.sh --login refuses the kmscon option {bad}",
                  p.returncode != 0 and f"kmscon option '{bad}' is refused" in p.stderr, p.stderr)

        p = subprocess.run(["bash", run_vt, "--dry-run", "05"], capture_output=True, text=True, timeout=30)
        check("run-vt.sh refuses VT 05", p.returncode != 0 and "not '05'" in p.stderr, p.stderr)

    if failures:
        print(f"{len(failures)} failed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
