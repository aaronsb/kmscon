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

# ADR-106: Black-box survey of five more kitty graphics clients: timg, ncplayer, kitten icat, chafa and viu

## Scope and method

What five more kitty graphics clients send, recorded on 2026-10-24 beside ADR-105's probe of mpv, so ADR-103's rules for step 4 rest on more than one client. Every client and kitty were run as black boxes; neither kitty's nor any client's source was read (ADR-300). These are observations of clients, for compatibility: ADR-103's rules rest on the spec, and these records say which clients a rule affects (ADR-103, Notes of 2026-10-24).

- **Clients:** timg 1.6.3 (`timg -pk`), ncplayer 3.0.17 from notcurses (`ncplayer -b pixel -q -t0`), kitten icat from kitty 0.49.2 (default, and `--transfer-mode=file`, `memory` and `stream`), chafa 1.18.3 (`chafa -f kitty`), and viu 1.6.1. Each showed a still PNG (640x360) and, where it plays them, a 20-frame GIF (320x180, 10 frames per second, no transparent colour) or the 640x360 30 fps clip ADR-105 used.
- **The harness** acted as a kitty-like terminal on a pty sized to 192x56 cells at 1920x1080 pixels and 404x56 cells at 7680x2160. It answered DA1, DA2, DSR, XTVERSION, XTWINOPS and kitty queries (`a=q`) as kitty does, and read `t=f`, `t=t` and `t=s` media, unlinking `t=t` and `t=s` objects after reading them; runs with no replies, and with no unlinking, varied each. It decodes nothing, so its frame rates are bounds set by the client and the pty.
- **kitty 0.49.2** ran each client full screen (3460x1800 pixels, 346x90 cells) with inotify on `/dev/shm` and `/tmp` and kitty's main-process RSS sampled every half second.
- The probe's files were kept in the session's scratch space and are not in the repository; the figures below are what they hold.

## What each client sends

| Client | Media and format | Ids | Per frame | Deletes |
|---|---|---|---|---|
| timg, video | direct, `f=100` PNG, chunked | two ids, alternating | `a=T,i=<id>,q=2,f=100`, about 92 KB of base64 at both sizes; the new transmission of an id replaces its image | none |
| ncplayer, video | direct, `f=32,o=z`, chunked | a new id per frame, with `p=1` | `a=t,f=32,o=z,s,v,i,p=1,q=2`, then `a=p,i,p=1,q=2,C=1`, then `a=d,d=I` on the previous frame's id | the previous frame, each frame |
| icat, still | `t=f` with `f=100` naming the file itself (default); `t=s` with `f=100` and `S` (memory); direct `f=100` (stream) | none | one `a=T,q=2` | none |
| icat, GIF | `t=s` with `S`: the first frame `f=24`; the `a=f` frames carry no `f`, so the default `f=32` applies (`S=186880` is 320x146x4) | `I=<n>` | `a=T` for the first frame, then `a=a` (animation control, carrying `I`, `v`, `r` and `z`, two of them also `s`, and no `q`) and `a=f` frames (`t=s`, `s`, `v`, `S`, `c`, `y`, `z`, `q=2`) | none |
| chafa, GIF | direct `f=32`, chunked, with `c` and `r` | none | `a=T,f=32,s,v,c,r,q=2`, about 361 KB of base64 | none |
| viu, GIF | `t=t`, `f=32`, with `c` and `r` | none | `a=T,f=32,s,v,c,r,t=t`, no `q`, then `ESC [ 5 n`, waiting for `ESC [ 0 n` before the next frame | none |

