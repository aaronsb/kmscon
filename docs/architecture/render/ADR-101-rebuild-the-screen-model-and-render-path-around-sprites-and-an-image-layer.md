---
contract: adr/v1
kind: decision
verb: change
capability: [renderer, cell-model, images]
basis:
  - operator: aaronsb
    level: directed
    said: "maybe we should plan out an adr for how we'd rebuild the pipeline"
    via: Claude Code session, after colour emoji rendered as monochrome silhouettes
  - operator: aaronsb
    level: guided
    said: "between you and me I think we can re-write it rather quickly. if we offend the authors, well thats a different matter."
    via: Claude Code session, choosing between patching libtsm and replacing it
  - operator: aaronsb
    level: guided
    said: "maybe we can be inspired by kitty's approach"
    via: Claude Code session, on the architecture to follow
  - precedent: ADR-100
  - precedent: ADR-300
  - precedent: ADR-301
  - upstream: "kmscon maintainer on issue #138 (2026-03-14): sixel or the kitty image protocol 'will require rewriting most of libtsm, and a large part of the rendering in kmscon', a task he will not take on himself"
  - standard: kitty terminal graphics protocol, https://sw.kovidgoyal.net/kitty/graphics-protocol/
  - standard: VT340 sixel graphics (DEC STD 070), and the SGR 4:x / 58 / 59 underline extensions
agent:
  name: claude
  model: claude-opus-5-5
considered:
  - operator: aaronsb
    said: "I read through the ADRs and they make sense to me and line up with what we discussed. I don't see any surprise decisions"
    via: Claude Code session, reviewing PR aaronsb/kmscon#1
status: accepted
date: 2026-10-20
deciders:
  - aaronsb
related: []
---

# ADR-101: Rebuild the screen model and render path around sprites and an image layer

## Summary

- **Decided:** Keep kmscon's platform layer: VT and seat handling, video modes and backends, input, font discovery. Replace the layer between them: the screen model becomes cells with structured attributes and a glyph key, every drawn cell resolves to sprites in one cache that holds both alpha masks and RGBA, and images live in a separate layer of placements that the screen keeps in step through events.
- **Trades away:** ABI compatibility with libtsm as packaged, and a small diff against upstream. The fork carries a replaced screen model and render path, not a patch series.
- **One-way?** No, but expensive. Each step lands behind the conformance harness (ADR-301) and the fork can return to upstream's pipeline. The cost of reversing grows with every feature built on the new cell model.
- **Probes:** *Confident:* the platform layer is the part to keep; it is the security-sensitive code kmscon was chosen for and it already works. *Not confident:* that the CPU renderer can composite images at 7680x2160 fast enough; damage tracking has to make up for the absent GPU, and that needs measuring.
- **Inversion:** Between "extend libtsm in place" and "write a new terminal". The answer replaces only the middle: the screen model and the path from cells to pixels.

## Context

ADR-100 records the pipeline as it stands. Three features the fork exists for cannot be added to it incrementally:

- **Colour glyphs.** The path from font to screen carries one byte of alpha per pixel from end to end: the glyph struct, the glyph cache, the video buffer, the blend operation and the GL atlas format. There is no place for a colour pixel to travel.
- **Styled and coloured underlines.** The cell has one underline bit, colours are resolved before the renderer sees them, and the underline is drawn into the glyph image by the font backend. A curly underline in a separate colour needs new cell fields, a later point of colour resolution, and an underline that is not part of the glyph.
- **Inline images.** The parser discards DCS and APC. The community patch shows what an image list bolted beside the cells costs: placements found by searching lines on every frame, images that vanish in scrollback, and pixels left behind because the image blit sits outside the renderer's damage tracking.

All three meet in the same place: the screen model and the render path between libtsm and the video backends. Patching each feature through that path separately would build three special cases into a design that has no room for any of them.

kitty solved the same problems with an architecture that can be described without reference to its code (ADR-300): cells hold attributes and a sprite reference, colour emoji and underline styles are sprites like any glyph, and images are placements in a layer the screen notifies on every change.

## Decision

### Boundary

| Keep from upstream | Replace |
|---|---|
| `src/uterm`: VT and seat (real, libseat), DRM and fbdev display setup, modes, page flips | libtsm's screen and cell model, and `tsm_screen_draw2` |
| `src/input`: xkbcommon keyboard handling | The glyph format, glyph cache and the `kmscon_text_ops` interface |
| `src/font`: font discovery, face selection and fallback | Glyph rasterisation, which gains colour glyph loading |
| libtsm's escape parser, as the behavioural baseline | DCS and APC handling, and CSI sub-parameter parsing, which are added |

The parser is not rewritten until the harness shows a reason to. Replacing it would discard years of compatibility fixes for no gain to the three features.

### Cell model

