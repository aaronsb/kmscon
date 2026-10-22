#!/bin/bash
# look-for: Each cluster is one picture two cells wide, bars in line with
# look-for: |ab|. Colour emoji are not implemented yet (ADR-102 step 4c).
# wrong-looks-like: Clusters in pieces or bars out of line. A clipped
# wrong-looks-like: corner or a monochrome emoji is expected until colour
# wrong-looks-like: emoji land.
# Emoji and multi-code-point clusters: a ZWJ sequence, a flag (two regional
# indicators), a keycap, a skin-tone modifier and VS16, which turns a text
# heart into an emoji one. Each sits between bars so its width shows. The
# invisible joiners are written as UTF-8 bytes so the output does not depend
# on the locale.
zwj=$'\xe2\x80\x8d'    # U+200D ZERO WIDTH JOINER
vs16=$'\xef\xb8\x8f'   # U+FE0F VARIATION SELECTOR-16
keycap=$'\xe2\x83\xa3' # U+20E3 COMBINING ENCLOSING KEYCAP
family="👨$zwj👩$zwj👧"

row() {
	printf '  %-32s |%s|\n' "$1" "$2"
}
row 'single emoji U+1F600' '😀'
row 'family: man ZWJ woman ZWJ girl' "$family"
row 'flag: regional indicators N O' '🇳🇴'
row 'keycap: 1 U+FE0F U+20E3' "1$vs16$keycap"
row 'thumbs up + skin tone U+1F3FD' '👍🏽'
row 'heart U+2764 + VS16' "❤$vs16"
row 'reference, two cells' 'ab'
echo
printf '  the text heart U+2764 alone is one cell:  |%s|  |a|\n' '❤'
printf '  in a line:  |%s|\n' "a😀b${family}c🇳🇴d"
printf '  reference:  |%s|\n' 'axxbxxcxxd'
