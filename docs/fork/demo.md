# Local VT demo

The demo puts a development build of the fork on one of this machine's VTs, so its output can be judged by eye on real hardware. `make demo` builds the package from the checkout and stages it under `fork/build`; `fork/demo/run-vt.sh` stops the stock console on one VT, runs the staged kmscon there with a paged demonstration as its only session, and starts the stock console again when the demonstration ends. Nothing is installed and no system file changes. It adds to the conformance tiers ([conformance.md](conformance.md)); it replaces none of them.

It is written for a host that runs the stock kmscon package on its VTs as `kmsconvt@ttyN.service` (an Arch Linux machine with `autovt@` pointed at kmscon). A VT that runs getty instead is refused only if a session is on it; the script starts no getty afterwards.

## Running it

One command builds, stages, picks a VT, runs the demo there and, when it ends, prints what was recorded:

```sh
make vt-demo                       # the first free VT of 5, 6, 4, 3, 2, the text kit
make vt-demo VT=4                  # that VT
make vt-demo KIT=sixel             # the sixel review kit (see "Review kits")
make vt-demo DEMO_ARGS='--font-name="Monoid Nerd Font Mono" --font-size=30 --mode=7680x2160 --no-use-original-mode'
                                   # options for kmscon
```

It asks for sudo's password first if sudo wants one, then prints the VT to switch to and the key back:

```
================================================================
  Switch to VT 5 now: Ctrl-Alt-F5    (Ctrl-Alt-F7 returns to this desktop)
================================================================
```

