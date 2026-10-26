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
  - operator: aaronsb
    level: guided
    said: "the vtium repo, because rust, doesn't need the seperate lib repo, I think?"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, who agreed
  - operator: aaronsb
    level: guided
    said: "any sort of contribution needs to be able to pass the evaluation gauntlet with proofs"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, on vtium's contribution policy
  - operator: aaronsb
    level: guided
    said: "if the contribution adds something new, then the eval harness needs to be proven to be extended too"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, on vtium's contribution policy, said after the statement above
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
considered:
  - operator: aaronsb
    said: "p1 yes"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, answering probe 1 (freeze)
    covers: [freeze]
  - operator: aaronsb
    said: "p2 yes"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, answering probe 2 (interface)
    covers: [interface]
  - operator: aaronsb
    said: "q1 a direct port of gltex"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, answering question 1 (GPU path)
  - operator: aaronsb
    said: "q2 I'm not so sure about this one. bindings likely to persist for a long time and be hard to back out/clean up later"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, answering question 2 (font stack), whose lean was FFI to FreeType, HarfBuzz and fontconfig first
  - operator: aaronsb
    said: "q3 yes"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, answering question 3 (event loop, lean calloop)
  - operator: aaronsb
    said: "q4 agree"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, answering question 4 (minspec targets, lean no worse than the C minimal build)
  - operator: aaronsb
    said: "q5 published"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, answering question 5 (when the repository is published); vtium/vtium is public and its first CI run is green
  - operator: aaronsb
    said: "q6 agree - it is likely not even in the same language"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, answering question 6 (upstream syncs during the freeze)
  - operator: aaronsb
    said: "the vtium repo, because rust, doesn't need the seperate lib repo, I think?"
    via: Claude Code session, 2026-10-26, relayed verbatim by the team lead, who agreed; also in basis, since Decision §1's one-repository paragraph rests on it
  - operator: aaronsb
    said: "Picked \"Any contribution, disclosed\": vtium accepts human- or LLM-written code, and each PR says how it was written; the maintainer's own code stays LLM-written under review."
    via: Claude Code session, 2026-10-26, relayed by the team lead as the operator's pick among drafted options for vtium's contribution policy, not verbatim
    paraphrase: true
  - operator: aaronsb
    said: "any sort of contribution needs to be able to pass the evaluation gauntlet with proofs"
    via: Claude Code session, 2026-10-26, relayed by the team lead verbatim, on vtium's contribution policy; also in basis
  - operator: aaronsb
    said: "if the contribution adds something new, then the eval harness needs to be proven to be extended too"
    via: Claude Code session, 2026-10-26, relayed by the team lead verbatim, on vtium's contribution policy, said after the statement above; also in basis
observable:
  - "R0: tier 1's cases and the capability probes that feed libtsm bytes run through one replay program, whose input and output are written down, and the C implementation passes every golden through it"
  - "vtium's minspec build passes `make tier3-minimal`'s two variants, `2d-potato-min` and `2d-potato-min-psf`, pixel-exact"
  - "vtium's README states the provenance and the contribution policy, its CONTRIBUTING and PR template ask each PR how its code was written and for its proofs, and its licence file quotes kmscon's and libtsm's copyright notices from its first commit"
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
  - **Licence and provenance.** vtium is MIT, as kmscon and libtsm are, and carries David Herrmann's and the other contributors' copyright notices from its first commit. Its README says the maintainer's code is written by an LLM, under the operator's review, and judged by the harness. Contributions are welcome however they were written, and each says how.
  - **Our own parser.** The escape-sequence parser is ported from the fork's libtsm. vtium depends on a crate only where it never needs to change the crate.
  - **Minspec first.** The first Rust kmscon is the minimal build as a vertical slice through every layer. Outline-font text comes next, then images, then the GPU path, then libseat, multi-monitor and the rest.
  - **One repository.** libtsm's role is carried by crates in vtium's workspace, so there is no separate library repository and no pin to bump.
  - **The stack.** Fonts are pure Rust from the start, with no bindings to FreeType, HarfBuzz or fontconfig. The GPU path is a direct port of gltex on glow over EGL and GBM. The event loop is calloop.
  - **The tiers judge it.** Tier 3 judges the Rust kmscon unchanged. Tier 1 and the capability probes move behind a replay interface, built first in the C repositories (R0). Tier 2's pixel tests are ported with the renderer. Goldens drawn with bitmap fonts are kept, and goldens drawn with outline fonts are re-baselined when the Rust font layer lands, each changed golden reviewed on its own.
  - **The C fork is frozen.** It takes security fixes only, cherry-picked, and stays the reference implementation and the harness's control. Full upstream syncs stop.
  - **Evaluation is batched.** Items are built in bundles of work that does not collide, and each bundle gets one evaluation pass.
  - **Contributions.** Any contribution is accepted, human- or LLM-written, if its PR says how it was written and proves it passes the gates and tiers that cover it. Anything new extends the harness in the same PR. The code of conduct is the Contributor Covenant 2.1.
