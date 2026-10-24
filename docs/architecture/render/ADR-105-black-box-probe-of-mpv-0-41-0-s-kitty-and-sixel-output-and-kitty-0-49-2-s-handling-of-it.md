---
contract: adr/v1
kind: evidence
capability: images
status: accepted
date: 2026-10-24
deciders:
  - aaronsb
related: []
---

# ADR-105: Black-box probe of mpv 0.41.0's kitty and sixel output, and kitty 0.49.2's handling of it

## Scope and method

What mpv sends for `--vo=kitty` and `--vo=sixel`, and what kitty does with it, recorded on 2026-10-24 for ADR-103's step 4. Both programs were run as black boxes; kitty's source was not opened (ADR-300). The spec cited is the kitty graphics protocol page, https://sw.kovidgoyal.net/kitty/graphics-protocol/, as read that day.

- **mpv 0.41.0** played `av://lavfi:testsrc2=size=640x360:rate=30:duration=5` with `--no-config --really-quiet --no-input-terminal --ao=null --no-audio`, the flags of the step 3e acceptance runs, under `--vo=sixel`, `--vo=kitty`, and `--vo=kitty --vo-kitty-use-shm=yes`.
- **The harness** gave mpv a pty sized with `TIOCSWINSZ` to 192x56 cells at 1920x1080 pixels and to 404x56 cells at 7680x2160, parsed every APC and DCS it wrote, and answered DA1, DSR and XTWINOPS had mpv asked; it asked nothing. A second sink only counted bytes, for the throughput ceiling. A variant unlinked each `t=s` object after reading it, as a spec-following terminal does. strace recorded mpv's calls on `/dev/shm`.
- **kitty 0.49.2** ran the same mpv full screen (3460x1800 pixels, 346x90 cells), directly and through a relay that rewrote each `t=s` name to begin with `/`, for 5 and 30 seconds. inotify watched `/dev/shm`, and kitty's main-process RSS was sampled every half second. GPU memory was not measured.
- The probe's files were kept in the session's scratch space and are not in the repository; the figures below are what they hold.

## What mpv sends under `--vo=kitty`

- **Keys.** Every frame is one transmission: `a=T,f=24,s=<w>,v=<h>,C=1,q=2`, with `t` absent (direct) or `t=s`. mpv sends no `i`, `I` or `p`, so every image is anonymous and no reply is due; it never sends `o=z`, `f=100` or `f=32`, never queries, and never reads a reply. A bare `a=d` comes at start and at exit, and no delete between frames.
- **Position.** Each frame is preceded by `CSI 0;0 f` and placed at the same cell, the top left of the picture area, at the same size. Frames pile up at one cell with nothing removing the older ones.
- **Size.** mpv scales to the pixel size the pty reports, keeping the aspect: 1920x1080 at 1920x1080, and 3840x2160 at 7680x2160.
- **Direct chunking.** The base64 payload is cut into 4096-byte chunks, `m=1` on all but the last and `m=0` on the last, with the full keys on the first chunk only, as the spec's remote-client section says.
- **Shared memory chunking.** Each `t=s` frame is one APC carrying the whole command, and it has `m=1`. No `m=0` follows, ever. A parser that holds a `t=s` command open until `m=0` takes every later frame as a continuation and shows nothing.
- **The shared memory name** is `mpv-kitty-0x<hex>`, with no leading `/`, base64-encoded in the payload. The spec's medium table says POSIX names "must start with a / and have no other / characters".
- **One name for the whole run.** strace shows, for every frame: `openat("/dev/shm/<name>", O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600)`, `ftruncate` to `s` x `v` x 3, `mmap`, about 32 ms of rendering, `munmap`, `close`, then the APC; the next frame's `openat` comes about 1 ms after the APC. mpv unlinks the name only before its first frame (stale cleanup) and at exit.
- **The unlink race.** With the harness unlinking each object after reading it, as the spec asks ("must read the data from the memory object and then unlink and close it"), 75 of 150 frames found no object: mpv had reopened the name before the unlink, rendered into the unlinked object, and named it in its next APC. Without the unlink, a terminal that reads after mpv's next `openat` reads the object mpv is rendering into; whether frames tear was not measured.

## Throughput on the pty

Frames presented by the counting sink, over the clip's 5 seconds at 30 frames per second.

| Mode | Size | Bytes per frame on the pty | Frames per second |
|---|---|---|---|
| kitty, direct | 1920x1080 | 8.3 MB | 15.9 |
| kitty, direct | 7680x2160 | 33.2 MB | 4.1 |
| kitty, shared memory | 1920x1080 | 82 bytes | 30.0 |
| kitty, shared memory | 7680x2160 | 84 bytes | 30.0 |
| sixel | 1920x1080 | 0.32 MB | 18.3 |
| sixel | 7680x2160 | 1.0 MB | 4.7 |

- Direct kitty output is bound by the pty, at about 135 MB/s through it. Sixel is bound by mpv's encoder (libsixel), at under 6 MB/s. At 7680x2160 the two give about the same rate.
- Shared memory carries the full rate at both sizes. It is the only mode in which mpv's kitty output plays at its source rate on a large display.

## What kitty 0.49.2 does with it

- **mpv's shared memory as sent shows nothing.** kitty refuses every frame with "POSIX SHM names must start with /", as the team lead read it from kitty's log; that run's log is not among the kept files, and its inotify log records no access to `/dev/shm` at all. Shared memory mode as mpv 0.41.0 ships it works with no terminal that enforces the spec's name rule.
- **With the name rewritten to `/<name>`,** kitty shows the video and unlinks each object after reading it, and loses frames to mpv's reuse of the name: its log reports "Failed to open file for graphics transmission ... No such file or directory" for 44 of 150 frames in a 5-second run and 443 of 900 in a 30-second run, 29% to 49% from run to run.
- **`m=1` on a whole `t=s` command:** kitty takes each such APC as a complete command; frames display without any `m=0`.
- **Piled-up anonymous frames:** kitty's main-process RSS rises to about 150 MB within the first half second and stays there for 30 seconds of frames placed at one cell, direct or through shared memory, so kitty bounds them by some means not visible from outside. The spec's storage quota section says that when a new image passes the quota, "the terminal emulator should delete older images to make space for the new one", and its delete section that "existing images without placements will be preferentially deleted".
