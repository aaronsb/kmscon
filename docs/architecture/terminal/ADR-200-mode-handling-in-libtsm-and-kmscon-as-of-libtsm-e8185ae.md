---
contract: adr/v1
kind: evidence
capability: protocols
status: accepted
date: 2026-10-23
deciders:
  - aaronsb
related: []
---

# ADR-200: Mode handling in libtsm and kmscon as of libtsm e8185ae

## Scope and method

Where the fork's two repositories set, reset, report and depend on terminal modes (SM/RM and DECSET/DECRST), read from source on 2026-10-23 at libtsm `e8185ae` and kmscon `896f44c`, whose `subprojects/libtsm.wrap` pins libtsm `75a3ae1`; the mode code is the same at both libtsm commits. Line numbers are libtsm's `src/tsm/tsm-vte.c` unless another file is named. ADR-201 decides what changes; this record says what is there.

## The modes libtsm recognises

`csi_mode` (`:1836-2089`) is one function holding two `switch` statements: ANSI modes when the sequence has no `?` (`:1841-1870`), DEC private modes otherwise (`:1873-2087`). SM (`h`) and RM (`l`) both call it (`:2428-2433`). A number neither switch names is logged at debug level and dropped (`:1868`, `:2084`).

It recognises 32 numbers: 4 ANSI and 28 DEC. Of these, 23 change behaviour and 9 are accepted with no effect.

| Mode | Kind | Name | Case | State held in | Effect | Reset by RIS and DECSTR |
|---|---|---|---|---|---|---|
| 2 | ANSI | KAM | `:1845` | vte flag | none; the flag is never read | yes |
| 4 | ANSI | IRM | `:1849` | vte flag and screen `TSM_SCREEN_INSERT_MODE` | insert mode | yes |
| 12 | ANSI | SRM | `:1859` | vte flag | reset echoes the host's own output locally (`:806-810`) | yes, to set |
| 20 | ANSI | LNM | `:1863` | vte flag | LF, VT and FF also return (`:1147`); Return sends CR LF (`:3570`) | yes |
| 1 | DEC | DECCKM | `:1877` | vte flag | cursor keys send SS3 (`:3334`, `:3730`, `:3737`) | yes |
| 2 | DEC | DECANM | `:1880` | none | none; VT52 is not emulated, by the comment's choice | n/a |
| 3 | DEC | DECCOLM | `:1887` | none | none; a TODO | n/a |
| 4 | DEC | DECSCLM | `:1902` | none | none | n/a |
| 5 | DEC | DECSCNM | `:1908` | vte flag and screen `TSM_SCREEN_INVERSE` | inverse screen | yes |
| 6 | DEC | DECOM | `:1917` | vte flag and screen `TSM_SCREEN_REL_ORIGIN` | origin mode; saved by DECSC (`:853`) | yes |
| 7 | DEC | DECAWM | `:1926` | vte flag and screen `TSM_SCREEN_AUTO_WRAP` | autowrap; saved by DECSC (`:852`) | yes, to set |
| 8 | DEC | DECARM | `:1935` | vte flag | none; the flag is never read | yes, to set |
| 9 | DEC | X10 mouse | `:1938` | `mouse_mode` | X10 press reports through the host's mouse callback | yes |
| 12 | DEC | cursor blink | `:1947` | none | none; a TODO | n/a |
| 18 | DEC | DECPFF | `:1950` | none | none; no printer | n/a |
| 19 | DEC | DECPEX | `:1956` | none | none; no printer | n/a |
| 25 | DEC | DECTCEM | `:1961` | vte flag and screen `TSM_SCREEN_HIDE_CURSOR` | cursor shown or hidden | yes, to set |
| 42 | DEC | DECNRCM | `:1970` | vte flag | none; the flag is never read | yes |
| 47 | DEC | alternate screen | `:1973` | screen `TSM_SCREEN_ALTERNATE` | switches buffers | yes |
| 80 | DEC | DECSDM | `:2066` | `sixel_no_scroll` | sixel placed without scrolling (ADR-103) | RIS only (`sixel_reset`, `:991-999`) |
| 1000 | DEC | normal mouse | `:1939` | `mouse_mode` | press and release reports | yes |
| 1002 | DEC | button-event mouse | `:2030` | `mouse_event` | motion while a button is held | yes |
| 1003 | DEC | any-event mouse | `:2031` | `mouse_event` | all motion | yes |
| 1006 | DEC | SGR mouse | `:2042` | `mouse_mode` | SGR encoding | yes |
| 1016 | DEC | SGR pixel mouse | `:2054` | `mouse_mode` | pixel coordinates | yes |
| 1047 | DEC | alternate screen, erase on leave | `:1984` | screen flag | switches buffers | yes |
| 1048 | DEC | save cursor | `:1997` | `alt_cursor_x`, `alt_cursor_y` | saves and restores the cursor | no |
| 1049 | DEC | alternate screen with cursor | `:2011` | screen flag and `alt_cursor_*` | switches buffers, erasing on entry | yes |
| 1070 | DEC | private sixel registers | `:2070` | `sixel_private` | each sixel gets its own registers (ADR-103) | RIS only, to the host's setting |
| 2004 | DEC | bracketed paste | `:2080` | `bracketed_paste` | `tsm_vte_paste` wraps a paste (`:4025`) | no |
| 2027 | DEC | grapheme clusters | `:2076` | screen `width_method` | width method (ADR-102) | yes, to the configured method |
| 8452 | DEC | cursor right of sixel | `:2073` | `sixel_cursor_right` | cursor ends right of a sixel (ADR-103) | RIS only |

