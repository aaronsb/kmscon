#!/bin/bash
# The local VT demo in one blocking command, as `make vt-demo` runs it (see
# docs/fork/demo.md): picks a free VT, runs run-vt.sh there through sudo, and
# when the demo ends prints a health check of kmscon's log, this run's answers
# (the pages skipped listed again as still to review), and the results file.
#
#   vt-demo.sh [N] [-- KMSCON-OPTION...]
#
# make passes VT and DEMO_ARGS in the environment, as VT_DEMO_VT and
# VT_DEMO_ARGS, so neither is ever recipe text; VT_DEMO_ARGS is split into
# words by xargs, which honours quotes and backslashes and expands nothing.
# Without N it takes the first free VT of 5, 6, 4, 3 and 2. A VT is free when
# it is not on screen (/sys/class/tty/tty0/active) and no logind session is on
# it; run-vt.sh refuses the same VTs again before it stops anything.
#
# VT_DEMO_KIT picks the demo's review kit (#123): text (run-vt.sh's default),
# sixel, kitty, glyphs or all.
#
# VT_DEMO_SHOWCASE names a video and runs the Bad Apple!! showcase
# (fork/showcase/bad-apple.sh) in place of the demo, as make showcase does;
# VT_DEMO_SHOWCASE_FROM starts it at an act and VT_DEMO_SHOWCASE_AUDIO (yes or
# 1; no or 0 for none) plays the soundtrack, through
# VT_DEMO_SHOWCASE_AUDIO_DEVICE when set (an mpv audio device; the show asks
# otherwise), which plays it without VT_DEMO_SHOWCASE_AUDIO too. The showcase
# records no answers, so its run ends with the health check alone.
#
# VT_DEMO_LOGIN=1 (yes or 1), as make vt-login sets it, runs kmscon's own
# login prompt in place of the demo (run-vt.sh --login). Its run ends with the
# health check, and with the answers of a dev build demo run in that login,
# if any.
#
# VT_DEMO_DRY_RUN=1 picks the VT and prints the run-vt.sh command without
# running it. For the tests, VT_DEMO_ACTIVE, VT_DEMO_LOGINCTL and
# VT_DEMO_FEEDBACK replace the sysfs file, loginctl and the feedback file, and
# VT_DEMO_SUDO (empty: no sudo) and VT_DEMO_RUN_VT replace sudo and run-vt.sh.
# Those two decide what runs as root, so they count only in a dry run or with
# VT_DEMO_TEST=1, never from a stale export in the operator's shell.
set -euo pipefail

usage() {
	sed -n '7,7s/^# \{0,1\}//p' "$0" >&2
	exit 2
}

die() {
	printf 'vt-demo: %s\n' "$*" >&2
	exit 1
}

vt=${VT_DEMO_VT:-}
extra=()
session=()
case ${VT_DEMO_KIT:-} in
'' | text | sixel | kitty | glyphs | all) ;;
*) die "KIT=$VT_DEMO_KIT: text, sixel, kitty, glyphs or all" ;;
esac
[ -z "${VT_DEMO_KIT:-}" ] || [ -z "${VT_DEMO_SHOWCASE:-}" ] ||
	die "KIT goes with the demo, not with the showcase"
kit=()
[ -z "${VT_DEMO_KIT:-}" ] || kit=(--kit="$VT_DEMO_KIT")
if [ -n "${VT_DEMO_SHOWCASE:-}" ]; then
	video=$(readlink -f -- "$VT_DEMO_SHOWCASE") && [ -f "$video" ] && [ -r "$video" ] ||
		die "BAD_APPLE=$VT_DEMO_SHOWCASE is not a readable file"
	session=(--showcase="$video")
	[ -z "${VT_DEMO_SHOWCASE_FROM:-}" ] || session+=(--showcase-from="$VT_DEMO_SHOWCASE_FROM")
	# A device named is a soundtrack asked for, unless AUDIO says no.
	audio=${VT_DEMO_SHOWCASE_AUDIO:-}
	[ -n "$audio" ] || [ -z "${VT_DEMO_SHOWCASE_AUDIO_DEVICE:-}" ] || audio=1
	case $audio in
	yes | 1) session+=(--showcase-audio${VT_DEMO_SHOWCASE_AUDIO_DEVICE:+="$VT_DEMO_SHOWCASE_AUDIO_DEVICE"}) ;;
	no | 0 | '')
		[ -z "${VT_DEMO_SHOWCASE_AUDIO_DEVICE:-}" ] ||
			echo "vt-demo: AUDIO_DEVICE=$VT_DEMO_SHOWCASE_AUDIO_DEVICE is ignored with AUDIO=$audio" >&2
		;;
	*) die "AUDIO=$VT_DEMO_SHOWCASE_AUDIO: yes or 1 plays the soundtrack, no or 0 does not" ;;
	esac
