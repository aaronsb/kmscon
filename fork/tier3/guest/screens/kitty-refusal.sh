#!/bin/bash
# kit: kitty
# look-for: Two lines: /etc/shadow, a file only root can read, and the
# look-for: terminal's reply i=7;EBADF:Failed to read image file. No picture,
# look-for: nothing else.
# wrong-looks-like: Any picture, a reply other than EBADF (OK would mean kmscon
# wrong-looks-like: read a root-only file for an ordinary user), or no reply.
# screen-text: none, the reply is read from the terminal, not printed by it
# The kitty graphics protocol's refusal of a file the session's user cannot
# read (ADR-103 step 4c; the root-file refusal is 4d's tier 3 case too).
# The page names /etc/shadow, which only root may read, with t=f and asks
# for a reply. kmscon's image helper runs as the session's user, never as
# root by fallback, so the open fails and the reply is the spec's one read
# failure, EBADF:Failed to read image file; before login, as in the VT demo,
# kmscon refuses the file without starting the helper, with the same reply.
# kmscon --debug logs which it was. The page refuses to run as root, where
# the file is readable and the case means nothing.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/kitty-common.inc"
kitty_offered
file=/etc/shadow
if [ -r "$file" ]; then
	printf 'This page runs as a user who can read %s; run it as an ordinary\n' "$file"
	printf 'user, for whom only root can read it.\n'
	exit 0
fi
kitty_ask "a=T,i=7,t=f,f=100,C=1" "$(b64 "$file")"
printf 'A file only root can read, named with t=f: %s\n' "$file"
printf 'reply: %s\n' "$reply"
