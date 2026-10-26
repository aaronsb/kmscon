#!/usr/bin/env python3
"""Capability probes: what kmscon's terminal does and what it is missing.

  run.py --list FILE (--replay EXE --probes FILE --spec FILE [--fixture PATH]...
                      | --probe EXE [--image-probe EXE])
         [--build FEATURE=true|false]... [--font-probe EXE --fonts DIR]
         [--terminfo SRC --tic PATH --infocmp PATH [--tput PATH]]
  run.py --parity --list FILE --replay EXE --probes FILE --spec FILE
         [--fixture PATH]... --probe EXE --image-probe EXE

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

The terminal's probes are the stanzas of probes.replay, replayed by a
program that meets libtsm's replay interface (--spec, INTERFACE.md): libtsm's
tsm-replay, or vtium's. --describe says which interface, views and features
the program has; a probe that needs a feature it does not declare fails as
absent, and a program of another major version is refused. Each probe's
expectations are checked against the spec's grammar before anything runs.
--fixture names a file each probe's fixture directory holds, as `fixture
NAME` asks. The C probe programs, test_capabilities (libtsm, through its
public API) and test_capabilities_images (sixel, kitty graphics, XTVERSION
and XTGETTCAP through kmscon's own configuration and vte), are the same
probes; --parity runs both sources and fails when a probe passes in one and
fails in the other.

The rest come from test_capabilities_font (the freetype backend) and the
terminfo entry, compiled with tic and read back with infocmp, for the
capabilities listed in TERMINFO below, and expanded with tput, for the
strings listed in TERMINFO_STRINGS. A source that was not built or has no
tool is reported as skipped, probe by probe.
"""

import argparse
import os
import re
import shutil
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


# probes.replay (README.md, The replay probes): the keys that build the input,
# and the assertions, each judged against the block of the last check
INPUT_KEYS = ("send", "cmd", "check")
VIEW_FEATURES = {"screen": None, "frame": "view-frame", "draw2": "view-draw2"}
# the escapes of INTERFACE.md's worked examples, each one byte
ESCAPES = {"e": b"\x1b", "r": b"\r", "n": b"\n", "t": b"\t", "\\": b"\\", '"': b'"'}


def unquote(text):
    """The bytes of a "..." string written with ESCAPES and \\xHH, or None."""
    if len(text) < 2 or text[0] != '"' or text[-1] != '"':
        return None
    out, i, body = b"", 0, text[1:-1]
    while i < len(body):
        c = body[i]
        if c == '"':
            return None
        if c != "\\":
            out += c.encode()
            i += 1
        elif body[i + 1:i + 2] in ESCAPES:
            out += ESCAPES[body[i + 1]]
            i += 2
        elif re.fullmatch(r"x[0-9a-fA-F]{2}", body[i + 1:i + 4]):
            out += bytes([int(body[i + 2:i + 4], 16)])
            i += 4
        else:
            return None
    return out


def shown(data):
    """@data printable, ESC as \\e and other controls as \\xNN."""
    text = data.decode("utf-8", "backslashreplace")
    return "".join("\\e" if c == "\x1b" else f"\\x{ord(c):02x}" if ord(c) < 0x20 or c == "\x7f"
                   else c for c in text)


def fenced(path, name):
    """The lines of the fenced block @name in @path."""
    lines, inside = [], False
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line == "```" + name:
                inside = True
            elif inside and line == "```":
                return lines
            elif inside:
                lines.append(line)
    sys.exit(f"{path}: no ```{name} block")


class Spec:
    """INTERFACE.md's describe and grammar blocks."""

    def __init__(self, path):
        describe = fenced(path, "describe")
        self.major = describe[0].split()[1].split(".")[0]
        self.features = {line.split()[1] for line in describe if line.startswith("feature ")}
        self.commands = {}
        for line in describe:
            if line.startswith("command "):
                words = line.split()
                self.commands[words[1]] = words[2] if len(words) > 2 else None

        defines, reserved = {}, {}
        self.lines = {}  # kind -> [(views, regex, regex with the reserved keys)]

        def expand(regex, with_reserved):
            def fragment(m):
                name = m.group(1)
                if name in reserved:
                    return f"(?:{reserved[name] if with_reserved else ''})"
                return f"(?:{defines[name][with_reserved]})"
            return re.sub(r"\{([A-Z][A-Z0-9]*)\}", fragment, regex)

        for line in fenced(path, "grammar"):
            word, rest = line.split(" ", 1)
            if word == "define":
                name, regex = rest.split(" ", 1)
                defines[name] = (expand(regex, False), expand(regex, True))
            elif word == "reserved":
                name, regex = rest.split(" ", 1)
                reserved[name] = regex
            elif word == "line":
                kind, views, regex = rest.split(" ", 2)
                self.lines.setdefault(kind, []).append(
                    ("screen,draw2,frame" if views == "*" else views,
                     expand(regex, False), expand(regex, True)))

    def parses(self, line, view, with_reserved):
        """Whether @line is a line the grammar allows in @view."""
        for views, plain, reserved in self.lines.get(line.split(" ", 1)[0], ()):
            if view in views.split(",") and re.fullmatch(reserved if with_reserved else plain,
                                                         line):
                return True
        return False


