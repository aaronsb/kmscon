#!/bin/sh
# Regenerate NotoColorEmoji-Subset.ttf from the Noto Color Emoji that the dev
# image pins (noto-fonts-emoji, through DEV_ARCHIVE in the Makefile). Run it
# in the dev image from the repository root:
#
#   docker run --rm --user $(id -u):$(id -g) -v $PWD:/src -w /src kmscon-dev \
#       tests/fonts/subset-emoji.sh
#
# The subset holds the codepoints the emoji screens of tests/test_pixels.c draw
# from the colour face, and every glyph HarfBuzz reaches from them through
# the font's substitutions (ZWJ sequences, flags, keycaps, skin tones). Adding
# a codepoint to those screens means adding it here, regenerating the subset
# and every golden that draws an emoji.
set -eu

src=/usr/share/fonts/noto/NotoColorEmoji.ttf
out=tests/fonts/NotoColorEmoji-Subset.ttf

# 😀 👍 🏽 👨 👩 👧 ZWJ 🇯 🇵 1 VS16 VS15 keycap ❤ ☺
unicodes=U+1F600,U+1F44D,U+1F3FD,U+1F468,U+1F469,U+1F467,U+200D,U+1F1EF,U+1F1F5,U+0031,U+FE0F,U+FE0E,U+20E3,U+2764,U+263A

pyftsubset "$src" --unicodes="$unicodes" --layout-features='*' --output-file="$out"
