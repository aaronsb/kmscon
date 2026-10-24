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
  - [zlib](https://zlib.net/) and [libpng](http://www.libpng.org/pub/png/libpng.html): kmscon-image-helper decodes the kitty graphics protocol's compressed and PNG images with them, as the session's user (ADR-103)
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
- [psf](https://aeb.win.tue.nl/linux/kbd/font-formats-1.html): bitmap font backend, this requires no external dependencies, but you need to provide a path to a psf file that kmscon can read.
- [unifont](https://unifoundry.com/unifont/index.html): embed unifont in kmscon, this requires no external dependencies.
- [freetype](https://freetype.org/): lightweight font rendering, using only freetype2 and fontconfig.
- [pango](https://gitlab.gnome.org/GNOME/pango): drawing text with Pango requires: glib, pango, fontconfig, freetype2 and more

#### Seats
- [libseat](https://sr.ht/~kennylevinsen/seatd/): Kmscon can take a seat with libseat . In this case you must have either systemd-logind, elogind, or seatd configured. It allows to run kmscon as a regular user, and configure which GPU/input device are allowed for kmscon. The drawback is that kmscon-launch-gui won't work to launch another GUI that uses libseat, as the seat is already in use.
- If libseat is not present, kmscon will use its own VT handling, or a fake backend if VT is disabled in the kernel.

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
|`sd_login`| `auto` | Find the image helper's user through sd-login (libsystemd, then libelogind) |
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
# font-name is only for freetype/pango. Unifont and 8x16 uses their own bitmap.
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
