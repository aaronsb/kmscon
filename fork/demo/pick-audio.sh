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
# server, each ALSA card's default and HDMI devices. A server with no sink
# but its dummy (auto_null) lists none. A number, or a device name typed in
# its place (any that mpv --audio-device=help lists), plays a one-second
# tone through one, y keeps it, t plays it again, and 0 is no sound. When the
# sound cards are out of reach, as on a VT that is no logind session for a
# user outside the audio group (#171), it says so first, with the fix. When
# it finds no output, one line says why, and a device name can be typed.
set -u

wait_key=1
[ "${1:-}" != --no-wait ] || wait_key=
# The sound cards' directory and /proc; the tests point them elsewhere.
snd=${PICK_AUDIO_SND:-/dev/snd}
proc=${PICK_AUDIO_PROC:-/proc}
if [ -n "${AUDIO_DEVICE:-}" ]; then
	printf '%s\n' "$AUDIO_DEVICE"
	exit 0
fi
exec 3>&1 1>&2

# Who can open the sound cards. On the VT run-vt.sh gives a demo, the session
# is no logind session, so while it is on screen logind lets nobody open the
# cards (/dev/snd) through their ACLs, and PipeWire, which opens them for the
# user, drops them; only the audio group opens them then. A process holds the
# groups its login had when it started, so joining the group takes a new
# login, and PipeWire's, which the user's service manager starts, takes a
# logout of every session. cards: there are cards; open: this script opens
# one; gid: the audio group's; in_login: this login is in it; in_etc:
# /etc/group lists the user in it; pw: PipeWire's PID; pw_audio: PipeWire is
# in it.
me=$(id -un)
cards='' open=''
for c in "$snd"/controlC*; do
	[ -e "$c" ] || break
	cards=1
	[ -r "$c" ] && [ -w "$c" ] && open=1
done
gid=$(getent group audio | cut -d: -f3) || true
in_login='' in_etc='' pw_audio=''
if [ -n "$gid" ]; then
	! id -G | tr ' ' '\n' | grep -qx "$gid" || in_login=1
	! getent group audio | cut -d: -f4 | tr ',' '\n' | grep -qx "$me" || in_etc=1
fi
pw=$(pgrep -u "$(id -u)" -x pipewire | head -1) || true  # none: PulseAudio
[ -z "$pw" ] || [ -z "$gid" ] ||
	! sed -n 's/^Groups:\t*//p' "$proc/$pw/status" | tr ' ' '\n' | grep -qx "$gid" || pw_audio=1

# Says why the sound cards may be missing from the list, and the fix. $1 says
# who opens them: a sound server, or ALSA here (alsa/...).
cards_note() {
	local ok=$open
	[ -n "$cards" ] && [ -n "$gid" ] || return 0  # no cards, or no audio group to offer
	if [[ $1 != alsa* ]]; then
		# The server opens the cards: through the ACLs this script has when it
		# can open them without the audio group, or through that group, which
		# PipeWire holds only if it started after the user joined it.
		[ -z "$ok" ] || [ -n "$in_login" ] || return 0
		ok=$pw_audio
	fi
	[ -z "$ok" ] || return 0
	echo "The sound cards are out of reach here: this VT is no logind session, so"
	echo "nobody opens them through their ACLs while it is on screen; the audio group does."
	if [ -n "$in_etc" ]; then
		echo "You are in the audio group, but this login and your sound server started before"
		echo "you joined it: log out of every session (or reboot) to take it up."
	else
		echo "Fix: sudo usermod -aG audio $me, then log out of every session (or"
		echo "reboot). Every program you run then opens the sound cards directly, on every"
		echo "VT, past logind's seat rule."
	fi
	echo "Until then, pick a Bluetooth output below or 0 for no sound, or run this from"
	echo "a login on a VT (make vt-login), which holds the cards while it is on screen."
	echo
}

