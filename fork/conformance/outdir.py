#!/usr/bin/env python3
"""The output directory of a page generator.

A generator empties part of its output directory before writing: compare.py
save replaces the whole directory, and the page writers delete the images a
previous page left. Each generator leaves a marker file in every directory
it writes and reuses an existing directory only when that directory is
empty or carries the marker, so a mistyped SAVE= or OUT= can never empty a
directory the generator did not make.

SPDX-License-Identifier: MIT
"""

from pathlib import Path


def claim(who, out, marker, inputs=()):
    """Make `out` ready to write and mark it, or exit saying why not.

    Refused: a directory holding the current directory or any of `inputs`,
    and an existing, non-empty directory without `marker`. Nothing is
    deleted here; the caller clears what it rewrites only after this returns.
    """
    out = Path(out)
    real = out.resolve()
    for held in [Path.cwd(), *map(Path, inputs)]:
        held = held.resolve()
        if held == real or real in held.parents:
            raise SystemExit("%s: refusing to write into %s: it holds %s" % (who, out, held))
    if out.exists() and not out.is_dir():
        raise SystemExit("%s: refusing to write into %s: it is not a directory" % (who, out))
    if out.exists() and any(out.iterdir()) and not (out / marker).exists():
        raise SystemExit("%s: refusing to write into %s: it was not written by %s (no %s); "
                         "remove it or choose another directory" % (who, out, who, marker))
    out.mkdir(parents=True, exist_ok=True)
    (out / marker).write_text("Written by %s, which may replace this directory's contents.\n" % who)
    return out
