---
contract: adr/v1
kind: decision
verb: add
capability: process
basis:
  - operator: aaronsb
    level: guided
    said: "we can set up an eval flow perhaps, where we have a virtual machine (kvm?) to build on"
    via: Claude Code session, while planning the pipeline rebuild
  - operator: aaronsb
    level: guided
    said: "If we can prove our build is 1) durable and functional with kitty (and possibly also sixel) support and 2) survives reasonable changes from upstream, I would be able to bring in fixes from kmscon as well as offer a chance to adopt what we have built with an ongoing proof of reasonable maturity"
    via: Claude Code session, when the fork was created
  - precedent: ADR-100
  - evidence: "Build host has /dev/kvm, the operator in the kvm group, qemu-full 11.1.2 and virglrenderer 1.3.0 (checked 2026-10-20)"
agent:
  name: claude
  model: claude-opus-5-5
considered:
  - operator: aaronsb
    said: "I read through the ADRs and they make sense to me and line up with what we discussed. I don't see any surprise decisions"
    via: Claude Code session, reviewing PR aaronsb/kmscon#1
status: accepted
date: 2026-10-20
deciders:
  - aaronsb
related: []
---

# ADR-301: Gate rendering changes on a three-tier conformance harness

## Summary

- **Decided:** A change to parsing, the screen model or rendering lands only when three tiers agree: screen-state replay of recorded byte streams, pixel comparison of the renderer drawing into memory, and an end-to-end run of the built package in a virtual machine with a real KMS device. The first two run in CI on every change; the third runs before a rendering change merges and on every upstream sync.
- **Trades away:** Speed of the first few changes, which wait for the harness to exist, and the upkeep of reference outputs.
- **One-way?** No. Tiers can be added or retired; the reference outputs are files in the tree.
- **Probes:** *Confident:* the screen-state tier catches parser and screen regressions cheaply, since libtsm's existing tests cover none of draw output, SGR underline or DCS. *Not confident:* that QEMU's virtio-gpu screenshots are stable enough for exact pixel comparison; the VM tier may need tolerance or region masks.
- **Inversion:** Between "trust the unit tests" and "test only by hand on the console". The answer is neither: mechanical comparison at three depths, each cheaper than the one below it.

## Context

kmscon's `meson test` runs eight tests and none inspects pixels; libtsm's tests do not cover the draw output, SGR underline or DCS (ADR-100). The pipeline rebuild (ADR-101) replaces the screen model and the render path, which are exactly the parts nothing tests. A terminal's compatibility comes from years of fixes for real programs' byte streams, and a rewrite loses them silently unless something compares old and new behaviour.

The fork is also meant to show, over time, that it builds and works against upstream as upstream moves, so the evidence has to be produced mechanically and repeatedly, not by someone looking at a screen.

## Decision

### Tier 1: screen-state replay (CI, every change)

- Recorded byte streams from real programs (shell sessions, tmux, vim, ncurses applications, vttest, esctest) are replayed into the screen model, and the resulting cells, attributes, cursor and placements are serialised to text.
- For behaviour the rebuild must preserve, the reference is upstream libtsm replaying the same stream. For new behaviour (underline styles, clusters, images), the reference is a reviewed golden file.
- A difference fails the run. A difference that is intended is accepted by updating the golden in its own reviewed change, never alongside the code that produced it.

### Tier 2: pixel comparison in memory (CI, every change)

- The renderer draws into an in-memory XRGB framebuffer through the video backend's stub seam (the shape `test_bbulk` already uses), for a fixed set of screens covering text, wide and cluster glyphs, colour emoji, each underline style and colour, and image placements in each band.
- Output is compared with golden images. Font files used by the tier are pinned in the tree so a font package update cannot move the goldens.

### Tier 3: end to end in a virtual machine (before merge of rendering changes; on every upstream sync)

- A pinned Arch Linux guest under QEMU/KVM boots with a virtio-gpu display at fixed resolutions, including a very large one, and with virgl for the GL renderer.
- The fork's package is built and installed in the guest exactly as a user would install it. kmscon runs on a VT, scripted input arrives through QEMU's control socket (QMP), and screenshots come back through it.
- Screens are compared with golden images, with masks for regions that legitimately vary (clock, cursor blink). The run also exercises VT switching away and back.

### Rules that apply to every tier

- The tier's exit status is the verdict. A run that reports failures and exits zero is a harness defect.
- Each comparison has been seen to fail once, by a planted difference, before it is trusted.
- A defect found by hand on the console becomes a case in the lowest tier that can express it, before the fix lands.
- Black-box comparison with kitty, for kitty protocol behaviour the specification leaves open, uses kitty as a running program only (ADR-300).

## Consequences

### Positive

- Parser and screen regressions surface in seconds, not on the console.
- The rebuild can replace large pieces with a mechanical answer to "did anything else change".
- Upstream syncs carry their own evidence: the VM tier builds and runs the merged result.
- A maintainer considering the work receives test results along with the code.

### Negative

- The harness is built before the first visible feature.
- Golden files need review discipline; accepting a golden without reading it turns the gate into a recorder.
- The VM tier is slow and needs KVM, so it runs locally and on demand rather than in hosted CI.

### Neutral

- Recorded byte streams become a growing corpus of real-program behaviour that outlives this rebuild.

## Alternatives Considered

- **Unit tests only.** Rejected: unit tests check what their authors thought of, and the risk in a rewrite is behaviour nobody thought of.
- **Manual testing on the console.** Rejected as a gate: it cannot be repeated per change or per upstream sync, and it produces no evidence anyone else can read. It remains how a person accepts a golden.
- **The VM tier alone.** Rejected: too slow to run on every change, and a failing screenshot says little about where the fault is. The cheaper tiers localise faults; the VM tier proves the assembled package.

## Amendment, 2026-10-21: three verdicts and marked differences

The operator asked that the record of each run "circle/mark" the areas that do not conform and allow "a bit of stretch if it's not pixel identical". This amends the rules for tiers 2 and 3; tier 1 compares text and stays exact.

- A pixel comparison has three verdicts: **pass** (identical outside masks), **near** (differs, but within the case's tolerance; the gate passes), and **fail** (beyond tolerance). A missing golden also fails.
- A case's tolerance and masks live in a committed sidecar beside its golden: the largest per-channel difference allowed, the number of pixels allowed to differ, and rectangles excluded from comparison. No sidecar means exact. A sidecar change is reviewed and committed on its own, like a golden, so the allowance cannot widen unnoticed.
- Each comparison reports the regions that differ as a short list of clustered rectangles, and the contact sheet (`make sheet`, ledger in `docs/conformance/`) outlines those regions on the golden and the actual image. For tier 1 the sheet outlines the cells that differ.
