# Bad Apple!! showcase

A demoscene homage that plays *Bad Apple!!* through each rendering layer a terminal has, each act running into the limit of the last: crude ASCII, block letters, aalib, sixel, colour emoji, kitty graphics (#75). It is the fork's announcement demo, filmed on the operator's display through the kmscon dev build.

The video is never downloaded or committed. *Bad Apple!!* is a fan work (original by ZUN, arrangement by Alstroemeria Records feat. nomico, shadow-art video by Anira); the operator supplies their own copy and names it with `BAD_APPLE=/path/to/video`.

## The acts

| Act | What plays | Needs | On kmscon today |
|---|---|---|---|
| 1. stick figures | Bad Apple!! as crude ASCII line art: the silhouettes' outlines traced with `- \| / \` on a 27-row grid at 12 fps | ffmpeg, python3 | plays |
| 2. we can do better | black, a dramatic pause, then "we can do better!" slammed in letter by letter in toilet's `mono12` block letters (▀ ▄ █, rainbow), then a half-block plasma fading in behind them | python3, toilet | plays |
| 3. aalib | libcaca's `img2txt` in a double-lined 80x25 frame, as if the console were in 80x25 mode; then it freezes, broken | ffmpeg, img2txt, python3 | plays |
| 4. don't we have libsixel? | "wait a minute... don't we have ... LIBSIXEL?" typed over the frozen frame | python3 | plays |
| 5. sixel | `mpv --vo=sixel` plays it 480 px wide, then it glitches (red channel only, shifted, a band torn sideways) and `img2sixel` holds the halted frame; without sixel, a panel of static reading "[sixel arrives in step 3]" | ffmpeg, mpv, img2sixel, python3 | placeholder: sixel arrives in step 3 of ADR-101 |
| 6. kitty and the cats | a fire wipe burns the screen away, then "hold on, we also have kitty. hold on, feeding the cats" and a crowd of colour cats (🐱🐈😺😸🙀🐈‍⬛ ...) | python3 | plays |
| 7. kitty, full size | `mpv --vo=kitty` full size with a sine-wave scroller of block letters and emoji over copper bars along the bottom, listing what the fork supports; without kitty graphics, the scroller under a panel reading "[kitty graphics arrive in steps 4 and 5]" | mpv, toilet, python3 | placeholder and scroller: kitty graphics arrive in steps 4 and 5 |

An end card with the credits follows act 7 and waits for a key. The acts follow the video on: act 1 plays from 0:00, act 3 from 0:30, act 5 from 0:38 and act 7 from 0:46 to the end.

Acts 5 and 7 ask the terminal what it draws. Sixel is attribute 4 in the answer to DA1 (`CSI c`); kitty graphics answer a query (`a=q`) sent before DA1 with `OK`, per the kitty graphics protocol's documentation. A terminal with neither ignores the query, an APC string, and answers DA1 alone; kmscon's libtsm does exactly that. `SIXEL=yes|no` and `KITTY=yes|no` override the answer.

## On a VT: `make showcase`

```sh
make showcase BAD_APPLE=/path/to/your/copy \
    DEMO_ARGS='--font-name="Monoid Nerd Font Mono" --font-size=30 --mode=7680x2160 --no-use-original-mode'
```

It renders the video's frame caches as you (so a missing video or tool fails before anything builds), builds and stages the dev build (`make demo`), and hands over to `fork/demo/vt-demo.sh`, which picks a free VT and runs `fork/demo/run-vt.sh` through sudo with the showcase as kmscon's session in place of the paged demo. The VT choice, the refusals, the restore of the stock console and the health check of kmscon's log are theirs, unchanged ([docs/fork/demo.md](../../docs/fork/demo.md)). Switch to the VT it names, press a key to start, and press q (or let the end card take a key) to finish.

| Variable | Effect |
|---|---|
| `BAD_APPLE=` | the video; required |
| `FROM=N` | start at act N |
| `AUDIO=1` | play the soundtrack with mpv, through the user's audio server (`run-vt.sh` passes the user's `/run/user/UID`) |
| `VT=N`, `DEMO_ARGS='...'` | as for `make vt-demo` |

`VT_DEMO_DRY_RUN=1 make showcase BAD_APPLE=...` builds, stages and prints the `run-vt.sh` command without running it. By hand: `sudo fork/demo/run-vt.sh --showcase=/abs/path [--showcase-from=N] [--showcase-audio] [N] [-- kmscon-option...]`.

`run-vt.sh` starts the session with a cleared environment, so the pacing variables below do not reach a VT run from your shell: tune them by editing their defaults at the top of `bad-apple.sh`.

## In any terminal

