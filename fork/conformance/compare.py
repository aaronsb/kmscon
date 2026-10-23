#!/usr/bin/env python3
"""Compare two builds of kmscon drawing the same screens, as a markdown page.

Two subcommands:

  compare.py save --out DIR REPORT...
      Copy a run's reports, with the screens each one captured, into DIR, so
      the run can be compared later. Paths in the saved reports are relative
      to DIR.

  compare.py page --out DIR --a DIR --b DIR [--a2 DIR] [--b2 DIR]
                  [--label-a TEXT] [--label-b TEXT] [--verdicts FILE]
                  [--harness REV] [--command TEXT]
      Write DIR/README.md and its images: each screen both runs captured, A
      beside B, and where they differ a difference image with each differing
      region outlined and numbered, and a table naming each region by
      terminal row and column and by the text the screen's script printed
      there. --a2 and --b2 are second runs of the same builds: the page says
      whether each side reproduces its own captures exactly. --verdicts holds
      "screen: which side is right and why" lines for the captions.

Screens pair by name; a screen only one run has is listed, not dropped. The
page is written for a reader who has not seen this repository, and the same
inputs always write the same bytes.

SPDX-License-Identifier: MIT
"""

import argparse
import json
import re
import shutil
import sys
import unicodedata
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from outdir import claim  # noqa: E402
from pageimg import crop, crop_box, diff_image, outline, step_for, strip, t3img, write_png  # noqa: E402
from pagetext import prose  # noqa: E402

REPO = "https://github.com/aaronsb/kmscon"
SAVED = ".saved-run"        # marks a directory compare.py save wrote
PAGE = ".compare-page"      # marks a directory compare.py page wrote
KINDS = {2: "screens rendered in memory without a display (tier 2)",
         3: "screens captured from the virtual machine's display (tier 3)"}


# ---------------------------------------------------------------- saved runs

def slug(name):
    return re.sub(r"[^\w.-]", "-", name)


def save(out, reports):
    """Copy each report and every screen it captured into `out`, replacing
    what an earlier save left there."""
    reps = [json.loads(Path(path).read_text()) for path in reports]
    # Tier 2 draws only the fork's renderer, and its report names no kmscon
    # commit: beside another build's tier 3 run it would pass for that build's.
    other = [r["tools"]["kmscon_repo"] for r in reps
             if r["tier"] == 3 and (r.get("tools") or {}).get("kmscon_repo") not in (None, REPO)]
    if other and any(r["tier"] == 2 for r in reps):
        raise SystemExit("compare.py save: the tier 3 run is of %s, and the tier 2 report is "
                         "the fork's; save tier 3 alone (make sheet-save TIERS=3)" % other[0])
    out = claim("compare.py save", out, SAVED, reports)
    for old in out.iterdir():
        if old.name != SAVED:
            shutil.rmtree(old) if old.is_dir() and not old.is_symlink() else old.unlink()
    for path, rep in zip(reports, reps):
        for case in rep["cases"]:
            src = case.get("actual")
            for key in ("golden", "diff", "thumb_actual", "thumb_golden", "thumb_diff"):
                case.pop(key, None)
            case["actual"] = None
            if src and Path(src).exists():
                dest = Path("t%d" % rep["tier"]) / (slug(case["name"]) + ".png")
                (out / dest).parent.mkdir(exist_ok=True)
                t3img.write_png(str(out / dest), t3img.read(src))
                case["actual"] = dest.as_posix()
        (out / Path(path).name).write_text(json.dumps(rep, indent=1, ensure_ascii=False, sort_keys=True) + "\n")
    print("compare: saved %d report(s) to %s" % (len(reports), out))


