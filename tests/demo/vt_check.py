#!/usr/bin/env python3
"""Checks the guided VT checks (#238) without a VT: fork/demo/checks/guide.sh
driven through scripted answers on a pseudo-terminal, with probe.py reading
the bytes the driver sends as the terminal would, then the results log they
leave; guide.sh --check on broken guides and on every guide in the tree; and
probe.py alone, for its bg mode's timing and how it splits a read.

    vt_check.py --root SOURCE_ROOT
"""

import argparse
import importlib.util
import os
import pty
import re
import select
import subprocess
import sys
import tempfile
import time

ISO = r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d[+-]\d\d:\d\d"

GUIDE = """# a guide for the test
title: Test guide
intro: An intro line for VT {vt}.

step: Focus
probe: focus
do: switch to F{desktop}
do: and back to F{vt}.
expect: FOCUS OUT then FOCUS IN.

step: Skipped
probe: decarm
do: nothing
expect: nothing

step: Look only
do: look at the screen
expect: it is there

step: Not run
probe: mouse
do: click
expect: a report
"""

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


class Pty:
    """A program on a pty, read until each prompt before it is answered."""

    def __init__(self, argv, env):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execve(argv[0], argv, env)
        self.out = b""
        self.status = None

    def pump(self, timeout):
        if select.select([self.fd], [], [], timeout)[0]:
            try:
                data = os.read(self.fd, 4096)
            except OSError:
                data = b""
            if not data:
                return False
            self.out += data
        return True

    def wait_for(self, text, timeout=10, after=0):
        end = time.monotonic() + timeout
        while self.out.find(text, after) < 0:
            if time.monotonic() > end or not self.pump(end - time.monotonic()):
                raise TimeoutError(f"no {text!r} in:\n{self.out[-2000:].decode(errors='replace')}")
        return self.out.find(text, after) + len(text)

    def send(self, data):
        os.write(self.fd, data)

    def finish(self, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                while self.pump(0.05):
                    pass
                self.status = os.waitstatus_to_exitcode(status)
                return self.status
            self.pump(0.05)
        os.kill(self.pid, 9)
        os.waitpid(self.pid, 0)
        raise TimeoutError("did not exit:\n" + self.out[-2000:].decode(errors="replace"))


def guide_env(log, **extra):
    env = {"PATH": os.environ["PATH"], "TERM": "linux", "GUIDE_LOG": log,
           "GUIDE_VT": "6", "GUIDE_DESKTOP": "7"}
    env.update(extra)
    return env


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = os.path.abspath(ap.parse_args().root)
    checks = os.path.join(root, "fork/demo/checks")
    runner = os.path.join(checks, "guide.sh")
    probe_py = os.path.join(checks, "probe.py")

    with tempfile.TemporaryDirectory() as tmp:
        guide = os.path.join(tmp, "test.guide")
        with open(guide, "w") as f:
            f.write(GUIDE)
        log = os.path.join(tmp, "out", "vt-check.log")

        # A whole run: a probe step answered yes with a note after one key it
        # does not take, a step skipped at its first prompt, a step without a
        # probe answered no, and q at the last step's first prompt.
        g = Pty(["/bin/bash", runner, guide], guide_env(log))
        try:
            at = g.wait_for(b"Press Enter to begin.")
            intro = g.out[:at]
            g.send(b"\r")
            at = g.wait_for(b"Press Enter to start the probe", after=at)
            step1 = g.out[:at]
            g.send(b"\r")
            at = g.wait_for(b"focus on; q quits", after=at)
            g.send(b"\x1b[O\x1b[I")
            at = g.wait_for(b"FOCUS IN", after=at)
            g.send(b"a")
            at = g.wait_for(b"b'a'", after=at)
            g.send(b"q")
            at = g.wait_for(b"Looked right?", after=at)
            probed = g.out
            g.send(b"x")
            at = g.wait_for(b"Looked right?", after=at)
            g.send(b"y")
            at = g.wait_for(b"Note (Enter for none): ", after=at)
            g.send(b"all good\r")
            at = g.wait_for(b"Step 2 of 4: Skipped", after=at)
            at = g.wait_for(b"skips this step", after=at)
            g.send(b"s\r")
            at = g.wait_for(b"Step 3 of 4: Look only", after=at)
            at = g.wait_for(b"Press Enter when done", after=at)
            g.send(b"\r")
            at = g.wait_for(b"Looked right?", after=at)
            g.send(b"n")
            at = g.wait_for(b"Note (Enter for none): ", after=at)
            g.send(b"\r")
            at = g.wait_for(b"Step 4 of 4: Not run", after=at)
            at = g.wait_for(b"q ends the checks", after=at)
            g.send(b"q\r")
            status = g.finish()
        except TimeoutError as e:
            check("guide.sh runs a guide to its end on a pty", False, str(e))
            status = None
        if status is not None:
            check("guide.sh ends with status 0 after q", status == 0, g.out[-500:].decode(errors="replace"))
            check("the title and intro are shown, {vt} filled in",
                  b"Test guide" in intro and b"An intro line for VT 6." in intro
                  and b"VT 6 gets its console back" in intro and b"Ctrl+Alt+F7 is your desktop" in intro,
                  intro.decode(errors="replace"))
            check("a step's do: and expect: lines are shown under their labels, {desktop} filled in",
                  b"Step 1 of 4: Focus" in step1
                  and b"Do:     switch to F7\r\n        and back to F6." in step1
                  and b"Expect: FOCUS OUT then FOCUS IN." in step1,
                  step1.decode(errors="replace"))
            check("the probe sets mode 1004 and resets it at q",
                  0 <= probed.find(b"\x1b[?1004h") < probed.find(b"#1    FOCUS OUT")
                  < probed.find(b"\x1b[?1004l"),
                  probed[-800:].decode(errors="replace"))
            lines = read(log).splitlines()
            want = [
                rf"=== Test guide, {ISO}, guide {re.escape(os.path.realpath(guide))}, tty /dev/pts/\d+, steps all",
                r"-- step 1 of 4: Focus",
                r"  \[focus\] focus on; q quits",
                r"  \[focus\] +\d+\.\d{3}  #1    FOCUS OUT  b'\\x1b\[O'",
                r"  \[focus\] +\d+\.\d{3}  #2    FOCUS IN   b'\\x1b\[I'",
                r"  \[focus\] +\d+\.\d{3}  #3               b'a'",
                r"  answer: yes  note: all good",
                r"-- step 2 of 4: Skipped",
                r"  answer: skip  \(before the probe\)",
                r"-- step 3 of 4: Look only",
                r"  answer: no",
                r"-- step 4 of 4: Not run",
                r"  ended here",
                rf"=== ended at step 4, {ISO}",
            ]
            check("the results log holds the run's header, each step, the probe's lines and the answers",
                  len(lines) == len(want) and all(re.fullmatch(w, l) for w, l in zip(want, lines)),
                  "\n".join(lines))

        # GUIDE_STEPS runs only the steps named, and the run ends at the last
        # screen; the log is appended to, under a header of its own.
        before = read(log)
        g = Pty(["/bin/bash", runner, guide], guide_env(log, GUIDE_STEPS="3"))
        try:
            at = g.wait_for(b"Press Enter to begin.")
            g.send(b"\r")
            at = g.wait_for(b"Step 3 of 4: Look only", after=at)
            shown = g.out
            at = g.wait_for(b"Press Enter when done", after=at)
            g.send(b"\r")
            at = g.wait_for(b"Looked right?", after=at)
            g.send(b"s")
            at = g.wait_for(b"Note (Enter for none): ", after=at)
            g.send(b"no second monitor\r")
            at = g.wait_for(b"Press Enter to end the run.", after=at)
            g.send(b"\r")
            status = g.finish()
        except TimeoutError as e:
            check("guide.sh with GUIDE_STEPS=3 runs on a pty", False, str(e))
            status = None
        if status is not None:
            added = read(log)[len(before):].splitlines()
            want = [rf"=== Test guide, {ISO}, guide .*, tty /dev/pts/\d+, steps 3",
                    r"-- step 3 of 4: Look only",
                    r"  answer: skip  note: no second monitor",
                    rf"=== done {ISO}"]
            check("GUIDE_STEPS=3 runs step 3 alone and appends its answers",
                  status == 0 and b"Step 1 of" not in shown and b"Step 2 of" not in shown
                  and read(log).startswith(before)
                  and len(added) == len(want) and all(re.fullmatch(w, l) for w, l in zip(want, added)),
                  "\n".join(added))

        # Ctrl-D at a line prompt is an end of file, never a crash: at a note
        # it is no note, at a step's first prompt it ends the checks, and the
        # log ends with a line saying so.
        before = read(log)
        g = Pty(["/bin/bash", runner, guide], guide_env(log))
        try:
            at = g.wait_for(b"Press Enter to begin.")
            g.send(b"\x04")
            at = g.wait_for(b"Step 1 of 4: Focus", after=at)
            at = g.wait_for(b"q ends the checks", after=at)
            g.send(b"\r")
            at = g.wait_for(b"focus on; q quits", after=at)
            g.send(b"q")
            at = g.wait_for(b"Looked right?", after=at)
            g.send(b"y")
            at = g.wait_for(b"Note (Enter for none): ", after=at)
            g.send(b"\x04")
            at = g.wait_for(b"Step 2 of 4: Skipped", after=at)
            at = g.wait_for(b"q ends the checks", after=at)
            g.send(b"\x04")
            status = g.finish()
            added = read(log)[len(before):].splitlines()
            want = [rf"=== Test guide, {ISO}, guide .*, steps all",
                    r"-- step 1 of 4: Focus",
                    r"  \[focus\] focus on; q quits",
                    r"  answer: yes",
                    r"-- step 2 of 4: Skipped",
                    r"  ended here",
                    rf"=== ended at step 2, {ISO}"]
            check("Ctrl-D at the first screen, a note and a step's prompt ends the checks cleanly",
                  status == 0 and len(added) == len(want)
                  and all(re.fullmatch(w, l) for w, l in zip(want, added)),
                  f"status {status}\n" + "\n".join(added))
        except TimeoutError as e:
            check("Ctrl-D at the first screen, a note and a step's prompt ends the checks cleanly", False, str(e))

        # A log that cannot be written is said on the screen before anything
        # is asked, and the run ends after Enter.
        g = Pty(["/bin/bash", runner, guide], guide_env("/proc/nonexistent/vt-check.log"))
        try:
            g.wait_for(b"Press Enter to end.")
            g.send(b"\r")
            status = g.finish()
            check("a results log that cannot be written ends the run at once, saying so",
                  status == 1 and b"cannot write /proc/nonexistent/vt-check.log" in g.out
                  and b"Step 1" not in g.out, g.out.decode(errors="replace"))
        except TimeoutError as e:
            check("a results log that cannot be written ends the run at once, saying so", False, str(e))

        # The log's directory is refused when it is a symlink or another
        # user's, as vt-demo.sh refuses it for kmscon's log.
        real = os.path.join(tmp, "real")
        os.makedirs(real)
        os.symlink(real, os.path.join(tmp, "linked"))
        dirs = [(os.path.join(tmp, "linked"), "symlinked")]
        if os.getuid() != 0:
            dirs.append(("/usr", "someone else's"))
        for d, what in dirs:
            g = Pty(["/bin/bash", runner, guide], guide_env(os.path.join(d, "vt-check.log")))
            try:
                g.wait_for(b"Press Enter to end.")
                g.send(b"\r")
                status = g.finish()
                check(f"a {what} results log directory is refused before anything is asked",
                      status == 1 and f"{d} is a symlink, or not a directory of yours".encode() in g.out
                      and b"Step 1" not in g.out and not os.listdir(real), g.out.decode(errors="replace"))
            except TimeoutError as e:
                check(f"a {what} results log directory is refused before anything is asked", False, str(e))

        # --check reads the guide without a terminal and refuses a broken one
        # by its line.
        def lint(text, **env):
            path = os.path.join(tmp, "lint.guide")
            with open(path, "w") as f:
                f.write(text)
            return subprocess.run(["bash", runner, "--check", path], capture_output=True, text=True,
                                  timeout=30, env=dict(os.environ, **env))

        p = lint(GUIDE)
        check("--check lists the steps and their probes",
              p.returncode == 0 and p.stdout.splitlines() == [
                  "Test guide: 4 steps",
                  "   1  focus        Focus",
                  "   2  decarm       Skipped",
                  "   3  (no probe)   Look only",
                  "   4  mouse        Not run"], p.stdout + p.stderr)
        step = "step: s\nprobe: focus\ndo: d\nexpect: e\n"
        for name, text, env, why in (
            ("no title", step, {}, "no title: line"),
            ("no steps", "title: t\n", {}, "no steps"),
            ("a second title", "title: t\ntitle: u\n" + step, {}, "lint.guide:2: one title"),
            ("an intro after a step", "title: t\n" + step + "intro: late\n", {}, "lint.guide:6: intro lines go before"),
            ("an unknown key", "title: t\n" + step + "note: x\n", {}, "lint.guide:6: unknown key 'note'"),
            ("a line without a key", "title: t\nstep s\n", {}, "lint.guide:2: not a 'key: text' line"),
            ("an unknown probe mode", "title: t\nstep: s\nprobe: nope\ndo: d\nexpect: e\n", {},
             "lint.guide:3: probe mode 'nope' is not one of: focus focus-later decarm mouse bg"),
            ("a step without expect:", "title: t\nstep: s\ndo: d\n", {}, "step 1 (s) needs do: and expect: lines"),
            ("Logo for the Super key", "title: t\nstep: s\ndo: Ctrl+Logo+Return\nexpect: e\n", {},
             "lint.guide:3: name the Super (Windows) key, not Logo"),
            ("logo in lower case", "title: t\nstep: s\ndo: press logo+Left\nexpect: e\n", {},
             "lint.guide:3: name the Super (Windows) key, not Logo"),
            ("LOGO in capitals", "title: t\nintro: the LOGO key\nstep: s\ndo: d\nexpect: e\n", {},
             "lint.guide:2: name the Super (Windows) key, not Logo"),
            ("GUIDE_STEPS past the last step", "title: t\n" + step, {"GUIDE_STEPS": "1,2"},
             "GUIDE_STEPS: '2' is not a step of this guide (1 to 1)"),
            ("GUIDE_STEPS of 0", "title: t\n" + step, {"GUIDE_STEPS": "0"}, "GUIDE_STEPS: '0'"),
        ):
            p = lint(text, **env)
            check(f"--check refuses {name}", p.returncode != 0 and why in p.stderr, p.stdout + p.stderr)
        p = lint("title: t\nstep: s\ndo: Logos and SuperLogo stay\nexpect: e\n")
        check("--check takes Logo inside another word", p.returncode == 0, p.stderr)

        for name in sorted(os.listdir(checks)):
            if name.endswith(".guide"):
                p = subprocess.run(["bash", runner, "--check", os.path.join(checks, name)],
                                   capture_output=True, text=True, timeout=30)
                check(f"fork/demo/checks/{name} passes --check", p.returncode == 0, p.stdout + p.stderr)

        # probe.py splits a read at each ESC, so two focus reports read
        # together get a line and a label each.
        spec = importlib.util.spec_from_file_location("probe", probe_py)
        probe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        for data, want in (
            (b"\x1b[O\x1b[I", [b"\x1b[O", b"\x1b[I"]),
            (b"a", [b"a"]),
            (b"ab\x1b[B", [b"ab", b"\x1b[B"]),
            (b"\x1b\r", [b"\x1b\r"]),
            (b"\x1b\x1b[I", [b"\x1b", b"\x1b[I"]),
            (b"\x1b[<0;1;1M\x1b[<0;1;1m", [b"\x1b[<0;1;1M", b"\x1b[<0;1;1m"]),
        ):
            check(f"probe.split({data!r})", probe.split(data) == want, repr(probe.split(data)))
        check("probe labels focus in and out only",
              [probe.label(s) for s in (b"\x1b[I", b"\x1b[O", b"\x1b[1;7D", b"I")]
              == ["FOCUS IN", "FOCUS OUT", "", ""])

        # bg reads while the session is in the background, so a focus report
        # shows when it arrives, before setForeground; q there still sends
        # setForeground before the probe ends.
        plog = os.path.join(tmp, "probe.log")
        p = Pty([sys.executable, probe_py, "bg"], {"PATH": os.environ["PATH"], "PROBE_LOG": plog})
        try:
            at = p.wait_for(b"sending OSC setBackground", timeout=5)
            at = p.wait_for(b"\x1b]setBackground\x07", after=at)
            p.send(b"\x1b[O")
            at = p.wait_for(b"FOCUS OUT", after=at)
            p.wait_for(b"sent OSC setForeground", timeout=10, after=at)
            p.send(b"q")
            status = p.finish()
            out = read(plog)
            t_out = float(re.search(r"(\d+\.\d+)  #1    FOCUS OUT", out).group(1))
            t_fg = float(re.search(r"(\d+\.\d+)  sent OSC setForeground", out).group(1))
            check("probe bg shows a report read during the background before setForeground",
                  status == 0 and t_out < 2.5 and t_fg >= 5.9
                  and out.index("FOCUS OUT") < out.index("sent OSC setForeground"), out)
        except (TimeoutError, AttributeError) as e:
            check("probe bg shows a report read during the background before setForeground", False, str(e))

        p = Pty([sys.executable, probe_py, "bg"], {"PATH": os.environ["PATH"]})
        try:
            at = p.wait_for(b"\x1b]setBackground\x07", timeout=5)
            p.send(b"q")
            start = time.monotonic()
            status = p.finish()
            tail = p.out[at:]
            check("probe bg quit during the background sends setForeground and resets 1004",
                  status == 0 and time.monotonic() - start < 3
                  and 0 <= tail.find(b"\x1b]setForeground\x07") < tail.find(b"\x1b[?1004l") and b"bg off" in tail,
                  tail.decode(errors="replace"))
        except TimeoutError as e:
            check("probe bg quit during the background sends setForeground and resets 1004", False, str(e))

        p = subprocess.run([sys.executable, probe_py, "nope"], capture_output=True, text=True, timeout=30)
        check("probe refuses an unknown mode with its usage", p.returncode == 2 and "probe.py focus" in p.stderr,
              p.stderr)

    if failures:
        print(f"{len(failures)} failed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
