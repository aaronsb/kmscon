#!/bin/bash
# kit: sixel
# look-for: Three photographs of the Earth side by side under two lines of
# look-for: text: by day (blue oceans, green and brown land, white ice) and by
# look-for: night (city lights on black) from chafa, then by day again from
# look-for: libsixel through mpv, each named under it. Then what img2sixel wrote.
# wrong-looks-like: No pictures, a picture over the text above it or the names
# wrong-looks-like: under it, colours swapped or washed out (a photograph has
# wrong-looks-like: no banding), torn or shifted rows, or a picture cut off.
# screen-text: none, sixel pictures and cursor moves do not print as rows
# ADR-103's acceptance scenario A1: real photographs through the sixel tools
# people use. The two are NASA Visible Earth photographs, which xplanet
# installs (its images/README: "These images are not copyrighted"). chafa,
# the operator's everyday tool, draws both; libsixel's encoder draws the day
# one again by way of mpv (ffmpeg decodes, libsixel encodes). img2sixel,
# libsixel's own tool, writes nothing with the gdk-pixbuf Arch ships (2.44,
# through glycin): libsixel 1.10.5 asks gdk-pixbuf for the picture before
# closing the loader, gets none, and returns success, so the page prints how
# many bytes it wrote and a fixed libsixel shows here by itself. Each picture
# is six rows tall and as wide as keeps the 2:1 shape, from the cell size the
# terminal reports (XTWINOPS 16), placed at its cell with CUP; the text after
# them is placed with CUP too.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/acceptance.inc"
sixel_offered
needs chafa chafa
needs mpv mpv
needs_photos
cell_size
# Six rows tall, as many columns as keep the photographs' 2:1 shape.
r=6 w=$(((2 * r * ch + cw / 2) / cw))
printf 'Earth by day and by night, NASA Visible Earth photographs, each %d by %d\n' "$w" "$r"
printf 'cells: chafa draws the first two, libsixel (through mpv) the third.\n'
chafa_photo earth.jpg 1 4 "$w" "$r"
chafa_photo night.jpg $((w + 3)) 4 "$w" "$r"
photo earth.jpg $((2 * w + 5)) 4 "$w" "$r"
printf '%s[%d;1H%-*s  %-*s  %s\n\n' "$esc" $((4 + r)) "$w" 'day, chafa' "$w" 'night, chafa' \
	'day, libsixel'
if command -v img2sixel >/dev/null; then
	printf 'img2sixel wrote %d bytes for the day photograph' "$(img2sixel_bytes $((w * cw)))"
	printf ' (libsixel 1.10.5 with\ngdk-pixbuf 2.44 writes none).\n'
else
	printf 'img2sixel is not installed (libsixel).\n'
fi
printf 'Cell %sx%s.%s\n' "$rw" "$rh" "${note:+ $note}"
