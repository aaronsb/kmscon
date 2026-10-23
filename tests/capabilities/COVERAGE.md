# Regression coverage of the fork's own features

Each feature the fork built, the tests that would catch a regression of it, and the paths those tests reach. A gap is either closed by a light test in the same change as this map or filed. `#N` is an aaronsb/kmscon issue or PR, `libtsm#N` an aaronsb/libtsm one. Probes are rows of [capabilities.list](capabilities.list), run by the `capabilities` meson test against the pinned libtsm.

Paths: font engine FT (freetype) / PG (pango); renderer BB (bbulk) / GT (gltex); video D2 (drm2d) / FB (fbdev). Tier 1 is libtsm's replay in three views (cell state `golden/`, `golden-draw2/`, `golden-frame/`); tier 2 is `tests/test_pixels.c` (FT + BB + the real drm2d blend into a fake display at normal orientation); tier 3's default variants run PG + BB + D2 in a VM with blink off; its GL variant (GT on drm3d) is outside the default run (#17). No test reaches unifont, psf or fbdev end to end, or a real GL context.

## libtsm

| Feature | Built in | Guarding tests | Gap and action |
|---|---|---|---|
| Colon sub-parameters; only SGR reads them; malformed colours rejected | libtsm#5 | tier 1 `csi-subparam-non-sgr`, `sgr-subparam-unknown`, `sgr-subparam-overflow`, `sgr-color-forms-*`, `sgr-color-colon-out-of-range`, `sgr-color-semicolon-subparams`; probe `sgr-colon-subparams` | guarded |
| SGR 4:1-4:5 underline styles, SGR 21 double, SGR 24 | libtsm#5 | tier 1 `sgr-underline-styles`, `sgr-underline-extended`; `test_vte_frame_colors`; tier 2 `underline`, `wide-deco`; probe `sgr-underline-styles` | SGR 21 rested on one case; the probe adds a second guard, libtsm#32 asks for a unit assertion |
| SGR 58/59 underline colour; 4;58;5;196 not blink | libtsm#5, #6 | tier 1 `sgr-color-forms-ul`, `sgr-underline-color-reset`, `sgr-58-semicolon-no-blink`, `colour-resolve`; `test_vte_draw2_underline_color*`; tier 2 `underline`, `deco-config`, `deco-force`; probe `sgr-underline-colour` | guarded |
| SGR 9/29 strike | libtsm#5 | tier 1 `sgr-basic`, `sgr-strike`; `test_vte_frame_colors`; tier 2 `strike`, `redraw`; probe `sgr-strike` | guarded |
| Colours kept as written, resolved at draw; `tsm_vte_get_palette_color` | libtsm#6, #11 | `test_vte_palette_recolours_text`, `test_vte_get_palette_color`, `test_screen_resize_alt_colors`; tier 1 `colour-resolve`; probe `palette-at-draw` | guarded |
| Grapheme clusters, interning, 32-codepoint cap, SipHash keys | libtsm#7 | tier 1 `combining`, `emoji*`, `cluster-*`; `test_symbol.c`; `test_vte_cluster_release`, `test_vte_frame_cluster_key`; tier 2 `combining`, `emoji*`; probe `cluster` | guarded |
| A cluster's generation is never 0 | libtsm#10 (e6b124e) | none | unguarded: libtsm#32 (needs an internal test in `test_symbol.c`) |
| Width methods, mode 2027 and its DECRQM reply; VS15/VS16 widths | libtsm#7 | tier 1 `width-methods`, `mode-2027-*`, `vs16-*`, `vs15-width`; `test_vte_width_method_resets`; tier 2 `vs16`; probes `mode-2027`, `vs16-width` | no pixel screen under wcwidth: #122 |
| Wide character at the margin: wraps whole, cut edits | libtsm#12, #10 | `test_vte_wide_at_margin`, `_widen_at_margin`, `_clear_past_margin`, `_frame_wide_cut`; tier 1 `wide-chars`; tier 2 `wide-deco`, `emoji`; tier 3 `wide-margin`; probe `wide-at-margin` | guarded (tier 2 `wide-deco`'s caption is stale, #70) |
| REP: wide characters whole, clusters by their first codepoint | libtsm#12 | `test_vte_repeat_last_printed`, `test_vte_repeat_cluster`; probe `rep` | no tier 1 case: libtsm#32 |
| Erased cells take the pen's colours only (BCE) | libtsm#13 | `test_vte_erase_colours_only`; tier 1 `prog-tmux`; probe `erase-colours-only` (EL, SU, SD, RI) | tier 1 has no coloured-pen erase case: libtsm#32; no pixel screen: #122 |
| Frame API, cluster keys, `TSM_FRAME_CHANGED` | libtsm#10 | all of `golden-frame/`; `test_vte_frame_*`; `test_bbulk.c`, `test_gltex.c`, tier 2 | guarded |
| Per-caller frame age (`tsm_screen_frame_since`) | libtsm#14 | `test_vte_frame_since_*`; `test_text.c` `test_two_displays`; `test_blink.c` second display; probe `frame-age-per-caller` | guarded |
| Blink flag | libtsm#10 | `golden-frame/`; `test_blink.c`; `test_gltex.c` `test_blink`; probe `sgr-blink` | guarded |
| XTMODKEYS/XTQMODKEYS not read as SGR 4 | upstream c0733a5, libtsm#15 | `test_vte_modkeys_not_sgr` (draw2 only); tier 1 `prog-vim`; probe `modkeys-not-sgr` | the unit test goes with draw2: libtsm#32 |
| draw2 view | libtsm#9 | all of `golden-draw2/` | guarded until removed |
| Selection bounded by line width; scrollback limit lowered while viewed | libtsm#7, #8, #12 | `test_selection.c`, `test_vte_word_select_space`, `test_vte_max_sb_while_viewed` | guarded |
| Fork soname `libtsm.so.4-fork.1` | libtsm#5 | none | unguarded: libtsm#32 |

## kmscon

| Feature | Built in | Guarding tests | Paths | Gap and action |
|---|---|---|---|---|
| Decoration sprites: underline styles, strike, SGR 58 colour | #33 | tier 2 `underline`, `strike`, `wide-deco`, `combining`, `redraw`; tier 3 `underline-styles`, `underline-colour`, `strike`, `sgr`; `test_bbulk.c` underline cursor colour | FT/BB/D2, PG/BB/D2 | gltex stubbed: #116 |
| Decoration config keys (`underline-color`, `strike-color`, `force-decoration-colors`) | #33 | `test_conf.c`; tier 2 `deco-config`, `deco-force` | FT/BB/D2 | gltex: #116; pango: #117 |
| Cell-exact block elements and box drawing | #59 | tier 2 `blocks`, `blocks-spaced`, `box` (gap checks); tier 3 `blocks`, `boxdraw` | FT/BB/D2, PG/BB/D2 | drawn in `font.c` before any engine, so every engine shares the sprites; gltex: #116 |
| Braille, sextants and octants as cell-exact sprites | #91 | `test_font_box` (meson); tier 2 `mosaic`, `mosaic-spaced` (mosaic check); tier 3 `mosaic` | FT/BB/D2, PG/BB/D2 | drawn in `font.c` before any engine, like the box sprites; gltex: #116 |
| Colour emoji, fit rule, presentation, `emoji-color` | #72 | `test_font_freetype.c` `test_wants_colour`, `test_colour_fit`, `test_colour_up_flow`; tier 2 `emoji`, `emoji-spaced`, `emoji-mono`, `vs16`; tier 3 `emoji` | FT/BB/D2, PG/BB/D2 | pango with colour off and missing glyph: #71, #117; gltex: #68, #116 |
| COLR glyphs | #72 | `test_font_freetype.c` `test_colour_table_text_face` | FT (shared `font_colour.c`) | no pixel screen; pango: #117 |
| Combining marks shaped by HarfBuzz | #33 | `test_font_freetype.c` cluster tests; tier 2 `combining`; tier 3 `combining` | FT/BB, PG/BB | gltex: #116 |
| Blink: both phases, both buffers | #66 | `test_blink.c` `test_phases`, `test_buffers`; `test_gltex.c` `test_blink`; `test_conf.c` | FT/BB/D2 fake, GT stub | terminal.c's timer: #120 |
| Two-frame damage rule, damage expiry, quiet frames | #33, #80 | `test_bbulk.c` `test_two_frame_rule`; `test_blink.c` `test_damage_extent`, `test_buffers` | BB on a fake display | DRM submission: #119 |
| Hardware cursor commit | #80 | `test_blink.c` `test_hw_cursor` | text.c + fake cursor | `drm_shared.c`: #119 |
| Glyph draw failure retried | #33, #80 | `test_bbulk.c` | BB | guarded |
| Frame age per display | #33, #41 | `test_text.c` `test_two_displays`; `test_blink.c` | any renderer | guarded |
| gltex glyph cache stays bounded | #33 | `test_gltex.c` `test_bounded` | GT, GL stubbed | real GL: #17 |
| drm2d and fbdev premultiplied blend | #33 | `test_blend.c` (drm2d; fbdev 32/24/16 bpp); tier 2 uses the real drm2d blend | D2; FB maths only | fbdev end to end, rotation: #118 |
| `grapheme-width` option | #29, #33 | `test_conf.c` | parse only | drawing under wcwidth: #122 |
| FreeType W-calibrated cell size (upstream sync) | #62 | every tier 2 golden (9x16 cell) | FT/BB | tier 3 runs pango: #121 |
| libseat `--vt`, font retry, pointer guard (upstream sync) | #62 | tier 3 `vt-switch`, `vt-restore` | PG/BB/D2 | font retry and pointer guard: #120; libseat: #121 |
| Local demo and per-page feedback, `s` to skip a page | #42, #48, #115 | meson `demo_feedback` (run 3 checks the skip key and the summary's `skipped` line) | script on a pty | guarded |
| `make vt-demo`, `run-vt.sh` refusals | #65 | meson `demo_vt_select` | script, stand-in loginctl | guarded |
| Showcase | #83 | meson `showcase` | script | playback not covered (manual) |
| Run and comparison page generators | #4, #6, #50, #52, #58 | `test_runpage.py`, `test_compare.py` | Python | now meson tests `sheet_runpage`, `sheet_compare` (were only in the Makefile's `make test`) |
| Tier 3 harness modules | #14, #21, #55, #56, #74, #89 | `test_t3img.py`, `test_t3.py` | Python | now meson tests `tier3_t3img`, `tier3_t3` (were only `make tier3-selftest`) |
| Capability probes | #113 | meson `capabilities` | libtsm, FT, terminfo | this map |
| Render timer `log` and `osd` (ADR-103): flips, CPU time and damage per frame, one line a second, and the same figures drawn at the right end of the top row | #89, ADR-103 step 3b | `test_render_timer` (fake clock and display: intervals, the quiet line, idle blinking, percentiles, the log line, the display's line); `test_conf` `test_parse_render_timer`; `test_bbulk` `test_damage_measured`; tier 2 `osd` (the overlay replaced by a shorter one leaves nothing behind); tier 3 `s1-scroll` fails when a fork build logs no line in its window | counters on fakes; the overlay BB/D2 in tier 2; terminal.c's wiring PG/BB/D2 in tier 3 only, `log` only | a failed swap counts as a queued frame: #93; `osd` under gltex not drawn (ADR-103 step 3f); GT and FB not run |
| Image composition in bbulk (ADR-103 step 3b): three bands, opaque row runs, scaled-picture cache, placement damage | ADR-103 step 3b | `test_bbulk` `test_placement_damage`, `test_opaque_runs`, `test_below_bg`; tier 2 `img-bands`, `img-sixel`, `img-scale`, `img-region`, `img-rotated`, `img-moves`, `img-banner-bg`, `img-banner-text`, `img-ed2`, each multi-frame screen also against a fresh draw | BB/D2, placements through libtsm's test calls | no parser makes placements yet (sixel 3c, kitty 4a), so tier 3 has no image screen; GT not drawn (3f); FB not run |
| S1 text-scrolling benchmark; the potato variant (1 vCPU, 512 MB, 1366x768) | #89 | tier 3 `s1-scroll` and every screen on `2d-1366x768-potato`; `test_t3.py` `BenchTest`, `VariantTest`; `test_t3img.py` padded PPM rows | PG/BB/D2 | figures move with host load; no ADR-103 target judged yet |

The render timer's on-screen display is drawn by bbulk only; gltex draws no placements until ADR-103 step 3f.
