# Tier 3: the package in a VM, on a real KMS device

Tier 3 of the conformance harness (ADR-301). The fork is packaged the way a user installs it, installed in a pinned Arch Linux guest under QEMU/KVM, and run by systemd on a VT. A script logs in over the emulated keyboard, draws fixed screens, captures them, and compares each capture exactly with a committed golden. The exit status is the verdict.

## Running it

```sh
make pkg            # build kmscon-aaronsb from this checkout -> fork/build/pkg
make tier3-image    # once: fetch, verify and provision fork/vm/base.qcow2
make tier3          # pkg, then the 2D variants; report in fork/build/tier3-report.json
make tier3-gl       # pkg, then the GL variant, which is outside the default run (#17)
make tier3 VARIANTS="2d-1280x800 2d-7680x2160 gl-1280x800"   # any set of variants
make tier3-run VARIANTS=2d-1280x800     # without rebuilding the package
make tier3 JOBS=2   # up to two variants at once, each in its own VM
make tier3-selftest # unit tests of the compare module
```

Host needs: docker (for `make pkg`), `qemu-system-x86_64` with KVM, `qemu-img`, `xorriso`, `gpg`, `curl`, python 3. Everything runs as the invoking user; nothing needs root. QEMU runs headless, one VM per variant (4 vCPUs, 4 GiB each), and the driver stops every VM it starts by the PID it started. Once the base image exists, the two 2D variants take about 50 s one after the other and about 28 s with `JOBS=2`.

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
2. Each variant boots a throwaway overlay of the base with a second disc labelled `T3PKG`: the built packages, the variant's `kmscon.conf`, the screen scripts and `guest/setup`. The boot hook runs `setup`, which installs the packages with `pacman -U`, routes kmscon's log to the serial console, and makes the second serial port (ttyS1) writable as the event port: the login shell and each screen script write a line there. Changing anything on the disc never needs a new base image.
3. The driver (`t3.py`) talks QMP on a unix socket. It waits for `TIER3-INSTALL-OK`, systemd's `Started User Login Management` and `Reached target Login Prompts` on the serial log, then presses Ctrl+Alt+F2: logind spawns kmscon on tty2, and a boot without the keypress has no kmscon there. It waits for kmscon's `vt: using tty /dev/tty2` and a drawn login prompt, logs in, and waits for the login shell's line on the event port. Then it types `t3 sgr`, `t3 boxdraw`, `t3 cjk`. After each, it waits for the screen script's line on the event port, then captures until two captures a second apart are identical and differ from the screen before.
4. VT switch: Ctrl+Alt+F3, then Ctrl+Alt+F2. The `vt-switch` case passes when logind's second kmscon on tty3 draws a screen that is neither blank nor the one just left, so a switch that never happens cannot let `vt-restore` pass on its own. `vt-restore` then requires the tty2 screen to equal the `cjk` golden exactly.
5. The driver reads kmscon's `Display ... with backend [..] text renderer [..]` line from the serial log and fails the variant if kmscon drew with a renderer other than the one the variant exists to test.

Each step waits for state the driver can observe: a line on the serial log or the event port, or a screen that has changed and then settled. Every wait has a timeout that fails the case and names what it waited for. On a slow host a capture taken too early can fail its case; it cannot pass it. The one fixed pause left is 50 ms between key chords, because QEMU's input queue is bounded and shows nothing outside QEMU. A password typed before the prompt is discarded by PAM, so when no login follows within 20 s the driver types it once more.

A step that fails part way keeps the captures taken before it: each remaining case is reported as `fail` with the error.

## Concurrent runs

