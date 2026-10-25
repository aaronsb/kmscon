# KMSCON

> **This is a fork** working toward images, colour emoji and styled underlines on the console. See [docs/fork/](docs/fork/README.md) for its goal, its relationship to upstream, how to build it and how it is tested. The requirements and build steps below are upstream's: this fork builds in a container with `make test` and never uses a system libtsm.

![Build Status](https://github.com/kmscon/kmscon/actions/workflows/meson.yml/badge.svg?branch=main)

Kmscon is a simple terminal emulator based on linux kernel mode setting (KMS).
It is an attempt to replace the in-kernel VT implementation with a userspace
console. See kmscon(1) man-page for usage information.

## Requirements
### Mandatory dependencies
Kmscon requires the following software:
  - [libtsm](https://github.com/kmscon/libtsm): terminal emulator state machine
  - [libudev](https://www.freedesktop.org/software/systemd/man/libudev.html): providing input, video, etc. device hotplug support (>=v172)
  - [libxkbcommon](https://xkbcommon.org/): providing internationalized keyboard handling
  - **linux-headers**: linux kernel headers for ABI definitions

### Optional dependencies
#### Video

For video output at least one of the following is required:
- [libdrm](https://gitlab.freedesktop.org/mesa/drm): graphics access to DRM/KMS subsystemDRM: For unaccelerated drm output the "libdrm" library must be installed and accessible via pkg-config.
- **OpenGLES2**: For accelerated video output via OpenGLESv2 the following must be installed: libdrm, libgbm, egl, glesv2 (i.e., mesa)
- **fbdev**: For framebuffer video output the kernel headers must be installed and located in the default include path, and the kernel built with fbdev device support (which is disabled on some distributions).

#### Fonts
For font handling the following is required:
- **8x16**: static built-in font which does not require external dependencies.
- [psf](https://aeb.win.tue.nl/linux/kbd/font-formats-1.html): bitmap font backend, this requires no external dependencies, but you need to provide a path to a psf file that kmscon can read. It reads gzipped fonts (`.psf.gz`, `.psfu.gz`) with zlib; a build without zlib reads uncompressed ones only.
- [unifont](https://unifoundry.com/unifont/index.html): embed unifont in kmscon; it needs [zlib](https://zlib.net/).
- [freetype](https://freetype.org/): lightweight font rendering, using only freetype2 and fontconfig.
- [pango](https://gitlab.gnome.org/GNOME/pango): drawing text with Pango requires: glib, pango, fontconfig, freetype2 and more

#### Seats
- [libseat](https://sr.ht/~kennylevinsen/seatd/): Kmscon can take a seat with libseat . In this case you must have either systemd-logind, elogind, or seatd configured. It allows to run kmscon as a regular user, and configure which GPU/input device are allowed for kmscon. The drawback is that kmscon-launch-gui won't work to launch another GUI that uses libseat, as the seat is already in use.
- If libseat is not present, kmscon will use its own VT handling, or a fake backend if VT is disabled in the kernel.

#### Images
- [libpng](http://www.libpng.org/pub/png/libpng.html) and zlib: sixel and the kitty graphics protocol, whose kmscon-image-helper decodes PNG and compressed images as the session's user (ADR-103). Without libpng, an `auto` build leaves images out: no helper, `--sixel` and `--kitty-graphics` turned off at start, and libtsm built without its sixel decoder and kitty parser.

#### Session user
- sd-login, from libsystemd or libelogind: kmscon-image-helper, which reads the files a kitty graphics command names, runs as the user of the logind session kmscon's login started. Before login (logind running and no session yet), files are refused and inline images decode as the system user `kmscon-image`, which kmscon creates through a sysusers.d entry, or `nobody`. Without sd-login, or with no logind running, it runs as the owner of the session's pty, except that a root-owned pty is treated as before login. [docs/image-helper.md](docs/image-helper.md) explains the rules and why the dedicated user exists.

#### Terminfo
 - [ncurses](https://invisible-island.net/ncurses/) You need the `tic` executable from ncurses, to build kmscon.ti into a binary terminfo definition.

## Setup

On Debian-based system, to install the systemd service files in the right location, you need to install systemd-dev.
```bash
sudo apt install systemd-dev
```

To build with manpages and documentation, the following is required:
 - xsltproc (packaged as `libxslt` in Fedora, or `xsltproc` in Ubuntu/Debian)
 - docbook stylesheets (packaged as `docbook-style-xsl` in Fedora, or `docbook-xsl` in Ubuntu/Debian)

## Download

Released tarballs can be found at: https://github.com/kmscon/kmscon/releases

## Install

To compile the kmscon binary, run the standard meson commands:
```bash
meson setup builddir/
````

By default this will install into `/usr/local`, you can change your prefix with `--prefix=/usr`
(or `meson configure builddir/ -Dprefix=/usr` after the initial meson setup).

Then build and install. Note that this requires ninja.
```bash
meson install -C builddir/
```

The following meson options are available.
They can be used to select backends for several subsystems in kmscon.
If build-time dependencies cannot be satisfied, an option is automatically turned off, except if you
explicitly enable it via command line:

| option | default | description |
|:------|:-------:|:-----------|
|`extra_debug`| `false` | Additional debug outputs |
|`libseat`| `false` | Use libseat to get access to device (DRM and inputs) |
|`zlib`| `auto` | zlib: gzipped PSF fonts, the unifont backend and images |
|`images`| `auto` | Sixel and the kitty graphics protocol, with the image helper (libpng, zlib) |
|`sd_login`| `auto` | Find the image helper's user through sd-login (libsystemd, then libelogind); needs `images` |
|`video_fbdev`| `auto` | Linux fbdev video backend |
|`video_drm2d`| `auto` | Linux DRM software-rendering backend |
|`video_drm3d`| `auto` | Linux DRM hardware-rendering backend |
|`font_psf`| `auto` | PC Screen font renderer, support psf1/psf2 |
|`font_unifont`| `auto` | Static built-in font, with integer scaling (Unicode Unifont) |
|`font_freetype`| `auto` | Freetype2 based scalable font renderer, also handle bitmap fonts |
|`font_pango`| `auto` | Pango based scalable font renderer |
|`renderer_gltex`| `auto` | OpenGLESv2 accelerated renderer |
|`docs`|`auto`| Build manpages and documentation |
|`term`|`kmscon`| Default $TERM value |

### Minimal build

The minimal build is kmscon as a userspace replacement for the kernel's console, with nothing it does not need: text on the screen through DRM or fbdev, read and written through a pty, with keyboard input through xkbcommon. One machine file selects it:

```sh
meson setup builddir --native-file scripts/profiles/minimal.ini
```

`scripts/profiles/minimal.ini` turns off `images`, `video_drm3d`, `renderer_gltex`, `font_freetype`, `font_pango` and `dbus`, turns on `zlib`, `font_psf` and `font_unifont`, and builds libtsm as the pinned static subproject, so libtsm leaves out its image decoders too. A `-D` given after the file overrides any of these. In the dev container, `make test PROFILE=minimal` builds and tests it in `fork/build-minimal`, and `make profile-check PROFILE=minimal` also checks what the installed files link.

| | |
|---|---|
| **You keep** | text with 16, 256 and 24-bit colours; Unicode across the Basic Multilingual Plane through unifont, the default, or a PSF console font (`font-engine=psf` and `font-name`), whose missing glyphs draw as U+FFFD or `?`; xkb keyboard layouts set with `--xkb-layout` or kmscon.conf; scrollback; drm2d and fbdev on every display; multiseat |
| **You give up** | inline images, sixel and the kitty graphics protocol; GPU drawing, so `hwaccel` has nothing to turn on; FreeType and Pango text: outline fonts, ligatures and other shaping, complex scripts, colour emoji, and fontconfig's font fallback; the keyboard layout from `localectl` |
| **You gain** | fewer libraries and less code reading untrusted data: no PNG decoder, no image helper, no GL stack, no font shaper, no D-Bus; a smaller install; a console that starts with libdrm, libudev, libxkbcommon and zlib alone |
| **Who it is for** | consoles on servers and appliances that show only text, rescue and recovery systems, and old or small hardware |

It is made for the fork's potato computer standard: tier 3's `2d-1366x768-potato` variant, a guest with 1 vCPU, 512 MB, a 1366x768 display, drm2d and bbulk, is its baseline.

A console font takes two settings. With `font-engine` unset, kmscon takes the first font backend that loads, in the order freetype, pango, unifont, psf, 8x16, and unifont always loads, so `font-name` alone changes nothing in this build. `font-engine=psf` with `font-name` set to a PSF file selects it:

```
font-engine=psf
font-name=/usr/share/kbd/consolefonts/default8x16.psfu.gz
```

There is no fallback between backends glyph by glyph: a PSF font gives its 256 or 512 glyphs, as under fbcon, and unifont gives the Basic Multilingual Plane in unifont's own face.

Beside the kernel's console:

- *Matches fbcon:* it needs no session manager, display server, D-Bus or logind, only a DRM or fbdev device, udev and its own files; with `font-engine=psf` and `font-name` it reads the PSF fonts `setfont` reads (`/usr/share/kbd/consolefonts/*.psfu.gz` on Arch), and draws a character the font lacks as fbcon does, with the font's replacement glyph. Running it from an initramfs that has udev is in reach, and untested.
- *Adds to it:* with unifont, the default, the whole Basic Multilingual Plane, where fbcon draws at most a PSF font's 512 glyphs; 24-bit colour, xkb layouts, scrollback (fbcon's was removed in Linux 5.9), more than one seat, and DRM modesetting per display.
- *Stays in the kernel:* messages before userspace starts, and panic output, which the kernel draws itself through drm_panic.

**The zlib-free variant** leaves zlib out as well, with a second machine file read after the first:

```sh
meson setup builddir --native-file scripts/profiles/minimal.ini --native-file scripts/profiles/no-zlib.ini
```

It has no unifont, and mod-psf reads uncompressed fonts only, while every PSF font Arch's kbd ships is gzipped. Decompress one and select it:

```sh
gunzip -c /usr/share/kbd/consolefonts/default8x16.psfu.gz > /etc/kmscon/default8x16.psfu
```

```
font-engine=psf
font-name=/etc/kmscon/default8x16.psfu
```

Without one, text outside U+0000 to U+00FF, the built-in 8x16 font, draws only where kmscon's box-drawing and block-element sprites cover it. `make test PROFILE=zlib-free` builds it in the dev container.


## Running

To get usage information, run:
```bash
kmscon --help
```
You can then run kmscon with:
```bash
kmscon [options]
```

### Locale

Kmscon queries and setups system locale settings before starting if built with dbus enabled.
Otherwise, you can change locale settings via `--xkb-{model,layout,variant,options}` command line options.
See `man kmscon` for more information.

### Config file

The default configuration file is `/etc/kmscon/kmscon.conf`. Any command line option can be put in the config file in
its long form without the leading `--` (double dash). See `man kmscon` for more information or look at [kmscon.conf](scripts/etc/kmscon.conf.example)

## Differences from fbcon
### Loadkeys

loadkeys doesn't work in kmscon, as it doesn't use the kernel tty for input.
You can configure the layout in kmscon.conf, or in the command line.
If you build kmscon with dbus support, you can also change the layout at runtime with `localectl set-keymap <keymap>`

### setfont

setfont doesn't work, as it sets the kernel font.
you can configure the font in kmscon.conf
```
font-engine=freetype
font-size=18
# font-name is the font for freetype and pango, and the PSF file for psf;
# unifont and 8x16 use their own bitmaps.
font-name=Hack Nerd Font
```

### GUI application

As kmscon is already using the GPU, starting a Desktop environment, or a graphic application won't work, as it won't be able to take the GPU.
There is a script, kmscon-launch-gui that put kmscon in the background, and is able to start a gui application.

`kmscon-launch-gui kmscube -c 100`

For some application like mpv, you need an additional wayland compositor, or the keyboard input won't work.

`kmscon-launch-gui cage -- mpv test.mp4`

## License

This software is licensed under the terms of an MIT-like license. Please see
[`COPYING`](./COPYING) for further information.

The photograph the sixel test pages draw, `fork/tier3/guest/screens/photo/street-sunset.jpg`, is Aaron Bockelie's, licensed CC BY 4.0; its credit is in the [`LICENSE`](./fork/tier3/guest/screens/photo/LICENSE) beside it.

## History

This project was maintained in [Aetf's](https://github.com/Aetf/kmscon) fork for 11 years, before coming back here in 2025
