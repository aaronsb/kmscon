---
contract: adr/v1
kind: evidence
capability: images
status: accepted
date: 2026-10-24
deciders:
  - aaronsb
related: []
---

# ADR-107: S and data length in kitty 0.49.2, black-box, and in Ghostty, WezTerm and Rio

## Scope and method

How much a terminal reads for a kitty `t=f`, `t=t` or `t=s` transmission, what it does with too few or too many bytes, and whether it removes a temporary file or shared memory object whose read fails. The spec leaves these open: it says `S` is "the size of data to read from a file" and that a file "smaller than the client claimed" must get the one failure reply, without saying whether `S` is exact or a maximum. Recorded on 2026-10-24 for ADR-103.

- **kitty 0.49.2,** run as a black box (ADR-300): a probe sent `a=q` and `a=t` commands with `f=24`, `s=1`, `v=1` (3 bytes needed) over each medium with chosen `O` and `S`, read the replies, and checked afterwards whether each file or object was left. The table below is what the first two rounds' records hold.
- **Where the evidence lives.** The probe records are not in the repository and are not committed. They are kept in `session-artifacts-2026-10-24/build-4b/kprobe/`, a directory beside the repository's checkout on the machine that ran them: round 1 (`probe_in_kitty.py` and its replies, `kitty-replies.json`), round 2 (`probe2_in_kitty.py` and `kitty-replies-2.json`, the offset, short-read and `f=32` cases), icat's detection sequence (`icat_fake.py`, `icat.out`, `icat.strace`), and `round3/`, `round4/` and `round5/`, each with a README naming its scripts, its outputs and how it was run. (Changed 2026-10-24, from the code review of aaronsb/kmscon#164: this said "The probe's records were kept in the session's scratch space and are not in the repository; the table below is what they hold.")
- **Ghostty 8ef0ec1, WezTerm e2a98e1 and Rio 59203cc,** all MIT, read for their handling of `S` and `O`. The citations are to those commits.

## kitty 0.49.2

| Case | Reply | File or object afterwards |
|---|---|---|
| `t=t`, 3 bytes, `S=47` | OK | removed |
| `t=s`, 3 bytes, `S=19` | OK | removed |
| `t=t` and `t=s`, 3 bytes, `S=3` or no `S` | OK | removed |
| `t=t`, 47 bytes, `S=47` or no `S`; `t=s`, 19 bytes, `S=19` or no `S` | OK | removed |
| `t=f`, 3 bytes, `S=47`; `t=f`, 47 bytes, no `S` | OK | kept |
| `t=f`, 2 bytes, no `S` | `EBADF:Failed to read image file` | kept |
| `t=f`, 3 bytes, `O=10`, with and without `S=3` | `EBADF:Failed to read image file` | kept |
| `t=t`, 2 bytes; `t=s`, 2 bytes | `EBADF:Failed to read image file` | removed |
| `t=t`, 3 bytes, `O=10`; `t=s`, 3 bytes, `O=10` | `EBADF:Failed to read image file` | removed |
| `t=f`, `f=32`, 8 bytes for 1x1 | OK | kept |
| direct, 3 bytes, with no `S`, `S=3` or `S=47`; direct, 6 bytes | OK | |
| direct, 2 bytes | `ENODATA:Insufficient image data: 2 < 3` | |
| `a=t`, `t=t`, 3 or 47 bytes, `S=47` | OK | removed |

- kitty reads from `O` to the lesser of `O` + `S` and the end, so `S` bounds the read; with no `S` it reads to the end. The first two rounds showed only `S` past the data; the third, below, settles `S` short of it. (Corrected 2026-10-24: this said "The first probe showed only `S` past the data; a second, below, settles `S` short of it.")
- It needs at least the `s` x `v` x bytes per pixel the keys declare and ignores the excess, from a file and inline.
- An offset past the end, and too few bytes from a file or object, answer EBADF; too few inline answer ENODATA. No round probed an offset exactly at the end.
- It removes a `t=t` file and unlinks a `t=s` object even when the read fails, and leaves a `t=f` file.

## Ghostty, WezTerm and Rio

Read at the commits above for each point the spec leaves open. Citations are to files under `src/terminal/kitty/` in Ghostty (`graphics_image.zig`, `graphics_exec.zig`), `term/src/terminalstate/kitty.rs` and `wezterm-escape-parser/src/apc.rs` in WezTerm, and `rio-vt/src/ansi/kitty_graphics_protocol.rs` in Rio.

| Point | kitty 0.49.2 | Ghostty | WezTerm | Rio |
|---|---|---|---|---|
| `S` | a maximum | exact: "S is the exact number of bytes to read, not a maximum" (`graphics_image.zig:450`), read with `readAlloc(size)` (`:457`) | exact on POSIX: `read_exact` for files (`apc.rs:188-191`) and shared memory (`apc.rs:291-293`); a maximum for shared memory on Windows, `size = size.min(val as usize)` (`apc.rs:423-425`) | exact: `read_exact` (`:1100-1101`); for shared memory, an `S` past the object's size is an error (`:1169-1182`) |
| `S` past the end | OK | EINVAL (`graphics_image.zig:457-461` for files, `:330` for shared memory; the reply text from `graphics_exec.zig:1104`) | an error carrying the I/O message (`kitty.rs:191-197`) | EINVAL (`:1101`, `:1169-1182`) |
| data longer than `s` x `v` x bpp | accepted, the excess ignored, on every medium | EINVAL (`graphics_image.zig:598-603`); only animation frames are cut to length (`:587-597`) | accepted for `f=24` (`kitty.rs:790-797`), refused for `f=32` (`kitty.rs:800-806`) | cut to length (`:1462-1467`) |
| data shorter than `s` x `v` x bpp | EBADF from files and shared memory, ENODATA inline | EINVAL | an error | EINVAL (`:1453-1460`) |
| `O` past the end | EBADF | EINVAL (shared memory `graphics_image.zig:320`; files seek at `:439`, then read short) | an error | EINVAL |
| `a=q` | loads | loads and decodes in full (`graphics_exec.zig:156-195`) | reads only (`kitty.rs:181-198`) | loads in full (`:724-756`) |
| `t=t` removed after a failed read | yes | yes, once the temporary-file rule has passed (`graphics_image.zig:390`, `:422`) | no (`apc.rs:217-253`) | no (`:1116-1118`) |
| `t=s` unlinked after a failed read | yes | yes (`graphics_image.zig:248`) | no (`apc.rs:298-305`) | on some failures: after an `fstat` failure (`:1158-1161`), an `S` past the object's size (`:1175-1178`) and a size past its limit (`:1185-1188`); not after a failed open (`:1147-1154`), nor after a failed `mmap` (`:1201-1205`), which an offset that is not page-aligned, such as 10, causes |

- On POSIX all three treat `S` as exact; WezTerm's Windows shared memory path treats it as a maximum, as kitty does. None matches kitty on every point. (Corrected 2026-10-24, from the skeptic's review of aaronsb/kmscon#164: this said "All three treat `S` as exact; none matches kitty on every point.")
- Ghostty and Rio also accept a shared memory name without a leading `/`, which the spec forbids. WezTerm does not require `tty-graphics-protocol` in a temporary file's path, and adds `/var/tmp` to the temporary directories.
- kitten icat's detection queries hold 3 bytes with `S` set to the length of the file or object's name, which varies with the name (47 and 19 in this probe, 46 for `t=t` in ADR-106's GIF run), so under an exact `S` they fail. Its real transfers carry a correct `S`, and in memory mode it sends no queries.

## `q` in kitty 0.49.2 (added 2026-10-24)

A fourth round of black-box probes in kitty 0.49.2, at the request of the code review of aaronsb/kmscon#164, kept in `round4/` beside the other rounds (the script, the replies as JSON, kitty's stderr, a README), sent direct `f=24`, `s=1`, `v=1` commands, each followed by DA1, under `q` absent, `q=0`, `q=1` and `q=2`. It was run twice, with byte-identical replies.

A fifth round (`round5/`), at the request of the re-check of aaronsb/kmscon#164, sent `a=q` with `I=52` and no `i`, and with `i=51` as the control, on `t=t` (a file under `/tmp` named `tty-graphics-protocol-*`) and on `t=s` (an object in `/dev/shm`), each followed by DA1. With `I` alone kitty sent no reply, logged "Query graphics command without image id", and left both the file and the object in place; with `i` it answered `i=51;OK` and removed both. kitty refuses the command before it reads the medium.

| Command | No `q`, or `q=0` | `q=1` | `q=2` |
|---|---|---|---|
| `a=q` with `i`, 3 bytes (loads) | `OK` | no reply | no reply |
| `a=q` with `i`, 2 bytes (too few) | `ENODATA:Insufficient image data: 2 < 3` | the same | no reply |
| `a=q` with `i` and `I` | `EINVAL:Must not specify both image id and image number`, answered as `i=<i>,I=<I>` | the same | no reply |
| `a=q` with `I` and no `i` | no reply; kitty logs "Query graphics command without image id"; a `t=t` file or `t=s` object is left in place (round 5) | no reply | no reply |
| `a=t` with `i` and `I` | as `a=q` with `i` and `I` | the same | no reply |

kitty sends no reply at all under `q=2`, OK or error, and under `q=1` drops only OK, on `a=q` as on `a=t` (round 3, below, has the `a=t` pair that loads and that is too short). An `a=q` without `i` is answered by nothing, whatever `q` says. The spec's prose section "Suppressing responses from the terminal" says "Set it to 1 to suppress OK responses and to 2 to suppress failure responses"; the control-data table's row for `q` reads only "Suppress responses from the terminal to this graphics command."; and its Unicode placeholder section relies on `q=2` leaving "no responses from the terminal", which is what kitty does.

(Corrected 2026-10-24, from the skeptic's review of aaronsb/kmscon#164: this section said "A second black-box probe in kitty 0.49.2, at the request of the code review of aaronsb/kmscon#164, sent three `a=q` commands under `q` absent, `q=1` and `q=2`", but no kept record held such a probe, since round 3's `q` cases were `a=t`; round 4 was run to back the table. It also said "The spec's control-data table says "Set it to 1 to suppress OK responses and to 2 to suppress failure responses"", a sentence the prose section holds, not the table.)

## `S` short of the data, and icat's default mode (added 2026-10-24)

A third round of black-box probes, kept in `session-artifacts-2026-10-24/build-4b/kprobe/round3/` beside the repository (scripts, the replies as JSON, a screenshot, the pty captures, a README), settles two points the first probe left half evidenced.

**`S` below the data, in kitty 0.49.2.** Each medium (`t=f`, `t=t`, `t=s`) held 47 distinct bytes (byte i being (37 i + 11) mod 256), placed as `f=24` at 1x1 and scaled to 6x2 cells, the placed colour read back from a screenshot of the window:

| Case | Reply, on each medium | Colour placed |
|---|---|---|
| `S=3`, a 1x1 needing 3 | OK | bytes 0 to 2 (11, 48, 85) |
| `S=2`, a 1x1 needing 3 | `EBADF:Failed to read image file` | |
| `O=10,S=3` | OK | bytes 10 to 12 (125, 162, 199) |
| `O=10`, no `S` | OK | bytes 10 to 12 |
| `S=3`, a 2x1 needing 6 | `EBADF:Failed to read image file` | |

So kitty reads from `O` to the lesser of `O` + `S` and the end, takes the bytes the image needs from the start of that read, and answers EBADF when the read holds fewer, the same on all three media. The rule this record matches is now evidenced in full.

**kitten icat's default mode,** run on a pty whose stand-in terminal answered icat's three detection queries as told:

| Detection answers | 8x8 PNG of 75 bytes | 640x360 PNG of 692552 bytes |
|---|---|---|
| all OK | `a=T,q=2,f=100,t=f,...` naming the file: 388 bytes on the pty in all | the same by reference: 390 bytes |
| `t=t` and `t=s` refused | `a=T,q=2,f=100,...` sent inline: 429 bytes | inline: 923851 bytes on the pty, the image in 8 chunks after the 3 detection queries |

The large inline capture (`icat-default-big-refused.bin`, 923851 bytes by `wc -c`) holds 11 APCs: icat's 3 detection queries, then the image in 8 chunks of 923403 bytes of base64, which decode to the 692552-byte PNG. (Corrected 2026-10-24, from the skeptic's review of aaronsb/kmscon#164: the table said "inline: 923851 bytes in 11 chunks", counting the queries as chunks.)

A terminal that answers icat's detection, whose queries carry an `S` longer than their 3 bytes, gets local images by file reference; one that refuses them, as an exact-`S` terminal does, gets every image as inline base64, about 1.33 times its size on the pty.

**icat's chunk size.** icat's inline chunks carry 131072 bytes of base64, past the spec's "chunked up into chunks no larger than 4096 bytes". libtsm decodes chunks as they stream and sets no chunk-size limit, so it takes them; this is client evidence, not a rule.

**`q` on `a=t`.** The same round, with direct `a=t` commands, found `q=1` dropping only OK (the failure still answered `ENODATA:Insufficient image data: 2 < 3`) and `q=2` dropping both; round 4, in the `q` section above, found the same for `a=q`. (Changed 2026-10-24: this was headed "`q` confirmed" and said "The same round found ...", without naming the action.)

