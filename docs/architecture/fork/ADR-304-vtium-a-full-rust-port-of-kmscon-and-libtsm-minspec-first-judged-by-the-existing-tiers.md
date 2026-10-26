---
contract: adr/v1
kind: decision
verb: add
capability: process
basis:
  - operator: aaronsb
    level: directed
    said: "while I experiment here - I think the direction is clear for our fork which has a future as kmscon + libtsm as a rust fork. kmscon-rust"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead
  - operator: aaronsb
    level: guided
    said: "also rust port of our work is probably pretty fast with claude"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead
  - operator: aaronsb
    level: directed
    said: "could we take apart the c code and essentially build a rust port of libtsm and kmscon, leaving all of it behind?"
    via: "Claude Code session, 2026-10-26, relayed verbatim by the team lead; one sentence of a longer message, in which the operator stated the premise \"All code was written by an LLM\""
  - operator: aaronsb
    level: directed
    said: "david herrman and contributors should definitely be quoted orginally. we could in that tagline you wrote replace potato with 'minspec' (we'll keep potato internally though). this is a clean break from kmscon, doesn't force a social convention, doesn't couple us to opinionated works. we can license it the same as kmscon (MIT). we can take what we've learned from flow (a full golden eval pass on every merge pendantically might be too much, so we do some logical bundling, for example)"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead; "that tagline" is the session's draft title for this record
  - operator: aaronsb
    level: directed
    said: "what I want to try and do is make the items consistent, so an agent or worktrees of subagents can work through the work efficiently. one thing that slows us down is constant full product review/eval. so this means we should build a bunch, then run a single eval pass for things, then the next - this should cut down the time to build"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead
  - operator: aaronsb
    level: guided
    said: "we've largely set out what we wanted to achieve at this point: text formatting and sixel and kitty support. all the bits we're tracking now is what makes it a good product, and I believe our general architecture will be a bit more stable at this point"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead
  - operator: aaronsb
    level: guided
    said: "well, capability first is still true - the architecture may need adjustment still"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, said after the statement above
  - operator: aaronsb
    level: guided
    said: "vtium has a nice sound to it"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, choosing the project's name in place of the working title kmscon-rust
  - precedent: ADR-303
  - precedent: ADR-301
  - precedent: ADR-103
  - precedent: ADR-201
  - precedent: ADR-300
  - upstream: "kmscon and libtsm are MIT-licensed. kmscon's COPYING gives `Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>` for any file without its own notice; libtsm's COPYING gives `Copyright (c) 2017-2018 Aetf <aetf@unlimitedcodeworks.xyz>` and `Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>`. Both say each source file carries its own notice. Read at kmscon a53867e6 and libtsm 9ec60d6"
  - upstream: "`kmscon/kmscon issue 138`, comment by upstream's maintainer, 2026-10-26: libtsm is to be converted to Rust on the `vte` crate and merged into kmscon, and kmscon converted to Rust later"
  - evidence: "vte 0.15.0 (crates.io, Apache-2.0 OR MIT), src/lib.rs: ESC X, ESC ^ and ESC _ enter State::SosPmApcString, whose bytes go to `anywhere()` and reach no Perform callback, so an APC string's content is dropped. The OSC buffer `osc_raw` is an `ArrayVec` bounded by MAX_OSC_RAW (1024) only under `not(feature = \"std\")`; under `std`, a default feature, it is a `Vec` that grows with the string"
  - evidence: "aaronsb/libtsm at 9ec60d6: the parser has STATE_APC_STRING for kitty graphics commands, OSC_MAX_LEN 128, CSI_ARG_MAX 16 and CSI_SUBARG_MAX 8 for colon sub-parameters, and the XTGETTCAP bounds (a 1039-byte payload, 16 names, a reply under 4096 bytes); `struct cell` is held to 32 bytes by a static assert in libtsm-int.h; test/conformance/tsm-replay.c prints screen state and replies as text through the public API only"
agent:
  name: claude
  model: claude-opus-5-5
