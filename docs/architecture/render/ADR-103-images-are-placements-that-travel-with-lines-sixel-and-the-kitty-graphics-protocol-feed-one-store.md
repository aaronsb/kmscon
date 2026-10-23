---
contract: adr/v1
kind: decision
verb: add
capability: images
basis:
  - operator: aaronsb
    level: directed
    paraphrase: true
    said: "Support all four kitty transmission media (direct, file, temporary file, shared memory). Invent no new security conventions: kmscon runs as root, so file and shared-memory opens happen in a helper running with the session user's identity, the owner of the session's tty as login sets it, with sudoedit and OpenSSH privilege separation as precedent, plus the kitty spec's own file rules."
    via: relayed by the team lead in the brief for this record, 2026-10-22; not the operator's own words
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
  - evidence: "aaronsb/kmscon issues #17 (gltex upload failures under virgl), #67 (bbulk damage carry-over, being fixed), #68 (gltex has no RGBA path until ADR-102 step 6), #75 (the showcase asks this record for performance targets and a frame counter)"
agent:
  name: claude
  model: claude-opus-5-5
status: proposed
date: 2026-10-22
deciders:
  - aaronsb
related: []
extends: ADR-101
amends: "ADR-101#parsing"
observable:
  - "img2sixel picture.png on a kmscon VT shows the picture under the command; Shift+PageUp scrolls it away and back with the text; clear removes it and leaves no pixels behind"
  - "viu picture.png shows the picture through the kitty graphics protocol, inline and from a temporary file"
  - "A kitty client run by the logged-in user that names a root-only file with t=f gets EBADF:Failed to read image file, the same reply as for a missing file"
  - "The tier 2 screens for each composition band, and the benchmark scenarios S1 to S6 recorded as an evidence record"
---

# ADR-103: Images are placements that travel with lines; sixel and the kitty graphics protocol feed one store

## Summary

- **Decided:** Steps 3 and 4 of ADR-101, made concrete.
  - **Store:** one image store per screen buffer, capped by `image-storage-limit` (320 MB, the kitty and Ghostty default). A new image evicts images with no placement first, then images placed only in scrollback. An image placed on the screen is never evicted; a new image that still does not fit is refused.
  - **Placements:** each is anchored to the line holding its top row, so it travels into scrollback and back with the text. A sixel behaves as cell content: text written or erased over it removes that part, as on the VT340's single bitmap. A kitty placement follows the kitty spec: of the erase commands only ED 2 touches it.
  - **Parsing:** sixel and kitty payloads are decoded as they stream in, never buffered whole.
  - **Files:** all four kitty media are supported. Opening files and shared memory, and decoding PNG and zlib, happen in a per-session helper process running as the tty's owner, under the kitty spec's file rules and Ghostty's checks. kmscon, which runs as root, decodes only sixel and base64 itself.
  - **Drawing:** bbulk composes the three placement bands per covered cell, sends cells wholly under an opaque top-band picture as one blit, and damages placement rectangles on both buffers. gltex follows with a texture per image, after ADR-102 step 6.
  - **Settings:** `image-storage-limit` (Ghostty), `max-graphic-size`, `num-color-registers` and `private-color-registers` (xterm).
- **Trades away:**
  - A program run under sudo cannot show root's files through the file media, because the helper reads as the logged-in user (probe 1).
  - A file or PNG image costs a round trip to the helper, and that VT's output pauses until the reply.
  - Animation, relative placements and Unicode placeholders wait for step 5. Clients that use them get EINVAL.
  - Byte-for-byte agreement with upstream on every tier 1 golden that records the DA1 reply, which gains attribute 4.
