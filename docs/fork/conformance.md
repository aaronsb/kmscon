# Conformance harness

The fork replaces libtsm's screen model and kmscon's render path ([ADR-101](../architecture/render/ADR-101-rebuild-the-screen-model-and-render-path-around-sprites-and-an-image-layer.md)). The harness is how that rewrite is kept honest: every change to parsing, the screen model or rendering is measured against recorded expectations, mechanically, and lands only when the measurements agree ([ADR-301](../architecture/fork/ADR-301-gate-rendering-changes-on-a-three-tier-conformance-harness.md)).

No person or agent judges pass or fail. Each tier compares and reports, and its exit status is the verdict. People look only at what failed, with the numbers and pictures the run produced.

## Three tiers

| Tier | Compares | Against | Runs |
|---|---|---|---|
| 1. Screen state | The screen libtsm builds from a recorded byte stream, as text | Goldens produced by upstream libtsm v4.8.0 | Every `make test` |
| 2. Pixels in memory | What kmscon's renderer draws into an in-memory framebuffer, with fonts pinned in the tree | Golden images | Every `make test` |
| 3. End to end | Screenshots of the installed package running on a VT in a virtual machine with a real KMS device | Golden screenshots, with masks for regions that legitimately vary | Before rendering changes merge, and on every upstream sync |

Each tier is cheaper and more precise than the one below it. Tier 1 says which cell changed; tier 2 says which pixels; tier 3 proves the assembled, installed package works.

### Tier 1: screen state

Lives in the libtsm fork under `test/conformance/`. `tsm-replay` feeds a byte stream through libtsm's public API and prints the screen: size, cursor, mode flags, replies the terminal sent back, then each row's text and its attribute runs. Recordings of full-screen programs carry checkpoints, so the screen is checked at every point the program showed one, not only after it exits.

The corpus has two kinds of stream:

- **Synthetic**, one area of terminal behaviour each: SGR attributes and colours, cursor movement, erasing, scroll regions, origin mode, wrapping, tabs, wide and combining characters, emoji, the alternate screen, replies to queries, DEC line drawing, OSC, DCS and APC sequences, scrollback, reset. Written by `gen-synthetic.py`.
- **Recorded programs**: bash, less, vim and tmux, run in a pseudo-terminal of fixed size with scripted keys by `record-programs.py`.

### Tier 2: pixels in memory

Lives in kmscon under `tests/` (`test_pixels.c`, `tier2/`). The bbulk renderer draws real screens through libtsm and the freetype backend, and the real DRM backend (`drm2d_render.c`) blends them into an in-memory XRGB buffer. Tier 2 covers bbulk on drm2d only; the fbdev and OpenGL paths are left to tier 3. The fonts are committed in `tests/fonts/`, so a font update on the build machine cannot move the results. `pixcmp.py` compares images and reports the number of differing pixels, the largest per-channel difference, and the regions that differ.

### Tier 3: end to end

A pinned Arch Linux guest under QEMU/KVM, with virtio-gpu at fixed resolutions. The fork's package is installed as a user would install it, kmscon runs on a VT, scripted keys arrive through QEMU's control socket and screenshots come back through it. The run includes switching VTs away and back.

## Verdicts

| Verdict | Meaning | Gate |
|---|---|---|
| pass | Identical to the golden (outside masked regions) | passes |
| near | Differs, but within the case's reviewed tolerance (pixel tiers only) | passes |
| fail | Differs beyond tolerance, or tier 1 differs at all | fails |
| missing | No golden for the case | fails |

A case's tolerance lives in a sidecar file beside its golden, `<name>.tol`, with one `key=value` per line: `max_delta=N` (the largest per-channel difference allowed), `max_pixels=N` (how many pixels may differ), and any number of `mask=x0,y0,x1,y1` rectangles that are not compared. `#` starts a comment. No sidecar means exact. Tier 1 is always exact.

A case with checkpoints counts as one case and passes only if every checkpoint matches.

## Goldens are the judge

A golden says what correct looks like, so it is treated with more care than the code it judges:

- Behaviour the fork preserves is judged by goldens produced by upstream libtsm from its release tag (`make golden REF=v4.8.0`), never by the fork's own code.
- Behaviour the fork changes on purpose is judged by goldens the fork produces (`make golden REF=<fork commit>` in the libtsm fork), reviewed as described next.
- A golden or tolerance file changes only in its own commit, with the difference read, never in the same commit as the code it judges.
- A golden that changes because behaviour changed on purpose says so in its commit, naming the change.
- Every check has been seen to fail before it is trusted: a planted defect turned it red and its removal turned it green.