- **Trades away:** New features in the C fork until vtium replaces the part they belong to. Pixel parity with the C fork for outline-font text, whose goldens are re-baselined. The C fork's upstream syncs and ADR-303's route to showing the work upstream. The months the port takes, during which the capability batches tracked against the C fork wait.
- **One-way?** No, expensive. The C fork stays intact as the reference, so stopping the port returns to it and loses only the freeze. The cost grows as features are built in Rust that the C fork does not have.
- **Probes:**
  1. *Confident (freeze):* while the port is under way, the C kmscon and libtsm get only security fixes. A feature you want in the meantime waits until vtium has the part it belongs to, and is built there. Is that the freeze you meant? Answered yes.
  2. *Not confident (interface):* vtium reads the same `kmscon.conf` and command line as today's kmscon, at least where the tests use them, so the VM tests judge it without being rewritten. Is keeping that compatibility what you want, or should vtium be free to rename options and change defaults from the start? Answered yes, keep it.
- **Inversion:** Between "keep building the C fork" and "start a new console from scratch". The answer ports the fork's behaviour whole and keeps the C fork running beside it as the answer key, so every part of the port is checked against what it replaces.

## Context

The fork has built what it set out to build. In the operator's words, "we've largely set out what we wanted to achieve at this point: text formatting and sixel and kitty support. all the bits we're tracking now is what makes it a good product". The operator judges the architecture "a bit more stable at this point", with the caveat that "capability first is still true - the architecture may need adjustment still". The operator has set the direction: "I think the direction is clear for our fork which has a future as kmscon + libtsm as a rust fork. kmscon-rust", asked whether the port could leave the C code behind ("could we take apart the c code and essentially build a rust port of libtsm and kmscon, leaving all of it behind?"), and expects it to go quickly: "also rust port of our work is probably pretty fast with claude".

The harness already judges behaviour from outside the code. Tier 3 (ADR-301) boots the installed package in a VM and compares screenshots pixel for pixel, and `make tier3-minimal` does the same for the minimal build on ADR-103's potato machine (1366x768, 1 vCPU, 512 MB). Tier 1's `tsm-replay` prints screen state and replies as text through libtsm's public API, and its header calls that text "the conformance contract". The capability probes in kmscon's `tests/test_capabilities.c` call libtsm's API directly, so they are C programs. A port can be judged by the same tiers if those C-bound parts move behind an interface first.

The operator wants the work cut into consistent items that agents can work through in parallel, evaluated in batches: "one thing that slows us down is constant full product review/eval. so this means we should build a bunch, then run a single eval pass for things, then the next".

Upstream's maintainer wrote on 2026-10-26, in `kmscon/kmscon issue 138`, that they plan to convert libtsm to Rust on the `vte` crate and merge it into kmscon, and to convert kmscon later.

## Decision

### 1. The repository and its relation to upstream

vtium is a new repository, vtium/vtium, in the GitHub organization `vtium`, which the operator created on 2026-10-26. Its working title was kmscon-rust. It ports kmscon and libtsm to Rust whole, as one project, and leaves the fork's C code behind. It follows no upstream convention as a rule and does not couple its plans to any upstream project's process: "this is a clean break from kmscon, doesn't force a social convention, doesn't couple us to opinionated works". ADR-303 keeps governing the C fork. It does not apply to vtium, and this record does not supersede it.

The name is distinct because upstream plans to convert kmscon itself to Rust, so "kmscon in Rust" would mean upstream's. A distinct name keeps packagers and bug reporters from confusing the two, and does not suggest that vtium is upstream's sanctioned Rust kmscon. The name was free as a GitHub organization and as a crates.io crate on 2026-10-26. vtium describes itself as a Rust port derived from kmscon and libtsm by David Herrmann and contributors.

