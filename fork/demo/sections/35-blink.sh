#!/bin/bash
# look-for: Each "blink:" run vanishes and returns once a second, keeping
# look-for: its background, underline and strike; the rest stays still.
# wrong-looks-like: Nothing blinks, a run stays gone or comes back in part,
# wrong-looks-like: or steady text (after 25 too) flickers.
# Blinking text, SGR 5, beside steady text: plain, coloured, on a background,
# reversed, bold, underlined, struck, CJK and combining clusters, and a line
# that SGR 25 makes steady halfway. In the hidden phase a blinking cell keeps
# its background, reverse block, underline and strike and loses its glyph,
# as foot keeps them. The demo runs kmscon with blinking on, its default. It
# is not a tier 3 screen: tier 3 runs with no-blink, and a capture would catch
# one phase at a time it does not choose.
# tests/test_blink.c checks both phases through both of bbulk's buffers.
acute=$'\xcc\x81' # U+0301 COMBINING ACUTE ACCENT
tilde=$'\xcc\x83' # U+0303 COMBINING TILDE
grave=$'\xcc\x80' # U+0300 COMBINING GRAVE ACCENT

row() {
	printf '  %-15s steady  \033[%smblink: %s\033[0m  steady\n' "$1" "$2" "$3"
}
row '5' '5' 'plain text'
row '5;31' '5;31' 'red text'
row '5;33;44' '5;33;44' 'yellow on blue'
row '5;7' '5;7' 'reverse video'
row '5;1' '5;1' 'bold text'
row '5;4' '5;4' 'underlined'
row '5;4:3;58:5:196' '5;4:3;58:5:196' 'red curly line'
row '5;9' '5;9' 'struck through'
row '5' '5' '日本語 中文'
row '5' '5' "e$acute a$tilde$grave n"
echo
printf '  %-15s steady  \033[5mblink: here\033[25m, steady from here on\033[0m\n' '5, then 25'
printf '  %-15s steady  %s  steady\n' 'no SGR' 'blink: none, a reference'
