#!/bin/bash
# kit: kitty
# look-for: Logged in: the street at sunset three times, from its file (t=f),
# look-for: a temporary file (t=t) and shared memory (t=s), the replies i=2,
# look-for: i=3 and i=4;OK, "deleted" and "unlinked", then /etc/shadow refused
# look-for: with i=5;EBADF. In the VT demo all four say EBADF, with the note.
# wrong-looks-like: A picture missing beside an OK, a reply other than OK or
# wrong-looks-like: EBADF, the temporary file or object "still there" after an
# wrong-looks-like: OK, EBADF without the note, or anything but EBADF for
# wrong-looks-like: /etc/shadow.
# screen-text: none, kitty pictures and cursor moves do not print as rows
# The kitty graphics protocol's file media through kmscon's image helper
# (ADR-103 step 4c), each with the terminal's reply: the photograph's PNG
# named by its path (t=f), read and left; a copy in /tmp named
# tty-graphics-protocol-* as the spec asks of a temporary file (t=t), read
# and deleted; a copy in a POSIX shared memory object (t=s), its name
# starting with / and holding no other / as the spec requires, read and
# unlinked; and /etc/shadow, which only root may read, named with t=f and
# refused with the spec's one read failure, EBADF. The helper reads as the
# session's user, so the pictures show in a session logged in through
# login, as tier 3's is and as docs/fork/demo.md's login run is. The VT
# demo's session is in no logind session of its own, which kmscon treats
# as before login (docs/image-helper.md): every file medium answers EBADF
# with no helper started, the temporary file and the object are left, and
# this page removes them and says why. The page refuses to run as a user
# who can read /etc/shadow.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/kitty-common.inc"
kitty_offered
shadow=/etc/shadow
if [ -r "$shadow" ]; then
	printf 'This page runs as a user who can read %s; run it as an ordinary\n' "$shadow"
	printf 'user, for whom only root can read it.\n'
	exit 0
fi
top_row
printf 'The photograph from its file (t=f), a temporary file (t=t), shared memory (t=s):\n'
row=$((top + 1))
printf '%s[%d;1H' "$esc" "$row"
kitty_ask "a=T,i=2,t=f,f=100,C=1,c=20,r=8" "$(b64 "$photo_png")"
file_reply=$reply
temp=$(mktemp /tmp/tty-graphics-protocol-XXXXXX)
cp "$photo_png" "$temp"
printf '%s[%d;24H' "$esc" "$row"
kitty_ask "a=T,i=3,t=t,f=100,C=1,c=20,r=8" "$(b64 "$temp")"
temp_reply=$reply
temp_state=deleted
[ ! -e "$temp" ] || temp_state='still there'
rm -f "$temp"
shm=/kmscon-demo-$$-shm
cp "$photo_png" "/dev/shm$shm"
printf '%s[%d;47H' "$esc" "$row"
kitty_ask "a=T,i=4,t=s,f=100,C=1,c=20,r=8" "$(b64 "$shm")"
shm_reply=$reply
shm_state=unlinked
[ ! -e "/dev/shm$shm" ] || shm_state='still there'
rm -f "/dev/shm$shm"
kitty_ask "a=T,i=5,t=f,f=100,C=1" "$(b64 "$shadow")"
shadow_reply=$reply
printf '%s[%d;1H' "$esc" $((row + 8))
printf 't=f: %s\n' "$file_reply"
printf 't=t: %s, temporary file %s\n' "$temp_reply" "$temp_state"
printf 't=s: %s, object %s\n' "$shm_reply" "$shm_state"
printf 't=f %s (root only): %s\n' "$shadow" "$shadow_reply"
[[ $file_reply != *EBADF* ]] || kitty_refused_note
