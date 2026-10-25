---
contract: adr/v1
kind: evidence
capability: images
status: proposed
date: 2026-10-25
deciders:
  - aaronsb
related: []
---

# ADR-109: Images with hwaccel off and on: bbulk against gltex in tier 3 at 1280x800 and 7680x2160

## Scope and method

ADR-108's decision 6: the same image and video workloads run with `hwaccel` off (drm2d with bbulk, the CPU path) and on (drm3d with gltex, the GPU path), once gltex draws image placements (aaronsb/kmscon#188, ADR-103 steps 3f and 4e). ADR-108's observable expects kmscon's CPU time per frame to be lower with `hwaccel` on and the presented rate no lower, and asks for the comparison to be recorded whichever way it comes out. Measured on 2026-10-25 in tier 3's VMs; the operator's run on the same machine's GPU without a VM is still to come (Real hardware, pending).

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
| kp-mpv | `mpv --vo=kitty` inline over the whole area above the 12 rows: 1280x570 at 1280x800, 7680x1919 at 7680x2160. |
| ps-region | The street photograph through chafa as sixel over the top half of the screen, and numbered lines scrolling in a region under it. |
| mpv-sixel | Tier 3's own A2 screen: `mpv --vo=sixel`, ffmpeg's testsrc2 at 30 frames per second for 5 seconds, in a 1280x720 window at 1280x800 and 1920x1080 at 7680x2160. |

The player sends each frame once to one of two image ids and places it once per copy. Computed from the player's layout for these grids, each copy's box is 38x15 cells (380x285 pixels) at 1280x800 and 254x100 cells (2540x1900) at 7680x2160. The player's own report gives the frame sizes it sent: 480x360 for the scaled screens, and 380x284 and 2540x1900 for the screens at their own size. So the scaled screens shrink each frame at 1280x800 and enlarge it about five times at 7680x2160. bbulk's damage share matches these boxes: 11% at 1280x800 and 29% at 7680x2160 for one copy.

### Measurement

