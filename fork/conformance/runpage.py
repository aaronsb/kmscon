#!/usr/bin/env python3
"""Write one conformance run as a markdown page, and record it in the ledger.

A run page is a README.md with PNG images beside it, in a directory named for
the run. It says what the run marks and measured, how it differs from the
previous ledger run, and for each tier what the tier checks and every case's
verdict. Tier 1 is text: a table of cases, and a text diff for a failure.
Tiers 2 and 3 show images cropped to the drawn part of the screen: a passing
case shows its screen; a failing case, and one whose golden changed since the
previous run, shows golden, actual and difference side by side.

The page is drawn only from its inputs, and identical inputs give
byte-identical files.

Usage:
  runpage.py --out DIR --kmscon REV --libtsm REV --date YYYY-MM-DD
             [--ledger LEDGER.md [--append --note TEXT]]
             [--history DIR] [--fork-goldens LIST] REPORT.json...

--ledger names the ledger whose last row is the previous run; --append adds
this run's row to it. --history is the directory golden-history.sh writes:
the previous run's commits, the goldens changed since, and the previous copy
of each. --fork-goldens is libtsm's list of tier 1 cases whose golden comes
from a fork commit, with the reason for each.

Paths in the reports are read relative to the current directory, which is the
repository root under `make`.

SPDX-License-Identifier: MIT
"""

import argparse
import difflib
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from outdir import claim  # noqa: E402
from pageimg import crop, crop_box, diff_image, load, step_for, strip, write_png  # noqa: E402

OK = ("pass", "near")
DIFF_LINES = 40         # a tier 1 text diff is cut after this many lines
LIBTSM_URL = "https://github.com/aaronsb/libtsm"

TIERS = {
    1: ("screen state",
        "libtsm, the terminal-state library kmscon draws from, replays a byte stream "
        "(synthetic escape sequences, or a recorded session of a real program) and dumps "
        "the screen it ends with as text: each cell's character and attributes, and the "
        "cursor. A case passes when the dump equals its golden dump. Where the fork keeps "
        "upstream libtsm's behaviour the golden comes from upstream; where it changed the "
        "behaviour on purpose the golden comes from a fork commit, and the last column "
        "gives the reason ([fork-goldens.list]({list})).",
        "the cell grid libtsm's parser produces"),
    2: ("pixels in memory",
        "kmscon's renderer (bbulk, through the real drm2d blend) draws each screen into "
        "memory with pinned fonts, and the image is compared pixel by pixel with its "
        "golden. No display or GPU is involved, so these cases run in every `make test`.",
        "the renderer's pixels, drawn in memory"),
    3: ("end to end",
        "The fork is packaged, installed in a pinned Arch Linux virtual machine and started "
        "by systemd on a virtual GPU through KMS. A script logs in, draws each screen, "
        "captures the display and compares it exactly with the golden. Images here are "
        "cropped to the drawn part of the screen.",
        "the installed package on a real KMS device"),
}


# ---------------------------------------------------------------- markdown

def anchor(heading):
    """The fragment GitHub gives a heading."""
    text = re.sub(r"[^\w\- ]", "", heading.lower())
    return text.replace(" ", "-")


def cell(text):
    return text.replace("|", "\\|").replace("\n", " ")


def verdict_word(status):
    return status if status in OK else status.upper()


def case_heading(tier, name, status):
    """A case's heading; the tier keeps it unique, since tiers share names."""
    return "`%s` (tier %d): %s" % (name, tier, verdict_word(status))


# ---------------------------------------------------------------- inputs

def ledger_rows(path):
    """The ledger's rows as dicts, oldest first."""
    rows = []
    for line in Path(path).read_text().splitlines():
        if not re.match(r"\| \d{4}-\d\d-\d\d \|", line):
            continue
        cells = [c.strip().replace("\\|", "|") for c in re.split(r"(?<!\\)\|", line.strip())[1:-1]]
        date, note, kmscon, libtsm, t1, t2, t3, link = cells
        counts = {}
        for tier, text in ((1, t1), (2, t2), (3, t3)):
            m = re.match(r"(\d+)/(\d+)", text)
            if m:
                counts[tier] = (int(m.group(1)), int(m.group(2)))
        m = re.search(r"\]\(([^)]+)\)", link)
        rows.append({"date": date, "note": note, "kmscon": kmscon.strip("`"),
                     "libtsm": libtsm.strip("`"), "counts": counts,
                     "link": m.group(1) if m else None})
    return rows


