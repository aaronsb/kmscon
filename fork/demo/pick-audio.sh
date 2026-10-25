#!/bin/bash
# Asks where a demo's sound plays, and prints mpv's name for the output on
# stdout, or none for no sound; the questions go to the terminal on stderr
# and stdin. The Bad Apple!! showcase runs it before act 1 when --audio is
# given (fork/showcase/README.md, Sound).
#
#   pick-audio.sh            lists the outputs and asks
#   pick-audio.sh --no-wait  lists them and asks nothing: mpv's default
#                            output (auto), or none when it lists none
#   AUDIO_DEVICE=NAME ...    prints NAME (or none) and asks nothing
#
# It lists the outputs mpv --audio-device=help finds: the sound server's,
# PipeWire's before PulseAudio's, the server's default first; with no
# server, each ALSA card's default and HDMI devices. A number plays a
# one-second tone through one, y keeps it, t plays it again, and 0 is no
# sound. When the sound cards are out of reach, as on a VT that is no logind
# session for a user outside the audio group (#171), it says so first, with
# the fix.
set -u

wait_key=1
[ "${1:-}" != --no-wait ] || wait_key=
# The sound cards' directory; the tests point it elsewhere.
snd=${PICK_AUDIO_SND:-/dev/snd}
if [ -n "${AUDIO_DEVICE:-}" ]; then
	printf '%s\n' "$AUDIO_DEVICE"
	exit 0
fi
exec 3>&1 1>&2

# Says why the sound cards may be missing from the list, and the fix. On the
# VT run-vt.sh gives a demo, the session is no logind session, so while it is
# on screen logind lets nobody open the cards (/dev/snd) through their ACLs,
# and PipeWire, which opens them for the user, drops them; only the audio
# group opens them then. $1 says who opens them: a sound server, or ALSA
# here (alsa/...).
cards_note() {
	local c gid pw ok=
	for c in "$snd"/controlC*; do
		[ -e "$c" ] || return 0
		[ -r "$c" ] && [ -w "$c" ] && ok=1
	done
	gid=$(getent group audio | cut -d: -f3) || true
	[ -n "$gid" ] || return 0  # no audio group to offer
	if [[ $1 != alsa* ]]; then
		# The server opens the cards: through the ACLs this script has when it
		# can open them without the audio group, or through that group, which
		# PipeWire holds only if it started after the user joined it.
		pw=$(pgrep -u "$(id -u)" -x pipewire | head -1) || true  # none: PulseAudio
		if [ -n "$ok" ] && ! id -G | tr ' ' '\n' | grep -qx "$gid"; then
			return 0
		fi
		ok=
		[ -z "$pw" ] || ! sed -n 's/^Groups:\t*//p' "/proc/$pw/status" | tr ' ' '\n' | grep -qx "$gid" || ok=1
	fi
	[ -z "$ok" ] || return 0
	echo "The sound cards are out of reach here: this VT is no logind session, so"
	echo "nobody opens them through their ACLs while it is on screen; the audio group does."
	if getent group audio | cut -d: -f4 | tr ',' '\n' | grep -qx "$(id -un)"; then
		echo "You are in the audio group, but this login and your sound server started before"
		echo "you joined it: log out of every session (or reboot) to take it up."
	else
		echo "Fix: sudo usermod -aG audio $(id -un), then log out of every session (or"
		echo "reboot). Every program you run then opens the sound cards directly, on every"
		echo "VT, past logind's seat rule."
	fi
	echo "Until then, pick a Bluetooth output below, or 0 for no sound."
	echo
}

all=() names=() descs=()
mapfile -t all < <(mpv --no-config --audio-device=help 2>/dev/null |
	sed -n "s/^  '\\(.*\\)' (\\(.*\\))\$/\\1\\t\\2/p")
for want in 'pipewire pipewire/*' 'pulse pulse/*' 'alsa/sysdefault:CARD=* alsa/hdmi:CARD=*'; do
	for line in "${all[@]}"; do
		# The patterns are matched, never expanded as file names.
		set -f
		for pat in $want; do
			# shellcheck disable=SC2053 # the patterns are globs
			[[ ${line%%$'\t'*} != $pat ]] || { names+=("${line%%$'\t'*}"); descs+=("${line#*$'\t'}"); }
		done
		set +f
	done
	[ ${#names[@]} -eq 0 ] || break
done
echo
cards_note "$want"
# An answer given without a question stays on screen a moment, or until a
# key, before the caller takes the screen.
said() {
	echo "$1"
	if [ -n "$wait_key" ]; then
		read -rp "press Enter to go on " _ || true
	else
		sleep 2
	fi
}
if [ ${#names[@]} -eq 0 ]; then
	said "mpv finds no sound output here, so it plays without sound."
	echo none >&3
	exit 0
fi
if [ -z "$wait_key" ]; then
	said "--no-wait: nothing is asked, and the sound plays through mpv's default output (AUDIO_DEVICE names another)."
	echo auto >&3
	exit 0
fi
echo "Where should the sound play?"
printf '  %2d  no sound\n' 0
for i in "${!names[@]}"; do
	printf '  %2d  %s\n' $((i + 1)) "${descs[i]}"
done
while :; do
	read -rp "number (0 to ${#names[@]}): " n || exit 1
	[[ $n =~ ^[0-9]{1,3}$ ]] || continue
	((10#$n <= ${#names[@]})) || continue
	n=$((10#$n))
	if [ "$n" = 0 ]; then
		echo none >&3
		exit 0
	fi
	dev=${names[n - 1]}
	while :; do
		echo "a one-second tone through ${descs[n - 1]} ..."
		timeout 10 mpv --no-config --really-quiet --no-video --no-input-terminal --audio-device="$dev" \
			'av://lavfi:sine=frequency=660:duration=1' </dev/null >/dev/null 2>&1 ||
			echo "mpv could not play through it."
		read -rp "heard it? y keeps it, t plays it again, anything else picks another: " k || exit 1
		[[ $k == [tT] ]] || break
	done
	if [[ $k == [yY] ]]; then
		printf '%s\n' "$dev" >&3
		exit 0
	fi
done