Today's tier 1 goldens record upstream's behaviour as it is, including two defects the rebuild will fix: a colon sub-parameter (`SGR 4:3`) discards the whole sequence, and `SGR 4;58;5;196` turns on blink.

## Pinned dev images

The dev image is pinned two ways, so a rebuild installs the same packages: `DEV_BASE` in the Makefile is the digest of `archlinux:base-devel`, and `DEV_ARCHIVE` is a date of the Arch Linux Archive that pacman reads instead of the live mirrors.
Both are passed to `fork/dev.Dockerfile` as build args, and the libtsm fork pins its image the same way.
The tier 3 guest has its own archive date, `T3_ARCHIVE` in `fork/tier3.mk`.

The tier 1 and tier 2 reports carry a `tools` object: the image digest, the archive date, and the versions of freetype2, fontconfig, harfbuzz, pixman, gcc and meson read inside the container. The tier 2 report also names the libtsm commit it built against, with `+dirty` when that checkout has uncommitted changes.
A golden mismatch can be traced to a version change by comparing `tools` between the report that passed and the one that failed.
The sheet header shows the same versions when a report has them; a sheet drawn only from reports without `tools` is unchanged.

To move a pin, change `DEV_BASE` or `DEV_ARCHIVE` in one commit, run `make test`, and read `git status`.
Nothing is regenerated unless a golden moves, and a moved golden lands in its own commit with the diff read, as for any golden.

## Run pages and the ledger

Each recorded run produces a run page: `docs/conformance/runs/<date>-<kmscon>-<libtsm>/README.md`, with its images beside it, written for a reader who was not there. It opens with what the run marks, the commits it measured, the pass count of each tier, and what changed since the previous ledger run: cases newly failing or newly passing, cases added or removed, and goldens changed. Then each tier says what it checks and lists its cases. Tier 1 is text: a table of cases with the verdict and, for a case whose golden comes from a fork commit, the reason from libtsm's `fork-goldens.list`; a failure, or a golden changed since the previous run, gets a short text diff. Tiers 2 and 3 show each screen cropped to its drawn region, with a caption saying what it shows and what correct looks like. A passing case shows its screen. A failing or near case shows golden, actual and their difference, with differing pixels in red, and a case whose golden changed shows the previous golden, the golden now and their difference.

The page is drawn only from the run's reports, so it cannot disagree with the verdicts, and the same reports always write the same bytes. The captions come from where the cases are defined: the `desc` of each screen in `tests/test_pixels.c`, the header comment of each tier 3 screen script (and `CHECKS` in `fork/tier3/t3.py` for the VT checks), and the reasons in libtsm's `test/conformance/fork-goldens.list`. A new case gets its caption there.

"Since the previous run" compares with the ledger's last row. Verdicts come from the `cases.json` that row's page left. Goldens changed, added and deleted come from git, between the previous row's kmscon commit and the working tree and between its libtsm commit and the subproject's (`fork/conformance/golden-history.sh`, which runs on the host). The three runs recorded before run pages existed are SVG contact sheets under `docs/conformance/sheets/` and left no per-case record: against those, a case counts as passing then when its tier passed every case, and as new when its golden was added since.

A committed run (`make sheet NOTE="..."`, which writes the page and adds its row to [the ledger](../conformance/LEDGER.md)) happens once per working session (ADR-302), at its bring-home point: every open branch of the session has merged, main is green, and the run is the first thing done on main afterwards. The row records the date (UTC), what the run marks, the kmscon and libtsm commits measured, the pass count of each tier, and a link to the page, so over time the ledger shows the fork building and conforming against upstream as both move. `make sheet` refuses a tree with uncommitted changes, and a HEAD that is not `origin/main` after fetching it; `SHEET_ANY_COMMIT=1` overrides the second check.

The exception is comparative analysis. When work on a branch needs before and after views, `make sheet-preview` writes the same page under `fork/build/run/`, uncommitted, as often as the analysis needs. To set two runs side by side, save each and compare them:

```sh
make tier3 && make sheet-save SAVE=fork/build/saved/before   # on main, say
make tier3 && make sheet-save SAVE=fork/build/saved/after    # on the branch
make sheet-compare A=fork/build/saved/before B=fork/build/saved/after LABEL_A=main LABEL_B=branch
```

## Comparison pages

`make sheet-compare` writes `fork/build/compare/README.md` (or `OUT=`): each screen two saved runs both captured, A beside B, paired by name. A screen only one run has is listed, not dropped. The same comparison sets upstream kmscon beside the fork, built and run in the same tier 3 machine, which is the evidence the fork is announced with; the page is written for a reader who has not seen this repository.

