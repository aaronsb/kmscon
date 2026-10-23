---
contract: adr/v1
kind: decision
verb: add
capability: images
basis:
  - operator: aaronsb
    level: guided
    said: "but I also want to avoid limiting kitty and sixel sources"
    via: Claude Code session, relayed verbatim by the team lead, on which kitty transmission media and sixel sources to support; read as all four kitty media
  - operator: aaronsb
    level: guided
    said: "I'd rather not invent new security conventions if possible"
    via: Claude Code session, relayed verbatim by the team lead, on reading files that kitty commands name while kmscon runs as root
  - operator: aaronsb
    level: directed
    said: "we need to make it configurable. it should be able to run on hamster wheels"
    via: Claude Code session, relayed verbatim by the team lead, answering the memory-default probe
  - operator: aaronsb
    level: guided
    said: "not everyone's going to have a ginormous 8k display"
    via: Claude Code session, relayed verbatim by the team lead, on benchmark targets that named only 7680x2160
  - precedent: ADR-101
  - precedent: ADR-102
  - precedent: ADR-300
  - precedent: ADR-301
  - standard: "kitty terminal graphics protocol, https://sw.kovidgoyal.net/kitty/graphics-protocol/ (transmission medium, chunking, display, deleting images, usage hints, storage quotas, interaction with other terminal actions)"
  - standard: "DEC STD 070 and the VT330/VT340 Programmer Reference Manual, sixel graphics and DECSDM"
  - standard: "xterm control sequences (ctlseqs): DECSDM (mode 80), modes 1070 and 8452, XTSMGRAPHICS, XTWINOPS, DA1 attribute 4; xterm(1) resources maxGraphicSize, numColorRegisters, privateColorRegisters"
  - standard: "Ghostty configuration reference, image-storage-limit: bytes of image data per terminal screen, 320MB default, zero disables image protocols, separate limits for the primary and alternate screens"
  - upstream: "kmscon's kmsconvt@.service has no User= line, so kmscon runs as root; with no command given it starts /bin/login -p (src/misc/pty.c), which gives the session's tty to the user who logs in"
  - evidence: "Code map of libtsm d1ab10e and kmscon b989c61, 2026-10-22: the DCS start, collect and end actions do nothing and APC goes to the ignore state (tsm-vte.c); DA1 replies ?60;1;6;9;15c; a line is an object holding its cells that moves whole between the screen and scrollback, and resize grows lines without reflow (tsm-screen.c); kmscon sets no pixel size with TIOCSWINSZ and libtsm answers no XTWINOPS size report"
  - evidence: "Survey of Ghostty 08a9518 src/terminal/kitty (MIT), 2026-10-22: file path checked after open from the opened handle, refusing /proc, /sys and /dev except /dev/shm; temporary files read only inside /tmp, /dev/shm or the temp directory and with tty-graphics-protocol in the path, else refused; shared memory unlinked once opened; eviction order transient-unplaced, unplaced, transient-placed, placed, oldest first; 320 MB total and 10000-pixel dimension limits; cursor after a placement indexes rows-1 times, bounded, then moves past the image or wraps"
  - evidence: "Survey of foot sixel.c (MIT), 2026-10-22: text and erase split a sixel into the parts left uncovered; cursor ends on the text row holding the image's last pixel row, at the image's left column, or past its right edge with mode 8452; DECSDM set places the image at the top left without scrolling or moving the cursor; P2=1 is transparent, else unset pixels take register 0; aspect from P1 and raster attributes"
  - evidence: "Check by the team lead, 2026-10-22: desktop terminals (kitty, Ghostty, WezTerm, foot, Konsole) run as the desktop user, so sudo mpv --vo=kitty inside one still has the terminal open files as that user and root-only files fail to load; sudoedit opens files as the invoking user"
  - upstream: "kmscon/kmscon#138: trollyjolly (2026-07-26) proposed handing DCS strings that start with a sixel introducer to a decoder through a new tsm_vte callback; jholt0147 (2026-08-29) attached a libtsm patch and a kmscon patch, with no licence statement, read here as prior art only and no code taken"
  - standard: "mpv manual, DOCS/man/vo.rst: vo=sixel; vo=kitty with --vo-kitty-use-shm (shared memory, local only) and --vo-kitty-auto-multiplexer-passthrough (tmux and GNU screen)"
  - standard: "foot.ini(5): render-timer=none|osd|log|both, a frame rendering timer shown on screen, logged, or both"
  - evidence: "tmux CHANGES: basic SIXEL support from 3.4 when built with --enable-sixel, at most 20 SIXEL images since 3.7; Arch's tmux PKGBUILD configures --enable-sixel. WezTerm attaches sixel images to cells (term/src/terminalstate/sixel.rs, assign_image_to_cells)"
  - evidence: "aaronsb/kmscon issues #17 (gltex upload failures under virgl), #67 (bbulk damage carry-over, being fixed), #68 (gltex has no RGBA path until ADR-102 step 6), #75 (the showcase asks this record for performance targets and a frame counter), #77 (OSC 50 font paths named by programs)"
agent:
  name: claude
  model: claude-opus-5-5
status: accepted
date: 2026-10-22
deciders:
  - aaronsb
related: []
extends: ADR-101
amends: "ADR-101#parsing"
considered:
  - operator: aaronsb
    said: "on 5, I would think the tty owner would be the right move, but I'd defer to other systems that use this"
    via: "Claude Code session, relayed verbatim by the team lead, 2026-10-22; '5' was the fifth point of the team lead's summary of the image-access design, the sudo identity question. The team lead then checked other systems: desktop terminals and sudoedit read as the session's user"
    covers: [sudo-identity]
  - operator: aaronsb
    said: "1. yes."
    via: Claude Code session, relayed verbatim by the team lead, 2026-10-22, answering whether PNG and compressed data are decoded in the helper as the session's user
    covers: [helper-decoding]
  - operator: aaronsb
    said: "2. we need to make it configurable. it should be able to run on hamster wheels"
    via: Claude Code session, relayed verbatim by the team lead, 2026-10-22, answering the 320 MB per screen default; the default became a per-process budget scaled from the displays, 16 MiB to 192 MiB
    covers: [memory-default]
  - operator: aaronsb
    said: "not everyone's going to have a ginormous 8k display"
    via: Claude Code session, relayed verbatim by the team lead, 2026-10-22, on benchmark targets that named only 7680x2160; targets now cover 1366x768 and 1920x1080 on a small tier 3 variant
  - operator: aaronsb
    said: "yes. can we allocate and hold based on the display size? is it reasonable to provide options to activate and deactivate sixel and kitty support to save on memory of devices that do not want those things? (does that follow a convention that libkms might have already for option management?)"
    via: "operator's reply in the steering session, 2026-10-23, to the questions raised by the libtsm #16 and #17 reviews (decoder requires a budget)"
  - operator: aaronsb
    said: "we should pick the aspect ratio approach that other terminals typically use. if its a compiler flag or option that might (again, if kmscon has a convention like I propose already) be settable that would work"
    via: "operator's reply in the steering session, 2026-10-23, to the questions raised by the libtsm #16 and #17 reviews (sixel aspect ratio)"
  - operator: aaronsb
    said: "yes. this follows on option 2's question about a setting option"
    via: "operator's reply in the steering session, 2026-10-23, to the questions raised by the libtsm #16 and #17 reviews (libtsm with no host budget fails closed)"
  - operator: aaronsb
    said: "keep it"
    via: "operator's reply in the steering session, 2026-10-23, to the questions raised by the libtsm #16 and #17 reviews (ED 3 clears scrollback)"
  - operator: aaronsb
    said: "fix it in 3b"
    via: "operator's reply in the steering session, 2026-10-23, relayed by the team lead, to kmscon #127's question 1 (the picture cache allowed four screens within a frame, past the 192 MiB cap)"
  - operator: aaronsb
    said: "adopt and tighten others"
    via: "operator's reply in the steering session, 2026-10-23, relayed by the team lead, to kmscon #127's question 2 (adopt ADR-104's number for 7680x2160; keep or tighten the common-size targets)"
  - operator: aaronsb
    said: "Xterm seems incorrect, pushing an unwanted newline"
    via: operator's reply in the steering session, 2026-10-23, relayed by the team lead, to the step 3c review's question on sixel cursor placement (xterm 411 as a black box against foot)
  - operator: aaronsb
    said: "Keep the per-call work bound on the sixel decoder, with no total cap per byte; a register count outside 2 to the maximum answers status 3; XTWINOPS 14 and 16 answer with sixel off"
    via: operator's replies in the steering session, 2026-10-23, relayed by the team lead as answers to the step 3c review's other two questions
    paraphrase: true
