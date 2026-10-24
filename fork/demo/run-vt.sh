#!/bin/bash
# Runs the kmscon staged by `make demo` on one VT, with the demo (demo.sh) as
# its only session, in place of the stock kmsconvt@ttyN unit. When the demo
# ends, or this script is interrupted, or kmscon fails, the unit is started
# again if it was running before. See docs/fork/demo.md.
#
#   sudo fork/demo/run-vt.sh [--dry-run] [--kit=KIT | --showcase=VIDEO ...] [N] [-- kmscon-option...]
#
# N is the VT, 2-12, default 3. Options after -- go to kmscon, for display
# settings of one machine (--mode=..., --no-use-original-mode, --font-size=...).
# Only display, font, palette, text and input options are accepted, each as one
# word: --name=value, or --name / --no-name for a switch.
# --kit=KIT picks the demo's review kit (#123): text (the default), sixel,
# kitty or all.
# --showcase=VIDEO runs the Bad Apple!! showcase (fork/showcase/bad-apple.sh)
# on VIDEO, an absolute path, in place of the demo; --showcase-from=N starts it
# at act N and --showcase-audio plays the soundtrack, through an output the
# show asks for, or --showcase-audio=DEVICE through mpv's audio device DEVICE.
# --dry-run prints every command and runs none; it needs no root. A dry run
# takes the module directory from RUN_VT_MODULEDIR when set, for the tests.
#
# kmscon loads its backend modules from a path compiled in (/usr/lib/kmscon)
# with no option to change it, and would otherwise load the stock package's
# modules. kmscon therefore runs in a private mount namespace in which the
# staged modules are bind-mounted over that path; the rest of the system never
# sees the mount, and it ends with kmscon.
set -euo pipefail

usage() {
	sed -n '7,20s/^# \{0,1\}//p' "$0" >&2
	exit 2
}

die() {
	printf 'run-vt: %s\n' "$*" >&2
	exit 1
}

