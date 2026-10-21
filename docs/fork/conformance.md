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

## Contact sheets and the ledger

Each recorded run produces a contact sheet: one SVG with a tile for every case and checkpoint. A passing tile shows the screen; a failing tile shows the golden beside the actual result with every region that differs outlined. Image tiers add the diff image, and a within-tolerance case is framed and outlined in amber. The sheet is drawn from the same report the verdict came from, so the two cannot disagree, and the same run always draws the same bytes.

Milestones commit their sheet under `docs/conformance/sheets/` and add a row to [the ledger](../conformance/LEDGER.md): the date (UTC), what the run marks, the kmscon and libtsm commits measured, and the pass count of each tier. Over time the ledger shows the fork building and conforming against upstream as both move.

## Commands

In the kmscon fork:

```sh
make test                      # all suites, including tiers 1 and 2
make test LIBTSM=../libtsm     # against a local libtsm checkout
make tier2                     # tier 2 only, with its report
make tier2-golden              # regenerate tier 2 goldens (review, then commit on their own)
make sheet-preview             # draw this run's contact sheet into fork/build/sheet.svg
make sheet NOTE="what it marks"   # commit-ready sheet and ledger row; refuses uncommitted changes
```

Tier 3's commands arrive with its pull request.

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
| 2 | Running: six screens (ASCII, bold, palette, inverse, CJK, box drawing), pixel-exact; coverage gaps tracked in #8 |
| 3 | In progress |
| CI | Not wired yet. ADR-301 has tiers 1 and 2 running in CI on every change; until then they run locally through `make test`, and the repository's GitHub workflow is upstream's plain build. |