def previous_run(ledger):
    """The ledger's last row, with the per-case record its page left, if any."""
    if not ledger or not Path(ledger).exists():
        return None
    rows = ledger_rows(ledger)
    if not rows:
        return None
    prev = rows[-1]
    link = prev["link"]
    # A run whose record has left the tree links to it on GitHub.
    prev["url"] = link if link and re.match(r"[a-z]+://", link) else None
    prev["path"] = Path(ledger).parent / link if link and not prev["url"] else None
    record = prev["path"].parent / "cases.json" if prev["path"] else None
    prev["cases"] = None
    if record and prev["path"].name == "README.md" and record.exists():
        prev["cases"] = {int(t): c for t, c in json.loads(record.read_text())["tiers"].items()}
    return prev


def read_history(path):
    """{(repo, path): status} of goldens changed since the previous run, the
    lines that say a repository's history could not be read, and the dir."""
    hist = {"changes": {}, "notes": [], "dir": Path(path) if path else None}
    tsv = Path(path) / "changes.tsv" if path else None
    if not tsv or not tsv.exists():
        return hist
    for line in tsv.read_text().splitlines():
        parts = line.split("\t")
        if parts[0] == "previous":
            continue
        repo, status, rest = parts[0], parts[1], parts[2]
        if status == "?":
            hist["notes"].append("%s: %s" % (repo, rest))
        else:
            hist["changes"][(repo, rest)] = status[0]
    return hist


def fork_goldens(path):
    """{case: reason} from libtsm's fork-goldens.list."""
    reasons = {}
    if not path or not Path(path).exists():
        return reasons
    for line in Path(path).read_text().splitlines():
        name, _, reason = line.partition("#")
        if name.strip():
            reasons[name.strip()] = reason.strip()
    return reasons


def provenance(reports, kmscon, libtsm):
    """Where a report says it measured other commits than the page records,
    or commits with uncommitted changes: a stale tier 3 report, say."""
    problems = []
    for rep in sorted(reports, key=lambda r: r["tier"]):
        for repo, want in (("kmscon", kmscon), ("libtsm", libtsm)):
            got = (rep.get("tools") or {}).get(repo)
            if not got:
                continue
            bare, _, dirty = got.partition("+")
            if not (bare.startswith(want) or want.startswith(bare)):
                problems.append("the tier %d report measured %s `%s`, not `%s`"
                                % (rep["tier"], repo, bare, want))
            elif dirty:
                problems.append("the tier %d report measured %s `%s` with uncommitted changes"
                                % (rep["tier"], repo, bare))
    return problems


def golden_keys(tier, case):
    """The (repo, path) of the files that judge a case: its golden and, for
    an image tier, the tolerance sidecar beside it."""
    if tier == 1:
        return [("libtsm", "test/conformance/golden/%s.txt" % case["name"])]
    if not case.get("golden"):
        return []
    g = case["golden"]
    return [("kmscon", g), ("kmscon", os.path.splitext(g)[0] + ".tol")]


# ---------------------------------------------------------------- the page

