---
contract: adr/v1
kind: evidence
capability: images
status: accepted
date: 2026-10-23
deciders:
  - aaronsb
related: []
---

# ADR-104: First measurement of bbulk image composition, S1 to S6 at three sizes

## Scope and method

ADR-103's first measurement (Order of work, step 3b): the CPU time bbulk spends per frame on the synthetic scenarios S1 to S6, at 1366x768, 1920x1080 and 7680x2160, and for information at 7680x4320, measured on 2026-10-23 at kmscon `84da010` (branch `feat/image-compose`) with libtsm `78fb688`. It is held against ADR-103's Targets section, which this record confirms or replaces.

No parser makes placements yet (sixel is step 3c, kitty 4a), so a program in tier 3's VM cannot put a picture on screen, and only S1 runs there. S1 to S6 were therefore measured through `tests/bench_compose.c`: the real bbulk renderer, freetype backend and drm2d blend drawing into an in-memory XRGB8888 display with damage clips, the path tier 2 draws, with pictures made through libtsm's test-only calls. The figure is the thread CPU time from `kmscon_text_prepare` to the end of `kmscon_text_render`, the span the render timer reports; parsing and picture creation happen between frames and are not counted, as kmscon parses before it renders.

- Host: AMD Ryzen 9 9950X3D, 32 threads, under other load (load average about 1.9). The bench ran in the dev image (`kmscon-dev`, `DEV_BASE` and `DEV_ARCHIVE` as pinned in the Makefile) pinned to host CPU 30 with `--cpuset-cpus`, standing in for the small tier 3 machine's single vCPU, which is also one host core. A first run on CPU 2 gave S1 at 1366x768 as 1.42 ms against 0.92 ms on CPU 30, from contention on that core; the figures below are all from CPU 30.
- Font: the pinned DejaVu Sans Mono at 16 px through freetype, a 9x16 cell: 151x48 cells at 1366x768, 213x67 at 1920x1080, 853x135 at 7680x2160. Tier 3's guest uses pango at 16 px, a 10x19 cell.
- Each scenario: 4 frames of warm-up, then 120 frames timed; three runs per size. The tables give the median of the three runs' medians, the largest of their 95th percentiles, the share of the screen in the frame's damage rectangles, and the blend requests per frame.
- Commands: `make bench-compose BENCH_FRAMES=120 BENCH_CPU=30`, three times; for the comparison with main, the same `tests/bench_compose.c` compiled against `main`'s (`4ab25dd`) `src/` tree and the same libtsm objects, run with `S1` only, interleaved with the branch's build five times per size.

