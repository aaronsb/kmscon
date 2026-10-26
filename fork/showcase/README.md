# Bad Apple!! showcase

A demoscene homage that plays *Bad Apple!!* through each rendering layer a terminal has, each act running into the limit of the last: crude ASCII, Braille dots, block letters, aalib, sixel, colour emoji, kitty graphics (#75). It is the fork's announcement demo, filmed on the operator's display through the kmscon dev build.

The video is never downloaded or committed. *Bad Apple!!* is a fan work (original by ZUN, arrangement by Alstroemeria Records feat. nomico, shadow-art video by Anira); the operator supplies their own copy and names it with `BAD_APPLE=/path/to/video`.

## The acts

| Act | What plays | Needs | On kmscon today |
|---|---|---|---|
| 1. stick figures | Bad Apple!! as crude ASCII line art (the silhouettes' outlines traced with `- \| / \` on a 27-row grid at 12 fps), then a Braille pass, 2x4 dots a cell, up to 52 rows at 24 fps | ffmpeg, python3 | plays (Braille is drawn from the font until #76) |
| 2. we can do better | black, a dramatic pause, then "we can do better!" slammed in letter by letter in toilet's `mono12` block letters (▀ ▄ █, rainbow), then a half-block plasma fading in behind them, then shadebobs | python3, toilet | plays |
| 3. aalib | "this has got to beat ascii line art... how about ... AALIB?" typed on black, then libcaca's `img2txt` in 80x25. It asks the terminal for a real 80x25 grid (xterm's `CSI 8 ; 25 ; 80 t`) and plays full screen where the size took effect; elsewhere it plays in a double-lined 80x25 frame. Then it freezes, broken, and a resized terminal is put back to its size | ffmpeg, img2txt, python3 | plays in the frame (kmscon gains `CSI 8 t` with #77) |
| 4. don't we have libsixel? | "wait a minute... don't we have ... LIBSIXEL?" typed over the frozen frame | python3 | plays |
| 5. sixel | `mpv --vo=sixel` plays it half the screen's height, centred, then it glitches (red channel only, shifted, a band torn sideways) and halts on its last frame while the sound stutters, its last 0.3 s repeating shorter each time like a skipping buffer; without sixel, a panel of static reading "[no sixel: DA1 lacks attribute 4]" | ffmpeg, mpv, python3 | plays (ADR-103 step 3) |
| 6. kitty and the cats | a fire wipe burns the screen away, uncovering lines of CJK ideographs scrolling up behind the flames, which fill the screen for two bars once it has burnt out (a stress of the parser, the glyph cache and scrolling, the glyph kit's seeded text) and are then blown away by a wind, then "hold on, we also have kitty. one sec, feeding the cats" and a crowd of colour cats (🐱🐈😺😸🙀🐈‍⬛ ...); then an interlude: six of Noto Emoji's vector cat faces in a row in kitty graphics, rendered by `rsvg-convert` at up to a quarter of the screen's height on the first run and cached, pixel sunglasses dropping onto each from left to right, one a beat, and the row sliding off the left edge (emoji and ⌐■-■ without kitty graphics or rsvg-convert) | python3; rsvg-convert for the interlude in kitty graphics | plays |
| 7. kitty, full size | the video through the fork's own kitty graphics player ([Act 7's player](#act-7s-player)) in the rows above a demoscene scroller, for a minute, its picture and sound fading in over 1.5 s (`ACT7_FADE_IN`): one copy in the middle slides left with a bounce, then a second slides out from behind it, then a third, each frame transmitted once and placed three times. The scroller is what the fork supports, in toilet's `mono9` block letters with colour emoji between, riding a sine wave over copper bars, one column a frame; without kitty graphics, a panel reading "[no kitty graphics: the a=q query got no OK]" above the scroller | ffmpeg, toilet, python3 | plays (kitty graphics, ADR-103 step 4c) |

The end card follows act 7: the credits over a turning Braille wireframe, until a key. The acts follow the video on: act 1 plays from 0:00 (its Braille pass from 0:11.3, the beat after the girl turns from the apple, where the grindy synth comes in), act 3 from 0:30, act 5 from 0:38 and act 7 from 0:46 to 1:46, where the end card takes over and act 7's sound has faded out over its last two seconds (`ACT7_FADE`). A placeholder in act 7 stays no longer than that. The times are one hand-written cue sheet, `CUES` at the top of `bad-apple.sh`, in the video's time (m:ss): edit them there, and `--list` prints them. Acts 2, 4 and 6 sit between cues for their own lengths, to music cues of their own ([Music](#music)), and the soundtrack picks up at the next cue; delete the `endcard` line to play act 7 to the end of the video. Act 7's moves are cues as well: its copy slides left at 0:54 (`act7-slide`), the second copy comes out at 1:02 (`act7-copy2`) and the third at 1:10 (`act7-copy3`); a move whose line is deleted never comes, and a time may carry a fraction (`0:54.5`).

Acts 5 and 7 ask the terminal what it draws. Sixel is attribute 4 in the answer to DA1 (`CSI c`); kitty graphics answer a query (`a=q`) sent before DA1 with `OK`, per the kitty graphics protocol's documentation. A terminal with neither ignores the query, an APC string, and answers DA1 alone. kmscon offers both: sixel since ADR-103 step 3, kitty graphics since step 4c. `SIXEL=yes|no` and `KITTY=yes|no` override the answer.

Both size their pictures in pixels from the cell size the terminal reports, asked before each act: XTWINOPS 16 (`CSI 16 t`, the cell), else XTWINOPS 14 (`CSI 14 t`, the text area) over the cells, else the pty's pixel size, else 10x20. A DA1 query follows the two, so a terminal that answers neither is not waited for and is not asked again. Act 5's picture is `ACT5_SIZE` (0.5) of the screen's rows tall, narrower where the columns run out, and centred in whole cells, so it fits at any cell size: 1419x1061 pixels in 75x28 cells at 7680x2160 with a 19x38 cell, 505x378 in 51x20 at 1366x768 with a 10x19 cell. The clip is rendered 960 pixels wide (`ACT5_WIDTH`) and mpv scales it, without dithering (`ACT5_DITHER=none`): about 85 KB a frame at 7680x2160, against about 240 KB with mpv's default dithering, which also drops frames on this machine. mpv's last frame stays on screen after it exits (`--vo-sixel-alt-screen=no`), and that is the halted frame. Act 7's copies are as tall as fits with three side by side above the scroller: 44 rows, 118 columns each, at 7680x2160 with a 19x38 cell.

The scroller's list is one variable, `BANNER_FEATURES` at the top of `bad-apple.sh` (features separated by `;`, each followed by the next emoji of `BANNER_EMOJI`): add a feature there when it lands. `--act 7 --banner-only` runs the scroller alone, without the video, so it can be shown and tuned on kmscon today.

## Preview in kitty, foot or WezTerm

Run it straight from the checkout in a terminal on the desktop, outside make and the VT:

```sh
BAD_APPLE=/path/to/your/copy fork/showcase/bad-apple.sh
```

Press a key to start; q ends it. Add `--audio` for the soundtrack ([Sound](#sound)). What each terminal plays:

| Terminal | Act 3 | Act 5 | Act 7 |
|---|---|---|---|
| kitty | the framed 80x25 (kitty ignores `CSI 8 t`) | placeholder (no sixel) | the video, through kitty graphics |
| foot | the framed 80x25 | sixel | placeholder (no kitty graphics) |
| WezTerm | the framed 80x25 unless the window can be resized | sixel | the video |
| `xterm -ti vt340` | real 80x25 when `allowWindowOps` is on | sixel | placeholder |

Act 7 leaves the scroller its rows (`BANNER_ROWS`) and plays in the rows above. Everything sizes itself to the window when each act starts, and the effects, the scroller and act 7's copies follow a resize while they run. To try act 7 alone, with its sound:

```sh
BAD_APPLE=/path/to/your/copy fork/showcase/bad-apple.sh --audio --act 7
```

Checked here in kitty 0.49 and `xterm -ti vt340` on a private Xvfb display: kitty played act 7's video above the scroller and showed act 5's placeholder and act 3's frame; xterm played act 5 in sixel. foot and WezTerm are not installed on this machine. On 2026-10-24, kitty 0.49.2 at 1280x800 ran the whole show with `--audio --no-wait`, its sound sent to a PipeWire null sink (`AUDIO_DEVICE=pipewire/kmscon_showcase_check`), each act's start timed from a trace of the script: act 1's outlines played 20.0 s and its Braille pass 10.1 s against the soundtrack's 30 s from 0:00; act 3 8.1 s against 8 s from 0:30 (after the 1.5 s act 3 waits for a resize kitty does not make); act 5 the placeholder for 6 s, silent; act 7 the video above the scroller for 60.4 s from 0:46, then the end card. Act 2 took 18.2 s, act 4 7.9 s and act 6 19.5 s, all silent then. On 2026-10-25, with their music cues, on a 140x40 pty with mpv's sound sent nowhere (`--ao=null`), act 2 took 19.4 s, act 4 9.1 s and act 6 20.9 s, act 6's cats cue starting 10.2 s after its fire cue.

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
| `AUDIO=1` | play the soundtrack with mpv, through the user's audio server (`run-vt.sh` passes the user's `/run/user/UID`); the show asks which output on the VT before it starts ([Sound](#sound)) |
| `AUDIO_DEVICE=` | the output to play through, as `mpv --audio-device=help` names it (`none` for no sound), so nothing is asked; it plays the soundtrack without `AUDIO=1` too, and `AUDIO=0` ignores it |
| `VT=N`, `DEMO_ARGS='...'` | as for `make vt-demo` |

`VT_DEMO_DRY_RUN=1 make showcase BAD_APPLE=...` builds, stages and prints the `run-vt.sh` command without running it. By hand: `sudo fork/demo/run-vt.sh --showcase=/abs/path [--showcase-from=N] [--showcase-audio[=DEVICE]] [N] [-- kmscon-option...]`.

`run-vt.sh` starts the session with a cleared environment, so the pacing variables below and `SHOWCASE_CACHE` do not reach a VT run from your shell: tune them by editing their defaults at the top of `bad-apple.sh`. `make showcase` renders the caches with the environment cleared too, so they are the ones the VT run reads. `AUDIO=` takes `yes` or `1` for the soundtrack and `no` or `0` for none; left out, the soundtrack plays when `AUDIO_DEVICE` names an output.

To run the show yourself in a login session, where logind gives the session the sound cards while its VT is on screen and no `audio` group is needed, use `make vt-login` ([docs/fork/demo.md](../../docs/fork/demo.md)) and run `BAD_APPLE=/path/to/your/copy fork/showcase/bad-apple.sh --audio` once logged in.

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

`--audio` plays the soundtrack ([Sound](#sound)), `--no-wait` starts without waiting for a key. q or Ctrl-C ends the show at any point: the players stop, and the cursor, the main screen, the colours and the terminal's mode come back. mpv's video outputs turn on mouse tracking (`CSI ?1003 h`) and turn it off only when they exit cleanly, so a player stopped by q left a terminal reporting every mouse movement to the shell (#88); the show now turns off every mouse mode and encoding, focus events and bracketed paste on its way out. While mpv plays, a mouse report arriving as keys (X10 or SGR) is dropped once it is whole, even when it arrives split across reads, so a byte of its coordinates is not taken for q; any other byte, a q right after Esc included, is a key. When mpv stops, however it stops, the show turns tracking off again and drops the reports already sent. Acts 2, 4 and 6, and act 7 with `--banner-only`, run without the video.

## Sound

With `--audio`, before the show takes the screen, it asks where the soundtrack plays, through `fork/demo/pick-audio.sh`, which other demos can call too. It lists the outputs `mpv --audio-device=help` finds: the sound server's (PipeWire's, else PulseAudio's), the server's default first, or, with no server, each ALSA card's default and HDMI devices, and `0` for no sound. A server with no sink but its dummy (`auto_null`) counts as none. A number plays a one-second 660 Hz tone through that output, and so does a device name typed in its place (any word of the form mpv's device names take, as `mpv --audio-device=help` lists them; any other answer asks again); `y` keeps it, `t` plays the tone again, anything else asks again. `AUDIO_DEVICE=NAME` (an mpv device name, or `none`) answers the question in advance; with `--no-wait` nothing is asked and the sound plays through mpv's default output, so `AUDIO_DEVICE` is what names an output in an unattended run. Nothing is asked when no act in the run plays sound (act 7 with `--banner-only` alone). The soundtrack, act 5's clip, act 7's video and the music cues then play through that output.

### Music

Where the video's soundtrack does not play, the show plays music of its own, written for it in Strudel and rendered to Opus in `music/`: original, in the soundtrack's key and tempo (E♭ minor, 138 BPM, measured from the video), so a cue hands back to the video without a jolt.

| Cue | Plays | Length |
|---|---|---|
| `act2-better` | act 2: a riser through the pause, a rising blip and tom for each letter, a slam on the "!", then a 909 groove, bass and arpeggio under the plasma, a gliding saw lead over the shadebobs, and a last hit | 10 bars and a ring |
| `act3-aalib` | act 3's opening line: a soft square-wave arpeggio thinking it over, a question after the "...", a run up to a chime on AALIB and a build into the video | 4.5 bars, the typing's 7.8 s |
| `act4-minute` | act 4: a tape stop, a ticking clock and a sneaking bass, a question after each "...", a B♭ chime on LIBSIXEL and a half-bar build into act 5 | 4.5 bars, the typing's 7.8 s |
| `act6-fire` | act 6's fire and CJK stream: rumble, crackle and an arpeggio doubling its speed each bar, data bleeps thickening as the stream rises, a hit as the fire goes out, two driving bars under the full-screen stream, a stop as it clears, then a music box in G♭ major under the typing | 5 bars, then until the cats |
| `act6-cats` | the cats: a square-wave tune in G♭ major whose long notes meow (a pitch envelope up and back), a bloop for each cat, a run up to D♭ into the interlude | 5 bars and a ring |
| `act6-shades` | the interlude: a bloop as each face pops in, a slide whistle as each pair of sunglasses falls and a synth meow and a click as it lands, over a half-time beat, a major seventh once all six are on, then a gust panning right to left over B♭ as the row slides off and act 7 fades in | 4 bars, the interlude's own |
| `endcard` | the end card: an epilogue under the torus, pad and arpeggio, half-time drums, a lead that glides and bends, looped until the key | 16 bars, looped |

The acts' times fall on their cue's beats: act 2's pause is 19 sixteenths (`ACT2_PAUSE`), its letters a sixteenth apart (`ACT2_LETTER`), so the "!" lands on bar 2, the plasma on bar 3 (`ACT2_HOLD`), four bars of it (`ACT2_PLASMA`, fading in over one, `ACT2_FADE`) and three of shadebobs (`ACT2_BOBS`); act 6's fire is three bars (`ACT6_FIRE`), its full-screen CJK stream two (`ACT6_CJK`) and its cats five (`ACT6_CATS`). Act 2's pause runs inside `fx big`, counted from its start, so the 0.3 s toilet takes to draw the letters falls inside it. A cue starts just before what it plays under and the act waits `CUE_LATENCY` (0.1 s, mpv's start to sound through PipeWire here) before drawing. Each cue stops the one before it, and the soundtrack stops a cue still ringing.

To change a cue, edit its `.js` (it plays as it is on strudel.cc), then render it again:

```sh
cd fork/showcase/music && npm install && node render.mjs act2-better.js   # or no file: every cue
```

`render.mjs` runs Strudel's own offline renderer (`renderPatternAudio` from `@strudel/web`) in headless Chromium (`/usr/bin/chromium`, else `CHROMIUM=`), with the 909 samples Strudel fetches from GitHub. Each cue's header names its tempo, bars, tail and whether it loops (a loop is rendered twice and the second pass kept, so it starts with its own reverb and loops without a seam); ffmpeg brings it to -8 LUFS (`LUFS=`; the soundtrack is about -6.7) under a limiter 2 dB down and encodes Opus at 160 kb/s. Node, npm and Chromium are needed only to render; the show plays the `.ogg` files.

On the VT, the wired and USB outputs need the user in the `audio` group: the operator's choice, 2026-10-24 (#171). `run-vt.sh` starts the show without a logind session, so while its VT is on screen logind grants nobody the sound cards (`/dev/snd`) through their ACLs, and PipeWire, which opens them for the user, drops them. With the group, PipeWire and mpv open the cards whatever VT is on screen. The trade-off: every process the user runs gets raw ALSA on every VT, outside logind's seat model. Join with `sudo usermod -aG audio $USER`, then log out of every session (or reboot), so that the user's PipeWire starts again with the group. Until then the show says so above its list and offers what still plays, a Bluetooth output (WirePlumber ships with Bluetooth seat monitoring off) or no sound. When it finds no output at all, one line says why: no sound card; the user in `/etc/group`'s audio group, with the cards out of reach because this login started before they joined it, or with a PipeWire that did (#171's case: log out of every session, or reboot); or the cards out of reach and the user not in the group. A device name can still be typed there, and Enter is no sound. On the desktop the user's session holds the cards, and no group is needed; so does a login on a VT, `make vt-login` ([docs/fork/demo.md](../../docs/fork/demo.md)).

## Act 7's player

`kitty.py` plays act 7, written from the kitty graphics protocol's published spec alone (ADR-300's clean room). ffmpeg decodes the clip at a copy's own pixel size, at most 2160 pixels tall (`KITTY_WIDTH=0`), and each frame is transmitted once (`a=t`) and placed up to three times (`a=p`, placement ids 1 to 3) in each copy's cells (`c` and `r`). A copy moves by being placed again under its placement id at another column, which the spec says replaces the placement "without flicker"; nothing is sent again. Re-sending an image id deletes that image's placements, so the frames alternate between two ids: a frame is transmitted to one, its copies placed, and the other deleted with `d=I`, which frees its data, all in one write. The terminal never holds more than two of the player's images, and a copy is never without its picture. Copy 1 is in front (`z=3`) and each later copy behind the one before, so a copy sliding out from behind another starts hidden. Every command carries `q=2`, so nothing comes back into the show's input, and every placement `C=1`, so the cursor stays put. `KITTY_SHM`, which act 7's mpv read, is gone; `KITTY_TRANSPORT=shm` takes its place.

The player encodes each frame in its own loop, so a copy's own size costs it more on a slow machine: a greyscale PNG took 4.6 ms at 2236x1672 and 14.5 ms at 4400x3300 in a synthetic run. A number in `KITTY_WIDTH` limits that: frames decode that many pixels wide, and the terminal scales them up, which costs the terminal instead (bbulk's scaling on kmscon, below). The frame size is set when act 7 starts; after a resize the terminal scales the frames until the next act.

The transports (`KITTY_TRANSPORT`):

| Transport | What travels | Bytes a frame at 480x360 |
|---|---|---|
| `raw` | `f=24` RGB inline, base64 in 4096-byte chunks | 693,600 |
| `zlib` | the same compressed with zlib (`o=z`); Bad Apple is nearly all black and white | about 31,400 |
| `png` (default) | `f=100`, an 8-bit greyscale PNG made by the player: the spec has no grey raw format (`f` is 24, 32 or 100), and PNG is its compact form | about 17,800 |
| `shm` | `f=24` RGB in a POSIX shared memory object (`t=s`), a new name starting with `/` each frame, which the terminal reads and unlinks; while the frames it has yet to read hold 64 MB (five at 8K, at least two), the player drops frames rather than send more | about 260 |

`shm` is asked for first with an `a=q` of one pixel and falls back to `png` when the terminal refuses it: kmscon reads no shared memory for a session outside logind, which is how `run-vt.sh` starts the show ([docs/image-helper.md](../../docs/image-helper.md)). On kmscon, `zlib`, `png` and `shm` go through its image helper, with the pty paused for each frame; `raw` stays in libtsm.

The frames follow the soundtrack: with `--audio` the player reads mpv's position (`time-pos`) over mpv's IPC socket four times a second and shows the frame for that moment, dropping the frames it is late for. The socket sits in `XDG_RUNTIME_DIR` (else `/tmp`), so its path stays within the 108 bytes a socket's path may hold. Until mpv answers, and for the rest of the act if it has not answered within 3 s, the player follows a wall clock from the moment the soundtrack started, and says so once the screen is back. Without sound it runs on its own clock. q ends it even when the terminal has fallen behind; SIGTERM and SIGHUP end it as Ctrl-C does, its images deleted and its shared memory unlinked.

Measured on 2026-10-24 on the development host (north, a Ryzen 9 9950X3D), 480x360 frames of the clip from 0:46, the copies side by side as act 7 sizes them (12 rows left for the scroller), the scroller off. In kitty 0.49.2 on a private Xvfb display, at 1280x800 (128x36 cells) and 3840x2160 (202x49), the player sent every frame, 30 a second, at one, two and three copies in every transport, with no frame dropped. Sent as fast as kitty read them from the pty (`--unpaced`; a ceiling on the transport, not frames kitty drew), three copies ran at 266 (`raw`), 864 (`zlib`), 1,294 (`png`) and 2,026 (`shm`) frames a second at 1280x800, and 73, 664, 1,049 and 1,585 at 3840x2160. mpv --vo=kitty, at the area act 7 gave it before, showed 27.8 frames a second at 1280x800 and 10.8 at 3840x2160; at one copy's size, 29.9 and 16.7, one copy to an mpv.

On kmscon, in tier 3's VM (4 vCPUs, the 2D renderer: drm2d and bbulk) at 7680x2160 with a 19x38 cell and at 1280x800 with a 10x19 cell, each 8 s, frames presented a second by kmscon's render timer, then kmscon's CPU time per frame sent, parsing and the image helper's round trip included:

| Transport | 7680x2160, 1 copy | 2 copies | 3 copies | 1280x800, 1 copy | 2 copies | 3 copies |
|---|---|---|---|---|---|---|
| `raw` | 29.2 (24.8 ms) | 29.7 (27.0 ms) | 26.7 (26.3 ms) | 29.4 (4.3 ms) | 29.8 (4.3 ms) | 29.8 (4.3 ms) |
| `zlib` | 30.0 (23.2 ms) | 29.9 (25.2 ms) | 27.1 (24.9 ms) | 30.4 (2.2 ms) | 30.4 (2.3 ms) | 30.5 (2.4 ms) |
| `png` | 30.0 (23.1 ms) | 30.1 (25.2 ms) | 27.6 (25.3 ms) | 30.0 (2.1 ms) | 30.3 (2.2 ms) | 30.8 (2.3 ms) |
| `shm` | 29.7 (22.8 ms) | 29.8 (24.9 ms) | 28.3 (25.6 ms) | 29.8 (1.9 ms) | 29.8 (2.0 ms) | 29.8 (2.1 ms) |

The player sent 29.2 to 29.7 frames a second in every run and dropped none. At 7680x2160 composition sets the rate: each copy is 2242x1672 pixels, the render timer's median frame took 22.4 ms with one copy and 26.5 ms with three, and three copies present 27 to 28 frames a second; the transport moves kmscon's time by about 2 ms a frame. `png` is the default: inline, so it works before login, the smallest on the pty, and as cheap as `shm` on kmscon within the run's noise. mpv --vo=kitty at act 7's old area sent 17.8 frames a second at 7680x2160 (94 of 240 dropped) at 50 ms of kmscon's time each, and 29.6 at 1280x800. The harness and every figure are in the session's artifacts (`session-artifacts-2026-10-24/kplayer`).

Act 7 itself, in kitty at 1280x800 with the scroller, the moves and the soundtrack on a PipeWire null sink, played 600 frames in 20 s on mpv's clock with none dropped.

### Where kmscon's time goes

Asked after the operator saw act 7 under `mpv --vo=kitty` run smoother in desktop kitty, full screen on the same display with a smaller font, than on kmscon at 7680x2160. The same tier 3 VM at 7680x2160 (19x38 cell, 404x56 cells; the video rows 404x44), 8 s each, kmscon's CPU per frame sent split into composition (the render timer's CPU over the window, bbulk's scaling and blending) and the rest (pty read, parsing, base64, conversion to the store's premultiplied ARGB, placements); the image helper runs in its own process, on another core, and is counted apart:

| Stream (pixels a frame) | Frames a second sent / presented | Composition, per frame sent | The rest, per frame sent | Helper, per frame sent | kmscon's CPU |
|---|---|---|---|---|---|
| player, `png` 480x360, 1 copy (scaled to 2229x1672) | 29.3 / 29.4 | 22.5 ms | 0.6 ms | 0.5 ms | 68% |
| player, `png` 480x360, 3 copies | 29.7 / 28.1 | 24.9 ms | 0.7 ms | 0.7 ms | 76% |
| player, `png` 2236x1672, 1 copy (unscaled) | 29.7 / 30.5 | 2.7 ms | 6.2 ms | 7.8 ms | 27% |
| player, `png` 2236x1672, 3 copies | 29.7 / 30.9 | 6.9 ms | 6.1 ms | 7.8 ms | 39% |
| player, `zlib` 2236x1672, 3 copies | 29.5 / 30.4 | 7.2 ms | 6.3 ms | 6.3 ms | 40% |
| player, `shm` 2236x1672, 3 copies | 29.7 / 29.8 | 6.6 ms | 3.0 ms | 2.4 ms | 29% |
| player, `raw` 2236x1672, 1 copy | 15.8 / 15.8 | 3.2 ms | 42.9 ms | none | 73% |
| mpv's recorded stream (2235x1672 `f=24`, as act 7 sent it), fed unpaced | 22.2 / 22.4 | 2.7 ms | 41.8 ms | none | 99% |
| the player's recorded `png` 2236x1672 3-copy stream, fed unpaced | 60.5 / 61.7 | 6.7 ms | 5.4 ms | 7.6 ms | 73% |

Two costs set kmscon's rate. Scaling: each frame is a new image, so bbulk scales it afresh, about 20 ms for a 2229x1672 copy of a 480x360 frame, where an unscaled picture of that size composes in about 3 ms. And inline raw data: mpv's 15 MB of base64 a frame costs kmscon about 42 ms, so it accepts 22 frames a second at most, at a full core. On the host, libtsm alone (`tsm-replay` on the recorded stream) spends about 25 ms a frame, most of it in its base64 decoder, which handles a byte at a time and copies three bytes per call into the upload; a pty carries the 15 MB in about 15 ms. So the player sends frames at a copy's own size, as greyscale PNG: nothing to scale, a twentieth of the bytes, and the decoding in the helper's process. That halves kmscon's CPU against 480-wide frames and presents every frame. Both costs are recorded against aaronsb/kmscon#161.

Desktop kitty on the operator's GPU could not be measured here. Under Xvfb (software GL), kitty 0.49.2 at 7680x2160 with a 22x51 cell (349x42 cells) took mpv's recorded stream at about 10 frames a second and the player's native PNG stream at about 90, so those figures say nothing of the operator's kitty. The streams were the same bytes for both terminals: 2235x1672 pixels a frame for mpv's.

## Pacing

The cue sheet, `CUES` at the top of `bad-apple.sh`, sets where each video item starts and stops. Every other timing is a variable at the top of `bad-apple.sh` (seconds unless marked), and each can also be set in the environment for a run from a shell: where each act starts in the video and for how long (`ACT1_START`, `ACT1_SECONDS`, `ACT1_DOTS_SECONDS`, ..., `ACT7_SECONDS`, all taken from the cue sheet unless set), act 7's fades (`ACT7_FADE_IN`, `ACT7_FADE`), frame rates, the act 2 pause, letter speed, plasma and shadebobs (`ACT2_PAUSE`, `ACT2_LETTER`, `ACT2_PLASMA`, `ACT2_BOBS`), act 3's resize (`ACT3_RESIZE=no` keeps the frame; `RESIZE_WAIT`, 1.5 s, is how long it waits for the terminal), act 3's opening line (`ACT3_CPS`, `ACT3_DOTS`, `ACT3_HOLD`), typing speed (`ACT4_CPS`), act 5's size, clip width, dither, glitch, stutter and hold (`ACT5_SIZE`, `ACT5_WIDTH`, `ACT5_DITHER`, `ACT5_GLITCH`, `ACT5_STUTTER`, `ACT5_HOLD`), the fire, the CJK stream and the cats of act 6 (`ACT6_FIRE`, `ACT6_CJK`, `ACT6_CATS`), act 7's moves (`ACT7_SLIDE_AT`, `ACT7_COPY2_AT`, `ACT7_COPY3_AT`, in seconds into the act and taken from the cue sheet unless set; `ACT7_MOVE`, 1.2 s, how long each slide takes; `ACT7_BOUNCE`, 0.12, how far past its place a copy swings, as a share of the slide; `ACT7_GAP`, 2, the columns between copies), its player's transport and frame width (`KITTY_TRANSPORT`, `KITTY_WIDTH`) and a file for its frame rate (`ACT7_REPORT`), the scroller's rows, frame rate and words (`BANNER_ROWS`, `BANNER_FPS`, `BANNER_FEATURES`, `BANNER_EMOJI`, `BANNER_INTRO`, `BANNER_OUTRO`), how long a placeholder stays (`PLACEHOLDER_SECONDS`), how long an act waits for its music cue's sound to start (`CUE_LATENCY`), and an optional starfield title card before act 1 (`TITLE_SECONDS`, 0 by default).

## The effects

Half blocks (▀ with a 24-bit foreground and background) make each cell two pixels, 384x108 at the operator's 384x54 cells; Braille makes each cell 2x4 dots in one colour, 768x216. The painters keep the last frame and send only the cells that changed, with a cursor move only where they skip and SGR only where the colour changes, and each frame is one `write()`. On a resize the effect is rebuilt at the new size.

| `--fx` | Where it plays | Drawn with |
|---|---|---|
| `plasma` | act 2, behind the letters | half blocks; two sine fields and a cycling palette of 64 colours |
| `shadebobs` | act 2, after the plasma | half blocks; blurred blobs on Lissajous paths added into a buffer that fades, brightness piling up where they cross |
| `fire` | the wipe into act 6 | half blocks; the classic fire, its source on an edge climbing the screen, charring what it touches; with `--cjk SECONDS`, CJK lines scroll up the rows the edge has left (a scroll region, three lines a frame) and then fill the screen for that long |
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
| ffmpeg, ffprobe | ffmpeg | acts 1, 3, 5 and 7: decoding, act 5's glitched clip | present (n9.0.2) |
| img2txt | libcaca | act 3 | present (0.99.beta20) |
| mpv | mpv | act 5 (`--vo=sixel`), `--audio`: the soundtrack and the music cues | present (0.41.0; built without the `caca` vo, so act 3 uses `img2txt`) |
| img2sixel | libsixel | not used: on this host it prints nothing for any picture, so act 5 holds mpv's last frame instead | present (1.10.5, with gdk-pixbuf 2.44, whose loader libsixel reads before closing it) |
| toilet | toilet | the block letters of acts 2 and 7 and the title card | present; without it, figlet's `banner` font, then plain text |
| figlet | figlet | fallback for toilet | present |
| chafa | chafa | not used | present |
| mplayer | mplayer | not used (`-vo aa` would need it) | missing |
| rsvg-convert | librsvg | act 6's interlude in kitty graphics: renders the cat faces and sunglasses (`cats/*.svg`) at the screen's size into the cache | present |
| node, npm, chromium | nodejs, npm, chromium | rendering the music cues again (`music/render.mjs`), not the show | present (2026-10-25: node 26.12, chromium) |

The dev image (`fork/dev.Dockerfile`) carries none of the media tools and needs none: the show runs on the host, and `make test` runs only `--list`, `--dry-run`, the refusals, the pure helpers, act 4 on a pty and act 7's player with stand-ins for ffmpeg and ffprobe (python3 alone), quit with q to check the modes it turns off. Pinning them there would mean adding `ffmpeg mpv libcaca toilet` to its `pacman -S` line.

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
| `kitty.py` | act 7's player: the kitty graphics commands, the transports, the copies' layout and moves, the soundtrack's clock |
| `shades.py` | act 6's interlude, the cats in sunglasses: the row's layout, the drops and the slide in kitty graphics, and its emoji fallback |
| `cats/` | the interlude's images: six Noto Emoji cat faces (`emoji_u*.svg`, from googlefonts/noto-emoji `2D/svg`, under `LICENSE-noto-emoji`) and pixel sunglasses (`sunglasses.svg`, the operator's) |
| `music/` | the music cues ([Music](#music)): each cue's Strudel source (`.js`) and its rendering (`.ogg`), and `render.mjs`, which renders them |
| `../showcase.mk` | `make showcase` |

The frame caches go to `~/.cache/kmscon-showcase/<video id>/` (`SHOWCASE_CACHE` moves them), keyed by the video's path, size and time and by each act's settings. A cache made for one grid size is not reused for another: act 1's grids and act 3's framed region shrink in a smaller terminal and render again there.

`tests/showcase/showcase.py`, part of `make test`, checks the act and effect tables, the plan, the refusals and the pure helpers (the painter's diff, Braille dots, the outline table, img2txt's colours, act 5's picture size, the XTWINOPS answers) without a video or any media tool, and quits act 4 on a pty to check that every mouse mode, focus events and bracketed paste are turned off. It checks act 7's player too: the copies' layout and moves, and its output replayed on a model of the spec's image store, from the player alone in every transport and from `fx.py kitty` on a pty with ffmpeg and ffprobe stood in for: q=2 on every command and C=1 on every placement, chunks within the spec, placement ids 1 to 3 at z 3 to 1, c and r as laid out, every frame decoding to the pixels sent (base64, zlib, the PNG's chunks and rows, shared memory), each frame's copies placed before the last frame's are deleted, never more than two images stored, and none left at the end. It also checks the soundtrack's clock against a stand-in mpv and its fallback, the shared memory probe's two answers and the cleanup of unread objects, and that q ends the player while the terminal is behind.
