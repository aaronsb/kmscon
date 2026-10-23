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

    def run(self, args, active="tty7", sessions=(), dry=True, gone=(), **fake):
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
            GONE=" ".join(str(s) for s in gone),
            FEEDBACK=self.feedback,
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
    return text.replace("\t", "\\t").replace("\n", "\\n")


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
        check("no dev-build answers fails the run",
              p.returncode != 0 and "no answers from this run" in p.stderr, p.stdout + p.stderr)
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

        # The showcase (make showcase): the video, made absolute, and its act
        # and audio options go to run-vt.sh before the VT; its run records no
        # answers, so it ends with the health check and status 0.
        video = os.path.join(tmp, "bad apple.mkv")
        open(video, "w").close()
        p = c.run(["6", "--", "--font-size=30"], VT_DEMO_SHOWCASE="bad apple.mkv",
                  VT_DEMO_SHOWCASE_FROM="5", VT_DEMO_SHOWCASE_AUDIO="1")
        cmd = next((l for l in p.stdout.splitlines() if l.startswith("+ ")), "")
        want = "run-vt.sh --showcase=" + video.replace(" ", "\\ ") + " --showcase-from=5 --showcase-audio 6 -- --font-size=30"
        check("the showcase's video, act and audio reach run-vt.sh before the VT",
              p.returncode == 0 and cmd.endswith(want), p.stdout + p.stderr)
        for value, wanted in (("no", False), ("0", False), ("yes", True), ("1", True)):
            p = c.run(["6"], VT_DEMO_SHOWCASE=video, VT_DEMO_SHOWCASE_AUDIO=value)
            check(f"AUDIO={value} {'plays' if wanted else 'does not play'} the soundtrack",
                  p.returncode == 0 and ("--showcase-audio" in p.stdout) == wanted, p.stdout + p.stderr)
        p = c.run(["6"], VT_DEMO_SHOWCASE=video, VT_DEMO_SHOWCASE_AUDIO="loud")
        check("AUDIO=loud is refused", p.returncode != 0 and "AUDIO=loud" in p.stderr, p.stderr)
        p = c.run(["6"], VT_DEMO_SHOWCASE=os.path.join(tmp, "missing.mkv"))
        check("a showcase video that is not there is refused",
              p.returncode != 0 and "missing.mkv is not a readable file" in p.stderr, p.stderr)
        p = c.run(["6"], dry=False, VT_DEMO_SHOWCASE=video, APPEND="")
        with open(c.args_out) as f:
            args = f.read().splitlines()
        check("a showcase run ends without answers, with the health check",
              p.returncode == 0 and "health: 0" in p.stdout and "results:" not in p.stdout
              and "no answers" not in p.stderr and args == ["--showcase=" + video, "6", "--"],
              p.stdout + p.stderr + repr(args))
        p = c.run(["6"], dry=False, VT_DEMO_SHOWCASE=video, STATUS="130", APPEND="")
        check("a showcase cut short keeps run-vt.sh's status", p.returncode == 130, p.stdout + p.stderr)

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
                     ["--render-timer", "log"],
                     ["Nerd"], ["/path/x"], ["--font-name", "Monoid"],
                     ["--font-name=Monoid", "Nerd", "Font", "Mono"]):
            p = run_vt_with(opts)
            bad = next(o for o in opts if not o.startswith("--font-name=")) if len(opts) > 1 else opts[0]
            check(f"run-vt.sh refuses {' '.join(opts)}",
                  p.returncode != 0 and f"kmscon option '{bad}' is refused" in p.stderr
                  and '--font-name="Monoid Nerd Font Mono"' in p.stderr, p.stderr)
        operator = ["--font-name=Monoid Nerd Font Mono", "--font-size=30", "--mode=7680x2160",
                    "--no-use-original-mode"]
        p = run_vt_with(operator + ["--no-blink", "--palette-red=255,0,0", "--hwaccel",
                                    "--render-timer=log"])
        # Past the option checks, a dry run stops at the staging check (no
        # make demo here) or prints its commands.
        check("run-vt.sh accepts the operator's options",
              "is refused" not in p.stderr
              and (p.returncode == 0 or "nothing staged" in p.stderr or "does not exist" in p.stderr),
              p.stderr)
        for opts, why in ((["--showcase=video.mkv"], "needs an absolute path"),
                          (["--showcase="], "needs the video's path"),
                          (["--showcase=/v.mkv", "--showcase-from=8"], "an act from 1 to 7"),
                          (["--showcase=/v.mkv", "--showcase-from=x"], "an act from 1 to 7"),
                          (["--showcase-audio"], "go with --showcase=VIDEO"),
                          (["--showcase-from=2"], "go with --showcase=VIDEO")):
            p = subprocess.run(["bash", run_vt, "--dry-run", *opts, "3"], capture_output=True, text=True, timeout=30)
            check(f"run-vt.sh refuses {' '.join(opts)}", p.returncode != 0 and why in p.stderr, p.stderr)
        p = subprocess.run(["bash", run_vt, "--dry-run", "--showcase=/v.mkv", "--showcase-from=5", "--showcase-audio", "3"],
                           capture_output=True, text=True, timeout=30)
        check("run-vt.sh accepts the showcase's options",
              "--showcase" not in p.stderr
              and (p.returncode == 0 or "nothing staged" in p.stderr or "does not exist" in p.stderr), p.stderr)
        # The showcase's login command, read from a dry run in a copy of the
        # tree with a stand-in staged build: the session runs after setpriv
        # has dropped to the user and cleared the environment, and the video
        # is one word of env's.
        copy = os.path.join(tmp, "tree")
        for d in ("fork/demo", "fork/showcase"):
            shutil.copytree(os.path.join(root, d), os.path.join(copy, d))
        stage = os.path.join(copy, "fork/build/demo-root")
        os.makedirs(os.path.join(stage, "usr/bin"))
        os.makedirs(os.path.join(stage, "usr/lib/kmscon"))
        with open(os.path.join(stage, "usr/bin/kmscon"), "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(os.path.join(stage, "usr/bin/kmscon"), 0o755)
        with open(os.path.join(stage, "BUILD"), "w") as f:
            f.write("v10-test kmscon-aaronsb 1\n")
        os.makedirs(os.path.join(tmp, "moduledir"))
        tricky = "/tmp/a b;$(id).mkv"
        p = subprocess.run(["bash", os.path.join(copy, "fork/demo/run-vt.sh"), "--dry-run",
                            f"--showcase={tricky}", "--showcase-from=5", "--showcase-audio", "4",
                            "--", "--font-size=30"],
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
        check("run-vt.sh runs the showcase as the user, its video one word of env's",
              p.returncode == 0 and 0 < login < setpriv < env_at
              and drop == ["/usr/bin/setpriv", f"--reuid={nobody.pw_uid}", f"--regid={nobody.pw_gid}",
                           "--init-groups", "--reset-env", "--"]
              and f"BAD_APPLE={tricky}" in session
              and session[-5:] == ["/bin/bash", os.path.join(copy, "fork/showcase/bad-apple.sh"),
                                   "--from", "5", "--audio"]
              and not any(w.startswith("BAD_APPLE=") for w in words[:env_at])
              and "--font-size=30" in words[:login],
              p.stdout + p.stderr)

        p = subprocess.run(["bash", run_vt, "--dry-run", "05"], capture_output=True, text=True, timeout=30)
        check("run-vt.sh refuses VT 05", p.returncode != 0 and "not '05'" in p.stderr, p.stderr)

    if failures:
        print(f"{len(failures)} failed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