`JOBS=N` runs up to N variants at once, each in its own worker process and VM; the default, 1, runs them one after the other in the driver's own process. The report lists variants in the requested order, so it equals a serial run's report apart from each variant's `timing`. GL variants share the host's render node and never run together; a GL variant runs beside a 2D one. A second run started while one holds `fork/build/tier3.lock` exits at once with the holder's PID. `make tier3` and `make tier3-gl` build the package under that lock, and a run installs the same package build in every variant. A worker that dies fails its own variant only; the driver kills any QEMU of that variant it left behind. A worker still running after `T3_DEADLINE` seconds (default 1800) is sent SIGTERM, stops its VM and fails its variant, and QMP commands time out after 30 s, so a wedged QEMU fails its variant instead of hanging the run. A run that is itself sent SIGTERM or Ctrl+C stops its workers and VMs before it releases the lock, and writes a report in which the unfinished variants fail. Log lines carry the seconds since the run started and the variant's name.

Only `JOBS=2` has been measured, on a 32-thread host. There is one GL variant, so the rule that keeps GL variants apart has been checked only with stand-in jobs. The default stays 1 until serial and concurrent reports have matched over more runs. Snapshots that would let a variant skip its boot are deferred (#16).

## Variants

| Variant | Device | kmscon path | Capture |
|---|---|---|---|
| `2d-1280x800` | `virtio-vga`, EDID 1280x800 | drm2d + bbulk | QMP `screendump` (PPM) |
| `2d-7680x2160` | `virtio-vga`, EDID 7680x2160, `max_hostmem=1G` | drm2d + bbulk | QMP `screendump` (PPM) |
| `gl-1280x800` | `virtio-vga-gl` with `-display egl-headless` (virgl on the host's render node, `T3_RENDERNODE`, default `/dev/dri/renderD128`) | drm3d + gltex (`hwaccel`) | VNC over a unix socket |

Two findings shaped this table:

- At 7680x2160 each XRGB buffer is 63 MiB. With the default `max_hostmem` (256 MiB) the second kmscon on tty3 fails to create its buffers (`virtio_gpu ... response 0x1201`, out of memory). 1 GiB clears it.
- Once virgl owns the scanout, QMP `screendump` fails with "no surface". egl-headless reads the scanout back into a surface for a paired VNC server, so the GL variant captures over RFB (raw encoding, implemented in `t3.py`). Captures are byte-identical across boots, like the 2D ones.

## Goldens, tolerances and masks

Goldens are `golden/<variant>/<screen>.png`, full resolution and lossless, written by `t3img.py` through zlib (all nine come to about 300 KB, the 7680x2160 ones included).

Comparison is exact unless a tolerance sidecar `golden/<variant>/<screen>.tol` sits beside the golden. It holds `key=value` lines; `#` starts a comment:

```
# cursor cell blinks; antialiasing differs by one level on the GL path
max_delta=1          largest per-channel difference allowed
max_pixels=200       how many pixels may differ
mask=0,780,10,800    x0,y0,x1,y1, end-exclusive, left out of the comparison; repeat for more
```

An absent `max_delta` or `max_pixels` is 0, and a malformed sidecar fails its case. No golden has a sidecar today: the guest config turns blinking off, the prompt is fixed and no screen shows a clock.

Each case gets one of four verdicts:

| Status | Meaning | Gate |
|---|---|---|
| `pass` | identical outside the masks | passes |
| `near` | differs, within both limits of the case's sidecar | passes |
| `fail` | differs beyond the tolerance, or the case could not run | fails |
| `missing` | no golden yet | fails |

The tier exits non-zero on any `fail` or `missing`.

`make tier3-bless` copies the last run's captures over the goldens. Per ADR-301 a golden or sidecar change lands in its own reviewed commit, never with the code that produced it, and each new golden is looked at before it is committed.

## Report

`fork/build/tier3-report.json`, one case per variant and screen:

```json
{"tier": 3,
 "cases": [{"name": "2d-1280x800/sgr", "status": "near",
            "description": "SGR attributes and colours: ...",
            "golden": "fork/tier3/golden/2d-1280x800/sgr.png",
            "actual": "fork/build/tier3/2d-1280x800/sgr.png",
            "diff": "fork/build/tier3/2d-1280x800/sgr.diff.png",
            "tolerance": {"max_delta": 1, "max_pixels": 1, "masks": []},
            "stats": {"pixels": 1, "bbox": [[1000, 700, 1001, 701]], "max_delta": 1},
            "thumb_actual": "fork/build/tier3/2d-1280x800/sgr.thumb.png",
            "thumb_golden": "fork/build/tier3/2d-1280x800/sgr.golden.thumb.png",
            "thumb_diff": "fork/build/tier3/2d-1280x800/sgr.diff.thumb.png"}],
 "variants": {"2d-1280x800": {"packages": ["..."], "kmscon": ["Display ... text renderer [bbulk] ..."],
                              "errors": {}}}}
```

- `tolerance` is `null` for an exact case.
- `vt-switch` has no golden (`"golden": null`); it is a check.
- `description` is the caption on the run page: the header comment of the screen's script, or for `vt-switch` and `vt-restore` their entry in `CHECKS` in `t3.py`.
- `actual`, `diff` and the thumbnails are PNGs, so the run page can show them. The raw PPM captures sit beside them.
- Thumbnails are at most 480 pixels wide, nearest-neighbour. In the diff thumbnail, any block holding a differing pixel is red, so a one-pixel difference stays visible. Fields are `null` where there is nothing to show.
- `timing` under each variant holds the seconds from the variant's start to the end of each phase (`install`, `boot`, `kmscon`, `login`, each screen, `vt-switch`, `vt-restore`) and the seconds `evaluate` took.
- `bbox` is a list of at most 16 regions for outlining. Differing pixels in touching 16-pixel tiles share a region, coarser tiles are used while there are many clusters, and then the closest regions merge.
- A variant that cannot boot or drive reports each of its cases as `fail` with an `error`.

## The gate has been seen red

- Guest config: `palette=solarized` appended to `guest/kmscon.conf` turned the four golden cases of `2d-1280x800` red (929 760 to 1 024 000 pixels; `vt-switch`, a check, stayed green); restoring the file turned them green.
- Code: offsetting double-width glyphs by one pixel in `src/render/bbulk.c` (rebuilt with `make pkg`) turned `cjk` and `vt-restore` red (1 870 and 1 852 pixels, all within the CJK lines) and left `sgr` and `boxdraw` green; restoring the file and rebuilding turned them green.
- Concurrent: the same `palette=solarized` under `JOBS=2` turned the four golden cases of both 2D variants red at once (`2d-7680x2160`: 16 494 560 to 16 588 800 pixels); restoring the file turned them green.
- Tolerance: a golden altered by one channel level at one pixel, with a sidecar `max_delta=1`, `max_pixels=1`, gave `near` and exit 0; without the sidecar the same run gave `fail` and exit 2. The golden was then restored.

## Known failure: the GL renderer under virgl, intermittently

In 6 of 16 runs of `gl-1280x800` on this host, one kmscon instance drew nothing: every glyph upload failed with `text_gltex: cannot load glyph data into OpenGL texture (1282: <INVALID_OPERATION>)` (21 504 times per failing run, on the serial log), and the screen stayed black. Four times it was the second instance (tty3, so `vt-switch` failed), twice the first (tty2, so every case failed). The 2D variants have not failed in any run. Whether the fault is in kmscon's gltex atlas code, guest Mesa's virgl driver or the host's virglrenderer is not established; it is tracked in aaronsb/kmscon#17, with versions and a reproduction. Until it is resolved the variant is outside the default `make tier3` and runs with `make tier3-gl`.

## Compare tool

`t3img.py` reads PPM and PNG and compares with a tolerance sidecar, stdlib only (`t3img.py compare ACTUAL GOLDEN [--tol FILE] [--diff D]`, exit 1 on `fail`). Tier 2's `pixcmp` covers the same ground and was built in parallel; merging them is aaronsb/kmscon#18.
