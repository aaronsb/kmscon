#!/bin/bash
# Runs the kmscon staged by `make demo` on one VT, with the demo (demo.sh) as
# its only session, in place of the stock kmsconvt@ttyN unit. When the demo
# ends, or this script is interrupted, or kmscon fails, the unit is started
# again if it was running before. See docs/fork/demo.md.
#
#   sudo fork/demo/run-vt.sh [--dry-run] [N] [-- kmscon-option...]
#
# N is the VT, 2-12, default 3. Options after -- go to kmscon, for display
# settings of one machine (--mode=..., --no-use-original-mode, --font-size=...);
# options that would move kmscon to another VT, replace the demo session or its
# config, or let it switch VTs are refused.
# --dry-run prints every command and runs none; it needs no root.
#
# kmscon loads its backend modules from a path compiled in (/usr/lib/kmscon)
# with no option to change it, and would otherwise load the stock package's
# modules. kmscon therefore runs in a private mount namespace in which the
# staged modules are bind-mounted over that path; the rest of the system never
# sees the mount, and it ends with kmscon.
set -euo pipefail

usage() {
	sed -n '7,11s/^# \{0,1\}//p' "$0" >&2
	exit 2
}

die() {
	printf 'run-vt: %s\n' "$*" >&2
	exit 1
}

dry=
vt=
extra=()
while [ $# -gt 0 ]; do
	case $1 in
	--dry-run) dry=1 ;;
	-h | --help) usage ;;
	--) shift; extra=("$@"); break ;;
	[0-9]*) [ -z "$vt" ] || usage; vt=$1 ;;
	*) usage ;;
	esac
	shift
done
vt=${vt:-3}
fg=

here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
stage=$(readlink -m "$here/../build/demo-root")
kmscon=$stage/usr/bin/kmscon
modules=$stage/usr/lib/kmscon
moduledir=/usr/lib/kmscon
unit=kmsconvt@tty$vt.service

# show prints a command as it would be typed; run shows it, then runs it unless
# this is a dry run.
show() {
	printf '+'
	printf ' %q' "$@"
	printf '\n'
}
run() {
	show "$@"
	[ -n "$dry" ] || "$@"
}

[[ $vt =~ ^[1-9][0-9]*$ ]] && [ "$vt" -ge 2 ] && [ "$vt" -le 12 ] ||
	die "VT must be a number from 2 to 12, not '$vt'"
# The checks below are made for VT $vt with the demo as the session; these
# options, after the fixed ones, would override that.
for o in "${extra[@]}"; do
	case $o in
	-- | --login | --login=* | --vt | --vt=* | --configdir | --configdir=* | --switchvt | --switchvt=* | -[lc]* | -[!-]*[lc]*)
		die "kmscon option '$o' is refused: the VT, the session, the config and --no-switchvt are fixed" ;;
	esac
done
[ -n "$dry" ] || [ "$(id -u)" -eq 0 ] || die "run as root, through sudo"
[ -x "$kmscon" ] && [ -d "$modules" ] || die "nothing staged in $stage: run make demo first"
[ -d "$moduledir" ] || die "$moduledir does not exist, so the staged modules have nowhere to be mounted"

user=${SUDO_USER:-}
[ -z "$dry" ] || user=${user:-$(id -un)}
[ -n "$user" ] && [ "$user" != root ] ||
	die "run through sudo from your own account; the demo runs as that user, never as root"
uid=$(id -u "$user") || die "no such user: $user"
gid=$(id -g "$user")

# Refuse the VT on screen (the one this command was typed on) and any VT a
# logind session sits on, the graphical one included: stopping the unit there
# would end that session.
if [ -n "$dry" ]; then
	show fgconsole
	echo "  (refuse if it prints $vt)"
	show loginctl list-sessions --no-legend
	echo "  (for each session: loginctl show-session -p VTNr --value ID; refuse if one is on VT $vt)"
else
	fg=$(fgconsole) || die "fgconsole failed, so the VT on screen is unknown"
	[ "$fg" != "$vt" ] || die "VT $vt is on screen now; run this from another VT"
	for s in $(loginctl list-sessions --no-legend | awk '{print $1}'); do
		[ "$(loginctl show-session -p VTNr --value "$s")" != "$vt" ] ||
			die "logind session $s ($(loginctl show-session -p Name -p Type --value "$s" | paste -sd' ')) is on VT $vt; pick another VT"
	done
fi

was_active=
if [ -n "$dry" ]; then
	show systemctl is-active --quiet "$unit"
	echo "  (the rest assumes it is active)"
	was_active=1
elif systemctl is-active --quiet "$unit"; then
	was_active=1
fi

child=
# shellcheck disable=SC2329 # invoked by the EXIT trap
# A second signal, or a closed terminal or pipe, must not cut it short before
# the unit is started again, so it ignores them while it runs.
restore() {
	trap - EXIT
	trap '' INT TERM HUP PIPE
	if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
		run kill -TERM "$child" || true
		wait "$child" 2>/dev/null || true
	fi
	if [ -n "$was_active" ]; then
		run systemctl start "$unit" ||
			echo "run-vt: restarting $unit failed; restore it with: sudo systemctl start $unit" >&2
	fi
}
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

[ -z "$was_active" ] || run systemctl stop "$unit"

build=$(cut -d' ' -f1 "$stage/BUILD" 2>/dev/null) || build='(unknown)'
if [ -n "$was_active" ]; then
	after="when the demo ends, $unit is started again on VT $vt"
else
	after="$unit was not running, so nothing is started when the demo ends"
fi
cat <<EOF

kmscon dev build $build on VT $vt
  1. switch with Ctrl-Alt-F$vt
  2. rate each page with 1, 2 or 3 (c adds a comment, b goes back);
     press q in the demo to finish
  3. $after;
     switch back to your desktop with Ctrl-Alt-F${fg:-N}
manual restore: sudo systemctl start $unit

EOF

# The demo runs as $user, never as root. kmscon is started in the background
# so that it ignores Ctrl-C (bash does that for background jobs) and this
# script decides how it stops: SIGTERM from restore(), which kmscon handles by
# releasing the VT. unshare and sh exec, so $! is kmscon's own PID. kmscon
# starts its child with execve, without a PATH search, so its path is absolute.
# kmscon and setpriv both clear the environment, so DEMO_LABEL, which names the
# build on the demo's title page, is set last, by env.
# shellcheck disable=SC2016 # $1, $2 and $@ are for the inner sh
cmd=(unshare --mount --propagation private --
	/bin/sh -c 'mount --bind "$1" "$2" && shift 2 && exec "$@"' sh "$modules" "$moduledir"
	"$kmscon" --vt="tty$vt" --no-switchvt --configdir "$here" --oneshot "${extra[@]}"
	--login -- /usr/bin/setpriv --reuid="$uid" --regid="$gid" --init-groups --reset-env
	-- /usr/bin/env DEMO_LABEL="dev build $build" /bin/bash "$here/demo.sh")
show "${cmd[@]}"
[ -z "$dry" ] || exit 0
"${cmd[@]}" &
child=$!
status=0
wait "$child" || status=$?
child=
[ "$status" -eq 0 ] || echo "run-vt: kmscon exited with status $status" >&2
exit "$status"
