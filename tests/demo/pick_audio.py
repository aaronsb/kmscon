#!/usr/bin/env python3
"""Checks fork/demo/pick-audio.sh, which asks where a demo's sound plays
(make vt-login's login session runs it by hand), with stand-ins for mpv,
pgrep, getent and id and a sound card directory of its own: the outputs it
lists, the answers it takes, AUDIO_DEVICE and --no-wait, and the one line
saying why when it finds no output (#171, #207). These checks were part of
the Bad Apple!! showcase's tests until the show moved to
aaronsb/kmscon-showcase (#239); the picker stays here.

    pick_audio.py --root SOURCE_ROOT
"""

import argparse
import os
import subprocess
import sys
import tempfile

failures = []


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name)
    if not ok:
        failures.append(name)
        if detail:
            print("      " + detail.replace("\n", "\n      "))


PULSE_LIST = """List of detected audio devices:
  'auto' (Autoselect device)
  'pulse' (Default (pulseaudio))
  'pulse/alsa_output.pci-0000_00_1f.3.analog-stereo' (Built-in Audio Analog Stereo)
  'alsa' (Default (alsa))
"""

# PipeWire with no sink but its dummy, as when it lost the sound cards (#171).
SINKLESS_LIST = """List of detected audio devices:
  'auto' (Autoselect device)
  'pipewire' (Default (pipewire))
  'pipewire/auto_null' (Dummy Output)
  'pulse' (Default (pulseaudio))
  'pulse/auto_null' (Dummy Output)
  'alsa' (Default (alsa))
  'alsa/pipewire' (PipeWire Sound Server)
"""


