---
contract: adr/v1
kind: evidence
capability: images
status: accepted
date: 2026-10-25
deciders:
  - aaronsb
related: []
---

# ADR-109: Images with hwaccel off and on: bbulk against gltex in tier 3 at 1280x800 and 7680x2160

## Scope and method

ADR-108's decision 6: the same image and video workloads run with `hwaccel` off (drm2d with bbulk, the CPU path) and on (drm3d with gltex, the GPU path), once gltex draws image placements (aaronsb/kmscon#188, ADR-103 steps 3f and 4e). ADR-108's observable expects kmscon's CPU time per frame to be lower with `hwaccel` on and the presented rate no lower, and asks for the comparison to be recorded whichever way it comes out. Measured on 2026-10-25 in tier 3's VMs; the operator's run on the same machine's GPU without a VM is still to come (Real hardware, pending). (Added 2026-10-25: it is in, in "Real hardware, 2026-10-25".)

> Corrected 2026-10-25: on the GL variants libtsm refused every image over 16 MiB, so the GL cells at 7680x2160 for the player at its own size and for `mpv --vo=kitty` measured frames without the picture. They are marked [superseded] where they stand, and the appended section "Correction, 2026-10-25: GL images above 16 MiB were refused" gives the defect, its fix, a third run and the revised verdicts.

