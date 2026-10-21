---
contract: adr/v1
kind: evidence
capability: renderer
status: proposed
date: 2026-10-20
deciders:
  - aaronsb
related: []
---

# ADR-100: kmscon render pipeline as of upstream 6c03eb4

## Scope and method

A read of kmscon at upstream `6c03eb4` (10.0.4+) and libtsm `v4.8.0`, tracing a byte from the pty to a pixel, made to locate where colour glyphs, inline images and styled underlines would have to enter. File references are to those trees (`src/...` is kmscon, `tsm/...` is libtsm's `src/tsm`). The community sixel patches attached to kmscon issue #138 were read for comparison. Statements marked *inferred* follow from the code but were not run.

## Frame flow

- `pty_input` (`src/terminal.c:1368`) calls `tsm_vte_input` and then redraws on every read chunk (up to 16 KiB). There is no frame coalescing.
- A redraw runs `kmscon_text_prepare`, `draw`, `draw_pointer`, `render`, then `display_swap` (`src/terminal.c:314-337`). A redraw during a pending page flip is deferred to the flip callback.
- `kmscon_text_draw` takes the whole grid from `tsm_screen_draw2()` (`src/render/text.c:449`), a flat array of resolved cells with no age information.
- bbulk keeps its own damage: it diffs each cell against the previous frame with `memcmp` (`src/render/bbulk.c:378`) and passes merged rectangles to DRM as `FB_DAMAGE_CLIPS`. gltex clears and redraws the whole frame (`src/render/gltex.c:502`).

## Cell model

- The exported cell is `{uint32_t ch; fg; bg; attr2}`, 11 bytes, with colours already resolved to RGB and inverse, dim and selection applied (`tsm/libtsm.h:194`, `tsm/tsm-render.c:280-298`).
- Attributes are single bits: bold, italic, underline, inverse, protect, blink, dim. There is no underline style and no underline colour. `attr2` has 4 reserved bits; there is no extension field or pointer.
- Wide characters occupy a first cell with `width=2` and a following `ch=0` cell; `draw2` does not export width (`tsm/tsm-screen.c:393-401`).
- Multi-codepoint symbols (ids above `TSM_UCS4_MAX`) are exported as empty cells (`tsm/tsm-render.c:265-269`), so ZWJ sequences and emoji presentation selectors render blank.
- Scrollback relinks whole line objects; cell data is not copied (`tsm/tsm-screen.c:203-241`).
- Adding underline style or colour changes `tsm_screen_attr` and `tsm_screen_cell`, which is a libtsm ABI change.

## Escape parsing

- DCS states exist but `DCS_START`, `DCS_COLLECT` and `DCS_END` do nothing (`tsm/tsm-vte.c:2349-2354`). There is no DCS callback, so sixel payloads are discarded.
- APC, PM and SOS go to `ST_IGNORE` (`tsm/tsm-vte.c:2425-2428`, `2490-2494`). There is no APC callback, so kitty graphics commands are discarded.
- OSC is collected into a 128-byte buffer; OSC 4, 10 and 11 are handled internally, the rest go to `tsm_vte_set_osc_cb`.
- A colon in CSI parameters sends the whole sequence to `CSI_IGNORE` (`tsm/tsm-vte.c:2529-2530`). `SGR 4:3` or `58:2::r:g:b` therefore discards every attribute in that sequence. SGR 58 and 59 are unhandled.

## Glyphs

- Every glyph is an 8-bit alpha mask: `struct video_buffer` is documented as one byte of alpha per pixel (`src/video/video.h:90-98`), and `struct kmscon_glyph` carries only that buffer and a width flag (`src/font/font.h:59-62`).
- freetype loads with `FT_LOAD_TARGET_LIGHT` and renders `FT_RENDER_MODE_NORMAL` (`src/font/font_freetype.c:387-425`). `FT_LOAD_COLOR`, CBDT, sbix and COLR appear nowhere; a BGRA bitmap would be copied as grey, and a fixed-size emoji strike would be cropped (*inferred*).
- Underline is drawn into the glyph mask by the font backend and the attribute is part of the glyph cache key (`src/font/font_freetype.c:319-343`), so it is always single and always the text colour.
- freetype fallback walks `FcFontSort` with one cached fallback face per weight; pango falls back through fontconfig but renders to a grey bitmap only. Italic is implemented only in pango.

## Renderers and backends

- bbulk blends one alpha mask per cell: `out = fg*a + bg*(255-a)`, painting the background in the same operation (`src/render/bbulk.c:344-352`).
- gltex uploads masks into `GL_ALPHA` atlases and its fragment shader mixes fg and bg by the sampled alpha (`src/render/gltex.c:323-330`, `gltex_atlas.frag`).
- The video API offers `display_blend` (alpha mask with fg/bg), `display_clear`, damage and swap. drm2d blends into an `XRGB8888` buffer; fbdev converts to 16/24/32 bpp; drm3d is GL-only with no blend op. No RGBA blit exists; the only ARGB surface is the hardware cursor plane.

## The community sixel patches (#138)

- Images are held in a screen-level list, each anchored to a line pointer and located by linear search on each draw, capped at 64.
- A new `display_blit_rgba` (drm2d, fbdev) and an optional `draw_image` text op (bbulk only) draw them after the text.
- They do not cover gltex, drm3d, rotation, scrollback viewing, resize or the alternate screen. The image blit bypasses bbulk's cell diff and damage rectangles, so a removed image leaves stale pixels (*inferred*).
- The kmscon half applies cleanly to `6c03eb4`; the libtsm half no longer applies to `v4.8.0`.

## Tests

- kmscon's `meson test` runs 8 tests; none inspects pixels. `test_bbulk` includes `bbulk.c` with stubbed `display_*` functions whose `display_blend` does nothing, which makes it the natural place for an in-memory framebuffer.
- libtsm's check-based tests (`test_vte` and others) do not cover `draw2` output, SGR underline or DCS.