observable:
  - "img2sixel picture.png on a kmscon VT shows the picture under the command; Shift+PageUp scrolls it away and back with the text; clear removes it and leaves no pixels behind"
  - "viu picture.png shows the picture through the kitty graphics protocol, inline and from a temporary file"
  - "A kitty client run by the logged-in user that names a root-only file with t=f gets EBADF:Failed to read image file, the same reply as for a missing file"
  - "mpv --vo=sixel and mpv --vo=kitty (inline and with --vo-kitty-use-shm=yes) play an animation full screen on drm2d with bbulk at 1366x768, 1920x1080 and 7680x2160, and the render timer's presented rate matches mpv's output rate"
  - "The 2d-1366x768-hamster tier 3 variant (1 vCPU, 512 MB) runs the image screens and S1 to S6 on every tier 3 run"
  - "tmux in a 3x3 grid of panes, each running mpv --vo=sixel, plays all nine and leaves no stale pixels when the panes close"
  - "The tier 2 screens for each composition band, and the render timer's figures for acceptance scenarios A1 to A7 recorded as an evidence record"
---

# ADR-103: Images are placements that travel with lines; sixel and the kitty graphics protocol feed one store

## Summary

- **Decided:** Steps 3 and 4 of ADR-101, made concrete.
  - **Store and budget:** one image store per screen buffer. Every store and the renderer's scaled pictures draw on one budget per kmscon process, `image-storage-limit`. By default it is four frames of the process's displays in ARGB, at least 16 MiB and at most 192 MiB: 16 MiB at 1024x768 and 1366x768, 31.6 MiB at 1920x1080, 192 MiB at 7680x2160. Over budget, scaled pictures not on screen go first, then images with no placement, then images placed only in scrollback. An image placed on the screen is never evicted; a new image that still does not fit is refused.
  - **Placements:** each is anchored to the line holding its top row, so it travels into scrollback and back with the text. A sixel behaves as cell content: text written or erased over it removes that part, as foot does and as terminals that keep image tiles in cells behave. A kitty placement follows the kitty spec: of the erase commands only ED 2 touches it.
  - **Parsing:** sixel and kitty payloads are decoded as they stream in, never buffered whole.
  - **Files:** all four kitty media are supported. Opening files and shared memory, and decoding PNG and zlib, happen in a per-session helper process running as the session's user, never as the foreground process, as a desktop terminal reads as its desktop user. It follows the kitty spec's file rules and Ghostty's checks. kmscon, which runs as root, decodes only sixel and base64 itself.
  - **Drawing:** bbulk composes the three placement bands per covered cell, sends cells nothing draws over an opaque picture as row strips, and damages placement rectangles on both buffers. gltex follows with a texture per image, after ADR-102 step 6.
  - **Acceptance:** full-motion video through mpv (`--vo=sixel`, and `--vo=kitty` inline and over shared memory) on drm2d with bbulk, the showcase's text banner over a full-screen video, and tmux grids of sixel videos. 1366x768 and 1920x1080 are the common case and carry targets, judged on a small tier 3 machine (1 vCPU, 512 MB); 7680x2160 is the stress case, with no target until the first measurement, an early step. A `render-timer` overlay (foot's option) shows frames presented, CPU time and damaged area per frame, drawn as a top-band placement.
  - **Settings:** `image-storage-limit` (Ghostty), `max-graphic-size`, `num-color-registers` and `private-color-registers` (xterm), and `render-timer` (foot).
- **Trades away:**
  - A program run under `sudo` cannot show root-only files through the file media, because the helper reads as the session's user, as a desktop terminal does under `sudo mpv`.
  - An optional dependency on sd-login (systemd's or elogind's) to find the session's user.
  - A file or PNG image costs a round trip to the helper, and that VT's output pauses until the reply.
  - Animation, relative placements and Unicode placeholders wait for step 5. Clients that use them get EINVAL.
  - Ghostty's meaning of `image-storage-limit`, which is per screen with a 320 MB default; here it is one budget per kmscon process, sized from its displays.
  - Byte-for-byte agreement with upstream on every tier 1 golden that records the DA1 reply, which gains attribute 4.
- **One-way?** No. `image-storage-limit=0` turns both protocols off. Reversing becomes expensive once programs rely on the replies, and once the helper protocol and the frame's placement list are interfaces both kmscon and libtsm build on.
- **Probes:**
  1. *Not confident (sudo-identity):* answered. The helper reads files as the session's user. Under `sudo`, a program running as root that asks to show `/root/picture.png` is refused with the protocol's generic error, though it could read the file itself. The alternative was to read as whichever program is in the foreground. The operator chose the session's user, deferring to what other systems do, and desktop terminals and sudoedit read as the session's user. The image helper section says how that user is found.
  2. *Confident (helper-decoding):* answered yes. A PNG or compressed image from any program, remote ones included, is decoded by the helper running as you, so a fault in the PNG library cannot become a root fault. The cost is that every PNG image, even one sent inline, passes through that helper.
  3. *Not confident (memory-default):* answered. The draft gave each screen 320 MB, kitty's and Ghostty's default, so one VT could hold 640 MB. The operator asked for a setting, and for kmscon to run on very small machines, so the default now scales with the displays and is capped well below that.
- **Inversion:** Between "images are cell content" (sixel's own model; mlterm and WezTerm store tiles in cells) and "images are a free layer text never touches" (the kitty spec's model). The answer keeps ADR-101's one layer and gives each protocol the erase rules its own specification describes.

## Context

ADR-101 decided that images are placements in a layer the screen keeps in step through events, and fixed the order: the image layer and sixel (step 3), then the kitty graphics protocol (step 4), then Unicode placeholders (step 5). ADR-102 delivered the structures images compose with: premultiplied ARGB glyphs, an ARGB blend in drm2d and fbdev, and a frame call that reports each caller's changes through its own `tsm_frame_age`.

The code as it stands (libtsm d1ab10e, kmscon b989c61):

- **Payloads are discarded.** The DCS actions do nothing and APC goes to the ignore state. DA1 replies `?60;1;6;9;15`, without attribute 4 (sixel).
- **Lines are objects.** A line holds its cells and moves whole between the screen and scrollback. Resize grows lines and never reflows them. Anchoring an image to a line makes it travel for free.
- **Clients cannot learn the cell size.** kmscon sets no pixel size with `TIOCSWINSZ`, and libtsm answers no XTWINOPS size report. Image clients size their output from one or the other.
- **kmscon runs as root.** `kmsconvt@.service` has no `User=`, and kmscon starts `/bin/login -p`, which gives the tty to the user who logs in. Every byte kmscon parses comes from programs in the session, remote ones over ssh included, so a file path in a kitty command is chosen by a program the console's user may not trust. The kitty spec's file rules were written for a terminal that runs as its user.

The two protocols disagree about erase. The kitty spec says "the other commands to erase text must have no effect on graphics"; only ED 2 clears images. Sixel comes from terminals with one bitmap, where text written over pixels replaces them: foot (MIT) splits an image into the parts text and erase leave uncovered, and WezTerm and mlterm store image tiles in cells.