fi
login=
case ${VT_DEMO_LOGIN:-} in
'') ;;
yes | 1)
	login=1
	[ -z "${VT_DEMO_KIT:-}" ] || die "KIT goes with the demo, not with a login: DEMO_KIT=$VT_DEMO_KIT before demo.sh, once logged in"
	[ -z "${VT_DEMO_SHOWCASE:-}" ] || die "the showcase goes with make showcase, or by hand once logged in, not with a login"
	session=(--login)
	;;
*) die "VT_DEMO_LOGIN=$VT_DEMO_LOGIN: yes or 1 runs a login prompt" ;;
esac
if [ -n "${VT_DEMO_ARGS:-}" ]; then
	printf '%s' "$VT_DEMO_ARGS" | xargs -r printf '%s\0' >/dev/null ||
		die "DEMO_ARGS cannot be split into words: $VT_DEMO_ARGS"
	mapfile -d '' -t extra < <(printf '%s' "$VT_DEMO_ARGS" | xargs -r printf '%s\0')
fi
while [ $# -gt 0 ]; do
	case $1 in
	-h | --help) usage ;;
	--) shift; extra+=("$@"); break ;;
	*) [ -z "$vt" ] || usage; vt=$1 ;;
	esac
	shift
done

here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
dry=${VT_DEMO_DRY_RUN:-}
active_file=${VT_DEMO_ACTIVE:-/sys/class/tty/tty0/active}
loginctl=${VT_DEMO_LOGINCTL:-loginctl}
sudo=sudo
run_vt=$here/run-vt.sh
if [ -n "$dry" ] || [ "${VT_DEMO_TEST:-}" = 1 ]; then
	sudo=${VT_DEMO_SUDO-sudo}
	run_vt=${VT_DEMO_RUN_VT:-$run_vt}
fi
# run-vt.sh starts the demo with a cleared environment, so the dev build's
# answers always go to demo.sh's default file; DEMO_FEEDBACK would not reach it.
feedback=${VT_DEMO_FEEDBACK:-/tmp/kmscon-demo/feedback.tsv}