def picker_checks(root):
    """fork/demo/pick-audio.sh, the demos' sound output picker, with stand-ins
    for mpv (a PulseAudio desktop's list), pgrep (no PipeWire running, or
    $PW_PID), getent and, when ID_G is set, id (user tester, uid 1000, the
    groups in ID_G), and a sound card directory of its own."""
    picker = os.path.join(root, "fork/demo/pick-audio.sh")
    with tempfile.TemporaryDirectory() as tmp:
        stubs = os.path.join(tmp, "bin")
        os.mkdir(stubs)
        stub = {"mpv": "#!/bin/sh\nfor a; do [ \"$a\" = --audio-device=help ] && { cat \"$LIST\"; exit 0; }; done\nexit 0\n",
                "pgrep": "#!/bin/sh\n[ -n \"$PW_PID\" ] || exit 1\necho \"$PW_PID\"\n",
                "getent": "#!/bin/sh\n[ -n \"$GROUP\" ] || exit 2\necho \"$GROUP\"\n",
                "id": "#!/bin/sh\n[ -n \"$ID_G\" ] || exec /usr/bin/id \"$@\"\n"
                      "case $1 in -un) echo tester ;; -u) echo 1000 ;; -G) echo \"$ID_G\" ;; *) exec /usr/bin/id \"$@\" ;; esac\n"}
        for name, body in stub.items():
            with open(os.path.join(stubs, name), "w") as f:
                f.write(body)
            os.chmod(os.path.join(stubs, name), 0o755)
        listing = os.path.join(tmp, "list")
        with open(listing, "w") as f:
            f.write(PULSE_LIST)
        snd = os.path.join(tmp, "snd")
        os.mkdir(snd)
        open(os.path.join(snd, "controlC0"), "w").close()
        # A directory holding pulse/cookie, as ~/.config does, must not turn
        # the pattern pulse/* into a file name.
        cwd = os.path.join(tmp, "cwd")
        os.makedirs(os.path.join(cwd, "pulse"))
        open(os.path.join(cwd, "pulse", "cookie"), "w").close()

        def pick(stdin, *args, group="audio:x:995:", **extra):
            env = {"PATH": stubs + ":/usr/bin:/bin", "HOME": tmp, "LIST": listing,
                   "GROUP": group, "PICK_AUDIO_SND": snd, **extra}
            return subprocess.run(["bash", picker, *args], input=stdin, env=env, cwd=cwd,
                                  capture_output=True, text=True, timeout=30)

        p = pick("0\n")
        check("the picker lists a PulseAudio desktop's sinks with no PipeWire running, and 0 is no sound",
              p.returncode == 0 and p.stdout == "none\n" and "Built-in Audio Analog Stereo" in p.stderr
              and "Default (pulseaudio)" in p.stderr, p.stdout + p.stderr)
        p = pick("0\n", group="")
        check("the picker runs on a host with no audio group",
              p.returncode == 0 and p.stdout == "none\n" and "Built-in Audio" in p.stderr, p.stdout + p.stderr)
        p = pick("2\ny\n")
        check("a number and y keep that output",
              p.returncode == 0 and p.stdout == "pulse/alsa_output.pci-0000_00_1f.3.analog-stereo\n",
              p.stdout + p.stderr)
        p = pick("", "--no-wait")
        check("--no-wait asks nothing and plays through mpv's default output",
              p.returncode == 0 and p.stdout == "auto\n" and "--no-wait" in p.stderr, p.stdout + p.stderr)
        p = pick("", AUDIO_DEVICE="pulse/x")
        check("AUDIO_DEVICE answers without a question", p.returncode == 0 and p.stdout == "pulse/x\n",
              p.stdout + p.stderr)
        # A device name typed in place of a number is tried and kept (#207).
        p = pick("pulse/typed.sink\ny\n")
        check("a device name typed at the list is tried and kept",
              p.returncode == 0 and p.stdout == "pulse/typed.sink\n" and "tone through pulse/typed.sink" in p.stderr,
              p.stdout + p.stderr)
        p = pick("a b\n$(id)\n0\n")
        check("a name that is no mpv device name is asked again",
              p.returncode == 0 and p.stdout == "none\n" and "tone through" not in p.stderr,
              p.stdout + p.stderr)

        # No outputs (#207): one line says why, and a device name can still
        # be typed. PipeWire with no sink but its dummy is #171's case, the
        # cards dropped by a PipeWire that started before the audio group.
        sinkless = os.path.join(tmp, "sinkless")
        with open(sinkless, "w") as f:
            f.write(SINKLESS_LIST)
        proc = os.path.join(tmp, "proc")
        os.makedirs(os.path.join(proc, "4242"))

        def pipewire_groups(groups):
            with open(os.path.join(proc, "4242", "status"), "w") as f:
                f.write(f"Name:\tpipewire\nGroups:\t{groups}\n")

        def why(p):
            """The one line saying why there is no output, or ''."""
            lines = [l for l in p.stderr.splitlines() if "finds no sound output" in l]
            return lines[0] if len(lines) == 1 else ""

        empty = {"LIST": sinkless, "PICK_AUDIO_PROC": proc}
        p = pick("\n", **empty)
        check("no outputs: Enter is no sound, and the cards' note above a list is left out",
              p.returncode == 0 and p.stdout == "none\n" and why(p)
              and "below" not in p.stderr, p.stdout + p.stderr)
        p = pick("pipewire/typed\ny\n", **empty)
        check("no outputs: a device name typed is tried and kept",
              p.returncode == 0 and p.stdout == "pipewire/typed\n", p.stdout + p.stderr)
        p = pick("", "--no-wait", **empty)
        check("no outputs with --no-wait: no sound, and the line says why",
              p.returncode == 0 and p.stdout == "none\n" and why(p), p.stdout + p.stderr)
        pipewire_groups("1000")
        # The group is blamed only for cards out of reach: cards this login
        # opens through their ACLs need none. Root opens any card, so the
        # cases with cards out of reach run only without root.
        cases = [("in this login's audio group but not PipeWire's",
                  dict(GROUP="audio:x:995:tester", ID_G="1000 995", PW_PID="4242"),
                  "your PipeWire started before you joined"),
                 ("in /etc/group's audio group, not this login's, with the cards in reach",
                  dict(GROUP="audio:x:995:tester", ID_G="1000"), "mpv --audio-device=help")]
        if os.geteuid() != 0:
            cases += [("in /etc/group's audio group but not this login's, with the cards out of reach",
                       dict(GROUP="audio:x:995:tester", ID_G="1000", CARDS_MODE="0"),
                       "this login started before you joined"),
                      ("outside the audio group with the cards out of reach",
                       dict(GROUP="audio:x:995:", ID_G="1000", CARDS_MODE="0"), "sudo usermod -aG audio tester")]
        cases += [("with no sound card", dict(GROUP="audio:x:995:", ID_G="1000", PICK_AUDIO_SND=os.path.join(tmp, "nosnd")),
                   "no sound card"),
                  ("with nothing else to blame", dict(GROUP="audio:x:995:", ID_G="1000"), "mpv --audio-device=help")]
        os.makedirs(os.path.join(tmp, "nosnd"))
        for name, extra, reason in cases:
            os.chmod(os.path.join(snd, "controlC0"), int(extra.pop("CARDS_MODE", "644"), 8))
            p = pick("\n", **empty, **extra)
            check(f"no outputs, {name}: one line says so", p.returncode == 0 and reason in why(p),
                  p.stdout + p.stderr)
        os.chmod(os.path.join(snd, "controlC0"), 0o644)

        if os.geteuid() != 0:
            os.chmod(os.path.join(snd, "controlC0"), 0)
            p = pick("0\n")
            check("unreachable sound cards are named, with the audio group's fix",
                  p.returncode == 0 and "out of reach" in p.stderr and "usermod -aG audio" in p.stderr,
                  p.stdout + p.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    picker_checks(os.path.abspath(ap.parse_args().root))
    if failures:
        print(f"{len(failures)} failed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
