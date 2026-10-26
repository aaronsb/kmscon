#!/usr/bin/env python3
"""The XTGETTCAP table kmscon gives libtsm, generated from kmscon.ti.

  gen_termcap.py [--tic PATH] [--infocmp PATH] SRC OUT          write OUT
  gen_termcap.py [--tic PATH] [--infocmp PATH] --check SRC OUT  fail if OUT
                                                                differs

The entry in SRC is compiled with tic -x into a temporary directory, its
capability names and kinds are read back with infocmp -x, and each value
with ncurses' tigetstr, tigetnum and tigetflag, so a string is in the form
libtsm.h asks for: escapes decoded to their bytes, % sequences as they are.
A number is its decimal text and a boolean has no value (NULL).

Beside the entry's own capabilities the table carries the names xterm's
XTGETTCAP answers that are not in a terminal description (xterm ctlseqs,
"Request Termcap/Terminfo String"): TN and name, the terminal's name, and
Co, termcap's colors. xterm 411 answers both spellings of each. lines and
cols are left out: they give a default screen size, and a program asking
the terminal wants its real one, which TIOCGWINSZ reports. Leaving them out
also keeps the output reproducible, since ncurses reads them from LINES and
COLUMNS when those are set; both are cleared before setupterm all the same.

A table libtsm would refuse fails here, naming the capability, instead of
at runtime: a name over 32 bytes, a value over 256 or holding a NUL, more
than 1024 entries, or a string or number ncurses cannot read.

make termcap rewrites OUT in the dev image; make test runs --check, so a
change to kmscon.ti that is not regenerated fails there.
"""

import argparse
import curses
import difflib
import os
import subprocess
import sys
import tempfile

TERM = "kmscon"
# capabilities of the entry the table leaves out, and why (above)
SKIP = {"lines", "cols"}
# libtsm's bounds (TSM_VTE_TERMCAP_MAX, _NAME_MAX, _VALUE_MAX in libtsm.h)
TERMCAP_MAX = 1024
NAME_MAX = 32
VALUE_MAX = 256


def read_entry(src, tic, infocmp):
    """{name: (kind, what ncurses returns)} of @src's entry, kind "str",
    "num" or "bool"."""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([tic, "-x", "-o", tmp, src], check=True, capture_output=True)
        out = subprocess.run([infocmp, "-x", "-1", "-A", tmp, TERM], check=True,
                             capture_output=True, text=True).stdout
        kinds = {}
        for line in out.splitlines():
            if not line.startswith("\t"):
                continue
            cap = line.strip().rstrip(",")
            for sep, kind in (("=", "str"), ("#", "num"), ("@", None)):
                if sep in cap:
                    name = cap.split(sep, 1)[0]
                    break
            else:
                name, kind = cap, "bool"
            if kind:
                kinds[name] = kind

        # setupterm reads TERMINFO when it is called, and the size from
        # LINES and COLUMNS unless use_env says not to
        os.environ["TERMINFO"] = tmp
        os.environ.pop("LINES", None)
        os.environ.pop("COLUMNS", None)
        curses.use_env(False)
        with open(os.devnull) as null:
            curses.setupterm(TERM, null.fileno())
        caps = {}
        for name, kind in kinds.items():
            if kind == "str":
                caps[name] = (kind, curses.tigetstr(name))
            elif kind == "num":
                caps[name] = (kind, curses.tigetnum(name))
            else:
                caps[name] = (kind, None)
    return caps


def value(name, kind, got):
    """The table's value for @name: bytes, or None for a boolean; exits
    naming @name when ncurses could not read it."""
    if kind == "str":
        if not isinstance(got, bytes):
            sys.exit(f"gen_termcap: tigetstr({name!r}) returned {got!r}, not a string")
        return got
    if kind == "num":
        if got < 0:
            sys.exit(f"gen_termcap: tigetnum({name!r}) returned {got}, not a number")
        return str(got).encode()
    return None


def c_string(value):
    """@value as a C string literal: printable ASCII as it is, the rest octal,
    and '?' escaped so that no trigraph can form."""
    out = []
    for b in value:
        if b in b'"\\?':
            out.append("\\" + chr(b))
        elif 0x20 <= b < 0x7f:
            out.append(chr(b))
        else:
            out.append(f"\\{b:03o}")
    return '"' + "".join(out) + '"'


def render(caps, src):
    table = {n: value(n, *v) for n, v in caps.items() if n not in SKIP}
    table["TN"] = TERM.encode()
    table["name"] = TERM.encode()
    if "colors" in table:
        table["Co"] = table["colors"]
    if len(table) > TERMCAP_MAX:
        sys.exit(f"gen_termcap: {len(table)} entries, over libtsm's {TERMCAP_MAX}")
    for name, v in table.items():
        if len(name) > NAME_MAX:
            sys.exit(f"gen_termcap: {name}: the name is {len(name)} bytes, over "
                     f"libtsm's {NAME_MAX}")
        if v is not None and len(v) > VALUE_MAX:
            sys.exit(f"gen_termcap: {name}: the value is {len(v)} bytes, over "
                     f"libtsm's {VALUE_MAX}")
        if v is not None and b"\0" in v:
            sys.exit(f"gen_termcap: {name}: the value holds a NUL")
    lines = [
        f"/* Generated by scripts/terminfo/gen_termcap.py from {src}; do not edit.",
        " * make termcap rewrites it, and make test fails when it differs from the",
        " * entry. The XTGETTCAP table terminal_vte.c gives libtsm (ADR-201's note",
        " * of 2026-10-26). */",
        "",
        "/* clang-format off */",
        "static const struct tsm_vte_termcap terminal_termcap[] = {",
    ]
    for name in sorted(table):
        v = table[name]
        lines.append(f"\t{{ {c_string(name.encode())}, "
                     f"{'NULL' if v is None else c_string(v)} }},")
    lines += ["};", "/* clang-format on */", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tic", default="tic")
    ap.add_argument("--infocmp", default="infocmp")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("src")
    ap.add_argument("out")
    args = ap.parse_args()

    # the path the header names, relative to the source root when it is under it
    shown = os.path.relpath(args.src, os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    text = render(read_entry(args.src, args.tic, args.infocmp), shown)
    if not args.check:
        with open(args.out, "w") as f:
            f.write(text)
        return 0

    with open(args.out) as f:
        old = f.read()
    if old == text:
        print(f"{args.out} matches {args.src}")
        return 0
    sys.stdout.writelines(difflib.unified_diff(
        old.splitlines(True), text.splitlines(True), args.out, "regenerated"))
    print(f"{args.out} differs from {args.src}: run make termcap")
    return 1


if __name__ == "__main__":
    sys.exit(main())