The repository is public; the operator published the workspace skeleton on 2026-10-26, and its first CI run passed. The skeleton splits the port along the C code's layers. `vtium-parse` is the escape-sequence parser and `vtium-term` the screen, cells, modes and replies, which together replace libtsm. `vtium-image` decodes sixel and kitty graphics and keeps the image store. `vtium-font` loads glyph sources, bitmap fonts first and outline fonts behind a feature. `vtium-render` draws frames with a CPU compositor, with the GPU path behind a feature. `vtium-platform` holds DRM/KMS and fbdev, VTs, seats, input, the pty and the event loop. The `vtium` binary wires them together; its default build is minspec, and `vtium-image` enters only with its `images` feature. The skeleton's COPYING carries kmscon's and libtsm's author lists and notices, and its README states the provenance (point 2).

vtium is one repository. libtsm's role is carried by the `vtium-parse`, `vtium-term` and `vtium-image` crates in the same workspace, so there is no separate library repository and no pin to bump. The operator asked for this: "the vtium repo, because rust, doesn't need the seperate lib repo, I think?" The C fork's flow of a libtsm PR followed by a kmscon PR that moves the pin ends with the port, and so does the stale and mixed libtsm build hazard (#38). The crate boundaries keep the terminal core a library with no I/O and no dependency outside the workspace: `vtium-parse` depends on nothing, and `vtium-term` and `vtium-image` only on `vtium-parse`. Bytes go in, and screen state and replies come out. The core can be published to crates.io on its own if that is ever wanted.

vtium keeps the C kmscon's interface where the tiers drive it: the `kmscon.conf` keys and command-line options the tier 3 variants use, the replies tier 1 and the probes check, and the `kmscon` terminfo entry. That is the harness's interface. A part that changes it updates the harness in the same bundle, and its record says so. The operator confirmed this compatibility (probe 2).

vtium is independent of upstream's Rust work. The fork's `upstream-bug` list and its conformance suite stay available to upstream if the operator chooses to offer them.

### 2. Licence and provenance

vtium is MIT-licensed, as kmscon and libtsm are. The port is a derivative work, so its licence file carries, from the first commit and beside the fork's own line:

- kmscon's COPYING notice, `Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>`, with the MIT permission text below it and kmscon's list of authors;
- libtsm's COPYING notices, `Copyright (c) 2017-2018 Aetf <aetf@unlimitedcodeworks.xyz>` and `Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>`, with libtsm's list of authors;
- the notice of each C file a Rust file is ported from, kept in the Rust file. The holders there include Ran Benita, Ted Kotz, the University of Tuebingen, Red Hat, Alberto Ruiz, and Mitchell Hashimoto and the Ghostty contributors for the geometry `font_box.c` adapts.

Third-party notices carry over with what they cover: unifont's OFL-1.1 notice while vtium ships unifont, which the minspec build does; the solarized palettes' MIT notice if the palettes are ported; the Unicode License v3 for tables generated from the Unicode Character Database. CCAN's htable (LGPL-2.1) and terminology's UCS-4 to UTF-8 encoder are not ported, since Rust's standard library does both jobs, and their notices do not carry over.

The README states the provenance: the maintainer's code is written by an LLM, under the operator's review, and judged by the harness. Contributions disclose how they were written (point 9). This record states it too.

### 3. Dependencies, and the parser

vtium depends on a crate only where it never needs to change it. Anything it would have to patch, it owns.

The escape-sequence parser is therefore vtium's own, ported from the fork's libtsm: the DEC state machine from vt100.net with the fork's fixes. APC strings reach a handler, for kitty graphics. DCS and OSC collection is bounded. Intermediates are passed to the dispatch and checked. Colon sub-parameters are parsed. XTGETTCAP keeps its bounds. ADR-201's mode registry is ported as the parser's table.

The `vte` crate (version 0.15) enters the SOS/PM/APC state on ESC _ and passes the string's bytes to no callback, so kitty graphics commands are dropped. Its OSC buffer is bounded at 1024 bytes only without its `std` feature, which is on by default. Using it would mean carrying patches to it in a fork of our own.

The platform C libraries stay, as FFI dependencies or through crates that wrap them: libdrm (or the `drm` crate), libxkbcommon, libudev, and later libseat, and EGL and GBM for the GPU path. Leaving C behind means leaving the fork's own C.