# Why mpv lists no output, in one line.
why_none() {
	if [ -z "$cards" ]; then
		echo "there is no sound card in $snd"
	elif [ -n "$in_etc" ] && [ -z "$in_login" ]; then
		echo "you are in the audio group in /etc/group, but this login started before you joined it: log out of every session (or reboot) and log in again"
	elif [ -n "$in_login" ] && [ -n "$pw" ] && [ -z "$pw_audio" ]; then
		echo "your PipeWire started before you joined the audio group, so it cannot open the sound cards: log out of every session (or reboot) and log in again"
	elif [ -z "$open" ] && [ -n "$gid" ] && [ -z "$in_login" ]; then
		echo "the sound cards are out of reach on a VT that is no logind session, and you are not in the audio group: sudo usermod -aG audio $me and log in again, or run this from a login on a VT (make vt-login)"
	else
		echo "no sound server's output and no ALSA card in mpv --audio-device=help"
	fi
}

all=() names=() descs=()
mapfile -t all < <(mpv --no-config --audio-device=help 2>/dev/null |
	sed -n "s/^  '\\(.*\\)' (\\(.*\\))\$/\\1\\t\\2/p")
for want in 'pipewire pipewire/*' 'pulse pulse/*' 'alsa/sysdefault:CARD=* alsa/hdmi:CARD=*'; do
	for line in "${all[@]}"; do
		name=${line%%$'\t'*}
		[[ $name != */auto_null ]] || continue  # the server's dummy sink
		# The patterns are matched, never expanded as file names.
		set -f
		for pat in $want; do
			# shellcheck disable=SC2053 # the patterns are globs
			[[ $name != $pat ]] || { names+=("$name"); descs+=("${line#*$'\t'}"); }
		done
		set +f
	done
	# A server's default with no sink of its own plays nowhere.
	[[ ${names[*]-} != */* ]] || break
	names=() descs=()
done

# mpv's device names: pipewire/alsa_output.usb-..., alsa/hdmi:CARD=HDMI,DEV=0.
is_name() {
	[[ $1 =~ ^[A-Za-z0-9_.:,=@+/-]+$ ]]
}
# mpv's description of device $1, or the name when mpv lists none.
describe() {
	local line
	for line in "${all[@]}"; do
		[ "${line%%$'\t'*}" != "$1" ] || { echo "${line#*$'\t'}"; return; }
	done
	echo "$1"
}
# Plays a one-second tone through device $1, described as $2, until the
# answer: true for y, false for anything but t, which plays it again.
heard() {
	local k
	while :; do
		echo "a one-second tone through $2 ..."
		timeout 10 mpv --no-config --really-quiet --no-video --no-input-terminal --audio-device="$1" \
			'av://lavfi:sine=frequency=660:duration=1' </dev/null >/dev/null 2>&1 ||
			echo "mpv could not play through it."
		read -rp "heard it? y keeps it, t plays it again, anything else picks another: " k || exit 1
		[[ $k == [tT] ]] || break
	done
	[[ $k == [yY] ]]
}
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

echo
if [ ${#names[@]} -eq 0 ]; then
	echo "mpv finds no sound output here: $(why_none)."
	if [ -z "$wait_key" ]; then
		said "--no-wait: it plays without sound."
		echo none >&3
		exit 0
	fi
	while :; do
		read -rp "a device name to try (mpv --audio-device=help lists them), or Enter for no sound: " n || exit 1
		if [ -z "$n" ]; then
			echo none >&3
			exit 0
		fi
		is_name "$n" || continue
		if heard "$n" "$(describe "$n")"; then
			printf '%s\n' "$n" >&3
			exit 0
		fi
	done
fi
cards_note "$want"
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
	read -rp "number (0 to ${#names[@]}) or a device name: " n || exit 1
	if [[ $n =~ ^[0-9]{1,3}$ ]]; then
		((10#$n <= ${#names[@]})) || continue
		n=$((10#$n))
		if [ "$n" = 0 ]; then
			echo none >&3
			exit 0
		fi
		dev=${names[n - 1]} desc=${descs[n - 1]}
	elif is_name "$n"; then
		dev=$n desc=$(describe "$n")
	else
		continue
	fi
	if heard "$dev" "$desc"; then
		printf '%s\n' "$dev" >&3
		exit 0
	fi
done
