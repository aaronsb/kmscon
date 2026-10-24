#!/bin/bash
# kit: sixel
# look-for: The sound outputs this terminal reaches, numbered, and 0 for none.
# look-for: A number and Enter plays a one-second tone there, and y keeps it
# look-for: for mpv-sixel. With nobody asked (tier 3), one line: no sound.
# wrong-looks-like: No tone from an output that plays on the desktop, or the
# wrong-looks-like: wired outputs missing: the dev build's VT is no logind
# wrong-looks-like: session, so they are out of its reach (demo.md, Sound).
# The VT demo's sound check, before mpv-sixel, the one page that plays sound.
# It lists the outputs mpv can play through from this terminal (mpv
# --audio-device=help): the sinks of the user's sound server, PipeWire or
# PulseAudio, when one answers, else the ALSA cards' own devices. The
# operator picks one by number, hears a one-second tone through it and keeps
# it, or picks 0 for no sound. The choice holds for this pass of the demo,
# in the file audio_file names (acceptance.inc), which mpv-sixel reads. With
# nobody to ask (tier 3, DEMO_NOPROMPT or DEMO_NOPAGE, input that is not a
# terminal) or no runtime directory to keep the choice in, it asks nothing,
# and the video plays without sound.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/acceptance.inc"

printf '%s[1mSound for the video pages%s[0m\n\n' "$esc" "$esc"
if [ -n "${T3_EVENTS:-}${DEMO_NOPROMPT:-}${DEMO_NOPAGE:-}" ] || [ ! -t 0 ]; then
	printf 'Nobody is asked here (tier 3, DEMO_NOPROMPT or no terminal), so\n'
	printf 'mpv-sixel plays without sound.\n'
	exit 0
fi
if ! file=$(audio_file); then
	printf 'XDG_RUNTIME_DIR names no directory of this user, so no sound server is\n'
	printf 'reachable and the choice has nowhere to be kept: mpv-sixel plays without\n'
	printf 'sound.\n'
	exit 0
fi
needs mpv mpv
term_size

# A new choice each time the page is drawn; and the choices of passes whose
# demo.sh has ended go.
rm -f -- "$file"
for f in "${file%.*}".*; do
	pid=${f##*.}
	[[ $pid =~ ^[0-9]+$ ]] && ! kill -0 "$pid" 2>/dev/null && rm -f -- "$f"
done

# The outputs, as mpv names them (names) with their descriptions (descs):
# the sound server's, PipeWire's before PulseAudio's, each with the server's
# default first; without a server, each ALSA card's default device and its
# HDMI outputs.
all=()
mapfile -t all < <(mpv --no-config --audio-device=help 2>/dev/null |
	sed -n "s/^  '\\(.*\\)' (\\(.*\\))\$/\\1\\t\\2/p")
names=() descs=() via=
for want in 'pipewire pipewire/*' 'pulse pulse/*' 'alsa/sysdefault:CARD=* alsa/hdmi:CARD=*'; do
	for line in "${all[@]}"; do
		for pat in $want; do
			# shellcheck disable=SC2053 # the patterns are globs
			[[ ${line%%$'\t'*} == $pat ]] || continue
			names+=("${line%%$'\t'*}")
			descs+=("${line#*$'\t'}")
		done
	done
	[ ${#names[@]} -eq 0 ] || { via=${want%% *}; break; }
done
case $via in
pipewire) via='the sound server, PipeWire' ;;
pulse) via='the sound server, PulseAudio' ;;
alsa*) via='ALSA, as no sound server answers' ;;
*)
	printf 'mpv finds no sound output from this terminal, so mpv-sixel plays\n'
	printf 'without sound. docs/fork/demo.md, Sound, says why one can be missing.\n'
	exit 0
	;;
esac

printf 'Where should mpv-sixel play its sound? Outputs of %s:\n\n' "$via"
printf '  %2d  no sound\n' 0
for i in "${!names[@]}"; do
	printf '  %2d  %s\n' $((i + 1)) "${descs[$i]:0:$((cols - 7))}"
done
printf '\nAn output missing here is out of this VT'"'"'s reach: docs/fork/demo.md, Sound.\n\n'

# The number typed, then Enter, into choice: from 0 to the last output.
pick() {
	local key num=
	printf 'number, then Enter: '
	while :; do
		IFS= read -rsn1 key || exit 0
		case $key in
		[0-9]) [ ${#num} -ge 3 ] || { num+=$key; printf '%s' "$key"; } ;;
		$'\177' | $'\b') [ -z "$num" ] || { num=${num%?}; printf '\b \b'; } ;;
		'')
			[ -n "$num" ] && [ $((10#$num)) -le ${#names[@]} ] && break
			num=
			printf '\nfrom 0 to %d, then Enter: ' ${#names[@]}
			;;
		# The rest of an escape sequence (an arrow key) is not a key.
		$'\033') while IFS= read -rsn1 -t 0.05 _; do :; done ;;
		esac
	done
	printf '\n'
	choice=$((10#$num))
}

# One second of 660 Hz through output $1; fails when mpv cannot open it.
tone() {
	timeout 10 mpv --no-config --really-quiet --no-video --no-input-terminal \
		--audio-device="$1" 'av://lavfi:sine=frequency=660:duration=1' >/dev/null 2>&1
}

while :; do
	pick
	if [ "$choice" -eq 0 ]; then
		printf 'No sound: mpv-sixel plays without it.\n'
		break
	fi
	dev=${names[choice - 1]} desc=${descs[choice - 1]}
	printf 'A one-second tone through %s ...\n' "$desc"
	if ! tone "$dev"; then
		printf 'mpv could not play through it. Pick another, or 0 for no sound.\n'
		continue
	fi
	printf 'Did you hear it?  y keep it  n pick another  t the tone again '
	while IFS= read -rsn1 key || exit 0; do
		case $key in
		y | Y | n | N) break ;;
		t | T) tone "$dev" ;;
		esac
	done
	printf '%s\n' "$key"
	case $key in
	y | Y)
		(umask 077 && printf '%s\n%s\n' "$dev" "$desc" >"$file")
		printf 'mpv-sixel plays its sound through %s.\n' "$desc"
		break
		;;
	esac
done
