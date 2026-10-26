"""What a reduced build installs and links (ADR-103, the minimal build): run on
the staging tree `meson install --destdir` wrote for a build configured with
scripts/profiles/minimal.ini (PROFILE minimal) or with no-zlib.ini after it
(PROFILE zlib-free).

- config.h: the features the profile turns off are off (images,
  video_drm3d, renderer_gltex, font_freetype, font_pango, dbus; and zlib and
  font_unifont for zlib-free), and video_drm2d, video_fbdev and font_psf (and
  font_unifont for minimal) are on. The feature loop writes config.h and the build summary from the same
  values.
- ldd: no installed executable or module has libpng, a GL library (libEGL,
  libGLESv2, libGL, libgbm), libpango, libharfbuzz, libfreetype,
  libfontconfig, libsystemd, libdbus-1 or a shared libtsm in its closure; nor
  libz for zlib-free. libxkbcommon in kmscon's closure is the run's positive
  control.
- readelf -d: libz.so.1 is named by mod-psf.so and mod-unifont.so and by
  nothing else; by nothing for zlib-free.
- nm: kmscon has no tsm_sixel_ or tsm_kitty_ symbol, after readelf -S shows
  it has a .symtab (a stripped binary would pass the check without one), and
  after CONTROL, kmscon from a build with images, has been seen to have them.
- not installed: kmscon-image-helper, kmscon-image.conf, mod-drm3d.so,
  mod-gltex.so, mod-freetype.so and mod-pango.so; mod-unifont.so for
  zlib-free. kmscon and mod-psf.so are.

Usage: check_profile.py PROFILE BUILDDIR STAGEDIR CONTROL
"""

import os
import re
import subprocess
import sys

OFF = ["images", "video_drm3d", "renderer_gltex", "font_freetype", "font_pango", "dbus"]
FORBIDDEN = ["libpng", "libEGL.", "libGLESv2.", "libGL.", "libgbm.", "libpango", "libharfbuzz",
             "libfreetype.", "libfontconfig.", "libsystemd.", "libdbus-1.", "libtsm"]
ON = ["video_drm2d", "video_fbdev", "font_psf"]
DECODER = re.compile(r" (tsm_sixel_|tsm_kitty_)\w+$", re.M)
PROFILES = {
    "minimal": {"off": OFF, "on": ON + ["font_unifont"], "forbidden": FORBIDDEN,
                "libz": {"mod-psf.so", "mod-unifont.so"}, "absent": []},
    "zlib-free": {"off": OFF + ["zlib", "font_unifont"], "on": ON,
                  "forbidden": FORBIDDEN + ["libz."], "libz": set(),
                  "absent": ["mod-unifont.so"]},
}


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def elf_files(stage):
    found = []
    for root, _, files in os.walk(stage):
        for name in files:
            path = os.path.join(root, name)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            with open(path, "rb") as f:
                if f.read(4) == b"\x7fELF":
                    found.append(path)
    return sorted(found)


def decoder_symbols(path):
    return len(DECODER.findall(run("nm", path)))


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in PROFILES:
        sys.exit(__doc__)
    name, build, stage, control = sys.argv[1:]
    want = PROFILES[name]
    errors = []

    with open(os.path.join(build, "config.h")) as f:
        config = f.read()
    for feature in want["off"] + want["on"]:
        macro = "BUILD_ENABLE_" + feature.upper()
        on = re.search(r"^#define %s\b" % macro, config, re.M) is not None
        off = re.search(r"^#undef %s\b" % macro, config, re.M) is not None
        if on == off:
            errors.append(f"config.h neither defines nor undefines {macro}")
        elif on != (feature in want["on"]):
            errors.append(f"{feature} is {'on' if on else 'off'} in this build")

    elves = elf_files(stage)
    names = {os.path.basename(p): p for p in elves}
    for required in ("kmscon", "mod-psf.so"):
        if required not in names:
            errors.append(f"{required} is not installed")
    for absent in ["kmscon-image-helper", "mod-drm3d.so", "mod-gltex.so", "mod-freetype.so",
                   "mod-pango.so"] + want["absent"]:
        if absent in names:
            errors.append(f"{absent} is installed")
    for root, _, files in os.walk(stage):
        if "kmscon-image.conf" in files:
            errors.append(f"kmscon-image.conf is installed in {root}")

    closure_ok = False
    named_libz = set()
    for path in elves:
        base = os.path.basename(path)
        closure = run("ldd", path)
        if base == "kmscon" and "libxkbcommon" in closure:
            closure_ok = True
        for lib in want["forbidden"]:
            for line in closure.splitlines():
                if line.strip().startswith(lib):
                    errors.append(f"{base}'s closure has {line.strip()}")
        needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^]]+)\]", run("readelf", "-d", path))
        if "libz.so.1" in needed:
            named_libz.add(base)
    if not closure_ok:
        errors.append("ldd does not show libxkbcommon in kmscon's closure: the closure check "
                      "would check nothing")
    if named_libz != want["libz"]:
        errors.append(f"libz.so.1 is named by {sorted(named_libz) or 'nothing'}, "
                      f"want {sorted(want['libz']) or 'nothing'}")

    kmscon = names.get("kmscon")
    control_count = decoder_symbols(control)
    if control_count == 0:
        errors.append(f"{control} has no tsm_sixel_ or tsm_kitty_ symbol: the nm check "
                      "would check nothing")
    if kmscon:
        if ".symtab" not in run("readelf", "-S", kmscon):
            errors.append("kmscon has no .symtab, so nm cannot show what it holds")
        elif decoder_symbols(kmscon):
            errors.append(f"kmscon has {decoder_symbols(kmscon)} tsm_sixel_ or tsm_kitty_ "
                          "symbols")

    for e in errors:
        print(f"check_profile {name}: {e}")
    if errors:
        return 1
    print(f"check_profile {name}: {len(elves)} installed ELF files; no forbidden library in "
          f"any closure; libz.so.1 named by {sorted(named_libz) or 'nothing'}; kmscon has no "
          f"decoder symbol, the control {control_count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