class Probe:
    def __init__(self, name, n):
        self.name, self.line = name, n
        self.size, self.view, self.placeholder = None, "screen", None
        self.needs, self.fixtures = set(), []
        self.steps = []  # (key, argument, line number), in file order


def read_probes(path, spec):
    """{name: Probe} from probes.replay, each checked for form here."""
    probes, probe = {}, None
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            key, _, arg = line.partition(" ")
            where = f"{path}:{n}"
            if key == "probe":
                if arg in probes:
                    sys.exit(f"{where}: {arg} is already a probe, on line {probes[arg].line}")
                probe = probes[arg] = Probe(arg, n)
                continue
            if probe is None:
                sys.exit(f"{where}: {key} before the first probe line")
            if key == "size":
                if not re.fullmatch(r"[1-9][0-9]* [1-9][0-9]*", arg):
                    sys.exit(f"{where}: want size COLS ROWS")
                probe.size = tuple(int(x) for x in arg.split())
            elif key == "view":
                if arg not in VIEW_FEATURES:
                    sys.exit(f"{where}: view {arg!r} is not one of {', '.join(VIEW_FEATURES)}")
                probe.view = arg
            elif key == "needs":
                for feature in arg.split():
                    if feature not in spec.features:
                        sys.exit(f"{where}: {feature} is not a feature of the spec")
                    probe.needs.add(feature)
            elif key == "fixture":
                probe.fixtures.append(arg)
            elif key == "placeholder":
                probe.placeholder = arg
            elif key == "cmd":
                if arg.split(";", 1)[0] not in spec.commands:
                    sys.exit(f"{where}: {arg.split(';', 1)[0]} is not a command of the spec")
                probe.steps.append((key, arg, n))
            elif key in ("send", "reply", "reply-re"):
                data = unquote(arg)
                if data is None:
                    sys.exit(f'{where}: want {key} "..." with \\e, \\r, \\n, \\t, \\\\, '
                             f'\\" or \\xHH escapes')
                if key == "reply-re":
                    try:
                        data = re.compile(data, re.DOTALL)
                    except re.error as e:
                        sys.exit(f"{where}: {e}")
                probe.steps.append((key, data, n))
            elif key in ("want-re", "reject-re"):
                try:
                    probe.steps.append((key, re.compile(arg), n))
                except re.error as e:
                    sys.exit(f"{where}: {e}")
            elif key in ("check", "want", "reject"):
                probe.steps.append((key, arg, n))
            else:
                sys.exit(f"{where}: {key!r} is not a key of probes.replay")

    for probe in probes.values():
        where = f"{path}:{probe.line}: {probe.name}"
        keys = [s[0] for s in probe.steps]
        if probe.placeholder is not None:
            if probe.steps:
                sys.exit(f"{where}: a placeholder has nothing to run")
            continue
        if probe.size is None:
            sys.exit(f"{where}: no size")
        if not any(k in ("want", "want-re", "reply", "reply-re") for k in keys):
            sys.exit(f"{where}: no want or reply, so nothing it prints could fail it")
        checked = False
        for key, arg, n in probe.steps:
            if key in INPUT_KEYS:
                checked = key == "check"
            elif not checked:
                sys.exit(f"{path}:{n}: {key} after input with no check between")
    return probes


# what a runner masks unless both sides declare damage-exact: a nonzero
# changed count, and a placement's changed flag (INTERFACE.md, Comparing
# with the goldens)
DAMAGE = re.compile(r"(?:changed|test frame-since [0-7] changed) [1-9][0-9]*|placement .* changed")