The scenarios as `tests/bench_compose.c` builds them (ADR-103 names them in a line each; the details are this measurement's):

| | Scenario |
|---|---|
| S1 | A full screen of text scrolling one line per frame, no picture |
| S2 | A static opaque picture over the top half (top band) above a scroll region in the bottom half, one line per frame |
| S3 | A full-width opaque picture over half the screen scrolling with the text one line per frame, put back at the top every eighth frame |
| S4 | A full-screen opaque picture replaced every frame, top band |
| S5 | S4 below text, under a two-row text band scrolling in its region every frame; no other text |
| S6 | Fifty 6x3-cell opaque pictures, each replaced every frame, over a screen of text |

## Results in memory, one core

CPU time per frame, in milliseconds.

| Size | Scenario | Median | 95th percentile (largest run) | Damage | Requests per frame |
|---|---|---|---|---|---|
| 1366x768 | S1 | 0.936 | 0.957 | 99.5% | 7248 |
| 1366x768 | S2 | 0.790 | 0.880 | 99.5% | 3648 |
| 1366x768 | S3 | 0.829 | 0.898 | 99.5% | 4323 |
| 1366x768 | S4 | 0.680 | 0.829 | 99.5% | 48 |
| 1366x768 | S5 | 0.786 | 0.878 | 99.5% | 347 |
| 1366x768 | S6 | 0.203 | 0.221 | 14.2% | 150 |
| 1920x1080 | S1 | 1.863 | 1.923 | 99.1% | 14271 |
| 1920x1080 | S2 | 1.614 | 1.732 | 99.1% | 7275 |
| 1920x1080 | S3 | 1.685 | 1.799 | 99.1% | 8229 |
| 1920x1080 | S4 | 1.420 | 1.701 | 99.1% | 67 |
| 1920x1080 | S5 | 1.563 | 1.718 | 99.1% | 490 |
| 1920x1080 | S6 | 0.313 | 0.331 | 7.2% | 150 |
| 7680x2160 | S1 | 15.046 | 15.601 | 100.0% | 115155 |
| 7680x2160 | S2 | 13.463 | 14.769 | 100.0% | 58071 |
| 7680x2160 | S3 | 13.512 | 14.253 | 100.0% | 61905 |
| 7680x2160 | S4 | 11.900 | 14.370 | 100.0% | 135 |
| 7680x2160 | S5 | 12.467 | 14.219 | 100.0% | 1838 |
| 7680x2160 | S6 | 1.864 | 1.920 | 0.9% | 150 |

The damage share is below 100% at the two smaller sizes because the grid leaves a margin the renderer never draws.

S1, branch against main, the median of each run's median over five interleaved runs:

| Size | main | branch | Difference |
|---|---|---|---|
| 1366x768 | 0.920 | 0.942 | +2.4% |
| 1920x1080 | 1.845 | 1.860 | +0.8% |
| 7680x2160 | 14.807 | 15.041 | +1.6% |

## Results at 7680x4320, for information

Full 8K, which no display the operator has runs; measured for speed only, with the same method, after the three sizes above (`make bench-compose BENCH_SIZES=7680x4320 BENCH_FRAMES=120 BENCH_CPU=30`, three runs, 853x270 cells). ADR-103 sets no target for it.

| Scenario | Median | 95th percentile (largest run) | Damage | Requests per frame |
|---|---|---|---|---|
| S1 | 28.929 | 29.478 | 100.0% | 230310 |
| S2 | 26.356 | 27.366 | 100.0% | 115290 |
| S3 | 26.502 | 27.520 | 100.0% | 119124 |
| S4 | 24.322 | 25.526 | 100.0% | 270 |
| S5 | 24.707 | 25.772 | 100.0% | 1973 |
| S6 | 3.515 | 3.572 | 0.4% | 150 |

S4 and S5 take about twice their 7680x2160 time, as the pixels double: above a 60 Hz frame interval (16.7 ms), below a 30 Hz one (33.3 ms), on one core.

## Memory, computed

Computed from the sizes, not measured. One frame is width x height x 4 bytes; ADR-103's `image-storage-limit=auto` budget is four frames of the process's displays, clamped to 16 MiB to 192 MiB (`IMAGE_BUDGET_MIN` and `IMAGE_BUDGET_MAX` in `src/render/bbulk.c`, one display here). Until step 3d (#125) the budget is the scaled pictures' alone; libtsm's image stores keep their own limit until then. The scaled-picture cache's peak within a frame never passes the budget; with full-screen pictures it holds as many whole screens as fit. Between frames a display keeps at most twice its screen, or its share of the budget by size if that is less (the whole budget for one display). Each display counts once, however many sessions draw it, and a session that leaves the foreground frees its pictures. Outside the budget, and only while one picture is being made: the scaler's working memory, at most 16 MiB, and the buffer a picture is rotated in on a rotated display, at most one screen.

| Display | One frame | Four frames | Budget | Screens the budget holds | Cache peak in a frame, full-screen pictures | Cache between frames, at most |
|---|---|---|---|---|---|---|
| 1366x768 | 4.0 MiB | 16.0 MiB | 16.0 MiB | 4.0 | 4 screens, 16.0 MiB | 8.0 MiB |
| 1920x1080 | 7.9 MiB | 31.6 MiB | 31.6 MiB | 4.0 | 4 screens, 31.6 MiB | 15.8 MiB |
| 3840x2160 | 31.6 MiB | 126.6 MiB | 126.6 MiB | 4.0 | 4 screens, 126.6 MiB | 63.3 MiB |
| 7680x2160 | 63.3 MiB | 253.1 MiB | 192 MiB (cap) | 3.0 | 3 screens, 189.8 MiB | 126.6 MiB |
| 7680x4320 | 126.6 MiB | 506.3 MiB | 192 MiB (cap) | 1.5 | 1 screen, 126.6 MiB | 192 MiB (with full-screen pictures, 1 screen) |

At 7680x4320 a full-screen scaled picture draws, but a second one over it at the same time never fits: it is left out until the budget changes, and idle frames (the blink timers redraw twice a second each) do not compose it again (test_bbulk `test_budget` checks both sizes). The budget's ceiling is the one constant `IMAGE_BUDGET_MAX`.

## Results in tier 3

S1 only, from one `make tier3 JOBS=2` run at `84da010` (45 of 45 cases pass), through the installed package, drm2d and virtio-gpu, read from the render timer's log lines by the harness (`s1-scroll`, 500000 lines):

| Variant | vCPUs, memory | Lines per second | Presented per second | CPU median | CPU 95th percentile | Damage |
|---|---|---|---|---|---|---|
| 2d-1280x800 | 4, 4096 MB | 185874 | 41.3 | 0.865 | 1.100 | 99.75% |
| 2d-1366x768-potato | 1, 512 MB | 183150 | 41.4 | 0.812 | 1.250 | 98.52% |
| 2d-7680x2160 | 4, 4096 MB | 114416 | 26.8 | 10.470 | 13.250 | 99.39% |

Tier 3 has no 1920x1080 variant (#126), and S1 was not run there on main for this record; the comparison with main above is the in-memory one.

## Against ADR-103's targets

- **S1 within 5% of main at every size:** met in memory, +0.8% to +2.4%. Not measured against main in tier 3.
- **S4 at most 10 ms at 1366x768 and 16 ms at 1920x1080:** met with room to spare: about 15 and 11 times under them at the median (0.68 ms and 1.42 ms), about 12 and 9 times at the 95th percentile (0.83 ms and 1.70 ms).
- **S5 at most one and a half times S4:** met, 1.16 times at 1366x768 and 1.10 times at 1920x1080.
- **S3 at least 30 lines per second:** met. One line per frame costs 0.83 ms at 1366x768 and 1.69 ms at 1920x1080, so the renderer alone allows over 500 lines per second; on a real display the frame rate caps it, and kmscon draws many lines per frame when they arrive together, as tier 3's S1 shows.
- **A2 and A5, the presented rate against mpv's output rate:** not measured; they need the sixel parser (3c, 3e) and the kitty protocol (step 4).
- **7680x2160, kmscon not the bottleneck:** S4 and S5 take 11.9 ms and 12.5 ms per frame at the median and at most 14.4 ms at the 95th percentile, below a 60 Hz frame interval (16.7 ms) on one core. The number this measurement supports for the stress size is kmscon's CPU time per frame for S4 and S5 at or below one 60 Hz frame interval, 16.7 ms. Tier 3's S1 at this size, through virtio-gpu, is 10.5 ms at the median and presents 26.8 frames per second while 114416 lines per second arrive.

The common-size targets hold with a wide margin on this host's core; whether to tighten them is a decision, not this record's.

## Findings

- **A region scroll redraws the whole screen.** libtsm's frame marks every cell, and every placement, changed whenever anything scrolls, a region included (`con->age` in `tsm-screen.c`). S2's picture is static and only the bottom half scrolls, yet every frame damages 99.5% of the screen. bbulk's placement damage ignores libtsm's placement flag for this reason and compares each placement with the last frame's, but the cells under the picture are still flagged by libtsm, so they are redrawn as row runs: aaronsb/libtsm#34.
- **Row runs carry the picture scenarios.** S4 is one request per cell row (48 at 1366x768) and costs less than S1, which blends a glyph per cell. Before cells under a top-band picture were kept in the run whatever glyph they held, S2 at 1366x768 composed each such cell alone and took 1.77 ms; with the rule, 0.77 ms.
- **The in-memory display is not scanout memory.** The bench writes to ordinary cached memory. A real dumb buffer can be write-combined or uncached, so writes to it cost more; tier 3's S1 at 7680x2160 (10.5 ms in the VM against 15.0 ms in memory) is not comparable, because its cell is larger (10x19 against 9x16) and its frames carry many lines each.

## What this does not measure

A1 to A7, which need the parsers; 7680x4320 in tier 3, a size with no display at hand, whose memory the table above computes; S2 to S6 on the small tier 3 machine and on the operator's hardware; a 1920x1080 tier 3 variant; the write bandwidth of real scanout buffers; scaled pictures (every scenario draws at the picture's own size, so the scaler and its cache are not in these figures); and gltex, which draws no placements until step 3f.

## Acceptance (2026-10-23)

Accepted by the operator (aaronsb) in the steering session, relayed by the team lead on kmscon #127. ADR-103's note of the same date adopts the 7680x2160 number and tightens the common-size targets from these figures.
