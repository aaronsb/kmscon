#!/bin/bash
# look-for: Each underline in the colour its line names while the text
# look-for: keeps its own; after 59 the line turns red with the text; 58
# look-for: alone draws none.
# wrong-looks-like: Underlines in the text's colour, rows with no
# wrong-looks-like: underline, or blinking rows: stock kmscon reads 58;5;n
# wrong-looks-like: as blink and drops colon rows.
# Underline colour, SGR 58 (set) and 59 (reset), in the colon and semicolon
# forms. Upstream libtsm reads 58;5;n with the 5 as blink, so there the
# semicolon rows blink, and it discards the colon rows' sequences entirely.
row() {
	printf '  %-20s \033[%sm%s\033[0m\n' "$1" "$2" "$3"
}
row '4;58:5:196' '4;58:5:196' 'white text, red underline'
row '4;58;5;196' '4;58;5;196' 'white text, red underline'
row '4;58:2::0:160:255' '4;58:2::0:160:255' 'white text, sky-blue underline'
row '4;58;2;0;160;255' '4;58;2;0;160;255' 'white text, sky-blue underline'
row '4:3;58:5:196' '4:3;58:5:196' 'white text, red curly underline'
row '4:3;58:5:226' '4:3;58:5:226' 'white text, yellow curly underline'
row '4:2;32;58:5:201' '4:2;32;58:5:201' 'green text, magenta double underline'
row '4;31;58:5:46 ... 59' '4;31;58:5:46' "red text, green line$(printf '\033[59m') then a red line"
row '58:5:196, no 4' '58:5:196' 'no underline: 58 alone draws nothing'