The font stack is the exception: it is pure Rust from the start, with no bindings to FreeType, HarfBuzz or fontconfig. The operator rejected bindings first: "bindings likely to persist for a long time and be hard to back out/clean up later". Rasterisation, shaping, colour glyphs and fallback come from Rust crates. swash, rustybuzz and fontdb are candidates to evaluate when the layer is designed; the layer's own record chooses, under the rule above. Font discovery and fallback of the kind fontconfig gives kmscon are part of that layer's design.

The event loop is calloop. It wraps file descriptors, timers and signals, as kmscon's `eloop` does.

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
- udev device discovery and the kernel's VT interface, which tier 3's VT switch case needs. These two are the drafter's addition to the operator's list, and the operator raised no objection.

It has no images, no GL, no outline fonts and no D-Bus, as the C minimal build has no images, GL, FreeType, Pango or D-Bus (ADR-103). Then the layers come in this order:

1. outline-font text, in Rust (point 3);
2. sixel, the kitty graphics protocol and the image store;
3. the GPU path: gltex ported directly, on glow over EGL and GBM;
4. libseat, multi-monitor and the rest.

The minspec build's size and memory targets are the C minimal build's measured figures on the same tier 3 variants, `2d-potato-min` with unifont and `2d-potato-min-psf` with a PSF font (ADR-103's measurement of the minimal build; README, "Minimal build"):

- resident after the first screen: at most 10.3 MiB with unifont and 5.6 MiB with the PSF font;
- resident peak, once the scrollback is full: at most 15.7 MiB and 11.0 MiB;
- installed package: at most 1.77 MiB.

Tier 3's report measures them on every run from the first Rust build.

### 6. Judged by the existing tiers

- **Tier 3** is black-box and judges the Rust kmscon unchanged. `make tier3-minimal` judges the minspec build. The default variants judge the later layers as they arrive: the 2D variants need outline-font text and images, and the GL variant needs the GPU path. Their screens drawn with outline fonts are re-baselined as tier 2's are.
- **Tier 1 and the probes, R0.** The first work item is a language-neutral replay interface, built in the C repositories: a program takes a terminal size, a byte stream, optional checkpoints and the host settings a case names (such as images off or the XTGETTCAP table), and prints the screen state and the replies as text. `tsm-replay` is that program for tier 1 today; R0 writes its input and output down as the interface. The probes in `test_capabilities.c` that feed bytes and check a reply or the screen become replay cases. The font probes, which test the C freetype backend, are rewritten for the Rust font layer, and the terminfo probes already read only `kmscon.ti`. The C implementation passes every golden through the interface before vtium relies on it. R0 is test infrastructure and is allowed under the freeze.
- **Tier 2's pixel tests** are ported with the renderer, each screen when the layer it draws arrives. A screen drawn with bitmap fonts keeps its golden. A screen drawn with outline fonts gets a new golden when the Rust font layer lands, since another rasteriser and shaper move pixels. The re-baseline is an enumerated delta: each changed golden is listed with what moved and why, reviewed on its own, and lands in its own change, apart from the code (ADR-301's rule for an intended difference).
- **Goldens.** Until vtium owns a behaviour, its goldens stay in the C repositories, vtium pins them by commit, and a golden changes in one place, reviewed against the C implementation. vtium owns a behaviour once it passes the tiers that cover it, and the goldens for that behaviour then move into vtium. Goldens for behaviour only vtium has live in vtium from the start, and so do the re-baselined outline-font goldens.

### 7. The C fork is frozen

The C kmscon and libtsm are the reference implementation and the harness's control. Until vtium passes the tiers that cover a part, the C fork takes security fixes only. It takes no new libtsm features, so the C capability batches pause. Full upstream syncs stop, since a sync moves the control; in the operator's words, "it is likely not even in the same language". A security fix upstream makes is cherry-picked into the C fork on its own. A feature for a part is built in vtium once vtium has that part.

### 8. Evaluation is batched

Work is cut into items of one form: the modules or files an item touches, the gates that judge it (named tests, tier cases and probes), what done looks like, and the items it depends on. A bundle is a set of items whose scopes do not overlap and that do not depend on one another, so parallel agents in separate worktrees can build them at once. An item merges into its bundle's branch once it builds and its own unit tests pass. The bundle then gets one evaluation pass: one review of the bundle, and the union of its items' gates run once. A failing item goes back to its builder or leaves the bundle. The bundle merges to `main` when the pass is clean. This keeps ADR-301's rule that a change to parsing, the screen or rendering lands on `main` only when the tiers agree; the change is the bundle. That reading of ADR-301 is the drafter's, and the operator raised no objection.