def check_expectations(path, probe, spec, missing):
    """Exits naming the line when a want or reject line is one the spec's
    grammar does not allow in the probe's view; the keys the spec reserves
    are allowed in a missing probe's expectations only, and an exact damage
    count or flag in a probe that does not need damage-exact."""
    for key, arg, n in probe.steps:
        if key not in ("want", "reject"):
            continue
        if not spec.parses(arg, probe.view, missing):
            sys.exit(f"{path}:{n}: {arg!r} is not a {probe.view} view line of the spec's "
                     f"grammar{'' if missing else ' (reserved keys are for missing probes)'}")
        if DAMAGE.fullmatch(arg) and "damage-exact" not in probe.needs:
            sys.exit(f"{path}:{n}: {arg!r} is informative unless damage-exact: tell zero "
                     f"from nonzero, or add needs damage-exact")


class Replay:
    """A replay implementation, judged by INTERFACE.md."""

    def __init__(self, exe, spec, fixtures, tmp):
        self.exe, self.spec, self.tmp = exe, spec, tmp
        self.fixtures = {os.path.basename(p): p for p in fixtures}
        res = subprocess.run([exe, "--describe"], capture_output=True, text=True)
        lines = res.stdout.splitlines()
        version = lines[0].split() if lines else []
        if (res.returncode or len(version) != 2 or version[0] != "interface"
                or version[1].split(".")[0] != spec.major):
            sys.exit(f"{exe} --describe: exit {res.returncode}, first line "
                     f"{lines[0] if lines else 'none'!r}; want interface {spec.major}.N")
        self.views = {line.split()[1] for line in lines if line.startswith("view ")}
        self.features = {line.split()[1] for line in lines if line.startswith("feature ")}
        self.commands = {line.split()[1] for line in lines if line.startswith("command ")}

    def absent(self, probe):
        """What the implementation lacks for @probe, or None."""
        if probe.view not in self.views:
            return f"the implementation has no {probe.view} view"
        needs = set(probe.needs)
        if VIEW_FEATURES[probe.view]:
            needs.add(VIEW_FEATURES[probe.view])
        for key, arg, _ in probe.steps:
            if key == "cmd":
                name = arg.split(";", 1)[0]
                if name not in self.commands:
                    return f"the implementation has no {name} command"
                if self.spec.commands[name]:
                    needs.add(self.spec.commands[name])
        lacking = sorted(needs - self.features)
        if lacking:
            return f"the implementation does not declare {', '.join(lacking)}"
        return None

    def run(self, probe):
        """(passed, reason) for @probe; passed is None when the replay could
        not judge it."""
        if probe.placeholder is not None:
            return False, f"placeholder: {probe.placeholder}"
        absent = self.absent(probe)
        if absent:
            return False, f"absent: {absent}"

        data, cuts = b"", []
        for key, arg, _ in probe.steps:
            if key == "send":
                data += arg
            elif key == "cmd":
                data += b"\x1b_tsm-test;" + arg.encode() + b"\x1b\\"
            elif key == "check":
                cuts.append(len(data))
        base = os.path.join(self.tmp, probe.name)
        with open(base + ".bin", "wb") as f:
            f.write(data)
        with open(base + ".cuts", "w") as f:
            f.writelines(f"{c}\n" for c in cuts)
        os.makedirs(base + ".files", exist_ok=True)
        for name in probe.fixtures:
            shutil.copy(self.fixtures[name], base + ".files")
        view = [] if probe.view == "screen" else ["--" + probe.view]
        res = subprocess.run([self.exe, *view, *map(str, probe.size), base + ".bin",
                              base + ".cuts"], capture_output=True)
        if res.returncode:
            return None, (f"the replay exited {res.returncode}: "
                          f"{res.stderr.decode(errors='replace').strip()}")

        # a test line belongs to the block whose checkpoint follows it
        blocks, pending = [], []
        for line in res.stdout.decode(errors="replace").splitlines():
            if line.startswith("test "):
                pending.append(line)
            elif line.startswith("checkpoint "):
                blocks.append([line] + pending)
                pending = []
            elif blocks:
                blocks[-1].append(line)
        if len(blocks) != len(cuts) + 1:
            return False, f"the replay printed {len(blocks)} blocks, want {len(cuts) + 1}"
        for k, cut in enumerate(cuts):
            if blocks[k][0] != f"checkpoint {cut}":
                return False, f"block {k + 1} is {blocks[k][0]!r}, want 'checkpoint {cut}'"

        k = -1
        for key, arg, n in probe.steps:
            if key == "check":
                k += 1
                continue
            if key in INPUT_KEYS:
                continue
            block = blocks[k]
            reply = b"".join(bytes.fromhex(line.split()[1]) for line in block
                             if line.startswith("reply "))
            what = f"line {n}, block {k + 1}"
            if key == "want" and arg not in block:
                kind = " ".join(arg.split()[:2])
                near = [line for line in block if line.startswith(kind + " ")]
                return False, f"{what}: no {arg!r}; " + (
                    f"it has {', '.join(map(repr, near))}" if near else f"no {kind!r} line")
            if key == "want-re" and not any(arg.fullmatch(line) for line in block):
                return False, f"{what}: no line matches {arg.pattern!r}"
            if key == "reject" and arg in block:
                return False, f"{what}: {arg!r}"
            if key == "reject-re":
                hit = [line for line in block if arg.fullmatch(line)]
                if hit:
                    return False, f"{what}: {hit[0]!r} matches {arg.pattern!r}"
            if key == "reply" and reply != arg:
                return False, f'{what}: replied "{shown(reply)}", want "{shown(arg)}"'
            if key == "reply-re" and not arg.fullmatch(reply):
                return False, (f'{what}: replied "{shown(reply)}", want a match of '
                               f'"{shown(arg.pattern)}"')
        return True, ""


