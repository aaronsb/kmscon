#!/bin/bash
# kit: glyphs
# look-for: Two thousand lines of CJK text scroll past for about half a
# look-for: minute, then stop: every ideograph whole in its two cells, the line
# look-for: numbers on the left counting up. Then Shift+PageUp a few pages into
# look-for: the scrollback and Shift+PageDown back before rating; it reads the
# look-for: same. With DEMO_ARGS='--hwaccel --render-timer=log' the terminal
# look-for: you ran make vt-demo from logs glyph_misses each second.
# wrong-looks-like: An ideograph where another should be, a blank cell or a
# wrong-looks-like: cut glyph in the text, a stall, or glyph_misses staying in
# wrong-looks-like: the hundreds a second after the scroll has stopped.
# The glyph cache under a reader's working set (ADR-102): a seeded text of
# 6000 ideographs, the first commoner, more than a screen holds, so a cache
# with too little room keeps drawing the rarer ones from the font again.
here=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
cols=$(tput cols 2>/dev/null || echo 80)
# paced on a terminal, as a reader scrolls; at once into a pipe (tests)
pace=0
[ ! -t 1 ] || pace=0.012
python3 -I "$here/../glyph-text.py" cjk 2000 "$cols" | while IFS= read -r line; do
	printf '%s\n' "$line"
	[ "$pace" = 0 ] || sleep "$pace"
done
