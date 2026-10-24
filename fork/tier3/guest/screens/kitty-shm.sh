#!/bin/bash
# kit: kitty
# look-for: The street at sunset, 20 cells by 8 rows, read from a shared
# look-for: memory object (t=s); under it i=4;OK and "shared memory object:
# look-for: unlinked". In the VT demo the reply is EBADF, with the note.
# wrong-looks-like: No picture beside an OK, a reply other than OK or EBADF,
# wrong-looks-like: EBADF without the note, the object "still there" after
# wrong-looks-like: an OK, or a picture over the text.
# screen-text: none, a kitty picture and cursor moves do not print as rows
# The kitty graphics protocol's shared memory medium (t=s, ADR-103 step 4c):
# the photograph's PNG in a POSIX shared memory object, whose name starts
# with / and holds no other / as the spec requires, made by writing it under
# /dev/shm, where Linux keeps those objects. kmscon's image helper opens it
# as the session's user, reads it and unlinks it, as the spec asks of the
# terminal. In the VT demo's session, which kmscon treats as before login,
# it is refused with EBADF and left, and this page removes it itself
# (docs/image-helper.md).
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/kitty-common.inc"
kitty_offered
top_row
name=/kmscon-demo-$$-shm
cp "$photo_png" "/dev/shm${name}"
printf 'The photograph read from a shared memory object (t=s):\n'
kitty_ask "a=T,i=4,t=s,f=100,C=1,c=20,r=8" "$(b64 "$name")"
if [ -e "/dev/shm${name}" ]; then
	state='still there'
	rm -f "/dev/shm${name}"
else
	state=unlinked
fi
printf '%s[%d;1Hreply: %s\n' "$esc" $((top + 9)) "$reply"
printf 'shared memory object: %s\n' "$state"
[[ $reply != *EBADF* ]] || kitty_refused_note