- The render timer logs one line per display per second: page flips, frames drawn, and kmscon's thread CPU time per drawn frame from preparing the frame to queuing its flip, as a mean, median and 95th percentile, with the damaged share of the screen.
- Each screen reports its window on tier 3's event port, with the sender's frame count and dropped frames (the player's report, or mpv through `mpv-stats.lua`) and kmscon's whole CPU time over the window from `/proc`, parsing included.
- **CPU per drawn frame** is the median of the medians of the seconds that overlap the window, the harness's `t3_bench.summarize`. **Presented** is flips over the window's length; **sent** is the sender's frames over the same length. **kmscon's CPU per frame sent** is its whole CPU time over the window divided by the frames sent.
- The 95th percentile is left out: the log carries one per second, the harness reports the largest, and one slow second sets it.
- **Two runs** of all four variants, one after the other (`run1`, `run2`), the same package. Experiment A, one run, is described below.
- Commands, from #188's worktree: `make pkg`; the installer; `make tier3-run VARIANTS="2d-1280x800 gl-1280x800 2d-7680x2160 gl-7680x2160" JOBS=1`, twice; the revert script. Figures: `python3 figures.py run1` (and `run2`), which calls `t3_bench.measure` on each variant's `serial.log` and `events.log`.
- For this record every figure was recomputed from the raw `serial.log` and `events.log` by a separate parser that does not use `t3_bench`. It matches `figures.py`'s output for both runs to the last digit printed; `figures.py` was not run on experiment A, whose figures match #188's.

The raw logs, captures, the installer and its revert, `figures.py` and both runs' figures are in `/tmp/claude-1000/gltex-images/perf/` on north (its README lists them), outside the repository; a reboot clears `/tmp`.

## Results

kmscon's CPU time per drawn frame, median, in milliseconds, run 1 / run 2. Off is drm2d with bbulk, on is drm3d with gltex.

| Workload | Off, 1280x800 | On, 1280x800 | Off, 7680x2160 | On, 7680x2160 |
|---|---|---|---|---|
| Player, PNG, 1 copy, 480x360 frames scaled | 1.56 / 1.23 | 0.66 / 0.64 | 29.77 / 30.50 | 8.60 / 8.61 |
| Player, PNG, 3 copies, 480x360 frames scaled | 1.35 / 1.41 | 0.66 / 0.70 | 35.03 / 34.88 | 8.66 / 8.89 |
| Player, PNG, 1 copy at its own size | 0.19 / 0.20 | 0.65 / 0.64 | 4.48 / 4.44 | 6.53 / 6.43 |
| Player, PNG, 3 copies at their own size | 0.36 / 0.34 | 0.65 / 0.66 | 9.80 / 9.92 | 6.40 / 6.46 |
| Player, shared memory, 3 copies at their own size | 0.37 / 0.34 | 0.65 / 0.65 | 9.71 / 9.80 | 6.45 / 6.48 |
| `mpv --vo=kitty` inline, the whole area | 0.54 / 0.50 | 0.75 / 0.73 | 5.36 / 5.34 | 6.86 / 7.42 (2 frames) |
| Photograph, text scrolling in a region under it | 0.66 / 0.78 | 0.43 / 0.42 | 14.67 / 14.86 | 8.77 / 7.66 |
| Tier 3's `mpv-sixel` | 0.57 / 0.75 | 0.69 / 0.72 | 2.57 / 2.56 | 9.29 / 9.06 |

Frames presented per second of frames sent per second, run 2. Run 1 is within 0.5 of every figure here except `mpv --vo=kitty` off at 7680x2160, 28.7 of 14.7.

| Workload | Off, 1280x800 | On, 1280x800 | Off, 7680x2160 | On, 7680x2160 |
|---|---|---|---|---|
| Player, PNG, 1 copy, scaled | 30.0 of 29.4 | 29.9 of 29.4 | 24.4 of 29.3 | 29.4 of 29.3 |
| Player, PNG, 3 copies, scaled | 30.8 of 29.7 | 30.5 of 29.7 | 24.7 of 29.6 | 30.8 of 29.7 |
| Player, PNG, 1 copy at its own size | 30.5 of 29.7 | 29.9 of 29.7 | 30.3 of 29.7 | 29.9 of 29.7 |
| Player, PNG, 3 copies at their own size | 31.0 of 29.7 | 30.2 of 29.7 | 30.8 of 29.7 | 30.5 of 29.7 |
| Player, shared memory, 3 copies at their own size | 29.8 of 29.7 | 29.8 of 29.7 | 29.8 of 29.7 | 29.7 of 29.7 |
| `mpv --vo=kitty` inline, the whole area | 58.5 of 29.7 | 58.4 of 29.6 | 30.9 of 16.0 | 0.2 of 17.6 |
| Photograph, text scrolling (presented only) | 75.2 | 75.2 | 37.7 | 37.6 |
| Tier 3's `mpv-sixel` | 29.9 of 29.3 | 30.1 of 29.5 | 28.5 of 27.9 | 27.3 of 26.7 |

mpv dropped no frames at 1280x800. At 7680x2160 it dropped 119 and 109 frames of `mpv --vo=kitty` off and 100 and 96 on, and 6 and 7 frames of `mpv-sixel` off and 13 and 12 on (run 1 and run 2). The player dropped one frame in one case (1 copy at its own size, on, 7680x2160, run 1).

kmscon's whole CPU time per frame sent, parsing included, in milliseconds, run 1 / run 2:

| Workload | Off, 1280x800 | On, 1280x800 | Off, 7680x2160 | On, 7680x2160 |
|---|---|---|---|---|
| Player, PNG, 1 copy, scaled | 2.0 / 1.8 | 1.2 / 1.1 | 25.6 / 26.4 | 9.4 / 9.4 |
| Player, PNG, 3 copies, scaled | 1.8 / 2.0 | 1.2 / 1.2 | 30.1 / 29.9 | 10.2 / 10.5 |
| Player, PNG, 1 copy at its own size | 0.5 / 0.6 | 1.1 / 1.1 | 11.2 / 11.0 | 10.4 / 7.7 |
| Player, PNG, 3 copies at their own size | 0.8 / 0.8 | 1.1 / 1.0 | 16.6 / 16.8 | 7.8 / 7.9 |
| Player, shared memory, 3 copies at their own size | 0.7 / 0.7 | 1.0 / 1.0 | 14.0 / 14.5 | 7.5 / 7.5 |
| `mpv --vo=kitty` inline, the whole area | 5.5 / 4.7 | 6.0 / 5.7 | 52.3 / 48.2 | 33.3 / 32.4 |
| Tier 3's `mpv-sixel` | 2.3 / 2.5 | 2.3 / 2.3 | 6.1 / 6.2 | 14.4 / 13.6 |

gltex reports 100% damage on every frame because it redraws every cell and every placement. bbulk's damage share, the same in both runs to the percent:

| Workload | Off, 1280x800 | Off, 7680x2160 |
|---|---|---|
| Player, 1 copy | 11% | 29% |
| Player, 3 copies | 33% | 88% |
| `mpv --vo=kitty` | 43% | 30% |
| Photograph, text scrolling | 100% | 99% |
| `mpv-sixel` | 86% | 15% |

## The 8K budget: `mpv --vo=kitty` with hwaccel on

With `hwaccel` on at 7680x2160, `mpv --vo=kitty` over the whole area presented 2 frames in 8 seconds in both runs, while mpv sent 17.1 and 17.6 frames a second and kmscon used 57% of a core. Each of mpv's frames is 7680x1919, 58,951,680 bytes (56.2 MiB) in ARGB. The default budget, `image-storage-limit=auto`, is four frames of the display (253.1 MiB at 7680x2160) capped at 192 MiB. gltex holds a texture of every image it draws, so the frame on screen is held twice, by libtsm and as a texture, 112.4 MiB together.

Experiment A ran `kp-mpv` alone on `gl-7680x2160` once more, with `image-storage-limit=1G` added to the guest's GL config:

| Run | Budget | Presented of sent | mpv dropped | CPU per drawn frame, median | kmscon's CPU per frame sent | kmscon's CPU, share of a core |
|---|---|---|---|---|---|---|
| Off, run 2 | auto (192 MiB) | 30.9 of 16.0 | 109 | 5.34 ms | 48.2 ms | 77.0% |
| On, run 1 | auto (192 MiB) | 0.2 of 17.1 | 100 | 6.86 ms | 33.3 ms | 56.8% |
| On, run 2 | auto (192 MiB) | 0.2 of 17.6 | 96 | 7.42 ms | 32.4 ms | 57.0% |
| On, experiment A | `image-storage-limit=1G` | 22.2 of 14.1 | 123 | 10.10 ms | 57.2 ms | 80.7% |

The cause is inferred from this experiment and was not traced: while gltex keeps the texture of the frame on screen (ADR-103's eviction spares what the last frame drew), the next frame's reservation does not fit, and its placement is left out until the budget grows. What else is reserved when that reservation fails, beyond the 112.4 MiB of the frame on screen, was not measured. Experiment A is one run.

## What the figures show

- **Scaled pictures are where gltex pays.** At 7680x2160 bbulk spends 29.8 to 35.0 ms a frame enlarging the player's frames and presents 24.4 to 24.7 of the 29.3 to 29.7 frames sent. gltex draws the same in 8.6 to 8.9 ms and presents every frame sent. At 1280x800, where the frames are shrunk, gltex takes about half bbulk's time (0.64 to 0.70 against 1.23 to 1.56 ms), and both keep up.
- **gltex's cost has a floor.** With a picture replaced every frame, gltex's median is 0.64 to 0.75 ms at 1280x800 and at least 6.40 ms at 7680x2160, whatever the number of copies or the transport. It redraws the whole screen each frame; bbulk draws only its damage. The photograph screen, which replaces no picture, costs gltex 0.42 to 0.43 ms at 1280x800, below that floor, and 7.66 to 8.77 ms at 7680x2160, above it.
- **Where bbulk's damage is small, bbulk is cheaper.** Pictures at their own size at 1280x800 cost bbulk 0.19 to 0.37 ms against gltex's 0.64 to 0.66. At 7680x2160 one copy at its own size (29% damage) costs bbulk 4.4 to 4.5 ms against 6.4 to 6.5; three copies (88% damage) cost bbulk 9.7 to 9.9 ms, and gltex is cheaper again at 6.4 to 6.5.
- **`mpv-sixel` at 7680x2160 is gltex's worst case here.** mpv's 1920x1080 window is 15% of the screen in bbulk's damage, and bbulk draws it in 2.6 ms. gltex redraws the whole screen and uploads each new sixel picture as a texture, 9.1 to 9.3 ms. Its whole CPU per frame sent is 13.6 to 14.4 ms against 6.1 to 6.2, mpv drops 12 to 13 frames against 6 to 7, and kmscon presents 27.2 to 27.3 frames a second against 28.5. The split of gltex's time between the redraw and the upload is not in the logs.
- **gltex's scaled pictures cost more than its unscaled ones at 7680x2160**, 8.6 to 8.9 ms against 6.4 to 6.5, though the scaled screens upload a 480x360 texture where the others upload 2540x1900. The logs do not show why.
- **The photograph with text scrolling under it** costs gltex a half to two thirds of bbulk's time at both sizes, at the same presented rate.
- **`mpv --vo=kitty` presents about two frames per frame mpv sends** at 1280x800 with either renderer, and with bbulk at 7680x2160, so its presented rate is not a count of mpv's frames. At 1280x800 gltex takes 0.73 to 0.75 ms against bbulk's 0.50 to 0.54.
- **Run-to-run spread.** Below 1.6 ms the two runs differ by up to a third (bbulk at 1280x800: 1.56 against 1.23 ms for the scaled single copy, 0.57 against 0.75 for `mpv-sixel`, 0.66 against 0.78 for the photograph). Above 2 ms they agree within 3%, except gltex's photograph at 7680x2160 (8.77 against 7.66 ms) and its two-frame stall.

## Against ADR-108's expectation

ADR-108 expects, for the same workload at the same size, lower CPU time per frame with `hwaccel` on and a presented rate no lower.

- **Holds:** scaled pictures at both sizes; three copies at their own size at 7680x2160, over PNG and shared memory; the photograph with scrolling text at both sizes.
- **Does not hold for CPU time:** pictures at their own size at 1280x800 (one or three copies); one copy at its own size at 7680x2160; `mpv --vo=kitty` at 1280x800; `mpv-sixel` at 7680x2160. In each, bbulk's damage is a fraction of the screen and gltex redraws all of it. `mpv-sixel` at 1280x800 is within the runs' spread (off 0.57 and 0.75 ms, on 0.69 and 0.72).
- **Does not hold for the presented rate:** `mpv --vo=kitty` over the whole area at 7680x2160 at the default budget, 0.2 frames a second against 28.7 and 30.9; and `mpv-sixel` at 7680x2160, 27.2 to 27.3 against 28.5, where mpv also sent fewer frames.

## Against ADR-103's targets

- **7680x2160, kmscon not the bottleneck,** judged as tier 3 judges it: kmscon's whole CPU time per frame sent within the clip's 33.3 ms frame interval, and the frames sent presented. Off, the scaled player misses it on the presented rate (24.4 to 24.7 of 29.3 to 29.7). On, it is met. `mpv --vo=kitty` over the whole area misses it with either renderer: off, kmscon's CPU per frame sent is 48.2 to 52.3 ms; on, the default budget stalls it, and at `image-storage-limit=1G` kmscon's CPU per frame sent is 57.2 ms. The other video workloads meet it both ways.
- **7680x2160, 16.7 ms per frame** (ADR-103's adopted number for S4 and S5, a full-screen picture replaced every frame): these workloads are not S4 or S5. For reference, every median with `hwaccel` on is under it (at most 10.10 ms, experiment A), and bbulk's scaled player is over it (29.8 to 35.0 ms).
- **The common sizes:** ADR-103 sets them for S1 to S6 at 1366x768 and 1920x1080 on one vCPU; none of these workloads ran there. Every median at 1280x800 is under 1.6 ms, off and on.

## What this does not show

- **Bare metal.** The GPU is the host's, reached through virgl: the guest's Mesa encodes GL commands for virtio-gpu, and the host's virglrenderer runs them inside QEMU. kmscon's thread CPU time with `hwaccel` on therefore includes the guest driver's encoding, and leaves out the host's virglrenderer work and the GPU's own time. Neither was measured, so the figures do not show whether the total CPU time falls with `hwaccel` on, only that kmscon's thread spends less.
- **The display.** "Presented" counts the guest's page flips; what reaches a monitor through `egl-headless` and virtio-gpu was not measured.
- **Other machines and sizes.** Only the 4-vCPU guest: no GL variant has one vCPU, so the potato's GL figures are unknown; 1366x768 and 1920x1080 did not run.
- **The budget mechanism** behind the 7680x2160 stall, which was inferred, and anything about budgets other than auto and 1G.
- **The texture upload's share** of gltex's time per frame.

## Figures in #188's body that the logs do not reproduce

Every figure in #188's performance table matches run 2 at the precision shown, as do its whole-CPU figures (9.4 against 26.4, 7.9 against 16.8, 13.6 against 6.2 ms) and experiment A's 22.2 frames presented of 14.1 sent. These do not:

- **Picture sizes.** #188 gives the scaled copy as 2229x1672 at 7680x2160 and about 760x570 at 1280x800, and the copy at its own size as 2236x1672. The player's reports in these logs give 2540x1900 at 7680x2160 and 380x284 at 1280x800 for the copies at their own size, and the player's layout, run with these screens' geometry, gives boxes of 2540x1900 and 380x285; bbulk's damage shares agree. The sizes in #188 are those of the earlier measurement it cites.
- **The 22.5 ms baseline** #188 cites for one 480x360 frame scaled to 2229x1672 is from that earlier measurement, which split composition from the rest of the frame. It is not in these logs and is not used here.
- **"Run 1 within a few percent"** holds above 2 ms; below it the runs differ by up to a third (What the figures show, run-to-run spread).
- **The gltex floor of about 0.65 ms at 1280x800** holds for the screens that replace a picture each frame; the photograph screen costs gltex 0.42 to 0.43 ms.

## Real hardware (pending)

The operator's run on north's RX 7900 without a VM, on a VT at 7680x2160, is to be appended here as a dated section. It runs the same pages with `hwaccel` off and on, from a checkout that holds #188 (its head, `b5ee3ba`, or main once it merges):

```sh
make vt-demo KIT=all DEMO_ARGS='--mode=7680x2160 --no-use-original-mode --render-timer=both'
make vt-demo KIT=all DEMO_ARGS='--hwaccel --mode=7680x2160 --no-use-original-mode --render-timer=both'
make showcase BAD_APPLE=/path/to/video FROM=7 DEMO_ARGS='--mode=7680x2160 --no-use-original-mode --render-timer=both'
make showcase BAD_APPLE=/path/to/video FROM=7 DEMO_ARGS='--hwaccel --mode=7680x2160 --no-use-original-mode --render-timer=both'
```

The first two run the demo's pages, the sixel acceptance pages (photographs, `mpv-sixel`, the tmux grids) among them; the last two run the showcase's act 7, the kitty player. Font options, if any are added, are the same in both runs of a pair. The figures come from the render timer's lines in each run's log, `/tmp/kmscon-demo/vt-demo-<time>-ttyN-<random>.log`, which also shows `backend [drm3d] text renderer [gltex]` when `hwaccel` took. The demo's cell differs from tier 3's 10x19, so the VT figures compare off with on, not with the tables above.

`run-vt.sh` at `b5ee3ba` refuses `--image-storage-limit`, which is not among the options it passes to kmscon, so the raised budget #188 suggests for the video pages at 7680x2160 (`DEMO_ARGS='--hwaccel --image-storage-limit=512M'`) cannot be passed this way until that list gains it. At the default budget, a page that plays `mpv --vo=kitty` over the whole screen with `hwaccel` on is expected to stall as in tier 3.
