#!/bin/bash
# kit: glyphs
# look-for: Every printable character of Unicode's Basic Multilingual Plane
# look-for: scrolls past three times, then the last lines stay: letters,
# look-for: marks on dotted circles, digits, punctuation and symbols of every
# look-for: script, each in its own cell or two. The last screen reads as it
# look-for: did the first time it scrolled past.
# wrong-looks-like: A character where another should be, a blank cell where
# wrong-looks-like: the font has a glyph, or a wide character overlapping its
# wrong-looks-like: neighbour. Boxes for characters the font lacks are right.
# The glyph cache under its widest churn: about 60000 distinct glyphs, of every
# kind of slot, three times over (ADR-102).
here=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
cols=$(tput cols 2>/dev/null || echo 80)
for pass in 1 2 3; do
	python3 -I "$here/../glyph-text.py" bmp "$cols"
done