class Page:
    def __init__(self, args, reports):
        self.args = args
        self.out = Path(args.out)
        self.reports = {r["tier"]: r for r in reports}
        self.prev = previous_run(args.ledger)
        self.hist = read_history(args.history)
        self.reasons = fork_goldens(args.fork_goldens)
        self.problems = provenance(reports, args.kmscon, args.libtsm)
        self.lines = []
        self.images = []

    def w(self, *lines):
        self.lines.extend(lines)

    def link(self, path):
        return os.path.relpath(path, self.out).replace(os.sep, "/")

    # Change since the previous run, per case.
    def changed(self, tier, case):
        return sorted(p for k in golden_keys(tier, case)
                      for p in [k[1]] if self.hist["changes"].get(k) in ("M", "T"))

    def added(self, tier, case):
        keys = golden_keys(tier, case)
        return bool(keys) and self.hist["changes"].get(keys[0]) == "A"

    def prev_status(self, tier, case):
        """The case's verdict in the previous run: a status, None for a case
        new since then, or "?" when the previous run left no way to tell."""
        if self.prev is None:
            return "?"
        if self.prev["cases"] is not None:
            return self.prev["cases"].get(tier, {}).get(case["name"])
        counts = self.prev["counts"].get(tier)
        if counts is None or self.added(tier, case):
            return None
        # No per-case record: a tier that passed every case passed this one.
        return "pass" if counts[0] == counts[1] else "?"

    def counts(self):
        return {t: (sum(c["status"] in OK for c in r["cases"]), len(r["cases"]))
                for t, r in sorted(self.reports.items())}

    def render(self):
        self.header()
        self.since()
        for tier in sorted(self.reports):
            self.tier(tier)
        return "\n".join(self.lines).rstrip() + "\n"

    def header(self):
        a = self.args
        self.w("# Conformance run %s" % a.date, "")
        if a.note:
            self.w("**What this run marks:** %s" % a.note, "")
        harness = self.link("docs/fork/conformance.md")
        adr = next(iter(sorted(Path("docs/architecture/fork").glob("ADR-301-*.md"))), None)
        self.w("This page records one run of the kmscon fork's conformance harness ([how it works](%s)%s). "
               "Every case compares what the fork produced with a golden, the expected result committed "
               "after a person reviewed it, and passes when the two match exactly, or within a tolerance "
               "committed beside the golden (a *near* pass)." % (
                   harness, ", [ADR-301](%s)" % self.link(adr) if adr else ""), "")
        self.w("| Repository | Commit measured |", "|---|---|",
               "| kmscon | `%s` |" % a.kmscon,
               "| libtsm | [`%s`](%s/commit/%s) |" % (a.libtsm, LIBTSM_URL, a.libtsm), "")
        if self.problems:
            self.w("**Not what this page names:** %s. Those tiers' results describe other code "
                   "than the commits above." % "; ".join(self.problems), "")
        self.w("| Tier | What it checks | Cases passing |", "|---|---|---|")
        for tier, (ok, n) in self.counts().items():
            title = "Tier %d: %s" % (tier, TIERS[tier][0])
            self.w("| [%s](#%s) | %s | %d of %d |" % (title, anchor(title), TIERS[tier][2], ok, n))
        self.w("")
        tools = next((r["tools"] for r in self.reports.values() if r.get("tools")), None)
        if tools:
            parts = []
            if tools.get("image_base"):
                parts.append("dev image `%s`" % tools["image_base"].split("sha256:")[-1][:12])
            if tools.get("archive_date"):
                parts.append("Arch Linux Archive %s" % tools["archive_date"])
            parts += ["%s %s" % kv for kv in sorted(tools.get("packages", {}).items())]
            self.w("Tools: %s." % ", ".join(parts), "")

    def since(self):
        self.w("## Since the previous run", "")
        prev = self.prev
        if prev is None:
            self.w("This is the first run in the ledger.", "")
            return
        href = prev["url"] or (self.link(prev["path"]) if prev["path"] else None)
        target = "[%s](%s)" % (prev["date"], href) if href else prev["date"]
        self.w("The previous ledger run is %s, \"%s\": kmscon `%s`, libtsm `%s`." % (
            target, prev["note"], prev["kmscon"], prev["libtsm"]), "")
        now = self.counts()
        self.w("| Tier | Then | Now |", "|---|---|---|")
        for tier in sorted(set(now) | set(prev["counts"])):
            fmt = lambda c: "%d of %d" % c if c else "not run"  # noqa: E731
            self.w("| %d | %s | %s |" % (tier, fmt(prev["counts"].get(tier)), fmt(now.get(tier))))
        self.w("")

        # A tier run only now, or only then, is named once, not listed case
        # by case as new or removed.
        then = set(prev["cases"] if prev["cases"] is not None else prev["counts"])
        groups = {"failing": [], "passing": [], "new": [], "changed": []}
        unknown = set()
        for tier, rep in sorted(self.reports.items()):
            if tier not in then:
                continue
            for case in rep["cases"]:
                was, ok = self.prev_status(tier, case), case["status"] in OK
                ref = "[`%s`](#%s)" % (case["name"], anchor(case_heading(tier, case["name"], case["status"])))
                entry = (tier, ref)
                if was is None:
                    groups["new"].append(entry)
                elif was == "?":
                    unknown.add(tier)
                elif was in OK and not ok:
                    groups["failing"].append(entry)
                elif was not in OK and ok:
                    groups["passing"].append(entry)
                if self.changed(tier, case):
                    groups["changed"].append(entry)
        removed = []
        if prev["cases"] is not None:
            for tier, cases in sorted(prev["cases"].items()):
                if tier not in self.reports:
                    continue
                names = {c["name"] for c in self.reports.get(tier, {"cases": []})["cases"]}
                removed += [(tier, "`%s`" % n) for n in sorted(cases) if n not in names]
        else:
            for (repo, path), status in sorted(self.hist["changes"].items()):
                if status == "D" and not path.endswith(".tol"):
                    removed.append((None, "`%s` (%s)" % (path, repo)))

        def listing(entries):
            if not entries:
                return "none."
            by = {}
            for tier, ref in entries:
                by.setdefault(tier, []).append(ref)
            return "; ".join(("tier %d: " % t if t else "") + ", ".join(refs) for t, refs in by.items()) + "."

        self.w("- **Newly failing:** " + listing(groups["failing"]),
               "- **Newly passing:** " + listing(groups["passing"]),
               "- **New cases:** " + listing(groups["new"]),
               "- **Removed cases:** " + listing(removed),
               "- **Goldens changed:** " + listing(groups["changed"]), "")
        for tier in sorted(set(self.reports) - then):
            self.w("Tier %d was not run then, so its cases are not compared." % tier, "")
        for tier in sorted(then - set(self.reports)):
            self.w("Tier %d is not run now, so its cases are not compared." % tier, "")
        if prev["cases"] is None:
            self.w("The previous run left no per-case record. A case counts as passing then when the "
                   "previous run passed every case of its tier, and as new when its golden was added "
                   "since; a removed case is a golden deleted since.", "")
        if unknown:
            self.w("Tier %s: the previous run did not pass every case and left no per-case record, "
                   "so a change of verdict cannot be told there." % ", ".join(map(str, sorted(unknown))), "")
        if self.hist["dir"] is None:
            self.w("No golden history was given, so new, removed and changed goldens are not known.", "")
        for note in self.hist["notes"]:
            self.w("Golden history unavailable for %s." % note, "")

    def tier(self, tier):
        title = "Tier %d: %s" % (tier, TIERS[tier][0])
        about = TIERS[tier][1].format(list="%s/blob/%s/test/conformance/fork-goldens.list"
                                      % (LIBTSM_URL, self.args.libtsm))
        self.w("## " + title, "", about, "")
        cases = self.reports[tier]["cases"]
        if tier == 1:
            self.tier1(cases)
            return
        self.w("| Case | Verdict | Pixels differing |", "|---|---|---|")
        for case in cases:
            heading = case_heading(tier, case["name"], case["status"])
            self.w("| [`%s`](#%s) | %s | %d |" % (case["name"], anchor(heading),
                                                verdict_word(case["status"]), case["stats"]["pixels"]))
        self.w("")
        variants = self.reports[tier].get("variants", {})
        shown = None
        for case in cases:
            variant = case["name"].split("/")[0] if tier == 3 else None
            if variant and variant != shown:
                shown = variant
                self.variant(variant, variants.get(variant, {}))
            self.image_case(tier, case, "####" if variant else "###")

    def variant(self, name, meta):
        self.w("### Variant %s" % name, "")
        line = next(iter(meta.get("kmscon") or []), "")
        found = dict(re.findall(r"(backend|text renderer|font engine) \[([^\]]*)\]", line))
        if found:
            self.w("kmscon drew with the %s backend, the %s renderer and the %s font engine." % (
                found.get("backend", "?"), found.get("text renderer", "?"), found.get("font engine", "?")), "")
        if meta.get("error"):
            self.w("The variant failed: %s" % meta["error"], "")

    def tier1(self, cases):
        self.w("| Case | Size | Verdict | Golden from |", "|---|---|---|---|")
        for case in cases:
            reason = self.reasons.get(case["name"])
            src = "fork: %s" % reason if reason else "upstream" if case["name"] not in self.reasons else "fork"
            name = "`%s`" % case["name"]
            if case["status"] not in OK or self.changed(1, case):
                name = "[%s](#%s)" % (name, anchor(case_heading(1, case["name"], case["status"])))
            self.w("| %s | %d×%d | %s | %s |" % (name, case["cols"], case["rows"],
                                                 verdict_word(case["status"]), cell(src)))
        self.w("")
        for case in cases:
            failed, changed = case["status"] not in OK, self.changed(1, case)
            if not failed and not changed:
                continue
            self.w("### " + case_heading(1, case["name"], case["status"]), "")
            if failed:
                self.w("The screen differs from the golden (`-` golden, `+` actual):", "")
                self.text_diff(case.get("golden") or "", case.get("actual") or "", "golden", "actual")
            if changed:
                old = self.hist["dir"] / "libtsm" / changed[0]
                self.w("The golden changed since the previous run (`-` then, `+` now):", "")
                self.text_diff(old.read_text() if old.exists() else "", case.get("golden") or "",
                               "previous golden", "golden")

    def text_diff(self, a, b, an, bn):
        lines = list(difflib.unified_diff(a.splitlines(), b.splitlines(), an, bn, n=1, lineterm=""))
        more = len(lines) - DIFF_LINES
        self.w("```diff", *lines[:DIFF_LINES])
        if more > 0:
            self.w("… %d more lines" % more)
        self.w("```", "")

    def image_case(self, tier, case, level):
        name, status = case["name"], case["status"]
        self.w("%s %s" % (level, case_heading(tier, name, status)), "")
        if case.get("description"):
            self.w(case["description"], "")
        stem = "t%d-%s" % (tier, re.sub(r"[^\w.-]", "-", name))
        golden, actual = load(case.get("golden")), load(case.get("actual"))
        changed = self.changed(tier, case)
        old = None
        if changed and changed[0] == case.get("golden"):
            old = load(self.hist["dir"] / "kmscon" / changed[0])
        problem = case.get("error") or case.get("note")
        if status not in OK:
            if problem:
                self.w("**Why:** %s" % problem, "")
            st = case["stats"]
            if st["pixels"]:
                self.w("%d pixels differ, by at most %d per channel." % (st["pixels"], st["max_delta"]), "")
        elif status == "near":
            self.w("%d pixels differ, by at most %d per channel, within the tolerance beside the golden." % (
                case["stats"]["pixels"], case["stats"]["max_delta"]), "")
        if status != "pass" and golden and actual:
            self.comparison(stem, [golden, actual], ["golden", "actual"], case["stats"]["bbox"])
        elif status != "pass":
            only = actual or golden
            if only:
                what = "actual screen; there is no golden" if actual else "golden; no screen was captured"
                self.picture(stem, only, "The %s." % what)
            else:
                self.w("No golden and no capture.", "")
        if changed:
            if old and golden:
                self.w("Its golden changed since the previous run%s:" % (
                    "; this run drew the golden now exactly" if status == "pass" else ""), "")
                self.comparison(stem + "-golden", [old, golden],
                                ["golden at %s" % self.prev["kmscon"], "golden now"], ())
            else:
                self.w("Changed since the previous run: %s." % ", ".join("`%s`" % p for p in changed), "")
        if status == "pass" and actual and not (changed and old and golden):
            self.picture(stem, actual, None, golden is None)

    def crop_note(self, img, box, step):
        if box == (0, 0, img.width, img.height) and step == 1:
            return ""
        note = " Cropped to %d,%d–%d,%d of the %d×%d screen" % (box + (img.width, img.height))
        return note + (", one pixel in %d kept." % step if step > 1 else ".")

    def picture(self, stem, img, caption, check=False):
        box = crop_box([img])
        step = step_for(box)
        self.save(stem + ".png", crop(img, box, step))
        text = caption or ("The screen this run drew; a check with no golden." if check else
                           "The screen this run drew, identical to the golden.")
        self.w("![%s](%s.png)" % (stem, stem), "", "*%s%s*" % (text, self.crop_note(img, box, step)), "")

    def comparison(self, stem, imgs, names, regions):
        want, got = imgs
        if (want.width, want.height) != (got.width, got.height):
            self.w("The %s is %d×%d and the %s %d×%d, so there is no difference image." % (
                names[0], want.width, want.height, names[1], got.width, got.height), "")
            for img, n in zip(imgs, names):
                self.picture("%s-%s" % (stem, n.split()[-1]), img, "The %s." % n)
            return
        box = crop_box(imgs, regions)
        step = step_for(box)
        diff, n = diff_image(want, got, box, step)
        img, across = strip([crop(want, box, step), crop(got, box, step), diff])
        self.save(stem + "-strip.png", img)
        order = "Left to right" if across else "Top to bottom"
        count = "%d pixels differ" % n if step == 1 else "%d blocks of %d×%d pixels differ" % (n, step, step)
        self.w("![%s: %s, %s and difference](%s-strip.png)" % (stem, names[0], names[1], stem), "",
               "*%s: %s, %s, and the difference (the %s dimmed, differing pixels in red; %s).%s*" % (
                   order, names[0], names[1], names[1], count, self.crop_note(want, box, step)), "")

    def save(self, name, img):
        self.images.append(name)
        write_png(self.out / name, img)

    def record(self):
        """The per-case verdicts, which the next run compares with."""
        tiers = {str(t): {c["name"]: c["status"] for c in r["cases"]} for t, r in sorted(self.reports.items())}
        return json.dumps({"kmscon": self.args.kmscon, "libtsm": self.args.libtsm, "tiers": tiers},
                          indent=1, sort_keys=True) + "\n"