- **One-way?** No. `image-storage-limit=0` turns both protocols off. Reversing becomes expensive once programs rely on the replies, and once the helper protocol and the frame's placement list are interfaces both kmscon and libtsm build on.
- **Probes:**
  1. *Not confident (sudo identity):* the helper reads files as the user who owns the console's tty, the one who logged in. Under `sudo`, a program running as root that asks to show `/root/picture.png` is refused with the protocol's generic error, though it could read the file itself. The alternative reads as whichever program is in the foreground, root in that case. Which identity do you want?
  2. *Confident (decoding in the helper):* a PNG or compressed image from any program, remote ones included, is decoded by the helper running as you, so a fault in the PNG library cannot become a root fault. The cost is that every PNG image, even one sent inline, passes through that helper.
  3. *Not confident (memory default):* each screen may hold 320 MB of images, kitty's and Ghostty's default, so a VT whose programs fill both screens holds up to 640 MB, and six such VTs nearly 4 GB. Is that right for a console, or should the default be lower?
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

## Decision

### Image store (libtsm)

- There is one store per screen buffer, main and alternate, as kitty's quota is "per buffer" and Ghostty's limit is per screen.
- An image has:
  - a kitty id (1 to 2^32-1) and image number, or neither for a sixel;
  - an internal key of 64 bits, a slot and a generation, never reused while any frame might name it, as cluster keys are;
  - width, height and premultiplied ARGB32 pixels, the colour glyph format of ADR-102;
  - an opaque flag, set when no pixel is below full alpha;
  - its last use, the transient hint (`N=1`) and its origin, sixel or kitty.
- An image costs width x height x 4 bytes. The store's total never exceeds `image-storage-limit`.
- A new image that would pass the limit evicts, in order:
  1. images with no placement, transient ones first, least recently used first;
  2. images whose placements all sit on lines in scrollback, least recently used first, together with those placements.
- An image with a placement on the screen's own lines is never evicted. If the new image still does not fit, it is refused: kitty replies ENOSPC, and a sixel is dropped.
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
- **Identity.** The owner of the session's pty, as `/bin/login` sets it, read with `fstat` on the pty's slave:
  - that uid, its primary gid, and the user's supplementary groups through `initgroups`;
  - then `PR_SET_NO_NEW_PRIVS`, working directory `/`, every descriptor closed but its socket, and a memory limit derived from the image limits.
  - When the owner is root (no login, or root logged in), the helper runs as root, which is then the session's identity.
- **Owner changes.** Before each request kmscon reads the tty's owner again. When it differs from the helper's (a logout and a new login), the helper is stopped and a new one started.
- **Precedent.** sudoedit opens files as the invoking user rather than as root, and OpenSSH's privilege separation parses untrusted input in an unprivileged process. The helper applies both; it introduces no convention of its own.
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
- **Limits:** the decoded width and height stop at `max-graphic-size`, and pixels past it are discarded, as xterm truncates at `maxGraphicSize`. A repeat count is bounded by the same width.
- The image is created and placed when ST ends the DCS. CAN or SUB discards it.
- **Placement and cursor,** with sixel scrolling on (DECSDM reset, the default):
  - the picture's top left is the cursor's cell, and its box is its pixel size over the cell size, rounded up;
  - the screen scrolls, as index does, whenever the picture would pass the bottom margin;
  - the cursor ends on the text row holding the picture's last pixel row, at the picture's left column (xterm and foot), or with mode 8452 set, in the column after the picture's right edge on that row, clamped to the last column;
  - a picture wider than the columns left is clipped at the right edge.
