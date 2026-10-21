# Tier 3: the package in a VM, on a real KMS device

Tier 3 of the conformance harness (ADR-301). The fork is packaged the way a user installs it, installed in a pinned Arch Linux guest under QEMU/KVM, and run by systemd on a VT. A script logs in over the emulated keyboard, draws fixed screens, captures them, and compares each capture exactly with a committed golden. The exit status is the verdict.

## Running it

```sh
make pkg            # build kmscon-aaronsb from this checkout -> fork/build/pkg
make tier3-image    # once: fetch, verify and provision fork/vm/base.qcow2
make tier3          # pkg, then every variant; report in fork/build/tier3-report.json
make tier3-run VARIANTS=2d-1280x800     # one variant, without rebuilding the package
make tier3-selftest # unit tests of the compare module
```

Host needs: docker (for `make pkg`), `qemu-system-x86_64` with KVM, `qemu-img`, `xorriso`, `gpg`, `curl`, python 3. Everything runs as the invoking user; nothing needs root. QEMU runs headless, one VM at a time (4 vCPUs, 4 GiB), and the driver stops every VM it starts by the PID it started. A run takes about 40 seconds per variant once the base image exists.

## What is pinned

| Piece | Pin |
|---|---|
| Guest image | `Arch-Linux-x86_64-cloudimg-20261015.611680.qcow2` from `geo.mirror.pkgbuild.com/images/v20261015.611680/` |
| Image SHA-256 | `08754e28d91b1680086c6211ef4df13559e766fff7cdb4006489b14b5d281f08` |
| Image signature | detached `.sig` by the arch-boxes key, primary fingerprint `1B9A16984A4E8CB448712D2AE0B78BF4326C6F8F` (fetched from `keyserver.ubuntu.com` into `fork/vm/images/gnupg`; archlinux.org publishes no WKD entry for it, so the fingerprint pin is the trust anchor) |
| Guest packages | Arch Linux Archive snapshot `2026/10/20`; provisioning runs `pacman -Syyuu` against it, so the guest matches the snapshot exactly |
| Fonts | `ttf-dejavu 2.37+18+g9b5d1b2f-8` (DejaVu Sans Mono, Latin and box drawing) and `wqy-microhei 0.2.0_beta-12` (CJK fallback through pango), from the snapshot |

The pins live at the top of `fork/tier3.mk`. The snapshot date has to be no older than the dev image the package is built in, or the package can need a newer glibc than the guest has.

## How a run works

1. `make tier3-image` provisions the base once: a qcow2 overlay on the pristine image, booted with a cloud-init seed disc (`guest/user-data.in`). Cloud-init creates the user `tester` (password `tester`, prompt `$ `, no MOTD), installs kmscon's runtime dependencies and the fonts, points `autovt@.service` at `kmsconvt@.service`, enables the boot hook `tier3-install.service`, disables itself and powers off.
2. Each variant boots a throwaway overlay of the base with a second disc labelled `T3PKG`: the built packages, the variant's `kmscon.conf`, the screen scripts and `guest/setup`. The boot hook runs `setup`, which installs the packages with `pacman -U` and routes kmscon's log to the serial console. Changing anything on the disc never needs a new base image.
3. The driver (`t3.py`) talks QMP on a unix socket. It waits for `TIER3-INSTALL-OK` on the serial log, presses Ctrl+Alt+F2 (logind spawns kmscon on tty2), logs in, and types `t3 sgr`, `t3 boxdraw`, `t3 cjk`. After each, it captures until two captures a second apart are identical.
4. VT switch: Ctrl+Alt+F3, then Ctrl+Alt+F2. The `vt-switch` case passes when logind's second kmscon on tty3 draws a screen that is neither blank nor the one just left, so a switch that never happens cannot let `vt-restore` pass on its own. `vt-restore` then requires the tty2 screen to equal the `cjk` golden exactly.
5. The driver reads kmscon's `Display ... with backend [..] text renderer [..]` line from the serial log and fails the variant if kmscon drew with a renderer other than the one the variant exists to test.

A step that fails part way keeps the captures taken before it: each remaining case is reported as `fail` with the error.

## Variants

