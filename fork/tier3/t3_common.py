"""Paths and logging shared by the tier 3 modules.

Paths are relative to the repository root, which is two levels above this file.
"""

import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HERE = os.path.join(ROOT, "fork", "tier3")
GUEST = os.path.join(HERE, "guest")
GOLDEN = os.path.join(HERE, "golden")
VM = os.path.join(ROOT, "fork", "vm")
PKGOUT = os.path.join(ROOT, "fork", "build", "pkg")
RUNDIR = os.path.join(ROOT, "fork", "build", "tier3")
REPORT = os.path.join(ROOT, "fork", "build", "tier3-report.json")
LOCK = os.path.join(ROOT, "fork", "build", "tier3.lock")
BASE = os.path.join(VM, "base.qcow2")


def rel(path):
    return os.path.relpath(path, ROOT) if path else None


class Logger:
    """Writes log lines to stderr.

    Every line carries the seconds since the run started and, inside a
    variant's job, the variant's name, so lines from concurrent variants can be
    told apart and phases timed from the log alone. A logger is passed to a
    worker process, so its lines count from the run's start too.
    """

    def __init__(self, t0=None, label=""):
        self.t0 = time.time() if t0 is None else t0
        self.label = label

    def labelled(self, label):
        """A logger for the same run whose lines carry `label`."""
        return Logger(self.t0, label)

    def __call__(self, msg):
        tag = "t3 +%.1fs" % (time.time() - self.t0)
        if self.label:
            tag += " " + self.label
        print("[%s] %s" % (tag, msg), file=sys.stderr, flush=True)