Mode 1048 is the only mode whose state is plain data in the vte struct. Modes 47, 1047, 1048 and 1049 are skipped when `TSM_VTE_FLAG_TITE_INHIBIT_MODE` is set (`:1974`, `:1985`, `:1998`, `:2012`); no sequence and no public call sets that flag.

## Where the state lives

Mode state is spread over five kinds of storage:

- a bit set in `vte->flags`, defined publicly as `TSM_VTE_FLAG_*` (`src/tsm/libtsm.h:844-861`) and read by hosts through `tsm_vte_get_flags` (`libtsm.h:1086`). `tsm-vte.c:125-142` defines the same bits again as `FLAG_*`, and nothing uses that copy;
- the screen's flags, `TSM_SCREEN_*` (`libtsm.h:165-171`), which several DEC modes set beside their vte flag;
- the mouse fields `mouse_mode` and `mouse_event`, whose values are the mode numbers themselves (`libtsm.h:926-932`); one variable holds both the tracking mode and the encoding (libtsm#30);
- named fields: `bracketed_paste` (`:193`), `sixel_private`, `sixel_no_scroll`, `sixel_cursor_right` (`:229-231`), `alt_cursor_x` and `alt_cursor_y`;
- the screen's `width_method`, for mode 2027.

The defaults are written in three places: `tsm_vte_reset` (`:892-933`, RIS and DECSTR), `sixel_reset` (`:991-999`, RIS only, through `tsm_vte_hard_reset`), and the field initialisers in `tsm_vte_new` (`:564`). Mode 2004 has none: `bracketed_paste` is written only at `:2081`, so RIS and DECSTR leave a program's bracketed paste on.

## Reports and saved modes

- **DECRQM** (`CSI ? Ps $ p`, `CSI Ps $ p`). The `p` final with `$` answers only DEC private mode 2027 (`:2418-2421`, reply built in `csi_decrqm_2027`, `:2240-2248`). Every other query, ANSI ones included, gets no reply, which a program reads as "not recognised". Tested by `test_vte.c:1020-1027`; the probe `decrqm` is listed missing against libtsm#31.
- **XTSAVE and XTRESTORE** (`CSI ? Pm s`, `CSI ? Pm r`). Neither exists. `s` has no CSI case. `r` is DECSTBM with no check for `?` (`:2434-2443`), so XTRESTORE sets the scroll margins from the mode numbers it carries; `tsm_screen_set_margins` (`tsm-screen.c:925-945`) turns most such pairs into the full screen, resetting a program's scroll region.
- **DECSC** saves autowrap and origin mode with the cursor (`:845-856`).

## Tests

Mode tests are spread across `test/test_vte.c` (1049, 47, 5, 7, 25 and 2027 among other cases, DECRQM of 2027 at `:1020-1027`, and a DECRQM ?12 at `:475` that checks only that the query changes nothing), `test/test_vte_mouse.c` (9, 1002, 1003, 1006, 1016) and `test/test_vte_sixel.c` (80, 1070, 8452), and tier 1 conformance cases (`alt-screen`, `autowrap`, `img-alt-47`, `img-alt-1047`, `img-alt-1049`, the `mode-2027-*` cases). Nothing lists which modes have a test, and nothing fails when a mode gains a case without one.

## kmscon's dependence on modes

- **Mouse.** `src/terminal.c:1321` reads `tsm_vte_get_mouse_mode` to decide whether pointer events go to the program or to selection.
- **Paste.** kmscon pastes through `tsm_vte_paste`, which reads mode 2004 inside libtsm.
- **Drawing.** kmscon reads no mode before drawing. `pty_input` (`src/terminal.c:1513-1523`) calls `redraw_all` after every `tsm_vte_input_bounded` call; the pty reads up to `KMSCON_NREAD`, 16384 bytes, at a time (`src/misc/pty.c:49`), up to 50 reads per readable event (`:468-491`). A redraw while a page flip is in flight is deferred to the flip (`src/terminal.c:366-375`, `:466-474`) and then draws whatever the screen holds at that moment. The per-call redraw also costs CPU: with full-screen `mpv --vo=sixel` at 7680x2160, 58% of kmscon's samples fall under `pty_input` → `redraw_all` → `do_redraw_screen`, which walks the whole frame (`screen_frame`, `cell_attr_resolve`) about 500 times a second and then presents nothing because a flip is pending (#139, open). The walk and the torn frames have one cause: a redraw requested after every read.
- **Capability probes.** `tests/capabilities/capabilities.list` lists the mode probes: `mode-2027`, `mouse-1000`, `mouse-1006`, `mouse-1003-sgr`, `mouse-1016` and `bracketed-paste` work; `mode-2026-sync` (libtsm#21), `decrqm` (libtsm#31), `mouse-1006-first` and `mouse-1003-x10` (libtsm#30) and `focus-1004` (libtsm#29) are missing. The probes are hand-listed: a mode added to libtsm adds no probe line.
- **Terminfo.** `scripts/terminfo/kmscon.ti` advertises no mode-backed capability beyond `kmous=\E[M` (`:84`), which selects the X10 encoding. #94 lists `BD`, `BE`, `PS`, `PE` (mode 2004) and `XM` (mode 1006) as missing; the probes `terminfo-sync` (`Sync`, mode 2026) and `terminfo-focus` (`fe`, `fd`, mode 1004) wait on libtsm#21 and #29.

## Torn frames

The torus diagnosis (2026-10-23) replayed a captured full-screen animation, each frame one write by the program, through libtsm in kmscon's 16384-byte reads, presenting after each read through bbulk and drm2d's blend in memory at 384x54 cells (20 736 cells). Presented frames mixed rows of the previous frame and the next, up to 13 320 cells in one present: a frame larger than one read is presented part written. Each presented frame equalled a fresh draw of the screen libtsm held at that moment, so the renderer draws the state it is given; the mix is in the state presented. PR #141 (`tests/test_present.c`, open) keeps that comparison as a test, presenting after every read, at write ends only and after random reads. Programs that bracket their frames in mode 2026 cannot prevent the mix on kmscon today, as libtsm drops `CSI ? 2026 h` at `:2084`.

## Mode 2026 elsewhere

- **Specification.** Contour's "Synchronized Output" (contour-terminal/vt-extensions `synchronized-output.md`, formerly Christian Parpart's gist `d8a62cc1ab659194337d73e399004036`): while the mode is set the renderer "will keep rendering the last rendered state", and the terminal "keeps processing incoming text and sequences"; resetting it ends the frame. DECRQM `CSI ? 2026 $ p` detects support: a reply of 1 or 2 means supported, 0, 4 or no reply means unsupported, 3 is undefined. On the timeout: "there is no real concensus if and if so how long a timeout should be", and "a too short timeout (maybe due to a very slow connection) won't be worse than having no synchronized output at all". It says nothing of RIS or input.
- **foot** (foot-ctlseqs(7)) lists private mode 2026, "Application synchronized updates mode", and keeps the older `DCS = 1 s` / `DCS = 2 s` for compatibility. **WezTerm** (escape-sequences documentation): "DECSET 2026 is set to batch (hold) rendering until DECSET 2026 is reset to flush the queued screen data"; neither page gives a timeout.
- **Timeouts in the field.** Alacritty added synchronized updates with a 150 ms timeout and a 2 MiB cap on buffered bytes (commit `9575aed`, "Add support for synchronized updates"). Rio holds for 150 ms (release v0.5.5 notes). st's `appsync` patch defaults to 200 ms (`su_timeout = 200`). wtmux ends a pane's hold after one second (release 4.1.0 notes).
