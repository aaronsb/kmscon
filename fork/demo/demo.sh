#!/bin/bash
# The local VT demo (#36): a title page naming the terminal it runs in, then
# one page per section script in sections/, in file name order. Below each
# page a footer gives the section's guidance, held in the section file: what a
# correct screen looks like (its "# look-for:" lines) and what a defect looks
# like ("# wrong-looks-like:"). Then one key records a judgement (#47):
#
#   1 looks right   2 looks wrong   3 can't tell
#   n types a one-line note, then asks for the rating
#   b goes back one page   q quits
#
# Each judgement is appended as one TSV line to $DEMO_FEEDBACK, by default
# /tmp/kmscon-demo/feedback.tsv; feedback.sh prints it. A summary page with
# this run's ratings ends the run.
#
# It runs in any terminal as any user and needs nothing from the dev build, so
# the same pages can be compared on a dev-build VT and a stock kmscon VT.
#
#   demo.sh [SECTION...]   SECTION is a file name without the number and .sh,
#                          such as cjk; with none, every section runs
#
# DEMO_LABEL names the terminal on the title page and in the feedback;
# run-vt.sh sets it to "dev build <git describe>". DEMO_NOPROMPT=1 shows the
# guidance, asks nothing and records nothing: any key goes to the next page and
# q quits. DEMO_NOPAGE=1 also waits for no key and leaves the last page on the
# screen, for checking the sections outside a VT.
set -u

here=$(cd "$(dirname "$0")" && pwd)
nopage=${DEMO_NOPAGE:-}
noprompt=${DEMO_NOPROMPT:-$nopage}
feedback=${DEMO_FEEDBACK:-/tmp/kmscon-demo/feedback.tsv}
run=$(date +%Y%m%dT%H%M%S)-$$
rows=25
cols=80
key=
note=
go=

clear_screen() {
	printf '\033[0m\033[H\033[2J\033[3J'
}

read_size() {
	local s
	s=$(stty size </dev/tty 2>/dev/null) || s=
	rows=${s% *}
	cols=${s#* }
	[[ $rows =~ ^[1-9][0-9]*$ && $cols =~ ^[1-9][0-9]*$ ]] || { rows=25; cols=80; }
}

# Reads one key into $key. An escape sequence (an arrow key, a late reply from
# the terminal) is read whole and returned as ESC, so its bytes are never taken
# for keys.
read_key() {
	IFS= read -rsn1 key </dev/tty || exit 0
	[ "$key" != $'\033' ] || drain
}

# Discards keys already typed, so a key pressed before a prompt appeared does
# not answer it.
drain() {
	while IFS= read -rsn1 -t 0.05 _ </dev/tty; do :; done
}

# Shows PROMPT in reverse video on the current line and waits for a key; q ends
# the demo.
page_end() {
	[ -z "$nopage" ] || return 0
	printf '\033[7m %s \033[0m' "$1"
	read_key
	case $key in q | Q) clear_screen; exit 0 ;; esac
}

