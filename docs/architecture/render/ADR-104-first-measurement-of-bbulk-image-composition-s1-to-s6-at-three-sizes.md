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

## A1, A2 and A4 in tier 3 (2026-10-23)

ADR-103 step 3e ran the sixel acceptance scenarios that tier 3 can run, at kmscon `1af01bd` (branch `feat/sixel-acceptance`) with libtsm `75a3ae1`, in one `make tier3 JOBS=2` run on the host above (104 of 104 cases pass); A1's pictures were then redrawn with chafa (below) and pass at the branch's head. The guest, archive 2026/10/20, gained chafa 1.18.3, libsixel 1.10.5, mpv 0.41.0 (with ffmpeg 9.0.2), tmux 3.8 and xplanet 1.3.1. Each figure comes from the case's window: the render timer's lines for kmscon's presented frames and its CPU time per frame from prepare to flip; mpv's own counts of the frames it showed and dropped (`mpv-stats.lua`); and kmscon's whole CPU time over the window from `/proc`, which includes parsing and the redraws that presented nothing. The screens are described in fork/tier3/README.md.

### A1: photographs through chafa and libsixel

`photos` draws each of two NASA Visible Earth photographs of the Earth, by day and by night (xplanet's `earth.jpg` and `night.jpg`, 2048x1024, which its images/README says are not copyrighted), twice side by side for comparison: chafa, the operator's everyday sixel tool, on the left, and libsixel's encoder, by way of mpv (ffmpeg decoding), on the right, each five rows tall; `photo-scroll` draws chafa's day photograph six rows tall. Placed, scrolled into scrollback with a screenful of lines, shown again with Shift+PageUp and returned with Shift+PageDown, they match their goldens at every variant, and the screen after them, drawn after `clear`, holds no pixel of them.

Note (2026-10-24, kmscon #145): the run above drew xplanet's photographs. The screens have since drawn one photograph, the operator's own street at sunset (2040x1536, CC BY 4.0, `fork/tier3/guest/screens/photo/`), eight rows tall on `photos` and six on `photo-scroll`, and the guest no longer installs xplanet. A1 has no timing figures, so none of this section's figures change.

img2sixel itself writes nothing on current Arch, on the host as in the guest, for any input: libsixel 1.10.5 tries gdk-pixbuf first and asks for the picture right after `gdk_pixbuf_loader_write`, without closing the loader; gdk-pixbuf 2.44 decodes through glycin and has no picture until the close, so libsixel returns success with no frame and skips its built-in loaders (src/loader.c, `load_with_gdkpixbuf`, MIT). `photos` prints how many bytes img2sixel wrote, 0 here, so a fixed libsixel shows on the page by itself (ADR-103's notes of 2026-10-23, step 3e).

### A2: mpv --vo=sixel full screen

ffmpeg's testsrc2 pattern, 640x360 at 30 frames per second for 5 seconds, scaled by mpv to the screen.

Note (2026-10-24, kmscon #145): the figures below are full screen. A2 has since played in a 1920x1080 window in the middle of the screen (1280x720 on 1280x800, 1351x760 on the potato) with `--vo-sixel-dither=none`, because full screen at 7680x2160 mpv's encoder alone sends about 4.4 frames a second (ADR-103's note of this date). The verdict, that kmscon is not the bottleneck, is the same test; the windowed run's figures are in kmscon #145.

| Variant | Window | mpv shown / dropped | mpv per second | Presented per second | Render CPU per frame, median / 95th | kmscon's CPU, share of a core | kmscon's CPU per mpv frame | Skipped redraws' share of a core |
|---|---|---|---|---|---|---|---|---|
| 2d-1280x800 (4 vCPUs) | 5.12 s | 150 / 0 | 29.3 | 59.2 | 0.70 / 1.34 ms | 19.9% | 6.8 ms | 9.4% |
| 2d-1366x768-potato (1 vCPU, 512 MB) | 5.15 s | 129 / 21 | 25.0 | 50.9 | 0.69 / 0.88 ms | 22.7% | 9.1 ms | 14.3% |
| 2d-7680x2160 (cell 10x19) | 33.21 s | 69 / 81 | 2.1 | 4.3 | 6.68 / 14.27 ms | 77.1% | 370.9 ms | 71.4% |
| 2d-7680x2160-font32 (cell 19x38) | 12.77 s | 46 / 104 | 3.6 | 7.5 | 5.84 / 10.82 ms | 48.6% | 134.8 ms | 39.5% |

Against ADR-103's criterion, that kmscon is not the bottleneck (its CPU time per frame under the frame interval, here the clip's 33.3 ms, and the frames mpv sends presented):

- **1280x800 and the potato: met.** kmscon presents more frames a second than mpv sends, and its whole CPU time per mpv frame, parsing included, is 6.8 ms and 9.1 ms, a fifth of the interval. The render timer's figure, 0.7 ms, is within ADR-103's 4 ms S4 target for the common sizes. The potato's 21 dropped frames are mpv's own: mpv encodes sixel on the same single vCPU, and kmscon used 22.7% of it.
- **7680x2160: not met.** kmscon is the bottleneck: the 5-second clip takes 33 s with the 10x19 cell and 13 s with the 19x38 one. The render timer's own frames stay within the 16.7 ms adopted for this size (6.7 ms median, 14.3 ms at the 95th percentile), and libtsm parses ten such frames in 0.11 s on the host (`tsm-replay`). The cost is redraws that present nothing: `pty_input` redraws after every bounded input call, each walks the whole frame (86 784 cells at 10x19, a quarter of that at 19x38, which is why the larger cell is three times cheaper), and a full-screen sixel arrives as hundreds of such calls; a guest `perf record` put 58% of kmscon's samples under that redraw (`screen_frame`, `cell_attr_resolve`). Filed as aaronsb/kmscon#139. Tier 3 carried both 7680x2160 variants as known bottlenecks naming it, so that the case would fail once it was fixed with the entry still there; both entries left with the fixes below.
- **7680x2160 after the fixes for #139: met.** #146 draws the child's output once per loop turn and #152 skips the frame for a display whose screen has not changed, so the redraws that presented nothing are gone. In one `make tier3 JOBS=2` run at `c91574b` (this branch merged with main at `a595627`, libtsm `ba42d1d`), with both entries removed from tier 3's `KNOWN_BOTTLENECKS`, `mpv-sixel` passed at all four variants:

| Variant | Window | mpv shown / dropped | mpv per second | Presented per second | Render CPU per frame, median / 95th | kmscon's CPU, share of a core | kmscon's CPU per mpv frame | Skipped redraws' share of a core |
|---|---|---|---|---|---|---|---|---|
| 2d-1280x800 (4 vCPUs) | 5.13 s | 150 / 0 | 29.2 | 30.0 | 0.64 / 1.09 ms | 7.8% | 2.7 ms | 0.0% |
| 2d-1366x768-potato (1 vCPU, 512 MB) | 5.18 s | 150 / 0 | 29.0 | 29.9 | 0.66 / 0.97 ms | 7.3% | 2.5 ms | 0.0% |
| 2d-7680x2160 (cell 10x19) | 6.40 s | 27 / 122 | 4.2 | 4.8 | 7.24 / 12.60 ms | 9.8% | 23.3 ms | 0.0% |
| 2d-7680x2160-font32 (cell 19x38) | 5.45 s | 25 / 124 | 4.6 | 5.1 | 5.87 / 9.26 ms | 8.6% | 18.8 ms | 0.0% |

At 7680x2160 the clip now plays in 6.4 s and 5.5 s, against 33 s and 13 s before, kmscon presents more frames a second than mpv sends, and its whole CPU time per mpv frame is under the 33.3 ms interval at both cells. mpv drops most of its frames there: it encodes each 7680x2160 frame as sixel on the guest's vCPUs while kmscon uses a tenth of a core. At the common sizes kmscon now presents once per mpv frame where it presented twice, and the potato drops nothing.

### A4: tmux grids of mpv --vo=sixel

The same pattern at 320x180 in every pane of a 2x2 and a 3x3 grid, on a tmux server whose config names kmscon's sixel support: tmux turns sixel on for a client only through `terminal-features` (`set -as terminal-features ",*:sixel"`) and draws a "SIXEL IMAGE" placeholder otherwise, so with TERM=kmscon and no such line no picture reaches kmscon. With it, every grid played to the end, and the screen tmux leaves holds no pixel of the video at any variant.

From the run at `33e5cd9`, where the tmux client runs under `script`, which copies its output to a pipe that counts the sixel pictures tmux sends to kmscon (the grids fail when there are none: with tmux drawing placeholders kmscon still presented 75 frames a second at 99.75% damage, so the render timer cannot tell):

| Variant | Grid | mpv shown / dropped, all panes | Sixel pictures tmux sent | Presented per second | Render CPU per frame, median | kmscon's CPU, share of a core |
|---|---|---|---|---|---|---|
| 2d-1280x800 | 2x2 | 600 / 0 | 601 | 61.3 | 0.93 ms | 14.9% |
| 2d-1280x800 | 3x3 | 1349 / 1 | 1419 | 61.5 | 0.89 ms | 14.6% |
| 2d-1366x768-potato | 2x2 | 313 / 287 | 328 | 55.2 | 0.93 ms | 9.7% |
| 2d-1366x768-potato | 3x3 | 555 / 793 | 583 | 42.0 | 0.98 ms | 8.0% |
| 2d-7680x2160 | 2x2 | 136 / 462 | 39 | 11.2 | 11.6 ms | 62.9% |
| 2d-7680x2160 | 3x3 | 318 / 1025 | 102 | 23.6 | 12.0 ms | 58.5% |
| 2d-7680x2160-font32 | 2x2 | 143 / 457 | 58 | 18.4 | 9.8 ms | 44.9% |
| 2d-7680x2160-font32 | 3x3 | 368 / 979 | 136 | 31.3 | 10.4 ms | 48.0% |

tmux decodes every pane's pictures and draws the grid again on kmscon, so mpv's drops count against tmux as much as kmscon, and the grids carry no rate verdict. At 1280x800 nothing is dropped. On the potato kmscon uses under a tenth of the one vCPU while the panes drop frames, so the drops are the programs sharing that vCPU. At 7680x2160 kmscon uses about half a core and renders each frame in 10 to 12 ms, under the 16.7 ms frame interval, while tmux sends it only 39 to 136 pictures in all; most frames are dropped before they reach it.

Note (2026-10-24, kmscon #145): the operator's second VT review of the KIT=sixel pages rated the grids wrong only for running under 30 frames a second. The limit is outside kmscon. With nothing reading the terminal at all (a pty sink in place of kmscon, on the operator's host), tmux and mpv bring 41-44% of the panes' frames through at 7680x2160, mpv alone reaches about 16 frames a second at the 2x2 pane's size, and at 3x3 the tmux server itself is the ceiling. mpv alone full screen at 7680x2160 sends 4.66 frames a second into a sink (`session-artifacts-2026-10-24/probe-mpv/fastsink.txt`, kept beside the checkouts; the tmux figures were taken in the steering session and not kept). The operator chose to keep A4's criterion without a frame rate: every pane animates, tmux's pictures reach the screen, nothing is left after exit, and kmscon keeps up with what tmux sends; the rate is reported (ADR-103's A4 note). The panes' mpv has run with `--vo-sixel-dither=none` since; the table above is from before that.

### What this does not measure

A3 (showcase act 5, another workstream); the operator's hardware, where the KIT=sixel review runs the same pages with `--render-timer=both`; 1920x1080, which tier 3 has no variant for (#126); and img2sixel's own output, until libsixel reads images again on Arch.