- An identical pair says so and shows the screen once.
- A pair that differs shows A, B and a difference image, with each differing region outlined in yellow and numbered. A table gives each region's terminal row and columns (from the character cell size kmscon logs, which the tier 3 report records per display), its pixel count and largest channel difference, and the text the screen's script printed there (the report carries each script's output with escape sequences removed). A one-line verdict on which side is correct comes from `VERDICTS=FILE`, lines of `screen: verdict`; until one is given the page says *not yet judged*, beside the screen's own note of what correct looks like.
- `A2=` and `B2=` take a second run of each build; the page then reports, per screen, whether each build reproduced its own capture exactly.
- The footer says how the screens were made: the guest image and its SHA-256, the package snapshot, each display's QEMU device, kmscon configuration and cell size, the dev image the packages were built in, both builds' commits, and the command that regenerates the page. The tier 3 report records these at run time (`tools` and `variants` in `fork/build/tier3-report.json`).

`make sheet-save SAVE=DIR` copies the last run's tier 2 and tier 3 reports and every screen they captured into `DIR`, with paths relative to it, so a run can be kept and compared later. `DIR` must be inside the checkout, because the generators run in the dev image with only the checkout mounted.

## Commands

In the kmscon fork:

```sh
make test                      # all suites, including tiers 1 and 2
make test LIBTSM=../libtsm     # against a local libtsm checkout
make tier2                     # tier 2 only, with its report
make tier2-golden              # regenerate tier 2 goldens (review, then commit on their own)
make sheet-preview             # write this run's page into fork/build/run/, uncommitted
make sheet NOTE="what it marks"   # run page and ledger row, once per session on a green main
make sheet-save SAVE=DIR       # keep the last run's reports and screens for comparing
make sheet-compare A=DIR B=DIR LABEL_A=.. LABEL_B=..   # pair two saved runs' screens
make sheet-selftest            # unit tests of the page generators (part of make test)
```

Tier 3 needs KVM and QEMU on the host besides docker; [fork/tier3/README.md](../../fork/tier3/README.md) has the requirements, pins and variants:

```sh
make pkg                       # build the kmscon-aaronsb package from this checkout
make tier3-image               # once: fetch, verify and provision the guest base image
make tier3                     # pkg, then the 2D variants; report in fork/build/tier3-report.json
make tier3-gl                  # pkg, then the GL variant, outside the default run (#17)
make tier3-bless               # copy the last run's captures over the goldens (review, then commit on their own)
make tier3-selftest            # unit tests of the tier 3 image compare module
```

`make sheet` includes tier 3 when a tier 3 run has written its report to `fork/build`, so run `make tier3` first at the same commit.

kmscon builds against a pinned commit of the libtsm fork (`revision` in `subprojects/libtsm.wrap`), so the goldens cannot drift with libtsm. A change in libtsm reaches kmscon by moving that pin, in its own commit, with the harness run against the new commit.

In the libtsm fork:

```sh
make test                      # unit tests and tier 1
make cases                     # rewrite the synthetic streams
make record                    # re-record the program streams
make golden REF=v4.8.0         # rewrite tier 1 goldens from upstream at REF
```

## Status

| Tier | State |
|---|---|
| 1 | Running: 26 cases (22 synthetic, 4 recorded programs with 27 checkpoints between them), all matching upstream |
| 2 | Running: fourteen screens, pixel-exact: ASCII, bold, palette, inverse, CJK, box drawing; the ADR-102 decorations (each underline style in the foreground and in an SGR 58 colour, strike-through, configured decoration colours with and without force-decoration-colors, decorated wide cells); combining marks shaped by HarfBuzz; a heart with VS16; and a redraw case drawn over four frames into two swapped buffers, which judges the two-frame damage rule. `test_blend` checks the drm2d and fbdev (16, 24 and 32 bpp) blend maths. Coverage gaps tracked in #8 |
| 3 | Running: two 2D variants (virtio-vga at 1280x800 and 7680x2160, drm2d + bbulk), five cases each (SGR, box drawing, CJK, the switch to another VT, the screen restored on switching back), all pixel-exact. The GL variant (drm3d + gltex under virgl) has goldens but is outside the default run: it intermittently draws a black screen, #17. Runs locally only. |
| CI | Not wired yet. ADR-301 has tiers 1 and 2 running in CI on every change; until then they run locally through `make test`, and the repository's GitHub workflow is upstream's plain build. |
