---
contract: adr/v1
kind: decision
verb: constrain
capability: images
basis:
  - operator: aaronsb
    level: directed
    said: "I want hw accel to follow the same behavior as kmscon does, which is conservative and off. and when on, image rendering takes the same hardware available path. this way we're not changing expected behaviors"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead, answering whether the GPU renderer should become the main path for images and whether hwaccel's default should change
  - operator: aaronsb
    level: directed
    said: "what we should be doing anyway is allowing gltex as the principle path for any image drawing (kmscon already uses it I think?) and if it's not available, kmscon I assume falls back to cpu, so we have the same fallback path too"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead, the operator's framing just before the answer above; the session checked both assumptions against the code (the upstream entries below)
  - upstream: "kmscon's hwaccel setting: src/config.c defines it as a boolean, default false, documented as \"--hwaccel [off] Use 3D hardware-acceleration if available\"; src/seat.c opens a DRM device with the drm3d backend when hwaccel is set and drm2d otherwise, and retries with drm2d when drm3d cannot be created (\"trying drm2d mode\"); src/terminal.c gives a display the gltex renderer when display_has_opengl() and bbulk otherwise. Read in upstream kmscon/kmscon 7d33779 and unchanged in the fork"
  - precedent: ADR-102
  - precedent: ADR-103
  - evidence: ADR-104
  - evidence: "aaronsb/kmscon issues #68 (gltex has no RGBA path until ADR-102 step 6) and #17 (gltex upload failures under virgl), both open on 2026-10-24"
agent:
  name: claude
  model: claude-opus-5-5
status: proposed
date: 2026-10-24
deciders:
  - aaronsb
