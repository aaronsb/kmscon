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

- **kitty 0.49.2,** run as a black box (ADR-300): a probe sent `a=q` and `a=t` commands with `f=24`, `s=1`, `v=1` (3 bytes needed) over each medium with chosen `O` and `S`, read the replies, and checked afterwards whether each file or object was left. The probe's records were kept in the session's scratch space and are not in the repository; the table below is what they hold.
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

- kitty reads from `O` to the lesser of `O` + `S` and the end, so `S` is a maximum; with no `S` it reads to the end.
- It needs at least the `s` x `v` x bytes per pixel the keys declare and ignores the excess, from a file and inline.
- An offset past the end, and too few bytes from a file or object, answer EBADF; too few inline answer ENODATA.
- It removes a `t=t` file and unlinks a `t=s` object even when the read fails, and leaves a `t=f` file.

## Ghostty, WezTerm and Rio

Read at the commits above for each point the spec leaves open. Citations are to files under `src/terminal/kitty/` in Ghostty (`graphics_image.zig`, `graphics_exec.zig`), `term/src/terminalstate/kitty.rs` and `wezterm-escape-parser/src/apc.rs` in WezTerm, and `rio-vt/src/ansi/kitty_graphics_protocol.rs` in Rio.

| Point | kitty 0.49.2 | Ghostty | WezTerm | Rio |
|---|---|---|---|---|
| `S` | a maximum | exact: "S is the exact number of bytes to read, not a maximum" (`graphics_image.zig:450`), read with `readAlloc(size)` (`:457`) | exact: `read_exact` (`apc.rs:188-191`) | exact: `read_exact` (`:1100-1101`); for shared memory, an `S` past the object's size is an error (`:1169-1182`) |
| `S` past the end | OK | EINVAL (`graphics_image.zig:457-461` for files, `:330` for shared memory; the reply text from `graphics_exec.zig:1104`) | an error carrying the I/O message (`kitty.rs:191-197`) | EINVAL (`:1101`, `:1169-1182`) |
| data longer than `s` x `v` x bpp | accepted, the excess ignored, on every medium | EINVAL (`graphics_image.zig:598-603`); only animation frames are cut to length (`:587-597`) | accepted for `f=24` (`kitty.rs:790-797`), refused for `f=32` (`kitty.rs:800-806`) | cut to length (`:1462-1467`) |
| data shorter than `s` x `v` x bpp | EBADF from files and shared memory, ENODATA inline | EINVAL | an error | EINVAL (`:1453-1460`) |
| `O` past the end | EBADF | EINVAL (shared memory `graphics_image.zig:320`; files seek at `:439`, then read short) | an error | EINVAL |
| `a=q` | loads | loads and decodes in full (`graphics_exec.zig:156-195`) | reads only (`kitty.rs:181-198`) | loads in full (`:724-756`) |
| `t=t` removed after a failed read | yes | yes, once the temporary-file rule has passed (`graphics_image.zig:390`, `:422`) | no (`apc.rs:217-253`) | no (`:1116-1118`) |
| `t=s` unlinked after a failed read | yes | yes (`graphics_image.zig:248`) | no (`apc.rs:298-305`) | yes (`:1149-1170`) |

- All three treat `S` as exact; none matches kitty on every point.
- Ghostty and Rio also accept a shared memory name without a leading `/`, which the spec forbids. WezTerm does not require `tty-graphics-protocol` in a temporary file's path, and adds `/var/tmp` to the temporary directories.
- kitten icat's detection queries hold 3 bytes with `S` set to the length of the name (47 for `t=t`, 19 for `t=s`), so under an exact `S` they fail. Its real transfers carry a correct `S`, and in memory mode it sends no queries.

## `q` in kitty 0.49.2 (added 2026-10-24)

A second black-box probe in kitty 0.49.2, at the request of the code review of aaronsb/kmscon#164, sent three `a=q` commands under `q` absent, `q=1` and `q=2`, each followed by DA1: one that loads (`f=24`, `s=1`, `v=1`, 3 bytes), one with too few bytes (2), and one with both `i` and `I`.

| Command | No `q` | `q=1` | `q=2` |
|---|---|---|---|
| loads | `OK` | no reply | no reply |
| too few bytes | `ENODATA:Insufficient image data: 2 < 3` | `ENODATA:...` | no reply |
| `i` with `I` | `EINVAL:Must not specify both image id and image number` | `EINVAL:...` | no reply |

kitty sends no reply at all under `q=2`, OK or error, and under `q=1` drops only OK. The spec's control-data table says "Set it to 1 to suppress OK responses and to 2 to suppress failure responses"; its Unicode placeholder section relies on `q=2` leaving "no responses from the terminal", which is what kitty does.
