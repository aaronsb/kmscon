# Bad Apple!! showcase

A demoscene homage that plays *Bad Apple!!* through each rendering layer a terminal has, each act running into the limit of the last: crude ASCII, Braille dots, block letters, aalib, sixel, colour emoji, kitty graphics (#75). It is the fork's announcement demo, filmed on the operator's display through the kmscon dev build.

The video is never downloaded or committed. *Bad Apple!!* is a fan work (original by ZUN, arrangement by Alstroemeria Records feat. nomico, shadow-art video by Anira); the operator supplies their own copy and names it with `BAD_APPLE=/path/to/video`.

## The acts

| Act | What plays | Needs | On kmscon today |
|---|---|---|---|
| 1. stick figures | Bad Apple!! as crude ASCII line art (the silhouettes' outlines traced with `- \| / \` on a 27-row grid at 12 fps), then a Braille pass, 2x4 dots a cell, up to 52 rows at 24 fps | ffmpeg, python3 | plays (Braille is drawn from the font until #76) |
| 2. we can do better | black, a dramatic pause, then "we can do better!" slammed in letter by letter in toilet's `mono12` block letters (▀ ▄ █, rainbow), then a half-block plasma fading in behind them, then shadebobs | python3, toilet | plays |
| 3. aalib | libcaca's `img2txt` in 80x25. It asks the terminal for a real 80x25 grid (xterm's `CSI 8 ; 25 ; 80 t`) and plays full screen where the size took effect; elsewhere it plays in a double-lined 80x25 frame. Then it freezes, broken, and a resized terminal is put back to its size | ffmpeg, img2txt, python3 | plays in the frame (kmscon gains `CSI 8 t` with #77) |
| 4. don't we have libsixel? | "wait a minute... don't we have ... LIBSIXEL?" typed over the frozen frame | python3 | plays |
| 5. sixel | `mpv --vo=sixel` plays it half the screen's height, centred, then it glitches (red channel only, shifted, a band torn sideways) and halts on its last frame; without sixel, a panel of static reading "[no sixel: DA1 lacks attribute 4]" | ffmpeg, mpv, python3 | plays (ADR-103 step 3) |
| 6. kitty and the cats | a fire wipe burns the screen away, then "hold on, we also have kitty. hold on, feeding the cats" and a crowd of colour cats (🐱🐈😺😸🙀🐈‍⬛ ...) | python3 | plays |
| 7. kitty, full size | `mpv --vo=kitty` full size in the rows above a demoscene scroller: what the fork supports, in toilet's `mono9` block letters with colour emoji between, riding a sine wave over copper bars, one column a frame; without kitty graphics, a panel reading "[kitty graphics arrive in steps 4 and 5]" above the scroller | mpv, toilet, python3 | placeholder and scroller: kitty graphics arrive in steps 4 and 5 |

The end card follows act 7: the credits over a turning Braille wireframe, until a key. The acts follow the video on: act 1 plays from 0:00 (its Braille pass from 0:20), act 3 from 0:30, act 5 from 0:38 and act 7 from 0:46 to the end.

Acts 5 and 7 ask the terminal what it draws. Sixel is attribute 4 in the answer to DA1 (`CSI c`); kitty graphics answer a query (`a=q`) sent before DA1 with `OK`, per the kitty graphics protocol's documentation. A terminal with neither ignores the query, an APC string, and answers DA1 alone. kmscon offers sixel (ADR-103 step 3) and ignores the kitty query until step 4. `SIXEL=yes|no` and `KITTY=yes|no` override the answer.

Both size their pictures in pixels from the cell size the terminal reports, asked before each act: XTWINOPS 16 (`CSI 16 t`, the cell), else XTWINOPS 14 (`CSI 14 t`, the text area) over the cells, else the pty's pixel size, else 10x20. A DA1 query follows the two, so a terminal that answers neither is not waited for and is not asked again. Act 5's picture is `ACT5_SIZE` (0.5) of the screen's rows tall, narrower where the columns run out, and centred in whole cells, so it fits at any cell size: 1419x1061 pixels in 75x28 cells at 7680x2160 with a 19x38 cell, 505x378 in 51x20 at 1366x768 with a 10x19 cell. The clip is rendered 960 pixels wide (`ACT5_WIDTH`) and mpv scales it, without dithering (`ACT5_DITHER=none`): about 85 KB a frame at 7680x2160, against about 240 KB with mpv's default dithering, which also drops frames on this machine. mpv's last frame stays on screen after it exits (`--vo-sixel-alt-screen=no`), and that is the halted frame.

The scroller's list is one variable, `BANNER_FEATURES` at the top of `bad-apple.sh` (features separated by `;`, each followed by the next emoji of `BANNER_EMOJI`): add a feature there when it lands. `--act 7 --banner-only` runs the scroller alone, without the video, so it can be shown and tuned on kmscon today.

## Preview in kitty, foot or WezTerm

Run it straight from the checkout in a terminal on the desktop, outside make and the VT:

```sh
BAD_APPLE=/path/to/your/copy fork/showcase/bad-apple.sh
```

Press a key to start; q ends it. Add `--audio` for the soundtrack. What each terminal plays:

| Terminal | Act 3 | Act 5 | Act 7 |
|---|---|---|---|
| kitty | the framed 80x25 (kitty ignores `CSI 8 t`) | placeholder (no sixel) | the video, through kitty graphics |
| foot | the framed 80x25 | sixel | placeholder (no kitty graphics) |
| WezTerm | the framed 80x25 unless the window can be resized | sixel | the video |
| `xterm -ti vt340` | real 80x25 when `allowWindowOps` is on | sixel | placeholder |

Act 7 sizes the video to leave the scroller its rows: mpv gets `--vo-kitty-rows` (the screen's rows less `BANNER_ROWS`) and the pixel height of those rows, so the video fills the rows above and is centred across. Everything sizes itself to the window when each act starts, and the effects and the scroller follow a resize while they run. mpv sends kitty its frames as escape codes: `KITTY_SHM=yes` would use shared memory instead, but kitty 0.49 refuses the shared memory names mpv 0.41 gives ("POSIX SHM names must start with /") and shows no video, so it is off.

Checked here in kitty 0.49 and `xterm -ti vt340` on a private Xvfb display: kitty played act 7's video above the scroller and showed act 5's placeholder and act 3's frame; xterm played act 5 in sixel. foot and WezTerm are not installed on this machine.

## On a VT: `make showcase`

```sh
make showcase BAD_APPLE=/path/to/your/copy \
    DEMO_ARGS='--font-name="Monoid Nerd Font Mono" --font-size=30 --mode=7680x2160 --no-use-original-mode'
```

It renders the video's frame caches as you (so a missing video or tool fails before anything builds), builds and stages the dev build (`make demo`), and hands over to `fork/demo/vt-demo.sh`, which picks a free VT and runs `fork/demo/run-vt.sh` through sudo with the showcase as kmscon's session in place of the paged demo. The VT choice, the refusals, the restore of the stock console and the health check of kmscon's log are theirs, unchanged ([docs/fork/demo.md](../../docs/fork/demo.md)). Switch to the VT it names, press a key to start, and press q (or a key on the end card) to finish.

| Variable | Effect |
|---|---|
| `BAD_APPLE=` | the video; required |
| `FROM=N` | start at act N |
| `AUDIO=1` | play the soundtrack with mpv, through the user's audio server (`run-vt.sh` passes the user's `/run/user/UID`) |
| `VT=N`, `DEMO_ARGS='...'` | as for `make vt-demo` |

`VT_DEMO_DRY_RUN=1 make showcase BAD_APPLE=...` builds, stages and prints the `run-vt.sh` command without running it. By hand: `sudo fork/demo/run-vt.sh --showcase=/abs/path [--showcase-from=N] [--showcase-audio] [N] [-- kmscon-option...]`.

`run-vt.sh` starts the session with a cleared environment, so the pacing variables below and `SHOWCASE_CACHE` do not reach a VT run from your shell: tune them by editing their defaults at the top of `bad-apple.sh`. `make showcase` renders the caches with the environment cleared too, so they are the ones the VT run reads. `AUDIO=` takes `yes` or `1` for the soundtrack and `no`, `0` or nothing for none.

## Options

```sh
fork/showcase/bad-apple.sh --list                                  # the acts and effects, and what each needs
fork/showcase/bad-apple.sh --dry-run [--act N | --from N]          # the plan and the tools each act would use
BAD_APPLE=... fork/showcase/bad-apple.sh --act 3                   # one act
BAD_APPLE=... fork/showcase/bad-apple.sh --from 5                  # from act 5 on
fork/showcase/bad-apple.sh --act 7 --banner-only                   # the scroller alone, no video needed
BAD_APPLE=... fork/showcase/bad-apple.sh --prepare                 # render the frame caches only
fork/showcase/bad-apple.sh --fx plasma                             # one effect alone, with its frame rate
```

`--audio` plays the soundtrack, `--no-wait` starts without waiting for a key. q or Ctrl-C ends the show at any point: the players stop, and the cursor, the main screen, the colours and the terminal's mode come back. mpv's video outputs turn on mouse tracking (`CSI ?1003 h`) and turn it off only when they exit cleanly, so a player stopped by q left a terminal reporting every mouse movement to the shell (#88); the show now turns off every mouse mode and encoding, focus events and bracketed paste on its way out. While mpv plays, a mouse report arriving as keys is read whole and dropped, so a byte of its coordinates is not taken for q. Acts 2, 4 and 6, and act 7 with `--banner-only`, run without the video.

## Pacing

Every timing is a variable at the top of `bad-apple.sh` (seconds unless marked), and each can also be set in the environment for a run from a shell: where each act starts in the video and for how long (`ACT1_START`, `ACT1_SECONDS`, `ACT1_DOTS_SECONDS`, ...), frame rates, the act 2 pause, letter speed, plasma and shadebobs (`ACT2_PAUSE`, `ACT2_LETTER`, `ACT2_PLASMA`, `ACT2_BOBS`), act 3's resize (`ACT3_RESIZE=no` keeps the frame; `RESIZE_WAIT`, 1.5 s, is how long it waits for the terminal), typing speed (`ACT4_CPS`), act 5's size, clip width, dither, glitch and hold (`ACT5_SIZE`, `ACT5_WIDTH`, `ACT5_DITHER`, `ACT5_GLITCH`, `ACT5_HOLD`), the fire and the cats of act 6, the scroller's rows, frame rate and words (`BANNER_ROWS`, `BANNER_FPS`, `BANNER_FEATURES`, `BANNER_EMOJI`, `BANNER_INTRO`, `BANNER_OUTRO`), how long a placeholder stays (`PLACEHOLDER_SECONDS`), and an optional starfield title card before act 1 (`TITLE_SECONDS`, 0 by default).

## The effects

Half blocks (▀ with a 24-bit foreground and background) make each cell two pixels, 384x108 at the operator's 384x54 cells; Braille makes each cell 2x4 dots in one colour, 768x216. The painters keep the last frame and send only the cells that changed, with a cursor move only where they skip and SGR only where the colour changes, and each frame is one `write()`. On a resize the effect is rebuilt at the new size.

| `--fx` | Where it plays | Drawn with |
|---|---|---|
| `plasma` | act 2, behind the letters | half blocks; two sine fields and a cycling palette of 64 colours |
| `shadebobs` | act 2, after the plasma | half blocks; blurred blobs on Lissajous paths added into a buffer that fades, brightness piling up where they cross |
| `fire` | the wipe into act 6 | half blocks; the classic fire, its source on an edge climbing the screen, charring what it touches |
| `copper` | act 7, under the scroller | half blocks; raster bars over a dark gradient |
| `stars` | the title card before act 1 | half blocks; a starfield behind the title in big letters |
| `braille` | behind the end card | Braille; a turning torus over an oscilloscope trace, lines that still read if a font draws the dots off their grid |
| `banner` | act 7's scroller | half blocks and emoji; the sine scroller over copper bars |

They double as a text-throughput benchmark for the parser, damage tracking (#67) and bbulk: `--fx NAME` runs one alone for `FX_SECONDS` (10) and prints its frames per second and bytes per frame. Measured in tmux at 384x54 on this machine, 4 s each:

| Effect | Frames a second | Bytes a frame | MB/s |
|---|---|---|---|
| plasma (unpaced) | 110 | 194,100 | 21.3 |
| shadebobs | 40 | 57,767 | 2.3 |
| fire | 40 | 28,920 | 1.2 |
| copper | 40 | 19,329 | 0.8 |
| stars | 39 | 35,804 | 1.4 |
| braille | 40 | 54,005 | 2.2 |
| banner | 40 | 35,805 | 1.4 |

All but the plasma are paced to 40 fps, and Python holds that at 384x54; the plasma runs as fast as the terminal takes it, so it measures the terminal. Sending only the changed cells took the plasma from 355 KB a frame to 194 KB.

## Tools

The acts run these programs; `--dry-run` says which are missing and the Arch package to install. Nothing is installed by the script.

| Program | Arch package | Used by | On the development host (2026-10-22) |
|---|---|---|---|
| python3 | python | every act and effect (standard library only) | present |
| ffmpeg, ffprobe | ffmpeg | acts 1, 3, 5: decoding, act 5's glitched clip | present (n9.0.2) |
| img2txt | libcaca | act 3 | present (0.99.beta20) |
| mpv | mpv | acts 5 and 7 (`--vo=sixel`, `--vo=kitty`), `--audio` | present (0.41.0; built without the `caca` vo, so act 3 uses `img2txt`) |
| img2sixel | libsixel | not used: on this host it prints nothing for any picture, so act 5 holds mpv's last frame instead | present (1.10.5, with gdk-pixbuf 2.44, whose loader libsixel reads before closing it) |
| toilet | toilet | the block letters of acts 2 and 7 and the title card | present; without it, figlet's `banner` font, then plain text |
| figlet | figlet | fallback for toilet | present |
| chafa | chafa | not used | present |
| mplayer | mplayer | not used (`-vo aa` would need it) | missing |

The dev image (`fork/dev.Dockerfile`) carries none of the media tools and needs none: the show runs on the host, and `make test` runs only `--list`, `--dry-run`, the refusals, the pure helpers and act 4 on a pty (python3 alone), quit with q to check the modes it turns off. Pinning them there would mean adding `ffmpeg mpv libcaca toilet` to its `pacman -S` line.

## Files

| File | What it holds |
|---|---|
| `bad-apple.sh` | the acts (`act_1` to `act_7`), the act and effect tables, the pacing, the probes, the clean exit |
| `fx.py` | the drawing commands bad-apple.sh runs |
| `screen.py` | output, keys, the window size and resizes, colours, the half-block and Braille painters |
| `effects.py` | plasma, shadebobs, fire, copper, stars, the Braille wireframe, and the runner that draws one alone |
| `scenes.py` | act 2's letters over the effects, act 7's scroller, typing, placeholders, the cats, the end card |
| `letters.py` | toilet's and figlet's letters read back into pixels |
| `frames.py` | the video acts' frame caches (outlines, Braille, libcaca) and their player |
| `../showcase.mk` | `make showcase` |

The frame caches go to `~/.cache/kmscon-showcase/<video id>/` (`SHOWCASE_CACHE` moves them), keyed by the video's path, size and time and by each act's settings. A cache made for one grid size is not reused for another: act 1's grids and act 3's framed region shrink in a smaller terminal and render again there.

`tests/showcase/showcase.py`, part of `make test`, checks the act and effect tables, the plan, the refusals and the pure helpers (the painter's diff, Braille dots, the outline table, img2txt's colours, act 5's picture size, the XTWINOPS answers) without a video or any media tool, and quits act 4 on a pty to check that every mouse mode, focus events and bracketed paste are turned off.