# The VT on screen now: the desktop's, since this runs from it.
active=$(cat "$active_file") || die "cannot read $active_file, so the VT on screen is unknown"
active=${active#tty}

# One "VT session (name type)" entry per logind session that sits on a VT. A
# session that ends between the list and the query is gone, so it is skipped.
sessions=()
list=$("$loginctl" list-sessions --no-legend) || die "$loginctl list-sessions failed"
while read -r s _; do
	[ -n "$s" ] || continue
	n=$("$loginctl" show-session -p VTNr --value "$s" 2>/dev/null) || continue
	[ -n "$n" ] && [ "$n" != 0 ] || continue
	what=$("$loginctl" show-session -p Name -p Type --value "$s" 2>/dev/null | paste -sd' ') || continue
	sessions+=("$n $s ($what)")
done <<<"$list"

# Prints why VT $1 is refused, or nothing when it is free.
why_busy() {
	local e
	[ "$1" != "$active" ] || { echo "it is on screen"; return; }
	for e in "${sessions[@]}"; do
		[ "${e%% *}" != "$1" ] || { echo "logind session ${e#* } is on it"; return; }
	done
}

if [ -n "$vt" ]; then
	[[ $vt =~ ^[1-9][0-9]*$ ]] && [ "$vt" -ge 2 ] && [ "$vt" -le 12 ] ||
		die "VT must be a number from 2 to 12, not '$vt'"
	why=$(why_busy "$vt")
	[ -z "$why" ] || die "VT $vt refused: $why"
else
	refused=
	for v in 5 6 4 3 2; do
		why=$(why_busy "$v")
		[ -n "$why" ] || { vt=$v; break; }
		refused+=$'\n'"  VT $v: $why"
	done
	[ -n "$vt" ] || die "no free VT among 5, 6, 4, 3 and 2; pick one with VT=N:$refused"
fi

if [[ $active =~ ^[0-9]+$ ]]; then
	back="Ctrl-Alt-F$active returns to this desktop"
else
	back="your desktop's Ctrl-Alt-F key returns to it"
fi

cmd=("$run_vt" "${kit[@]}" "${session[@]}" "$vt" -- "${extra[@]}")
[ -z "$sudo" ] || cmd=("$sudo" "${cmd[@]}")
dir=${feedback%/*}
log=$dir/vt-demo-$(date +%Y%m%dT%H%M%S)-tty$vt-XXXXXX.log

banner() {
	local rule
	rule=$(printf '=%.0s' {1..64})
	printf '\n%s\n  Switch to VT %s now: Ctrl-Alt-F%s    (%s)\n%s\n\n' \
		"$rule" "$vt" "$vt" "$back" "$rule"
}

if [ -n "$dry" ]; then
	banner
	printf '+'
	printf ' %q' "${cmd[@]}"
	printf '\n'
	echo "  (kmscon's log would go to $log; the answers are read from $feedback)"
	exit 0
fi

# Ask for the password, if sudo wants one, before the operator leaves for the VT.
[ -z "$sudo" ] || "$sudo" -v || die "sudo failed"
# The log goes only in a directory of the invoking user's own, under a new
# name mktemp creates (never a file already there).
[ -e "$dir" ] || [ -L "$dir" ] || mkdir -m 0700 -- "$dir" || die "cannot create $dir"
[ ! -L "$dir" ] && [ -d "$dir" ] && [ -O "$dir" ] ||
	die "$dir is a symlink, or not a directory of yours; remove it and run again"
log=$(mktemp --suffix=.log "${log%.log}") || die "cannot create a log in $dir"

# Answers appended from here on belong to this run.
before=0
[ ! -f "$feedback" ] || before=$(stat -c %s "$feedback")

banner
# Ctrl-C, a closed terminal or SIGTERM reaches run-vt.sh, whose trap restores
# the VT. tee ignores them and keeps reading, so the restore never writes into
# a closed pipe; on Ctrl-C this script stays to print the summary.
trap 'echo "vt-demo: interrupted" >&2' INT
set +e
"${cmd[@]}" 2>&1 | (trap '' INT HUP TERM; exec tee -p -a "$log")
status=${PIPESTATUS[0]}
set -e
trap - INT

# The dev build's last run among the answers added since the start, by its run
# id: a stock run on another VT may have appended to the same file meanwhile.
run=
if [ -f "$feedback" ]; then
	[ "$(stat -c %s "$feedback")" -ge "$before" ] || before=0
	run=$(tail -c "+$((before + 1))" "$feedback" |
		awk -F'\t' 'NF >= 7 && $1 != "time" && index($3, "dev build ") == 1 { r = $2 } END { print r }')
fi

commits=$(grep -c 'atomic commit failed' "$log" || true)
timeouts=$(grep -c 'vt_timeout' "$log" || true)
echo
if [ "$commits" -eq 0 ] && [ "$timeouts" -eq 0 ]; then
	echo "health: 0 'atomic commit failed', 0 'vt_timeout' lines in kmscon's log"
else
	echo "health: $commits 'atomic commit failed', $timeouts 'vt_timeout' lines in kmscon's log: the stock-kmscon freeze signature"
fi
# kmscon's exit warning when a terminal's screen never left the image budget
budget=$(grep -c 'still in the image budget' "$log" || true)
if [ "$budget" -eq 0 ]; then
	echo "health: 0 'still in the image budget' lines in kmscon's log"
else
	echo "health: $budget 'still in the image budget' lines in kmscon's log: a terminal was freed without leaving the image budget"
fi
echo "kmscon log: $log"
echo

# The demo's answers; a login's, when a demo was run in it.
if [ ${#session[@]} -eq 0 ] || { [ -n "$login" ] && [ -n "$run" ]; }; then
	[ -z "$run" ] || "$here/feedback.sh" --file "$feedback" --run "$run"
	echo
	echo "results: $(readlink -m "$feedback")"
fi

# A login ends at its logout or at Ctrl-C (130); either is a normal end.
if [ -n "$login" ] && [ "$status" -eq 130 ]; then
	echo "vt-login: ended with Ctrl-C"
	status=0
fi
if [ "$status" -ne 0 ]; then
	echo "vt-demo: run-vt.sh exited with status $status: kmscon failed or the demo was cut short" >&2
	exit "$status"
fi
[ ${#session[@]} -eq 0 ] || exit 0
[ -n "$run" ] || die "no answers from this run in $feedback: the demo did not run, or q was pressed before any page was rated"