LEDGER_HEAD = """# Conformance ledger

One row per recorded run of the harness (ADR-301), oldest first. `make sheet NOTE="..."` runs the tiers, writes the run's page under `runs/`, and appends its row here. The kmscon and libtsm columns name the commits that run measured. The first three runs were drawn as SVG contact sheets, since removed from the tree; their rows link to each sheet at commit `627f3d1`.

| Date (UTC) | Note | kmscon | libtsm | Tier 1 | Tier 2 | Tier 3 | Run |
|---|---|---|---|---|---|---|---|
"""


def append_ledger(path, args, counts, page):
    by_tier = {tier: "%d/%d" % c for tier, c in counts.items()}
    rel = os.path.relpath(page, Path(path).parent).replace(os.sep, "/")
    row = "| %s | %s | `%s` | `%s` | %s | %s | %s | [%s](%s) |\n" % (
        args.date, cell(args.note), args.kmscon, args.libtsm, by_tier.get(1, "—"),
        by_tier.get(2, "—"), by_tier.get(3, "—"), Path(page).parent.name, rel)
    p = Path(path)
    text = p.read_text() if p.exists() else LEDGER_HEAD
    p.write_text(text + row)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("reports", nargs="+")
    ap.add_argument("--out", required=True, help="the run's directory")
    ap.add_argument("--kmscon", required=True)
    ap.add_argument("--libtsm", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--ledger")
    ap.add_argument("--append", action="store_true")
    ap.add_argument("--note", default="")
    ap.add_argument("--history")
    ap.add_argument("--fork-goldens")
    args = ap.parse_args(argv)
    if args.append and not (args.ledger and args.note):
        ap.error("--append needs --ledger and --note")

    reports = [json.loads(Path(r).read_text()) for r in args.reports]
    problems = provenance(reports, args.kmscon, args.libtsm)
    if args.append and problems:
        raise SystemExit("runpage: refusing a ledger row: %s" % "; ".join(problems))
    inputs = args.reports + [p for p in (args.ledger, args.history, args.fork_goldens) if p]
    out = claim("runpage.py", args.out, ".run-page", inputs)
    for old in out.glob("*.png"):
        old.unlink()
    page = Page(args, reports)
    text = page.render()
    (out / "README.md").write_text(text)
    (out / "cases.json").write_text(page.record())
    counts = page.counts()
    for tier, (ok, n) in counts.items():
        print("runpage: tier %d: %d/%d cases pass" % (tier, ok, n))
    size = sum(f.stat().st_size for f in out.iterdir())
    print("runpage: wrote %s (%d images, %d KB)" % (out / "README.md", len(page.images), -(-size // 1024)))
    if args.append:
        append_ledger(args.ledger, args, counts, out / "README.md")
        print("runpage: ledger row added to %s" % args.ledger)
    return 0


if __name__ == "__main__":
    sys.exit(main())