class Run:
    """A saved run: its image-tier reports, keyed by (tier, case name)."""

    def __init__(self, path):
        self.path = Path(path)
        self.reports = [json.loads(p.read_text()) for p in sorted(self.path.glob("*-report.json"))]
        self.reports = [r for r in self.reports if r["tier"] in (2, 3)]
        if not self.reports:
            raise SystemExit("compare: no tier 2 or tier 3 report in %s" % path)
        self.cases = {(r["tier"], c["name"]): c for r in self.reports for c in r["cases"]}
        self.ids = {r["tier"]: r.get("run_id") for r in self.reports}
        self.tools = next((r["tools"] for r in self.reports if r["tier"] == 3 and r.get("tools")), {})
        self.variants = next((r.get("variants", {}) for r in self.reports if r["tier"] == 3), {})

    def image(self, key):
        case = self.cases.get(key)
        path = self.path / case["actual"] if case and case.get("actual") else None
        return t3img.read(str(path)) if path and path.exists() else None

    def meta(self, key):
        return self.variants.get(key[1].split("/")[0], {}) if key[0] == 3 else {}

    def renderer(self):
        found = {}
        for meta in self.variants.values():
            for line in meta.get("kmscon") or []:
                found.update(re.findall(r"(backend|text renderer|font engine) \[([^\]]*)\]", line))
        return found


# ---------------------------------------------------------------- regions

def region_stats(want, got, points, regions):
    """[(pixels, max channel delta)] for each region; a point counts in the
    first region that holds it."""
    stats = [[0, 0] for _ in regions]
    for x, y in points:
        for i, (x0, y0, x1, y1) in enumerate(regions):
            if x0 <= x < x1 and y0 <= y < y1:
                a = want.row(y)[x * 3:x * 3 + 3]
                b = got.row(y)[x * 3:x * 3 + 3]
                stats[i][0] += 1
                stats[i][1] = max(stats[i][1], *(abs(p - q) for p, q in zip(a, b)))
                break
    return [tuple(s) for s in stats]


def width(ch):
    """Cells a character takes: two for a wide one."""
    return 2 if unicodedata.east_asian_width(ch[0]) in ("W", "F") else 1


def screen_rows(text, cols):
    """The screen's rows as lists of (column, character), lines wrapped at
    `cols` cells, wide characters two cells."""
    rows = []
    for line in text or []:
        row, col = [], 0
        for ch in line:
            if unicodedata.combining(ch) and row:
                row[-1] = (row[-1][0], row[-1][1] + ch)
                continue
            if col + width(ch) > cols:
                rows.append(row)
                row, col = [], 0
            row.append((col, ch))
            col += width(ch)
        rows.append(row)
    return rows