name_of() {
	local n=${1##*/}
	n=${n%.sh}
	printf '%s' "${n#[0-9][0-9]-}"
}

id_of() {
	local n=${1##*/}
	printf '%s' "${n%.sh}"
}

# A section's metadata lines "# KEY: text", joined into one line.
meta() {
	sed -n "s/^# $1: //p" "$2" | paste -sd' ' -
}

# TEXT folded to the screen's width, after a ten-column LABEL on the first
# line and under it on the others.
wrapped() {
	local label=$1 line
	[ -n "$2" ] || return 0
	while IFS= read -r line; do
		printf '%-10s%s\n' "$label" "${line% }"
		label=
	done < <(printf '%s\n' "$2" | fold -s -w $((cols - 11)))
}

# The footer of section I, without its prompt line, into foot[]: a blank line,
# the guidance, and the note typed for the page.
footer_lines() {
	local f=${sections[$1]}
	mapfile -t foot < <(
		echo
		wrapped 'right:' "$(meta look-for "$f")"
		wrapped 'wrong:' "$(meta wrong-looks-like "$f")"
		wrapped 'note:' "$note"
	)
}

draw_page() {
	clear_screen
	printf '\033[1m[%d/%d] %s\033[0m\n\n' $(($1 + 1)) ${#sections[@]} "$(name_of "${sections[$1]}")"
	bash "${sections[$1]}" || printf '\n\033[31m(section exited with status %d)\033[0m\n' "$?"
}

# The cursor's row, asked of the terminal (DSR 6); fails when the terminal
# does not answer within a second.
cursor_row() {
	local reply
	printf '\033[6n' >/dev/tty
	IFS= read -rsd R -t 1 reply </dev/tty || return 1
	reply=${reply##*[}
	reply=${reply%;*}
	[[ $reply =~ ^[0-9]+$ ]] || return 1
	printf '%s' "$reply"
}

# Tabs and line breaks become spaces, so a field stays one TSV field.
clean() {
	local s=${1//$'\t'/ }
	s=${s//$'\n'/ }
	printf '%s' "${s//$'\r'/ }"
}

# Creates the feedback file with its header line if it is missing. Sets
# recording, or why_not when the file cannot be written.
recording=
why_not=
init_feedback() {
	local dir=${feedback%/*}
	[ "$dir" != "$feedback" ] || dir=.
	[ -n "$dir" ] || dir=/
	[ -d "$dir" ] || mkdir -m 0755 -- "$dir" 2>/dev/null
	if [ ! -e "$feedback" ]; then
		(set -C; umask 022
			printf 'time\trun\tlabel\tterm\tsize\tsection\trating\tnote\n' >"$feedback") 2>/dev/null
	fi
	if [ -f "$feedback" ] && [ -w "$feedback" ]; then
		recording=1
	else
		why_not="$feedback cannot be written"
	fi
}

# Records RATING for section I with the typed note.
rating=()
notes=()
rate() {
	rating[$1]=$2
	notes[$1]=$note
	[ -n "$recording" ] || return 0
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -Iseconds)" "$run" \
		"$(clean "${DEMO_LABEL:-unlabelled}")" "$(clean "${TERM:-unset}")" \
		"${rows}x$cols" "$(id_of "${sections[$1]}")" "$2" "$(clean "$note")" \
		>>"$feedback" || { recording='' why_not="writing $feedback failed"; }
}

prompt() {
	printf '\r\033[K\033[7m %s \033[0m' "1 right  2 wrong  3 can't tell  n note  b back  q quit"
}

# Shows page I with its footer and reads keys until one rates the page, goes
# back or quits; go says which. When the footer does not fit below the page,
# it gets a screen of its own.
ask() {
	local i=$1 row
	note=
	while :; do
		read_size
		draw_page "$i"
		footer_lines "$i"
		if ! row=$(cursor_row) || [ $((row + ${#foot[@]})) -gt "$rows" ]; then
			drain
			printf '\n\033[7m %s \033[0m' 'press a key for the questions on this page'
			read_key
			clear_screen
			printf '\033[1m[%d/%d] %s: questions\033[0m\n' $((i + 1)) ${#sections[@]} \
				"$(name_of "${sections[$i]}")"
		fi
		printf '%s\n' "${foot[@]}"
		drain
		prompt
		while :; do
			read_key
			case $key in
			1) rate "$i" right; go=next; return ;;
			2) rate "$i" wrong; go=next; return ;;
			3) rate "$i" unsure; go=next; return ;;
			n | N)
				printf '\r\033[K'
				IFS= read -r -e -i "$note" -p 'note: ' note </dev/tty || note=
				note=$(clean "$note")
				continue 2
				;;
			b | B) [ "$i" -eq 0 ] || { go=back; return; } ;;
			q | Q) go=quit; return ;;
			esac
			prompt
		done
	done
}

summary() {
	local i id nt
	clear_screen
	printf '\033[1mkmscon fork: local VT demo, answers of this run\033[0m\n\n'
	for i in "${!sections[@]}"; do
		id=$(id_of "${sections[$i]}")
		nt=${notes[$i]:-}
		printf '  %-22s %-7s %s\n' "$id" "${rating[$i]:--}" "${nt:0:$((cols - 35))}"
	done
	echo
	if [ -n "$recording" ]; then
		printf '  recorded as run %s in\n  %s\n' "$run" "$feedback"
		printf '  fork/demo/feedback.sh prints them\n\n'
	else
		printf '  \033[31mnot recorded: %s\033[0m\n\n' "$why_not"
	fi
	page_end 'press a key to finish'
}

sections=()
for f in "$here"/sections/*.sh; do
	if [ $# -eq 0 ]; then
		sections+=("$f")
	else
		for want in "$@"; do
			[ "$(name_of "$f")" != "$want" ] || sections+=("$f")
		done
	fi
done
[ ${#sections[@]} -gt 0 ] || { echo "demo: no such section: $*" >&2; exit 2; }

[ -n "$noprompt" ] || init_feedback
read_size

# Echo stays off for the whole run, so neither a key typed between prompts nor
# the terminal's answer to cursor_row lands on a page. The note's line editor
# echoes for itself.
if [ -z "$nopage" ] && tty_mode=$(stty -g </dev/tty 2>/dev/null); then
	trap 'stty "$tty_mode" </dev/tty' EXIT
	stty -echo </dev/tty
fi

clear_screen
printf '\033[1mkmscon fork: local VT demo\033[0m\n\n'
printf '  running   %s\n' "${DEMO_LABEL:-unlabelled (DEMO_LABEL not set; run-vt.sh sets it for the dev build)}"
printf '  terminal  TERM=%s, %s rows by %s columns\n\n' "${TERM:-unset}" "$rows" "$cols"
printf '  Below each page: what a right and a wrong screen look like.\n'
if [ -z "$noprompt" ]; then
	printf '  Then one key: 1 looks right, 2 looks wrong, 3 cannot tell,\n'
	printf '  n types a note first, b goes back a page, q quits.\n'
	if [ -n "$recording" ]; then
		printf '  Answers go to %s.\n' "$feedback"
	else
		printf '  \033[31mAnswers are not recorded: %s.\033[0m\n' "$why_not"
	fi
fi
echo
i=0
for f in "${sections[@]}"; do
	i=$((i + 1))
	printf '  %2d  %s\n' "$i" "$(name_of "$f")"
done
echo
page_end 'press a key for the first section, q to quit'

if [ -n "$noprompt" ]; then
	for i in "${!sections[@]}"; do
		draw_page "$i"
		footer_lines "$i"
		printf '%s\n' "${foot[@]}"
		if [ "$i" -lt $((${#sections[@]} - 1)) ]; then
			page_end 'press a key for next, q to quit'
		else
			page_end 'end of the demo: press a key to finish'
		fi
	done
	[ -n "$nopage" ] || clear_screen
	exit 0
fi

i=0
while [ "$i" -lt ${#sections[@]} ]; do
	ask "$i"
	case $go in
	next) i=$((i + 1)) ;;
	back) i=$((i - 1)) ;;
	quit) break ;;
	esac
done
summary
clear_screen