related: []
amends: [ADR-103#gltex]
considered:
  - operator: aaronsb
    said: "1. yes 2. yes, but that's an intermediate phase that only happens while we're working on it. once gltex draws, I don't think we'll encounter this state. we use the same option path that kmscon respects for hardware rendering (not a seperate switch for graphics)"
    via: Claude Code session, 2026-10-24, answering the two probes of ADR-108 as put to them in plain words
    covers: [default-stays-off, images-off-meanwhile]
  - operator: aaronsb
    said: "hwaccel off is cpu path for things, hwaccel on is gpu path for things."
    via: Claude Code session, 2026-10-24, stating the principle after answering ADR-108's probes
observable:
  - "With hwaccel at its default (off), kmscon's log reads `backend [drm2d] text renderer [bbulk]` for each display, and sixel and kitty images draw as they did before this record"
  - "With hwaccel on and a display that has OpenGL, once gltex draws placements (ADR-103 steps 3f and 4e), the log reads `backend [drm3d] text renderer [gltex]` and the same images show; until then, programs are offered no image support on that terminal (no DA1 attribute 4), under ADR-103's rule that images are offered only while every display draws placements"
  - "With hwaccel on and no OpenGL, the log reads `trying drm2d mode`, then `backend [drm2d] text renderer [bbulk]`, and images draw as with hwaccel off"
  - "Expected, to be verified: for the same image or video workload at the same display size, kmscon's CPU time per frame, as `--render-timer=log` reports it, is lower with hwaccel on (gltex, once it draws placements) than with hwaccel off (bbulk), and the presented rate is no lower. The comparison is recorded as an evidence record whichever way it comes out"
  - "Tier 3's `2d-1366x768-potato` variant (one vCPU, drm2d, bbulk) keeps drawing the image screens on every run"
---

# ADR-108: Images draw through the renderer hwaccel selects, and hwaccel stays off by default

## Summary

- **Decided:** Images are drawn by whichever renderer kmscon's existing `hwaccel` setting selects, and `hwaccel` keeps upstream kmscon's default, off.
  - **hwaccel off (the default):** drm2d and bbulk compose images on the CPU, as sixel and kitty images draw today. Nothing changes.
  - **hwaccel on, with OpenGL:** images take the same GPU path as text, through gltex.
  - **hwaccel on, without OpenGL:** kmscon falls back to drm2d and bbulk, as it does today, and images draw on the CPU.
  - **Until gltex draws placements,** ADR-103's rule that images are offered only while every display draws them means hwaccel on turns images off. The work that closes the gap is ADR-103's gltex section and its steps 3f and 4e, after ADR-102 step 6 (#68) and #17.
  - **Not decided:** turning hwaccel on by default, and choosing the renderer automatically from the hardware.
- **Trades away:** GPU composition for images on machines that could use it but run the default. A user who wants it turns hwaccel on. Until gltex draws placements, hwaccel on also costs images.
- **One-way?** No. Reversible: the setting and the renderer choice already exist, and this record adds no interface or stored data. Changing the default later is one line and a new record.
- **Probes:**
  1. *Confident (default-stays-off):* With the setting left alone, images look and perform as they do now. kmscon never turns hardware acceleration on by itself, even on a machine where the GPU would clearly be faster. You turn it on yourself when you want it.
  2. *Not confident (images-off-meanwhile):* Until the GPU renderer can draw pictures, turning hardware acceleration on switches sixel and kitty images off on that console: programs are told it has no image support. The other choice is for kmscon to drop back to CPU drawing whenever images are in use, which means kmscon choosing the renderer for you. Is losing images under hardware acceleration acceptable until the GPU path is done?
- **Inversion:** Between "images always draw on the GPU" and "images always draw on the CPU". The answer sits outside that framing: images follow the renderer the user's existing setting selects, so they never get a path of their own.

## Context

kmscon has two video paths. drm2d with the bbulk renderer draws everything on the CPU. drm3d with the gltex renderer draws text through OpenGL. Upstream's `hwaccel` setting chooses between them. It is off by default, so a default install logs `backend [drm2d] text renderer [bbulk]`. With hwaccel on, kmscon tries drm3d and falls back to drm2d when that fails. Each display then gets gltex when it has OpenGL and bbulk when it does not.

ADR-103 composes images in bbulk first and has gltex follow later, with a texture per image key and one draw per layer. Its targets list "gltex for video where GL is available" as the last remedy if CPU composition misses a target. It did not say when gltex is the path for images or how that relates to `hwaccel`.

The question came from large video. At 7680x2160, kitty-graphics video on kmscon ran slightly slower than in a desktop terminal at the same size with a smaller font. The desktop terminal composes on the GPU. bbulk decodes base64, converts `f=24` to ARGB, and scales and blits on the CPU. ADR-104 measured bbulk replacing a full-screen picture every frame (S4). A measurement of kitty video through bbulk is under way, taken while the showcase's spec-written kitty video player is built (#75). It is pending evidence, not a result, and this record does not depend on what it shows.

The CPU path is proven. On the operator's hardware, the full showcase ran end to end on drm2d and bbulk on 2026-10-24: ASCII, Braille, aalib, sixel and kitty-graphics video, and audio. Tier 3's `2d-1366x768-potato` variant (one vCPU, drm2d, bbulk) draws the image screens on every run. A machine with no usable GPU has a working path today.

## Decision

1. **Images follow `hwaccel`.** No image setting picks a renderer. The renderer that draws a display's text draws its images: bbulk on drm2d, gltex on drm3d.
2. **`hwaccel` keeps upstream's default, off.** The fork does not change it, does not add a mode that picks a renderer from the hardware, and does not switch renderers because images are in use. A later change to any of these is a new decision.
3. **The fallback is upstream's.** hwaccel on without OpenGL lands on drm2d and bbulk through upstream's existing retry, and images draw there as with hwaccel off.
4. **The CPU path is the baseline.** drm2d and bbulk are the path images are judged on, as ADR-103's acceptance scenarios already say. gltex is an optional acceleration on top of it and never a requirement. A change that breaks images on bbulk fails, whatever gltex does.
5. **The gap until gltex draws placements.** ADR-103's rule stays as it is: a terminal offers sixel and kitty graphics only while every display it draws on has a renderer that draws placements. gltex does not yet, so a terminal on gltex offers no images. Closing the gap is ADR-103's gltex section and its steps 3f and 4e, which need an RGBA path and an untinted shader from ADR-102 step 6 (#68) and image uploads that survive virgl (#17). This record adds nothing to that design.
6. **What is measured.** Once gltex draws placements, the same image and video workloads run with hwaccel off and on, with the render timer on. The expectation that gltex lowers kmscon's CPU time per frame is checked, not assumed, and the result is recorded as evidence.

## Consequences

### Positive

- Defaults and expectations stay as upstream sets them. A console that works today draws images the same way after this record.
- Machines without a usable GPU keep the path images are built and tested on.
- One setting covers text and images, so the renderer named in the log is the one drawing the pictures.
- The gltex work in ADR-103 now has a stated purpose and a stated switch, and no setting of its own.

### Negative

- Until steps 3f and 4e land, hwaccel on means no images. A user who turned hwaccel on for text loses sixel and kitty graphics, with no message beyond the terminal not offering them.
- A default install never uses the GPU for images, even where it would lower CPU load at large sizes. Getting that benefit is up to the user.
- The gltex image path lives behind a non-default setting, so fewer runs exercise it, and its tier 3 variant stays outside the default run until #17 is resolved.

### Neutral

- No new kmscon.conf setting, no change to `hwaccel`'s parsing or help text, and no code change follows from this record alone.
- The remedy list in ADR-103's targets is unchanged. CPU-side remedies (threads, SIMD) still apply to the default path, and gltex remains the remedy only where the user has turned hwaccel on.

## Alternatives Considered

- **Images always on the GPU where it exists.** This would turn hwaccel on by default, or give images their own GPU path. Rejected: it changes upstream's conservative default and what existing installs do, and the operator ruled that out ("this way we're not changing expected behaviors").
- **Choose the renderer automatically.** For example, use gltex when OpenGL is present and bbulk otherwise, regardless of `hwaccel`. Rejected for the same reason. It also makes what a console does depend on driver quality, which #17 shows varies.
- **Images always on the CPU.** This would keep gltex out of image drawing and drop back to bbulk whenever images are in use, even under hwaccel on. Rejected: drm3d has no CPU blend operation (ADR-100), so this means kmscon switching the backend for the user. It also drops the GPU relief the operator wants when they turn acceleration on.
- **A separate image-renderer setting.** Rejected: a second switch for the same choice, with combinations (GL text, CPU images) that neither backend supports today.
