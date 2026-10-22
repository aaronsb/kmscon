#!/bin/bash
# demo: Each mark sits on its letter and the pair takes one cell: the decomposed
# demo: column matches the precomposed one, and the bars line up with |x|.
# Combining marks: a base letter followed by one or two marks forms one cell.
# The left column builds each letter from a base and marks; the right column
# is the precomposed character where Unicode has one. The marks are written as
# UTF-8 bytes so the output does not depend on the locale.
acute=$'\xcc\x81'     # U+0301
grave=$'\xcc\x80'     # U+0300
tilde=$'\xcc\x83'     # U+0303
diaeresis=$'\xcc\x88' # U+0308
ring=$'\xcc\x8a'      # U+030A
cedilla=$'\xcc\xa7'   # U+0327

row() {
	printf '  %-24s |%s|   |%s|\n' "$1" "$2" "$3"
}
printf '  %-24s %-6s %s\n' '' 'marks' 'one code point'
row 'e + U+0301 acute' "e$acute" 'é'
row 'a + U+0303 tilde' "a$tilde" 'ã'
row 'n + U+0303 tilde' "n$tilde" 'ñ'
row 'o + U+0308 diaeresis' "o$diaeresis" 'ö'
row 'A + U+030A ring' "A$ring" 'Å'
row 'c + U+0327 cedilla' "c$cedilla" 'ç'
row 'a + U+0303 + U+0300' "a$tilde$grave" "ã$grave"
row 'reference' 'x' 'x'
echo
printf '  in words:   |%s|  |%s|\n' "cafe$acute man${tilde}ana" 'café mañana'
printf '  reference:  |%s|  |%s|\n' 'xxxx xxxxxx' 'xxxx xxxxxx'