observable:
  - "R0: tier 1's cases and the capability probes that feed libtsm bytes run through one replay program, whose input and output are written down, and the C implementation passes every golden through it"
  - "vtium's minspec build passes `make tier3-minimal`'s two variants, `2d-potato-min` and `2d-potato-min-psf`, pixel-exact"
  - "vtium's README states the provenance, and its licence file quotes kmscon's and libtsm's copyright notices from its first commit"
  - "the minspec build's size and memory targets are written down before the first Rust build, and tier 3's report measures them on every run from that build"
status: proposed
date: 2026-10-26
deciders:
  - aaronsb
related: [ADR-303, ADR-301, ADR-103]
---

# ADR-304: vtium: a full Rust port of kmscon and libtsm, minspec first, judged by the existing tiers

## Summary

- **Decided:**
  - **vtium.** The fork's future is vtium, a full Rust port of kmscon and libtsm that leaves the fork's C code behind, in a new repository, vtium/vtium. It is named apart from kmscon because upstream plans a Rust kmscon of its own. It is a clean break from upstream kmscon. ADR-303 keeps governing the C fork and does not apply to vtium.
  - **Licence and provenance.** vtium is MIT, as kmscon and libtsm are, and carries David Herrmann's and the other contributors' copyright notices from its first commit. Its README says all of its code is written by an LLM, under the operator's review, judged by the harness.
  - **Our own parser.** The escape-sequence parser is ported from the fork's libtsm. vtium depends on a crate only where it never needs to change the crate.
  - **Minspec first.** The first Rust kmscon is the minimal build as a vertical slice through every layer. FreeType and Pango text come next, then images, then the GPU path, then libseat, multi-monitor and the rest.
  - **The tiers judge it.** Tier 3 judges the Rust kmscon unchanged. Tier 1 and the capability probes move behind a replay interface, built first in the C repositories (R0). Tier 2's pixel tests are ported with the renderer and keep their goldens.
  - **The C fork is frozen.** It takes security fixes only, and stays the reference implementation and the harness's control.
  - **Evaluation is batched.** Items are built in bundles of work that does not collide, and each bundle gets one evaluation pass.
- **Trades away:** New features in the C fork until vtium replaces the part they belong to. The C fork's upstream syncs and ADR-303's route to showing the work upstream. The months the port takes, during which the capability batches tracked against the C fork wait.
- **One-way?** No, expensive. The C fork stays intact as the reference, so stopping the port returns to it and loses only the freeze. The cost grows as features are built in Rust that the C fork does not have.
- **Probes:**
  1. *Confident (freeze):* while the port is under way, the C kmscon and libtsm get only security fixes. A feature you want in the meantime waits until vtium has the part it belongs to, and is built there. Is that the freeze you meant?
  2. *Not confident (interface):* vtium reads the same `kmscon.conf` and command line as today's kmscon, at least where the tests use them, so the VM tests judge it without being rewritten. Is keeping that compatibility what you want, or should vtium be free to rename options and change defaults from the start?
- **Inversion:** Between "keep building the C fork" and "start a new console from scratch". The answer ports the fork's behaviour whole and keeps the C fork running beside it as the answer key, so every part of the port is checked against what it replaces.

## Context

The fork has built what it set out to build. In the operator's words, "we've largely set out what we wanted to achieve at this point: text formatting and sixel and kitty support. all the bits we're tracking now is what makes it a good product". The operator judges the architecture "a bit more stable at this point", with the caveat that "capability first is still true - the architecture may need adjustment still". The operator has set the direction: "I think the direction is clear for our fork which has a future as kmscon + libtsm as a rust fork. kmscon-rust", asked whether the port could leave the C code behind ("could we take apart the c code and essentially build a rust port of libtsm and kmscon, leaving all of it behind?"), and expects it to go quickly: "also rust port of our work is probably pretty fast with claude".

