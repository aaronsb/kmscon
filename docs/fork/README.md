# About this fork

This is a fork of [kmscon](https://github.com/kmscon/kmscon), the user-space console that runs on Linux kernel mode setting, maintained by Aaron Bockelie. It works toward one goal: a console that needs no compositor and still draws what modern terminal programs send it, including images, colour emoji and styled underlines, without giving up the security and reliability that make kmscon a sound replacement for the kernel's VT.

Everything here is MIT-licensed, like kmscon and libtsm, and is meant to be offered back to upstream once it has earned that.

## The goal

A terminal on the bare console that handles what a desktop terminal like kitty handles:

- the kitty graphics protocol and sixel, for inline images;
- colour emoji and multi-codepoint characters (ZWJ sequences, flags, skin tones);
- styled and coloured underlines (`SGR 4:x`, `58`, `59`), as editors use for diagnostics;
- later, the kitty keyboard protocol.

The console keeps working when the graphics stack does not: on DRM without OpenGL, on fbdev, as a recovery terminal.

## Why kmscon

kmscon already has the hard and security-sensitive part right. It runs as a systemd service in place of getty, opens displays and input through DRM, udev and libseat, and hands them back on a VT switch. Its maintainer is active, and its direction matches the kernel's: the kernel keeps a minimal console for boot messages and panics, and terminals move to user space.

What kmscon lacks is everything between parsed bytes and pixels that these features need. [ADR-100](../architecture/render/ADR-100-kmscon-render-pipeline-as-of-upstream-6c03eb4.md) records it in detail: glyphs travel as one byte of alpha from the font to the screen, a cell has a single underline bit, sixel and kitty sequences are discarded by the parser, and multi-codepoint characters are drawn blank.

## What changes and what does not

[ADR-101](../architecture/render/ADR-101-rebuild-the-screen-model-and-render-path-around-sprites-and-an-image-layer.md) draws the line:

| Kept as upstream maintains it | Replaced |
|---|---|
| VT and seat handling, DRM and fbdev display setup, input (`src/uterm`, `src/input`) | libtsm's screen and cell model |
| Font discovery and selection | The glyph format, glyph cache and renderer interface |
| libtsm's escape parser, as the behavioural baseline, extended where noted | Additions to the parser: DCS and APC handling, CSI sub-parameters |

The replacement is built around three ideas: cells that carry structured attributes and a key for a whole grapheme cluster; one sprite cache that holds both alpha masks and full-colour glyphs, with underline styles as sprites of their own; and an image layer of placements that the screen keeps in step on every scroll, erase and resize.

## Rewriting libtsm is not taken lightly

libtsm is small, and its screen model is the heart of every terminal built on it. It carries years of fixes for the byte streams real programs send. A rewrite can lose those silently, and a terminal that misdraws a shell prompt or an editor is worse than one that cannot draw images.

So the rewrite is held to these rules:

- **The harness comes first.** No line of the new screen model lands before the conformance harness that judges it ([ADR-301](../architecture/fork/ADR-301-gate-rendering-changes-on-a-three-tier-conformance-harness.md)). The harness records what upstream libtsm does today, and the rebuilt model must reproduce it.
- **Behaviour changes are named.** Where the fork changes what a sequence does, the change appears as an updated golden in its own reviewed commit. Two such changes are already known: upstream discards a whole SGR sequence that contains a colon sub-parameter, and reads `58;5;n` so that the `5` turns on blink.
- **The parser stays** until the harness shows a reason to replace it.
- **The platform layer is not touched.** The parts that decide who may read the keyboard and draw on the screen stay exactly as upstream maintains them.
- **Each step is usable on its own.** The order is the harness, then colour emoji and underline styles, then images and sixel, then the kitty graphics protocol.

## Relationship with upstream

The kmscon maintainer has said that sixel or kitty images would need most of libtsm rewritten and a large part of kmscon's rendering, a task the maintainer will not take on (kmscon issue #138). This fork does that work where it cannot disturb anyone, keeps itself mergeable with upstream, and brings fixes in from upstream as they land. The fork's own code lives in its own files; upstream files change only at named hook points.

When the work is mature it will be offered upstream as a proposal with evidence: the decision records, the conformance ledger, and a fork that has kept building and passing against upstream over time. Whether and how to adopt any of it is upstream's decision.

## Clean room

Kitty is GPL-3.0. This fork takes kitty's published protocols and its architectural ideas, described in our own words, and none of its code. Nobody working on the fork, person or agent, opens kitty's source. Permissive implementations (Ghostty, WezTerm, foot, libsixel) may be consulted and are credited where anything is adapted. [ADR-300](../architecture/fork/ADR-300-write-from-protocol-specs-and-permissive-sources-never-from-kitty-s-code.md) has the full rule.

## How the work is produced

Much of the code and documentation in this fork is written by AI coding agents under the maintainer's direction. The decisions are the maintainer's and are recorded in the decision records in the maintainer's own words. The code is judged by the conformance harness rather than by its author: every change is measured against goldens produced from upstream behaviour, and every check has been seen to fail before it is trusted.

## Where to read next

- [Conformance harness](conformance.md): how the fork is tested and how to run it.
- [Local VT demo](demo.md): a development build on one of this machine's VTs, with a paged demonstration of what it draws.
- [Decision records](../architecture/INDEX.md): the evidence, the decisions and their reasons.
- [Conformance ledger](../conformance/LEDGER.md): one row per recorded run, with a link to its run page.

## Building

Every build step runs in a container as the invoking user; the host needs only `docker` and `make`.

```sh
make test                    # build and run kmscon's and libtsm's test suites, including the harness
make test LIBTSM=../libtsm   # the same, against a local checkout of the libtsm fork
make demo                    # stage the package for a VT demo: sudo fork/demo/run-vt.sh (see demo.md)
make help                    # every target
```

kmscon builds against [the libtsm fork](https://github.com/aaronsb/libtsm), statically, so a system libtsm is never used and nothing from libtsm is installed.
