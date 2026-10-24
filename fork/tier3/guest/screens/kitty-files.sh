#!/bin/bash
# kit: kitty
# look-for: The street at sunset twice, side by side, each 20 cells by 8 rows:
# look-for: read from the file (t=f) on the left and from a temporary file
# look-for: (t=t) on the right; under them i=2;OK, i=3;OK and "temporary file:
# look-for: deleted". In the VT demo both say EBADF, with the note under them.
# wrong-looks-like: A picture missing beside an OK, a reply other than OK or
# wrong-looks-like: EBADF, EBADF without the note, the temporary file "still
# wrong-looks-like: there" after an OK, or a picture over the text.
# screen-text: none, kitty pictures and cursor moves do not print as rows
# The kitty graphics protocol's file media through kmscon's image helper
# (ADR-103 step 4c): the photograph's PNG named by its path (t=f), which the
# helper reads and leaves, and a copy of it in /tmp named
# tty-graphics-protocol-*, as the spec asks of a temporary file (t=t), which
# the helper reads and deletes. The helper reads as the session's user, so
# both work in a session logged in through login, as tier 3's is. The VT
# demo's session is in no logind session of its own, which kmscon treats as
# before login: it reads no file, answers EBADF, and leaves the temporary
# file, which this page then removes itself (docs/image-helper.md).
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/kitty-common.inc"
kitty_offered
top_row
printf 'The photograph read from its file (t=f) and from a temporary file (t=t):\n'
row=$((top + 1))
printf '%s[%d;1H' "$esc" "$row"
kitty_ask "a=T,i=2,t=f,f=100,C=1,c=20,r=8" "$(b64 "$photo_png")"
file_reply=$reply
temp=$(mktemp /tmp/tty-graphics-protocol-XXXXXX)
cp "$photo_png" "$temp"
printf '%s[%d;24H' "$esc" "$row"
kitty_ask "a=T,i=3,t=t,f=100,C=1,c=20,r=8" "$(b64 "$temp")"
temp_reply=$reply
if [ -e "$temp" ]; then
	temp_state='still there'
	rm -f "$temp"
else
	temp_state=deleted
fi
printf '%s[%d;1H%-23s%s\n' "$esc" $((row + 8)) "file: $file_reply" "temporary: $temp_reply"
printf 'temporary file: %s\n' "$temp_state"
[[ $file_reply != *EBADF* ]] || kitty_refused_note