def parity(c_runners, replay, probes):
    """Each probe's result from the C programs against the replay's."""
    differ = 0
    names = sorted(set(c_runners) | set(probes))
    for name in names:
        if name not in c_runners or name not in probes:
            print(f"DIFF {name}: only the {'replay' if name in probes else 'C'} probes have it")
            differ += 1
            continue
        missing_fixture = [f for f in probes[name].fixtures if f not in replay.fixtures]
        if missing_fixture:
            print(f"skip {name}: no --fixture {', '.join(missing_fixture)}")
            continue
        (c_passed, c_reason), (r_passed, r_reason) = c_runners[name](), replay.run(probes[name])
        word = {True: "passes", False: "fails", None: "cannot run"}
        if c_passed is not None and c_passed == r_passed:
            print(f"same {name}: both {'pass' if c_passed else 'fail'}")
            continue
        print(f"DIFF {name}: the C probe {word[c_passed]}{': ' + c_reason if c_reason else ''}; "
              f"the replay {word[r_passed]}{': ' + r_reason if r_reason else ''}")
        differ += 1
    print(f"{len(names)} probes: {len(names) - differ} agree, {differ} differ")
    return 1 if differ else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", required=True)
    ap.add_argument("--replay")
    ap.add_argument("--probes")
    ap.add_argument("--spec")
    ap.add_argument("--fixture", action="append", default=[])
    ap.add_argument("--probe")
    ap.add_argument("--image-probe")
    ap.add_argument("--parity", action="store_true")
    ap.add_argument("--build", action="append", default=[])
    ap.add_argument("--font-probe")
    ap.add_argument("--fonts")
    ap.add_argument("--terminfo")
    ap.add_argument("--tic")
    ap.add_argument("--infocmp")
    ap.add_argument("--tput")
    args = ap.parse_args()
    args.list = os.path.normpath(args.list)
    if args.replay and not (args.probes and args.spec):
        sys.exit("--replay needs --probes and --spec")
    if args.parity and not (args.replay and args.probe and args.image_probe):
        sys.exit("--parity needs --replay and the C probe programs, --probe and --image-probe")
    if not args.parity and bool(args.replay) == bool(args.probe):
        sys.exit("give the probes' source: --replay, or the C programs with --probe")

    entries = read_list(args.list)
    runners = {}  # probe name -> callable returning (passed, reason)
    skipped = {}  # probe name -> why it cannot run here

    c_runners = {}
    for program in filter(None, (args.probe, args.image_probe)):
        for name in program_probes([program]):
            c_runners[name] = lambda name=name, program=program: run_program([program], name)

    if args.replay:
        tmp = tempfile.TemporaryDirectory()
        spec = Spec(args.spec)
        probes = read_probes(args.probes, spec)
        missing = {e[0] for e in entries if e[1] == "missing"}
        for probe in probes.values():
            check_expectations(args.probes, probe, spec, probe.name in missing)
        replay = Replay(args.replay, spec, args.fixture, tmp.name)
        if args.parity:
            return parity(c_runners, replay, probes)
        for name, probe in probes.items():
            lacking = [f for f in probe.fixtures if f not in replay.fixtures]
            if lacking:
                skipped[name] = f"no --fixture {', '.join(lacking)}"
            runners[name] = lambda probe=probe: replay.run(probe)
    else:
        runners.update(c_runners)

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