- A cell carries a glyph key, a width, and attributes as structured fields: foreground, background and underline colour, each kept as a palette index or RGB value and resolved only at render time; an underline style (none, single, double, curly, dotted, dashed); and the existing bold, italic, dim, blink, inverse, protect and strike bits.
- The glyph key names a grapheme cluster. A single codepoint is stored inline; a multi-codepoint cluster (ZWJ sequence, presentation selector, combining marks) is stored once in a table and referenced. Width is part of the cell, not inferred by the renderer.
- Scrollback keeps whole lines, as now, so a line moving into scrollback keeps its cells and its image anchors.

### Sprites

- Every visible cell resolves to a glyph sprite and an optional decoration sprite, through one cache keyed by glyph key, font face and style.
- A sprite is either an alpha mask, tinted with the cell's resolved colour, or RGBA, drawn as it is. Colour emoji load through freetype's colour glyph support and are scaled into the cell box.
- Underline styles are decoration sprites, generated once per style and cell size and tinted with the underline colour. The font backend no longer draws underlines into glyphs.

### Image layer

- Images are stored by id, with a total storage limit and eviction of the least recently used image that has no visible placement.
- A placement anchors an image to a cell (line and column) with a pixel offset, a size in cells and a z-index.
- The screen reports every scroll, erase, line insert or delete, resize, alternate-screen switch and reset to the image layer, which moves, clips or drops placements. Placements on lines that enter scrollback travel with those lines.
- Sixel creates an anonymous image and places it at the cursor, advancing the cursor as DEC terminals do. The kitty protocol creates images by id and places them by command. Unicode placeholder cells that reference placements follow later.

### Composition

Each frame is composed in fixed order: the default background, placements below the cell backgrounds, cell backgrounds, placements below the text, glyph and decoration sprites, then placements above the text. The z-index of a placement chooses its band.

### Renderers and backends

- The renderer interface receives the cells and the visible placements for a frame.
- gltex is the primary renderer: a mask atlas, an RGBA atlas, a texture per image, and one draw per band.
- bbulk is the fallback for systems without GL. It composites the same bands on the CPU. Its damage covers changed cells and the rectangles of placements added, moved or removed, so nothing is left behind.
- drm2d and fbdev gain an RGBA blend operation. drm3d needs none.

### Parsing

- DCS and APC payloads are collected into buffers with a size limit and dispatched to the sixel and kitty handlers. A payload over the limit is dropped whole.
- CSI parameters accept colon sub-parameters, so `SGR 4:3` and `58:2::r:g:b` are applied rather than discarding the sequence they appear in. SGR 58 and 59 set and clear the underline colour.

### Where the code lives

New subsystems live in new files. Upstream kmscon files change only at named hook points, so merging upstream touches those points and nothing else. libtsm is forked alongside kmscon, and its screen model is replaced wholesale in the fork; the subproject wrap points at the fork.

### Order

1. The conformance harness (ADR-301).
2. The cell model and sprite cache, delivering colour emoji, grapheme clusters and underline styles.
3. The image layer and sixel.
4. The kitty graphics protocol.
5. Unicode placeholders.

## Consequences

### Positive

- Colour glyphs, underline styles and images share one mechanism instead of three special cases.
- The renderers stop guessing: width, cluster and colour arrive in the cell.
- Images behave correctly through scrolling, scrollback, resize and erase, which is where bolted-on designs fail.
- The security-sensitive platform layer stays as upstream maintains it.

### Negative

- The fork diverges from upstream libtsm's ABI. Programs other than kmscon that use the system libtsm are unaffected only because kmscon builds the fork as a subproject.
- Every upstream change to libtsm's screen or to the renderers has to be ported by hand instead of merged.
- The CPU path at very high resolutions may be slow with images on screen; this is unmeasured.

### Neutral

- The order puts visible wins (colour emoji, underlines) before images, so each step is usable on its own.
- Offering the work upstream becomes a single proposal backed by a working fork, not a patch series.

## Alternatives Considered

- **Extend libtsm in place, feature by feature**, as the community sixel patch does. Rejected: the cell layout cannot hold underline style or colour without the same ABI change, colours resolved before rendering block a separate underline colour, and an image list beside the cells without screen events leaves stale pixels and loses images in scrollback.
- **Store images as cell tiles**, each cell holding a piece of an image, as mlterm does. Scrolling and erasing then work for free. Rejected: tiles cannot express z-index, pixel offsets or placements that move independently of the text, which the kitty protocol requires, and they tie image lifetime to cell contents.
- **A GPU-only renderer**, as kitty has. Rejected: kmscon is also a recovery console, so it has to draw on fbdev and drm2d when GL is unavailable. The CPU renderer stays, composing the same bands.
- **A new terminal from scratch, platform included.** Rejected: VT switching, seat handling, DRM mode setting and input are the security-sensitive parts and already work in kmscon. Rewriting them spends effort where nothing is missing.
- **Port kitty's implementation.** Rejected by ADR-300: it would make the fork GPL-3.0.