dry=
vt=
kit=
extra=()
video=
from=
audio=
device=
while [ $# -gt 0 ]; do
	case $1 in
	--dry-run) dry=1 ;;
	--kit=*) kit=${1#*=}; [ -n "$kit" ] || die "--kit takes text, sixel, kitty or all, not ''" ;;
	--showcase=*) video=${1#*=}; [ -n "$video" ] || die "--showcase needs the video's path: --showcase=/path/to/video" ;;
	--showcase-from=*) from=${1#*=} ;;
	--showcase-audio) audio=1 ;;
	--showcase-audio=*) audio=1; device=${1#*=} ;;
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
# Only a dry run, which mounts nothing, lets the tests point it elsewhere.
[ -z "$dry" ] || moduledir=${RUN_VT_MODULEDIR:-$moduledir}
showcase=$(readlink -m "$here/../showcase/bad-apple.sh")
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
# The checks below are made for VT $vt with the demo as the session, and the
# extra options come after the fixed ones, so only options that cannot change
# that are let through: an allowlist of exact names, since getopt_long also
# takes a unique prefix (--lo for --login). An option with a value must carry
# it in the same word: getopt_long would take a separate word as the value,
# and it moves stray words to the end of argv, into the login command. Left
# out on purpose: the VT, seat and session options, --login, --configdir,
# --oneshot, the grabs (one reboots), and options naming files (asciicast,
# issue-path, xkb-keymap, xkb-compose-file).
valued=" font-engine font-name font-size mode multi-monitor gpus term sb-size
	palette palette-black palette-red palette-green palette-yellow palette-blue
	palette-magenta palette-cyan palette-light-grey palette-dark-grey
	palette-light-red palette-light-green palette-light-yellow palette-light-blue
	palette-light-magenta palette-light-cyan palette-white palette-foreground
	palette-background underline-color strike-color grapheme-width xkb-model
	xkb-layout xkb-variant xkb-options xkb-repeat-delay xkb-repeat-rate
	dpms-timeout render-timer "
switches=" drm hwaccel use-original-mode blink bell soft-cursor mouse
	natural-scrolling force-decoration-colors backspace-delete verbose debug silent "
valued=$(tr -s ' \t\n' ' ' <<<"$valued")
switches=$(tr -s ' \t\n' ' ' <<<"$switches")
for o in "${extra[@]}"; do
	name=${o#--}
	if [[ $o == --?*=* ]]; then
		[[ $valued == *" ${name%%=*} "* ]] && continue
	elif [[ $o == --?* ]]; then
		[[ $switches == *" ${name#no-} "* ]] && continue
	fi
	die "kmscon option '$o' is refused. Accepted: display, font, palette, text and input options, one word each: --name=value (quoted as one word: --font-name=\"Monoid Nerd Font Mono\"), or --name / --no-name for a switch (see docs/fork/demo.md)"
done
# The showcase's video is a path the session opens as the user; it reaches the
# session as one word in its environment, never through a shell.
case $kit in
'' | text | sixel | kitty | all) ;;
*) die "--kit takes text, sixel, kitty or all, not '$kit'" ;;
esac
[ -z "$kit" ] || [ -z "$video" ] || die "--kit goes with the demo, not with --showcase"
if [ -n "$video" ]; then
	[[ $video == /* ]] || die "--showcase needs an absolute path, not '$video'"
	[ -z "$from" ] || [[ $from =~ ^[1-7]$ ]] || die "--showcase-from must be an act from 1 to 7, not '$from'"
	# mpv's device names: pipewire/alsa_output.usb-..., alsa/hdmi:CARD=HDMI,DEV=0.
	[[ -z $device || $device =~ ^[A-Za-z0-9_.:,=@+/-]+$ ]] ||
		die "--showcase-audio=DEVICE takes an mpv audio device name (mpv --audio-device=help), not '$device'"
elif [ -n "$from" ] || [ -n "$audio" ]; then
	die "--showcase-from and --showcase-audio go with --showcase=VIDEO"
fi
[ -n "$dry" ] || [ "$(id -u)" -eq 0 ] || die "run as root, through sudo"
[ -x "$kmscon" ] && [ -d "$modules" ] || die "nothing staged in $stage: run make demo first"
[ -d "$moduledir" ] || die "$moduledir does not exist, so the staged modules have nowhere to be mounted"

user=${SUDO_USER:-}
[ -z "$dry" ] || user=${user:-$(id -un)}
[ -n "$user" ] && [ "$user" != root ] ||
	die "run through sudo from your own account; the demo runs as that user, never as root"
uid=$(id -u "$user") || die "no such user: $user"
gid=$(id -g "$user")
drop=(/usr/bin/setpriv --reuid="$uid" --regid="$gid" --init-groups --reset-env)
if [ -n "$video" ] && [ -z "$dry" ]; then
	"${drop[@]}" -- /usr/bin/test -f "$video" -a -r "$video" ||
		die "$user cannot read the showcase video $video"
fi

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
		for _ in $(seq 50); do
			kill -0 "$child" 2>/dev/null || break
			sleep 0.1
		done
		! kill -0 "$child" 2>/dev/null || run kill -KILL "$child" || true
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
if [ -n "$video" ]; then
	what="press a key to start the showcase; q ends it at any time"
else
	what="rate each page with 1, 2 or 3 (c adds a comment, b goes back);
     press q in the demo to finish"
fi
cat <<EOF

kmscon dev build $build on VT $vt
  1. switch with Ctrl-Alt-F$vt
  2. $what
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
# build on the demo's title page, is set last, by env, with the user's runtime
# directory, where mpv finds the user's sound server. The showcase also gets
# its video, and the soundtrack's output when one was named.
session=(/usr/bin/env DEMO_LABEL="dev build $build" ${kit:+DEMO_KIT="$kit"}
	XDG_RUNTIME_DIR="/run/user/$uid" /bin/bash "$here/demo.sh")
if [ -n "$video" ]; then
	session=(/usr/bin/env SHOWCASE_LABEL="dev build $build" BAD_APPLE="$video"
		XDG_RUNTIME_DIR="/run/user/$uid" ${device:+AUDIO_DEVICE="$device"} /bin/bash "$showcase")
	[ -z "$from" ] || session+=(--from "$from")
	[ -z "$audio" ] || session+=(--audio)
fi
# shellcheck disable=SC2016 # $1, $2 and $@ are for the inner sh
cmd=(unshare --mount --propagation private --
	/bin/sh -c 'mount --bind "$1" "$2" && shift 2 && exec "$@"' sh "$modules" "$moduledir"
	"$kmscon" --vt="tty$vt" --no-switchvt --configdir "$here" --oneshot "${extra[@]}"
	--login -- "${drop[@]}" -- "${session[@]}")
show "${cmd[@]}"
[ -z "$dry" ] || exit 0
"${cmd[@]}" &
child=$!
status=0
wait "$child" || status=$?
child=
[ "$status" -eq 0 ] || echo "run-vt: kmscon exited with status $status" >&2
exit "$status"
