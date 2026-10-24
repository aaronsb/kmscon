#!/bin/bash
# kit: sixel
# look-for: Two photographs of the Earth, each drawn twice side by side, chafa
# look-for: on the left and libsixel (through mpv) on the right, both named
# look-for: under them: by day (blue oceans, green and brown land, white ice),
# look-for: then by night (city lights on black). Then what img2sixel wrote.
# wrong-looks-like: No pictures, a picture over the text or the names, colours
# wrong-looks-like: swapped or washed out (a photograph has no banding), torn
# wrong-looks-like: or shifted rows, a picture cut off, or one encoder's copy
# wrong-looks-like: in colours unlike the other's.
# screen-text: none, sixel pictures and cursor moves do not print as rows
# ADR-103's acceptance scenario A1: real photographs through two sixel
# encoders, compared side by side. The two are NASA Visible Earth
# photographs, which xplanet installs (its images/README: "These images are
# not copyrighted"). chafa, the operator's everyday tool, draws each on the
# left; libsixel's encoder draws it on the right by way of mpv (ffmpeg
# decodes, libsixel encodes). img2sixel, libsixel's own tool, writes nothing
# with the gdk-pixbuf Arch ships (2.44, through glycin): libsixel 1.10.5 asks
# gdk-pixbuf for the picture before closing the loader, gets none, and
# returns success, so the page prints how many bytes it wrote and a fixed
# libsixel shows here by itself. Each picture is five rows tall and as wide
# as keeps the 2:1 shape, from the cell size the terminal reports (XTWINOPS
# 16), placed at its cell with CUP; the text after them is placed with CUP
# too. Every row is counted from the row the page starts on (top_row), so the
# pictures stay under the page's first line below the VT demo's header. The
# page is 16 rows.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/acceptance.inc"
sixel_offered
needs chafa chafa
needs mpv mpv
needs_photos
cell_size
top_row
# Five rows tall, as many columns as keep the photographs' 2:1 shape.
r=5 w=$(((2 * r * ch + cw / 2) / cw))
printf 'NASA Visible Earth photographs, %d by %d cells: chafa left, libsixel right.\n' "$w" "$r"
row=$((top + 2))
for pair in day:earth night:night; do
	name=${pair%%:*} file=${pair#*:}.jpg
	chafa_photo "$file" 1 "$row" "$w" "$r"
	photo "$file" $((w + 3)) "$row" "$w" "$r"
	printf '%s[%d;1H%-*s  %s' "$esc" $((row + r)) "$w" "$name, chafa" "$name, libsixel (mpv)"
	row=$((row + r + 1))
done
printf '%s[%d;1H' "$esc" "$row"
if command -v img2sixel >/dev/null; then
	printf 'img2sixel wrote %d bytes for the day photograph (libsixel 1.10.5 with\n' \
		"$(img2sixel_bytes $((w * cw)))"
	printf 'gdk-pixbuf 2.44 writes none). '
else
	printf 'img2sixel is not installed (libsixel). '
fi
printf 'Cell %sx%s.%s\n' "$rw" "$rh" "${note:+ $note}"