The harness already judges behaviour from outside the code. Tier 3 (ADR-301) boots the installed package in a VM and compares screenshots pixel for pixel, and `make tier3-minimal` does the same for the minimal build on ADR-103's potato machine (1366x768, 1 vCPU, 512 MB). Tier 1's `tsm-replay` prints screen state and replies as text through libtsm's public API, and its header calls that text "the conformance contract". The capability probes in kmscon's `tests/test_capabilities.c` call libtsm's API directly, so they are C programs. A port can be judged by the same tiers if those C-bound parts move behind an interface first.

The operator wants the work cut into consistent items that agents can work through in parallel, evaluated in batches: "one thing that slows us down is constant full product review/eval. so this means we should build a bunch, then run a single eval pass for things, then the next".

Upstream's maintainer wrote on 2026-10-26, in `kmscon/kmscon issue 138`, that they plan to convert libtsm to Rust on the `vte` crate and merge it into kmscon, and to convert kmscon later.

## Decision

### 1. The repository and its relation to upstream

vtium is a new repository, vtium/vtium, in a GitHub organization `vtium` that the operator creates. Its working title was kmscon-rust. It ports kmscon and libtsm to Rust whole, as one project, and leaves the fork's C code behind. It follows no upstream convention as a rule and does not couple its plans to any upstream project's process: "this is a clean break from kmscon, doesn't force a social convention, doesn't couple us to opinionated works". ADR-303 keeps governing the C fork. It does not apply to vtium, and this record does not supersede it.

The name is distinct because upstream plans to convert kmscon itself to Rust, so "kmscon in Rust" would mean upstream's. A distinct name keeps packagers and bug reporters from confusing the two, and does not suggest that vtium is upstream's sanctioned Rust kmscon. The name was free as a GitHub organization and as a crates.io crate on 2026-10-26. vtium describes itself as a Rust port derived from kmscon and libtsm by David Herrmann and contributors.

vtium keeps the C kmscon's interface where the tiers drive it: the `kmscon.conf` keys and command-line options the tier 3 variants use, the replies tier 1 and the probes check, and the `kmscon` terminfo entry. That is the harness's interface. A part that changes it updates the harness in the same bundle, and its record says so (probe 2).

vtium is independent of upstream's Rust work. The fork's `upstream-bug` list and its conformance suite stay available to upstream if the operator chooses to offer them.

### 2. Licence and provenance

vtium is MIT-licensed, as kmscon and libtsm are. The port is a derivative work, so its licence file carries, from the first commit and beside the fork's own line:

- kmscon's COPYING notice, `Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>`, with the MIT permission text below it and kmscon's list of authors;
- libtsm's COPYING notices, `Copyright (c) 2017-2018 Aetf <aetf@unlimitedcodeworks.xyz>` and `Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>`, with libtsm's list of authors;
- the notice of each C file a Rust file is ported from, kept in the Rust file. The holders there include Ran Benita, Ted Kotz, the University of Tuebingen, Red Hat, Alberto Ruiz, and Mitchell Hashimoto and the Ghostty contributors for the geometry `font_box.c` adapts.

Third-party notices carry over with what they cover: unifont's OFL-1.1 notice while vtium ships unifont, which the minspec build does; the solarized palettes' MIT notice if the palettes are ported; the Unicode License v3 for tables generated from the Unicode Character Database. CCAN's htable (LGPL-2.1) and terminology's UCS-4 to UTF-8 encoder are not ported, since Rust's standard library does both jobs, and their notices do not carry over.

The README states the provenance: all code is written by an LLM, under the operator's review, and judged by the harness. This record states it too.

### 3. Dependencies, and the parser

vtium depends on a crate only where it never needs to change it. Anything it would have to patch, it owns.

The escape-sequence parser is therefore vtium's own, ported from the fork's libtsm: the DEC state machine from vt100.net with the fork's fixes. APC strings reach a handler, for kitty graphics. DCS and OSC collection is bounded. Intermediates are passed to the dispatch and checked. Colon sub-parameters are parsed. XTGETTCAP keeps its bounds. ADR-201's mode registry is ported as the parser's table.