| Variant | Device | kmscon path | Capture |
|---|---|---|---|
| `2d-1280x800` | `virtio-vga`, EDID 1280x800 | drm2d + bbulk | QMP `screendump` (PPM) |
| `2d-7680x2160` | `virtio-vga`, EDID 7680x2160, `max_hostmem=1G` | drm2d + bbulk | QMP `screendump` (PPM) |
| `gl-1280x800` | `virtio-vga-gl` with `-display egl-headless` (virgl on the host's render node, `T3_RENDERNODE`, default `/dev/dri/renderD128`) | drm3d + gltex (`hwaccel`) | VNC over a unix socket |

Two findings shaped this table:

- At 7680x2160 each XRGB buffer is 63 MiB. With the default `max_hostmem` (256 MiB) the second kmscon on tty3 fails to create its buffers (`virtio_gpu ... response 0x1201`, out of memory). 1 GiB clears it.
- Once virgl owns the scanout, QMP `screendump` fails with "no surface". egl-headless reads the scanout back into a surface for a paired VNC server, so the GL variant captures over RFB (raw encoding, implemented in `t3.py`). Captures are byte-identical across boots, like the 2D ones.

## Goldens and masks

Goldens are `golden/<variant>/<screen>.png`, full resolution and lossless, written by `t3img.py` through zlib (all nine come to about 300 KB, the 7680x2160 ones included). Comparison is exact. A mask for regions that legitimately vary sits beside its golden as `<screen>.mask.json`:

```json
{"rects": [[x0, y0, x1, y1]], "why": "cursor cell"}
```

Rectangles are end-exclusive. No golden needs one today: the guest config turns blinking off, the prompt is fixed and no screen shows a clock.

`make tier3-bless` copies the last run's captures over the goldens. Per ADR-301 a golden change lands in its own reviewed commit, never with the code that produced it, and each new golden is looked at before it is committed.

## Report

`fork/build/tier3-report.json`, one case per variant and screen:

```json
{"tier": 3,
 "cases": [{"name": "2d-1280x800/sgr", "status": "pass", "golden": "fork/tier3/golden/2d-1280x800/sgr.png",
            "actual": "fork/build/tier3/2d-1280x800/sgr.png", "diff": null,
            "stats": {"pixels": 0, "bbox": [], "max_delta": 0}}],
 "variants": {"2d-1280x800": {"packages": ["..."], "kmscon": ["Display ... text renderer [bbulk] ..."]}}}
```

`status` is `pass`, `fail` or `missing` (no golden yet). `vt-switch` has no golden (`"golden": null`); it is a check. `actual` and `diff` are PNGs so a contact sheet can show them; the raw PPM captures sit beside them. `bbox` groups differing pixels into boxes of touching 16-pixel tiles. A variant that cannot boot or drive reports each of its cases as `fail` with an `error`.

## The gate has been seen red

- Guest config: `palette=solarized` appended to `guest/kmscon.conf` turned the four golden cases of `2d-1280x800` red (929 760 to 1 024 000 pixels; `vt-switch`, a check, stayed green); restoring the file turned them green.
- Code: offsetting double-width glyphs by one pixel in `src/render/bbulk.c` (rebuilt with `make pkg`) turned `cjk` and `vt-restore` red (1 870 and 1 852 pixels, all within the CJK lines) and left `sgr` and `boxdraw` green; restoring the file and rebuilding turned them green.

## Known failure: the GL renderer under virgl, intermittently

In 5 of 15 runs of `gl-1280x800` on this host, one kmscon instance drew nothing: every glyph upload failed with `text_gltex: cannot load glyph data into OpenGL texture (1282: <INVALID_OPERATION>)` (21 504 times per failing run, on the serial log), and the screen stayed black. Four times it was the second instance (tty3, so `vt-switch` failed), once the first (tty2, so every case failed). The 2D variants have not failed in any run. Whether the fault is in kmscon's gltex atlas code, guest Mesa's virgl driver or the host's virglrenderer is not established; the variant stays in the default run because the failure is real and a user on virgl would see a blank console.

## Compare tool

`t3img.py` reads PPM and PNG and compares with masks, stdlib only (`t3img.py compare ACTUAL GOLDEN [--mask M] [--diff D]`, exit 1 on difference). Tier 2's `pixcmp` covers the same ground and was built in parallel; the two should be merged.