- **Build.** kmscon package `10.0.4.r658.g49996fc`, built with `make pkg` from a clean tree at `49996fc` (#188's branch `feat/gltex-images`; its two later commits, `f298aa2` and `b5ee3ba`, change only ADR-103's text), with libtsm `7aae406`, the pin on main. The harness reports the kmscon revision as `49996fc+dirty` because the measuring screens below were installed into the worktree after the package was built.
- **Host** "north": AMD Ryzen 9 9950X3D; GPU PCI ID 1002:744c on `/dev/dri/renderD128`, a Navi 31 part (the Radeon RX 7900 family); Arch Linux, linux 7.2.11-arch1-1, qemu-full 11.1.2-1, virglrenderer 1.3.0-2, mesa 26.2.5-1.
- **Guest**: Arch cloud image 20261015.611680, archive 2026/10/20, base image `base-a801c98304d282ae.qcow2`. Every variant has 4 vCPUs and 4096 MB, and draws DejaVu Sans Mono 16 through pango, a 10x19 cell: 128x42 cells at 1280x800, 768x113 at 7680x2160. Both configs set `render-timer=log`; the GL config also sets `hwaccel`. The log line of every run names `backend [drm2d] text renderer [bbulk]` on the 2D variants and `backend [drm3d] text renderer [gltex]` on the GL ones.

| Variant | Path | QEMU device |
|---|---|---|
| 2d-1280x800 | off: drm2d, bbulk | `virtio-vga,xres=1280,yres=800,edid=on` |
| gl-1280x800 | on: drm3d, gltex, virgl | `virtio-vga-gl,xres=1280,yres=800,edid=on`, `-display egl-headless,rendernode=/dev/dri/renderD128` |
| 2d-7680x2160 | off | `virtio-vga,xres=7680,yres=2160,edid=on,max_hostmem=1G` |
| gl-7680x2160 | on, virgl | `virtio-vga-gl,xres=7680,yres=2160,edid=on,max_hostmem=1G`, `egl-headless` as above |

`gl-7680x2160` is temporary: no tier 3 variant draws GL at that size, and the installer below adds it to the worktree's `t3_scenario.py` as a copy of `gl-1280x800` at the larger size.

### Workloads

Temporary measuring screens, put into the guest by an installer and taken out after (not committed), modelled on the kitty player's measurement of 2026-10-24. The clip is a 20-second cut of Bad Apple!! from 0:46, 1444x1080 at 29.97 frames per second. The player and mpv screens play 8 seconds of it with 12 rows left free at the bottom.

| Screen | Workload |
|---|---|
| kp-png-1-w480, kp-png-3-w480 | The showcase's spec-written kitty player (`fork/showcase/fx.py kitty`), 1 or 3 copies held still. Frames decoded at 480x360, sent inline as PNG (`f=100`) and scaled by the terminal into each copy's box. |
| kp-png-1-w0, kp-png-3-w0 | The same with frames decoded at the box's own size, so nothing is scaled. |
| kp-shm-3-w0 | 3 copies at their own size, frames sent as `f=24` RGB in shared memory (`t=s`). |
| kp-mpv | `mpv --vo=kitty` inline, given the area above the 12 rows (1280x570 at 1280x800, 7680x1919 at 7680x2160), into which it fits the clip's shape (`mpv --vo=kitty` at 7680x2160 with hwaccel on). |
| ps-region | The street photograph through chafa as sixel over the top half of the screen, and numbered lines scrolling in a region under it. |
| mpv-sixel | Tier 3's own A2 screen: `mpv --vo=sixel`, ffmpeg's testsrc2 at 30 frames per second for 5 seconds, in a 1280x720 window at 1280x800 and 1920x1080 at 7680x2160. |

(Note 2026-10-26: the player these screens ran, `fork/showcase/fx.py kitty`, is now `fork/tools/kitty-player/kitty.py`, with the same options; the Bad Apple!! showcase moved to aaronsb/kmscon-showcase with a copy of its own, and kmscon kept the player for its acceptance runs (#239).)

The player sends each frame once to one of two image ids and places it once per copy. Computed from the player's layout for these grids, each copy's box is 38x15 cells (380x285 pixels) at 1280x800 and 254x100 cells (2540x1900) at 7680x2160. The player's own report gives the frame sizes it sent: 480x360 for the scaled screens, and 380x284 and 2540x1900 for the screens at their own size. So the scaled screens shrink each frame at 1280x800 and enlarge it about five times at 7680x2160. bbulk's damage share matches these boxes: 11% at 1280x800 and 29% at 7680x2160 for one copy.


### Measurement

- The render timer logs one line per display per second: page flips, frames drawn, and kmscon's thread CPU time per drawn frame from preparing the frame to queuing its flip, as a mean, median and 95th percentile, with the damaged share of the screen.
- Each screen reports its window on tier 3's event port, with the sender's frame count and dropped frames (the player's report, or mpv through `mpv-stats.lua`), and the CPU time of the kmscon process over the window from `/proc/<pid>/stat`, in clock ticks of 10 ms (`acceptance.inc`, `measure`).
- **CPU per drawn frame** is the median of the medians of the seconds that overlap the window and drew at least one frame (`t3_bench.summarize`). A second that drew nothing does not count.
- **Presented** is the flips of every second that overlaps the window, edge seconds counted whole, divided by the window's length; **sent** is the sender's frames over the same length. The screens pause 1.5 s before and after each window. The edge seconds weigh most in `mpv-sixel`, whose windows are about 5 s.
- **The pause does not keep gltex's flips out of the edge seconds.** With `hwaccel` on, kmscon flips during the pause: the last second before a window holds 3 to 10 flips on the GL variants (for example `run2/gl-1280x800`, the second ending at 54.66 with 7 flips, before the window starting at 55.26; `run1/gl-7680x2160`, the second ending at 107.90 with 8, before 108.56). The same seconds hold none on the 2D variants, except before the scaled three-copy screen at 7680x2160 (20 and 25), where bbulk is still drawing the screen before. A GL edge second can therefore carry roughly 5 to 10 flips that are not the workload's: up to about 1 frame a second over an 8 s window, and more over `mpv-sixel`'s 5 s windows. The bias favours `hwaccel` on: it can hide a small drop in its presented rate and cannot create one, so `mpv-sixel`'s drop at 7680x2160 is probably larger than the 1.13 to 1.31 measured.
- **kmscon's CPU per frame sent** is the kmscon process's CPU time over the window divided by the frames sent. It includes parsing and everything else kmscon does, and leaves out `kmscon-image-helper`, a separate process: the helper decodes the player's PNG frames (`f=100`) and reads its shared memory frames (`t=s`), so the player's figures leave that work out. They compare off with on, since the helper does the same work under both. The mpv screens send `f=24` inline and sixel, which kmscon decodes itself, so their figures are whole.
- The 95th percentile is left out: the log carries one per second, the harness reports the largest, and one slow second sets it.
- **Two runs** of all four variants, one after the other (`run1`, `run2`), the same package. Experiment A, one run, is described below.
- **Run-to-run spread of the presented rate:** the same variant and screen differ between the runs by at most 0.5 frames a second, except `mpv --vo=kitty` off at 7680x2160 (28.7 and 30.9), where mpv itself sent 14.7 and 16.0. A difference between two such figures, off against on, can therefore move by up to 1.0 between runs.
- Commands, from #188's worktree: `make pkg`; the installer; `make tier3-run VARIANTS="2d-1280x800 gl-1280x800 2d-7680x2160 gl-7680x2160" JOBS=1`, twice; the revert script. Figures: `python3 figures.py run1` (and `run2`), which calls `t3_bench.measure` on each variant's `serial.log` and `events.log`.
- For this record every figure was recomputed from the raw `serial.log` and `events.log` by a separate parser that does not use `t3_bench`. It matches `figures.py`'s output for both runs to the last digit printed; `figures.py` was not run on experiment A, whose figures match #188's.

The raw logs, captures, the installer and its revert, `figures.py` and both runs' figures are kept in `/home/aaron/Projects/terminal/emulators/session-artifacts-2026-10-25/gltex-images-perf/` on north, beside the checkouts and outside the repository (its README lists them). They were copied there from `/tmp/claude-1000/gltex-images/perf/`, which a reboot clears.

## Results

kmscon's CPU time per drawn frame, median, in milliseconds, run 1 / run 2. Off is drm2d with bbulk, on is drm3d with gltex.

| Workload | Off, 1280x800 | On, 1280x800 | Off, 7680x2160 | On, 7680x2160 |
|---|---|---|---|---|
| Player, PNG, 1 copy, 480x360 frames scaled | 1.56 / 1.23 | 0.66 / 0.64 | 29.77 / 30.50 | 8.60 / 8.61 |
| Player, PNG, 3 copies, 480x360 frames scaled | 1.35 / 1.41 | 0.66 / 0.70 | 35.03 / 34.88 | 8.66 / 8.89 |
| Player, PNG, 1 copy at its own size | 0.19 / 0.20 | 0.65 / 0.64 | 4.48 / 4.44 | 6.53 / 6.43 [superseded] |
| Player, PNG, 3 copies at their own size | 0.36 / 0.34 | 0.65 / 0.66 | 9.80 / 9.92 | 6.40 / 6.46 [superseded] |
| Player, shared memory, 3 copies at their own size | 0.37 / 0.34 | 0.65 / 0.65 | 9.71 / 9.80 | 6.45 / 6.48 [superseded] |
| `mpv --vo=kitty` inline | 0.54 / 0.50 | 0.75 / 0.73 | 5.36 / 5.34 | 6.86 / 7.42, two frames at the window's edges, not comparable (below) [superseded] |
| Photograph, text scrolling in a region under it | 0.66 / 0.78 | 0.43 / 0.42 | 14.67 / 14.86 | 8.77 / 7.66 |
| Tier 3's `mpv-sixel` | 0.57 / 0.75 | 0.69 / 0.72 | 2.57 / 2.56 | 9.29 / 9.06 |

Cells marked [superseded] measured gltex at 7680x2160 drawing frames without the picture, whose image libtsm had refused; run 3 replaces them (Correction, 2026-10-25).

Frames presented per second of frames sent per second, run 2. Run 1 is within 0.5 of every figure here except `mpv --vo=kitty` off at 7680x2160, 28.7 of 14.7.

| Workload | Off, 1280x800 | On, 1280x800 | Off, 7680x2160 | On, 7680x2160 |
|---|---|---|---|---|
| Player, PNG, 1 copy, scaled | 30.0 of 29.4 | 29.9 of 29.4 | 24.4 of 29.3 | 29.4 of 29.3 |
| Player, PNG, 3 copies, scaled | 30.8 of 29.7 | 30.5 of 29.7 | 24.7 of 29.6 | 30.8 of 29.7 |
| Player, PNG, 1 copy at its own size | 30.5 of 29.7 | 29.9 of 29.7 | 30.3 of 29.7 | 29.9 of 29.7 [superseded] |
| Player, PNG, 3 copies at their own size | 31.0 of 29.7 | 30.2 of 29.7 | 30.8 of 29.7 | 30.5 of 29.7 [superseded] |
| Player, shared memory, 3 copies at their own size | 29.8 of 29.7 | 29.8 of 29.7 | 29.8 of 29.7 | 29.7 of 29.7 [superseded] |
| `mpv --vo=kitty` inline | 58.5 of 29.7 | 58.4 of 29.6 | 30.9 of 16.0 | 0.2 of 17.6 [superseded] |
| Photograph, text scrolling (presented only) | 75.2 | 75.2 | 37.7 | 37.6 |
| Tier 3's `mpv-sixel` | 29.9 of 29.3 | 30.1 of 29.5 | 28.5 of 27.9 | 27.3 of 26.7 |

Cells marked [superseded] measured gltex at 7680x2160 drawing frames without the picture, whose image libtsm had refused; run 3 replaces them (Correction, 2026-10-25).

mpv dropped no frames at 1280x800. At 7680x2160 it dropped 119 and 109 frames of `mpv --vo=kitty` off and 100 and 96 on, and 6 and 7 frames of `mpv-sixel` off and 13 and 12 on (run 1 and run 2). The player dropped one frame in one case (1 copy at its own size, on, 7680x2160, run 1).

kmscon's CPU time per frame sent, in milliseconds, run 1 / run 2. For the player this leaves out the image helper's decoding (Measurement).

| Workload | Off, 1280x800 | On, 1280x800 | Off, 7680x2160 | On, 7680x2160 |
|---|---|---|---|---|
| Player, PNG, 1 copy, scaled | 2.0 / 1.8 | 1.2 / 1.1 | 25.6 / 26.4 | 9.4 / 9.4 |
| Player, PNG, 3 copies, scaled | 1.8 / 2.0 | 1.2 / 1.2 | 30.1 / 29.9 | 10.2 / 10.5 |
| Player, PNG, 1 copy at its own size | 0.5 / 0.6 | 1.1 / 1.1 | 11.2 / 11.0 | 10.4 / 7.7 [superseded] |
| Player, PNG, 3 copies at their own size | 0.8 / 0.8 | 1.1 / 1.0 | 16.6 / 16.8 | 7.8 / 7.9 [superseded] |
| Player, shared memory, 3 copies at their own size | 0.7 / 0.7 | 1.0 / 1.0 | 14.0 / 14.5 | 7.5 / 7.5 [superseded] |
| `mpv --vo=kitty` inline | 5.5 / 4.7 | 6.0 / 5.7 | 52.3 / 48.2 | 33.3 / 32.4 [superseded] |
| Tier 3's `mpv-sixel` | 2.3 / 2.5 | 2.3 / 2.3 | 6.1 / 6.2 | 14.4 / 13.6 |

Cells marked [superseded] measured gltex at 7680x2160 drawing frames without the picture, whose image libtsm had refused; run 3 replaces them (Correction, 2026-10-25).

gltex reports 100% damage on every frame because it redraws every cell and every placement. bbulk's damage share, the same in both runs to the percent:

| Workload | Off, 1280x800 | Off, 7680x2160 |
|---|---|---|
| Player, 1 copy | 11% | 29% |
| Player, 3 copies | 33% | 88% |
| `mpv --vo=kitty` | 43% | 30% |
| Photograph, text scrolling | 100% | 99% |
| `mpv-sixel` | 86% | 15% |

## `mpv --vo=kitty` at 7680x2160 with hwaccel on

mpv is given the area above the 12 rows and fits the clip's shape into it. bbulk's damage per frame of mpv's picture is 4,931,830 pixels at 7680x2160, 257x101 cells, so mpv's pictures are about 2566x1919 (the area's 1919 rows at the clip's 1444:1080), about 18.8 MiB each in ARGB; at 1280x800 the damage is 77x30 cells, about 762x570. These runs do not log the guest's mpv version.

With `hwaccel` on, no frame was drawn or presented in any render-timer second wholly inside the window, in either run. The timer logged a second with one frame that straddles the window's start, a second with none, and then no line until a second with one frame that straddles its end (`gl-7680x2160/serial.log`, lines 356 to 358 in run 1 and 355 to 357 in run 2). Meanwhile mpv sent 17.1 and 17.6 frames a second, and kmscon used 57% of a core. Those two edge frames cost 6.5 to 8.3 ms each, what gltex's full redraw costs at this size, and need not hold any of mpv's pictures. Their 6.86 and 7.42 ms medians are therefore not comparable with the 5.34 ms with `hwaccel` off or the 10.10 ms of experiment A.

Experiment A ran `kp-mpv` alone on `gl-7680x2160` once more, with `image-storage-limit=1G` added to the guest's GL config by the run's account (#188 and the perf README). No artifact of the run records the setting: the guest's log does not print it, so the setting is unverified. Its figures recompute from its logs:

| Run | Budget | Presented of sent | mpv dropped | CPU per drawn frame, median | kmscon's CPU per frame sent | kmscon's CPU, share of a core |
|---|---|---|---|---|---|---|
| Off, run 2 | auto (192 MiB) | 30.9 of 16.0 | 109 | 5.34 ms | 48.2 ms | 77.0% |
| On, run 1 | auto (192 MiB) | 0.2 of 17.1 (edge seconds only) | 100 | not comparable | 33.3 ms | 56.8% |
| On, run 2 | auto (192 MiB) | 0.2 of 17.6 (edge seconds only) | 96 | not comparable | 32.4 ms | 57.0% |
| On, experiment A | `image-storage-limit=1G`, unverified | 22.2 of 14.1 | 123 | 10.10 ms | 57.2 ms | 80.7% |

(Superseded 2026-10-25: the cause is traced; see Correction. libtsm refused every frame mpv sent, so nothing changed to draw.) The cause of the stall is not known beyond its dependence on the budget: frames were presented in experiment A and not at the default. If gltex were leaving mpv's placement out for want of budget, as #188 describes its starved path, it would still draw and present the rest of the screen as mpv's frames arrived; no frame was drawn at all. A trace is pending on #188.

## What the figures show

- **Scaled pictures are where gltex pays.** At 7680x2160 bbulk spends 29.8 to 35.0 ms a frame enlarging the player's frames and presents 24.4 to 24.7 of the 29.3 to 29.7 frames sent. gltex draws the same in 8.6 to 8.9 ms and presents every frame sent. At 1280x800, where the frames are shrunk, gltex takes about half bbulk's time (0.64 to 0.70 against 1.23 to 1.56 ms), and both keep up.
- (Superseded 2026-10-25 at 7680x2160: the 6.40 ms frames carried no picture; with it, gltex takes 9.63 to 11.10 ms. See Correction.) **gltex's cost has a floor.** With a picture replaced every frame, gltex's median is 0.64 to 0.75 ms at 1280x800 and at least 6.40 ms at 7680x2160, whatever the number of copies or the transport. It redraws the whole screen each frame; bbulk draws only its damage. The photograph screen, which replaces no picture, costs gltex 0.42 to 0.43 ms at 1280x800, below that floor, and 7.66 to 8.77 ms at 7680x2160, above it.
- (Superseded 2026-10-25 at 7680x2160: gltex takes 10.94 ms for one PNG copy at its own size and 11.10 ms for three, so bbulk is cheaper for both. See Correction.) **Where bbulk's damage is small, bbulk is cheaper.** Pictures at their own size at 1280x800 cost bbulk 0.19 to 0.37 ms against gltex's 0.64 to 0.66. At 7680x2160 one copy at its own size (29% damage) costs bbulk 4.4 to 4.5 ms against 6.4 to 6.5; three copies (88% damage) cost bbulk 9.7 to 9.9 ms, and gltex is cheaper again at 6.4 to 6.5.
- **`mpv-sixel` at 7680x2160 is gltex's worst case here.** mpv's 1920x1080 window is 15% of the screen in bbulk's damage, and bbulk draws it in 2.6 ms. gltex redraws the whole screen and uploads each new sixel picture as a texture, 9.1 to 9.3 ms. Its CPU per frame sent is 13.6 to 14.4 ms against 6.1 to 6.2, mpv drops 12 to 13 frames against 6 to 7, and kmscon presents 27.2 to 27.3 frames a second against 28.5, all of the 26.7 that mpv sent. The split of gltex's time between the redraw and the upload is not in the logs.
- (Superseded 2026-10-25: the unscaled frames carried no picture. With it, the unscaled screens cost gltex more, 9.63 to 11.10 ms. See Correction.) **gltex's scaled pictures cost more than its unscaled ones at 7680x2160**, 8.6 to 8.9 ms against 6.4 to 6.5, though the scaled screens upload a 480x360 texture where the others upload 2540x1900. The logs do not show why.
- **The photograph with text scrolling under it** costs gltex a half to two thirds of bbulk's time at both sizes, at the same presented rate.
- **`mpv --vo=kitty` presents about two frames per frame mpv sends** at 1280x800 with either renderer, and with bbulk at 7680x2160, so its presented rate is not a count of mpv's frames. At 1280x800 gltex takes 0.73 to 0.75 ms against bbulk's 0.50 to 0.54.
- **Run-to-run spread of CPU time.** Below 1.6 ms the two runs differ by up to a third (bbulk at 1280x800: 1.56 against 1.23 ms for the scaled single copy, 0.57 against 0.75 for `mpv-sixel`, 0.66 against 0.78 for the photograph). Above 2 ms they agree within 3%, except gltex's photograph at 7680x2160 (8.77 against 7.66 ms).

## Against ADR-108's expectation

ADR-108 expects, for the same workload at the same size, lower CPU time per frame with `hwaccel` on and a presented rate no lower. Here a presented rate counts as lower only when it is below the rate with `hwaccel` off by more than 1.0 frame a second in both runs. An off-against-on difference can move by up to 1.0 between runs (Measurement); the largest movement observed is 0.62 (1 copy at its own size, 1280x800: 0.05 above off in run 1, 0.57 below in run 2). A smaller drop is within the runs' spread and counts as no lower; the largest such is 0.82 (3 copies at their own size, 1280x800, run 2).

- (Superseded 2026-10-25: three copies at their own size at 7680x2160 no longer hold; see Correction for the revised verdicts.) **Holds on both counts:** scaled pictures at both sizes; three copies at their own size at 7680x2160, over PNG and shared memory; the photograph with scrolling text at both sizes.
- (Superseded 2026-10-25 for `mpv --vo=kitty`: see Correction.) **Lower presented rate, by the rule above:** `mpv --vo=kitty` at 7680x2160 at the default budget, with no frame presented inside the window against 28.7 and 30.9 with `hwaccel` off; and `mpv-sixel` at 7680x2160, 1.31 and 1.13 below off (27.2 and 27.3 against 28.5), where mpv also sent fewer frames (26.7 against 27.9 and 28.0).
- (Superseded 2026-10-25 in part, see Correction: at 7680x2160 the list now also holds three PNG copies at their own size and `mpv --vo=kitty`, three shared memory copies are not lower beyond the spread, and the one-copy entry rests on run 3. For three copies, bbulk's damage is 88% of the screen, so the reason given below does not apply to them.) **Higher CPU time:** pictures at their own size at 1280x800 (one or three copies); one copy at its own size at 7680x2160; `mpv --vo=kitty` at 1280x800; `mpv-sixel` at 7680x2160. In each, bbulk's damage is a fraction of the screen and gltex redraws all of it. `mpv-sixel` at 1280x800 is within the runs' spread (off 0.57 and 0.75 ms, on 0.69 and 0.72).
- (Explained 2026-10-25: at 1G libtsm's own budget held mpv's frames; run 3 measures the same case at the default budget after the fix. See Correction.) **Experiment A** (`image-storage-limit=1G`, unverified, one run) fails both halves against `hwaccel` off at the default budget: 10.10 ms per drawn frame against 5.34 and 5.36, and 22.2 frames a second presented against 28.7 and 30.9, with mpv sending 14.1 against 14.7 and 16.0.

## Against ADR-103's targets

- (Superseded 2026-10-25 for `mpv --vo=kitty` with `hwaccel` on: run 3 presents, and misses on CPU per frame sent, 60.0 ms. See Correction.) **7680x2160, kmscon not the bottleneck,** judged as tier 3 judges it: kmscon's CPU time per frame sent within the clip's 33.3 ms frame interval, and the frames sent presented. Off, the scaled player misses it on the presented rate (24.4 to 24.7 of 29.3 to 29.7). On, it is met. `mpv --vo=kitty` misses it with either renderer: off, kmscon's CPU per frame sent is 48.2 to 52.3 ms; on, nothing is presented at the default budget, and in experiment A (`image-storage-limit=1G`, unverified) kmscon's CPU per frame sent is 57.2 ms. The other video workloads meet it both ways, the player's figures without the image helper's work.
- (Superseded 2026-10-25: with pictures, the highest median with `hwaccel` on is 11.10 ms. See Correction.) **7680x2160, 16.7 ms per frame** (ADR-103's adopted number for S4 and S5, a full-screen picture replaced every frame): these workloads are not S4 or S5. For reference, every median with `hwaccel` on is under it (at most 10.10 ms, experiment A), and bbulk's scaled player is over it (29.8 to 35.0 ms).
- **The common sizes:** ADR-103 sets them for S1 to S6 at 1366x768 and 1920x1080 on one vCPU; none of these workloads ran there. Every median at 1280x800 is under 1.6 ms, off and on.

## What this does not show

- (Superseded 2026-10-25 in part, see Correction: for the player at its own size, PNG, and `mpv --vo=kitty` at 7680x2160, kmscon's thread spends more with `hwaccel` on, not less.) **Bare metal, with `hwaccel` on.** The GPU is the host's, reached through virgl: the guest's Mesa encodes GL commands for virtio-gpu, and the host's virglrenderer runs them inside QEMU. kmscon's thread CPU time with `hwaccel` on therefore includes the guest driver's encoding, and leaves out the host's virglrenderer work and the GPU's own time. Neither was measured, so the figures do not show whether the total CPU time falls with `hwaccel` on, only that kmscon's thread spends less.
- **Bare metal, with `hwaccel` off.** bbulk in the VM writes a virtio-vga framebuffer in the guest's RAM. A real dumb buffer can be write-combined or uncached memory (ADR-104's finding on scanout memory), so bbulk's cost on the operator's hardware is not shown either.
- (Added 2026-10-25, see Correction: for the player at its own size and `mpv --vo=kitty` at 7680x2160 in runs 1 and 2, gltex did not draw them; libtsm had refused the images.) **That gltex drew the pictures during the windows.** Every screen clears before tier 3 captures it, so no capture shows a picture from a measured window. The frames drawn, the damage and the rates are what the render timer reports; #188's own tier 3 captures of the image screens show gltex drawing pictures, outside these windows.
- **The image helper's CPU time,** which the player's figures leave out.
- **The display.** "Presented" counts the guest's page flips; what reaches a monitor through `egl-headless` and virtio-gpu was not measured.
- **Other machines and sizes.** Only the 4-vCPU guest: no GL variant has one vCPU, so the potato's GL figures are unknown; 1366x768 and 1920x1080 did not run.
- (Superseded 2026-10-25: the stall's cause is traced; see Correction.) **The cause of the 7680x2160 stall,** and anything about budgets other than auto and the unverified 1G.
- **The texture upload's share** of gltex's time per frame.

## Figures in #188's body that the logs do not reproduce

Every figure in #188's performance table matches run 2 at the precision shown, as do its whole-CPU figures (9.4 against 26.4, 7.9 against 16.8, 13.6 against 6.2 ms) and experiment A's 22.2 frames presented of 14.1 sent. These do not:

- **Picture sizes.** #188 gives the scaled copy as 2229x1672 at 7680x2160 and about 760x570 at 1280x800, and the copy at its own size as 2236x1672. The player's reports in these logs give 2540x1900 at 7680x2160 and 380x284 at 1280x800 for the copies at their own size, and the player's layout, run with these screens' geometry, gives boxes of 2540x1900 and 380x285; bbulk's damage shares agree. The sizes in #188 are those of the earlier measurement it cites.
- **mpv's frames.** #188 gives them as 7680x1919, about 59 MB each. bbulk's damage shows pictures of about 2566x1919, about 18.8 MiB in ARGB (`mpv --vo=kitty` at 7680x2160 with hwaccel on).
- **"2 frames in 8 s"** are two frames in seconds that straddle the window's edges; no frame was presented inside it.
- (Superseded 2026-10-25: traced; neither #188's account nor this record's held. See Correction.) **The stall's mechanism,** a texture held for the frame on screen leaving no room for the next frame's reservation, is not supported by the logs (same section).
- **The 22.5 ms baseline** #188 cites for one 480x360 frame scaled to 2229x1672 is from that earlier measurement, which split composition from the rest of the frame. It is not in these logs and is not used here.
- **"Run 1 within a few percent"** holds above 2 ms; below it the runs differ by up to a third (What the figures show, run-to-run spread).
- **The gltex floor of about 0.65 ms at 1280x800** holds for the screens that replace a picture each frame; the photograph screen costs gltex 0.42 to 0.43 ms.
- **"kmscon's whole CPU per frame sent (parsing included)"** leaves out the image helper for the player's screens (Measurement).

## Real hardware (pending)

(Done 2026-10-25: the runs are in "Real hardware, 2026-10-25: the RX 7900 at 7680x2160" below. They used `KIT=kitty` and `KIT=sixel` and the showcase's act 7 with sound, not the commands as written here.) The operator's run on north's RX 7900 without a VM, on a VT at 7680x2160, is to be appended here as a dated section. It runs the same pages with `hwaccel` off and on, from a checkout that holds #188 (its head, `b5ee3ba`, or main once it merges): (Superseded 2026-10-25: the checkout must hold #188 at `e0e53ffa` or later, the budget fix; at `b5ee3ba` images over 16 MiB are refused on gltex. See Correction.)

```sh
make vt-demo KIT=all DEMO_ARGS='--mode=7680x2160 --no-use-original-mode --render-timer=both'
make vt-demo KIT=all DEMO_ARGS='--hwaccel --mode=7680x2160 --no-use-original-mode --render-timer=both'
make showcase BAD_APPLE=/path/to/video FROM=7 DEMO_ARGS='--mode=7680x2160 --no-use-original-mode --render-timer=both'
make showcase BAD_APPLE=/path/to/video FROM=7 DEMO_ARGS='--hwaccel --mode=7680x2160 --no-use-original-mode --render-timer=both'
```

(Note 2026-10-26: `make showcase` is gone from kmscon (#239). Act 7's player stays in kmscon as `fork/tools/kitty-player/kitty.py`; the show's copy in aaronsb/kmscon-showcase may drift from it, so a re-run measures kmscon's. Start a `make vt-login` session with the same `DEMO_ARGS` and, once logged in, run from the checkout:

```sh
python3 -c 'import fcntl, struct, termios; r, c, x, y = struct.unpack("4H", fcntl.ioctl(0, termios.TIOCGWINSZ, bytes(8))); print(x // c, y // r)'
python3 fork/tools/kitty-player/kitty.py --video /path/to/video --start 46 --seconds 60 --fade-in 1.5 \
    --slide-at 8 --copy2-at 16 --copy3-at 24 --banner-rows 12 --cell-w W --cell-h H --report /tmp/kitty-player.txt
```

The first line prints the session's cell in pixels, W and H; the player assumes 10x20 without them. The times are act 7's cue sheet: from 0:46 to the end card at 1:46, the moves at 0:54, 1:02 and 1:10. The run leaves out the scroller and the soundtrack that runs 6 and 7 had: the 12 rows stay empty (a banner screen in kmscon is #241), and the player follows the monotonic clock.)

The first two run the demo's pages, the sixel acceptance pages (photographs, `mpv-sixel`, the tmux grids) among them; the last two run the showcase's act 7, the kitty player. Font options, if any are added, are the same in both runs of a pair. The figures come from the render timer's lines in each run's log, `/tmp/kmscon-demo/vt-demo-<time>-ttyN-<random>.log`, which also shows `backend [drm3d] text renderer [gltex]` when `hwaccel` took. The demo's cell differs from tier 3's 10x19, so the VT figures compare off with on, not with the tables above.

(Superseded 2026-10-25: #191 lets `run-vt.sh` pass `--image-storage-limit`, up to 4G, and after `e0e53ffa` the default budget holds mpv's frames at 7680x2160; see Correction.) `run-vt.sh` at `b5ee3ba` refuses `--image-storage-limit`, which is not among the options it passes to kmscon, so the raised budget #188 suggests for the video pages at 7680x2160 (`DEMO_ARGS='--hwaccel --image-storage-limit=512M'`) cannot be passed this way until that list gains it. At the default budget, a page that plays `mpv --vo=kitty` over the whole screen with `hwaccel` on may stall as in tier 3.

## Correction, 2026-10-25: GL images above 16 MiB were refused

### The defect and its fix

mod-gltex, the module gltex is built into, links the render static library, so it carried its own copy of `image_budget.c`, with hidden visibility. gltex counted its display, its textures and its cache in that private copy. kmscon's own budget, the one libtsm reserves from, saw no display on a GL variant and stayed at the 16 MiB floor (16,777,216 bytes) whatever the screen size, so libtsm refused any image that did not fit in 16 MiB by itself. bbulk is part of kmscon and counts in kmscon's budget, so the 2D variants were unaffected. Fixed in #188 by `e0e53ffa`, which exports the eleven budget calls a renderer makes so that the module's calls reach kmscon's budget. A meson test, `gltex_budget_symbols`, fails unless each call gltex makes is in the dynamic symbol table of both kmscon and mod-gltex.

Traced in tier 3 with a reservation logged in `screen_reserve` (`trace-logging.diff`), on builds of `b5ee3ba` with that logging, `kp-mpv` alone at 7680x2160 (`traceA` to `traceD` in the evidence directory):

| Trace | Variant and build | Reservations | Limit | Presented of sent |
|---|---|---|---|---|
| traceA | GL, without the fix | 142 of 19,689,452 bytes, every one ENOSPC with no image held | 16,777,216 | 0.2 of 17.0 |
| traceB | GL, with the export applied | every one fits | 201,326,592 | 24.9 of 12.6 |
| traceC | 2D | 120, every one fits | 201,326,592 | 28.2 of 14.7 |
| traceD | GL, with the export applied, `kp-png-1-w0` | 1,823, every one fits, with a 2540x1900 frame held (19.3 MB of images) | 201,326,592 | 26.2 of 27.1 |

traceA's package is `10.0.4.r660.gb5ee3ba`, `b5ee3ba` with the logging only, which has no diff of its own in the directory; traceB to traceD ran package `10.0.4.r660.gb5ee3ba4`, whose change from `b5ee3ba` is `trace-logging.diff`, the logging plus the export. The harness names every trace's tree `b5ee3ba4+dirty`, so the package and the limit reserved against tell the builds apart. `traceA/2d-7680x2160/` is an aborted run with an empty event log, absent from `traceA.log`. The traces' rates carry the cost of the logging and are not used below.

In traceB, once fixed, libtsm's copy of mpv's frames and gltex's textures of them peak at 98,447,260 and 98,444,700 bytes, 196,891,960 of the 201,326,592 budget: every frame on screen is held twice, and full-screen `mpv --vo=kitty` at 7680x2160 runs within 2% of the default budget's edge. Run 3's `kp-mpv` runs at that edge too.

### The stall, explained

At 7680x2160 with `hwaccel` on, libtsm refused every frame mpv sent, stored no new picture, and nothing changed on screen to draw. The two frames in the windows' edge seconds were the screen's own. Neither this record's earlier account (unknown beyond the budget) nor #188's (a texture held for the frame on screen) was the mechanism. Experiment A's `image-storage-limit=1G` replaced kmscon's floor with a budget that held mpv's frames, which is why it presented; its setting is still recorded in no artifact.

### Which cells the defect touched

Every GL cell, checked against the images it stores. The limit was 16 MiB at both sizes: at 1280x800, `auto` gives four frames (15.6 MiB) raised to the same 16 MiB floor, so kmscon's limit there was what it should have been, and textures, counted in the private copy, only had room of their own.

| Screen | 1280x800: image, ARGB | 7680x2160: image, ARGB | 1280x800 cells | 7680x2160 cells |
|---|---|---|---|---|
| Player, scaled | 480x360, 0.69 MB | 480x360, 0.69 MB | stand | stand |
| Player at its own size, PNG and shared memory | 380x284, 0.43 MB | 2540x1900, 19.3 MB, over 16 MiB | stand | superseded: every frame refused (traceD shows the frames held once fixed) |
| `mpv --vo=kitty` | about 762x570, 1.7 MB | 19,689,452 bytes reserved, over 16 MiB | stands | superseded: every frame refused (traceA) |
| Photograph | at most 530x399 (53x21 cells), 0.85 MB | at most 1410x1064 (141x56 cells), 6.0 MB | stands | stands |
| `mpv-sixel` | 1280x720, 3.7 MB | 1920x1080, 8.3 MB; two held at once, 16,588,800 bytes, under 16,777,216 only if nothing else is held | stands | stands, not traced; run 3 agrees with runs 1 and 2 |

The photograph's sizes are the box chafa was given; its picture is at most that. The 2D cells all stand.

### Run 3

- **Build.** kmscon package `10.0.4.r664.g398cd6c5`, built at `398cd6c5` on #188's branch, with libtsm `7aae406`. Against `49996fc`, runs 1 and 2's build, it carries #188's commits `f298aa2` (ADR-103's text and a comment in `gltex.c`) and `b5ee3ba` (ADR-103's text), and the four after `b5ee3ba`: `b6280e54`, #188's review round 1; `69c852d6`, ADR-103's text and tier 3 dropping the `skipped` status; `e0e53ffa`, the fix; and `398cd6c5`, ADR-103's text. Two of them change what kmscon does: `e0e53ffa` and `b6280e54`. That change makes gltex draw an image over half the budget at its drawn size, makes "exact" sampling mean one texel a pixel from a texel's edge, clips a scaled placement's sampling to its source rectangle in the image shader, and remembers an upload GL refused. A run 3 figure therefore differs from runs 1 and 2 by both changes.
- **What ran.** The GL variants only, `gl-1280x800` and the temporary `gl-7680x2160`, once, with the same installer, measuring screens, host and guest (`run3.log`, `run3/`). The figures with `hwaccel` off stay those of runs 1 and 2: traceC shows the 2D budget held mpv's frames, and neither change touches bbulk.
- **Figures.** Recomputed from `run3/*/serial.log` and `events.log` with the same separate parser; they match `run3-figures.md` to the last digit printed. Each log names `backend [drm3d] text renderer [gltex]`.
- **Where the defect did not reach,** run 3 agrees with runs 1 and 2. At 7680x2160: the scaled player 8.64 ms against 8.60 to 8.89, the photograph 8.57 against 7.66 to 8.77, `mpv-sixel` 9.18 against 9.06 to 9.29. At 1280x800 every cell is within 0.10 ms of runs 1 and 2, mostly above. So `b6280e54` moved little where the two can be compared.

Run 3, every GL cell: CPU per drawn frame (median), presented of sent, and kmscon's CPU per frame sent.

| Workload | On, 1280x800 | On, 7680x2160 |
|---|---|---|
| Player, PNG, 1 copy, scaled | 0.71 ms; 29.7 of 29.4; 1.3 ms | 8.64 ms; 29.5 of 29.3; 9.4 ms |
| Player, PNG, 3 copies, scaled | 0.70 ms; 30.4 of 29.7; 1.2 ms | 8.64 ms; 30.5 of 29.7; 9.8 ms |
| Player, PNG, 1 copy at its own size | 0.72 ms; 29.8 of 29.7; 1.2 ms | 10.94 ms; 29.8 of 29.5; 17.4 ms |
| Player, PNG, 3 copies at their own size | 0.75 ms; 30.7 of 29.7; 1.2 ms | 11.10 ms; 30.0 of 29.5; 17.7 ms |
| Player, shared memory, 3 copies at their own size | 0.69 ms; 29.9 of 29.7; 1.1 ms | 9.63 ms; 29.4 of 29.7; 14.0 ms |
| `mpv --vo=kitty` inline | 0.69 ms; 59.0 of 29.7; 5.1 ms | 9.82 ms; 27.1 of 13.6; 60.0 ms |
| Photograph, text scrolling (presented only) | 0.42 ms; 75.2 | 8.57 ms; 37.6 |
| Tier 3's `mpv-sixel` | 0.78 ms; 30.2 of 29.6; 2.5 ms | 9.18 ms; 27.6 of 27.0; 13.9 ms |

### The superseded cells, replaced

At 7680x2160, `hwaccel` off from runs 1 and 2 (run 1 / run 2) against on from run 3:

| Workload | CPU per drawn frame, off / on | Presented of sent, off / on | kmscon's CPU per frame sent, off / on | Sender dropped, off / on |
|---|---|---|---|---|
| Player, PNG, 1 copy at its own size | 4.48 / 4.44 against 10.94 ms | 30.6 / 30.3 of 29.7 against 29.8 of 29.5 | 11.2 / 11.0 against 17.4 ms | 0 / 0 against 1 |
| Player, PNG, 3 copies at their own size | 9.80 / 9.92 against 11.10 ms | 31.2 / 30.8 of 29.7 against 30.0 of 29.5 | 16.6 / 16.8 against 17.7 ms | 0 / 0 against 1 |
| Player, shared memory, 3 copies at their own size | 9.71 / 9.80 against 9.63 ms | 29.8 / 29.8 of 29.7 against 29.4 of 29.7 | 14.0 / 14.5 against 14.0 ms | 0 / 0 against 0 |
| `mpv --vo=kitty` inline | 5.36 / 5.34 against 9.82 ms | 28.7 of 14.7 / 30.9 of 16.0 against 27.1 of 13.6 | 52.3 / 48.2 against 60.0 ms | 119 / 109 against 128 |

The player's CPU per frame sent leaves out the image helper's decoding, as in the Measurement section. The PNG screens cost gltex more than the shared memory one (10.94 to 11.10 against 9.63 ms) while both reach kmscon through the helper; the logs do not show why.

### Revised verdicts against ADR-108's expectation

Run 3 is one run, so the presented-rate rule reads: lower when on is more than 1.0 frame a second below off in each of off's two runs. The GL flips in the pause still favour on (Measurement).

- **Player at its own size, one PNG copy, 7680x2160: higher CPU time,** 10.94 ms against 4.44 to 4.48, now by more than twice. Presented rate no lower (0.8 and 0.5 below).
- **Three PNG copies, 7680x2160: higher CPU time,** 11.10 ms against 9.80 to 9.92; this cell held before the correction and does not now. The presented rate is 1.2 below run 1 and 0.8 below run 2, so it is not lower by the rule, which the pause's flips weaken here.
- **Three shared memory copies, 7680x2160: CPU time not lower beyond the runs' spread,** 9.63 ms against 9.71 to 9.80, under 1% apart. Presented rate no lower (0.4 below). The expectation of lower CPU time is not met.
- **`mpv --vo=kitty`, 7680x2160: fails both halves.** 9.82 ms against 5.34 to 5.36; presented 27.1 against 28.7 and 30.9 (1.6 and 3.8 below), with mpv sending 13.6 frames a second against 14.7 and 16.0. kmscon's CPU per frame sent, 60.0 ms, is over the clip's 33.3 ms interval, so it misses ADR-103's "not the bottleneck" with `hwaccel` on, as it does off.
- **Unchanged:** every cell at 1280x800; at 7680x2160 the scaled player and the photograph (hold on both counts) and `mpv-sixel` (higher CPU time, lower presented rate in runs 1 and 2). Run 3's `mpv-sixel` presents 27.6 against off's 28.5, 0.9 below, inside the rule; it agrees on CPU time (9.18 ms) and does not by itself confirm the lower rate.

With the correction, the expectation holds at 7680x2160 for scaled pictures and the photograph beside scrolling text, and fails for pictures at their own size and for both mpv outputs. At 1280x800 it holds where it held before.

### Against ADR-103's targets, revised

- **7680x2160, kmscon not the bottleneck,** with `hwaccel` on: met for the player at its own size (14.0 to 17.7 ms per frame sent, the frames sent presented, the helper's work left out); missed for `mpv --vo=kitty` (60.0 ms per frame sent).
- **7680x2160, 16.7 ms per frame:** every median with `hwaccel` on is under it, at most 11.10 ms.

## Real hardware, 2026-10-25: the RX 7900 at 7680x2160

### What ran

- **Machine.** The operator's workstation, north, without a VM: AMD Radeon RX 7900 (PCI 1002:744c, radeonsi, Mesa 26.2.5). One display at 7680x2160, `DP-8` in runs 1 to 5 and `DP-7` in runs 6 and 7, as each log's `Display` line names it.
- **kmscon.** Dev builds of the local demo branch `demo/morning-2026-10-25`, staged by `make vt-demo` or `make showcase`. Each run's log names its build. Runs 1 to 3 ran `g1a319907`, which carries #188's budget fix (`e0e53ffa`) but neither of its round 3 fixes: the textures' budget of their own (`309304de`) and the glyph-atlas sampling (`7ee63c9d`, #197). Run 5 ran `gaec40b59` and runs 6 and 7 ran `g77457cc1`, which carry both. None of the three commits is on main; #188's final head is.
- **Options.** `--font-name="Monoid Nerd Font Mono" --font-size=30 --mode=7680x2160 --no-use-original-mode`, pango, a 20x40 cell, a 384x54 grid. Add `--hwaccel` for the GL runs, and `--render-timer=both`, so the on-screen figures redraw once a second and count in the timer's own lines.
- **Evidence.** In `/home/aaron/Projects/terminal/emulators/session-artifacts-2026-10-25/morning-vt/`: each run's kmscon log (`runN-*.log`, with the render timer's lines), the command's output (`runN-*.out`), and snapshots of the demo's feedback file after runs 1 to 5. Every run's output reports 0 `atomic commit failed`, 0 `vt_timeout` and 0 `still in the image budget` lines.

| Run | Path | Pages | Build | Operator's ratings |
|---|---|---|---|---|
| 1 | on, gltex | `KIT=kitty`: kitty, kitty-photo, kitty-files, kitty-layers | `g1a319907` | 4 of 4 right |
| 2 | on, gltex | `KIT=sixel`: sixel, ramps, article, photos, sixel-evict, photo-scroll, mpv-sixel, tmux-2x2, tmux-3x3 | `g1a319907` | 8 of 9 right; sixel-evict wrong |
| 3 | off, bbulk | `KIT=sixel` | `g1a319907` | 9 of 9 right |
| 5 | on, gltex | `KIT=sixel` | `gaec40b59` | 9 of 9 right |
| 6 | on, gltex | the showcase's act 7 (`FROM=7`, with sound) | `g77457cc1` | not rated in the feedback file; the operator's words below |
| 7 | off, bbulk | the showcase's act 7 | `g77457cc1` | not rated in the feedback file; the operator's words below |

Run 4 ran the text kit on bbulk for another workstream (ADR-201) and is not used here.

### How the figures are windowed

The kmscon log carries no timestamps. The render timer writes one line per display per second, in order, so a window is a run of consecutive lines, chosen by its signature and the page order. In each window, CPU time is the median of the seconds' medians (every second in these windows drew frames). The 95th percentile is the 95th percentile of those medians, alongside the largest second's own 95th percentile and its maximum. The presented rate is the median of the seconds' rates. The parser, `vt.py`, is kept with `recompute.py` in `/home/aaron/Projects/terminal/emulators/session-artifacts-2026-10-25/evidence-109-scripts/` (`python3 -I vt.py <log> [first end]`, where `end` is exclusive, as in `vt.py run6-act7-hwaccel.log 14 73`) and is separate from the harness's.

- **Act 7.** The act's seconds run from the first line above 20 presents a second after the start screen to the last before the end card's steady 44 a second. The windows below are the 59 seconds wholly inside: lines 14 to 72 of run 6 and 4 to 62 of run 7, counted from 0. Each frame is 2246x1680 (15.1 MB in ARGB), placed in a 112x42-cell box of 2240x1680, both computed from the player's layout for this grid. Both renderers therefore draw it slightly scaled. Three copies show from the cue at 1:10 to the end card at 1:46, the act's last 36 seconds. In run 7 the damage shows it: 87.5% to 90.8% of the screen on lines 28 to 62, three boxes (68%) and the scroller's rows (22%), against 43% to 46% with one copy on lines 4 to 18. Between them, lines 19 to 27 carry 55% to 76% damage as the second copy slides out from the cue at 1:02. For run 6 the same 35 lines from the end, 38 to 72, stand for the three-copy phase.
- **mpv-sixel.** Five consecutive lines at about 34 presents a second, after the photo pages and before the tmux grids. In run 3 they carry 11.9% to 17.4% damage, mpv's 1920x1080 window being 12.5% of the screen. Those are lines 48 to 52 of run 3, 85 to 89 of run 2, and 36 to 40 of run 5. The fifth line is partial in runs 2 (28.97) and 5 (24.94), as the clip ends.
- **Pages at rest.** Every line with at most 5 presents a second, the cursor's blink and the on-screen figures, after the first line. In run 3, 12 of these 35 lines are page changes, with 33% to 100% damage, 5 of them at medians of 8.30 to 8.64 ms. The other 23 carry under 1% damage at a median of 0.73 ms (at most 0.82), the same as all 35.
- **The tmux grids** are not separated: the logs hold stretches that fit them, but not cleanly enough to attribute.

### Figures

| Workload | Off: bbulk | On: gltex |
|---|---|---|
| Act 7, all 59 s: CPU per drawn frame | 38.37 ms (95th of medians 38.58; largest second's 95th 43.00; max 43.86) | 2.37 ms (95th of medians 2.73; largest second's 95th 3.57; max 4.69) |
| Act 7, all 59 s: presented per second | 29.9 (24.0 to 40.0) | 71.0 (68.8 to 75.0) |
| Act 7, three copies, 35 s: CPU per drawn frame | 38.51 ms | 2.31 ms |
| Act 7, three copies, 35 s: presented per second | 29.8 (27.9 to 31.5) | 70.9 (68.8 to 74.8) |
| mpv-sixel, 5 s: CPU per drawn frame | 1.68 ms (run 3) | 2.17 ms (run 2), 2.19 ms (run 5) |
| mpv-sixel, 5 s: presented per second | 34.0 (run 3) | 34.0 (run 2), 34.0 (run 5) |
| Pages at rest: CPU per drawn frame | 0.73 ms (run 3, 35 s; damage 0.05%) | 2.38 ms (run 1, 47 s), 2.44 ms (run 2, 38 s), 2.39 ms (run 5, 15 s) |

- **Act 7's presented rates are not frame counts of the video.** The act plays one 29.97 frame-a-second video, each frame sent once and placed up to three times, under a scroller that draws 40 frames a second in a process of its own (`BANNER_FPS`). Together that is about 70 updates a second (29.97 and 40), computed from the two senders' settings. The copies' slides also move them (`ACT7_MOVE`, `ACT7_BOUNCE`), and the on-screen figures redraw once a second, while updates that coincide fold into one frame. gltex's 69 to 75 a second is consistent with drawing every update but does not establish it. bbulk presents about 30 a second. Its frame cost is bimodal: with one copy its seconds' medians run from 5.3 to 35.4 ms, and with three copies its mean is 28.9 ms against a median of 38.5. The 59 s median of 38.37 ms is essentially the three-copy phase's.
- **Busy share of kmscon's thread.** Mean CPU per frame times frames per second gives bbulk about 846 ms of each second over the act (85%), and 856 ms with three copies (86%). It draws less often than the updates arrive and folds several into one frame. gltex spends about 171 ms of each second (2.39 ms times 71.6 frames, 17%). Per second, gltex's thread does about a fifth of bbulk's work; per drawn frame, about a sixteenth. The player's own count of frames shown and dropped was not kept (`ACT7_REPORT` was unset), so whether bbulk kept every video frame is not known.
- **mpv-sixel** plays a 30 frame-a-second clip, and both renderers present 34 a second. mpv's own counts were not kept in these runs, so what the extra presents carry, and whether either path dropped frames, is not shown.
- **At rest,** bbulk redraws only the cursor's cell and the figures (0.05% of the screen). gltex redraws all of it every frame.

### The operator's judgement

From the feedback file, verbatim:

- Run 2, 2026-10-25 08:49, sixel-evict, wrong: "pictures 1-5 do not appear: they just have the empty space for the picture. 6- through the remaining show the various colored strips". This was the texture budget shared with libtsm's images, fixed in #188 by `309304de` before run 5, which rated the page right.
- Run 2, 08:50, tmux-3x3, right: "right visually, but performance is degraded a bit (less frames, choppier)".
- Run 3 (bbulk), 08:54, tmux-2x2, right: "framerate was not as fast as single video and frame consistency had some 'burstiness' to consistency (frame replay timing had bursting jitter)"; tmux-3x3, right: "same bursting jitter as last, with an even lower overall framerate".

From #197, filed from the operator's VT review on 2026-10-25 and fixed in `7ee63c9d` before run 5:

- On run 1 (kitty kit, gltex, `g1a319907`), whose pages were rated right, quoted verbatim in the issue's body: "the text itself looked slightly poorer than before, as if it was just a hair too thin and even a few characters had some single pixel artifacts on the edge of the cell (just a few, out of all the characters)".
- On run 2 (sixel kit, the same build), relayed verbatim by the team lead, written by the operator after the run on 2026-10-25 and spelled as typed: "that test had the same visual artificats [sic] on occasional character cells. the inverse video lowercase 'q' had it for example". The issue's follow-up comment records the same example.

Relayed by the team lead from the operator's session on 2026-10-25, verbatim. These are not in the feedback file:

- On run 5: "the text was crisp and accurate just like cpu rendering". #197's closing comment quotes the same words.
- On run 6: "with three video streams on screen it renders at a constant 72 fps". The act sends one video placed as three copies, and the 72 counts the scroller's updates too (above).
- On run 7, act 7 with bbulk: "roughly 28-30 fps" and "still, pretty good for cpu rendering". The render timer's median over the three-copy phase is 29.8 presented a second (27.9 to 31.5).

### Against ADR-108's expectation, on the real hardware

- **Act 7, kitty video at 7680x2160 drawn slightly scaled, up to three copies, under a scroller: holds on both counts.** gltex takes 2.37 ms per drawn frame against bbulk's 38.37, about a sixteenth, and presents 71.0 a second against 29.9. Per second it keeps kmscon's thread busy about 17% of the time against bbulk's 85%, about a fifth. It agrees in direction with the VM's scaled player (8.6 against 30 to 35 ms). The workload is the showcase's, not tier 3's player, so the two are not the same cell.
- **mpv-sixel at 7680x2160: higher CPU time with gltex,** 2.17 to 2.19 ms against 1.68. The presented rate is the same, 34.0. This agrees with the VM's verdict on CPU (9.1 to 9.3 against 2.6 ms), and on the real hardware the gap is about half a millisecond. The VM's lower presented rate is not seen here, and the GL edge-second bias does not apply to these medians of whole seconds.
- **Pages at rest:** gltex's full redraw costs 2.38 to 2.44 ms a frame against bbulk's 0.73 for the cursor alone. This is a few frames a second, not a workload ADR-108 names. It is not gltex's floor: its mpv-sixel frames cost less, 2.17 to 2.19 ms.
- **VM-only:**
  - every cell at 1280x800;
  - at 7680x2160, the tier 3 player at its own size (PNG and shared memory, one and three copies) and scaled. Act 7 shrinks its frames by 0.3% (2246 to 2240 pixels wide), where tier 3's scaled player enlarges them about five times;
  - `mpv --vo=kitty`;
  - the photograph beside scrolling text;
  - the tmux grids, which the logs do not separate.
- **The kitty pages** (run 1) ran on gltex only. There is no off-against-on comparison for them.

### Limits

- **One run of each,** with no repeat to measure the spread. The two act 7 runs share a build and the mpv-sixel comparison pairs runs 2 and 3 on one build, while runs 1, 2 and 5, and runs 3 and 5, differ in build.
- **The render timer measures kmscon's thread only:** its CPU time from preparing a frame to queuing its flip. With gltex that includes radeonsi's work on that thread. It leaves out the GPU's own time, any driver threads, and the kernel's, and nothing else was measured. The figures show that kmscon's thread spends less with `hwaccel` on, not that the machine does.
- **Windows are chosen by signature in an untimed log,** as above. The act 7 phases rest on the cue sheet's times.
- **Act 7 is the showcase's choreography,** with moving copies, a 40 frame-a-second scroller and sound. It is not tier 3's player.
- **The operator's ratings judge what is seen;** they are not measurements. The only judgement of act 7 is the operator's words above on runs 6 and 7; neither run was rated in the feedback file.

## Acceptance (2026-10-25)

Accepted on 2026-10-25 at the team lead's direction, once the operator's real-hardware runs above were in, as the record's Scope set out. The correction of the same date stands as part of the record.
