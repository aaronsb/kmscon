#!/bin/bash
# kit: sixel
# look-for: Two photographs of the Earth side by side, by day (blue oceans,
# look-for: green and brown land, white ice) and by night (city lights on
# look-for: black), each filling whole cells under the two lines of text,
# look-for: with its name under it. The last lines say what img2sixel wrote.
# wrong-looks-like: No pictures, a picture over the text above it or the names
# wrong-looks-like: under it, colours swapped or washed out (a photograph has
# wrong-looks-like: no banding), torn or shifted rows, or a picture cut off.
# screen-text: none, sixel pictures and cursor moves do not print as rows
# ADR-103's acceptance scenario A1: real photographs through libsixel. The
# two are NASA Visible Earth photographs, which xplanet installs (its
# images/README: "These images are not copyrighted"). img2sixel, libsixel's
# own tool, writes nothing with the gdk-pixbuf Arch ships (2.44, through
# glycin): libsixel 1.10.5 asks gdk-pixbuf for the picture before closing the
# loader and gets none, so the page draws them through libsixel's encoder by
# way of mpv (ffmpeg decodes, libsixel encodes), at a size of whole cells
# from the cell size the terminal reports (XTWINOPS 16), and prints how many
# bytes img2sixel wrote. Each picture is placed at its cell with CUP by mpv
# and stays on the screen as sixel content; the text after them is placed
# with CUP too.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/acceptance.inc"
sixel_offered
needs mpv mpv
needs_photos
cell_size
# Six rows tall, as many columns as keep the photographs' 2:1 shape.
r=6 w=$(((2 * r * ch + cw / 2) / cw))
printf 'Earth by day and by night: NASA Visible Earth photographs, drawn by\n'
printf 'mpv --vo=sixel through libsixel'\''s encoder, each %d by %d cells.\n' "$w" "$r"
photo earth.jpg 1 4 "$w" "$r"
photo night.jpg $((w + 3)) 4 "$w" "$r"
printf '%s[%d;1H%-*s  %s\n\n' "$esc" $((4 + r)) "$w" 'by day' 'by night'
if command -v img2sixel >/dev/null; then
	printf 'img2sixel wrote %d bytes for the day photograph' "$(img2sixel_bytes $((w * cw)))"
	printf ' (libsixel 1.10.5 with\ngdk-pixbuf 2.44 writes none).\n'
else
	printf 'img2sixel is not installed (libsixel).\n'
fi
printf 'Cell %sx%s.%s\n' "$rw" "$rh" "${note:+ $note}"
