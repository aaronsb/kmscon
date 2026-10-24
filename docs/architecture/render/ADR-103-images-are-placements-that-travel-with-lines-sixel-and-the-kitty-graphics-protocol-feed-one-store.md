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
  - evidence: ADR-105
  - evidence: ADR-106
  - evidence: ADR-107
agent:
  name: claude
  model: claude-opus-5-5
status: accepted
date: 2026-10-22
deciders:
  - aaronsb
related: []
extends: ADR-101
amends: [ADR-101#parsing, ADR-101#image-layer]
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
  - operator: aaronsb
    said: "why not sketch the outline and include the flag to deactivate it so that sixel support is truthfully unavailable"
    via: "operator's reply in the steering session, 2026-10-23, relayed by the team lead, to kmscon #133's question 2 (gltex draws no placements, yet its terminal offered sixel)"
  - operator: aaronsb
    said: "Keep the K, M and G suffixes on image-storage-limit; accept the shrinking auto limit evicting at the next reservation, with a note"
    via: "operator's replies in the steering session, 2026-10-23, relayed by the team lead as answers to kmscon #133's questions 3 and 4"
    paraphrase: true
  - operator: aaronsb
    said: "yes"
    via: "operator's reply in the steering session, 2026-10-23, relayed by the team lead, to Q1: on a display that needs a scaled copy of a picture (rotation, zoom, or a cell size other than the font's), may bbulk's scaled copy for an on-screen placement evict images in scrollback or unplaced, so that viewing a new picture can push older pictures out of scrollback and leave blank cells there when scrolled back, the way old lines drop off"
  - operator: aaronsb
    said: "I have typically been using chafa"
    via: "operator's reply in the steering session, 2026-10-23, relayed by the team lead, to kmscon #145's question 1 (img2sixel writes nothing on Arch; which encoder draws A1's photographs)"
  - operator: aaronsb
    said: "Carry A2 at the two 7680x2160 variants as a known failure naming #139, in the capability-probe style: green while it fails, red once it passes with the entry still there"
    via: "steering session, 2026-10-23, relayed by the team lead as the answer to kmscon #145's question 2"
    paraphrase: true
  - operator: aaronsb
    said: "Change now: 85 plays a centred 1920x1080 window with dithering off; 86 and 87 say what tmux and mpv deliver; all three show mpv's shown and dropped counts on screen"
    via: "Claude Code session, 2026-10-24, answering how pages 85-87 change; the operator picked the option from choices the session offered"
    paraphrase: true
  - operator: aaronsb
    said: "Same, plus dither off in panes: A4 passes when every pane animates, tmux's pictures reach the screen, nothing is left after exit, and kmscon keeps up with what tmux sends; the frame rate is reported, not judged, and the page says tmux and mpv set it; the panes' mpv runs with --vo-sixel-dither=none"
    via: "Claude Code session, 2026-10-24, after the second VT review of #145; the operator picked the option from choices the session offered"
    paraphrase: true
  - operator: aaronsb
    said: "Pre-encoded feeder, new PR: tmux grids are compatibility only; a fork-written feeder judges kmscon's streaming"
    via: "Claude Code session, 2026-10-24, answering how kmscon's image streaming is judged apart from tmux; the operator picked the option from choices the session offered"
    paraphrase: true
  - operator: aaronsb
    said: "As ADR-103 says"
    via: "Claude Code session, 2026-10-24, relayed by the team lead; an option label the team lead wrote, chosen by the operator, on the image helper's hardening: ADR-103's uid, gid and initgroups, PR_SET_NO_NEW_PRIVS, closed descriptors and memory limit, with a seccomp or landlock filter left to aaronsb/kmscon#153"
  - operator: aaronsb
    said: "auto. this way we have first class support for systemd, elongind and friends, and bare"
    via: "Claude Code session, 2026-10-24, relayed verbatim by the team lead, answering how the build finds the session's user: a meson feature for sd-login, default auto, using libsystemd's or libelogind's sd-login when present and the pty's owner otherwise"
  - operator: aaronsb
    said: "Build step 4 with two agents in parallel, libtsm's 4a with the host hook and kmscon's helper in 4b, once this interface is fixed"
    via: Claude Code session, 2026-10-24, relayed by the team lead as a summary, on how step 4 is built
    paraphrase: true
  - operator: aaronsb
    said: "The showcase improvements #85 to #87 are done with 4d"
    via: "Claude Code session, 2026-10-24, relayed by the team lead as a summary, on when the showcase issues aaronsb/kmscon#85, #86 and #87 are taken up"
    paraphrase: true
  - operator: aaronsb
    said: "a poor player implementation isn't a spec we write against. each of these has various interpetations of using the kitty protocol and I suspect each of them were largely written at one point against the kitty spec then the spec changed and they remained relatively static"
    via: Claude Code session, 2026-10-24, on the client probe results, relayed verbatim by the team lead
  - operator: aaronsb
    said: "thats a claim not backed by evidence though"
    via: "Claude Code session, 2026-10-24, on the client probe results, relayed by the team lead: the operator's correction of their own suspicion, in the entry above, that the clients were written against an older spec; the principle stands without it"
  - operator: aaronsb
    said: "Question 1, the copies a load holds outside the budget: (a), they stay outside the budget, as bbulk's working memory does"
    via: "Claude Code session, 2026-10-24, answering PR #155's open questions; the operator picked the recommended option in a structured question, relayed by the team lead"
    paraphrase: true
  - operator: aaronsb
    said: "Question 2, the build option's name: (a), sd_login, following meson.options"
    via: "Claude Code session, 2026-10-24, answering PR #155's open questions; the operator picked the recommended option in a structured question, relayed by the team lead"
    paraphrase: true
  - operator: aaronsb
    said: "Question 3, images past the budget: (a), the spec's storage quota rule: evict unplaced images, then images placed only in scrollback, then kitty images placed on screen, least recently used first; on-screen sixel keeps the never-evict rule; covered-placement removal is not adopted and can be proposed later only on 4d evidence"
    via: "Claude Code session, 2026-10-24, answering PR #155's open questions; the operator picked the recommended option in a structured question, relayed by the team lead"
    paraphrase: true
  - operator: aaronsb
    said: "I don't like append only. I thought it made sense but we need to be able to edit where it makes sense too"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead, overruling the append-only convention for accepted records; it governs this PR's in-place edits to the Summary's store and budget bullet and the Image store section
  - operator: aaronsb
    said: "Rank 3's order: on-screen kitty images are evicted oldest transmission first; unplaced and scrollback images stay least recently used"
    via: Claude Code session, 2026-10-24, answering the rank-3 order question; the operator picked the option in a structured question, relayed by the team lead
    paraphrase: true
  - operator: aaronsb
    said: "Keep it as a gate"
    via: Claude Code session, 2026-10-24, answering whether mpv inline stays an A5 gate; the operator picked the option in a structured question, relayed by the team lead
    paraphrase: true
  - operator: aaronsb
    said: "Fix the transparent-sixel pile-up (libtsm#49) with per-cell coverage"
    via: Claude Code session, 2026-10-24, answering whether libtsm#49 is fixed with per-cell coverage; the operator picked the recommended option in a structured question
    paraphrase: true
  - operator: aaronsb
    said: "Add the four performance limits of the one-pipeline design to ADR-103, after the merge"
    via: Claude Code session, 2026-10-24, after asking whether one pipeline with several protocol handlers limits performance; the operator picked the recommended option in a structured question
    paraphrase: true
  - operator: aaronsb
    said: "This pass the video performance is noticibly better and more consistent with frame delivery. I'm still marking it as incorrect, because 30 fps would be passing. we can decide if that's an unattainable criteria"
    via: "the operator's VT review of #145 at f1071a0, 2026-10-24, rating tmux-2x2 and tmux-3x3; relayed verbatim by the team lead"
  - operator: aaronsb
    said: "I'm concerned we're evaluating tmux more than we're evaluating kmscon's ability to stream image data"
    via: "Claude Code session, 2026-10-24, the A4 discussion after the second VT review of #145; relayed verbatim by the team lead"
  - operator: aaronsb
    said: "1, probably but which would lead on performance and compatibilty?"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead, on whether to match kitty fully on S and data length; the spec is silent and ADR-300 settles open points by black-box kitty
  - operator: aaronsb
    said: "Yes, both (Recommended): match kitty 0.49.2 on S and data length, read from O to min(O+S, end) with S as a maximum, need at least s x v x bytes per pixel and ignore the excess"
    via: Claude Code session, 2026-10-24, relayed by the team lead; the option the operator picked after being told kitty leads on both performance and compatibility
    paraphrase: true
  - operator: aaronsb
    said: "for our implementation, are keeping seperation from kmscon core: meaning, when/if kitty spec changes, is it something that won't affect kmscon's consumption of the data"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead; answered by making libtsm the only reader of the kitty spec and the image helper mechanical
  - operator: aaronsb
    said: "Refuse before login: when logind is running and the terminal's child is in no logind session, t=f, t=t and t=s answer EBADF without starting the helper; inline media still work; without logind the pty-owner fallback stays"
    via: Claude Code session, 2026-10-24, relayed by the team lead from the review of aaronsb/kmscon#162; the option the operator picked, since before login the helper's identity would fall back to the pty's owner, root
    paraphrase: true
  - operator: aaronsb
    said: "if there is a verbose debug path somehow, we could emit that it was discarded"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead, on the excess data kitty's length rule ignores
  - operator: aaronsb
    said: "refusing this would make presenting a graphic at login harder. how commmon is the nobody user"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead, asked whether to refuse inline PNG and zlib before login
  - operator: aaronsb
    said: "Dedicated user, nobody fallback: kmscon ships a sysusers.d entry for a system user kmscon-image; before login inline PNG and zlib decode as kmscon-image, as nobody when it is absent, and are refused when neither exists; file and shared memory media stay refused before login"
    via: Claude Code session, 2026-10-24, relayed by the team lead; the option the operator picked after being told nobody (65534) is near-universal but shared with NFS squash and some daemons
    paraphrase: true
  - operator: aaronsb
    said: "Never read as root by fallback: whenever the identity comes from the pty-owner fallback and is uid 0, file, temp-file and shared memory media are refused, and inline PNG and zlib decode as kmscon-image, then nobody, else are refused; a logind-authenticated uid 0 keeps the helper as root"
    via: Claude Code session, 2026-10-24, relayed by the team lead; the option the operator picked after the pre-login rule was found to leave systems without logind reading file media as root at the login prompt
    paraphrase: true
  - operator: aaronsb
    said: "keep as it is"
    via: Claude Code session, 2026-10-24, asked whether kmscon should reply to an a=q carrying I and no i instead of staying silent as kitty, Ghostty, WezTerm and Rio do
observable:
  - "img2sixel picture.png on a kmscon VT shows the picture under the command; Shift+PageUp scrolls it away and back with the text; clear removes it and leaves no pixels behind"
  - "viu picture.png shows the picture through the kitty graphics protocol, inline and from a temporary file"
  - "A kitty client run by the logged-in user that names a root-only file with t=f gets EBADF:Failed to read image file, the same reply as for a missing file"
  - "mpv --vo=sixel and mpv --vo=kitty (inline) play an animation full screen on drm2d with bbulk at 1366x768, 1920x1080 and 7680x2160, and the render timer's presented rate matches mpv's output rate; the spec-written client, inline and over shared memory, and kitten icat --transfer-mode=memory do the same; mpv --vo=kitty --vo-kitty-use-shm=yes is measured for compatibility only (changed 2026-10-24: under the operator's principle that the spec is the target, \"a poor player implementation isn't a spec we write against\", mpv's shared memory mode, whose names the spec forbids, became a compatibility measurement, and the operator chose to keep mpv --vo=kitty inline as a gate, an option they picked; this said: \"mpv --vo=sixel and mpv --vo=kitty (inline and with --vo-kitty-use-shm=yes) play an animation full screen on drm2d with bbulk at 1366x768, 1920x1080 and 7680x2160, and the render timer's presented rate matches mpv's output rate\")"
  - "The 2d-1366x768-hamster tier 3 variant (1 vCPU, 512 MB) runs the image screens and S1 to S6 on every tier 3 run"
  - "tmux in a 3x3 grid of panes, each running mpv --vo=sixel, plays all nine and leaves no stale pixels when the panes close"
  - "The tier 2 screens for each composition band, and the render timer's figures for acceptance scenarios A1 to A7 recorded as an evidence record"
---

# ADR-103: Images are placements that travel with lines; sixel and the kitty graphics protocol feed one store

## Summary

- **Decided:** Steps 3 and 4 of ADR-101, made concrete.
  - **Store and budget:** one image store per screen buffer. Every store and the renderer's scaled pictures draw on one budget per kmscon process, `image-storage-limit`. By default it is four frames of the process's displays in ARGB, at least 16 MiB and at most 192 MiB: 16 MiB at 1024x768 and 1366x768, 31.6 MiB at 1920x1080, 192 MiB at 7680x2160. Over budget, scaled pictures not on screen go first, then images with no placement, then images placed only in scrollback, then kitty images placed on the screen, oldest first. A sixel placed on the screen is never evicted; a new image that still does not fit is refused. (Changed 2026-10-24: the operator chose the spec's storage-quota rule. This bullet said: "Over budget, scaled pictures not on screen go first, then images with no placement, then images placed only in scrollback. An image placed on the screen is never evicted; a new image that still does not fit is refused." The Image store section says which parts of the order are this record's choices.)
  - **Placements:** each is anchored to the line holding its top row, so it travels into scrollback and back with the text. A sixel behaves as cell content: text written or erased over it removes that part, as foot does and as terminals that keep image tiles in cells behave. A kitty placement follows the kitty spec: of the erase commands only ED 2 touches it.
  - **Parsing:** sixel and kitty payloads are decoded as they stream in, never buffered whole.
  - **Files:** all four kitty media are supported. Opening files and shared memory, and decoding PNG and zlib, happen in a per-session helper process running as the session's user, never as the foreground process, as a desktop terminal reads as its desktop user. It follows the kitty spec's file rules and Ghostty's checks. kmscon, which runs as root, decodes only sixel and base64 itself.
  - **Drawing:** bbulk composes the three placement bands per covered cell, sends cells nothing draws over an opaque picture as row strips, and damages placement rectangles on both buffers. gltex follows with a texture per image, after ADR-102 step 6.
  - **Acceptance:** full-motion video through mpv (`--vo=sixel`, and `--vo=kitty` inline), through the spec-written client inline and over shared memory, and through kitten icat over shared memory, on drm2d with bbulk; mpv over shared memory is measured for compatibility only (changed 2026-10-24: under the operator's principle that the spec is the target, "a poor player implementation isn't a spec we write against", mpv's shared memory mode, whose names the spec forbids, became a compatibility measurement, and the operator chose to keep `mpv --vo=kitty` inline as an A5 gate, an option they picked; this began: "full-motion video through mpv (`--vo=sixel`, and `--vo=kitty` inline and over shared memory) on drm2d with bbulk,"), the showcase's text banner over a full-screen video, and tmux grids of sixel videos. 1366x768 and 1920x1080 are the common case and carry targets, judged on a small tier 3 machine (1 vCPU, 512 MB); 7680x2160 is the stress case, with no target until the first measurement, an early step. A `render-timer` overlay (foot's option) shows frames presented, CPU time and damaged area per frame, drawn as a top-band placement.
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
  3. images whose placements all sit on lines in scrollback, least recently used first, together with those placements;
  4. kitty images with a placement on a screen's own lines, oldest first by when each was transmitted, together with all their placements.
- A sixel image with a placement on a screen's own lines is never evicted. If the new image still does not fit, it is refused: kitty replies ENOSPC, and a sixel is dropped.
- The spec asks only that "the terminal emulator should delete older images to make space for the new one", and that "existing images without placements will be preferentially deleted". Three parts of the order are this record's choices: "older" is read as least recently used for unplaced images and those in scrollback, as this record had it, and as oldest transmission for images on a screen's own lines, since every placement a frame draws has its last use renewed in draw order each frame, so least recently used among them would follow draw order rather than age; images placed only in scrollback go before images on a screen's own lines, since they are out of view; and sixel pictures are exempt, since the quota text is the kitty protocol's and a sixel is cell content.
- This order, step 4 included, applies when libtsm reserves room for a new image in its stores. bbulk's scaled copies keep the rule of the Notes of 2026-10-23 (Q1): they never evict an image placed on a screen's own lines, whether or not their frame draws it (the view scrolled back, or the main screen behind the alternate one). In the interface (Notes of 2026-10-24), libtsm reports step 4 as victim rank `TSM_IMAGE_PLACED_KITTY` (3), with the image's transmission time in the victim's `last_use`, so kmscon's order across screens (rank, then `last_use`) gives oldest transmission first without a change; kmscon's scaled-copy room counting stops at rank 3.
- (Changed 2026-10-24: the operator chose the spec's storage-quota rule. Before, this list ended at step 3, and the sentence above read: "An image with a placement on a screen's own lines is never evicted.")
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
| A new sixel placed over cells | The parts of older sixels under every cell the new sixel covers completely with opaque pixels are removed, as a character written there would remove them, whatever its P2; an older piece left holding no set pixel is removed too, since it draws nothing. Under cells the new sixel covers only partly, older sixels stay and show through. (Changed 2026-10-24: the operator chose per-cell coverage to fix libtsm#49, where a picture padded with transparent pixels, as chafa sends, stacked on the last until the budget froze the animation. This row said: "When the new sixel is opaque (P2 other than 1), the parts of older sixels under it are removed, as a character written there would remove them; a transparent one leaves them".) | No effect |
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
  - The payload is base64-decoded as it arrives into the pending image. Raw data needs at least the declared `s` x `v` x bytes per pixel; bytes past that are ignored, and fewer at the end of the upload fail it with EINVAL. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "Raw data may not pass the declared `s` x `v` x bytes per pixel; past it, the upload fails with EINVAL and the rest is ignored until ST.") PNG or compressed data is buffered for the host, each growth reserved from the image budget before it is allocated; a refused reservation fails the upload with ENOSPC, and the rest is ignored until ST. (Changed 2026-10-24 with the host interface of that date's Notes, which give inline PNG and zlib data to the host: this said "Raw data may not pass the declared `s` x `v` x bytes per pixel, and PNG or compressed data may not pass `image-storage-limit`. Past either bound, the upload fails with EINVAL and the rest is ignored until ST.")
  - Chunks follow the spec: `m=1` until the last chunk, and later chunks carry only `m` and `q`. `m` applies to direct transmission only: a `t=f`, `t=t` or `t=s` command is complete at its own ST, whatever its `m`, and a following APC is a new command (Notes of 2026-10-24). Any delete command aborts a pending upload (spec), and so does any other graphics command that is not its continuation. (Changed 2026-10-24 with the host interface of that date's Notes, which read the spec's chunking as defined only for pixel data sent directly: this said "Chunks follow the spec: `m=1` until the last chunk, and later chunks carry only `m` and `q`." for every medium.)
  - Every other APC stays ignored, as now.
- libtsm decodes base64 and raw `f=24` and `f=32` data itself. Files, shared memory, PNG and zlib go to the host (next section).
- **DA1** adds attribute 4 while images are enabled.
- **XTSMGRAPHICS** answers item 1 (colour registers) for read, reset, set within 2 to `num-color-registers`, and read maximum; item 2 (sixel geometry) for read and read maximum, both `max-graphic-size`, and refuses a set with status 3, as xterm refuses changes to graphics sizes. Item 3 (ReGIS) answers status 1.
- **XTWINOPS** answers 14 (text area in pixels), 16 (cell size in pixels) and 18 (text area in cells). kmscon also sets `ws_xpixel` and `ws_ypixel` with every `TIOCSWINSZ`.

### Host interface (libtsm and kmscon)

- The vte gains a host hook for what libtsm does not do: reading `t=f`, `t=t` and `t=s` media, and decoding PNG (`f=100`) and zlib (`o=z`). A request carries what to open (the path or shared memory name, or the received bytes for inline PNG and zlib), where to start, the fewest bytes the image needs and the most that may be read, and whether to inflate or decode PNG. libtsm turns the kitty keys into those terms, so the host and its helper read no part of the kitty spec. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "A request carries the medium, the path or shared memory name, `O`, `S`, the format, the compression and the declared size, or the received bytes for inline PNG and zlib.")
- **Input pauses while a request is out.** A new input call returns how many bytes it consumed. kmscon keeps the rest and stops reading that pty until the reply arrives, or until 2 seconds pass, when the request fails with EBADF and the helper is restarted. Commands therefore keep their order (a client may send a put right after a file transmission without waiting for the reply), and kmscon's event loop keeps running, so drawing and VT switching are never blocked.
- The existing input call stays for hosts without the hook. Without it, `t=f`, `t=t` and `t=s` answer EBADF, and PNG and zlib answer EINVAL. `tsm-replay` installs a fixture host that serves files from `cases/<case>.files/` beside the test's stream (Notes of 2026-10-24, tsm-replay's fixture host).

### The image helper (kmscon)

- **Process.** A separate executable, `kmscon-image-helper`, installed beside kmscon's modules. kmscon starts one per terminal session at its first request: it forks, takes on the identity below, and execs the helper, so the helper starts with nothing of kmscon's memory.
- **Identity.** The session's user, never the foreground process's:
  - When kmscon is built with sd-login (systemd's or elogind's) and the process it started is in a logind session, the session's uid: `sd_pid_get_session` on that process, then `sd_session_get_uid`. pam_systemd registers the session when login authenticates the user.
  - Before login, file media are refused. When logind is running (kmscon built with sd-login) and the terminal's child is in no logind session, as at the login prompt before anyone has logged in, or for a command kmscon started without login, `t=f`, `t=t` and `t=s` answer EBADF without starting the helper. The pty's owner is then root, and reading files as root for a program at the login prompt is what this section exists to prevent. Inline media still work: raw data stays in libtsm, and inline PNG and zlib go to a helper running as `kmscon-image`, a system user kmscon creates through a sysusers.d entry, with home `/`, no login shell and no files of its own; where that user is absent, as `nobody`; and where neither exists, they answer EBADF too. A user of kmscon's own keeps the decoder apart from what else runs as `nobody`, which NFS root squashing and some daemons share. On a system without `systemd-sysusers` the entry creates nothing, so there `nobody` is the norm unless a package creates the user. The candidates are taken in order, and a candidate is passed over, for the next, when its lookup fails or when it resolves to uid 0 or gid 0, as a name service or a mistake can make it; when none is left the request is refused. The pre-login user gets its primary gid only, with no supplementary groups. kmscon resolves the candidates once, when it first needs them, and keeps the answer, so a slow network name service cannot stall the console at the login prompt on every request. A pre-login refusal sends no discard, so a `t=t` file or `t=s` object a client named at the login prompt stays where it is, unlike after a refused command once logged in; this is accepted, since no helper may delete a file before anyone has logged in, and `kmscon-image` could not remove another user's file in the sticky `/tmp` in any case. (New 2026-10-24. The operator chose "Refuse before login" in the review of aaronsb/kmscon#162; kept inline decoding before login ("refusing this would make presenting a graphic at login harder. how commmon is the nobody user"); and chose "Dedicated user, nobody fallback". The candidates' rules came from the code review of aaronsb/kmscon#164.)
  - **Whether logind is running** is `sd_get_seats(NULL)`: more than 0 is running, 0 is not, and a negative return, an error, counts as running, so it fails closed. sd-login counts the entries of `/run/systemd/seats/` and returns 0 when that directory does not exist (systemd 3a7f91a, `src/libsystemd/sd-login/sd-login.c:988-998`), the directory systemd's own `logind_running()` tests (`src/basic/login-util.c:16-18`); elogind's sd-login has the same code (elogind 361db57, `src/libelogind/sd-login/sd-login.c:1055-1065`), and the sd_get_seats(3) page documents the count and a negative errno on failure. `sd_booted` is not the test: it says whether systemd is the init system, which an elogind system is not. With logind running, or its state unknown, any failure to find the session's uid, `sd_pid_get_session` finding no session or failing otherwise, or `sd_session_get_uid` failing, is handled as before login: file media refused, inline PNG and zlib decoded as `kmscon-image`, then `nobody`, then refused. Only `sd_get_seats` returning 0, or a build without sd-login, leads to the pty's owner. aaronsb/kmscon#162 implements the same test (`*files = sd_get_seats(NULL) == 0`). (New 2026-10-24, from the code review of aaronsb/kmscon#164 and its skeptic's review: the record named no test for "logind is running" and no rule for sd-login's other errors.)
  - Otherwise, when kmscon is built without sd-login or no logind is running, the owner of the session's pty, as `/bin/login` sets it, read with `fstat` on the pty's slave, unless that owner is root (Never root by fallback, below). (Changed 2026-10-24: the operator chose "Never read as root by fallback", since without logind the pty's owner is root at the login prompt. It said the pty's owner with no exception for root.) (Changed 2026-10-24 with the bullet above; this said: "Otherwise (built without sd-login, no logind running, or a command kmscon started without login), the owner of the session's pty, as `/bin/login` sets it, read with `fstat` on the pty's slave.")
  - logind's answer comes first because it is the identity PAM authenticated, held by logind for the session's life. The pty's owner is file metadata: any root process in the session, a `sudo chown` among them, can hand the pty to another user, root included, and every program in the session would then have files read as that user. When the two disagree, kmscon logs it and uses logind's.
  - kmscon uses systemd today only to find its unit directory (meson.build). sd-login becomes an optional dependency, and a build without it uses the pty's owner.
  - The helper takes that uid, its primary gid and the user's supplementary groups, then `PR_SET_NO_NEW_PRIVS`, working directory `/`, every descriptor closed but its socket, and a memory limit derived from the image limits. kmscon resolves the gid and the group list in its own process before forking (`getpwuid_r`, `getgrouplist`), since a library kmscon loads may start threads, and a child forked from a threaded process may call only async-signal-safe functions. The child calls only `setgroups`, `setresgid`, `setresuid`, `prctl` (`PR_SET_NO_NEW_PRIVS`, and `PR_CAP_AMBIENT` with `PR_CAP_AMBIENT_CLEAR_ALL`), `setrlimit`, `chdir`, `dup2` and `close_range`, resets its signal mask, and `execve`s the helper with an environment holding only `TMPDIR`, kmscon's, for the temporary-file rule. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the ambient-capability clearing is added, and "a clean environment" is named; this said "`execve`s the helper with a clean environment".) (Changed 2026-10-24, from the code review: this said the helper takes the groups with `initgroups`, which is not safe after a fork in a threaded process.)
  - **Never root by fallback.** When the identity comes from the pty's owner and that owner is uid 0, as at a login prompt on a system without logind, or after root logs in there, `t=f`, `t=t` and `t=s` answer EBADF without starting the helper, and inline PNG and zlib decode as `kmscon-image`, then `nobody`, and are otherwise refused, as before login. Only a uid 0 that logind authenticated, root logged in through PAM with logind running, runs the helper as root. The cost: a root console login on a system without logind gets no file images, while inline images still work. (Changed 2026-10-24: the operator chose "Never read as root by fallback", since without logind the pty's owner is root at the login prompt. This said: "When the session's user is root (root logged in, or no login and the pty still root's), the helper runs as root, which is then the session's identity.")
- **User changes.** Before each request kmscon finds the session's user again. When it differs from the helper's (a logout and a new login), the helper is stopped and a new one started.
- **Precedent.** Desktop terminals (kitty, Ghostty, WezTerm, foot, Konsole) run as the desktop user, so `sudo mpv --vo=kitty` inside one still has the terminal open files as that user, and root-only files fail to load. sudoedit opens files as the invoking user, and OpenSSH's privilege separation parses untrusted input in an unprivileged process. kmscon matches the desktop terminals and introduces no convention of its own.
- **Font paths.** OSC 50 font paths (#77) are opened by the same helper, with the same identity and path rules. What it returns for a font is settled with #77.
- **Files** (`t=f`, `t=t`):
  - Before opening, the path is resolved (`realpath`, following symlinks, failing on loops, as the spec asks) and refused when it lies in `/proc`, `/sys` or `/dev` other than `/dev/shm`. Checking before opening honours the spec's warning that opening can have side effects.
  - Nothing but a regular file is opened. The resolved path is first opened `O_PATH` through `openat2` with `RESOLVE_NO_SYMLINKS`, which opens nothing; `fstat` on that descriptor must show a regular file, and `fstatfs` must show no kernel pseudo-filesystem (procfs, sysfs, debugfs, tracefs, securityfs, cgroup, devpts, configfs, bpf, pstore, efivarfs, the security modules', binfmt_misc, nsfs, anonymous inodes, usbfs), wherever it is mounted. Only then is the file itself opened read-only (`O_RDONLY`, `O_NOCTTY`, `O_NONBLOCK`) through `/proc/self/fd`, and its device and inode must match the `O_PATH` descriptor's. A FIFO or a device is therefore never opened, so a writer waiting on a FIFO is not let go and a device sees no open. The path of the opened descriptor must then pass the same location check, Ghostty's check, which catches a symlink swapped in after the first check. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164, matching #162: this said "`stat` must show a regular file", and "After opening (`O_RDONLY`, `O_NOCTTY`, `O_NONBLOCK`), `fstat` must show a regular file again, and the path of the opened descriptor must pass the same check.")
  - `O` is the offset and `S` the most bytes to read: the read covers `O` to the lesser of `O` + `S` and the end of the file, or to the end when `S` is 0. An offset past the end fails, and so does a read shorter than the image needs; bytes past what it needs are ignored. An offset exactly at the end fails too, `UNREADABLE`: that is this record's choice, since no probe of kitty covers it (ADR-107), and it answers as an offset past the end does. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: this said "An offset at or past the end fails", as if both were evidenced.) The rest are kitty 0.49.2's rules, which this record matches knowingly (Notes "S and data length; a mechanical helper"); libtsm sets the bounds and the helper applies them. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "`O` is the offset and `S` the exact number of bytes to read; a shorter file fails.")
  - `t=t` is read only when the resolved path lies in `/tmp`, `/dev/shm` or kmscon's `TMPDIR` and contains `tty-graphics-protocol`, and the file is deleted once opened, whether the read succeeds or not, as kitty and Ghostty delete it. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "and the file is deleted after reading".) Any other temporary file is refused, as Ghostty refuses it. The spec only forbids deleting such a file; refusing it keeps a command from reading a file it asked to have deleted.
  - **Deleting** removes the resolved path, never the path as given, and only the file that was checked. The helper opens the resolved path's parent with `openat2` and `RESOLVE_NO_SYMLINKS` (`O_PATH | O_DIRECTORY`), checks with `fstatat(dirfd, base, AT_SYMLINK_NOFOLLOW)` that the entry has the device and inode of the descriptor it read, and only then calls `unlinkat(dirfd, base, 0)`. Deleting by path would let another local user swap a directory component for a symlink after the check and have the helper, running as the session's user, delete one of that user's files elsewhere. A failure leaves the file and is logged; the client's reply is unchanged. (Added 2026-10-24, from the code review of this record's Notes.)
- **Shared memory** (`t=s`): the name must start with `/`, contain no other `/`, and be at most 255 bytes. It is opened as the user, as `shm_open` would under `/dev/shm` but through the Files bullets' regular-file open, with `openat2` under `/dev/shm` and `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`, so a FIFO or a symlink there is never opened. It is read under the same bounds as a file, and unlinked once opened, whether the read succeeds or not, by `unlinkat` under `/dev/shm` after `fstatat` shows the entry still has the device and inode opened. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164, matching #162: it was opened "read-only as the user", by `shm_open`, and unlinked by name.) (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "must be at least `O` + `S` bytes".)
- **One failure reply.** Every failure to read (missing, unreadable, not a regular file, a refused location, an offset at or past the end, a read too short for the image) answers the same bytes, `EBADF:Failed to read image file`, the reply the spec gives as kitty's. Inflated data too short for the image is not a read failure and answers EINVAL (What libtsm answers). The reason goes to kmscon's log. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: this listed "too short" without saying it is the read, and named no offset.)
- **Decoding.** PNG through libpng, and zlib through zlib, in the helper, which declares both as its own build dependencies. zlib is already a direct dependency of kmscon (meson.build); libpng reaches kmscon today only through FreeType's private requirements, and FreeType is an optional backend, so the helper adds libpng to kmscon's build. (Corrected 2026-10-24: this said "FreeType already links both, so kmscon gains no dependency".) A PNG's header is read and its size checked against `max-graphic-size` before any pixels are allocated.
- **Result.** A decoded PNG's pixels, or exactly the bytes the request asked for, come back in a sealed memfd over the helper's socket. kmscon maps it read-only, checks its size (width x height x 4 within the limits for a PNG, the bytes asked for otherwise), and hands it to libtsm, which converts and stores it. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "The pixels come back in a sealed memfd over the helper's socket. kmscon maps it read-only, checks that its size is exactly width x height x 4 and the size within the limits, and stores it.") One request per session is out at a time.

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
- **Formats:** `f=24`, `f=32` (the default) and `f=100` (PNG, through the host); `o=z` through the host. Raw formats need `s` and `v`, and their data, after decompression, at least `s` x `v` x bytes per pixel; the excess is ignored. Too few bytes from a file or shared memory answer EBADF, and too few sent inline or after decompression answer EINVAL. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "Raw formats need `s` and `v`, and their data, after decompression, must be exactly `s` x `v` x bytes per pixel.")
- **Media:** `t=d` in libtsm, chunked or whole; `t=f`, `t=t` and `t=s` through the host.
- **Ids:** `i` names an image. `I` names the newest image with that number, and the terminal picks an unused id for it, reported as `i=<id>,I=<n>`. Both in one command is EINVAL (spec).
- **Placement keys:** `x`, `y`, `w`, `h` choose the source rectangle, cut to the image; `X`, `Y` offset within the first cell and must be smaller than the cell (EINVAL otherwise); `c`, `r`, `z`, `p`; `C=1` leaves the cursor where it is.
- **Position:** the cursor's position when the final chunk arrives (spec).
- **Cursor after a placement** (`C=0`), Ghostty's reading of the spec: index (scrolling as needed) one row fewer than the box's rows, bounded by the distance to the bottom margin plus one screen; then move to the column after the box, or, when that reaches the right edge, index once more and move to column 0.
- **Deletes:** `d` of `a`, `i` (with `p`), `n`, `c`, `p`, `q`, `r`, `x`, `y` and `z`, in both cases, as the spec's table says, with no `d` meaning `a`. "Visible on screen" means on the screen's own lines, not scrollback. An uppercase delete frees an image left with no placement anywhere, scrollback included. `d=f` and `d=F` delete nothing until animation exists. Kitty deletes never touch sixel placements.
- **Replies** go back through the vte's write callback, as DA replies do: `ESC _ G i=<id>[,I=<n>][,p=<pid>];OK ESC \`, or `;CODE:message`. A reply is sent only when the command carries `i` or `I` (spec), except that `a=q` with `I` and no `i` gets no reply under any `q`, and the command is dropped before any medium is read (What reaches the hook, in the Host interface Notes), as kitty 0.49.2 run as a black box does (ADR-107, the `q` table and round 5; ADR-300 settles the point, and the operator kept it: "keep as it is"). (Changed 2026-10-24, from the round-4 probe for aaronsb/kmscon#164: this said only "A reply is sent only when the command carries `i` or `I` (spec)", which gave `a=q` with `I` alone a reply.) `q=1` suppresses OK and `q=2` suppresses every reply, OK and error alike, as kitty 0.49.2 does run as a black box (ADR-107, the `q` section). The spec leaves the point open between two sentences, and ADR-300 settles such points by kitty run as a black box, so this needs no operator answer. The spec's prose section "Suppressing responses from the terminal" says "Set it to 1 to suppress OK responses and to 2 to suppress failure responses" (the control-data table's row says only "Suppress responses from the terminal to this graphics command."), and its Unicode placeholder section creates images "in quiet mode (q=2) so that there are no responses from the terminal that could confuse the host application"; the second sentence holds only if `q=2` silences OK as well, so this record reads `q=2` as silencing both. (Changed 2026-10-24, from the review of libtsm's kitty parser, whose code already silences every reply under `q=2`: this said "`q=1` suppresses OK and `q=2` suppresses errors.") (Corrected 2026-10-24, from the skeptic's review of aaronsb/kmscon#164: this said "The spec's table says" of the prose section's sentence.) `a=q` loads and checks the data without storing it and replies as a transmission would. The codes are OK, ENOENT (no such image), EINVAL (bad or conflicting keys, an unsupported action or format), EBADF (any read failure, one identical message) and ENOSPC (the storage limit).

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
  - Criterion, set 2026-10-24 (kmscon #145): A4 passes when every pane animates, tmux's pictures reach the screen, nothing is left after tmux exits, and kmscon keeps up with what tmux sends. The frame rate is reported, not judged: tmux and mpv set it. Each pane's mpv runs with `--vo-sixel-dither=none`. Until then the record named no criterion of A4's own; its observable read "tmux in a 3x3 grid of panes, each running mpv --vo=sixel, plays all nine and leaves no stale pixels when the panes close", and the demo pages asked for each pane's "counter rising by 30 a second". The operator's second VT review rated the grids wrong only for running under 30 frames a second ("30 fps would be passing. we can decide if that's an unattainable criteria"). With nothing reading the terminal, tmux and mpv bring 41-44% of the panes' frames through at 7680x2160, mpv alone reaches about 16 frames a second at the 2x2 pane's size, and at 3x3 tmux itself is the ceiling (ADR-104's A4 note). Shown that, the operator chose "Same, plus dither off in panes" (the considered entry of this date).
  - Changed the same day: A4 is compatibility through tmux. Concerned that the grids were "evaluating tmux more than we're evaluating kmscon's ability to stream image data", the operator chose to judge kmscon's streaming of many pictures at once apart from any encoder or multiplexer, with a pre-encoded sixel feeder the fork writes (#161, the considered entry "Pre-encoded feeder, new PR"). A4 now passes when tmux's pictures reach the screen, every pane animates, and nothing is left after tmux exits. The frame rate and kmscon's CPU time per picture tmux sent are reported, not judged; "kmscon keeps up with what tmux sends" leaves A4 for #161. Tier 3 fails a grid when tmux sent kmscon no picture or the screen after it differs from its golden; that every pane animates is judged on the VT.

Step 4, kitty:

- **A5.** Full-motion video through the kitty protocol. The conformance runs are `mpv --vo=kitty` inline (its default, chunked through the pty), the spec-written client inline and over shared memory (`t=s`, through the helper), and `kitten icat --transfer-mode=memory`. `mpv --vo=kitty --vo-kitty-use-shm=yes` is a compatibility measurement that gates nothing (Notes of 2026-10-24, Acceptance: conformance and compatibility). (Changed 2026-10-24: under the operator's principle that the spec is the target, "a poor player implementation isn't a spec we write against", mpv's shared memory mode, whose names the spec forbids, became a compatibility measurement, and the operator chose to keep `mpv --vo=kitty` inline as an A5 gate, an option they picked. This said: "`mpv --vo=kitty`, inline (its default, chunked through the pty) and with `--vo-kitty-use-shm=yes` (shared memory, `t=s`, through the helper).")
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
  - A2 and A5 at 1366x768 and 1920x1080: the render timer's presented rate matches the sending program's output rate, mpv's in A2 and each conformance client's in A5 (mpv inline, the spec-written client, kitten icat), on the small machine and on the operator's. (Changed 2026-10-24 with A5's clients: under the operator's principle that the spec is the target, "a poor player implementation isn't a spec we write against", mpv's shared memory mode, whose names the spec forbids, became a compatibility measurement, and the operator chose to keep `mpv --vo=kitty` inline as an A5 gate, an option they picked. This said "the render timer's presented rate matches mpv's output rate, on the small machine and on the operator's.")
- **Basis.** A frame is 4.0 MiB at 1366x768 and 7.9 MiB at 1920x1080; at 30 frames per second bbulk writes about 120 and 240 MiB per second, which one core should sustain; that is an estimate, not a measurement. The figures are first estimates. 3b's measurement confirms or replaces them in its evidence record, and the scenarios are judged against what that record states.
- **The stress size,** 7680x2160: no number can be justified before measuring. Each frame writes 66 MB to the scanout buffer, and that hardware's write bandwidth has not been measured. The criterion that holds now: kmscon is not the bottleneck. In A2 the presented rate matches mpv's output rate. In A5 it matches each conformance client's rate: mpv inline, the spec-written client inline and over shared memory, and kitten icat in memory mode. At this size mpv inline is bound by the pty at about 4 frames per second (ADR-105: 4.1), so for it the gate is that kmscon presents what mpv sends, at the rate it sends it; mpv over shared memory is a compatibility measurement that gates nothing. In A6 it matches the client's send rate. kmscon's CPU time per frame stays below the frame interval. 3b's measurement sets the number. (Changed 2026-10-24: under the operator's principle that the spec is the target, "a poor player implementation isn't a spec we write against", mpv's shared memory mode, whose names the spec forbids, became a compatibility measurement, and the operator chose to keep `mpv --vo=kitty` inline as an A5 gate, an option they picked. This said "In A2, A5 and A6 the presented rate matches mpv's output rate (or the client's send rate in A6), and kmscon's CPU time per frame stays below the frame interval.")
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

1. **4a, libtsm kitty and the host hook.** APC framing, control keys, inline `f=24` and `f=32`, chunking, the actions, ids, numbers and placement ids, bands, cursor movement, deletes, replies, and eviction with ENOSPC. The vte's host hook and the input call that reports what it consumed, the pause, and the fixture host in `tsm-replay`. A fuzz target. Tier 1 through the spec-written client. Without a host, the other media answer EBADF and PNG and zlib EINVAL. The Notes of 2026-10-24 (Order of work) list the rest.
2. **4b, the helper,** built in parallel with 4a against the interface in the Notes of 2026-10-24. `kmscon-image-helper` with its identity, file and shared memory rules, PNG and zlib, and memfd results; the pty's pause and the hook's wiring in kmscon. Unit tests for every rule.

(Changed 2026-10-24: the operator chose to build step 4 with two agents in parallel, relayed as a summary (a paraphrase in the considered entry): build step 4 with two agents in parallel, libtsm's 4a with the host hook and kmscon's helper in 4b, once this interface is fixed. So the host hook and the fixture host move from 4b to 4a. 4a's heading was "4a, libtsm kitty.", and 4b read: "**4b, the host and the helper.** The vte's host hook and the input call that reports what it consumed; `kmscon-image-helper` with its identity, file and shared memory rules, PNG and zlib, and memfd results. Unit tests for every rule, and the fixture host in `tsm-replay`.")
3. **4c, kmscon.** Tier 2 screens for the lower two bands, text over pictures, and A6's banner over a changing picture.
4. **4d, acceptance.** Tier 3 with viu and the spec-written client through each medium, the root-file refusal, and the demo; A5 through both media (inline with mpv and the spec-written client, shared memory with the spec-written client and kitten icat), A6 in both bands, and A7. (Changed 2026-10-24 with A5's clients: under the operator's principle that the spec is the target, "a poor player implementation isn't a spec we write against", mpv's shared memory mode, whose names the spec forbids, became a compatibility measurement, and the operator chose to keep `mpv --vo=kitty` inline as an A5 gate, an option they picked. This said "A5 through both media".)
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
- At the login prompt, a `t=t` file or `t=s` object a program names is refused and left in place, since nothing may delete a user's file before anyone has logged in. (Added 2026-10-24 with the refusal before login.)
- Any program that writes to the pty, `cat` of an untrusted file included, can have the helper unlink any POSIX shared memory object the session's user owns (for example `/pulse-shm-*`), through `t=s` and through a discard. The spec requires the unlink after reading; kitty was seen unlinking objects it read (ADR-105), its handling of a failed read was not observed, and the discard of a refused command is this record's own choice. A filter on names is a question for aaronsb/kmscon#153. (Added 2026-10-24, from the code review.)
- The budget is shared, so a program on one screen can evict another screen's unplaced and scrollback images, and kitty images that screen shows; only sixel pictures on a screen's own lines are safe from it. (Changed 2026-10-24 with the storage-quota rule the operator chose; this said "another screen's unplaced or scrollback images".)

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
- **A vendored PNG decoder** (stb_image, spng). Rejected: libpng is a common system library, zlib is already a dependency of kmscon, and a vendored decoder is code the fork would maintain. (Corrected 2026-10-24: this said libpng and zlib were already loaded through FreeType, which holds only where FreeType is built in.)
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

- **The scaled pictures stay within the image budget now** ("fix it in 3b"). Until the setting and the budget shared with libtsm's stores land in step 3d (#125), bbulk computes `image-storage-limit=auto` from the displays it draws (four frames in ARGB, 16 MiB to 192 MiB) and keeps every display's scaled pictures within it at every moment, a frame's peak included. Between frames a display keeps at most twice its screen, or its share of the budget by size if that is less. A picture that would pass the budget is left out until the budget changes (pictures freed, a display added or removed), and frames in between do not compose it again. Each display counts once, however many sessions draw it, and a session that leaves the foreground frees its pictures. The scaler's working memory (at most 16 MiB) and the buffer a picture is rotated in (at most one screen) exist only while one picture is made and sit outside the budget. This replaces the bbulk section's bound of "twice the screen's size" as the only limit: at 7680x2160 a frame could otherwise hold four screens, 265 MB, past the cap.
- **Targets** ("adopt and tighten others"). ADR-104 measured them. The figures below replace the Targets section's common-size numbers. The ADR-104 figures come from one core of the host the harness runs on, which is also what the small tier 3 machine's single vCPU is, and that core is fast. These targets leave about five times headroom over the measured 95th percentile, for the slower cores of the low-end laptops and single-board computers the small machine stands for. They still fail a regression that slows composition several times over.
  - S4: at most 4 ms per frame at 1366x768 (measured 0.68 ms median, 0.83 ms 95th percentile) and 8 ms at 1920x1080 (1.42 ms and 1.70 ms). At 30 frames per second that is 12% and 24% of one core.
  - S5: at most 1.25 times S4 at the same size (measured 1.16 and 1.10 times). A banner over a full-screen picture should cost little more than the picture.
  - S3: at least 60 lines per second, one line per 60 Hz frame (the renderer alone allows over 500).
  - S1: within 5% of main, unchanged (measured +0.8% to +2.4%).
  - 7680x2160, adopted from ADR-104: S4 and S5 at most 16.7 ms per frame, one 60 Hz frame interval (measured 11.9 ms and 12.5 ms median, at most 14.4 ms at the 95th percentile). This replaces "no number until the first measurement". 7680x2160 is the largest display at hand. 7680x4320 (full 8K) is measured in ADR-104 for information (S4 24.3 ms, S5 24.7 ms median) and is not a target, as the operator has no such display; ADR-104 also computes each size's memory under the budget, where the 192 MiB ceiling holds one and a half full-8K screens.
- **Sixel cursor placement follows foot, not xterm** ("Xterm seems incorrect, pushing an unwanted newline"). xterm 411, run as a black box, matched 6 of 8 cursor cases. It scrolls one extra row at the bottom margin, and with mode 8452 it wraps to the next line at the right edge. The Sixel section's foot rules stand. Where xterm and foot differ, foot's rule is the reference, not xterm's black-box answer.
- **The sixel decoder keeps its per-call work bound** and adds no total cap per byte.
- **XTSMGRAPHICS:** a register count outside 2 to the maximum answers status 3. **XTWINOPS 14 and 16** answer even with sixel off.

## Notes (2026-10-23, operator answers from the step 3d review)

Recorded as considered entries above; what they change:

- **Sixel only where it can be drawn** ("why not sketch the outline and include the flag to deactivate it so that sixel support is truthfully unavailable"). Each renderer declares whether it draws image placements: bbulk does, gltex does not until step 3f, and any other renderer without placement drawing does not. A terminal offers sixel (DA1 attribute 4, XTSMGRAPHICS) and stores pictures only while every display it draws on has a renderer that does; otherwise it behaves as if `sixel` were off, offering nothing and storing no new picture. Pictures it stored before such a display arrived stay, counted in the budget and not drawn, until they are cleared or evicted, and show again when the display goes. Displays drawn by a renderer without placements do not count toward `image-storage-limit=auto`. gltex keeps the hook where step 3f will compose placements (`gltex_draw_placements`).
- **A shrinking auto limit evicts at the next reservation**, accepted by the operator. This refines the Memory section's "A smaller budget evicts down to fit": a renderer is unset and set again on every font change and zoom, which uncounts its display and counts it back at once, and evicting on that dip threw away pictures the budget could hold. The auto limit therefore follows the displays without evicting, and the next reservation evicts down to the limit it finds. A set limit that shrinks still evicts at once.
- **`image-storage-limit` takes K, M and G** (KiB, MiB, GiB) beside plain bytes, kept by the operator, and at most 1 TiB: a display's share of the budget is the limit times its pixels over all the displays', computed in 64 bits.

## Notes (2026-10-23, operator answer to Q1)

Recorded as a considered entry above; what it changes:

- **A picture on screen may evict the history** ("yes"). On a display that needs a scaled copy of a picture (a rotated display, a zoom, or placements made in a cell size other than the font's), bbulk makes that copy within the frame, and until now it evicted nothing outside its own renderer: while scrollback images held the budget, the picture on screen was left out. The scaled copy for an on-screen placement now takes its room in ADR-103's order after bbulk's own pictures the frame does not place: other displays' pictures their last frame did not draw, then libtsm's images unplaced, then those placed only in scrollback, least recently used first. It never evicts an image placed on a screen's own lines, nor one the frame draws, such as a scrollback image in view while the view is scrolled back; a picture that would need one is still left out, and retried only when the budget changes. Viewing a new picture on such a display can therefore push older pictures out of scrollback, which then show blank cells when scrolled back to, as old lines drop off the end of scrollback. A display that draws pictures at their own size makes no scaled copy and is unchanged. (2026-10-24: unchanged by the storage-quota rule the operator chose that day, which evicts kitty images on a screen's own lines only for a new image's reservation in libtsm's stores; see the Image store section.)
- History is evicted only when the eviction lets the picture fit. Before anything is freed, bbulk counts what can be: the pictures other displays' last frames did not draw, and each screen's victims (libtsm's `tsm_screen_image_victims`, which evicts nothing) up to the first image the frame draws. When that falls short, nothing is evicted and the picture is left out until the budget changes, as libtsm's own reservation evicts nothing for an image it cannot fit. Every terminal's screens give room, main and alternate, each as far as its own first drawn image.

## Notes (2026-10-23, operator answers from step 3e, kmscon #145)

Recorded as considered entries above; what they change:

- **A1 uses chafa and mpv, not img2sixel** ("I have typically been using chafa"). img2sixel writes nothing on Arch with libsixel 1.10.5 and gdk-pixbuf 2.44 (glycin), for any input: libsixel's `load_with_gdkpixbuf` (src/loader.c) calls `gdk_pixbuf_loader_get_pixbuf` right after the write and before `gdk_pixbuf_loader_close`, gets no picture from glycin, and returns success with no frame, which also skips its built-in loaders. A1's photographs are therefore drawn twice side by side for comparison ("we could use both as a comparison"), neither encoder the main one: by chafa (`chafa -f sixels`, sized in cells), the operator's everyday tool, and by libsixel's encoder by way of mpv (`--vo=sixel`, ffmpeg decoding). The page prints how many bytes img2sixel wrote, so a libsixel that reads images again shows by itself. This replaces "img2sixel pictures" in the Acceptance scenarios and img2sixel in Testing's tier 3 line.
- **The photographs** are NASA Visible Earth images of the Earth by day and by night, 2048x1024 (`/usr/share/xplanet/images/earth.jpg` and `night.jpg` from xplanet 1.3.1, Arch extra). xplanet's images/README cites visibleearth.nasa.gov and says "These images are not copyrighted", NASA imagery being usable for educational and informational purposes. Nothing is downloaded at test time and no picture is committed.
  - Changed 2026-10-24 (kmscon #145): A1 draws one photograph, the operator's own street at sunset (2040x1536), committed beside the screens as `fork/tier3/guest/screens/photo/street-sunset.jpg` under CC BY 4.0 with its credit in `photo/LICENSE`, in place of xplanet's two. The guest no longer installs xplanet. Nothing is downloaded at test time still; the picture is committed.
- **A2 at 7680x2160 is a known failure** while kmscon is the bottleneck there (aaronsb/kmscon#139): tier 3's `mpv-sixel` case at both 7680x2160 variants passes with a note naming the issue while it fails, and fails once it passes with the entry still in place, in the style of the capability probes. Both entries were removed in the same PR, once aaronsb/kmscon#146 and #152 made A2 pass at 7680x2160 (ADR-104); the mechanism, `KNOWN_BOTTLENECKS` in tier 3's `t3_evaluate.py`, stays. ADR-104's "A1, A2 and A4 in tier 3" has the figures.
  - Changed 2026-10-24 (kmscon #145, with the operator's approval): tier 3's `mpv-sixel` and the VT demo's page play A2 in a 1920x1080 window in the middle of the screen, the largest 16:9 window that fits on a smaller screen, with `--vo-sixel-dither=none`. Full screen at 7680x2160, mpv's own libsixel encoding holds it to about 4.4 frames a second with no terminal attached, so a full-screen run there measured mpv's encoder; at 1920x1080 without dithering mpv sends all 150 frames. The criterion, that kmscon is not the bottleneck, is unchanged. The A4 pages keep their panes as they were: at 7680x2160 under half of the panes' frames reach the screen (41-44% on the operator's machine, 23-30% in tier 3's guest), and at 3x3 tmux itself is the limit.

## Notes (2026-10-24, the host hook, the helper protocol and step 4's order)

The interface between libtsm and kmscon for files, shared memory, PNG and zlib, fixed so that libtsm's half and kmscon's half of step 4 can be built at once. It refines the Parsing, Host interface, The image helper, Kitty graphics protocol, Memory, Acceptance scenarios, Testing and Order of work sections; what they decide stands except where a bullet here says what it replaces. Every rule here rests on the kitty graphics protocol spec's text, or, where the spec leaves a point open, on a choice this record makes and says it makes. ADR-105, a black-box probe of mpv and kitty, and ADR-106, of five more clients (timg, ncplayer, kitten icat, chafa and viu), are compatibility evidence: they say which clients a rule affects, and no rule rests on them.

### Operator answers

Recorded as considered entries above; what they change:

- **The helper's hardening is as this record states** ("As ADR-103 says", an option the team lead wrote): the session user's uid, primary gid and supplementary groups, `PR_SET_NO_NEW_PRIVS`, the ambient capabilities cleared (`PR_CAP_AMBIENT_CLEAR_ALL`, added 2026-10-24 from the code review of #164), working directory `/`, every descriptor closed but its socket, and a memory limit. The option named `initgroups`; the groups are now resolved before the fork with `getgrouplist` and set with `setgroups` in the child (The image helper, Identity), since `initgroups` is not safe in a child forked from a threaded process. A seccomp or landlock filter is a follow-up, aaronsb/kmscon#153, outside step 4.
- **sd-login is found at build time** ("auto. this way we have first class support for systemd, elongind and friends, and bare"): the build option below defaults to auto, so a systemd or elogind system gets logind's answer and a system with neither gets the pty's owner, with no configuration. (2026-10-24: a root pty owner is the exception, under the operator's "Never read as root by fallback"; see The image helper, Identity.)
- **Step 4 is built by two agents in parallel** once this section lands: libtsm's 4a, which now takes the host hook, and kmscon's 4b, the helper and its wiring. Order of work, below, says what moves.
- **The showcase's acts 6 and 7** (#85, #86, #87) are done with 4d.
- **The spec is the target** ("a poor player implementation isn't a spec we write against"). A rule exists because the spec says so, or because the spec leaves the point open and this record chooses; never because a client does something. Where a client departs from the spec, kmscon does what the spec says, and the record names the clients that affects. The probes are evidence about clients, not requirements, and acceptance judges conformance with spec-conformant clients (Acceptance, below). The operator's further suspicion, that the clients were written against an older spec that later changed, is in their own words "a claim not backed by evidence": no rule here rests on it, and Spec version annotations, below, says what the spec's own marks can show.

### libtsm: the host hook

Declared in `libtsm.h` behind a feature macro, as sixel's calls are:

```c
#define TSM_VTE_IMAGE_HOST 1

enum tsm_vte_image_medium {
	TSM_VTE_IMAGE_DIRECT = 'd',	/* t=d: PNG or zlib data sent inline */
	TSM_VTE_IMAGE_FILE = 'f',	/* t=f */
	TSM_VTE_IMAGE_TEMP = 't',	/* t=t */
	TSM_VTE_IMAGE_SHM = 's',	/* t=s */
};

/* what the host does with the bytes it reads */
#define TSM_VTE_IMAGE_INFLATE	0x01	/* zlib-inflate them first */
#define TSM_VTE_IMAGE_PNG	0x02	/* decode them as PNG; without it, return them as they are */
#define TSM_VTE_IMAGE_EXACT	0x04	/* the inflated bytes must number exactly min_bytes */
#define TSM_VTE_IMAGE_DISCARD	0x08	/* refused: remove a TEMP or SHM object, read nothing */

struct tsm_vte_image_request {
	uint64_t id;		/* names the request in tsm_vte_image_reply; from 1, per vte */
	unsigned int medium;	/* enum tsm_vte_image_medium */
	unsigned int flags;	/* TSM_VTE_IMAGE_* */
	uint64_t offset;	/* FILE, TEMP, SHM: where the read starts; else 0 */
	uint64_t read_max;	/* FILE, TEMP, SHM: the most bytes to read, 0 to the end; else 0 */
	uint64_t min_bytes;	/* the fewest bytes the image needs, after inflating; without
				   TSM_VTE_IMAGE_PNG exactly these come back; 0 for none */
	const char *name;	/* FILE, TEMP, SHM: the path or name, NUL-terminated; else NULL */
	const uint8_t *data;	/* DIRECT: the payload, base64-decoded; else NULL */
	size_t data_len;
};

typedef int (*tsm_vte_image_cb)(struct tsm_vte *vte,
				const struct tsm_vte_image_request *req,
				void *data);

void tsm_vte_set_image_cb(struct tsm_vte *vte, tsm_vte_image_cb image_cb,
			  void *image_data);

/* what came of a request, in the host's terms; libtsm maps it to the kitty reply */
enum tsm_vte_image_status {
	TSM_VTE_IMAGE_OK = 0,
	TSM_VTE_IMAGE_UNREADABLE,	/* not opened, refused by a path or name rule,
					   the offset at or past the end, or a read error */
	TSM_VTE_IMAGE_TOO_SHORT,	/* fewer bytes than min_bytes, read or inflated */
	TSM_VTE_IMAGE_CORRUPT,		/* a zlib or PNG stream that does not decode, or more
					   than min_bytes inflated under TSM_VTE_IMAGE_EXACT */
	TSM_VTE_IMAGE_TOO_LARGE,	/* PNG dimensions past max-graphic-size */
	TSM_VTE_IMAGE_OVER_BUDGET,	/* read, inflated or decoded past the budget */
	TSM_VTE_IMAGE_FAILED,		/* the host could not finish: no helper, a timeout, a bad reply */
};

struct tsm_vte_image_result {
	unsigned int status;		/* enum tsm_vte_image_status */
	unsigned int width;		/* TSM_VTE_IMAGE_OK with PNG: the decoded size; else 0 */
	unsigned int height;
	const uint8_t *data;		/* TSM_VTE_IMAGE_OK: with PNG, width x height x 4 bytes of
					   RGBA; else the min_bytes bytes read; a discard: NULL */
	size_t len;
};

int tsm_vte_image_reply(struct tsm_vte *vte, uint64_t id,
			const struct tsm_vte_image_result *res);

#define TSM_VTE_KITTY_GRAPHICS 1
/* kitty-graphics: off until the host turns it on, as tsm_vte_set_sixel */
void tsm_vte_set_kitty_graphics(struct tsm_vte *vte, bool enable);

/* the eviction rank step 4 adds (Image store); an enum value cannot be
 * tested with #ifdef, so a host tests this macro before naming it */
#define TSM_SCREEN_IMAGE_PLACED_KITTY 1
enum tsm_image_rank { /* ... */ TSM_IMAGE_PLACED_KITTY = 3, };

struct tsm_image_victim {
	/* ... */
	/* the order within its rank, on the clock all screens in the process
	 * share: for ranks 0 to 2, when it was last created, placed or drawn;
	 * for TSM_IMAGE_PLACED_KITTY, when it was transmitted */
	uint64_t last_use;
	/* ... */
};
```

- **libtsm reads the kitty spec; the host does not.** libtsm turns each command's keys into the request's terms, and the host's outcome back into the kitty reply, so a change to the spec's rules on `S`, lengths or reply codes touches libtsm alone. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": the request carried the kitty keys themselves, `format`, `zlib`, `width` (`s`), `height` (`v`) and `size` (`S`), and the reply an errno.)
- **Before the hook,** libtsm refuses with EINVAL a raw format whose `s` or `v` passes `max-graphic-size`, or that gives `s=0` or `v=0` explicitly, as `tsm-kitty.c` does; neither reaches the host.
- **What libtsm asks for,** command by command (bpp is 3 for `f=24` and 4 for `f=32`):

| Command | `flags` | `offset` | `read_max` | `min_bytes` |
|---|---|---|---|---|
| `t=f`, `t=t`, `t=s`, raw, no `o=z` | none | `O` | `S` | `s` x `v` x bpp |
| `t=f`, `t=t`, `t=s`, raw, `o=z` | `INFLATE` | `O` | `S` | `s` x `v` x bpp |
| `t=f`, `t=t`, `t=s`, `f=100` | `PNG`, and `INFLATE` with `o=z` | `O` | `S` | 0 |
| `t=d`, raw, `o=z` | `INFLATE` | 0 | 0 | `s` x `v` x bpp |
| `t=d`, `f=100` | `PNG` | 0 | 0 | 0 |
| `t=d`, `f=100`, `o=z` | `PNG`, `INFLATE`, and `EXACT` when `S` is non-zero | 0 | 0 | `S` when non-zero, or 0 |
| a refused `t=t` or `t=s` | `DISCARD` | 0 | 0 | 0 |

- **`t=d`, `f=100`, `o=z` without `S`.** The spec says "if you use both PNG and compression, then you must provide the S key with the size of the PNG data", and its control-data table gives `S` the default 0, so libtsm cannot tell an absent `S` from `S=0` and reads both as no `S`. This record chooses to accept such a command rather than refuse it: libtsm sends `PNG` and `INFLATE` without `EXACT`, the helper inflates up to the budget and decodes what comes out, and the PNG's own structure ends it. The spec names no reply for a client that leaves `S` out, and refusing would turn away a stream every decoder can read. A non-zero `S` holds the inflated length to it exactly, as the spec words the rule. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the row said "`EXACT` when `S` is given" and "`S`, or 0", which left an explicit `S=0` sending `EXACT` with `min_bytes` 0; this bullet is new.)

- **What libtsm answers,** outcome by outcome:

| Outcome | `t=f`, `t=t`, `t=s` without `INFLATE` | with `INFLATE`, or inline |
|---|---|---|
| `OK` | stored, placed and answered OK, after the checks below | the same |
| `UNREADABLE` | EBADF | EBADF |
| `TOO_SHORT` | EBADF | EINVAL |
| `CORRUPT` | EINVAL | EINVAL |
| `TOO_LARGE` | EINVAL | EINVAL |
| `OVER_BUDGET` | ENOSPC | ENOSPC |
| `FAILED` | EBADF | EBADF |

- **What reaches the hook.** A transmission (`a=t`, `a=T` or `a=q`) with `t=f`, `t=t` or `t=s`, and one with `t=d` whose format is `f=100` or that carries `o=z`. libtsm calls the hook once, when the command's last chunk ends, after checking its keys. The string ends at the byte that ends it: the ESC of `ESC \`, or a C1 ST; the `\` may arrive in a later read and is parsed after the reply. CAN and SUB abort the command without calling the hook. A command libtsm refuses itself (`i` with `I`, a raw format without `s` and `v`, an action left to step 5) reaches the host only as a discard (next bullet), and inline `f=24` or `f=32` without `o=z` stays in libtsm. An `a=q` with `I` and no `i` never reaches the hook, not even as a discard: libtsm drops it at its ST, reads nothing, deletes no `t=t` file or `t=s` object, and sends no reply, as kitty 0.49.2 run as a black box does (ADR-107, rounds 4 and 5). (Added 2026-10-24, from the re-check of aaronsb/kmscon#164, which found the no-reply rule left this command reaching the hook.)
- **A refused `t=t` or `t=s` command is still discarded.** The spec gives the removal of these objects to the terminal (`t=t`: "the terminal emulator will delete the file after reading"; `t=s`: it "must read the data from the memory object and then unlink and close it") and says nothing of a command the terminal refuses. This record chooses that a refusal does not hand the removal back to the client, since nothing in the spec tells the client to remove the object. A client this affects is kitten icat, whose GIFs send their frames as `a=f` with `t=s`, an action left to step 5 (ADR-106). In a harness run that read and removed none of its objects, its 20 frame objects were still in `/dev/shm` at a later check (ADR-106): icat leaves its frame objects to the terminal, so without the discard each refused frame would leave its object behind. The rule rests on the spec; this run shows which client it serves. libtsm therefore hands such a command to the hook with `TSM_VTE_IMAGE_DISCARD` set (changed 2026-10-24 with the request's flags, Notes "S and data length; a mechanical helper": this said "with `discard` set"); the host removes the object under the helper's rules, reads nothing, and replies. libtsm waits for that reply as for any other, so order holds, and then answers the client with its own refusal, whatever the reply says; a hook that returns a negative value for the discard, as kmscon's does before login, gets the same refusal at once (The hook's answer). (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the clause on a negative return is added.) A refused `t=f` or `t=d` command does not reach the hook.
- **The name.** The payload of `t=f`, `t=t` and `t=s` is the path or name in base64. libtsm decodes it into at most 4095 bytes. An empty name, a longer one, or one holding a NUL byte answers EBADF without a request, the same reply as an unreadable file.
- **All integers are 32-bit,** as the spec's control-data table says: libtsm refuses `O` or `S` above `UINT32_MAX` with EINVAL, so the request's 64-bit `offset` and `read_max` never exceed it. (Changed 2026-10-24 with the request's fields: this said "the request's 64-bit `offset` and `size` never exceed it".)
- **`m` applies to direct transmission only.** A `t=f`, `t=t` or `t=s` command is complete at its own ST, whatever its `m`, and a following APC is a new command. The spec defines chunking only under remote clients, "those that are unable to use the filesystem/shared memory to transmit data", which "must send the pixel data directly using escape codes. Since escape codes are of limited maximum length, the data will need to be chunked up for transfer. This is done using the m key"; a name is not pixel data and needs no chunking, and the spec says nothing of `m` for the other media, so this record reads `m` as a direct-transmission key. The rule rests on that text alone. It also keeps mpv 0.41.0 working, which sends every shared memory frame as one whole command with `m=1` and never an `m=0`, and kitty 0.49.2 shows such frames (ADR-105); but mpv's shared memory frames are refused for their name in any case (Clients that depart from the spec, below). This replaces the Parsing section's chunk rule for the other media, and that section is edited to match, with a dated note.
- **Inline PNG and zlib data** are base64-decoded into a buffer as they arrive, each growth reserved from the screen's image budget before it is allocated. A refused reservation fails the upload with ENOSPC, and the rest is ignored until ST. This replaces the Parsing section's EINVAL for PNG or compressed data past `image-storage-limit`, and that section is edited to match, with a dated note: the budget is that bound, and its refusal is the storage-limit error. Raw data without `o=z` stays in libtsm: it needs at least `s` x `v` x bytes per pixel, ignores the excess, and answers EINVAL for fewer. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "Raw data without `o=z` keeps its EINVAL past `s` x `v` x bytes per pixel.")
- **Ownership.** The request and everything it points to are libtsm's and valid only during the hook call; a host copies what it keeps. When the hook returns, libtsm frees the inline buffer and releases its reservation.
- **The hook's answer.** 0 means the host took the request. libtsm keeps the command's keys and stops: the input call returns the bytes up to and including the byte that ended the string, and every input call after it returns 0 and consumes nothing until the reply. A negative value answers the request at once as `TSM_VTE_IMAGE_FAILED`, EBADF to the client, and parsing continues in the same call; for a `DISCARD` request the negative value is ignored, as any reply status is, and libtsm answers the client with its own refusal of the command, so a discard has one reply whatever the host does. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "A negative errno answers the request at once (-EBADF, -EINVAL and -ENOSPC answer their code, any other value EBADF)".) (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the discard's exemption is added; this said "A negative value answers the request at once as `TSM_VTE_IMAGE_FAILED`, EBADF to the client" for every request, while the discard bullet above gave libtsm's own refusal "whatever the reply says", two replies for one command before login.)
- **The reply.** `tsm_vte_image_reply` names the request's `id` and gives a result: a status, and with `TSM_VTE_IMAGE_OK` the bytes. For a PNG request the bytes are `width` x `height` pixels of straight-alpha 8-bit R, G, B, A, rows top to bottom with no padding, the `f=32` layout; libtsm checks the size against `max-graphic-size` (EINVAL otherwise). For any other request they are exactly `min_bytes` bytes, the first of what was read and inflated, which libtsm converts as `f=24` or `f=32` data of `s` by `v` pixels, as it converts inline data. libtsm reads them only during the call. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "`tsm_vte_image_reply` names the request's `id` and gives either `error` 0 with `width`, `height` and `rgba`, or `error` -EBADF, -EINVAL or -ENOSPC", the host returning RGBA for raw formats too and libtsm checking it against `s` and `v`.) Unless the action is `a=q`, it then reserves width x height x 4 plus `TSM_SCREEN_IMAGE_OVERHEAD` from the budget (which may evict, or refuse with ENOSPC), allocates, converts to premultiplied ARGB with the opaque flag, stores, places the image for `a=T`, and replies to the client as the Kitty graphics section says. `a=q` reserves and stores nothing and replies as a transmission would. The client's reply text is libtsm's: the host passes a code, never a message.
- **Its return value:** 0 when the request is answered; -ENOENT when `id` is not the request outstanding, a stale reply after a cancel, and nothing happens; -EINVAL for a result that disagrees with its request (an unknown status; with `TSM_VTE_IMAGE_OK`, no bytes, a zero PNG size, or a length other than `width` x `height` x 4 or `min_bytes`; a discard is exempt, any status accepted and ignored), which also ends the request with EBADF to the client, so a host's mistake cannot leave the vte paused; -EBUSY when called from inside the hook, leaving the request out. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "-EINVAL for a NULL `rgba` or a zero size with `error` 0", with no exemption for a discard.) The conversion runs inside this call, outside `tsm_vte_input_bounded`'s work bound: at most one image per request, at most `max-graphic-size` and the budget, in its own turn of the host's loop.
- **The placement's cursor** is the cursor when the reply arrives. Input is paused, so that is the cursor at the command's ST unless the host resized the screen meanwhile, which clamps it as resize clamps any cursor.
- **Cancelling.** `tsm_vte_reset`, `tsm_vte_hard_reset`, turning `kitty-graphics` off, and freeing the vte drop an outstanding request without a reply to the client. Installing or removing the hook while a request is out answers it EBADF, or, for a discard, with libtsm's own refusal of the command. (Changed 2026-10-24, from the re-check of aaronsb/kmscon#164: this said "Installing or removing the hook while a request is out answers it EBADF.")
- **Without a hook,** as the Host interface section says, `t=f`, `t=t` and `t=s` answer EBADF and `f=100` or `o=z` answer EINVAL, at the command's ST; libtsm buffers no inline data for them.
- **`tsm_vte_input`** never hands out a request: commands that need the host answer there as they do without a hook. A host that installs the hook feeds input through `tsm_vte_input_bounded`. Bytes passed to `tsm_vte_input` while a request is out are a host error, dropped with a warning. `tsm_vte_input_bounded`'s comment changes from "at least one when @len is not 0" to "at least one when @len is not 0 and no request is out".
- **One request per vte** follows from the pause: while one is out nothing is parsed, so no second command reaches the hook, and commands keep their order.
- **Replies keep that order too.** A query that follows a host request in the input is parsed, and answered, only after the request's reply, so its answer follows the request's. The spec asks for this: its detection method is a query followed by DA1, and "terminal emulators that support the graphics protocol, must reply to query actions immediately without processing other input". Clients that rely on it (ADR-106): kitten icat, which sends `a=q` direct, `a=q,t=t` and `a=q,t=s`, then DA1, the spec's detection sequence (no run shows what icat does when the order is broken); viu, which sends DSR after each frame; and ncplayer, which closes its queries with DA1.

### kmscon: the pause and the helper's lifetime

- `kmscon_pty` gains `kmscon_pty_pause` and `kmscon_pty_resume`. While paused, the pty neither reads nor hands its held bytes to the input callback; resuming hands them over from the next turn, as the held buffer does now, and then reads again. The terminal pauses its pty from inside the hook, taking effect when the input callback returns, and the bytes after the command's ST stay held. Other ptys, drawing, input and VT switching carry on. Every path that feeds the input callback honours the pause, including `feed()`'s fallback in `src/misc/pty.c`, which today loops `while (off < len)` calling the callback when the idle source cannot be registered: with a request out the callback consumes nothing, so that loop would spin forever in the root process, and the idle source would re-register and spin as well. So pausing calls `feeding_stop`, `feed` and `read_buf` check the pause after each call of the callback and leave the rest held, and resuming calls `rearm()`, since the pty's descriptor is edge-triggered, then feeds what is held. `terminal_close` and `terminal_open` clear the pause (a restart after the child exits goes through both, with `tsm_vte_hard_reset`).
- In the hook, kmscon finds the session's user (The image helper, Identity). Before login it returns a negative value at once for `t=f`, `t=t` and `t=s` (answered EBADF, or for a discard with libtsm's own refusal, The hook's answer; changed 2026-10-24, from the code review of aaronsb/kmscon#164: this said "(answered EBADF)" for a discard too), and runs inline PNG and zlib requests in a helper as `kmscon-image`, or `nobody` when that user is absent, refusing them when neither exists (Identity). It looks the user up with `getpwnam_r` in its own process before forking, as it resolves every identity. Otherwise it starts the helper when none runs or its user differs, sends the request, arms a 2-second timer and watches the helper's socket. A helper that cannot be started, or a send that fails, returns a negative value from the hook, which answers the command at once as `TSM_VTE_IMAGE_FAILED` and lets input continue. (Changed 2026-10-24 with the refusal before login and the request's outcomes: this said "In the hook, kmscon finds the session's user (The image helper, Identity), starts the helper when none runs or its user differs", and "A helper that cannot be started, or a send that fails, returns -EBADF from the hook".)
- A reply that passes kmscon's checks (below) ends the request: kmscon calls `tsm_vte_image_reply`, sets `redraw_owed` (as `pty_input` does today), disarms the timer and resumes the pty. If libtsm answers -ENOENT, because the request was cancelled meanwhile, kmscon resumes all the same.
- **A failure ends the request with EBADF:** the timer firing, the helper's socket closing or failing, or a reply that fails a check. kmscon kills the helper with `SIGKILL` through a pidfd taken when it started, lets the loop's child callback reap it (`ev_eloop_register_child_cb`; the loop reaps every child with `waitpid(-1)`, so kmscon never signals a pid it has seen reaped), closes the socket, replies `TSM_VTE_IMAGE_FAILED` to libtsm, which answers EBADF, or for a discard ignores it and answers its own refusal (libtsm: the host hook, The hook's answer), and resumes the pty. (Changed 2026-10-24 with the request's outcomes: this said "replies -EBADF to libtsm and resumes the pty".) (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the discard's exemption is added; this said "replies `TSM_VTE_IMAGE_FAILED` to libtsm, which answers EBADF" for every request.) The next request starts a new helper, so a decoder fault on a hostile PNG costs the client an EBADF and the helper a restart.
- **The helper is also restarted** when the session's user changes (this record), or when a request's `max_bytes` is larger than the one the running helper's memory limit was set from.
- Closing the terminal (its child exits) or destroying it kills the helper and drops the request without a reply; closing also cancels libtsm's request with `tsm_vte_reset`, so the next session starts with none out.
- **kmscon's own `tsm_vte_input` callers.** Asciicast playback (`asciinema_write` in `src/terminal.c`) is paused with the pty while a request is out (`kmscon_asciinema_pause`, and `kmscon_asciinema_resume` after the reply when the terminal is still active), so it feeds nothing during a request and loses no byte. `terminal_activate`, which today resumes playback whenever an opened terminal returns to the foreground, resumes it only when no request is out; otherwise the reply resumes it. The issue text (`/etc/issue`, fed when the terminal opens) is fed only when no request is out, since a request never survives a close. Both stay on `tsm_vte_input`, so file and PNG commands in a recording or an issue file are answered as without a hook: their paths name another machine's or another time's files.

### The helper protocol

- **Transport.** `socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC)`. The helper's end is descriptor 3 across `exec`; descriptors 0, 1 and 2 are open on `/dev/null`, so a library's write to stderr cannot land in a file the helper opened; every other descriptor is closed. A message is one packet: a fixed header in host byte order, both ends being built from one tree for one machine, then its variable part, with at most one descriptor (`SCM_RIGHTS`). kmscon receives with `MSG_CMSG_CLOEXEC`, and a packet that is short, truncated (`MSG_TRUNC`, `MSG_CTRUNC`) or carries a descriptor it should not is a failed reply.
- **The request,** kmscon to helper, 64 bytes with no padding:

```c
struct kmscon_image_request {
	uint32_t version;	/* 2 */
	uint32_t type;		/* 1, an image; other values reserved, #77's fonts among them */
	uint64_t id;		/* libtsm's request id */
	uint8_t medium;		/* 'd', 'f', 't' or 's' */
	uint8_t flags;		/* KMSCON_IMAGE_INFLATE 0x01, KMSCON_IMAGE_PNG 0x02,
				   KMSCON_IMAGE_EXACT 0x04, KMSCON_IMAGE_DISCARD 0x08,
				   libtsm's TSM_VTE_IMAGE_* values; other bits make the helper exit */
	uint16_t reserved;	/* 0, or the helper exits */
	uint32_t name_len;	/* 1 to 4095 for 'f', 't' and 's'; 0 for 'd' */
	uint64_t offset;	/* where the read starts; 0 for 'd' */
	uint64_t read_max;	/* the most bytes to read, 0 to the end; 0 for 'd' */
	uint64_t min_bytes;	/* the fewest bytes the image needs, after inflating; without
				   KMSCON_IMAGE_PNG or _DISCARD, at least 1 and exactly these come back */
	uint32_t max_width;	/* max-graphic-size */
	uint32_t max_height;
	uint64_t max_bytes;	/* image-storage-limit, the whole budget */
	/* name_len bytes of name follow, without a NUL */
};
```

- The request is 64 bytes with no padding: `version` at 0, `type` 4, `id` 8, `medium` 16, `flags` 17, `reserved` 18, `name_len` 20, `offset` 24, `read_max` 32, `min_bytes` 40, `max_width` 48, `max_height` 52, `max_bytes` 56. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": version 1 carried `uint16_t format`, `uint32_t width`, `uint32_t height` and `uint64_t size`, the kitty keys `f`, `s`, `v` and `S`, and its flags were "KMSCON_IMAGE_ZLIB 0x01 (o=z), KMSCON_IMAGE_DISCARD 0x02".)

- For `'d'`, the packet carries a memfd holding the inline data, which kmscon writes and seals (`F_SEAL_SHRINK`, `F_SEAL_GROW`, `F_SEAL_WRITE`, `F_SEAL_SEAL`) before sending and closes once sent.
- **Limits travel with each request,** so a changed `max-graphic-size`, or a budget `auto` recomputes, applies from the next request with no other message. The helper's memory limit (`RLIMIT_AS`) is set when it starts: three times `max_bytes`, for the data read, an inflated copy and the output, plus 64 MiB for code and libraries. That holds because the PNG decoder writes its rows straight into the output memfd's mapping, and the helper reads or maps only the bytes its request bounds: from `offset` to the lesser of `offset` + `read_max` and the end, stopping at `max_bytes`, and without inflating or decoding only `min_bytes` of them. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "the helper reads or maps only the bytes from `O` to `O` + `S` of a file or shared memory object, never the whole object".)
- **A discard** (`KMSCON_IMAGE_DISCARD`) removes the object as the helper's rules allow and reads nothing from it. A discard carries `offset`, `read_max` and `min_bytes` 0, no other flag, and medium `'t'` or `'s'`; any other discard makes the helper exit (The outcomes, below). (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the flags and media a discard may carry are added; this said only "A discard carries `offset`, `read_max` and `min_bytes` 0".) For `'s'`, the helper opens the name as the Shared memory bullet does and unlinks it there by device and inode. For `'t'`, it resolves and opens the path as the Files bullets do, following symlinks as the spec asks, and requires a regular file; it deletes only when the resolved path passes the temporary-file rule, and then deletes the resolved file through the Deleting bullet's `openat2` and `unlinkat` check, never a symlink that led to it. A FIFO, or a symlink whose target is not a regular file or fails the temporary-file rule, is refused and nothing is deleted. Following the final symlink keeps a discard to the rule a `t=t` read follows; refusing it with `O_NOFOLLOW` would go beyond the spec, which asks for symlinks to be followed. Whether or not it deleted, the helper answers `KMSCON_IMAGE_OK` with no descriptor, and libtsm accepts any status for a discard and ignores it, answering the client with its own refusal. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said the helper "answers status -EINVAL with no descriptor".) (Changed 2026-10-24, from the code review of aaronsb/kmscon#164, matching #162 and the Shared memory bullet: for `'s'` this said "the helper opens the name read-only, requires a regular object, and calls `shm_unlink`".)
- **What the helper does,** in order, knowing nothing of the kitty keys:
  1. Opens the source: a file or shared memory object under this record's rules, which stand, or the memfd for `'d'`. A failure, or a refused path or name, is `UNREADABLE`. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the offset check moves to step 3, after step 2's delete; this said "A failure, a refused path or name, or an `offset` at or past the end of a file or object is `UNREADABLE`.")
  2. A `'t'` file that passed the temporary-file rule and was opened is deleted after this step, whatever the outcome of the steps after it, as kitty and Ghostty delete it (Deleting, in The image helper); a `'s'` object is unlinked once opened, as before.
  3. For a file or shared memory object, an `offset` at or past its end is `UNREADABLE`, found after step 2's delete, so a `'t'` file or `'s'` object with such an offset is still removed, as kitty removes it (ADR-107). Then it reads from `offset` up to `read_max` bytes, or to the end when `read_max` is 0. Without `INFLATE` or `PNG` it reads only `min_bytes`: when the lesser of `read_max` (unbounded at 0) and the bytes available from `offset` is less than `min_bytes`, the outcome is `TOO_SHORT`, before anything is read; whatever lies past `min_bytes` is never read, so a large file costs no budget. With `INFLATE` or `PNG` it reads what the bounds allow, and more than `max_bytes` to read is `OVER_BUDGET`, found before it is read. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the offset check moves here from step 1, and "Otherwise it reads what the bounds allow" becomes "With `INFLATE` or `PNG` it reads what the bounds allow", so a raw read of a large file cannot be `OVER_BUDGET`.)
  4. With `INFLATE`, inflates what it read, stopping at `max_bytes` (`OVER_BUDGET` past it). It stops early on these conditions: without `PNG` or `EXACT`, once `min_bytes` are out, and the rest is ignored; under `EXACT`, with or without `PNG`, once one byte past `min_bytes` is out, which is `CORRUPT`, checked before the `max_bytes` bound. A stream that does not inflate is `CORRUPT`; fewer than `min_bytes` out is `TOO_SHORT`. Under `EXACT` the inflated length must therefore be exactly `min_bytes`: fewer is `TOO_SHORT`, any byte past it `CORRUPT`. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: this said "and without `PNG` stopping once `min_bytes` are out" and "Under `EXACT` the inflated length must be exactly `min_bytes`: fewer is `TOO_SHORT`, more `CORRUPT`", which for `INFLATE` and `EXACT` without `PNG` both stopped at `min_bytes` and asked for more to be found.)
  5. With `PNG`, reads the header; dimensions past `max_width` or `max_height` are `TOO_LARGE`, and width x height x 4 past `max_bytes` is `OVER_BUDGET`, both before any pixels are allocated. It decodes with libpng to 8-bit RGBA, palette, grey, 16-bit and interlaced images expanded, with no gamma or colour-profile correction; a stream that does not decode is `CORRUPT`.
  6. Without `PNG`, returns exactly the first `min_bytes` bytes, as they were read or inflated; bytes past them are ignored.
  7. Reports in `ignored` how many bytes it left past what it used, clamped to `UINT32_MAX`: without `INFLATE` or `PNG`, the lesser of `read_max` (unbounded at 0) and the bytes available from `offset`, less `min_bytes`, taken from `fstat` and never by reading them; with `INFLATE` or `PNG`, the input bytes left unconsumed when it stopped. It is 0 on an error or discard reply. (Changed 2026-10-24, from libtsm #51's build, which found that under version 2 libtsm never sees the bytes the helper skips, and the helper, its stderr on `/dev/null`, has no log of its own: there was no step 7.)
- **The outcomes** are the request's terms, not kitty's: `KMSCON_IMAGE_OK` 0, `KMSCON_IMAGE_UNREADABLE` 1, `KMSCON_IMAGE_TOO_SHORT` 2, `KMSCON_IMAGE_CORRUPT` 3, `KMSCON_IMAGE_TOO_LARGE` 4 and `KMSCON_IMAGE_OVER_BUDGET` 5, libtsm's `tsm_vte_image_status` values, which libtsm maps to reply codes (libtsm: the host hook, above). The helper never sees whether a failure counts as EBADF or EINVAL. A `version`, `type`, flag bit or `reserved` value the helper does not know, `min_bytes` 0 without `PNG` or `DISCARD`, `EXACT` without `INFLATE`, `DISCARD` with any other flag, `DISCARD` with medium `'f'` or `'d'`, `DISCARD` with a non-zero `offset`, `read_max` or `min_bytes`, or `min_bytes` past `max_bytes`, makes it exit, which kmscon reads as a failure. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the three `DISCARD` conditions are added.) kmscon never sends the last: a request whose `min_bytes` passes `max_bytes` it answers `TSM_VTE_IMAGE_OVER_BUDGET` (ENOSPC to the client) without sending it, so the helper exits only on a request kmscon would not build. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": the helper read `size` as "the exact count", needed exactly `width` x `height` x 3 or 4 bytes for raw formats and expanded `f=24` to RGBA itself, and answered EBADF, EINVAL or ENOSPC.)
- **The reply,** helper to kmscon, 32 bytes:

```c
struct kmscon_image_reply {
	uint32_t version;	/* 2 */
	uint32_t status;	/* KMSCON_IMAGE_OK 0 to KMSCON_IMAGE_OVER_BUDGET 5 */
	uint64_t id;		/* the request's */
	uint32_t width;		/* KMSCON_IMAGE_OK with PNG; else 0 */
	uint32_t height;
	uint32_t reason_len;	/* 0 to 255 */
	uint32_t ignored;	/* bytes past what was used; 0 for an error or discard */
	/* reason_len bytes follow: why it failed, for kmscon's log */
};
```

- The reply is 32 bytes with no padding: `version` at 0, `status` 4, `id` 8, `width` 16, `height` 20, `reason_len` 24, `ignored` 28. (Changed 2026-10-24, from libtsm #51's build, which found that under version 2 libtsm never sees the bytes the helper skips, and the helper, its stderr on `/dev/null`, has no log of its own: this field was `reserved`, 0.) (Changed 2026-10-24, Notes "S and data length; a mechanical helper": version 1 carried `int32_t status`, "0, -EBADF, -EINVAL or -ENOSPC".)
- With `KMSCON_IMAGE_OK`, except for a discard, the packet carries a memfd: for a PNG request, exactly width x height x 4 bytes of RGBA in the layout `tsm_vte_image_reply` takes; otherwise exactly `min_bytes` bytes, with width and height 0. Either is made with `MFD_ALLOW_SEALING` and sealed with `F_SEAL_SHRINK`, `F_SEAL_GROW` and `F_SEAL_WRITE` after the helper unmaps its writable mapping. An error reply carries no descriptor. kmscon logs the reason; the client gets only the one reply for its code. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this said "With status 0 the packet carries a memfd of exactly width x height x 4 bytes of RGBA in the layout `tsm_vte_image_reply` takes", for raw formats too, and made no exception for a discard.)
- **kmscon's checks,** before it reads a pixel; any failure is a failed reply: a request outstanding at all (a packet with none is a failure); the received length exactly 32 plus `reason_len`, with `reason_len` at most 255, so root never reads past the packet; `ignored` 0 on an error or discard reply, and otherwise at most `read_max` when `read_max` is not 0; the version; the outstanding `id`; a status among the six; for a discard's `KMSCON_IMAGE_OK`, no descriptor; for any other `KMSCON_IMAGE_OK`, exactly one descriptor, and `fstat` showing a regular file; then, in this order, `F_GET_SEALS` holding at least `F_SEAL_SHRINK`, `F_SEAL_GROW` and `F_SEAL_WRITE`, seals only a memfd made with `MFD_ALLOW_SEALING` can carry, with any further seal accepted beside them (`F_SEAL_FUTURE_WRITE` does not count in place of `F_SEAL_WRITE`, since a writable shared mapping made before it was set can still write; beside `F_SEAL_WRITE` it is accepted and adds nothing), and only once the seals are known a second `fstat` showing exactly width x height x 4 bytes for a PNG request, the product checked for overflow (`__builtin_mul_overflow`), or exactly `min_bytes` with width and height 0 otherwise, `min_bytes` itself at most `max_bytes`. Until `F_SEAL_SHRINK` is set the helper can truncate the memfd after any `fstat`, and a mapping past the new end faults the root process, so the size counts only when read after the seals (#162, a15d49a). A helper writing its memfd with `write()` is not bounded by its `RLIMIT_AS`, which is why `min_bytes` is checked against `max_bytes` here. For a PNG request width and height run from 1 to `max_width` and `max_height`, and width x height x 4 is at most `max_bytes`. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the seals-then-size order, the overflow check and the `min_bytes` bound are added; this said "`fstat` showing a regular file of exactly width x height x 4 bytes computed in 64 bits" before the seals.) kmscon then passes the outcome on unchanged as libtsm's status, and every failure of its own as `TSM_VTE_IMAGE_FAILED`. (Changed 2026-10-24, Notes "S and data length; a mechanical helper": this checked "a status among the four" and, "for a raw format with `s` and `v` given, width and height equal to them"; libtsm now checks the bytes against its own request.) It then maps the memfd `PROT_READ` and `MAP_PRIVATE`, passes it to `tsm_vte_image_reply`, and unmaps and closes it. Every descriptor received, across every `SCM_RIGHTS` control message, is closed on every failure path. The reason is reduced to printable ASCII before kmscon logs it. On an OK reply with `ignored` above 0, kmscon logs one line at debug level, shown under `kmscon --debug` in a normal build: the request's `id`, the medium and the count, never the name; this is the host path's half of the log the operator asked for ("if there is a verbose debug path somehow, we could emit that it was discarded"), libtsm's being inline data (Order of work, 4a). (Changed 2026-10-24, from libtsm #51's build, which found that under version 2 libtsm never sees the bytes the helper skips, and the helper, its stderr on `/dev/null`, has no log of its own: kmscon's checks required "`reserved` 0", and kmscon logged nothing of ignored bytes.)
- kmscon trusts the helper no more than the client: the helper runs as the user, who can trace it. The shrink seal keeps it from truncating the object under kmscon's read, which would fault the root process. For the same reason, the helper copies a client's `t=s` object into its own sealed memfd instead of passing the client's descriptor on.

### Memory

- The two copies kmscon holds while a request is out, the inline memfd it sends and the reply it maps, sit outside the budget, as bbulk's working memory does: at most one request per session, each copy at most `max_bytes`, and both gone when the request ends. The inline buffer in libtsm is reserved (above), and so is the stored image.

### The `sd_login` build option

```meson
option('sd_login', type: 'feature', value: 'auto',
  description: 'Find the image helper\'s user through sd-login (libsystemd or libelogind)')
```

- `auto` and `enabled` look for `libsystemd`, then `libelogind`; both provide `<systemd/sd-login.h>`, libelogind's pkg-config file adding its include directory. `enabled` with neither fails the configure, `auto` builds without, and `disabled` looks for neither. The result is `BUILD_ENABLE_SD_LOGIN` in `config.h` and a line in the build summary, as the other features have.
- Only kmscon links it, for `sd_get_seats` (whether logind is running, failing closed), `sd_pid_get_session` on the pty's child and `sd_session_get_uid`; the helper never does. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: `sd_get_seats` is added; this said "for `sd_pid_get_session` on the pty's child and `sd_session_get_uid`".) Built without it, or with no logind running, the identity is the pty's owner, except that a root owner reads no file and decodes as `kmscon-image` or `nobody`; with logind running and the child in no logind session, file media are refused (The image helper, Identity). (Changed 2026-10-24 with the refusal before login and the operator's "Never read as root by fallback": this said "Built without it, or with the child in no logind session, the identity is the pty's owner, as this record says.")
- The name follows `meson.options`, where every option is snake case (`video_drm2d`, `font_freetype`).

### tsm-replay's fixture host

- Against a libtsm that declares `TSM_VTE_IMAGE_HOST`, tsm-replay installs a hook that records the request and returns 0. Its input loop then answers with `tsm_vte_image_reply` and feeds the rest, the path kmscon takes, pause and resume included.
- Answers come from `cases/<case>.files/` beside the stream. A request names a fixture: the last component of its path or name for `t=f`, `t=t` and `t=s`, and `direct-<n>` for the case's nth inline request, counting from 1. tsm-replay answers, taking the first that applies:
  1. from `<fixture>.error`, whose first word, an outcome name (`UNREADABLE`, `TOO_SHORT`, `CORRUPT`, `TOO_LARGE` or `OVER_BUDGET`), is the outcome;
  2. for a `PNG` request, from `<fixture>.rgba`, a decoded image (width and height as little-endian 32-bit integers, then RGBA), since tsm-replay links no decoder;
  3. for an `INFLATE` request without `PNG`, from `<fixture>.bytes`, the inflated bytes, applying `min_bytes` as the helper does;
  4. for a request with neither, from `<fixture>` itself, applying `offset`, `read_max` and `min_bytes` as the helper does;
  5. otherwise `UNREADABLE` for the file media and `CORRUPT` for inline data.
  (Changed 2026-10-24, Notes "S and data length; a mechanical helper": the fixture answered in kitty codes (`EBADF`, `EINVAL`, `ENOSPC`), honoured `O` and `S` as exact and expanded `f=24`.)
- It deletes nothing and applies none of the helper's path rules, which kmscon's unit tests cover.
- Each request is printed among the replies, as `host: <id> t=<medium> flags=<inflate,png,exact,discard or -> O=<offset> max=<read_max> min=<min_bytes> name=<the name as received> len=<data_len>` (changed 2026-10-24 with the request's fields; this printed `f=`, `o=`, `discard=`, `s=`, `v=` and `S=`), the name with every byte outside 0x21 to 0x7e, and the backslash, written as `\xNN` in lower-case hex (a space as `\x20`, a newline as `\x0a`), so each request stays one line of space-separated fields and the goldens judge what libtsm hands out as well as what it answers the client. `gen-synthetic.py` writes the fixtures beside the streams.

### Testing

- **Tier 1 and libtsm's unit tests:** each medium reaching the hook and inline raw data staying in libtsm; an `a=q` with `I` and no `i`, on `t=d`, `t=t` and `t=s` under `q` absent, 1 and 2, with no reply and no request (added 2026-10-24); a name too long or holding NUL; a `t=s` command with `m=1` followed by more commands, each taken whole, from mpv's recorded stream; icat's detection sequence (three `a=q` and DA1), with the three OK replies before the DA1 answer; an `a=f` with `t=s` from icat's GIF stream, refused with the object discarded; the pause, with bytes after the command's ST parsed only after the reply and in order, a put right after a file transmission among them; each error from the hook's return and from the reply, and a discard whose hook call returns a negative value answered with libtsm's own refusal alone; ENOSPC from the inline buffer's reservation and from the image's; `a=q` storing nothing; a reply past `max-graphic-size`, or of the wrong length; the request each command row of What libtsm asks for produces, and the reply each outcome of What libtsm answers produces; kitty's length rules from ADR-107's probe (`S` past the end of a file read to the end, `S` 0, an offset past the end, too few bytes from a file and inline, excess ignored inline and from a file); and every case again with no hook. The unit tests also cover a stale `id`, -EBUSY from inside the hook, and each way of cancelling.
- **kmscon's unit tests,** beside this record's refusal tests: each reply check (no descriptor, two descriptors, a memfd unsealed or missing one seal or sealed with `F_SEAL_FUTURE_WRITE` in place of `F_SEAL_WRITE`, a size one byte off, dimensions past the limits, a wrong `id` or version, a truncated packet, a length that disagrees with `reason_len`, an `ignored` past `read_max` or non-zero on an error or discard reply, a reply with no request out) ends the request with EBADF and a new helper, with every received descriptor closed; an OK reply with `ignored` above 0 logged at debug level with its count and no name (changed 2026-10-24 with the reply's `ignored`: the list said "a non-zero `reserved`"); a `t=t` directory component swapped for a symlink after the check, leaving the file elsewhere in place; a discard aimed at a FIFO, or at a symlink whose target fails the temporary-file rule, refused with nothing deleted; a discard aimed at a symlink whose target passes it, deleting the target and leaving the symlink; the timeout, with a helper that never answers; PNG and zlib decoding, a PNG whose header claims a size past the limits refused before allocation among them; each outcome from each step of What the helper does, `read_max` 0 and non-zero, an offset at and past the end, `min_bytes` met, missed and exceeded, and `EXACT` met, short and long; a `t=t` file deleted after a read that fails; before login, file media answered EBADF with no helper started, and an inline PNG decoded by a helper running as `kmscon-image`, as `nobody` when `kmscon-image` is absent or its lookup fails or it resolves to uid 0 or gid 0, and refused when neither serves, with no supplementary group and the lookup made once; a discard answered and the helper still running; a request with `min_bytes` past `max_bytes` answered ENOSPC with nothing sent; a raw reply whose memfd is sealed after its size is read, refused; a request the helper must exit on (an unknown flag bit, a non-zero `reserved`, `min_bytes` 0 without `PNG` or `DISCARD`, `EXACT` without `INFLATE`, `DISCARD` with another flag, with medium `'f'` or `'d'`, or with a non-zero bound); `INFLATE` and `EXACT` without `PNG`, met, short and one byte long; an offset at the end of a `t=t` file, answered `UNREADABLE` with the file deleted; the memory limit; and the identity, logind's uid when built with sd-login and the pty's owner otherwise; `sd_get_seats` returning 0, more than 0 and an error, the error refusing file media as before login; `sd_pid_get_session` and `sd_session_get_uid` failing with an error other than no session, with logind running, handled as before login; a root pty owner with no logind, both at the login prompt and after a root login, refusing file media without starting the helper and decoding inline PNG as `kmscon-image`, then `nobody`, then refusing; and a logind-authenticated root keeping the helper as root.

### Order of work

This details the step 4 list's 4a and 4b, which are edited to match, with a dated note; 4c, 4d and 4e stand.

1. **4a, libtsm kitty and the host hook,** in parallel with 4b once this section lands. Under the Notes "S and data length; a mechanical helper": inline raw `f=24` and `f=32` data past `s` x `v` x bpp is ignored, no longer EINVAL (libtsm's parser, 4a's aaronsb/libtsm#50, not the helper); `a=q` applies the same length rules as a transmission, accepting excess, requiring at least the minimum and storing nothing; and when libtsm ignores excess data, inline (raw `t=d` data, a transmission or `a=q`), it logs one line with `llog_dprintf` at `LLOG_DEBUG`, through the `tsm_log_t` callback the host installs, naming the image (its `i`, its `I`, or anonymous), the medium and how many bytes it discarded, for example `kitty image i=1, t=d: 6 bytes past s x v ignored`. `llog_debug` compiles to nothing outside debug builds, so `llog_dprintf` is used, and the line shows under `kmscon --debug` in a normal build; at debug level only, video at 30 frames a second does not fill a normal log; the operator asked for it: "if there is a verbose debug path somehow, we could emit that it was discarded". On the host path the helper skips the excess, so libtsm never sees it and logs nothing; the helper counts it in the reply's `ignored` and kmscon logs it (The helper protocol). (Changed 2026-10-24, from libtsm #51's build, which found that under version 2 libtsm never sees the bytes the helper skips, and the helper, its stderr on `/dev/null`, has no log of its own: this said "inline or from the helper's read", from version 1, whose reply handed libtsm every byte read.) Before that: what 4a listed, plus the calls above, the pause, the fixture host in `tsm-replay`, and their tier 1 cases and unit tests. libtsm builds each request from the kitty keys (What libtsm asks for), maps each outcome to the kitty reply (What libtsm answers), converts raw bytes the host returns as it converts inline data, and applies kitty's length rules to inline raw data (at least `s` x `v` x bpp, the excess ignored, fewer EINVAL). It also adds victim rank `TSM_IMAGE_PLACED_KITTY` (3), declared with the feature macro `TSM_SCREEN_IMAGE_PLACED_KITTY`:
   - kitty images placed outside scrollback stop being pinned (`images_mark_pinned` in `tsm-image.c`); sixel stays pinned;
   - each image keeps the time it was transmitted, a stamp from the shared `image_clock` taken when it is stored; a retransmission of an id is a new image and takes a new stamp;
   - for a rank 3 image, `tsm_screen_image_victim` and `tsm_screen_image_victims` report that stamp in `last_use`, and `image_victim` orders rank 3 by it, so a screen's own order and its host's order across screens agree; ranks 0 to 2 keep the last time the image was created, placed or drawn; every value stays unique within its rank, so `victim_cmp` needs no change;
   - it rewrites the never-evict statements, in `libtsm.h` above `tsm_screen_image_reserve_cb`, at the top of `tsm-image.c` and above `images_mark_pinned`, and the comments on `enum tsm_image_rank`, `last_use`, `image_victim` and `victim_cmp`, to say this.
2. **4b, the helper,** in kmscon: the refusal of file media before login and the pre-login decoding user (The image helper, Identity), with a sysusers.d file, `kmscon-image.conf`, holding `u kmscon-image - "kmscon image helper before login" - -` (a system user with home `/` and no login shell; on a system without `systemd-sysusers` nothing creates it, and `nobody` serves), installed into systemd's `sysusersdir` (read from the `systemd` pkg-config file, as `systemdsystemunitdir` is, defaulting to `lib/sysusers.d`); then `kmscon-image-helper` with this protocol, version 2, and this record's identity, file and shared memory rules, PNG and zlib, reading within the request's bounds and returning outcomes only, with the bytes it skipped counted in `ignored` and logged by kmscon at debug level, with no kitty key in its code; declaring `libpng` and `zlib` as its own meson dependencies; `kmscon_pty_pause` and `kmscon_pty_resume`; the hook's wiring in the terminal; the `sd_login` option; and the unit tests. In `src/render/image_budget.c`, `image_budget_picture_room` stops at rank 3 (`give_victim` ends the walk at the first rank 3 victim), inside `#ifdef TSM_SCREEN_IMAGE_PLACED_KITTY`. `next_victim` and `make_room` need no code change: they order by rank, then `last_use`, which now means oldest transmission at rank 3. Their comments, and `image_budget.h`'s on the order and on never evicting, are rewritten. Until the pin carries 4a, it builds against the 4a branch with `LIBTSM=`, and its unit tests drive the helper over its socket without libtsm.
3. 4c follows both, after the pin moves. 4d also takes the showcase's acts 6 and 7 (#85, #86, #87).

### Clients that depart from the spec

What kmscon does where a surveyed client departs from the spec, and the clients each affects (ADR-105, ADR-106):

- **Shared memory names without a leading `/`.** The spec says POSIX names "must start with a / and have no other / characters", and the Shared memory bullet of The image helper already refuses any other name, with the one EBADF reply. That stands. mpv 0.41.0's shared memory mode names its object `mpv-kitty-0x<hex>`, so under kmscon it shows nothing, as under kitty 0.49.2. Two alternatives were set out and are rejected under the operator's principle, since each accepts a name the spec forbids: reading the name as `/<name>` and unlinking (mpv then loses frames to its own reuse of the name: 29% to 49% in kitty with the name rewritten, 50% in the probe's harness), and reading it as `/<name>` but leaving those objects unlinked.
- **Unlinking.** The spec's unlink after reading stands for every `t=s` object. mpv reuses one name for every frame, so even with a spec name its frames would race that unlink; that is mpv's to fix. kitten icat, the only other surveyed client using `t=s`, names its objects by the spec; the spec leaves their removal to the terminal, and icat leaves its frame objects for the terminal to remove (ADR-106). Its query objects were gone at the same later check of `/dev/shm`; the harness records which objects appeared, not who removed them.
- **mpv's `m=1` on whole `t=s` commands** is harmless under the `m` rule above.
- **icat's `a=a` commands** carry `I` and no `q`, so until step 5 icat receives an EINVAL reply for each, as the spec's reply rules require.
- **Anonymous frames piled at one cell** (mpv, chafa, viu) send nothing the spec forbids; past the budget, the spec's storage quota rule evicts their oldest frames (Operator answers to the open questions, below). mpv enters the alternate screen (mode 1049) before its first frame, so its pile sits in the alternate store and is emptied when mpv leaves it; chafa's and viu's piles sit in the main store and stay until evicted or deleted.

### Spec version annotations

- The spec marks some features with the kitty version that added them: placement ids, `q` and image numbers (`I`) in 0.19.3; `C=1` and animation in 0.20.0; frame composition in 0.22.0; Unicode placeholders in 0.28.0; relative placements in 0.31.0; `d=r` in 0.33.0. The shared memory name rule, the unlink, chunking and the storage quota carry no mark.
- The surveyed clients use marked features: mpv sends `C=1` and `q`; chafa and timg send `q`; ncplayer sends `p`, `q` and `C=1`; icat sends `I`, `q` and animation. None of the observed departures (mpv's name, its reuse of it, `m=1` on whole `t=s` commands) involves a marked feature, so the marks cannot say when the rules those departures break entered the spec. Whether any client was written against an older form of the spec is an unverified hypothesis.

### Acceptance: conformance and compatibility

- **Conformance** is judged with spec-conformant clients: the fork's spec-written client (ADR-300); kitten icat for what it does within the spec (detection, stills through `t=f`, `t=t`, `t=s` and direct transmission, deletes); and mpv's inline output, which follows the spec and which the operator chose to keep as an A5 gate. These are 4d's pass and fail gates.
- **A5 changes.** Its conformance runs are `mpv --vo=kitty` inline, the spec-written client inline and over shared memory, and `kitten icat --transfer-mode=memory`; `mpv --vo=kitty --vo-kitty-use-shm=yes` moves to the compatibility measurements. The operator chose to keep mpv inline as a gate (considered entry above, an option they picked). The Targets section's A5 criteria are read against each conformance client's rate; at 7680x2160 mpv inline is bound by the pty at about 4 frames per second (ADR-105), so for it the gate is that kmscon presents what mpv sends. The frontmatter's observable, the Summary's Acceptance bullet, the Acceptance scenarios' A5, both Targets criteria and 4d are edited to match, each with a dated note. A6 and A7 place their pictures with the spec-written client.
- **Compatibility measurements,** reported beside conformance and gating nothing: mpv over shared memory, expected to show nothing; timg (PNG frames) and ncplayer (zlib frames) on the 30 fps clip at 1920x1080 and 7680x2160; chafa and viu on a GIF. Each records whether the client displays and its presented rate beside its own. timg and ncplayer put the helper on every frame, so their rates measure the helper's round trip at video rate, which no conformance run does. This replaces A8.
- **icat on a GIF** shows its first frame only until step 5: its first frame is an `a=T`, and the rest are `a=a` and `a=f`, answered EINVAL, with each `a=f` object discarded. Its GIFs join step 5's scenarios.

### Operator answers to the open questions

The operator chose the recommended option of each, recorded as considered entries above:

1. **The copies a load holds stay outside the budget,** as bbulk's working memory does (Memory, above). The budget bounds what is kept; a load briefly needs up to two more copies, each at most the budget, one request per session.
2. **The build option is `sd_login`,** snake case as every option in `meson.options` is.
3. **Images past the budget follow the spec's storage quota rule.** The spec says that when a new image passes the quota, "the terminal emulator should delete older images to make space for the new one". kmscon evicts, across every screen it runs: its scaled pictures the current frame does not draw, then images with no placement, then images placed only in scrollback, then kitty images placed on a screen's own lines, oldest first by transmission, with their placements; libtsm reports the transmission time in each such victim's `last_use`, so kmscon's order across screens holds without a change (Order of work, above). A sixel placed on a screen's own lines is never evicted. The spec asks only for older images to be deleted, unplaced ones first; reading "older" as least recently used off the screen and as oldest transmission on it, putting scrollback ahead of the screen, and exempting sixel are this record's choices, with the reasons the Image store section gives. ENOSPC remains for an image larger than everything that can be freed. Passages of this record are edited in place to say this, each with a dated note quoting what it said before and naming the operator's choice: the Summary's store and budget bullet, whose "An image placed on the screen is never evicted" now holds for sixel only; the Image store section's eviction list, which gains step 4, kitty images with a placement on a screen's own lines, and its sentence "An image with a placement on a screen's own lines is never evicted.", now for sixel images only; and Consequences' shared-budget line. ADR-101's Image layer bullet on eviction is edited the same way, and this record's `amends` names that section beside Parsing. The operator set the editing rule: "I don't like append only. I thought it made sense but we need to be able to edit where it makes sense too" (docs/fork/development.md). Covered-placement removal (dropping a kitty placement wholly covered by a newer opaque one) is not adopted: it is outside the spec, and it can be proposed later only on 4d's evidence that video in practice pushes scrollback pictures out. The clients this serves (ADR-105, ADR-106): mpv's video and chafa's and viu's GIFs, which place every frame as a new anonymous image at one cell, keep playing past the budget instead of freezing. bbulk's scaled copies keep the rule of the 2026-10-23 Notes (Q1): the quota rule applies to a new image's reservation in libtsm's stores. 4a builds the order with tier 1 cases: unplaced, then scrollback, then placed kitty images oldest first, and a placed sixel never.

## Notes (2026-10-24, what one pipeline costs)

The operator asked whether one pipeline with a handler per protocol limits performance. The shared store, composition, damage and budget mean every optimisation serves both protocols; the costs below come from choices made with the pipeline, and 4d measures them. The operator chose to record them here.

- **One store format.** Every image is premultiplied ARGB32, 4 bytes per pixel, which keeps composition to one blend path. A sixel has at most `num-color-registers` colours and could be held as palette indices: one byte per pixel with 256 registers or fewer, two with the default 1024. Sixel pictures therefore take two to four times the budget they need.
- **Two passes on the shared memory path.** The helper copies a client's object into its own sealed memfd (The helper protocol: kmscon never touches the client's object), and libtsm converts the reply to premultiplied ARGB in its store. At 3840x2160, the picture mpv sends on a 7680x2160 screen, that is two passes over about 25 to 33 MB per frame. Converting in the helper, so that kmscon maps a picture already in the store's format, would remove one.
- **One thread.** Parsing, decoding, conversion and composition run on kmscon's event loop. Bounded input keeps text and VT switching responsive, but one frame's work cannot use a second core. Fed pre-encoded sixel pictures of 3712x2088 on a 7680x2160 screen as fast as it reads them, kmscon accepted 54 to 77 a second in a tier 3 VM (measured on 2026-10-24, aaronsb/kmscon#158); that rate is its ceiling for one thread.
- **A new image per frame.** mpv, chafa and viu send each frame as a new image, so the store allocates and frees a buffer the size of each picture: 33 MB for the 3840x2160 picture mpv sends on a 7680x2160 screen, a few hundred KB for chafa's and viu's GIF frames. Reusing the buffer is not yet filed; libtsm#45, which replaces an image in place when a new one lands exactly over the previous placement, is the nearest.

Estimate for kitty over shared memory at 7680x2160, on kmscon's thread per frame, unmeasured: 8 to 15 ms of conversion and 6 to 12 ms of composition (ADR-104: S4, a full-screen picture replaced every frame, 11.9 ms median), about 14 to 27 ms against 33 ms at 30 frames per second, with the helper's copy in its own process. The cheapest gains, in order: a SIMD conversion pass, conversion in the helper, and buffer reuse.

## Notes (2026-10-24, S and data length; a mechanical helper)

Two decisions of the operator's, recorded as considered entries above, and edited in place through this record with dated notes naming this section.

- **kitty's rule for `S` and data length.** The spec does not say how much a terminal reads or what "smaller than the client claimed" means. ADR-300 settles such points by kitty run as a black box, and kitty 0.49.2 (ADR-107):
  - reads from `O` to the lesser of `O` + `S` and the end, `S` being a maximum, and to the end when `S` is 0;
  - needs at least `s` x `v` x bytes per pixel, and ignores the excess;
  - answers EBADF for an offset past the end and for too few bytes from a file or shared memory, and ENODATA for too few bytes inline. ENODATA is not among this record's codes, so too few inline or inflated bytes answer EINVAL;
  - deletes a `t=t` file and unlinks a `t=s` object even when the read fails.
- The direct PNG-with-compression rule stays exact, as the spec words it: "If you use both PNG and compression, then you must provide the S key with the size of the PNG data". That holds for a non-zero `S`; a command without `S`, or with `S=0`, is accepted and decoded without the length check, this record's choice (What libtsm asks for). (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: the second sentence is added.)
- Ghostty, WezTerm and Rio, the permissive implementations, treat `S` as exact on POSIX; WezTerm's Windows shared memory path treats it as a maximum (ADR-107). (Corrected 2026-10-24, from the skeptic's review of aaronsb/kmscon#164: this said they "treat `S` as exact (ADR-107)".) This record chooses kitty's rule knowingly: it accepts every stream the exact rule accepts and more, and kitten icat's default mode can then send a local image by file reference rather than inline base64 (ADR-107: with its detection answered, 388 and 390 bytes on the pty for a 75-byte and a 692 KB PNG; refused, 429 bytes and 923851 bytes inline), which is the compatibility and the performance the operator asked about ("1, probably but which would lead on performance and compatibilty?"; then the option "Yes, both (Recommended)"). This replaces the Files section's "`S` the exact number of bytes to read".
- **`a=q` follows the same rules,** accepting excess, requiring at least the minimum, and storing nothing; libtsm logs any excess it ignores at `LLOG_DEBUG` (Order of work, 4a).
- **The kitty spec stays in libtsm** ("for our implementation, are keeping seperation from kmscon core: meaning, when/if kitty spec changes, is it something that won't affect kmscon's consumption of the data"). libtsm turns kitty's keys into a request in plain terms (what to open, where to start, the fewest bytes the image needs, the most to read, whether to inflate or decode PNG) and turns the helper's outcome back into kitty's reply code. The helper applies only kmscon's own rules (paths, symlinks, the device and inode checks, the temporary-file and shared memory rules, deletion, the limits), and kmscon's root-side checks on its replies stay. A future change to the spec's rules on `S`, lengths or reply codes then changes libtsm alone.
- **File media are refused before login** (The image helper, Identity): with logind running and the terminal's child in no logind session, `t=f`, `t=t` and `t=s` answer EBADF without starting the helper, and inline PNG and zlib decode in a helper running as `kmscon-image`, or as `nobody` when that user is absent, and are refused when neither exists (the operator's "Dedicated user, nobody fallback"). 4b implements it in aaronsb/kmscon#162.
- **Never root by fallback:** wherever the identity comes from the pty's owner and is uid 0, file media are refused and inline PNG and zlib decode as `kmscon-image` or `nobody`, as before login; only a uid 0 logind authenticated runs the helper as root (The image helper, Identity). 4b implements it with the refusal before login.
- **What changes for 4a and 4b:** the request struct and the result call in libtsm: the protocol at version 2, the request losing `format`, `width`, `height` and `size` for `read_max` and `min_bytes`, and the flags gaining `PNG` and `EXACT`; the reply's status becoming an outcome; the helper's steps; and the fixture host. libtsm now converts `f=24` and `f=32` bytes from files and shared memory, as it converts inline data, so the helper holds no knowledge of kitty's pixel formats.