- With DECSDM set, the picture is placed at the screen's top left, clipped to the screen, and the cursor does not move (VT340 manual; foot).
- Text already in the covered cells stays; it shows through transparent pixels, as it would on a VT340.
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
- **Opaque fast path.** Cells wholly under an opaque placement in the top band are not composed. The placement's picture is sent over them as row strips, so a full-screen video above the text is one large blit per frame.
- **Scaled pictures** are cached per image key and drawn size: box filter down (ADR-102's filter), bilinear up. The cache holds at most twice the screen's size in ARGB and drops the least recently used.
- **Damage.** bbulk keeps this display's previous placement list. A placement added, removed, moved, resized, clipped differently or given new image data damages its old and new rectangles. The cells there are drawn on this frame and the next, the two-frame rule cells follow. This depends on #67's fix and is tested on displays with and without damage clips.
- Rotated screens rotate pictures with the cells.

### gltex

- Images come to gltex after ADR-102 step 6 gives it an RGBA path and an untinted shader (#68), not before.
- Each image key gets a texture, uploaded when a placement of it is first drawn. An image larger than `GL_MAX_TEXTURE_SIZE` is uploaded scaled down to fit. Textures of images not drawn recently are freed once the total passes the bound bbulk's cache uses.
- One draw per layer: clear to the default background, lowest-band quads, non-default cell backgrounds, middle-band quads, glyphs and decorations, top-band quads.
- Image uploads take the same path that fails under virgl in #17, so the GL variant's image screens stay outside the default tier 3 run until #17 is resolved.

### Memory

- Images: at most twice `image-storage-limit` per terminal session, one store per screen buffer. kmsconvt@ runs one session per kmscon process, so that is per VT.
- bbulk's scaled pictures, or gltex's textures: at most twice the screen's ARGB size per display, 132 MB at 7680x2160.
- The helper: one decode at a time, bounded by `max-graphic-size` x 4 plus the compressed input.

### Performance

The risk ADR-101 named stands: at 7680x2160 a screen is 16.6 million pixels, 66 MB in ARGB, and bbulk composes on the CPU.

- **Measurement.** A benchmark draws through bbulk into the in-memory display tier 2 uses, at 1920x1080, 3840x2160 and 7680x2160, and reports milliseconds per frame (median and 95th percentile) and bytes sent:
  - S1: text scrolling, no image: the baseline against main.
  - S2: text scrolling in a region beside a static half-screen image.
  - S3: one line of scroll with a full-width image on screen, so every cell under it moves.
  - S4: a full-screen opaque image replaced every frame, a 480x360 source scaled up: the showcase's video.
  - S5: S4 under text, the image in the middle band: the showcase's banner over the video.
  - S6: fifty small images.
- The same scenarios then run in tier 3 on drm2d at 7680x2160, and the showcase runs on the operator's display.
- **Targets**, recorded as targets until measured: S1 within 5% of main; S3 at least 30 lines per second at 7680x2160; S4 at least 30 frames per second at 1920x1080 and 24 at 7680x2160 on the operator's hardware.
- **If a target is missed,** the remedies in order: splitting composition across threads by rows, SIMD scaling and blending, and gltex for video where GL is available. The measurements go in an evidence record, and the remedy chosen in a decision that cites it.
- **Frame counter.** kmscon counts completed page flips per display and logs the rate each second at debug level. The on-screen counter the showcase films is a debug overlay drawn from that count, added in its own change with its own option, since it touches every renderer and tier 2 must leave it out.

### Settings (kmscon.conf)

Each setting takes the name an existing terminal gives it, written in kmscon.conf's lower-case, hyphenated form.

| Setting | Named after | Default | Meaning |
|---|---|---|---|
| `image-storage-limit` | Ghostty `image-storage-limit` | 320000000 (kitty's 320 MB per buffer, Ghostty's default) | Bytes of decoded images per screen buffer. 0 turns sixel and kitty graphics off and removes DA1 attribute 4. |
| `max-graphic-size` | xterm `maxGraphicSize` | 10000x10000 | The largest image in pixels, for both protocols. xterm's 1000x1000 is smaller than this console's screens; 10000 is Ghostty's dimension limit. |
| `num-color-registers` | xterm `numColorRegisters` | 1024 (xterm's value for terminal ids that are not DEC models) | Sixel colour registers. |
| `private-color-registers` | xterm `privateColorRegisters` | true | The initial state of mode 1070. |

### Testing (ADR-301)

- **Tier 1** (screen state):
  - `tsm-replay` prints each placement (numbered in draw order, so keys do not enter goldens) with its anchor row and column, screen or scrollback, box, offset, source rectangle, z and kind; each image's size and a digest of its pixels; the cursor; and the replies, which it already prints.
  - Cases cover every row of the screen-events table for both kinds; the sixel decoder (aspect, raster attributes, HLS and RGB, repeats, shared and private registers, P2, truncation at `max-graphic-size`, DECSDM both ways, mode 8452); kitty transmission (whole and chunked, `f=24` and `f=32`, an upload aborted by a delete, `i` and `I`, replacement); placement (`c` and `r`, source rectangles, offsets, `C=1`, each band, cursor movement at the bottom margin and the right edge); every delete specifier in both cases; replies under `q=0`, `1` and `2`; `a=q`; eviction order and ENOSPC; the alternate screen and RIS.
  - Streams are recorded from img2sixel (libsixel, MIT) and from a kitty client the fork writes from the spec alone (ADR-300).
  - Goldens come from the fork, as upstream has no images, and each is reviewed in its own commit. The DA1 goldens change in their own commit.
- **Unit tests** (kmscon) for the helper: each refusal (`/proc`, `/sys`, `/dev`, a FIFO, a socket, a device, a directory, a symlink loop, a symlink swapped between check and open, a file shorter than `S`, `t=t` outside a temporary directory or without the name, shared memory names with extra slashes or too long) answers byte-identical EBADF; refused temporary files survive; accepted `t=t` files and `t=s` objects are removed.
- **Fuzzing.** The sixel decoder and the kitty command parser run in kmscon's root process on bytes any program sends. Each has a fuzz target, run for a fixed budget before a change to it lands.
- **Tier 2** (pixels): each band with text over and under it; the lowest band under default and non-default backgrounds; transparent and opaque sixels; a sixel split by text; a region scroll clipping a placement; scaled and letterboxed placements; a changed cell size. Incremental frames on a display with damage clips: a placement added, moved and removed leaves no stale pixels, the failure that ruled out the community patch (ADR-101).
- **Tier 3** (VM): the guest gains img2sixel (libsixel) and viu (MIT), both in Arch's extra repository, and the fork's spec-written client for `t=f`, `t=t` and `t=s`. Screens: a picture from img2sixel, scrolled into scrollback and back, then cleared; kitty pictures through each medium; text over a middle-band picture; a root-owned file refused to the logged-in user; and the sudo case as probe 1 settles it. bbulk variants first; GL after #17 and ADR-102 step 6.
- **Demo.** The VT demo gains an images section, and its screens become tier 3 screens as the others did. The showcase's acts 5 and 7 (#75) run on top.

### Order of work

Each item lands behind the harness, with goldens changed only in their own commits.

Step 3, the image layer and sixel:

1. **3a, libtsm store and placements.** The store, placements, every screen event in the table, the frame's placement list, the image call, and the cell size from the host. Unit tests build placements through a test-only call; `tsm-replay` prints them.
2. **3b, libtsm sixel.** DCS streaming, the decoder, registers, modes 80, 1070 and 8452, DA1 attribute 4, XTSMGRAPHICS, XTWINOPS 14, 16 and 18. A fuzz target. Tier 1 cases from img2sixel streams.
3. **3c, kmscon.** The cell size to libtsm and `TIOCSWINSZ`, the settings, and bbulk's bands, scaled-picture cache, opaque fast path and placement damage, after #67. Tier 2 screens.
4. **3d, evidence.** Tier 3 sixel screens, the demo section, and benchmark scenarios S1 to S4 recorded as an evidence record.
5. **3e, gltex images,** after ADR-102 step 6 and #17.

Step 4, the kitty graphics protocol:

1. **4a, libtsm kitty.** APC framing, control keys, inline `f=24` and `f=32`, chunking, the actions, ids, numbers and placement ids, bands, cursor movement, deletes, replies, and eviction with ENOSPC. A fuzz target. Tier 1 through the spec-written client. Without a host, the other media answer EBADF and PNG and zlib EINVAL.
2. **4b, the host and the helper.** The vte's host hook and the input call that reports what it consumed; `kmscon-image-helper` with its identity, file and shared memory rules, PNG and zlib, and memfd results. Unit tests for every rule, and the fixture host in `tsm-replay`.
3. **4c, kmscon.** Tier 2 screens for the lower two bands and text over pictures; benchmark scenarios S5 and S6.
4. **4d, evidence.** Tier 3 with viu and the spec-written client through each medium, the root-file refusal, the sudo case, the demo, and the showcase's act 7.
5. **4e, gltex** for the lower two bands, with 3e.

## Consequences

### Positive

- img2sixel, viu and other image tools work on the console, and their pictures follow the text through scrolling and scrollback, clear with `clear`, and leave no stale pixels.
- kmscon, running as root, never opens a path a program names and never runs a PNG or zlib decoder. What a client can read or delete is bounded by the logged-in user's own permissions.
- Both protocols share one store and one placement model. Step 5 adds Unicode placeholders and relative placements to them rather than beside them.
- The settings carry names xterm and Ghostty users already know.
- libtsm gains no dependency: everything it cannot decode itself goes to the host.

### Negative

- A program under sudo cannot show root-only files through the file media, unless probe 1 changes the identity.
- A VT's output pauses while a helper request is out. A slow file system stalls that VT for up to 2 seconds per command.
- The helper is a new executable to package, and a new process for each session that shows a file or PNG image.
- What an erase does to a picture depends on which protocol drew it. A reader who does not know the source cannot predict what `EL` does.
- ICH and DCH remove part of a sixel instead of shifting it.
- CPU composition at 7680x2160 is unmeasured, and video on bbulk may miss its target there.
- At the default limit, a hostile program can make one VT hold 640 MB of images.

### Neutral

- DA1 changes for every program. Programs that look for attribute 4 start sending sixel.
- Animation, relative placements and Unicode placeholders answer EINVAL until step 5, so clients can tell they are missing.
- The image goldens come from the fork alone; upstream libtsm cannot judge them.
- Pictures scale with the cell size, so a zoomed console shows them larger, and a second display with another font size draws them at its own scale.

## Alternatives Considered

- **Collect each DCS and APC payload whole, then decode,** as ADR-101 worded it. Rejected: an 8K picture's encoded payload is tens of megabytes held beside the decoded image. Streaming bounds memory by the decoded result.
- **Store image tiles in cells** (mlterm, WezTerm). Rejected by ADR-101. Splitting placements reproduces sixel's cell behaviour without it.
- **One erase rule for both protocols.** Kitty's rule for sixel lets a picture survive text written over it, which no DEC-derived terminal does, and leaves stale pictures under programs that redraw over their sixel output. Sixel's rule for kitty breaks the spec's statement that erase commands do not affect graphics, and removes pictures that kitty clients draw text over on purpose.
- **Open files in kmscon itself, with only the spec's checks,** as a terminal running as its user does. Rejected: as root, a remote program could have the console display any file on the machine as raw pixels, and a `t=t` command would delete as root.
- **Switch kmscon's file-system identity to the user for each open** (`setfsuid`), with no helper. Cheaper, but the process stays root, PNG and zlib still decode as root, and the identity switch is per thread in a process whose libraries may start threads.
- **Open in the helper but decode in kmscon,** the narrowest reading of the operator's direction. Rejected for PNG and zlib, library decoders whose faults would run as root. Sixel and base64 stay in kmscon: they are byte state machines written in the fork and fuzzed like the parser.
- **The foreground process's identity for the helper.** The other answer to probe 1. The tty's owner is the default here because login sets it, a program in the session cannot change it, and reading it cannot race a process exiting. The foreground process group can belong to a setuid program and can change between the command and the open.
- **Refuse the file media,** as some terminals do. Rejected by the operator: all four media are supported.
- **Call the helper synchronously.** Simpler, but it blocks kmscon's event loop, and with it drawing and VT switching, for up to the timeout.
- **libtsm reports removed placements to each caller.** Rejected: libtsm does not know its callers, and keeping removals until every caller has seen them needs that knowledge.
- **A vendored PNG decoder** (stb_image, spng). Rejected: libpng and zlib are already loaded through FreeType, and a vendored decoder is code the fork would maintain.
- **xterm's default `max-graphic-size` of 1000x1000.** Rejected: it is about an eighth of this console's width, and full-screen images are the case the showcase exists for.