### 9. Contributions

vtium accepts any contribution, human- or LLM-written, and each PR says how its code was written. The maintainer's own code stays LLM-written, under the operator's review.

Every contribution passes the gates and the tiers that cover it, and its PR carries the proofs: the commands run, their results, and a test seen red before the change and green after it. In the operator's words, "any sort of contribution needs to be able to pass the evaluation gauntlet with proofs".

A contribution that adds something new extends the harness in the same PR: "if the contribution adds something new, then the eval harness needs to be proven to be extended too". The new case is shown red without the feature and green with it. A capability with no harness case that catches its absence is not accepted.

The code of conduct is the Contributor Covenant, version 2.1. vtium's CONTRIBUTING file and PR template carry this policy.

## Consequences

### Positive

- Every part of the port is checked against the C implementation it replaces, through tiers that already exist and already pass against C.
- The minspec slice exercises every layer early, and `make tier3-minimal` judges it with no harness work beyond R0.
- R0 leaves tier 1 and most probes runnable against any implementation, upstream's included.
- The parser keeps the fork's fixes, and vtium carries no patched fork of a crate.
- Bundled evaluation runs review and the slow gates once per bundle, where the C fork ran them once per change.
- The font layer carries no C bindings to remove later.
- A contribution's PR shows its own evidence, so review reads results; it does not reproduce them first.
- One repository: a change to the parser or the screen and the console code that uses it land in one PR, with no pin to move and no mixed build.

### Negative

- C fork users get no new features until vtium replaces the part, and the C fork falls behind upstream while frozen.
- Two implementations exist during the port. A security fix lands in the C fork and, where the part is ported, in vtium.
- Outline-font text loses pixel parity with the C fork. Its tier 2 and tier 3 goldens are re-baselined when the Rust font layer lands, and for those pixels the review of each delta stands in for the C control. The minspec build is unaffected: it draws only PSF and unifont bitmaps.
- A contribution that adds a capability costs a harness case as well as the code, which raises the bar for small additions.
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
- **A pure-Rust stack down to the platform.** Rejected for now: libdrm, libxkbcommon, udev, libseat, EGL and GBM are not the fork's code, and replacing them adds risk with no capability behind it. Fonts are the exception (point 3).
- **FFI to FreeType, HarfBuzz and fontconfig first, pure Rust later.** It keeps outline-font goldens matching the C fork. The draft leaned this way. Rejected by the operator: "bindings likely to persist for a long time and be hard to back out/clean up later".
- **wgpu for the GPU path.** Rejected by the operator, who chose "a direct port of gltex".

## Questions for the operator

1. **When the crate name is published.** crates.io discourages placeholder crates. The lean is to publish `vtium` with the first real code.

## Operator answers (2026-10-26)

Relayed verbatim by the team lead. The words of every item are in `considered`, the pick in item 10 as a paraphrase. Items 9 and 10 are also in `basis`, since the Decision rests on them. The Decision above is written to match.

1. **Freeze** (probe 1): "p1 yes". The C fork gets security fixes only.
2. **Interface** (probe 2): "p2 yes". vtium keeps the `kmscon.conf` keys and options the tests use.
3. **GPU path:** "q1 a direct port of gltex". It runs on glow over EGL and GBM.
4. **Font stack:** "q2 I'm not so sure about this one. bindings likely to persist for a long time and be hard to back out/clean up later". The draft's lean, FFI first, is replaced by a pure-Rust font stack from the start, and outline-font goldens are re-baselined when that layer lands.
5. **Event loop:** "q3 yes". calloop.
6. **Minspec targets:** "q4 agree". The targets are the C minimal build's measured figures.
7. **Repository:** "q5 published". vtium/vtium is public, and its first CI run passed.
8. **Upstream syncs during the freeze:** "q6 agree - it is likely not even in the same language". Full syncs stop, and a security fix is cherry-picked on its own.
9. **One repository:** "the vtium repo, because rust, doesn't need the seperate lib repo, I think?". Agreed: libtsm's role is carried by workspace crates, and there is no library repository.
10. **Contribution policy:** picked "Any contribution, disclosed", then "any sort of contribution needs to be able to pass the evaluation gauntlet with proofs" and "if the contribution adds something new, then the eval harness needs to be proven to be extended too". Point 9 of the Decision states the policy, and the README's premise becomes "the maintainer's code is LLM-written; contributions disclose".