def grid_offset(size, cell):
    """The pixel where the character grid starts: bbulk and gltex centre the
    whole cells that fit in the screen and leave the rest as a margin."""
    (w, h), (cw, ch) = size, cell
    return (w - w // cw * cw) // 2, (h - h // ch * ch) // 2


def cells(region, cell, rows, size):
    """A region of a `size` screen as terminal cells, rows and columns
    counted from 1, with the text printed there: ("row 3, cols 10-24",
    "the text")."""
    cw, ch = cell
    ox, oy = grid_offset(size, cell)
    x0, y0, x1, y1 = region
    c0, r0 = max((x0 - ox) // cw, 0), max((y0 - oy) // ch, 0)
    c1, r1 = min((x1 - 1 - ox) // cw, size[0] // cw - 1), min((y1 - 1 - oy) // ch, size[1] // ch - 1)
    if c0 > c1 or r0 > r1:
        return "the margin around the character grid", ""
    where = ("row %d" % (r0 + 1) if r0 == r1 else "rows %d-%d" % (r0 + 1, r1 + 1)) + ", " + \
        ("col %d" % (c0 + 1) if c0 == c1 else "cols %d-%d" % (c0 + 1, c1 + 1))
    texts = []
    for r in range(r0, r1 + 1):
        if r < len(rows):
            t = "".join(s for c, s in rows[r] if c <= c1 and c + width(s) - 1 >= c0).strip()
            if t:
                texts.append(t)
    shown = " / ".join(texts[:3]) + (" / …" if len(texts) > 3 else "")
    return where, shown


def code(text):
    """Text as inline code in a table cell; GitHub splits cells at a bare |
    even inside backticks."""
    text = text.replace("|", "\\|")
    return "`` %s ``" % text if "`" in text else "`%s`" % text


# ---------------------------------------------------------------- the page

def machine(meta):
    """A variant's vCPUs and memory as the report records them, or None."""
    if not meta.get("cpus"):
        return None
    return "%d vCPU%s, %d MB" % (meta["cpus"], "" if meta["cpus"] == 1 else "s", meta["memory_mb"])


class Page:
    def __init__(self, args):
        self.args = args
        self.out = Path(args.out)
        self.a, self.b = Run(args.a), Run(args.b)
        self.a2 = Run(args.a2) if args.a2 else None
        self.b2 = Run(args.b2) if args.b2 else None
        self.la, self.lb = args.label_a, args.label_b
        for flag, run, again in (("--a2", self.a, self.a2), ("--b2", self.b, self.b2)):
            same = [t for t, i in sorted(run.ids.items()) if i and again and again.ids.get(t) == i]
            if same:
                raise SystemExit("compare: %s holds the same tier %d run (%s) as %s; a second run "
                                 "is a separate capture" % (flag, same[0], run.ids[same[0]], flag[:-1]))
        self.verdicts = {}
        if args.verdicts:
            for line in Path(args.verdicts).read_text().splitlines():
                name, sep, text = line.partition(":")
                if sep and not line.startswith("#"):
                    self.verdicts[name.strip()] = text.strip()
        self.lines = []

    def w(self, *lines):
        self.lines.extend(lines)

    def tiers(self):
        """Kinds of screen both runs hold; a kind one run lacks entirely is
        named once rather than listed screen by screen."""
        ta, tb = {r["tier"] for r in self.a.reports}, {r["tier"] for r in self.b.reports}
        return ta & tb, sorted((ta ^ tb))

    def keys(self):
        common = self.tiers()[0]
        order = [k for k in list(self.a.cases) + [k for k in self.b.cases if k not in self.a.cases]
                 if k[0] in common]
        return [k for k in order if k in self.a.cases and k in self.b.cases], \
            [k for k in order if (k in self.a.cases) != (k in self.b.cases)]

    def display(self, key, run):
        """Where a screen was drawn: "1280×800, bbulk" for a display, from
        its capture and kmscon's log, or "384×160 in memory"."""
        img = run.image(key)
        size = "%d×%d" % (img.width, img.height) if img else "?"
        if key[0] == 2:
            return size + " in memory"
        found = re.findall(r"text renderer \[([^\]]*)\]", " ".join(run.meta(key).get("kmscon") or []))
        return size + (", %s" % found[0] if found else "")

    def screen_name(self, key):
        return key[1].split("/")[-1]

    def heading(self, key):
        # A plain x: GitHub's heading anchors drop some symbols.
        return "`%s`, %s" % (self.screen_name(key), self.display(key, self.a).replace("×", "x"))

    def render(self):
        both, one = self.keys()
        results = {k: self.result(k) for k in both}
        self.header(both, results)
        self.reproducibility(both)
        self.w("## The screens", "")
        for k in both:
            self.screen(k, results[k])
        for tier in self.tiers()[1]:
            side = self.la if any(r["tier"] == tier for r in self.a.reports) else self.lb
            self.w("Only %s's run includes %s, so those are not compared." % (side, KINDS[tier]), "")
        if one:
            self.w("## Screens only one build drew", "", "| Screen | Drawn by | Not drawn by |", "|---|---|---|")
            for k in one:
                has, lacks = (self.la, self.lb) if k in self.a.cases else (self.lb, self.la)
                self.w("| `%s` | %s | %s |" % (k[1], has, lacks))
            self.w("")
        self.footer()
        return "\n".join(self.lines).rstrip() + "\n"

    def result(self, key):
        """(status, data) for a pair: missing, size, same or differ."""
        a, b = self.a.image(key), self.b.image(key)
        if a is None or b is None:
            why = [self.a.cases[key], self.b.cases[key]][a is not None]
            return "missing", (self.la if a is None else self.lb, why.get("error") or "no capture")
        if (a.width, a.height) != (b.width, b.height):
            return "size", (a, b)
        stats, _, points = t3img.compare(b, a)
        if not points:
            return "same", (a,)
        regions = [tuple(r) for r in stats["bbox"]]
        return "differ", (a, b, stats, regions, region_stats(a, b, points, regions),
                          t3img.regions_merged(points))

    def summary(self, status, data):
        if status == "same":
            return "identical"
        if status == "missing":
            return "no screen from %s" % data[0]
        if status == "size":
            return "different sizes"
        return "differ: %d pixels in %d region%s" % (data[2]["pixels"], len(data[3]),
                                                    "" if len(data[3]) == 1 else "s")

    def header(self, both, results):
        self.w("# kmscon: %s and %s drawing the same screens" % (self.la, self.lb), "")
        common = self.tiers()[0]
        how = {3: "For screens on a display, each build was installed in the same virtual machine, "
                  "one at a time, drew each screen on the same virtual display, and the display was "
                  "captured pixel for pixel." if self.same_machine() else
                  "For screens on a display, each build was installed in a virtual machine and drew "
                  "each screen on a virtual display, captured pixel for pixel; the two runs' machines "
                  "or displays differ, as listed at the end.",
               2: "For screens in memory, kmscon's renderer drew each screen into a buffer with "
                  "pinned fonts, with no display involved."}
        self.w("This page compares two builds of kmscon, %s and %s, drawing the same screens. %s "
               "The two captures of each screen are set side by side, and every pixel where they "
               "differ is marked. How the screens were made is at the end." % (
                   self.la, self.lb, " ".join(how[t] for t in sorted(common, reverse=True))), "")
        self.w("| | %s | %s |" % (self.la, self.lb), "|---|---|---|")
        for label, key in (("kmscon commit", "kmscon"), ("libtsm commit", "libtsm")):
            self.w("| %s | %s | %s |" % (label, self.commit(self.a, key), self.commit(self.b, key)))
        ra, rb = self.a.renderer(), self.b.renderer()
        for label, key in (("Video backend", "backend"), ("Text renderer", "text renderer"),
                           ("Font engine", "font engine")):
            self.w("| %s | %s | %s |" % (label, ra.get(key, "not recorded"), rb.get(key, "not recorded")))
        self.w("")
        counts = {}
        for k in both:
            counts[results[k][0]] = counts.get(results[k][0], 0) + 1
        self.w("Of the %d screens both builds drew: %d identical, %d different%s." % (
            len(both), counts.get("same", 0), counts.get("differ", 0) + counts.get("size", 0),
            ", %d without a capture from one side" % counts["missing"] if counts.get("missing") else ""), "")
        self.w("| Screen | Display | Result |", "|---|---|---|")
        for k in both:
            self.w("| [`%s`](#%s) | %s | %s |" % (self.screen_name(k), anchor(self.heading(k)),
                                                self.display(k, self.a), self.summary(*results[k])))
        self.w("")

    def commit(self, run, key):
        rev = run.tools.get(key)
        if not rev:
            return "not recorded"
        bare, _, dirty = rev.partition("+")
        # A report names the repository it built from; one written before
        # reports did is the fork's.
        repo = run.tools.get(key + "_repo") or (REPO if key == "kmscon" else REPO.replace("kmscon", "libtsm"))
        return "[`%s`](%s/commit/%s)%s" % (bare, repo, bare, " with uncommitted changes" if dirty else "")

    def reproducibility(self, both):
        self.w("## Reproducibility", "")
        pairs = [(lbl, r, r2) for lbl, r, r2 in ((self.la, self.a, self.a2), (self.lb, self.b, self.b2)) if r2]
        if not pairs:
            self.w("Each build was captured once. To check that a build draws the same pixels every "
                   "time, capture it twice and pass the second run as well (see the end of the page).", "")
            return
        self.w("Each build below was captured in two separate runs, each in a freshly booted machine. "
               "A build that draws the same pixels every time shows 0 differing pixels.", "")
        unknown = [lbl for lbl, r, r2 in pairs if not all(r.ids.values()) or not all(r2.ids.values())]
        if unknown:
            self.w("The reports of %s carry no run id, so this page cannot confirm that its two "
                   "runs are separate captures rather than one run saved twice." % " and ".join(unknown), "")
        self.w("| Screen | Display | " + " | ".join("%s: run 1 vs run 2" % p[0] for p in pairs) + " |",
               "|---|---|" + "---|" * len(pairs))
        for k in both:
            row = []
            for _, r, r2 in pairs:
                a, b = r.image(k), r2.image(k)
                if a is None or b is None:
                    row.append("no capture in one run")
                elif (a.width, a.height) != (b.width, b.height):
                    row.append("different sizes")
                else:
                    n = t3img.compare(b, a)[0]["pixels"]
                    row.append("identical (0 px)" if n == 0 else "**%d px differ**" % n)
            self.w("| `%s` | %s | %s |" % (self.screen_name(k), self.display(k, self.a), " | ".join(row)))
        self.w("")

    def screen(self, key, result):
        status, data = result
        case = self.a.cases[key]
        self.w("### " + self.heading(key), "")
        desc = case.get("description") or self.b.cases[key].get("description") or ""
        wrong = case.get("wrong") or self.b.cases[key].get("wrong")
        shows, _, correct = desc.partition("Correct:")
        if shows.strip():
            self.w("**What it shows:** " + prose(shows.strip()), "")
        if correct.strip():
            self.w("**What correct looks like:** " + prose(correct.strip()), "")
        if wrong:
            self.w("**What wrong looks like:** " + prose(wrong), "")
        stem = slug(key[1])
        if status == "same":
            img = data[0]
            box = crop_box([img])
            step = step_for(box)
            write_png(self.out / (stem + ".png"), crop(img, box, step))
            self.w("Identical: both builds drew exactly the same pixels.", "",
                   "![%s](%s.png)" % (stem, stem), "", "*%s*" % self.crop_note(img, box, step), "")
            return
        verdict = self.verdicts.get(key[1]) or self.verdicts.get(self.screen_name(key))
        self.w("**Which is correct:** " + (verdict or "*not yet judged*"), "")
        if status == "missing":
            self.w("No capture from %s: %s" % data, "")
            return
        if status == "size":
            a, b = data
            self.w("The captures differ in size (%d×%d and %d×%d), so they are not compared pixel by "
                   "pixel." % (a.width, a.height, b.width, b.height), "")
            return
        a, b, stats, regions, per, merged = data
        box = crop_box([a, b], regions)
        step = step_for(box)
        diff, _ = diff_image(a, b, box, step)
        img, across = strip([crop(a, box, step), crop(b, box, step), outline(diff, regions, box, step)])
        write_png(self.out / (stem + "-strip.png"), img)
        self.w("%d pixels differ, by at most %d in any colour channel (0 to 255)." % (
            stats["pixels"], stats["max_delta"]), "")
        self.w("![%s: %s, %s, difference](%s-strip.png)" % (stem, self.la, self.lb, stem), "",
               "*%s: %s, %s, and the difference: %s dimmed, differing pixels in red, each region "
               "outlined in yellow and numbered as in the table. %s*" % (
                   "Left to right" if across else "Top to bottom", self.la, self.lb, self.lb,
                   self.crop_note(a, box, step)), "")
        ca, cb = self.a.meta(key).get("cell"), self.b.meta(key).get("cell")
        cell = ca if not cb or ca == cb else cb if not ca else None
        rows = screen_rows(case.get("text") or self.b.cases[key].get("text"),
                           a.width // cell[0]) if cell else []
        self.w("| # | Where | Pixels | Max delta | Text drawn there |", "|---|---|---|---|---|")
        for n, (region, (px, delta)) in enumerate(zip(regions, per), 1):
            if cell:
                where, text = cells(region, cell, rows, (a.width, a.height))
            else:
                where, text = "pixels %d,%d to %d,%d" % tuple(region), ""
            self.w("| %d | %s | %d | %d | %s |" % (n, where, px, delta, code(text) if text else "—"))
        self.w("")
        if cell:
            self.w("*Rows and columns count from 1 at the top left of the character grid, in "
                   "%d×%d-pixel cells; kmscon centres the grid on the screen, offset %d,%d here.*"
                   % (tuple(cell) + grid_offset((a.width, a.height), cell)), "")
        elif ca and cb:
            self.w("*Regions are given in pixels: the runs record different character cells, "
                   "%d×%d and %d×%d.*" % (tuple(ca) + tuple(cb)), "")
        if merged:
            self.w("*More than %d separate areas differ, so nearby ones are merged into one region: "
                   "a region can cover cells that did not all differ.*" % t3img.MAX_REGIONS, "")

    def crop_note(self, img, box, step):
        if box == (0, 0, img.width, img.height) and step == 1:
            return "The whole screen."
        note = "Cropped to the drawn part, pixels %d,%d to %d,%d of the %d×%d screen" % (
            box + (img.width, img.height))
        return note + ("; one pixel in %d kept." % step if step > 1 else ".")

    # What each run records of the machine and display it drew on: the
    # page says "the same machine" only when every one of these agrees.
    MACHINE = ("guest_image", "guest_image_sha256", "guest_archive")
    DISPLAY = ("device", "cpus", "memory_mb", "config", "cell")

    def same_machine(self):
        return all(self.a.tools.get(k) == self.b.tools.get(k) for k in self.MACHINE) and \
            all(self.a.variants.get(n, {}).get(k) == self.b.variants.get(n, {}).get(k)
                for n in self.common_variants() for k in self.DISPLAY)

    def common_variants(self):
        return sorted({k[1].split("/")[0] for k in self.keys()[0] if k[0] == 3})

    def fact(self, va, vb, fmt):
        """One value when both runs recorded the same, else each run's."""
        show = lambda v: fmt(v) if v else "not recorded"  # noqa: E731
        if va == vb:
            return show(va)
        return "%s for %s, %s for %s" % (show(va), self.la, show(vb), self.lb)

    def tool(self, key, fmt=lambda v: "`%s`" % v):
        return self.fact(self.a.tools.get(key), self.b.tools.get(key), fmt)

    def footer(self):
        self.w("## How this was produced", "")
        harness = self.args.harness
        where = "[`fork/tier3`](%s/tree/%s/fork/tier3)" % (REPO, harness) if harness else "`fork/tier3`"
        self.w("- **Machine:** a QEMU/KVM virtual machine booted from the Arch Linux cloud image %s "
               "(SHA-256 %s), with packages from the Arch Linux Archive snapshot of %s. Each build "
               "was installed as a package and run by systemd on a virtual console, as on a real "
               "system." % (self.tool("guest_image"), self.tool("guest_image_sha256"),
                            self.tool("guest_archive", str)))
        for name in sorted(set(self.a.variants) | set(self.b.variants)):
            ma, mb = self.a.variants.get(name, {}), self.b.variants.get(name, {})
            self.w("- **Display `%s`:** QEMU device %s. Machine: %s. kmscon configuration: %s. "
                   "Character cell %s." % (
                name, self.fact(ma.get("device"), mb.get("device"), lambda v: "`%s`" % v),
                self.fact(machine(ma), machine(mb), str),
                self.fact(ma.get("config"), mb.get("config"), lambda v: ", ".join("`%s`" % c for c in v)),
                self.fact(ma.get("cell"), mb.get("cell"), lambda v: "%d×%d pixels" % tuple(v))))
        common = self.tiers()[0]
        if 3 in common:
            self.w("- **Screens on a display:** for each screen the console is cleared and a shell "
                   "script prints it; the display is captured once two captures a second apart are "
                   "identical. The scripts and the driver are in %s of the repository that made "
                   "this page." % where)
        if 2 in common:
            self.w("- **Screens in memory:** `tests/test_pixels.c` feeds each screen's bytes to the "
                   "terminal emulator and draws the result with kmscon's renderer into a buffer, "
                   "using the fonts in `tests/fonts`.")
        self.w("- **Builds:** each package was built in the container image %s, with packages "
               "from the Arch Linux Archive snapshot of %s." % (
                   self.tool("image_base"), self.tool("archive_date", str)))
        if self.args.command:
            self.w("- **This page:** `%s`" % self.args.command)
        self.w("")


def anchor(heading):
    """The fragment GitHub gives a heading."""
    return re.sub(r"[^\w\- ]", "", heading.lower()).replace(" ", "-")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("save")
    s.add_argument("--out", required=True)
    s.add_argument("reports", nargs="+")
    p = sub.add_parser("page")
    for opt in ("--out", "--a", "--b"):
        p.add_argument(opt, required=True)
    for opt in ("--a2", "--b2", "--verdicts", "--harness", "--command"):
        p.add_argument(opt)
    p.add_argument("--label-a", default="A")
    p.add_argument("--label-b", default="B")
    args = ap.parse_args(argv)
    if args.cmd == "save":
        save(args.out, args.reports)
        return 0
    out = claim("compare.py page", args.out, PAGE,
                [p for p in (args.a, args.b, args.a2, args.b2, args.verdicts) if p])
    for old in out.glob("*.png"):
        old.unlink()
    (out / "README.md").write_text(Page(args).render())
    size = sum(f.stat().st_size for f in out.iterdir())
    print("compare: wrote %s (%d KB)" % (out / "README.md", -(-size // 1024)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