The `vte` crate (version 0.15) enters the SOS/PM/APC state on ESC _ and passes the string's bytes to no callback, so kitty graphics commands are dropped. Its OSC buffer is bounded at 1024 bytes only without its `std` feature, which is on by default. Using it would mean carrying patches to it in a fork of our own.

The platform C libraries stay, as FFI dependencies or through crates that wrap them: libdrm (or the `drm` crate), libxkbcommon, libudev and later libseat, FreeType, HarfBuzz and fontconfig. Leaving C behind means leaving the fork's own C.

ADR-300 applies to vtium: no source from a GPL terminal is read while writing it, and that includes Rust crates derived from kitty.

### 4. The cell model is designed fresh

vtium does not inherit libtsm's `struct cell` or its 32-byte cap. The new cell model budgets from the start for what the fork has tracked as missing: reflow's wrapped-line flag (libtsm#28), hyperlink ids (#105), overline (libtsm#27) and conceal. The size it settles on is measured against the potato machine's memory, as ADR-103 measures images.

### 5. Minspec first

The first Rust kmscon is the minimal build, called minspec here and the potato build inside the project, after ADR-103's potato computer standard. It is a vertical slice through every layer:

- drm2d and fbdev, with a CPU renderer;
- PSF and unifont fonts;
- the parser and the screen;
- the pty;
- xkb input;
- udev device discovery and the kernel's VT interface, which tier 3's VT switch case needs.

It has no images, no GL, no FreeType, no Pango and no D-Bus, as the C minimal build has none (ADR-103). Then the layers come in this order:

1. FreeType and Pango text;
2. sixel, the kitty graphics protocol and the image store;
3. the GPU path;
4. libseat, multi-monitor and the rest.

The minspec build has a size target and a memory target, set before the first Rust build and measured from it by tier 3's report. The C minimal build's figures on the same variants are the reference: 10.3 MiB resident after the first screen with unifont and 5.6 MiB with a PSF font, and a peak of 15.7 MiB and 11.0 MiB with a full scrollback (README, "Minimal build").

### 6. Judged by the existing tiers

- **Tier 3** is black-box and judges the Rust kmscon unchanged. `make tier3-minimal` judges the minspec build. The default variants judge the later layers as they arrive: the 2D variants need FreeType, Pango and images, and the GL variant needs the GPU path.
- **Tier 1 and the probes, R0.** The first work item is a language-neutral replay interface, built in the C repositories: a program takes a terminal size, a byte stream, optional checkpoints and the host settings a case names (such as images off or the XTGETTCAP table), and prints the screen state and the replies as text. `tsm-replay` is that program for tier 1 today; R0 writes its input and output down as the interface. The probes in `test_capabilities.c` that feed bytes and check a reply or the screen become replay cases. The font probes stay with the FreeType layer, and the terminfo probes already read only `kmscon.ti`. The C implementation passes every golden through the interface before vtium relies on it. R0 is test infrastructure and is allowed under the freeze.
- **Tier 2's pixel tests** are ported with the renderer and keep their goldens. Each screen is ported when the layer it draws arrives.
- **Goldens.** Goldens for behaviour the C fork has stay in the C repositories, and vtium pins them by commit, as kmscon pins libtsm today. A golden changes in one place, reviewed against the C implementation. Goldens for behaviour only vtium has live in vtium.

### 7. The C fork is frozen

The C kmscon and libtsm are the reference implementation and the harness's control. Until vtium passes the tiers that cover a part, the C fork takes security fixes only. It takes no new libtsm features, so the C capability batches pause, and no upstream syncs, since a sync moves the control. A feature for a part is built in vtium once vtium has that part.

### 8. Evaluation is batched

Work is cut into items of one form: the modules or files an item touches, the gates that judge it (named tests, tier cases and probes), what done looks like, and the items it depends on. A bundle is a set of items whose scopes do not overlap and that do not depend on one another, so parallel agents in separate worktrees can build them at once. An item merges into its bundle's branch once it builds and its own unit tests pass. The bundle then gets one evaluation pass: one review of the bundle, and the union of its items' gates run once. A failing item goes back to its builder or leaves the bundle. The bundle merges to `main` when the pass is clean. This keeps ADR-301's rule that a change to parsing, the screen or rendering lands on `main` only when the tiers agree; the change is the bundle.

## Consequences

### Positive

- Every part of the port is checked against the C implementation it replaces, through tiers that already exist and already pass against C.
- The minspec slice exercises every layer early, and `make tier3-minimal` judges it with no harness work beyond R0.
- R0 leaves tier 1 and most probes runnable against any implementation, upstream's included.
- The parser keeps the fork's fixes, and vtium carries no patched fork of a crate.
- Bundled evaluation runs review and the slow gates once per bundle, where the C fork ran them once per change.

### Negative

- C fork users get no new features until vtium replaces the part, and the C fork falls behind upstream while frozen.
- Two implementations exist during the port. A security fix lands in the C fork and, where the part is ported, in vtium.
- Pixel-exact goldens tie tier 2 and tier 3 to the font stack's output. A Rust font stack that rasterises differently moves goldens, which favours FFI to FreeType at first (question 2).
- A bundle that fails its pass holds back its other items until the failing one is fixed or removed.

### Neutral

- ADR-303's plan to build a history of upstream syncs and then show the work upstream applies to the C fork, which no longer moves. Whether the C fork's work is offered upstream is the operator's call.
- The fork's records stay in aaronsb/kmscon. vtium cites them by number until it keeps records of its own.
- The C fork's future after vtium passes every tier is a later decision.

## Alternatives Considered

- **Keep building the C fork.** Rejected by the operator's direction. The capabilities the fork set out to prove are proven, and the remaining work is product work the port can absorb.
- **Replace libtsm with Rust inside the C kmscon, then kmscon.** It moves one library at a time and keeps the build mixed throughout. Rejected: it keeps vtium inside the C fork's build and conventions, where the operator asked for a clean break.
- **Build on upstream's Rust conversion.** Rejected: it ties vtium's direction and timing to another project's plan ("doesn't couple us to opinionated works").
- **Use the `vte` crate as the parser.** Rejected: it drops APC strings, and its OSC buffer is unbounded under its default feature. Both would be patches vtium carries in a fork of the crate.
- **Port the whole product first, and cut the minimal build out of it later.** Rejected: nothing would pass a tier until most layers existed. The minspec slice passes `make tier3-minimal` on its own.
- **New tests written for the Rust code.** Rejected: they would lose the C implementation as the control, and the goldens it already proves.
- **A full golden pass on every merge.** ADR-301's cadence for the C fork. Rejected for vtium by the operator: "a full golden eval pass on every merge pendantically might be too much, so we do some logical bundling".
- **A pure-Rust stack down to the platform.** Rejected for now: libdrm, libxkbcommon, udev, libseat and the font libraries are not the fork's code, and replacing them adds risk with no capability behind it.

## Questions for the operator

1. **GPU path.** glow on EGL and GBM, a direct port of gltex, or wgpu. The lean is glow: gltex's shaders and atlas port as they are, and tier 3's GL variant judges them against gltex.
2. **Font stack.** FFI to FreeType, HarfBuzz and fontconfig first, for parity with tier 2's goldens, or pure-Rust swash and rustybuzz. The lean is FFI first, with pure Rust as a later layer that moves goldens in its own reviewed change.
3. **Event loop.** The lean is calloop: it wraps file descriptors, timers and signals, as kmscon's `eloop` does.
4. **Minspec targets.** What size and memory targets the minspec build holds to. The lean is no worse than the C minimal build on the same tier 3 variants: resident memory after the first screen, peak memory, and package size.
5. **When the repository is created.** The lean is once R0 lands, so vtium's first commit can pin the goldens it is judged by.
6. **Upstream syncs during the freeze.** The record pauses them, since a sync moves the control. Should a sync that brings a security fix be taken whole, or only the fix?
7. **When the crate name is published.** crates.io discourages placeholder crates. The lean is to publish `vtium` with the first real code.
