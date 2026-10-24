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

All three treat `S` as exact.

- **Ghostty** reads exactly `S` bytes from a file: "S is the exact number of bytes to read, not a maximum" (`src/terminal/kitty/graphics_image.zig:449-462`). For shared memory, an `S` past the bytes available from `O` is invalid data (`:318-332`). A transmission whose data is not exactly `s` x `v` x bytes per pixel fails; only animation frames may run long, and are cut (`:582-600`).
- **WezTerm** seeks to `O` and reads exactly `S` bytes with `read_exact`, from a file (`wezterm-escape-parser/src/apc.rs:185-190`) and from shared memory (`:288-293`), so a shorter source fails.
- **Rio** seeks to `O` and reads exactly `S` bytes with `read_exact` from a file (`rio-vt/src/ansi/kitty_graphics_protocol.rs:1093-1102`). For shared memory it fails when `S` passes the object's size (`:1169-1182`).