Switch to that VT, judge each page with one key (below), and press q to finish. The command blocks until then. When the demonstration ends, kmscon exits, the stock console starts again on that VT, and the command prints a health check of kmscon's log, the path of the full log, this run's answers as `fork/demo/feedback.sh --run` prints them, and the path of the results file. It exits non-zero when kmscon failed (with `run-vt.sh`'s status, 130 after Ctrl-C) or nothing was recorded.

`VT`, `KIT` and `DEMO_ARGS` reach the script through the environment, never as recipe text. `DEMO_ARGS` is split into words the way `xargs` splits them: quotes and backslashes are honoured and nothing is expanded or run, so a value with spaces is quoted inside it, as the font name is above. Each option is one word, `--name=value` or `--name` / `--no-name`; `run-vt.sh` refuses any other form (see below). `VT_DEMO_DRY_RUN=1 make vt-demo` builds and stages, picks the VT and prints the `run-vt.sh` command without running it.

`make showcase BAD_APPLE=/path/to/video` runs the Bad Apple!! showcase the same way, with `fork/showcase/bad-apple.sh` as the session in place of the paged demo; it records no answers, so its run ends with the health check alone. See [fork/showcase/README.md](../../fork/showcase/README.md).

### By hand

`make vt-demo` runs `fork/demo/run-vt.sh`, which can also be run directly:

```sh
make demo                          # build the package and stage it (no root)
sudo fork/demo/run-vt.sh           # VT 3
sudo fork/demo/run-vt.sh 4         # another VT, 2 to 12
fork/demo/run-vt.sh --dry-run      # print every command, run none (no root)
sudo fork/demo/run-vt.sh 3 -- --mode=7680x2160 --no-use-original-mode
                                   # options after -- go to kmscon
```

Then switch to the VT with Ctrl-Alt-F3 (or the VT's number), judge each page, and press q to finish. When the demonstration ends, kmscon exits and the stock console starts again on that VT. Switch back to the desktop with its own Ctrl-Alt-F key, and read the answers with `fork/demo/feedback.sh`.

If anything goes wrong and the stock console does not come back, start it by hand:

```sh
sudo systemctl start kmsconvt@tty3.service
```

## What `make demo` does

It runs `make pkg` (the package is built and its test suite run in the dev image, as for tier 3), then extracts `kmscon-aaronsb` and `kmscon-aaronsb-terminfo` into `fork/build/demo-root`, which is gitignored, and writes `fork/build/demo-root/BUILD` with the checkout's `git describe` and the package version. `run-vt.sh` prints that `git describe` and hands it to the demonstration, whose title page shows it.

The extracted binary runs on the host as it is: libtsm is linked statically, and every other library it and its modules need (`ldd`) comes from packages the stock kmscon already depends on.

## How the staged build finds its pieces

| Piece | Where kmscon looks | What the demo does |
|---|---|---|
| Backend modules (pango, freetype, gltex, drm3d, ...) | `/usr/lib/kmscon`, compiled in (`BUILD_MODULE_DIR`); no option or environment variable changes it | kmscon runs in a private mount namespace (`unshare --mount`) in which `fork/build/demo-root/usr/lib/kmscon` is bind-mounted over `/usr/lib/kmscon`. The rest of the system never sees the mount, and it ends with kmscon. Without it the fork's binary would load the stock package's modules, which are built against a different tree. |
| Config | `--configdir`, default `/etc/kmscon` | `--configdir fork/demo`, so `fork/demo/kmscon.conf` is read and `/etc/kmscon` is not. |
| Fonts | fontconfig, through the pango module | the host's fonts. `kmscon.conf` asks for DejaVu Sans Mono at size 24; pango falls back to other installed fonts for CJK and emoji. |
| The session | `--login -- argv`, started with `execve` | `/usr/bin/setpriv` drops to the invoking user (`$SUDO_USER`), then `env` sets `DEMO_LABEL="dev build <git describe>"` and runs `/bin/bash fork/demo/demo.sh`; the demonstration never runs as root. |

Display settings belong to one machine, so they are not in `fork/demo/kmscon.conf`. Pass them after `--` (for example the `mode=` and `use-original-mode` lines of `/etc/kmscon/kmscon.conf`, as `--mode=...` and `--no-use-original-mode`), together with `--font-size=N` for a different size.

## What `make vt-demo` does

1. Runs `make demo`.
2. Picks the VT: `VT=` if given, else the first free one of 5, 6, 4, 3 and 2. A VT is free when it is not on screen (`/sys/class/tty/tty0/active`) and no logind session is on it (`loginctl`). It refuses a `VT=` that is not free, and when no VT is free it fails naming each VT and why it was refused. `run-vt.sh` checks the same again before it stops anything.
3. Reads the desktop's VT, the one on screen at the start, for the key back.
4. Runs `sudo -v`, prints the line naming the VT, and runs `sudo fork/demo/run-vt.sh [--kit=KIT] N -- DEMO_ARGS` in the foreground, its output and kmscon's log copied to a new file, `/tmp/kmscon-demo/vt-demo-<time>-ttyN-<random>.log` (mode 0600). `/tmp/kmscon-demo` is created with mode 0700 when it is missing, and refused when it is a symlink or another user's. Ctrl-C, a closed terminal or SIGTERM reaches `run-vt.sh`, whose trap stops kmscon and restarts the stock console; the copy (`tee`) ignores those signals, so the restore never writes into a closed pipe. After Ctrl-C the summary still follows.
5. Counts `atomic commit failed` and `vt_timeout` lines in that log, the stock-kmscon freeze signature (see "Seen on hardware").
6. Finds this run by its run id: the last run of a `dev build` label among the answers appended to `/tmp/kmscon-demo/feedback.tsv` since step 4, so a stock run on another VT writing to the same file meanwhile is not taken for it. It prints that run with `feedback.sh --run`.

The script is `fork/demo/vt-demo.sh [N] [-- KMSCON-OPTION...]`; `tests/demo/vt_demo.py`, part of `make test`, runs it against a planted VT on screen, a stand-in `loginctl` and a stand-in `run-vt.sh`.

## What `run-vt.sh` does

1. Refuses unless it runs as root, N is from 2 to 12, `make demo` has staged a build, it was started through `sudo` from a user account, and `/usr/lib/kmscon` exists. It also refuses every kmscon option after `--` but display, font, palette, text and input ones and `--render-timer`, named in full (kmscon takes a unique prefix, so `--lo` would be `--login`), each as one word: `--name=value`, or `--name` / `--no-name` for a switch. A value in a word of its own would be taken as the option's value or, as a stray word, be moved by kmscon into the login command that runs as root. Left out on purpose: the VT, seat and session options, `--login`, `--configdir`, `--oneshot`, the grabs, and the options that name files (`--asciicast`, `--issue-path`, `--xkb-keymap`, `--xkb-compose-file`). The list is `valued` and `switches` in `run-vt.sh`. Before N, `--kit=KIT` picks the demo's review kit (text, sixel, kitty or all; any other name is refused), which reaches the demo as `DEMO_KIT`, one word of `env`'s after `setpriv`; and `--showcase=VIDEO` (an absolute path the user can read) makes the session the showcase on that video, with `--showcase-from=N` (an act from 1 to 7) and `--showcase-audio`; it refuses either of those without `--showcase`, and `--kit` with it.
2. Refuses the VT on screen (`fgconsole`) and any VT a logind session is on, the desktop's included: stopping the unit there would end that session.
3. Records whether `kmsconvt@ttyN.service` is active and stops it. A trap on exit, Ctrl-C, SIGTERM and SIGHUP stops kmscon with SIGTERM if it still runs, waits for it (SIGKILL after 5 seconds), and starts the unit again if it was active, so the stock console comes back after a normal end, an interrupt or a kmscon failure. While it restores, it ignores Ctrl-C, SIGTERM, SIGHUP and SIGPIPE, so a second Ctrl-C or a closed terminal cannot cut it short before the unit is started.
4. Prints the steps and the manual restore command.
5. Runs the staged kmscon on VT N with `--no-switchvt --oneshot`, the demo config and the demonstration (or the showcase, which `env` also hands `BAD_APPLE` and the user's `XDG_RUNTIME_DIR`) as its session, in the background so that only the script's trap stops it, and waits for it.

`--dry-run` prints each of these commands (and the checks it would make) without running any, and needs no root.

## The demonstration

`fork/demo/demo.sh` shows a title page (`DEMO_LABEL`, `TERM`, the screen's size, where the answers go, the list of sections), then one page per section script in `fork/demo/sections/`, in file name order, then a summary page. Below each page a footer gives the section's guidance and asks for a judgement (#47):

```
right:    Each line underlined in the shape it names, all distinct: single,
          double, curly, dotted, dashed; 4:0 and 24 end it mid-line.
wrong:    Every style drawn as one single line, or a line with none of its
          attributes: on stock kmscon only the plain 4 line is underlined.
 1 right  2 wrong  3 unsure  s skip  c comment  r redraw  b back  q quit
```

| Key | What it does |
|---|---|
| `1` | the page looks right; records it and goes to the next page |
| `2` | the page looks wrong |
| `3` | unsure: the page was looked at and the reviewer cannot tell |
| `s` | skipped: the page is not being reviewed this time. It is recorded as `skipped`, apart from unsure, and listed again as still to review on the summary page, by `feedback.sh` and at the end of `make vt-demo` |
| `c` | opens a comment below the page, as many lines as needed, each with line editing; on a full page each line scrolls the page up by one. Ctrl-D on an empty line, or a line holding only `.`, ends it. Then one key rates the page (`1`, `2`, `3`, `s`, or Enter for no rating, recorded as `none`) and the next page follows. An empty comment goes back to the page. Ctrl-D never ends the demonstration. |
| `r` | draws the page again from the start and records nothing: for a page whose keys changed kmscon's state, such as sixel-evict's turned screen, before trying its keys again |
| `b` | back one page; a new answer there replaces the old one on the summary page and in `feedback.sh`, and the file keeps both lines |
| `q` | quits to the summary page |

Any other key asks again. Keys typed before the footer appears are discarded, so a double press does not rate the next page unseen. The footer sits below the page when it fits there: demo.sh asks the terminal for the cursor's row (DSR) and compares it with the screen's height. When it does not fit, or the terminal does not answer within a second, a key shows the footer on a screen of its own. At 80x25 every section fits with its footer; the tallest, colour-forms, ends its prompt on row 25.

The summary page lists this pass's rating and comment for each section, how many pages were skipped, and the file they went to, then asks `r review again from page 1  q exit`. `r` goes through the pages again as a new run, with its own run id, so `feedback.sh` shows that pass as the latest run and its summary page shows only that pass.

### Review kits

The pages fall into review kits (#123), so a visual review runs the pages a change can affect. A section's `# kit:` line names its kit; a section without one is in the text kit. `make vt-demo KIT=NAME` (or `DEMO_KIT=NAME` for demo.sh) picks one:

| Kit | Pages |
|---|---|
| `text`, the default | every page from sgr to emoji: text, colour, decorations, box drawing, CJK, combining marks, emoji |
| `sixel` | sixel, sixel-ramps and sixel-article, then ADR-103's acceptance pages: photos, photo-scroll, mpv-sixel, tmux-2x2 and tmux-3x3; and sixel-evict, whose section, numbered before the two sides met, sorts between photos and photo-scroll |
| `kitty` | kitty, kitty-photo, kitty-files and kitty-layers: the kitty graphics protocol through a small sender the pages hold, written from the published spec (`kitty-common.inc`, ADR-300), ADR-103 step 4c. kitty-files needs a logged-in session to show its pictures ([File media: the login run](#file-media-the-login-run)); 4d adds the acceptance pages (viu, kitten icat, mpv) |
| `all` | every page |

The title page names the kit. Sections named on demo.sh's command line run whatever their kit. The acceptance pages run real programs on the host: chafa and mpv for the photograph, mpv for the video and tmux for the grids; the photograph itself is in the tree, beside the pages. A page whose program is missing says what to install and shows nothing else. The video pages print what was measured under the page (mpv's frames shown and dropped, kmscon's CPU time); with `--render-timer=osd` or `both` in `DEMO_ARGS` kmscon's own figures show in a corner while they play.

### Where the answers go

Each answer appends one line to `$DEMO_FEEDBACK`, by default `/tmp/kmscon-demo/feedback.tsv`. `/tmp` is cleared on reboot, so copy the file elsewhere or set `DEMO_FEEDBACK` to keep it longer. The directory is created with mode 0755 and the file with mode 0644 and a header line, owned by whoever runs the demo. The dev build's demo runs as the invoking user (through `setpriv`), and the stock run in the comparison below is typed by the same user, so both runs append to the same file. If the file cannot be written, the title and summary pages say so and the demonstration runs without recording.

| Column | Value |
|---|---|
| `time` | ISO 8601 time of the answer |
| `run` | the start time of the pass and demo.sh's PID, the same for every line of one pass; `r` on the summary page starts a new one |
| `label` | `DEMO_LABEL`, or `unlabelled` |
| `term` | `TERM` |
| `size` | rows x columns, such as `25x80` |
| `section` | the section's file name without `.sh`, such as `20-underline-styles` |
| `rating` | `right`, `wrong`, `unsure`, `skipped`, or `none` for a comment without a rating |
| `note` | the comment, with each line break written as `\n`, each backslash as `\\` and tabs as spaces; empty without one |

`fork/demo/feedback.sh` prints the file as tables, one line per section with the last answer given for it and that answer's comment below it, one indented line per line typed. The sections whose last answer is `skipped` follow under `skipped, still to review`:

```sh
fork/demo/feedback.sh                  # the latest run
fork/demo/feedback.sh --run ID         # one run, by its run column
fork/demo/feedback.sh --compare        # the latest run of each DEMO_LABEL, side by side
fork/demo/feedback.sh --file PATH ...  # another file (default: $DEMO_FEEDBACK, then /tmp)
```

`DEMO_NOPROMPT=1` shows the guidance below each page and asks nothing: any key goes to the next page, q ends the demonstration, and nothing is recorded or created. It is for tier 3 and for testing. `DEMO_NOPAGE=1` implies it and waits for no key.

`tests/demo/feedback.py`, part of `make test`, drives demo.sh through scripted keys on a pseudo-terminal of 80x25 and checks the lines it records and what `feedback.sh` prints.

It runs in any terminal, as any user, and needs nothing from the dev build: no root, no staging directory. The pages are the same wherever it runs, so it can be shown on a stock kmscon VT beside the dev build (below). `DEMO_LABEL` names the terminal on the title page; `run-vt.sh` sets it to `dev build <git describe>`, and without it the title page says it is unlabelled.

What each page should and should not look like is in its section file's `# look-for:` and `# wrong-looks-like:` lines, which the footer shows.

| Section | What it shows |
|---|---|
| sgr | the 16 colours as text and background, bold, underline, reverse, faint, italic, the 256-colour cube, a 24-bit ramp |
| colour-forms | SGR 38 and 48 in the semicolon forms (`38;5;n`, `38;2;r;g;b`) beside the colon forms (`38:5:n`, `38:2::r:g:b`) |
| underline-styles | SGR `4:1` to `4:5`, ended by `4:0` and `24`, over colour, bold, reverse and CJK |
| underline-colour | SGR 58 in both forms, with styles, and 59 |
| strike | SGR 9 and 29, with colour, underline, reverse and CJK |
| blink | SGR 5 beside steady text: plain, coloured, on a background, reversed, bold, underlined, curly, struck, CJK and combining, and a line that SGR 25 makes steady halfway |
| boxdraw | box drawing, shades and blocks |
| blocks | block elements and box lines stacked across rows and columns (tier 2's `blocks` screen as well), which show the line spacing gap of #46 if the font draws them |
| mosaic | every Braille pattern, sextant and octant, then checkerboards, a dot grid, a wave and bands of them stacked across cells (tier 2's `mosaic` screen as well) |
| cjk | CJK beside Latin |
| wide-margin | double-width characters placed and typed at the right margin |
| combining | letters built from a base and combining marks beside the precomposed ones, including `ã̀` (a, U+0303, U+0300) and e + U+0301 |
| emoji | a ZWJ family, a flag, a keycap, a skin tone, a VS16 heart and a single emoji, each between bars; the text heart alone and with VS15; emoji in a line, underlined and reversed |
| sixel | sixel pictures the page makes from the cell size the terminal reports (XTWINOPS 16) and sends through the pty, each a whole number of cells: opaque colour bars over a checkerboard with TEXT written across them, an orange ring with transparent corners over text, and seven discs in the primaries, the secondaries and the ring's orange, each with a square of the same colour drawn as text (SGR 48;2) beside it and its name under both; then the same pairs as larger squares on a mid-grey band, and the cell size. A disc unlike its square is a kmscon fault; a pair off alike is the display (a colour profile, or dimming of a small bright patch on black). Without an XTWINOPS answer the pictures are made for 12x24 cells and the page says so. When the DA1 reply lacks attribute 4, the page says so and sends no picture |
| sixel-ramps | sixel ramps of red, green, blue and grey from black to full, a hue sweep and skin-tone and pastel swatches, each above the same colours drawn as text cells (SGR 48;2), so each sixel step can be compared with the text cell under it; then a ring with a transparent hole over text on yellow. Sixel colours are whole percents: a ramp has 101 steps of 1% when the screen has the columns and the terminal the registers (XTSMGRAPHICS), fewer otherwise, and the page says so |
| sixel-article | a short article the script word-wraps itself to the screen's width (up to 100 columns): prose whose phrases are styled as they describe themselves (bold, italic, faint, the underline styles, a red underline, strike-through, reverse, 256-colour and truecolour, a CJK word, a combining mark, an emoji), a landscape centred on rows of its own, and a bar chart floated left with the last paragraph wrapping beside it and then at full width. The sentences about each picture give its size and place in cells and pixels from the reported cell size |
| sixel-evict | the image budget under pressure (ADR-103's Q1 note), a page that asks for keys before it is rated: twelve or more pictures, each a band the screen's width under a caption, that fill `image-storage-limit=auto` for the terminal's size, the last on screen and the rest in scrollback. Logo+= turns the screen, and the last picture's turned copy fits only by evicting picture 1. Logo+- turns it back, and Shift+PageUp to the top then shows picture 1's caption over blank cells and picture 2 onward drawn. The pictures fill the auto limit, so with `image-storage-limit` set nothing is evicted. Tier 3 presses the same keys (`sixel-evict-rotated`, `-history` and `-zoomed`) |
| photos | ADR-103's A1: the operator's photograph of a street at sunset (CC BY 4.0, `fork/tier3/guest/screens/photo/LICENSE`), drawn twice side by side for comparison, chafa on the left and libsixel's encoder (through mpv) on the right, eight rows tall and named under each copy. Then how many bytes img2sixel wrote for it: img2sixel writes nothing on current Arch (libsixel 1.10.5 with gdk-pixbuf 2.44) |
| photo-scroll | A1's second half: the photograph, from chafa, under a caption, then one numbered line per screen row, so the picture scrolls into scrollback. Shift+PageUp shows it again above line 1, and Shift+PageDown returns |
| mpv-sixel | ADR-103's A2: mpv --vo=sixel plays ffmpeg's testsrc2 pattern for five seconds in a 1920x1080 window in the middle of the screen (the largest 16:9 window that fits, on a smaller screen) without dithering, then the page comes back with nothing of the video left and prints what was measured, mpv's frames sent and dropped in bold. Full screen at 8K, mpv's encoder, not kmscon, would set the rate (about 4.4 frames a second) |
| tmux-2x2, tmux-3x3 | ADR-103's A4: tmux in a 2x2 and a 3x3 grid, each pane playing the pattern through mpv --vo=sixel without dithering, on a tmux server of the page's own whose config names kmscon's sixel support; then the page on a cleared screen and what was measured, mpv's frames sent and dropped in bold. The frame rate is reported, not judged (ADR-103's A4): tmux and mpv set it, and at 7680x2160 under half of the panes' frames reach the screen, with tmux itself the limit at 3x3. The page itself closes any DCS tmux left open, leaves the alternate screen, shows the cursor and clears the screen and the scrollback, since tmux can exit without restoring the terminal (#154) |
| kitty | the kitty graphics protocol through the pty (ADR-103 step 4c), its three inline media in its three bands, each picture scaled into 20x5 cells under or over a row of blue, plain and green cells: 32x16 RGBA (f=32) that libtsm decodes, below cell backgrounds, its right half at half alpha; a PNG disc that the image helper decodes, below text; zlib-compressed RGB stripes (f=24, o=z) that the helper inflates, above text. Then image 1 placed again at 4x2 cells with a=p, which moves the cursor past it. Inline media only: the demo's session is in no logind session of its own, so kmscon reads no file or shared memory for it ([image-helper.md](../image-helper.md)), and PNG and zlib decode as the pre-login user. When the terminal does not answer the protocol's query, the page says so and sends no picture |
| kitty-photo | the photograph of photos, scaled to 320x240 and saved as PNG (`photo/street-sunset-320.png`), sent inline (t=d, f=100) in 4096-byte chunks and drawn in 20x8 cells, with the terminal's reply (`i=1;OK`) under it |
| kitty-files | the same PNG read by kmscon's image helper from its file (t=f), from a copy in `/tmp/tty-graphics-protocol-*` (t=t) and from a POSIX shared memory object (t=s), side by side, each with the terminal's reply and whether the temporary file was deleted and the object unlinked; then `/etc/shadow`, which only root can read, named with t=f and answered `EBADF:Failed to read image file`. In the VT demo all four replies are EBADF and the page says why: the demo's session is in no logind session of its own, which kmscon treats as before login and reads no file for ([image-helper.md](../image-helper.md)); the page removes the file and the object itself. To see the pictures, run it in a login session ([File media: the login run](#file-media-the-login-run)). The page refuses to run as a user who can read `/etc/shadow` |
| kitty-layers | three rows of text, then one image placed three times by placement id: under the text (z=-1), over it (z=1), and a third placement deleted with `a=d,d=i,p=3`, whose cells must show the words on black and no pixel of the picture |

Every section but blink is also a tier 3 screen, so the demo shows what tier 3 captures and judges. The screen script in `fork/tier3/guest/screens/` is the one source: colour-forms, underline-styles, underline-colour, strike, blocks, mosaic, wide-margin, combining, emoji, sixel, sixel-ramps, sixel-article, sixel-evict, photos, photo-scroll, mpv-sixel, the tmux grids and the kitty pages are symlinks to their screen scripts, which carry the guidance lines; sgr, boxdraw and cjk are wrappers that hold the guidance and run the screen script. The `sgr` screen is 96 columns wide; on a screen narrower than that its first two rows wrap.

blink is not a tier 3 screen: tier 3's guest runs kmscon with `no-blink`, and a capture would catch whichever phase the timer was in. The demo leaves blinking on, kmscon's default, since a person watches it: the blink page blinks and so does the cursor. `-- --no-blink` turns it off for one run. `tests/test_blink.c`, part of `make test`, checks both phases through both of bbulk's buffers.

The step 2 sections (colour-forms, underline-styles, underline-colour, strike, combining, emoji) can only be judged on the fork. Upstream libtsm discards a whole SGR sequence that holds a colon and reads `58;5;n` with the 5 as blink, so stock kmscon draws those pages wrong by design. The sections name what a finished fork draws, and the fork draws every one of them that way. Emoji need a colour emoji font installed (Noto Color Emoji is the common one); without one they draw as the text font's missing-glyph box, fitted to their cells.

## File media: the login run

kmscon reads a file, temporary file or shared memory object for a program only when the program's session is a logind session of its own, of a login class ([image-helper.md](../image-helper.md)). `make vt-demo` starts the demo through `setpriv`, outside any such session, so there the kitty-files page shows four EBADF replies and a note, by design. To see the file media draw, run that page on a VT you logged in on normally, under the fork's kmscon installed as the system's (the operator's choice, 2026-10-24, ADR-103's considered entries):

1. Build the packages from the checkout: `make pkg`. They land in `fork/build/pkg`.
2. Install them over the system's kmscon: `sudo pacman -U fork/build/pkg/kmscon-aaronsb-<version>-1-x86_64.pkg.tar.zst fork/build/pkg/kmscon-aaronsb-terminfo-<version>-1-x86_64.pkg.tar.zst`, with the version `make pkg` printed. They provide and conflict with `kmscon` and `kmscon-terminfo`, so pacman offers to remove Arch's packages, which is what this replaces: `/usr/bin/kmscon`, its modules and `kmscon-image-helper` in `/usr/lib/kmscon`, `kmsconvt@.service`, and the terminfo entry. The install also runs `systemd-sysusers`, which creates the `kmscon-image` system user from `/usr/lib/sysusers.d/kmscon-image.conf`.
3. Start a kmscon VT if none runs: `sudo systemctl start kmsconvt@tty5.service` (stop `getty@tty5.service` first if it holds that VT). A kmscon VT already running keeps the old binary until restarted: `sudo systemctl restart kmsconvt@tty5.service`.
4. Ctrl-Alt-F5, log in, and run `fork/demo/demo.sh kitty-files` from the checkout. Its look-for and wrong-looks-like lines are the same as in the VT demo; logged in, the three photographs draw and the three file media answer OK.

To undo it, put Arch's packages back with `sudo pacman -S kmscon kmscon-terminfo` (pacman offers to remove the fork's), and restart or stop the kmscon VT you started. The `kmscon-image` user stays; `sudo userdel kmscon-image` removes it, since it owns no files.

## Compare with stock kmscon

Run the dev build on one VT and the same demonstration under stock kmscon on another, then flip between them page by page:

1. `make vt-demo VT=3` (or `sudo fork/demo/run-vt.sh 3`) from the desktop.
2. Ctrl-Alt-F4, log in on the stock kmscon there, and run `DEMO_LABEL="stock $(pacman -Q kmscon)" fork/demo/demo.sh` from the checkout.
3. Ctrl-Alt-F3 shows the dev build; page both to the same page number and flip with Ctrl-Alt-F3 and Ctrl-Alt-F4. The title pages say which is which.
4. Rate each page on both VTs. Both runs append to the same `/tmp/kmscon-demo/feedback.tsv` under their own labels, and `fork/demo/feedback.sh --compare` puts them side by side.

The stock VT draws with `/etc/kmscon/kmscon.conf`, the dev build with `fork/demo/kmscon.conf`, so their fonts and grids can differ, while a fair comparison differs only in libtsm and the renderer. To compare like with like, pass the stock config's display and font settings to the dev build after `--` (for example `-- --font-engine=freetype --font-name="..." --font-size=N --mode=...`); kmscon logs the engine it used in the line `Display ... font engine [...]` (`journalctl -u kmsconvt@tty4`). pango falls back to any installed font for CJK and freetype only to monospaced ones (both take emoji from a colour emoji font), so with different engines some of the differences below are the font engine's.

What stock kmscon is expected to get wrong, page by page. The list comes from upstream's code as ADR-100 records it and from the libtsm changes the fork carries; it has not yet been checked side by side.

| Section | Stock kmscon |
|---|---|
| sgr, boxdraw, cjk | Nothing beyond the font: their tier 3 goldens match upstream's behaviour. |
| blocks | Every block and box line comes from the font, so wherever the font's line height has spacing beyond its glyphs, a dark line runs between rows of blocks and along vertical lines. With a font that has none, no gap shows, but line weights, junctions and shades are the font's own. |
| mosaic | Every Braille pattern, sextant and octant comes from the font: where the font has them, Braille dots and blocks sit inside the font's glyph box rather than on the cell's grid, and with line spacing the checkerboards and bands show a dark line between rows; where it has none, they are missing-glyph boxes or come from a fallback font with other metrics. |
| colour-forms | The colon rows have no colour: upstream libtsm discards a whole SGR sequence that holds a colon, so their text is the default colour and their backgrounds are empty. The colon half of the mixed line is plain. The semicolon rows are right. |
| underline-styles | Only the plain `4` line is underlined. Every line set with `4:n`, including the red, bold, reverse and blue-background ones, is drawn with none of its attributes. |
| underline-colour | No line has a coloured underline. The colon rows have no underline. Upstream does not know 58, so the numbers after it are read as SGR codes of their own: `4;58;5;196` is underlined in the text's colour and blinks (5), and in `4;58;2;0;160;255` the 0 resets the line, which loses its underline. |
| strike | No strike anywhere: upstream has no SGR 9. `9;4` is underlined only, and the line with `4:3` is plain. |
| blink | Upstream blinks too, drawing a blinking cell as a space in the hidden phase. The `5;9` line blinks without a strike, the `5;4:3;58:5:196` line is plain and steady (the colon voids the whole sequence), and the combining line is blank. Stock's own config decides whether anything blinks (`blink` is on by default). |
| wide-margin | A wide character placed or typed at the last column is cut at the edge instead of moving to the next line, so case 2, case 3 and the emoji in case 5 show half a character or none. |
| combining | The decomposed column is blank: kmscon draws a cell of more than one code point as nothing. The precomposed column is right except its last row, which is a precomposed letter plus a mark. |
| emoji | Every cluster (family, flag, keycap, skin tone, VS16 heart) is drawn blank or as separate pieces, so its bars do not line up. Each emoji is the grey top-left corner of the colour font's large bitmap, cut at its cells: kmscon has no colour glyphs and does not scale them. |
| sixel | No pictures: stock kmscon's DA1 reply has no attribute 4, so the page says so and sends none. Sent anyway, a sixel string would be skipped. |
| sixel-ramps to tmux-3x3 | The same: each page says the terminal does not offer sixel graphics and shows nothing else. |

## Seen on hardware

The first real run, on 2026-10-22: Arch Linux, one output (DP-8) at 7680x2160, the desktop on tty7, build v10.0.4-150-gbfa7b6e with pango and Monoid Nerd Font Mono at 30 (cell 20x40), on tty5.

- The handoff, the staged modules, the demo running as the user, and the stock unit's return after `q` all worked.
- The operator found these pages correct: sgr, colour forms, underline styles and colours, strike, wide characters at the margin, combining marks and CJK.
- Emoji draw as a clipped corner. The fallback emoji font's bitmaps are larger than the cell, and nothing scales them. ADR-102 step 4c draws them in colour, fitted to their cells, on both font engines; that is not yet checked on this machine.
- Stacked block elements show a one-pixel gap between rows (#46).
- Stock kmscon 10.0.4 on that machine never repaints after a switch from the desktop. Its journal repeats `drm_shared: test-only atomic commit failed, -22`, then `vt_timeout: failed to wake up after 20 retries`. The dev build, on newer code, takes the display. The comparison with stock could not be made there, so the table above is still a prediction.
- `run-vt.sh` refuses a VT that has a login on it, and a login on a stock kmscon VT counts as one. Pick a VT nobody is logged into; `make vt-demo` picks one.

## Adding a section

A capability that adds something visible adds its section in the same PR. A section is one file, `fork/demo/sections/NN-name.sh`, normally a symlink to the tier 3 screen script `fork/tier3/guest/screens/name.sh` that holds it:

- `NN` places it in the order; `name` is what the title page lists and what `demo.sh name` runs.
- A `# kit:` line names its review kit, `sixel` or `kitty`; a section without one is in the text kit. `tests/demo/feedback.py` fails a section naming any other kit, and holds the list of each kit's pages.
- Its guidance is two comment fields, each one or more lines that demo.sh joins with spaces, in terms a person can check by eye. `# look-for:` says what a correct screen looks like; `# wrong-looks-like:` says what a defect looks like, naming what stock kmscon is known to get wrong and any defect the fork already knows about (with its issue), so that one is not reported again. Keep each to about 130 characters of ASCII, so it wraps to two rows of the footer at 80 columns. `tests/demo/feedback.py` fails a section without both fields.
- It draws one screen and nothing else: deterministic output (no clock, no randomness, no dependence on the locale; write invisible code points as UTF-8 bytes, `$'\xcc\x81'`), self-contained so it can be copied into the tier 3 guest, and at most 17 rows and 80 columns, so the page fits 80x25 with its header and a footer of two-row guidance. A taller page still works: its footer moves to a screen of its own. A section that must sit at the screen's edge reads the size from the terminal, as `55-wide-margin.sh` does.
- It draws the same below demo.sh's two-line header as on tier 3's clear screen, where it starts on row 1. A section that places anything with CUP counts its rows from the row it starts on, as the photo pages do with `top_row` (`sixel-common.inc`). `tests/demo/sixel_placement.py`, part of `make test`, runs each sixel page both ways, with stand-ins for chafa and mpv, and fails when the two differ by more than the header's two rows, or when a page that keeps its pictures apart from its text draws one over the other; the video and tmux pages are left to tier 3.

Check it outside a VT with `DEMO_NOPAGE=1 fork/demo/demo.sh name`, which runs without waiting for keys and leaves the page and its guidance on the screen, then on a VT with `make vt-demo`.

Each section is a tier 3 screen as well (#36): tier 3 runs it in its guest and compares the capture with a golden, so every demonstration is also a check. A new section adds its name to `SCREENS` in `fork/tier3/t3_evaluate.py`, before `cjk`, which stays last because `vt-restore` is compared with its golden (`test_t3.py` fails otherwise), and its goldens with `make tier3-bless`, in a commit of their own. Its `# look-for:` and `# wrong-looks-like:` lines are also its case's caption on the run page. Tier 3 does not judge blink, which it runs without.