Three open issues bear on the renderers: bbulk's damage carry-over keeps every damaged cell damaged on displays with damage clips (#67, being fixed), gltex has no RGBA path until ADR-102 step 6 (#68), and gltex's tier 3 variant is outside the default run (#17). The showcase (#75) asks this record for performance targets and a frame counter.

Upstream's sixel request (kmscon/kmscon#138) carries a proposal to route sixel DCS strings to a decoder through a vte callback, and a pair of patches that decode sixel in libtsm and blit it in kmscon. Alternatives says what this record takes from them.

## Decision

### Image store (libtsm)

- There is one store per screen buffer, main and alternate, as kitty's quota is "per buffer" and Ghostty's limit is per screen.
- An image has:
  - a kitty id (1 to 2^32-1) and image number, or neither for a sixel;
  - an internal key of 64 bits, a slot and a generation, never reused while any frame might name it, as cluster keys are;
  - width, height and premultiplied ARGB32 pixels, the colour glyph format of ADR-102;
  - an opaque flag, set when no pixel is below full alpha;
  - its last use, the transient hint (`N=1`) and its origin, sixel or kitty.
- An image costs width x height x 4 bytes, reserved from the process's image budget before it is allocated (Memory, below). libtsm asks its host for each reservation. Without a host budget, as in `tsm-replay`, a per-screen limit set through the screen applies instead.
- When a reservation would pass the budget, kmscon frees, in order, across every screen it runs:
  1. its own scaled pictures and textures that the current frame does not draw;
  2. images with no placement, transient ones first, least recently used first;
  3. images whose placements all sit on lines in scrollback, least recently used first, together with those placements.
- An image with a placement on a screen's own lines is never evicted. If the new image still does not fit, it is refused: kitty replies ENOSPC, and a sixel is dropped.
- Sixel images are anonymous. No kitty command addresses them, and one is freed when its last placement goes. A kitty image without placements stays until it is deleted or evicted, so a lowercase delete keeps its data, as the spec says.
- Transmitting an id again replaces the image and deletes all its placements (spec).
- With `image-storage-limit=0`, both protocols are parsed only to find where they end, DA1 omits attribute 4, and kitty commands get no reply, so the spec's detection (a query followed by DA1) reports no support.

### Placements (libtsm)

- A placement has:
  - a 64-bit key, never reused, and the key of its image;
  - a kitty placement id, or 0;
  - an anchor: the line holding its top row, and a column;
  - a box in cells (columns, rows), a pixel offset inside its first cell, and a source rectangle in image pixels;
  - a z-index and its kind, sixel or kitty;
  - the cell size in pixels it was made with, the unit of its pixel quantities;
  - an age.
- **Size.** A kitty placement takes `c` and `r` when given. With both, the picture is letterboxed or pillarboxed inside them; with one, the other follows the source's aspect (spec). Without either, and for every sixel, the box is the picture's pixel size divided by the cell size, rounded up.
- **Scale.** The picture draws at its pixel size in the cell size it was made with. When the cell size changes (zoom, or a second display with another font size), the picture scales with its box, so it stays aligned with the text around it.
- **Cell size.** kmscon tells libtsm the cell size in pixels, the same size it reports through `TIOCSWINSZ`. libtsm uses it for sixel cursor movement, kitty box sizes and XTWINOPS replies. A display with a different font size scales pictures by its own cell size.
- **Finding placements.** Each line keeps the keys of the placements anchored on it. The frame walks the visible lines, plus as many lines above the top as the tallest placement has rows, so a placement whose top has left the view still draws its lower rows.
- **Clipping and splitting** change a placement's source rectangle, offset and box. A placement clipped to nothing is removed. This one mechanism serves region scrolling, discarded lines and sixel erase.
- **Replacement.** A kitty placement with the same image id and placement id as an existing one replaces it, which moves a picture without flicker (spec).
- **Order.** Placements fall into three bands by z: below cell backgrounds (z < -1,073,741,824), below text (z < 0), and above text (z >= 0). Within a band they order by z, then by image id, lower first (spec), then by creation. Sixel placements are z 0, after kitty placements at z 0, in creation order.

### Screen events (libtsm)

What each event does to placements. "Part" means the cells of the placement the event touches; removing a part splits the placement into the pieces left over, as figure B shows.

| Event | Sixel placement | Kitty placement |
|---|---|---|
| A character written into a cell | The part under the written cells is removed | No effect |
| A new sixel placed over cells | When the new sixel is opaque (P2 other than 1), the parts of older sixels under it are removed, as a character written there would remove them; a transparent one leaves them | No effect |
| ED 0, ED 1, EL, ECH, DECSED, DECSEL | The part under the erased cells is removed | No effect (spec) |
| ED 2 | Every sixel on the screen's lines is removed | Every placement on the screen's lines is removed, as `a=d` does; the image data stays (spec, "so that the clear command works") |
| ED 3 | Placements anchored on the dropped scrollback lines go | Same |
| ICH, DCH | The part from the cursor to the end of its row is removed: shifting part of a picture by whole cells would tear it | No effect |
| Scrolling the whole screen (LF or IND at the bottom, SU, SD, RI at the top) | Moves with its lines; lines entering scrollback carry their placements | Same (spec) |
| Scrolling a region (margins, IL and DL, which scroll from the cursor's line) | The part inside the moving band moves with it and the part outside stays: the placement splits at the band's edges, and parts pushed out of the region are clipped | A placement wholly inside the band moves and is clipped at its edges; one that straddles an edge stays where it is (spec: "only images that are entirely within the page area must be scrolled") |
| A line discarded (the top line of a region, or scrollback past `sb-size`) | Clipped by that row and anchored on the next line | Same |
| Resize | Lines keep their placements; drawing clips at the right edge and the bottom; lines the height change moves to or from scrollback carry theirs | Same |
| Entering the alternate screen (47, 1047, 1049) | The alternate store is emptied | Same (spec: "all images in the alternate screen must be cleared") |
| Leaving the alternate screen | The alternate store is emptied, and the main screen's placements show again | Same |
| RIS | Both stores are emptied | Same (spec: visible images are cleared on reset) |
| DECSTR | No effect: a soft reset leaves text, so it leaves pictures | Same |
| A kitty delete (`a=d`) | No effect | As the spec's table says |

Emptying the alternate store on leaving it as well as on entering goes beyond the spec, which names only entry, so a full-screen program's images do not hold memory after it exits.

The figure shows the frame's draw order and what one erase does to each kind of placement.

![Two panels. A: the six layers of a frame from the bottom: default background; placements with z below -1,073,741,824; cell backgrounds that are not the default; placements with z from -1,073,741,824 to below 0; glyph and decoration sprites; placements with z of 0 or more and every sixel. B: before and after EL erases a row crossing an orange sixel and a teal kitty image; after it, the sixel has lost that row and remains as two pieces, and the kitty image is unchanged](figures/ADR-103-bands-and-erase.svg)

### Frame interface (libtsm to kmscon)

- `tsm_screen_frame` and `tsm_screen_frame_since` gain a placement list: every placement with a visible row, in draw order. Each entry carries:
  - its key and its image key;
  - its visible row, negative when its top is above the view, and its column;
  - its box, pixel offset and source rectangle, with the cell size they are measured in;
  - its band, and whether its image is opaque;
  - `TSM_FRAME_CHANGED` when it was added, moved, clipped or given new image data since this caller's previous frame, by the caller's `tsm_frame_age`, as cells are marked.
- The entry's size is reported beside the list, as `cell_size` is for cells, so a later libtsm can grow it.
- A caller finds removed placements by comparing keys with its previous frame's list. libtsm cannot report removals per caller, because it does not know its callers: each holds its own age.
- A new call returns an image's width, height, stride, pixels and opaque flag by image key. The pointer is valid until the screen next changes, the same rule as `tsm_screen_frame_cluster`.
- A placement change does not mark the cells under it changed. The renderer damages the placement's rectangle itself.
- `tsm-replay` prints placements and images through the frame, only where the fork's headers declare them (a feature macro), so it still builds against upstream for `make golden REF=v4.8.0`, as ADR-102 arranged.

### Parsing (libtsm)

This replaces ADR-101's Parsing bullet on DCS and APC: payloads are decoded as they stream in rather than collected into buffers. A sixel or kitty payload for a 7680x2160 picture is tens of megabytes of text, and holding it beside the decoded image would double the memory a client can make kmscon spend.

- **DCS** with final byte `q` and no intermediate is sixel. Its bytes go to the decoder as they arrive. Every other DCS stays ignored, as now.
- **APC** whose first byte is `G` is a kitty command.
  - Its control keys are buffered up to 1 KiB. A longer key section makes the whole APC ignored.
  - The payload is base64-decoded as it arrives into the pending image. Raw data may not pass the declared `s` x `v` x bytes per pixel, and PNG or compressed data may not pass `image-storage-limit`. Past either bound, the upload fails with EINVAL and the rest is ignored until ST.
  - Chunks follow the spec: `m=1` until the last chunk, and later chunks carry only `m` and `q`. Any delete command aborts a pending upload (spec), and so does any other graphics command that is not its continuation.
  - Every other APC stays ignored, as now.
- libtsm decodes base64 and raw `f=24` and `f=32` data itself. Files, shared memory, PNG and zlib go to the host (next section).
- **DA1** adds attribute 4 while images are enabled.
- **XTSMGRAPHICS** answers item 1 (colour registers) for read, reset, set within 2 to `num-color-registers`, and read maximum; item 2 (sixel geometry) for read and read maximum, both `max-graphic-size`, and refuses a set with status 3, as xterm refuses changes to graphics sizes. Item 3 (ReGIS) answers status 1.
- **XTWINOPS** answers 14 (text area in pixels), 16 (cell size in pixels) and 18 (text area in cells). kmscon also sets `ws_xpixel` and `ws_ypixel` with every `TIOCSWINSZ`.

### Host interface (libtsm and kmscon)

- The vte gains a host hook for what libtsm does not do: reading `t=f`, `t=t` and `t=s` media, and decoding PNG (`f=100`) and zlib (`o=z`). A request carries the medium, the path or shared memory name, `O`, `S`, the format, the compression and the declared size, or the received bytes for inline PNG and zlib.
- **Input pauses while a request is out.** A new input call returns how many bytes it consumed. kmscon keeps the rest and stops reading that pty until the reply arrives, or until 2 seconds pass, when the request fails with EBADF and the helper is restarted. Commands therefore keep their order (a client may send a put right after a file transmission without waiting for the reply), and kmscon's event loop keeps running, so drawing and VT switching are never blocked.
- The existing input call stays for hosts without the hook. Without it, `t=f`, `t=t` and `t=s` answer EBADF, and PNG and zlib answer EINVAL. `tsm-replay` installs a fixture host that serves files from the test's own directory.

### The image helper (kmscon)

- **Process.** A separate executable, `kmscon-image-helper`, installed beside kmscon's modules. kmscon starts one per terminal session at its first request: it forks, takes on the identity below, and execs the helper, so the helper starts with nothing of kmscon's memory.
- **Identity.** The session's user, never the foreground process's:
  - When kmscon is built with sd-login (systemd's or elogind's) and the process it started is in a logind session, the session's uid: `sd_pid_get_session` on that process, then `sd_session_get_uid`. pam_systemd registers the session when login authenticates the user.
  - Otherwise (built without sd-login, no logind running, or a command kmscon started without login), the owner of the session's pty, as `/bin/login` sets it, read with `fstat` on the pty's slave.
  - logind's answer comes first because it is the identity PAM authenticated, held by logind for the session's life. The pty's owner is file metadata: any root process in the session, a `sudo chown` among them, can hand the pty to another user, root included, and every program in the session would then have files read as that user. When the two disagree, kmscon logs it and uses logind's.
  - kmscon uses systemd today only to find its unit directory (meson.build). sd-login becomes an optional dependency, and a build without it uses the pty's owner.
  - The helper takes that uid, its primary gid and the user's supplementary groups (`initgroups`), then `PR_SET_NO_NEW_PRIVS`, working directory `/`, every descriptor closed but its socket, and a memory limit derived from the image limits.
  - When the session's user is root (root logged in, or no login and the pty still root's), the helper runs as root, which is then the session's identity.
- **User changes.** Before each request kmscon finds the session's user again. When it differs from the helper's (a logout and a new login), the helper is stopped and a new one started.
- **Precedent.** Desktop terminals (kitty, Ghostty, WezTerm, foot, Konsole) run as the desktop user, so `sudo mpv --vo=kitty` inside one still has the terminal open files as that user, and root-only files fail to load. sudoedit opens files as the invoking user, and OpenSSH's privilege separation parses untrusted input in an unprivileged process. kmscon matches the desktop terminals and introduces no convention of its own.
- **Font paths.** OSC 50 font paths (#77) are opened by the same helper, with the same identity and path rules. What it returns for a font is settled with #77.
- **Files** (`t=f`, `t=t`):
  - Before opening, the path is resolved (`realpath`, following symlinks, failing on loops, as the spec asks) and refused when it lies in `/proc`, `/sys` or `/dev` other than `/dev/shm`. `stat` must show a regular file. Checking before opening honours the spec's warning that opening can have side effects.
  - After opening (`O_RDONLY`, `O_NOCTTY`, `O_NONBLOCK`), `fstat` must show a regular file again, and the path of the opened descriptor must pass the same check. This is Ghostty's check: it catches a symlink swapped between the first check and the open.
  - `O` is the offset and `S` the exact number of bytes to read; a shorter file fails.
  - `t=t` is read only when the resolved path lies in `/tmp`, `/dev/shm` or kmscon's `TMPDIR` and contains `tty-graphics-protocol`, and the file is deleted after reading. Any other temporary file is refused, as Ghostty refuses it. The spec only forbids deleting such a file; refusing it keeps a command from reading a file it asked to have deleted.
- **Shared memory** (`t=s`): the name must start with `/`, contain no other `/`, and be at most 255 bytes. It is opened read-only as the user, must be at least `O` + `S` bytes, and is unlinked and closed once opened, whether the read succeeds or not.
- **One failure reply.** Every failure to read (missing, unreadable, not a regular file, a refused location, too short) answers the same bytes, `EBADF:Failed to read image file`, the reply the spec gives as kitty's. The reason goes to kmscon's log.
- **Decoding.** PNG through libpng, and zlib through zlib, in the helper. FreeType already links both, so kmscon gains no dependency. A PNG's header is read and its size checked against `max-graphic-size` before any pixels are allocated.
- **Result.** The pixels come back in a sealed memfd over the helper's socket. kmscon maps it read-only, checks that its size is exactly width x height x 4 and the size within the limits, and stores it. One request per session is out at a time.

### Sixel (libtsm)

- The decoder is written from DEC STD 070 and the VT340 manual. foot (MIT) and libsixel (MIT) are references; anything adapted from them names its source and licence in the file and the commit (ADR-300).
- **Introducer** `DCS P1;P2;P3 q`:
  - P1 chooses the pixel aspect: 0 and 1 give 2:1, 2 gives 5:1, 3 and 4 give 3:1, 5 and 6 give 2:1, 7 to 9 give 1:1. Raster attributes (`"Pan;Pad;Ph;Pv`) override it until the first sixel data and are ignored after it; `Ph` and `Pv` size the opaque area and let the decoder allocate once.
  - P2 of 1 leaves unset pixels transparent; 0 and 2 give them colour register 0.
  - P3, the grid size, is ignored, as xterm ignores it.
- **Data:** `#Pc;Pu;Px;Py;Pz` selects or defines a register, in HLS (`Pu=1`, hue in degrees with blue at 0) or RGB percentages (`Pu=2`); `!Pn` repeats; `$` returns to the band's start; `-` starts the next band. Other bytes are ignored. A register number never indexes outside the table.
- **Registers:** `num-color-registers` of them, 0 to 15 starting from the VT340's default colour map. With mode 1070 set (its initial state from `private-color-registers`), each image starts from the defaults; reset, registers are shared and persist across images until RIS.
- **Limits:** the decoded width and height stop at `max-graphic-size`, and pixels past it are discarded. xterm's `maxGraphicSize` likewise bounds "the maximum size of a graph which can be displayed". A repeat count is bounded by the same width.
- The image is created and placed when ST ends the DCS. CAN or SUB discards it.
- **Placement and cursor,** with sixel scrolling on (DECSDM reset, the default):
  - the picture's top left is the cursor's cell, and its box is its pixel size over the cell size, rounded up;
  - the screen scrolls, as index does, whenever the picture would pass the bottom margin;
  - the cursor ends on the text row holding the picture's last pixel row, at the picture's left column (foot), or with mode 8452 set, in the column after the picture's right edge on that row, clamped to the last column;
  - a picture wider than the columns left is clipped at the right edge.
- With DECSDM set, the picture is placed at the screen's top left, clipped to the screen, and the cursor does not move (foot).
- **DECSDM's sense follows xterm:** set turns sixel scrolling off. xterm's ctlseqs notes that the VT330/VT340 graphics manual (second edition) says the opposite, while the VT382 manuals say "Disable sixel scroll" and "No Sixel scrolling"; xterm's `sixelScrolling` resource is documented as the opposite of DECSDM. foot follows xterm's sense, and so does this record.
- Text already in the covered cells stays and shows through transparent pixels.
- Cursor positions in the tier 1 goldens are checked against xterm, MIT-X11 licensed, running the same bytes as a black box and answering DSR.

### Kitty graphics protocol (libtsm, with the host for media)

- **Actions:** `t` (transmit), `T` (transmit and place), `p` (place), `q` (query) and `d` (delete). Animation (`a=f`, `a=a`, `a=c`), relative placements (`P`, `Q`, `H`, `V`) and virtual placements for Unicode placeholders (`U=1`) answer EINVAL until step 5.
- **Formats:** `f=24`, `f=32` (the default) and `f=100` (PNG, through the host); `o=z` through the host. Raw formats need `s` and `v`, and their data, after decompression, must be exactly `s` x `v` x bytes per pixel.
- **Media:** `t=d` in libtsm, chunked or whole; `t=f`, `t=t` and `t=s` through the host.
- **Ids:** `i` names an image. `I` names the newest image with that number, and the terminal picks an unused id for it, reported as `i=<id>,I=<n>`. Both in one command is EINVAL (spec).
- **Placement keys:** `x`, `y`, `w`, `h` choose the source rectangle, cut to the image; `X`, `Y` offset within the first cell and must be smaller than the cell (EINVAL otherwise); `c`, `r`, `z`, `p`; `C=1` leaves the cursor where it is.
- **Position:** the cursor's position when the final chunk arrives (spec).
- **Cursor after a placement** (`C=0`), Ghostty's reading of the spec: index (scrolling as needed) one row fewer than the box's rows, bounded by the distance to the bottom margin plus one screen; then move to the column after the box, or, when that reaches the right edge, index once more and move to column 0.
- **Deletes:** `d` of `a`, `i` (with `p`), `n`, `c`, `p`, `q`, `r`, `x`, `y` and `z`, in both cases, as the spec's table says, with no `d` meaning `a`. "Visible on screen" means on the screen's own lines, not scrollback. An uppercase delete frees an image left with no placement anywhere, scrollback included. `d=f` and `d=F` delete nothing until animation exists. Kitty deletes never touch sixel placements.
- **Replies** go back through the vte's write callback, as DA replies do: `ESC _ G i=<id>[,I=<n>][,p=<pid>];OK ESC \`, or `;CODE:message`. A reply is sent only when the command carries `i` or `I` (spec). `q=1` suppresses OK and `q=2` suppresses errors. `a=q` loads and checks the data without storing it and replies as a transmission would. The codes are OK, ENOENT (no such image), EINVAL (bad or conflicting keys, an unsupported action or format), EBADF (any read failure, one identical message) and ENOSPC (the storage limit).

### Composition (kmscon)

A frame is drawn in ADR-101's order, shown in figure A: the default background; placements below cell backgrounds; cell backgrounds that are not the default; placements below text; glyph and decoration sprites; placements above text, including every sixel. The lowest band shows only through cells whose background is the default (spec).

### bbulk

- A cell no placement covers draws as it does today.
- A cell some placement covers is composed in system memory in the order above and sent as one opaque ARGB request, the path decorated cells already take.
- **Opaque fast path.** A cell wholly under an opaque placement that nothing draws over takes the picture's pixels as they are: no glyph or decoration, no higher placement, and, when the placement is in the lowest band, a default background. Runs of such cells go as row strips. A full-screen video with a banner over its bottom rows is then strips for the picture and per-cell composition for the banner's cells only.
- **Scaled pictures** are cached per image key and drawn size: box filter down (ADR-102's filter), bilinear up. The cache holds at most twice the screen's size in ARGB and drops the least recently used.
- **Damage.** bbulk keeps this display's previous placement list. A placement added, removed, moved, resized, clipped differently or given new image data damages its old and new rectangles. The cells there are drawn on this frame and the next, the two-frame rule cells follow. This depends on #67's fix and is tested on displays with and without damage clips.
- Rotated screens rotate pictures with the cells.

### gltex

- Images come to gltex after ADR-102 step 6 gives it an RGBA path and an untinted shader (#68), not before.
- Each image key gets a texture, uploaded when a placement of it is first drawn. An image larger than `GL_MAX_TEXTURE_SIZE` is uploaded scaled down to fit. Textures of images not drawn recently are freed once the total passes the bound bbulk's cache uses.
- One draw per layer: clear to the default background, lowest-band quads, non-default cell backgrounds, middle-band quads, glyphs and decorations, top-band quads.
- Image uploads take the same path that fails under virgl in #17, so the GL variant's image screens stay outside the default tier 3 run until #17 is resolved.

### Memory

- **One budget per kmscon process.** `image-storage-limit` covers every image in every screen's stores, main and alternate, across all the process's sessions, and the renderers' scaled pictures (bbulk) or textures (gltex). kmsconvt@ runs one session per process, so it is also the ceiling per VT.
- **Default, `auto`:** four frames of the process's displays in ARGB (width x height x 4, added over its displays), at least 16 MiB and at most 192 MiB, recomputed when a display is added, removed or changes mode. A smaller budget evicts down to fit.

| Display | One ARGB frame | Default budget |
|---|---|---|
| 1024x768 | 3.0 MiB | 16 MiB (the floor) |
| 1366x768 | 4.0 MiB | 16 MiB |
| 1920x1080 | 7.9 MiB | 31.6 MiB |
| 3840x2160 | 31.6 MiB | 126.6 MiB |
| 7680x2160 | 63.3 MiB | 192 MiB (the cap) |

- **Why four frames.** Full-screen video is the largest steady case. Its peak is three frames: the picture still placed, the next one being decoded, and one scaled copy when the picture is drawn at another size. Four leaves one frame for everything else on screen. At 7680x2160 the cap holds just over three frames, still the video peak. On the small tier 3 machine (512 MB at 1366x768) the default is 16 MiB, about 3% of its memory.
- **Raising it.** `image-storage-limit=<bytes>` in kmscon.conf replaces `auto` with a fixed budget, any size, for galleries or many large pictures at once; `0` turns images off.
- **The helper** is a separate process. It decodes one image at a time and refuses, from the PNG header and before allocating, any image whose decoded size would not fit the budget or `max-graphic-size`.

### Render timer (kmscon)

- `render-timer=none|osd|log|both` in kmscon.conf, foot's option with foot's values; the default is `none`.
- For each display it reports:
  - frames presented per second, counted as page flips complete;
  - CPU time per frame, kmscon's thread CPU time from preparing the frame to queuing its flip;
  - damaged area per frame, the pixels in the frame's damage rectangles and their share of the screen.
- `log` writes one line per display per second to kmscon's log. `osd` draws the figures in a corner as a top-band placement that kmscon appends to the display's placement list after libtsm's. It passes through the same composition, damage and blit as any picture, and it belongs to no libtsm store, so no program can erase, delete or replace it. Its own redraw, once a second, is part of what it reports.
- It measures what kmscon presents. mpv's own statistics report what mpv decodes and outputs; a frame mpv outputs that kmscon never presents shows as the gap between the two rates.
- Tier 2 and tier 3 goldens are taken with it off.

### Acceptance scenarios

bbulk composes on the CPU. Most consoles are 1366x768 or 1920x1080 on modest machines; 7680x2160, 16.6 million pixels and 66 MB per ARGB frame, is the stress case ADR-101 named. Every scenario runs on drm2d with bbulk at 1366x768, 1920x1080 and 7680x2160, in tier 3 and on the operator's hardware, with the render timer on.

**Machines.** Tier 3 gains a small variant, `2d-1366x768-hamster`: 1 vCPU, 512 MB of memory, 1366x768, drm2d and bbulk. It runs the image screens and the S1 to S6 scenarios on every tier 3 run, so a regression on small machines shows in the harness. Its vCPU is a core of the host, so it bounds parallelism and memory rather than reproducing slow hardware; real low-end hardware is measured when it is at hand.

**Synthetic scenarios,** drawn through bbulk into the in-memory display tier 2 uses and through drm2d in tier 3:

- **S1.** Text scrolling, no image: the baseline against main.
- **S2.** Text scrolling in a region beside a static half-screen picture.
- **S3.** One line of scroll with a full-width picture on screen, so every cell under it moves.
- **S4.** A full-screen opaque picture replaced every frame, in the top band.
- **S5.** S4 in the middle band, under a text band that scrolls every frame: the showcase's banner.
- **S6.** Fifty small pictures.

**Programs.** Each run records frames presented per second, CPU time per frame and damaged area per frame (median and 95th percentile), with mpv's output frame rate beside them where mpv plays.

Step 3, sixel:

- **A1.** img2sixel pictures: placed, scrolled into scrollback and back, cleared.
- **A2.** `mpv --vo=sixel` playing an animation full screen, Bad Apple!! for the showcase.
- **A3.** The showcase's act 5: a small sixel video, then a deliberate glitch and a halt. Clearing the screen afterwards leaves no stale pixels.
- **A4,** step 3's final scenario: tmux, which supports sixel from 3.4 when built with `--enable-sixel` (Arch builds it so), in a 2x2 and then a 3x3 grid of panes, each running `mpv --vo=sixel`. tmux sends every pane's pictures on to kmscon, so many sixel streams and placements change at once.

Step 4, kitty:

- **A5.** `mpv --vo=kitty`, inline (its default, chunked through the pty) and with `--vo-kitty-use-shm=yes` (shared memory, `t=s`, through the helper).
- **A6,** the showcase's finale: a video full screen through the kitty protocol with a scrolling banner of text and colour emoji over its bottom rows, the text moving every frame while the picture changes every frame. It runs twice: the picture below cell backgrounds (z < -1,073,741,824), where the banner's coloured backgrounds cover it, and below text only (-1,073,741,824 <= z < 0), where only glyphs cover it. mpv's manual documents no z-index option for `vo=kitty`, so the case uses the spec-written client to place the picture; the showcase may use either that or mpv sized to leave the banner's rows free (`--vo-kitty-rows`).
- **A7.** Several videos at once as separate kitty placements from one client, with no multiplexer.
- The same grid of kitty videos inside tmux needs mpv's multiplexer passthrough (`--vo-kitty-auto-multiplexer-passthrough`) and Unicode placeholders to keep each picture in its pane, so it is step 5's final scenario.

The public post's footage is captured with ffmpeg's `kmsgrab` device. Runs with capture are recorded apart from runs without it, since reading the scanout buffer takes time on the same machine.

### Targets

- **Common sizes, on the small machine** (`2d-1366x768-hamster`, and the same limits at 1920x1080 on one vCPU):
  - S1: within 5% of main at every size, on every variant.
  - S4: kmscon's CPU time per frame at most 10 ms at 1366x768 and 16 ms at 1920x1080. At 30 frames per second that is 30% and 48% of the one vCPU, leaving the rest to the program sending the video.
  - S5: at most one and a half times S4's figure at the same size.
  - S3: at least 30 lines per second.
  - A2 and A5 at 1366x768 and 1920x1080: the render timer's presented rate matches mpv's output rate, on the small machine and on the operator's.
- **Basis.** A frame is 4.0 MiB at 1366x768 and 7.9 MiB at 1920x1080; at 30 frames per second bbulk writes about 120 and 240 MiB per second, which one core should sustain; that is an estimate, not a measurement. The figures are first estimates. 3b's measurement confirms or replaces them in its evidence record, and the scenarios are judged against what that record states.
- **The stress size,** 7680x2160: no number can be justified before measuring. Each frame writes 66 MB to the scanout buffer, and that hardware's write bandwidth has not been measured. The criterion that holds now: kmscon is not the bottleneck. In A2, A5 and A6 the presented rate matches mpv's output rate (or the client's send rate in A6), and kmscon's CPU time per frame stays below the frame interval. 3b's measurement sets the number.
- **If a target is missed,** the remedies in order: splitting composition across threads by rows, SIMD scaling and blending, and gltex for video where GL is available. On a one-CPU machine only the last two help. The remedy chosen is a decision that cites the evidence record.

### Damage for moving text over a changing picture

- A placement whose image changes damages its whole rectangle on that frame and the next. In A2, A5 and A6 that is the whole picture every frame, so damage saves nothing there; the cost is composition, which CPU time per frame shows.
- The banner's cells change every frame and are damaged as cells always are. The union is the frame's damage.
- Within that damage, the opaque fast path sends the cells nothing draws over as strips, and only the banner's cells are composed one by one: picture, then a non-default background in the lowest-band run, then glyph and decorations. The per-frame cost is one strip copy of the picture plus the banner's rows.
- Both depend on two prerequisites: #67's fix, without which every damaged cell stays damaged on every later frame, and placement damage (3b).

### Settings (kmscon.conf)

Each setting takes the name an existing terminal gives it, written in kmscon.conf's lower-case, hyphenated form.

| Setting | Named after | Default | Meaning |
|---|---|---|---|
| `image-storage-limit` | Ghostty `image-storage-limit` | `auto` (Memory, above) | Bytes for all images and scaled pictures in the kmscon process. 0 turns sixel and kitty graphics off and removes DA1 attribute 4. Ghostty's applies per screen with a 320 MB default; kmscon's covers the process, so one number bounds it. |
| `max-graphic-size` | xterm `maxGraphicSize` | 10000x10000 | The largest image in pixels, for both protocols. xterm's 1000x1000 is smaller than this console's screens; 10000 is Ghostty's dimension limit. |
| `num-color-registers` | xterm `numColorRegisters` | 1024 (xterm's value for terminal ids that are not DEC models) | Sixel colour registers. |
| `private-color-registers` | xterm `privateColorRegisters` | true | The initial state of mode 1070. |
| `render-timer` | foot `render-timer` | none | `osd`, `log` or `both` turn on the render timer. |

### Testing (ADR-301)

- **Tier 1** (screen state):
  - `tsm-replay` prints each placement (numbered in draw order, so keys do not enter goldens) with its anchor row and column, screen or scrollback, box, offset, source rectangle, z and kind; each image's size and a digest of its pixels; the cursor; and the replies, which it already prints.
  - Cases cover every row of the screen-events table for both kinds; the sixel decoder (aspect, raster attributes, HLS and RGB, repeats, shared and private registers, P2, truncation at `max-graphic-size`, DECSDM both ways, mode 8452); kitty transmission (whole and chunked, `f=24` and `f=32`, an upload aborted by a delete, `i` and `I`, replacement); placement (`c` and `r`, source rectangles, offsets, `C=1`, each band, cursor movement at the bottom margin and the right edge); every delete specifier in both cases; replies under `q=0`, `1` and `2`; `a=q`; eviction order and ENOSPC; the alternate screen and RIS.
  - Streams are recorded from img2sixel (libsixel, MIT) and from a kitty client the fork writes from the spec alone (ADR-300).
  - Goldens come from the fork, as upstream has no images, and each is reviewed in its own commit. The DA1 goldens change in their own commit.
- **Unit tests** (kmscon) for the helper: each refusal (`/proc`, `/sys`, `/dev`, a FIFO, a socket, a device, a directory, a symlink loop, a symlink swapped between check and open, a file shorter than `S`, `t=t` outside a temporary directory or without the name, shared memory names with extra slashes or too long) answers byte-identical EBADF; refused temporary files survive; accepted `t=t` files and `t=s` objects are removed.
- **Fuzzing.** The sixel decoder and the kitty command parser run in kmscon's root process on bytes any program sends. Each has a fuzz target, run for a fixed budget before a change to it lands.
- **Tier 2** (pixels): each band with text over and under it; the lowest band under default and non-default backgrounds; transparent and opaque sixels; a sixel split by text; a region scroll clipping a placement; scaled and letterboxed placements; a changed cell size. Incremental frames on a display with damage clips: a placement added, moved and removed leaves no stale pixels, the failure that ruled out the community patch (ADR-101); a changing picture under a text band that scrolls every frame, as A6; a sixel cleared by ED 2 after a run of frames, as A3.
- **Tier 3** (VM): the guest gains img2sixel (libsixel) and viu (MIT), both in Arch's extra repository, and the fork's spec-written client for `t=f`, `t=t` and `t=s`. Screens: a picture from img2sixel, scrolled into scrollback and back, then cleared; kitty pictures through each medium; text over a middle-band picture; a root-owned file refused to the logged-in user, also under `sudo`; the acceptance scenarios that tier 3 can run (mpv, tmux), with the render timer logging. bbulk variants first, `2d-1366x768-hamster` among them; GL after #17 and ADR-102 step 6.
- **Demo.** The VT demo gains an images section, and its screens become tier 3 screens as the others did. The showcase's acts 5 and 7 (#75) run on top.

### Order of work

Each item lands behind the harness, with goldens changed only in their own commits.

Step 3, the image layer and sixel:

1. **3a, libtsm store and placements.** The store, placements, every screen event in the table, the frame's placement list, the image call, and the cell size from the host. Unit tests build placements through a test-only call; `tsm-replay` prints them.
2. **3b, kmscon composition, the render timer, and the first measurement.** bbulk's bands, scaled-picture cache, opaque fast path and placement damage, after #67. The render timer comes next, as the first placement bbulk draws, so the measuring tool exists before anything it measures. Then the harness change: `fork/tier3` variants gain their own vCPU count and memory, which are fixed at 4 and 4096 MB for every variant today (`t3_qemu.py`), and `2d-1366x768-hamster` joins the default run. Then S1 to S6 at 1366x768, 1920x1080 and 7680x2160, recorded as an evidence record that confirms or replaces the targets. Tier 2 screens for each band draw placements made through libtsm's test-only call.
3. **3c, libtsm sixel.** DCS streaming, the decoder, registers, modes 80, 1070 and 8452, DA1 attribute 4, XTSMGRAPHICS, XTWINOPS 14, 16 and 18. A fuzz target. Tier 1 cases from img2sixel streams.
4. **3d, kmscon wiring.** The cell size to libtsm and `TIOCSWINSZ`, and the settings. Tier 2 sixel screens.
5. **3e, acceptance.** Tier 3 sixel screens and the demo section; A1, A2 and A3; then A4, step 3's final scenario.
6. **3f, gltex images,** after ADR-102 step 6 and #17.

Step 4, the kitty graphics protocol:

1. **4a, libtsm kitty.** APC framing, control keys, inline `f=24` and `f=32`, chunking, the actions, ids, numbers and placement ids, bands, cursor movement, deletes, replies, and eviction with ENOSPC. A fuzz target. Tier 1 through the spec-written client. Without a host, the other media answer EBADF and PNG and zlib EINVAL.
2. **4b, the host and the helper.** The vte's host hook and the input call that reports what it consumed; `kmscon-image-helper` with its identity, file and shared memory rules, PNG and zlib, and memfd results. Unit tests for every rule, and the fixture host in `tsm-replay`.
3. **4c, kmscon.** Tier 2 screens for the lower two bands, text over pictures, and A6's banner over a changing picture.
4. **4d, acceptance.** Tier 3 with viu and the spec-written client through each medium, the root-file refusal, and the demo; A5 through both media, A6 in both bands, and A7.
5. **4e, gltex** for the lower two bands, with 3f.

Step 5 has its own record. Its final scenario is A4 again with `mpv --vo=kitty` in each pane.

## Consequences

### Positive

- img2sixel, viu and other image tools work on the console, and their pictures follow the text through scrolling and scrollback, clear with `clear`, and leave no stale pixels.
- kmscon, running as root, never opens a path a program names and never runs a PNG or zlib decoder. What a client can read or delete is bounded by the logged-in user's own permissions.
- Both protocols share one store and one placement model. Step 5 adds Unicode placeholders and relative placements to them rather than beside them.
- The settings carry names xterm and Ghostty users already know.
- libtsm gains no dependency: everything it cannot decode itself goes to the host.

### Negative

- A program under `sudo` cannot show root-only files through the file media, as in a desktop terminal.
- Finding the session's user through logind adds an optional dependency on sd-login.
- A VT's output pauses while a helper request is out. A slow file system stalls that VT for up to 2 seconds per command.
- The helper is a new executable to package, and a new process for each session that shows a file or PNG image.
- What an erase does to a picture depends on which protocol drew it. A reader who does not know the source cannot predict what `EL` does.
- ICH and DCH remove part of a sixel instead of shifting it.
- CPU composition at 7680x2160 is unmeasured, and the acceptance scenarios have no frame-rate target until 3b measures one. Video on bbulk may fall short at that size.
- A program can still fill the budget, up to 192 MiB at the default on the largest displays; a small default also means a gallery of large pictures evicts sooner, until the user raises `image-storage-limit`.
- The budget is shared, so a program on one screen can evict another screen's unplaced or scrollback images.

### Neutral

- DA1 changes for every program. Programs that look for attribute 4 start sending sixel.
- Animation, relative placements and Unicode placeholders answer EINVAL until step 5, so clients can tell they are missing.
- The image goldens come from the fork alone; upstream libtsm cannot judge them.
- Pictures scale with the cell size, so a zoomed console shows them larger, and a second display with another font size draws them at its own scale.

## Alternatives Considered

- **The patches attached to kmscon/kmscon#138.** As their commit messages describe them, they buffer a DCS payload up to 1 MiB and decode a sixel at its end; anchor each image to the line holding its top row, the anchor this record also uses; skip drawing an image whose anchor line has scrolled into scrollback or while the alternate screen is shown; drop a whole image when text, an erase or a new image touches its cells; and blit images after the glyph pass through a new display operation, without rotation and with no gltex path. Their author reports artifacts on drm2d with bbulk. Not adopted: 1 MiB holds only a small picture; a picture that vanishes once its top line enters scrollback breaks scrolling back; dropping a whole image on any overwrite loses pictures kitty clients draw text over; and a blit after the glyphs, outside the renderer's damage, is what leaves stale pixels. They carry no licence statement, so they were read as prior art and no code was taken.
- **A sixel callback out of the vte** (trollyjolly on #138), handing DCS strings that start with a sixel introducer to a decoder through a new `tsm_vte` callback. Taken in part: Parsing routes by the introducer. The decoder and the store stay in libtsm, because placements must follow screen events the host never sees; the callback shape serves what libtsm cannot do itself, the host hook for files and PNG.
- **Collect each DCS and APC payload whole, then decode,** as ADR-101 worded it. Rejected: an 8K picture's encoded payload is tens of megabytes held beside the decoded image. Streaming bounds memory by the decoded result.
- **Store image tiles in cells** (mlterm, WezTerm). Rejected by ADR-101. Splitting placements reproduces sixel's cell behaviour without it.
- **One erase rule for both protocols.** Kitty's rule for sixel lets a picture survive text written over it, which no DEC-derived terminal does, and leaves stale pictures under programs that redraw over their sixel output. Sixel's rule for kitty breaks the spec's statement that erase commands do not affect graphics, and removes pictures that kitty clients draw text over on purpose.
- **Open files in kmscon itself, with only the spec's checks,** as a terminal running as its user does. Rejected: as root, a remote program could have the console display any file on the machine as raw pixels, and a `t=t` command would delete as root.
- **Switch kmscon's file-system identity to the user for each open** (`setfsuid`), with no helper. Cheaper, but the process stays root, PNG and zlib still decode as root, and the identity switch is per thread in a process whose libraries may start threads.
- **Open in the helper but decode in kmscon,** the narrowest reading of the operator's direction. Rejected for PNG and zlib, library decoders whose faults would run as root. Sixel and base64 stay in kmscon: they are byte state machines written in the fork and fuzzed like the parser.
- **The foreground process's identity for the helper.** Rejected by the operator's answer to the sudo-identity probe, and by precedent: desktop terminals read as their desktop user under `sudo`. The foreground process group can also belong to a setuid program and can change between the command and the open.
- **The pty's owner as the only source of the session's user.** No new dependency, and it is what login sets. It stays as the fallback. Rejected as the first source because it is file metadata a root process in the session can change, which would change the identity every program in the session reads with.
- **Refuse the file media,** as some terminals do. Rejected by the operator: all four media are supported.
- **Call the helper synchronously.** Simpler, but it blocks kmscon's event loop, and with it drawing and VT switching, for up to the timeout.
- **libtsm reports removed placements to each caller.** Rejected: libtsm does not know its callers, and keeping removals until every caller has seen them needs that knowledge.
- **A vendored PNG decoder** (stb_image, spng). Rejected: libpng and zlib are already loaded through FreeType, and a vendored decoder is code the fork would maintain.
- **Ghostty's default: 320 MB per screen.** Rejected by the operator's answer: on a small machine one VT could hold 640 MB, more than the machine has.
- **A fixed small default, the same on every machine.** Simple and predictable, but a number that suits 1366x768 starves a 4K or 8K console's full-screen video, and one that suits 8K is too large for a small machine. Scaling with the displays gives each the video case's peak.
- **A separate setting for the scaled-picture cache.** Rejected: two limits give two ceilings to reason about, and the operator asked for one predictable one.
- **xterm's default `max-graphic-size` of 1000x1000.** Rejected: it is about an eighth of this console's width, and full-screen images are the case the showcase exists for.

## Notes (2026-10-23)

- The small tier 3 variant this record calls `2d-1366x768-hamster` is named `2d-1366x768-potato`, the potato computer, at the operator's request: "We should call it potato computer not hamster." It is the same machine (1 vCPU, 512 MB, 1366x768, drm2d and bbulk), and what this record decides about it stands.

## Notes (2026-10-23, operator answers from the step 3a and 3c reviews)

Recorded as considered entries above; what they change:

- **A budget is required.** The sixel decoder refuses to run without a reserve callback and reserves the image's full size before decoding into it, with no transient use above the reservation. libtsm with no host budget defaults its per-screen fallback limit to 0, so images are off unless a host sets a budget (tsm-replay sets its own). The display size sets the ceiling (`image-storage-limit=auto`); memory is reserved as images arrive and released when they go, not held.
- **Each protocol can be switched off.** Two settings follow kmscon's boolean convention (`CONF_OPTION_BOOL`, as `blink`: `--sixel`/`--no-sixel`, `sixel`/`no-sixel` in kmscon.conf): `sixel` and `kitty-graphics`, both on by default. A protocol that is off is neither advertised (DA1 attribute 4 for sixel) nor decoded. `image-storage-limit=0` still turns both off.
- **Sixel aspect ratio** follows what most terminals do. The survey in libtsm #16: xterm 411 (`-ti vt340`, measured as a black box), WezTerm and mlterm do not apply P1's pixel aspect or the raster attributes' Pan;Pad; foot applies both per axis. So one sixel dot is one pixel: P1 and Pan;Pad are read and not applied, and Ph;Pv still size the picture. This replaces the Sixel section's "P1 chooses the pixel aspect". No setting is added, since the terminals do not split.
- **ED 3 clears the scrollback**, as in xterm; the linux and xterm-256color terminfo entries carry `E3`, so `clear` wipes it.
- **The test-only image API** leaves libtsm's exported symbols and moves to a test-only file linked by the tests and tsm-replay; no test is removed. Once sixel is wired into the parser (3c), the tier 1 image cases move from the private escape to real sixel sequences where they can.

## Notes (2026-10-23, operator answers from kmscon #127 and the step 3c review)

Recorded as considered entries above; what they change:

- **The scaled pictures stay within the image budget now** ("fix it in 3b"). Until the setting and the budget shared with libtsm's stores land in step 3d (#125), bbulk computes `image-storage-limit=auto` from the displays it draws (four frames in ARGB, 16 MiB to 192 MiB) and keeps every display's scaled pictures within it at every moment, a frame's peak included. Between frames a display keeps at most twice its screen, or its share of the budget by size if that is less. A picture that would pass the budget is left out of that frame and its cells drawn again on later frames until it fits. This replaces the bbulk section's bound of "twice the screen's size" as the only limit: at 7680x2160 a frame could otherwise hold four screens, 265 MB, past the cap.
- **Targets** ("adopt and tighten others"). ADR-104 measured them. The figures below replace the Targets section's common-size numbers. The ADR-104 figures come from one core of the host the harness runs on, which is also what the small tier 3 machine's single vCPU is, and that core is fast. These targets leave about five times headroom over the measured 95th percentile, for the slower cores of the low-end laptops and single-board computers the small machine stands for. They still fail a regression that slows composition several times over.
  - S4: at most 4 ms per frame at 1366x768 (measured 0.68 ms median, 0.83 ms 95th percentile) and 8 ms at 1920x1080 (1.42 ms and 1.70 ms). At 30 frames per second that is 12% and 24% of one core.
  - S5: at most 1.25 times S4 at the same size (measured 1.16 and 1.10 times). A banner over a full-screen picture should cost little more than the picture.
  - S3: at least 60 lines per second, one line per 60 Hz frame (the renderer alone allows over 500).
  - S1: within 5% of main, unchanged (measured +0.8% to +2.4%).
  - 7680x2160, adopted from ADR-104: S4 and S5 at most 16.7 ms per frame, one 60 Hz frame interval (measured 11.9 ms and 12.5 ms median, at most 14.4 ms at the 95th percentile). This replaces "no number until the first measurement".
- **Sixel cursor placement follows foot, not xterm** ("Xterm seems incorrect, pushing an unwanted newline"). xterm 411, run as a black box, matched 6 of 8 cursor cases. It scrolls one extra row at the bottom margin, and with mode 8452 it wraps to the next line at the right edge. The Sixel section's foot rules stand. Where xterm and foot differ, foot's rule is the reference, not xterm's black-box answer.
- **The sixel decoder keeps its per-call work bound** and adds no total cap per byte.
- **XTSMGRAPHICS:** a register count outside 2 to the maximum answers status 3. **XTWINOPS 14 and 16** answer even with sixel off.
