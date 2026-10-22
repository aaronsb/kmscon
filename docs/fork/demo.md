# Local VT demo

The demo puts a development build of the fork on one of this machine's VTs, so its output can be judged by eye on real hardware. `make demo` builds the package from the checkout and stages it under `fork/build`; `fork/demo/run-vt.sh` stops the stock console on one VT, runs the staged kmscon there with a paged demonstration as its only session, and starts the stock console again when the demonstration ends. Nothing is installed and no system file changes. It adds to the conformance tiers ([conformance.md](conformance.md)); it replaces none of them.

It is written for a host that runs the stock kmscon package on its VTs as `kmsconvt@ttyN.service` (an Arch Linux machine with `autovt@` pointed at kmscon). A VT that runs getty instead is refused only if a session is on it; the script starts no getty afterwards.

## Running it

```sh
make demo                          # build the package and stage it (no root)
sudo fork/demo/run-vt.sh           # VT 3
sudo fork/demo/run-vt.sh 4         # another VT, 2 to 12
fork/demo/run-vt.sh --dry-run      # print every command, run none (no root)
sudo fork/demo/run-vt.sh 3 -- --mode=7680x2160 --no-use-original-mode
                                   # options after -- go to kmscon
```

Then switch to the VT with Ctrl-Alt-F3 (or the VT's number), page with any key, and press q to finish. When the demonstration ends, kmscon exits and the stock console starts again on that VT. Switch back to the desktop with its own Ctrl-Alt-F key.

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

## What `run-vt.sh` does

1. Refuses unless it runs as root, N is from 2 to 12, `make demo` has staged a build, it was started through `sudo` from a user account, and `/usr/lib/kmscon` exists.
2. Refuses the VT on screen (`fgconsole`) and any VT a logind session is on, the desktop's included: stopping the unit there would end that session.
3. Records whether `kmsconvt@ttyN.service` is active and stops it. A trap on exit, Ctrl-C, SIGTERM and SIGHUP stops kmscon with SIGTERM if it still runs, waits for it, and starts the unit again if it was active, so the stock console comes back after a normal end, an interrupt or a kmscon failure.
4. Prints the steps and the manual restore command.
5. Runs the staged kmscon on VT N with `--no-switchvt --oneshot`, the demo config and the demonstration as its session, in the background so that only the script's trap stops it, and waits for it.

`--dry-run` prints each of these commands (and the checks it would make) without running any, and needs no root.

## The demonstration

`fork/demo/demo.sh` shows a title page (`DEMO_LABEL`, `TERM`, the screen's size, the list of sections), then one page per section script in `fork/demo/sections/`, in file name order. Each page starts with the section's label: what a correct screen looks like. Then any key goes to the next page and q ends the demonstration.

It runs in any terminal, as any user, and needs nothing from the dev build: no root, no staging directory. The pages are the same wherever it runs, so it can be shown on a stock kmscon VT beside the dev build (below). `DEMO_LABEL` names the terminal on the title page; `run-vt.sh` sets it to `dev build <git describe>`, and without it the title page says it is unlabelled.

| Section | What it shows | What correct looks like |
|---|---|---|
| sgr | tier 3's `sgr` screen: the 16 colours as text and background, bold, underline, reverse, faint, italic, the 256-colour cube, a 24-bit ramp | every colour distinct, the ramp smooth |
| colour-forms | SGR 38 and 48 in the semicolon forms (`38;5;n`, `38;2;r;g;b`) beside the colon forms (`38:5:n`, `38:2::r:g:b`) | in each group the two rows match swatch for swatch |
| underline-styles | SGR `4:1` to `4:5`, ended by `4:0` and `24`, over colour, bold, reverse and CJK | each line underlined in the shape it names |
| underline-colour | SGR 58 in both forms, with styles, and 59 | the underline in the named colour, the text in its own; 59 gives the line back the text's colour |
| strike | SGR 9 and 29, with colour, underline, reverse and CJK | a line through the middle of each struck run |
| boxdraw | tier 3's `boxdraw` screen: box drawing, shades and blocks | lines join with no gaps |
| cjk | tier 3's `cjk` screen: CJK beside Latin | each CJK character two cells wide; the bar rows line up |
| wide-margin | double-width characters placed and typed at the right margin | each either fits the last two cells or moves to the next line whole; no half character at the edge |
| combining | letters built from a base and combining marks beside the precomposed ones, including `ã̀` (a, U+0303, U+0300) and e + U+0301 | each pair in one cell, matching the precomposed column |
| emoji | a ZWJ family, a flag, a keycap, a skin tone, a VS16 heart and a single emoji | each one picture two cells wide, bars aligned with the reference row |

`sgr`, `boxdraw` and `cjk` run tier 3's screen scripts unchanged (`fork/tier3/guest/screens/`), so the demo shows what tier 3 captures. The `sgr` screen is 96 columns wide; on a screen narrower than that its first two rows wrap. Every other section fits 80 columns by 25 rows with its label and prompt.

The step 2 sections (colour-forms, underline-styles, underline-colour, strike, combining, emoji) can only be judged on the fork. Upstream libtsm discards a whole SGR sequence that holds a colon and reads `58;5;n` with the 5 as blink, so stock kmscon draws those pages wrong by design. The sections name what a finished fork draws: as of this page, libtsm parses the underline styles, underline colour and strike, but the renderer draws every underline style as a single line and draws neither strike nor underline colour until step 2.4 lands its decoration sprites.

## Compare with stock kmscon

Run the dev build on one VT and the same demonstration under stock kmscon on another, then flip between them page by page:

1. `sudo fork/demo/run-vt.sh 3` from the desktop.
2. Ctrl-Alt-F4, log in on the stock kmscon there, and run `DEMO_LABEL="stock $(pacman -Q kmscon)" fork/demo/demo.sh` from the checkout.
3. Ctrl-Alt-F3 shows the dev build; page both to the same page number and flip with Ctrl-Alt-F3 and Ctrl-Alt-F4. The title pages say which is which.

The stock VT draws with `/etc/kmscon/kmscon.conf`, the dev build with `fork/demo/kmscon.conf`, so their fonts and grids can differ, while a fair comparison differs only in libtsm and the renderer. To compare like with like, pass the stock config's display and font settings to the dev build after `--` (for example `-- --font-engine=freetype --font-name="..." --font-size=N --mode=...`); kmscon logs the engine it used in the line `Display ... font engine [...]` (`journalctl -u kmsconvt@tty4`). pango falls back to other fonts for CJK and emoji and freetype does not, so with different engines some of the differences below are the font engine's.

What stock kmscon is expected to get wrong, page by page. The list comes from upstream's code as ADR-100 records it and from the libtsm changes the fork carries; it has not yet been checked side by side.

| Section | Stock kmscon |
|---|---|
| sgr, boxdraw, cjk | Nothing beyond the font: these are tier 3's screens, whose goldens match upstream's behaviour. |
| colour-forms | The colon rows have no colour: upstream libtsm discards a whole SGR sequence that holds a colon, so their text is the default colour and their backgrounds are empty. The colon half of the mixed line is plain. The semicolon rows are right. |
| underline-styles | Only the plain `4` line is underlined. Every line set with `4:n`, including the red, bold, reverse and blue-background ones, is drawn with none of its attributes. |
| underline-colour | No line has a coloured underline. The colon rows have no underline. Upstream does not know 58, so the numbers after it are read as SGR codes of their own: `4;58;5;196` is underlined in the text's colour and blinks (5), and in `4;58;2;0;160;255` the 0 resets the line, which loses its underline. |
| strike | No strike anywhere: upstream has no SGR 9. `9;4` is underlined only, and the line with `4:3` is plain. |
| wide-margin | A wide character placed or typed at the last column is cut at the edge instead of moving to the next line, so case 2, case 3 and the emoji in case 5 show half a character or none. |
| combining | The decomposed column is blank: kmscon draws a cell of more than one code point as nothing. The precomposed column is right except its last row, which is a precomposed letter plus a mark. |
| emoji | Every cluster (family, flag, keycap, skin tone, VS16 heart) is drawn blank or as separate pieces, so its bars do not line up. The single emoji is monochrome at best: kmscon has no colour glyphs. |

## Seen on hardware

The first real run, on 2026-10-22: Arch Linux, one output (DP-8) at 7680x2160, the desktop on tty7, build v10.0.4-150-gbfa7b6e with pango and Monoid Nerd Font Mono at 30 (cell 20x40), on tty5.

- The handoff, the staged modules, the demo running as the user, and the stock unit's return after `q` all worked.
- The operator found these pages correct: sgr, colour forms, underline styles and colours, strike, wide characters at the margin, combining marks and CJK.
- Emoji draw as a clipped corner. The fallback emoji font's bitmaps are larger than the cell, and nothing scales them until colour emoji land (ADR-102 step 4c).
- Stacked block elements show a one-pixel gap between rows (#46).
- Stock kmscon 10.0.4 on that machine never repaints after a switch from the desktop. Its journal repeats `drm_shared: test-only atomic commit failed, -22`, then `vt_timeout: failed to wake up after 20 retries`. The dev build, on newer code, takes the display. The comparison with stock could not be made there, so the table above is still a prediction.
- `run-vt.sh` refuses a VT that has a login on it, and a login on a stock kmscon VT counts as one. Pick a VT nobody is logged into.

## Adding a section

A capability that adds something visible adds its section in the same PR. A section is one file, `fork/demo/sections/NN-name.sh`:

- `NN` places it in the order; `name` is what the title page lists and what `demo.sh name` runs.
- Its `# demo:` lines are the label: one or two lines, under 80 columns, saying what a correct screen looks like, in terms a person can check by eye.
- It draws one screen and nothing else: deterministic output (no clock, no randomness, no dependence on the locale; write invisible code points as UTF-8 bytes, `$'\xcc\x81'`), self-contained so it can be copied into the tier 3 guest, and at most 19 lines (with a two-line label) and 80 columns, so the page fits 80x25 with its header and prompt. A section that must sit at the screen's edge reads the size from the terminal, as `55-wide-margin.sh` does.

Check it outside a VT with `DEMO_NOPAGE=1 fork/demo/demo.sh name`, which runs without waiting for keys and leaves the page on the screen, then on a VT with `run-vt.sh`.

The sections are meant to be tier 3's screens as well (#36): tier 3 would run each section in its guest and compare the capture with a golden, so every demonstration is also a check. Until tier 3 runs them (#22), only `sgr`, `boxdraw` and `cjk` are judged by tier 3.
