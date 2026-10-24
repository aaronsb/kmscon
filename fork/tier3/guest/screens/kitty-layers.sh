#!/bin/bash
# kit: kitty
# look-for: Three places across, each over three rows of text. Left, the
# look-for: street at sunset under the text: every word drawn over it. Middle,
# look-for: the same picture over the text, hiding the words inside it.
# look-for: Right, the words alone and black around them: that placement was
# look-for: deleted (a=d), and nothing of it is left.
# wrong-looks-like: Words hidden on the left, words through the middle
# wrong-looks-like: picture, any pixel of the picture at the right, or a
# wrong-looks-like: picture over the labels under them.
# screen-text: none, kitty pictures and cursor moves do not print as rows
# Composition by z-index and a delete, through the kitty graphics protocol
# (ADR-103 step 4c). Three rows of text are written first across the page.
# The photograph's PNG is then transmitted once (a=t, i=5) and placed three
# times with a=p, each with its own placement id (p), 20x6 cells, the cursor
# kept (C=1): at the left with z=-1, below text, so the words draw over it;
# in the middle with z=1, above text, so it hides them; and at the right with
# z=-1 again. Then a=d with d=i, i=5, p=3 deletes the right placement only:
# its cells must show the words on black, with no pixel of the picture left.
# Every command carries q=2, so the terminal sends no reply.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/kitty-common.inc"
kitty_offered
top_row
printf 'One picture, three placements: under the text, over it, and deleted.\n'
row=$((top + 1))
for r in 1 3 5; do
	printf '%s[%d;1H' "$esc" $((row + r))
	for _ in 1 2 3; do
		printf ' words over/under %-6s' "row $r"
	done
done
printf '%s[%d;1H' "$esc" "$row"
kitty_send "a=t,i=5,f=100,q=2" "$(base64 -w0 "$photo_png")"
printf '%s[%d;1H' "$esc" "$row"
kitty_send "a=p,i=5,p=1,c=20,r=6,z=-1,C=1,q=2"
printf '%s[%d;27H' "$esc" "$row"
kitty_send "a=p,i=5,p=2,c=20,r=6,z=1,C=1,q=2"
printf '%s[%d;53H' "$esc" "$row"
kitty_send "a=p,i=5,p=3,c=20,r=6,z=-1,C=1,q=2"
kitty_send "a=d,d=i,i=5,p=3,q=2"
printf '%s[%d;1H%-26s%-26s%s\n' "$esc" $((row + 7)) "z=-1: under the text" \
	"z=1: over the text" "p=3: placed, then deleted"
