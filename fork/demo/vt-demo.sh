#!/bin/bash
# The local VT demo in one blocking command, as `make vt-demo` runs it (see
# docs/fork/demo.md): picks a free VT, runs run-vt.sh there through sudo, and
# when the demo ends prints this run's answers, the results file, and a health
# check of kmscon's log.
#
#   vt-demo.sh [N] [-- KMSCON-OPTION...]
#
# Without N it takes the first free VT of 5, 6, 4, 3 and 2. A VT is free when
# it is not on screen (/sys/class/tty/tty0/active) and no logind session is on
# it; run-vt.sh refuses the same VTs again before it stops anything.
#
# VT_DEMO_DRY_RUN=1 picks the VT and prints the run-vt.sh command without
# running it. For the tests, VT_DEMO_ACTIVE, VT_DEMO_LOGINCTL, VT_DEMO_SUDO
# (empty: no sudo), VT_DEMO_RUN_VT and VT_DEMO_FEEDBACK replace the sysfs file,
# loginctl, sudo, run-vt.sh and the feedback file.
set -euo pipefail

usage() {
	sed -n '7,7s/^# \{0,1\}//p' "$0" >&2
	exit 2
}

die() {
	printf 'vt-demo: %s\n' "$*" >&2
	exit 1
}

vt=
extra=()
while [ $# -gt 0 ]; do
	case $1 in
	-h | --help) usage ;;
	--) shift; extra=("$@"); break ;;
	*) [ -z "$vt" ] || usage; vt=$1 ;;
	esac
	shift
done

here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
dry=${VT_DEMO_DRY_RUN:-}
active_file=${VT_DEMO_ACTIVE:-/sys/class/tty/tty0/active}
loginctl=${VT_DEMO_LOGINCTL:-loginctl}
sudo=${VT_DEMO_SUDO-sudo}
run_vt=${VT_DEMO_RUN_VT:-$here/run-vt.sh}
# run-vt.sh starts the demo with a cleared environment, so the dev build's
# answers always go to demo.sh's default file; DEMO_FEEDBACK would not reach it.
feedback=${VT_DEMO_FEEDBACK:-/tmp/kmscon-demo/feedback.tsv}

# The VT on screen now: the desktop's, since this runs from it.
active=$(cat "$active_file") || die "cannot read $active_file, so the VT on screen is unknown"
active=${active#tty}

# One "VT session (name type)" entry per logind session that sits on a VT.
sessions=()
list=$("$loginctl" list-sessions --no-legend) || die "$loginctl list-sessions failed"
while read -r s _; do
	[ -n "$s" ] || continue
	n=$("$loginctl" show-session -p VTNr --value "$s")
	[ -n "$n" ] && [ "$n" != 0 ] || continue
	sessions+=("$n $s ($("$loginctl" show-session -p Name -p Type --value "$s" | paste -sd' '))")
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
	[[ $vt =~ ^[0-9]+$ ]] && [ "$vt" -ge 2 ] && [ "$vt" -le 12 ] ||
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

cmd=("$run_vt" "$vt" -- "${extra[@]}")
[ -z "$sudo" ] || cmd=("$sudo" "${cmd[@]}")
log=${feedback%/*}/vt-demo-$(date +%Y%m%dT%H%M%S)-tty$vt.log

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
[ -d "${feedback%/*}" ] || mkdir -m 0755 -- "${feedback%/*}" || die "cannot create ${feedback%/*}"
: >"$log" || die "cannot write $log"

# Answers appended from here on belong to this run.
before=0
[ ! -f "$feedback" ] || before=$(stat -c %s "$feedback")

banner
# Ctrl-C here reaches run-vt.sh, whose trap restores the VT; this script and
# tee stay to log that and print the summary.
trap 'echo "vt-demo: interrupted" >&2' INT
set +e
"${cmd[@]}" 2>&1 | (trap '' INT; exec tee "$log")
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
echo "kmscon log: $log"
echo

[ -z "$run" ] || "$here/feedback.sh" --file "$feedback" --run "$run"
echo
echo "results: $(readlink -m "$feedback")"

[ "$status" -eq 0 ] || die "run-vt.sh exited with status $status: kmscon failed or the demo was cut short"
[ -n "$run" ] || die "no answers from this run in $feedback: the demo did not run, or q was pressed before any page was rated"
