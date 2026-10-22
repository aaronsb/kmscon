#!/bin/bash
# The local VT demo (#36): a title page naming the build, then one page per
# section script in sections/, in file name order. Each page shows the
# section's label (its "# demo:" lines), which says what a correct screen looks
# like, then the screen the section draws.
#
#   demo.sh [SECTION...]   SECTION is a file name without the number and .sh,
#                          such as cjk; with none, every section runs
#
# DEMO_NOPAGE=1 runs every page without waiting for a key and leaves the last
# page on the screen, for checking the sections outside a VT.
set -u

here=$(cd "$(dirname "$0")" && pwd)
nopage=${DEMO_NOPAGE:-}

clear_screen() {
	printf '\033[0m\033[H\033[2J\033[3J'
}

# Waits for a key; q ends the demo.
page_end() {
	local key
	[ -z "$nopage" ] || return 0
	printf '\n\033[7m %s \033[0m' "$1"
	IFS= read -rsn1 key </dev/tty || exit 0
	case $key in q | Q) clear_screen; exit 0 ;; esac
}

name_of() {
	local n=${1##*/}
	n=${n%.sh}
	printf '%s' "${n#[0-9][0-9]-}"
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

build=$(cat "$here/../build/demo-root/BUILD" 2>/dev/null ||
	git -C "$here" describe --tags --long --dirty 2>/dev/null || echo unknown)
size=$(stty size </dev/tty 2>/dev/null || echo '? ?')

clear_screen
printf '\033[1mkmscon fork: local VT demo\033[0m\n\n'
printf '  build     %s\n' "$build"
printf '  terminal  TERM=%s, %s rows by %s columns\n\n' "${TERM:-unset}" "${size% *}" "${size#* }"
printf '  Each page names what a correct screen looks like.\n\n'
i=0
for f in "${sections[@]}"; do
	i=$((i + 1))
	printf '  %2d  %s\n' "$i" "$(name_of "$f")"
done
page_end 'press a key for the first section, q to quit'

i=0
for f in "${sections[@]}"; do
	i=$((i + 1))
	clear_screen
	printf '\033[1m[%d/%d] %s\033[0m\n' "$i" "${#sections[@]}" "$(name_of "$f")"
	sed -n 's/^# demo: //p' "$f"
	echo
	bash "$f" || printf '\n\033[31m(section exited with status %d)\033[0m\n' "$?"
	if [ "$i" -lt ${#sections[@]} ]; then
		page_end 'press a key for next, q to quit'
	else
		page_end 'end of the demo: press a key to finish'
	fi
done
[ -n "$nopage" ] || clear_screen