- **Piling up.** chafa and viu place each GIF frame as a new anonymous image (no `i` or `I`) at the same cell with no delete, as mpv does with video (ADR-105). timg replaces by id and ncplayer deletes the previous frame, so neither piles up. At 320x180 a frame holds 230,400 bytes, so a 16 MiB budget holds about 70 such frames.
- **Shared memory and temporary names.** icat names its objects `/icat-<13 characters>`, with the spec's leading `/`, and its `t=t` files `/dev/shm/kitty-tty-graphics-protocol-<n>`. viu's `t=t` files are `/tmp/.tty-graphics-protocol.viuer.<c>`. Both names hold `tty-graphics-protocol` and lie in a temporary directory, as ADR-103's `t=t` rule requires. No client but mpv sent a shared memory name without a leading `/`.
- **Who removes the objects.** The spec gives the removal of `t=t` files and `t=s` objects to the terminal. The harness removes an object only after reading it, and in two icat runs it read none: the detection run, with unlinking off, and the GIF run at 7680x2160, with unlinking on but no medium read. In both, icat's query objects (the `a=q,t=t` file and the `a=q,t=s` object) were gone at the end, so icat, the only other process in each run, removed them itself. In the GIF run, its 20 frame objects (the first frame's `a=T` and 19 `a=f`, all `t=s`; `icat-7ZPA3DTHIYZ3S`, `icat-LBQMPERQDCWQQ`, `icat-SFS4COUJE5736` and 17 more) were left, and were still in `/dev/shm` when checked later on 2026-10-24. `/dev/shm` is live state, not a kept file, so this check holds only for that moment. icat therefore leaves its frame objects for the terminal to remove. In kitty, inotify recorded 22 creations and 22 deletions in `/dev/shm` for one icat GIF; the log does not record which process deleted. viu removes its `t=t` files itself: in a run where the harness removed nothing, no viu file was left in `/tmp`.
- **Animation.** Only icat, and only on GIFs, uses `a=a` and `a=f` (with `c`, `r`, `y`, `z`, `s` and `v`). Its `a=a` commands carry `I` and no `q`, so a terminal that refuses them replies to each.
- **Detection and reply order.** icat sends three queries, `a=q` direct, `a=q,t=t` and `a=q,t=s`, each with an id, then DA1, the spec's detection sequence, which relies on the spec's ordering: a terminal that supports the protocol answers the queries before DA1. The harness always answered in that order, so no run shows what icat does when the order is broken. viu sends `a=q` direct and `a=q,t=t` (`i=31`), each followed by DA1, and `ESC [ 5 n` after every frame, waiting for `ESC [ 0 n`. ncplayer sends a burst of queries (DSR 6, DA3, XTVERSION, DA2, kitty keyboard, DECRQM 2026 and 1016, OSC 10 and 11, `a=q,i=1`, XTWINOPS 14 and 18) closed by DA1, its sentinel; with no replies it showed no frame. timg sends XTVERSION and DSR 5. chafa sends XTWINOPS 18, 14 and 16, DA1, and OSC 10 and 11.

## Rates on the harness

| Client | 1920x1080 | 7680x2160 |
|---|---|---|
| timg, 30 fps clip | 30 fps, 92 KB per frame | 30 fps, 92 KB per frame |
| ncplayer, 30 fps clip | 23 fps, 830 KB per frame (1920x1064) | 3.4 fps, 4.6 MB per frame (7676x2128) |
| chafa, 10 fps GIF | 10 fps | 10 fps |
| viu, 10 fps GIF | 9.9 fps | 9.9 fps |

timg does not scale its PNG frames to the display; ncplayer scales to it, and at 7680x2160 the pty bounds it. Under ADR-103 every timg frame (PNG) and every ncplayer frame (zlib) is decoded in the image helper, which the harness does not model.

## What kitty 0.49.2 does with them

- Every client displayed in kitty. kitty logged no graphics error for any of them.
- kitty's main-process RSS, sampled every half second, stayed between 135 and 140 MB for 158 seconds of viu's GIF, which places a new anonymous image every 100 ms, and between 138 and 146 MB for timg's video. Under ncplayer it reached 207 MB at its peak and ended at 139 MB. These figures are the main process's RSS only; GPU memory was not measured, so whether and how kitty bounds the piled-up frames is not known from these runs. The spec's storage quota section says that when a new image passes the quota, "the terminal emulator should delete older images to make space for the new one".