```sh
BAD_APPLE=/path/to/your/copy fork/showcase/bad-apple.sh            # the whole show
fork/showcase/bad-apple.sh --list                                  # the acts and what each needs
fork/showcase/bad-apple.sh --dry-run [--act N | --from N]          # the plan and the tools each act would use
BAD_APPLE=... fork/showcase/bad-apple.sh --act 3                   # one act
BAD_APPLE=... fork/showcase/bad-apple.sh --from 5                  # from act 5 on
BAD_APPLE=... fork/showcase/bad-apple.sh --prepare                 # render the frame caches only
fork/showcase/bad-apple.sh --fx plasma                             # one effect alone, with its frame rate
```

`--audio` plays the soundtrack, `--no-wait` starts without waiting for a key. q or Ctrl-C ends the show at any point: the players stop, and the cursor, the main screen, the colours and the terminal's mode come back. Only acts 1, 3, 5 and 7 need the video; acts 2, 4 and 6 run without it.

Acts 5 and 7 play for real where the terminal has the protocol: sixel in foot, `xterm -ti vt340`, WezTerm and a tmux built with sixel (Arch's 3.8 is); kitty graphics in kitty, Ghostty and WezTerm. Both were checked here on a private Xvfb display: act 5 in `xterm -ti vt340`, act 7 in kitty 0.49.

## Pacing

Every timing is a variable at the top of `bad-apple.sh` (seconds unless marked), and each can also be set in the environment for a run from a shell: where each act starts in the video and for how long (`ACT1_START`, `ACT1_SECONDS`, ...), frame rates, the act 2 pause, letter speed and plasma length (`ACT2_PAUSE`, `ACT2_LETTER`, `ACT2_PLASMA`), typing speed (`ACT4_CPS`), the glitch and hold of act 5, the fire and the cats of act 6, the scroller's rows and speed (`BANNER_ROWS`, `BANNER_SPEED`) and its text (`BANNER_TEXT`), how long a placeholder stays (`PLACEHOLDER_SECONDS`), and an optional starfield title card before act 1 (`TITLE_SECONDS`, 0 by default).

## The effects

The plasma (act 2), the fire wipe (act 6), the copper bars under the scroller (act 7) and the starfield title card are drawn with half blocks: ▀ with a 24-bit foreground and background is two pixels per cell, 384x108 pixels at the operator's 384x54 cells. They double as a text-throughput benchmark for the parser, damage tracking (#67) and bbulk: `--fx plasma|fire|copper|starfield|banner` runs one alone for `FX_SECONDS` (10) and prints its frames per second and bytes per frame. Each frame is one `write()`. The plasma runs as fast as the terminal takes it; the others are paced to 30 to 40 fps. In tmux at 384x54 on this machine the plasma drew 65 fps at 355 KB a frame (23 MB/s), the fire 32 fps at 31 KB.

## Tools

The acts run these programs; `--dry-run` says which are missing and the Arch package to install. Nothing is installed by the script.

| Program | Arch package | Used by | On the development host (2026-10-22) |
|---|---|---|---|
| python3 | python | every act (`fx.py`, standard library only) | present |
| ffmpeg, ffprobe | ffmpeg | acts 1, 3, 5: decoding, act 5's glitched clip | present (n9.0.2) |
| img2txt | libcaca | act 3 | present (0.99.beta20) |
| mpv | mpv | acts 5 and 7 (`--vo=sixel`, `--vo=kitty`), `--audio` | present (0.41.0; built without the `caca` vo, so act 3 uses `img2txt`) |
| img2sixel | libsixel | act 5's halted frame | present (1.10.5) |
| toilet | toilet | the block letters of acts 2 and 7 | present; without it, figlet's `banner` font, then plain text |
| figlet | figlet | fallback for toilet | present |
| chafa | chafa | not used | present |
| mplayer | mplayer | not used (`-vo aa` would need it) | missing |

The dev image (`fork/dev.Dockerfile`) carries none of the media tools and needs none: the show runs on the host, and `make test` runs only `--list`, `--dry-run` and the refusals. Pinning them there would mean adding `ffmpeg mpv libcaca libsixel toilet` to its `pacman -S` line.

## Files

- `bad-apple.sh`: the acts (`act_1` to `act_7`), the act table, the pacing, the probes, the clean exit.
- `fx.py`: everything that draws more than a line of text: the frame caches and their player, the big letters, typing, the placeholders, the cats, the scroller and the effects.
- `../showcase.mk`: `make showcase`.
- The frame caches go to `~/.cache/kmscon-showcase/<video id>/` (`SHOWCASE_CACHE` moves them), keyed by the video's path, size and time and by each act's settings. A cache made for one grid size is not reused for another: act 1's 27 rows and act 3's 80x25 shrink in a smaller terminal and render again there.

`tests/showcase/showcase.py`, part of `make test`, checks the act table, the plan, the refusals and `fx.py`'s helpers without a video, a terminal or any media tool.
