#!/bin/bash
# The Bad Apple!! showcase (#75): a demoscene homage that plays Bad Apple!!
# through each rendering layer a terminal has, each act running into the limit
# of the last. See fork/showcase/README.md.
#
#   BAD_APPLE=/path/to/video bad-apple.sh [--act N | --from N] [--audio] [--no-wait]
#   bad-apple.sh --list | --dry-run [--act N | --from N] | --prepare | --fx NAME | --act 7 --banner-only
#
# --list prints the acts and effects and what each needs; --dry-run prints
# the plan and the tools each act would use, and runs nothing. --prepare
# renders the frame caches of the video acts ahead of a run (make showcase
# does it before it takes the VT). --fx runs one effect alone (--list names
# them) and reports its frame rate and bytes per frame. --banner-only makes
# act 7 the scroller alone, which needs no video. --audio plays the
# soundtrack with mpv, through an output it asks for first (AUDIO_DEVICE
# names one and skips the question). --no-wait starts without waiting for a
# key. The cue sheet below sets where each video act starts and stops.
#
# The video is never downloaded: BAD_APPLE names the operator's own copy.
# q or Ctrl-C ends the show at any point and restores the terminal.
set -euo pipefail

# The cue sheet: where each video item starts in the video, and so in its
# soundtrack, as m:ss. Each plays until the next cue: act 1's outlines, its
# Braille pass, act 3, act 5 (its last ACT5_GLITCH seconds glitched), then
# act 7 until the end card's cue (delete that line to play act 7 to the end
# of the video). Acts 2, 4 and 6 play between them, silent, for the lengths
# set below, and the soundtrack picks up again at the next cue, so the times
# are the video's, not the show's. Act 7's three moves are cues too: its one
# copy slides left (act7-slide), a second copy slides out from behind it
# (act7-copy2), then a third (act7-copy3); delete a move's line and it never
# comes. Seconds may carry a fraction (0:54.5). Edit the times here.
CUES="
0:00 act1-ascii
0:20 act1-braille
0:30 act3-aalib
0:38 act5-sixel
0:46 act7-kitty
0:54 act7-slide
1:02 act7-copy2
1:10 act7-copy3
1:46 endcard
"
# The time of cue $1 in seconds; with $2, the seconds from $1 to $2 (0 when
# there is no cue $2, or with a third argument, that argument).
cue() {
	awk -v a="$1" -v b="${2:-}" -v none="${3:-0}" 'function s(x, t) { split(x, t, ":"); return t[1] * 60 + t[2] }
		$2 == a { x = s($1) } b != "" && $2 == b { y = s($1); found = 1 }
		END { print (b == "" ? x : (found ? y - x : none)) }' <<<"$CUES"
}

# Pacing, in seconds unless marked. Each can be set in the environment too,
# the cue sheet's included.
: "${START_WAIT:=1}"             # wait for a key before the first act (0: no)
: "${TITLE_SECONDS:=0}"          # starfield title card before act 1 (0: none)
: "${ACT_GAP:=0.6}"              # black between acts

: "${ACT1_START:=$(cue act1-ascii)}"   # stick figures: from this point of the video
: "${ACT1_SECONDS:=$(cue act1-ascii act1-braille)}"  #   for this long
: "${ACT1_FPS:=12}"              #   at this frame rate
: "${ACT1_ROWS:=27}"             #   on a grid this many rows tall (columns follow the video)
: "${ACT1_FILL:= }"              #   the character inside a shape (space: outlines only)
: "${ACT1_DOTS_SECONDS:=$(cue act1-braille act3-aalib)}"  #   then a Braille pass, this long (0: none)
: "${ACT1_DOTS_FPS:=24}"
: "${ACT1_DOTS_ROWS:=52}"        #   this many rows tall, at most

: "${ACT2_PAUSE:=2.5}"           # the dramatic pause on a black screen
: "${ACT2_TEXT:=we can do better!}"
: "${ACT2_LETTER:=0.14}"         #   each letter slams in after this long
: "${ACT2_HOLD:=1.5}"            #   then holds on black
: "${ACT2_PLASMA:=6}"            #   then a plasma behind it, this long
: "${ACT2_FADE:=2}"              #   fading in over this long
: "${ACT2_BOBS:=5}"              #   then shadebobs, this long
: "${ACT2_FPS:=60}"              #   frames a second at most

: "${ACT3_START:=$(cue act3-aalib)}"   # aalib (libcaca) in 80x25
: "${ACT3_SECONDS:=$(cue act3-aalib act5-sixel)}"
: "${ACT3_FPS:=15}"
: "${ACT3_DITHER:=none}"         #   img2txt -d: none, ordered2, ordered4, ordered8, random, fstein
: "${ACT3_RESIZE:=auto}"         #   auto: ask for a real 80x25 grid (CSI 8;25;80 t); no: the framed box
: "${RESIZE_WAIT:=1.5}"          #   seconds to wait for the terminal to resize

: "${ACT4_CPS:=14}"              # "wait a minute": characters per second
: "${ACT4_DOTS:=0.9}"            #   the pause after each "..."
: "${ACT4_HOLD:=2}"

: "${ACT5_GLITCH:=2}"            # sixel: plays small, its last seconds glitched (one RGB channel, a tear)
: "${ACT5_START:=$(cue act5-sixel)}"
: "${ACT5_SECONDS:=$(awk -v a="$(cue act5-sixel act7-kitty)" -v g="$ACT5_GLITCH" 'BEGIN { print a - g }')}"
: "${ACT5_SIZE:=0.5}"            #   this share of the screen's rows tall (mpv scales the clip)
: "${ACT5_WIDTH:=960}"           #   the clip is rendered this many pixels wide
: "${ACT5_DITHER:=none}"         #   mpv's --vo-sixel-dither (none keeps the frames small)
: "${ACT5_HOLD:=2.5}"            #   the halted frame stays this long

: "${ACT6_FIRE:=5}"              # the fire wipe
: "${ACT6_CPS:=16}"
: "${ACT6_CATS:=9}"              #   the cats
: "${ACT6_DENSITY:=0.55}"        #   share of the screen's cat spots filled

: "${ACT7_START:=$(cue act7-kitty)}"   # kitty graphics, full size
: "${ACT7_SECONDS:=$(cue act7-kitty endcard)}"  #   0: to the end of the video
: "${ACT7_FADE:=2}"              #   the soundtrack fades out over its last seconds (0: no fade)
: "${ACT7_SLIDE_AT:=$(cue act7-kitty act7-slide -1)}"   #   copy 1 slides left, seconds into the act (-1: never)
: "${ACT7_COPY2_AT:=$(cue act7-kitty act7-copy2 -1)}"   #   copy 2 slides out from behind it
: "${ACT7_COPY3_AT:=$(cue act7-kitty act7-copy3 -1)}"   #   copy 3 from behind copy 2
: "${ACT7_MOVE:=1.2}"            #   each slide takes this long
: "${ACT7_BOUNCE:=0.12}"         #   and overshoots by this share of its distance before it settles (0: none)
: "${ACT7_GAP:=2}"               #   columns between the copies
: "${BANNER_ROWS:=12}"           #   the scroller's rows along the bottom
: "${BANNER_FPS:=40}"            #   frames a second; it moves one column a frame
: "${BANNER_SECONDS:=0}"         #   --banner-only runs this long (0: until q)
: "${KITTY_TRANSPORT:=png}"      #   how each frame travels: raw, zlib, png or shm (fork/showcase/kitty.py)
: "${KITTY_WIDTH:=0}"            #   frames are decoded this many pixels wide (0: a copy's own size, at most 2160 tall); a number limits the player's work on a slow machine
: "${ACT7_REPORT:=}"             #   a file to append the player's frame rate to
: "${PLACEHOLDER_SECONDS:=6}"    # a placeholder (act 5 or 7) stays this long

: "${BIG_FONT:=mono12}"          # toilet fonts drawn with ▀ ▄ █
: "${BANNER_FONT:=mono9}"
: "${PROBE_TIMEOUT:=1}"          # seconds to wait for the terminal's answers
: "${SIXEL:=auto}"               # auto: ask the terminal; yes or no to force
: "${KITTY:=auto}"
: "${FX_SECONDS:=10}"            # --fx runs this long
: "${SHOWCASE_LABEL:=}"          # names the build on the title card
: "${SHOWCASE_CACHE:=${HOME:-/tmp}/.cache/kmscon-showcase}"

# The scroller of act 7: what the fork supports, one feature per entry of
# BANNER_FEATURES (separated by ;), each followed by the next emoji of
# BANNER_EMOJI. Add a feature here when it lands.
: "${BANNER_FEATURES:=styled and coloured underlines;strike;grapheme clusters;colour emoji;cell-exact box drawing;sixel;kitty graphics}"
: "${BANNER_EMOJI:=〰️ ✂️ 🐈‍⬛ 🎨 🧱 🖼️ 🐱}"
: "${BANNER_INTRO:=kmscon, the fork ✨ now with}"
: "${BANNER_OUTRO:=greetings to ZUN, Alstroemeria Records, nomico and Anira 🍎 libtsm, mpv, ffmpeg, libcaca, libsixel and toilet 💾 see you on the console 🖥️}"
END_TEXT=${END_TEXT:-"Bad Apple!! - original by ZUN (Touhou Project), arrangement by Alstroemeria Records feat. nomico, shadow-art video by Anira"}

# The act table: name, the programs it needs, what it does, and how it plays
# on kmscon today. --list and --dry-run print it; act_N runs it.
ACT_NAME=(""
	"stick figures"
	"we can do better"
	"aalib"
	"don't we have libsixel?"
	"sixel"
	"kitty and the cats"
	"kitty, full size")
ACT_NEEDS=(""
	"ffmpeg python3"
	"python3 toilet"
	"ffmpeg img2txt python3"
	"python3"
	"ffmpeg mpv python3"
	"python3"
	"ffmpeg toilet python3")
ACT_VIDEO=("" 1 0 1 0 1 0 1)
ACT_WHAT=(""
	"Bad Apple!! as crude ASCII line art: outlines traced on a ${ACT1_ROWS}-row grid at ${ACT1_FPS} fps, then a Braille pass (2x4 dots a cell) at ${ACT1_DOTS_FPS} fps"
	"black, a pause, then \"$ACT2_TEXT\" in big block letters, then a plasma and shadebobs behind them"
	"libcaca's img2txt in 80x25 at ${ACT3_FPS} fps (a real 80x25 grid where CSI 8;25;80 t takes, else a framed region), then it freezes, broken"
	"\"wait a minute... don't we have ... LIBSIXEL?\" typed over the frozen frame"
	"sixel (DA1 attribute 4): mpv --vo=sixel plays it ${ACT5_SIZE} of the screen's height, sized from the cell size the terminal reports (XTWINOPS 16), it glitches to one channel and tears, and its last frame stays; else a placeholder"
	"a fire wipe, \"hold on, we also have kitty. hold on, feeding the cats\", a crowd of colour cats"
	"kitty graphics (a=q): each frame transmitted once and placed up to three times (fork/showcase/kitty.py), one copy in the middle that slides left, then a second and a third sliding out from behind it, until the end card's cue, above a scroller of the fork's features over copper bars; else a placeholder above the scroller; --banner-only: the scroller alone")
ACT_KMSCON=(""
	"plays"
	"plays"
	"plays"
	"plays"
	"plays (sixel, ADR-103 step 3)"
	"plays"
	"plays (kitty graphics, ADR-103 step 4c)")
LAST_ACT=7

# The effects: where each plays, and what it needs. --fx NAME runs one alone.
EFFECTS=(
	"plasma|act 2, behind the letters|python3"
	"shadebobs|act 2, after the plasma|python3"
	"fire|the wipe into act 6|python3"
	"copper|act 7, under the scroller|python3"
	"stars|the title card before act 1 (TITLE_SECONDS)|python3 toilet"
	"braille|behind the end card|python3"
	"banner|act 7's scroller (--act 7 --banner-only)|python3 toilet")

# Arch Linux package of each program.
declare -A PKG=([ffmpeg]=ffmpeg [ffprobe]=ffmpeg [python3]=python [img2txt]=libcaca
	[mpv]=mpv [toilet]=toilet [figlet]=figlet [chafa]=chafa)

here=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)

die() {
	printf 'bad-apple: %s\n' "$*" >&2
	exit 1
}

usage() {
	sed -n '7,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}" >&2
	exit 2
}

# ------------------------------------------------------------------ plan

have() { command -v "$1" >/dev/null 2>&1; }

tool_line() {
	local t s=
	for t in $1; do
		if have "$t"; then
			s+=" $t"
		else
			s+=" $t (missing: pacman -S ${PKG[$t]:-$t})"
		fi
	done
	printf '%s' "${s# }"
}

list_acts() {
	local n e name where needs
	printf '%-3s %-24s %-30s %s\n' act name needs "on kmscon today"
	for ((n = 1; n <= LAST_ACT; n++)); do
		printf '%-3s %-24s %-30s %s\n' "$n" "${ACT_NAME[n]}" "${ACT_NEEDS[n]}" "${ACT_KMSCON[n]}"
	done
	echo
	printf '%-10s %-44s %s\n' effect "where it plays" needs
	for e in "${EFFECTS[@]}"; do
		IFS='|' read -r name where needs <<<"$e"
		printf '%-10s %-44s %s\n' "$name" "$where" "$needs"
	done
	echo
	echo "cue sheet (video time; CUES in bad-apple.sh):"
	sed '/^$/d; s/^/  /' <<<"$CUES"
}

video_note() {
	if [ -z "${BAD_APPLE:-}" ]; then
		echo "BAD_APPLE is unset: a run of a video act stops here (BAD_APPLE=/path/to/your/copy)"
	elif [ ! -r "$BAD_APPLE" ] || [ ! -f "$BAD_APPLE" ]; then
		echo "BAD_APPLE=$BAD_APPLE is not a readable file: a run of a video act stops here"
	else
		echo "BAD_APPLE=$BAD_APPLE"
	fi
}

# A move cued before act 7 starts gets a negative time and never comes:
# each one is named.
act7_cue_warnings() {
	local name
	for name in act7-slide act7-copy2 act7-copy3; do
		awk -v n="$name" '$2 == n { found = 1 } END { exit !found }' <<<"$CUES" || continue
		[[ $(cue act7-kitty "$name") != -* ]] ||
			echo "  warning: the cue $name comes before act7-kitty, so that move never comes"
	done
}

dry_run() {
	local n
	echo "plan: acts $first to $last$( [ "$audio" = 1 ] && echo ', with the soundtrack')"
	echo "video: $(video_note)"
	echo "cache: $SHOWCASE_CACHE"
	echo "probes: sixel=$SIXEL kitty=$KITTY (auto asks the terminal, within ${PROBE_TIMEOUT}s)"
	for ((n = first; n <= last; n++)); do
		echo
		echo "act $n: ${ACT_NAME[n]}"
		echo "  does:  ${ACT_WHAT[n]}"
		echo "  uses:  $(tool_line "${ACT_NEEDS[n]}")"
		case $n in
		1) echo "  video: ${ACT1_START}s for ${ACT1_SECONDS}s, then ${ACT1_DOTS_SECONDS}s in Braille" ;;
		3) echo "  video: ${ACT3_START}s for ${ACT3_SECONDS}s" ;;
		5) echo "  video: ${ACT5_START}s for ${ACT5_SECONDS}s, then ${ACT5_GLITCH}s glitched" ;;
		7) echo "  video: from ${ACT7_START}s $( [ "$ACT7_SECONDS" = 0 ] && echo 'to the end' || echo "for ${ACT7_SECONDS}s, then the end card")"
		   echo "  moves: slide ${ACT7_SLIDE_AT}s, copy 2 ${ACT7_COPY2_AT}s, copy 3 ${ACT7_COPY3_AT}s into the act (-1: never); frames by $KITTY_TRANSPORT, $( [ "$KITTY_WIDTH" = 0 ] && echo "a copy's own size" || echo "${KITTY_WIDTH} px wide")"
		   act7_cue_warnings ;;
		esac
		echo "  kmscon today: ${ACT_KMSCON[n]}"
	done
	echo
	echo "effects (--fx NAME):"
	local e name where needs
	for e in "${EFFECTS[@]}"; do
		IFS='|' read -r name where needs <<<"$e"
		echo "  $name: $where; uses $(tool_line "$needs")"
	done
}

# ------------------------------------------------------------- the caches

cache_dir() {
	local id
	id=$(printf '%s\n%s\n' "$(readlink -f "$BAD_APPLE")" "$(stat -c '%s %Y' "$BAD_APPLE")" | sha1sum)
	printf '%s/%s' "$SHOWCASE_CACHE" "${id:0:12}"
}

# The grids the video acts draw on: act 1's are ACT1_ROWS and ACT1_DOTS_ROWS
# tall and act 3's 80x25, each shrunk to fit a smaller terminal. --prepare,
# run before the show takes its screen, renders at the configured sizes.
fit_rows() { local r=$1; [ -z "${screen_rows:-}" ] || [ "$r" -le $((screen_rows - 2)) ] || r=$((screen_rows - 2)); echo "$r"; }
act3_size() {
	local w=80 h=25
	if [ -n "${screen_rows:-}" ]; then
		[ "$w" -le $((screen_cols - 2)) ] || w=$((screen_cols - 2))
		[ "$h" -le $((screen_rows - 4)) ] || h=$((screen_rows - 4))
	fi
	echo "$w $h"
}

act1_cache() { echo "$cache/act1-$(fit_rows "$ACT1_ROWS")r-${ACT1_FPS}fps-${ACT1_START}+${ACT1_SECONDS}.frames"; }
act1_dots_start() { awk -v a="$ACT1_START" -v b="$ACT1_SECONDS" 'BEGIN { print a + b }'; }
act1_dots_cache() { echo "$cache/act1-dots-$(fit_rows "$ACT1_DOTS_ROWS")r-${ACT1_DOTS_FPS}fps-$(act1_dots_start)+${ACT1_DOTS_SECONDS}.frames"; }
# act3_cache [W H]: the frames at that size, by default the framed region's.
act3_cache() {
	local s=${1:-} h=${2:-}
	[ -n "$s" ] && s="$s $h" || s=$(act3_size)
	echo "$cache/act3-${s/ /x}-${ACT3_FPS}fps-${ACT3_DITHER}-${ACT3_START}+${ACT3_SECONDS}.frames"
}
act5_clip() { echo "$cache/act5-${ACT5_WIDTH}px-${ACT5_START}+${ACT5_SECONDS}+${ACT5_GLITCH}.mkv"; }

prepare_act() {
	local f w h size
	case $1 in
	1)
		f=$(act1_cache)
		if [ ! -s "$f" ]; then
			echo "rendering act 1's outlines into $f"
			python3 -B "$here/fx.py" prep-outline --video "$BAD_APPLE" --start "$ACT1_START" \
				--seconds "$ACT1_SECONDS" --fps "$ACT1_FPS" --rows "$(fit_rows "$ACT1_ROWS")" --fill "$ACT1_FILL" --out "$f"
		fi
		f=$(act1_dots_cache)
		if [ "$ACT1_DOTS_SECONDS" != 0 ] && [ ! -s "$f" ]; then
			echo "rendering act 1's Braille pass into $f"
			python3 -B "$here/fx.py" prep-dots --video "$BAD_APPLE" --start "$(act1_dots_start)" \
				--seconds "$ACT1_DOTS_SECONDS" --fps "$ACT1_DOTS_FPS" --rows "$(fit_rows "$ACT1_DOTS_ROWS")" --out "$f"
		fi
		;;
	3)
		# The real 80x25 grid's frames, and the framed region's where a small
		# terminal shrinks it.
		for size in "80 25" "$(act3_size)"; do
			read -r w h <<<"$size"
			f=$(act3_cache "$w" "$h")
			[ ! -s "$f" ] || continue
			echo "rendering act 3's libcaca frames into $f"
			python3 -B "$here/fx.py" prep-caca --video "$BAD_APPLE" --start "$ACT3_START" \
				--seconds "$ACT3_SECONDS" --fps "$ACT3_FPS" --cols "$w" --rows "$h" --dither "$ACT3_DITHER" --out "$f"
		done
		;;
	5)
		f=$(act5_clip)
		[ ! -s "$f" ] || return 0
		echo "rendering act 5's clip into $f"
		# The clip: ACT5_SECONDS as it is, then ACT5_GLITCH with only the red
		# channel, shifted, and a band of it torn sideways. The soundtrack
		# runs straight through. Its last frame is the halted one.
		# The shift and the tear scale with the clip's width.
		local total w=$ACT5_WIDTH
		total=$(awk -v a="$ACT5_SECONDS" -v b="$ACT5_GLITCH" 'BEGIN { print a + b }')
		local graph="[0:v]scale=${w}:-2,format=rgb24,split[a][b];"
		graph+="[a]trim=0:${ACT5_SECONDS},setpts=PTS-STARTPTS[n];"
		graph+="[b]trim=${ACT5_SECONDS}:${total},setpts=PTS-STARTPTS,"
		graph+="lutrgb=g=0:b=0,rgbashift=rh=-$((w / 40)):rv=$((w / 160)),split[g][t];"
		graph+="[t]crop=iw:ih/6:0:ih*2/5[band];[g][band]overlay=x=$((w * 7 / 120)):y=main_h*2/5[gl];"
		graph+="[n][gl]concat=n=2:v=1:a=0,format=yuv420p[v]"
		ffmpeg -v error -nostdin -y -ss "$ACT5_START" -t "$total" \
			-i "$BAD_APPLE" -filter_complex "$graph" -map '[v]' -map '0:a?' \
			-c:v libx264 -crf 18 -c:a aac "$f.part.mkv"
		mv "$f.part.mkv" "$f"
		;;
	esac
}

# ------------------------------------------------------------ the screen

E=$'\e'
ST=$'\e\\'
fx_py=

# Runs fx.py; q there ends the show, Ctrl-C ends it with 130.
fx() {
	local st=0
	python3 -B "$fx_py" "$@" || st=$?
	case $st in
	0) ;;
	10) exit 0 ;;
	130) exit 130 ;;
	*) die "fx.py $1 failed with status $st" ;;
	esac
}

pause() { fx pause "$1"; }

clear_screen() { printf '%s[0m%s[2J%s[H' "$E" "$E" "$E"; }

# The modes a player may leave on (#88): mpv's video outputs turn on mouse
# tracking (CSI ?1003 h) and turn it off only when they exit cleanly, so a
# player that is killed leaves the terminal reporting every mouse movement.
# Every report mode and encoding, focus events and bracketed paste go off.
MODES_OFF="$E[?1000l$E[?1002l$E[?1003l$E[?1005l$E[?1006l$E[?1015l$E[?1004l$E[?2004l"

bg_pids=()
audio_pid=
ipc_dir=  # act 7's directory for mpv's IPC socket
audio_started=  # when audio_start last started the soundtrack (EPOCHREALTIME)
notes_file=  # what the players said on stderr, printed once the screen is back

# The keys read while a player runs, one byte at a time: q ends the show.
# While mpv has mouse tracking on, the terminal sends reports as input: X10
# (ESC [ M and three bytes, any of which can be q, as column or row 81 is)
# and SGR (ESC [ < digits and semicolons, then M or m). A report is dropped
# only once it is whole, and keybuf carries a partial one from one read to
# the next; bytes that turn out not to be a report are taken as keys.
keybuf=
take_key() {
	local LC_ALL=C k=$1 s i
	local partial='^'$E'(\[(M.{0,2}|<[0-9;]*)?)?$' whole='^'$E'\[(M...|<[0-9;]*[Mm])$'
	if [ -z "$keybuf" ]; then
		if [ "$k" = "$E" ]; then
			keybuf=$E
		elif [[ $k == [qQ] ]]; then
			exit 0
		fi
		return 0
	fi
	keybuf+=$k
	if [[ $keybuf =~ $whole ]]; then
		keybuf=
	elif [[ ! $keybuf =~ $partial ]]; then
		s=${keybuf#"$E"}
		keybuf=
		for ((i = 0; i < ${#s}; i++)); do
			take_key "${s:i:1}"
		done
	fi
}

read_key() { LC_ALL=C IFS= read -rsn1 -d '' -t "$1" k; }

# Waits for background process $1, reading keys through take_key.
wait_bg() {
	local k
	while kill -0 "$1" 2>/dev/null; do
		read_key 0.1 || continue
		take_key "$k"
	done
	wait "$1" 2>/dev/null || true
}

# After a player stops: turns its mouse tracking off, in case it did not
# (#88), and hides the cursor, which mpv's video outputs show when they exit,
# so the acts after it never draw with it on. Then reads what the terminal
# sent before that took effect through take_key, until 0.2 s pass with no
# input and no partial report, or stdin ends (1 s at most), so no report
# reaches fx.py's keys, which take any q as quit.
player_done() {
	local k end=$((${EPOCHREALTIME//[!0-9]/} + 1000000))
	printf '%s%s[?25l' "$MODES_OFF" "$E"
	while ((${EPOCHREALTIME//[!0-9]/} < end)); do
		if read_key 0.2; then
			take_key "$k"
		elif (($? <= 128)) || [ -z "$keybuf" ]; then
			break
		fi
	done
	keybuf=
}

stop_bg() {
	local p
	for p in "${bg_pids[@]}" $audio_pid; do
		kill "$p" 2>/dev/null || true
	done
	for p in "${bg_pids[@]}" $audio_pid; do
		wait "$p" 2>/dev/null || true
	done
	bg_pids=()
	audio_pid=
	[ -z "$ipc_dir" ] || rm -rf "$ipc_dir"
	ipc_dir=
}

# audio_start START LENGTH [mpv option...]: the soundtrack from START for
# LENGTH seconds (0: to the end).
audio_start() {
	[ "$audio" = 1 ] || return 0
	local length=(--length="$2")
	[ "$2" != 0 ] || length=()
	audio_started=$EPOCHREALTIME
	mpv --no-config --really-quiet --no-input-terminal --no-video --start="$1" "${length[@]}" \
		"${audio_opts[@]}" "${@:3}" -- "$BAD_APPLE" </dev/null >/dev/null 2>&1 &
	audio_pid=$!
}

audio_stop() {
	[ -n "$audio_pid" ] || return 0
	kill "$audio_pid" 2>/dev/null || true
	wait "$audio_pid" 2>/dev/null || true
	audio_pid=
}

# The output the soundtrack plays through, asked for before the show takes
# the screen (--audio) by fork/demo/pick-audio.sh, which AUDIO_DEVICE
# answers in advance: mpv's name for it, or none for no sound. With
# --no-wait nothing is asked. Nothing is asked either when no act in the run
# plays sound (acts 2, 4 and 6, or act 7 with --banner-only).
audio_opts=()
pick_audio() {
	local dev n heard= no_wait=()
	for ((n = first; n <= last; n++)); do
		case $n in 1 | 3 | 5) heard=1 ;; 7) [ -n "$banner_only" ] || heard=1 ;; esac
	done
	if [ -z "$heard" ]; then
		audio=0
		return
	fi
	[ "$wait_key" = 1 ] || no_wait=(--no-wait)
	dev=$(bash "$here/../demo/pick-audio.sh" "${no_wait[@]}") || exit $?
	if [ "$dev" = none ]; then
		audio=0
	else
		audio_opts=(--audio-device="$dev")
	fi
}

saved_tty=
screen_taken=
resized_from=  # "rows cols" while act 3 holds the terminal at 80x25
have_sixel=
have_kitty=

# shellcheck disable=SC2329 # invoked by the EXIT trap
restore() {
	local st=$?
	trap - EXIT
	trap '' INT TERM HUP
	stop_bg
	if [ -n "$screen_taken" ]; then
		[ -z "$resized_from" ] || printf '%s[8;%d;%dt' "$E" "${resized_from% *}" "${resized_from#* }"
		[ -z "$have_kitty" ] || printf '%s_Ga=d,d=A%s' "$E" "$ST"
		printf '%s[0m%s[2J%s[H%s[?25h%s[?1049l%s' "$E" "$E" "$E" "$E" "$E" "$MODES_OFF"
	fi
	[ -z "$saved_tty" ] || stty "$saved_tty" 2>/dev/null || true
	[ ! -s "$notes_file" ] || cat "$notes_file" >&2
	[ -z "$notes_file" ] || rm -f "$notes_file"
	exit "$st"
}

take_screen() {
	saved_tty=$(stty -g)
	trap restore EXIT
	trap 'exit 130' INT
	trap 'exit 143' TERM
	trap 'exit 129' HUP
	stty -echo -icanon min 1 time 0
	screen_taken=1
	printf '%s[?1049h%s[?25l' "$E" "$E"
	clear_screen
}

# Asks the terminal what it draws: DA1 lists 4 when it has sixel; a kitty
# graphics query (a=q) is answered OK by a terminal that has kitty graphics,
# before its DA1 answer. A terminal without either ignores the query (an APC
# string) and answers DA1 alone.
probe() {
	local reply='' c da
	local da_end=$'\e''\[\?[0-9;]*c$'
	[ "$SIXEL" != yes ] || have_sixel=1
	[ "$KITTY" != yes ] || have_kitty=1
	[ "$SIXEL" = auto ] || [ "$KITTY" = auto ] || return 0
	if [ "$KITTY" = auto ]; then
		printf '%s_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA%s' "$E" "$ST"
	fi
	printf '%s[c' "$E"
	while IFS= read -rsn1 -d '' -t "$PROBE_TIMEOUT" c; do
		reply+=$c
		[[ ! $reply =~ $da_end ]] || break
	done
	da=${reply##*"$E"[?}
	da=";${da%c};"
	[ "$SIXEL" != auto ] || [[ $da != *";4;"* ]] || have_sixel=1
	[ "$KITTY" != auto ] || [[ $reply != *"${E}_Gi=31;OK"* ]] || have_kitty=1
}

# cols rows cell-width cell-height of the screen, read again before each act
# so a resized window and a changed font are followed. Once the show has the
# screen, the cell size is the terminal's answer to XTWINOPS 16 (CSI 16 t),
# else its text area (XTWINOPS 14) over the cells; before that, or where the
# terminal answers neither, the pty's pixel size over the cells, else 10x20.
# DA1 follows the two queries, so a terminal that ignores them answers DA1
# alone and is not waited for; one that does is not asked again.
cell_w=0
cell_h=0
winops=yes
read_geometry() {
	local xp yp
	read -r screen_cols screen_rows xp yp < <(python3 -B "$fx_py" geom)
	cell_w=0
	cell_h=0
	if [ -n "$screen_taken" ] && [ "$winops" = yes ]; then
		ask_winops
	fi
	if [ "$cell_w" = 0 ] && [ "${xp:-0}" -gt 0 ] && [ "${yp:-0}" -gt 0 ]; then
		cell_w=$((xp / screen_cols))
		cell_h=$((yp / screen_rows))
	fi
	[ "$cell_w" -gt 0 ] || cell_w=10
	[ "$cell_h" -gt 0 ] || cell_h=20
}

ask_winops() {
	local reply='' c
	local da_end=$'\e''\[\?[0-9;]*c$'
	printf '%s[14t%s[16t%s[c' "$E" "$E" "$E"
	while IFS= read -rsn1 -d '' -t "$PROBE_TIMEOUT" c; do
		reply+=$c
		[[ ! $reply =~ $da_end ]] || break
	done
	winops_cell "$reply"
	[ "$cell_w" != 0 ] || winops=no
}

# The cell size in the answers to XTWINOPS 16 (CSI 6 ; height ; width t),
# else XTWINOPS 14 (CSI 4 ; height ; width t, the text area) over the cells.
# Numbers are read as decimal whatever their leading zeros, and a cell is
# kept within 1 to 1000 pixels a side.
winops_cell() {
	local h=0 w=0
	if [[ $1 =~ \[6\;([0-9]{1,9})\;([0-9]{1,9})t ]]; then
		h=$((10#${BASH_REMATCH[1]}))
		w=$((10#${BASH_REMATCH[2]}))
	elif [[ $1 =~ \[4\;([0-9]{1,9})\;([0-9]{1,9})t ]]; then
		h=$((10#${BASH_REMATCH[1]} / screen_rows))
		w=$((10#${BASH_REMATCH[2]} / screen_cols))
	fi
	if [ "$h" -gt 0 ] && [ "$w" -gt 0 ]; then
		cell_h=$((h > 1000 ? 1000 : h))
		cell_w=$((w > 1000 ? 1000 : w))
	fi
}

# Waits up to RESIZE_WAIT for the screen to be $1 x $2 cells, reading keys.
wait_for_size() {
	local c r k end
	end=$(($(date +%s%N) + $(awk -v s="$RESIZE_WAIT" 'BEGIN { printf "%d", s * 1e9 }')))
	while :; do
		read -r c r _ < <(python3 -B "$fx_py" geom)
		[ "$c" != "$1" ] || [ "$r" != "$2" ] || return 0
		[ "$(date +%s%N)" -lt "$end" ] || return 1
		if IFS= read -rsn1 -t 0.1 k && [[ $k == [qQ] ]]; then
			exit 0
		fi
	done
}

banner_text() {
	local IFS=';' f emoji text=$BANNER_INTRO i=0
	read -ra emoji <<<"${BANNER_EMOJI// /;}"
	for f in $BANNER_FEATURES; do
		text+=" $f ${emoji[i % ${#emoji[@]}]}"
		i=$((i + 1))
	done
	printf '%s %s' "$text" "$BANNER_OUTRO"
}

banner() {
	fx banner --text "$(banner_text)" --rows "$BANNER_ROWS" --font "$BANNER_FONT" --fps "$BANNER_FPS" "$@"
}

# --------------------------------------------------------------- the acts

title_card() {
	[ "$TITLE_SECONDS" != 0 ] || return 0
	fx effect stars --seconds "$TITLE_SECONDS" --title "kmscon" --subtitle "${SHOWCASE_LABEL:-Bad Apple!!}" --font "$BIG_FONT"
	clear_screen
	pause "$ACT_GAP"
}

act_1() {
	title_card
	audio_start "$ACT1_START" "$(awk -v a="$ACT1_SECONDS" -v b="$ACT1_DOTS_SECONDS" 'BEGIN { print a + b }')"
	fx play "$(act1_cache)"
	if [ "$ACT1_DOTS_SECONDS" != 0 ]; then
		clear_screen
		fx play "$(act1_dots_cache)"
	fi
	audio_stop
	clear_screen
}

act_2() {
	clear_screen
	pause "$ACT2_PAUSE"
	fx big "$ACT2_TEXT" --font "$BIG_FONT" --letter "$ACT2_LETTER" --hold "$ACT2_HOLD" \
		--plasma "$ACT2_PLASMA" --fade "$ACT2_FADE" --bobs "$ACT2_BOBS" --fps "$ACT2_FPS"
	clear_screen
	pause "$ACT_GAP"
}

# Act 3 asks the terminal for a real 80x25 grid (xterm's CSI 8 ; rows ; cols
# t, which kmscon gains with #77) and plays full screen when the size took
# effect; otherwise, or with ACT3_RESIZE=no, in a framed 80x25 region. A
# resized screen is put back to its size before act 4, which types over the
# frozen frame redrawn in its frame.
act_3() {
	local version title cols=$screen_cols rows=$screen_rows real=
	version=$(img2txt --version 2>/dev/null | sed -n 's/.*Version: \([^,]*\),.*/\1/p')
	title="img2txt $version · $(act3_size | tr ' ' x)"
	clear_screen
	if [ "$cols" = 80 ] && [ "$rows" = 25 ]; then
		real=same
	elif [ "$ACT3_RESIZE" != no ]; then
		resized_from="$rows $cols"
		printf '%s[8;25;80t' "$E"
		if wait_for_size 80 25; then
			real=resized
		else
			resized_from=  # it did not take; there is nothing to put back
		fi
	fi
	audio_start "$ACT3_START" "$ACT3_SECONDS"
	if [ -n "$real" ]; then
		clear_screen
		fx play "$(act3_cache 80 25)" --broken
	else
		fx play "$(act3_cache)" --box "$title" --broken
	fi
	audio_stop
	if [ "$real" = resized ]; then
		pause 0.8
		printf '%s[8;%d;%dt' "$E" "$rows" "$cols"
		wait_for_size "$cols" "$rows" || true
		resized_from=
		read_geometry
		clear_screen
		fx play "$(act3_cache)" --box "$title" --still
	fi
}

act_4() {
	fx type --bar --cps "$ACT4_CPS" --dots "$ACT4_DOTS" --hold "$ACT4_HOLD" \
		"wait a minute..." "don't we have ... LIBSIXEL?"
}

# Act 5's picture for a clip of $1 x $2 pixels: ACT5_SIZE of the screen's
# rows tall, narrower where the screen's columns run out, centred in whole
# cells. Prints its width in pixels, the rows it covers, and the column and
# row of its top left cell. Its height is a multiple of 6, as mpv rounds a
# sixel picture's.
act5_box() {
	awk -v w="$1" -v h="$2" -v cols="$screen_cols" -v rows="$screen_rows" \
		-v cw="$cell_w" -v ch="$cell_h" -v share="$ACT5_SIZE" 'BEGIN {
		r = int(rows * share)
		if (r > rows - 2) r = rows - 2
		if (r < 1) r = 1
		ph = r * ch
		if (ph * w / h > (cols - 2) * cw) ph = (cols - 2) * cw * h / w
		ph = int(ph / 6) * 6
		pw = int(ph * w / h)
		vc = int((pw + cw - 1) / cw)
		vr = int((ph + ch - 1) / ch)
		printf "%d %d %d %d\n", pw, vr, int((cols - vc) / 2) + 1, int((rows - vr) / 2) + 1
	}'
}

# Act 5 plays the clip through mpv's sixel output (libsixel), sized from the
# cell size the terminal reports; its last frame, glitched, stays on screen
# as the halted frame. mpv takes all but the last of the rows it is told it
# has for the picture's height, so it is told one row more than the picture
# covers and fits the picture to its width.
act_5() {
	local clip w h pw vr left top
	clear_screen
	if [ -z "$have_sixel" ] || ! have mpv; then
		local why="[no sixel: DA1 lacks attribute 4]"
		[ -z "$have_sixel" ] || why="[sixel: mpv is needed]"
		fx placeholder --title "libsixel" --text "$why" --cols 48 --rows 18 --seconds "$PLACEHOLDER_SECONDS"
		return
	fi
	clip=$(act5_clip)
	read -r w h < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$clip" | tr ',' ' ')
	read -r pw vr left top < <(act5_box "$w" "$h")
	local sound=(--no-audio)
	[ "$audio" != 1 ] || sound=("${audio_opts[@]}")
	mpv --no-config --really-quiet --no-input-terminal --vo=sixel --vo-sixel-alt-screen=no \
		--vo-sixel-config-clear=no --vo-sixel-dither="$ACT5_DITHER" \
		--vo-sixel-cols="$screen_cols" --vo-sixel-rows=$((vr + 1)) \
		--vo-sixel-width="$pw" --vo-sixel-height=$(((vr + 1) * cell_h)) \
		--vo-sixel-left="$left" --vo-sixel-top="$top" \
		"${sound[@]}" -- "$clip" </dev/null 2>/dev/null &
	bg_pids=($!)
	wait_bg "${bg_pids[0]}"
	bg_pids=()
	player_done
	pause "$ACT5_HOLD"
}

act_6() {
	# Sixel pixels are cleared before the fire, so none outlive the act;
	# a placeholder is text, and burns.
	[ -z "$have_sixel" ] || clear_screen
	fx effect fire --seconds "$ACT6_FIRE"
	clear_screen
	pause "$ACT_GAP"
	fx type --row 3 --cps "$ACT6_CPS" --dots 0.4 --hold 0.4 \
		"hold on, we also have kitty." "hold on, feeding the cats"
	fx cats --top 8 --seconds "$ACT6_CATS" --density "$ACT6_DENSITY"
	clear_screen
	pause "$ACT_GAP"
}

# Act 7: the video in the rows above the scroller, through the fork's own
# kitty graphics player (kitty.py): each frame transmitted once and placed up
# to three times, one copy in the middle until the moves of the cue sheet
# slide it left and bring out a second and a third. The scroller draws in
# its own process below, the soundtrack in mpv, whose position the player
# follows over mpv's IPC socket, dropping frames rather than drifting; it
# fades out over the act's last ACT7_FADE seconds. Until the end card's cue
# (ACT7_SECONDS). A placeholder stays no longer than the video would.
act_7() {
	clear_screen
	if [ -n "$banner_only" ]; then
		banner --seconds "$BANNER_SECONDS"
		clear_screen
		return
	fi
	if [ -z "$have_kitty" ]; then
		banner --placeholder "[no kitty graphics: the a=q query got no OK]" --title "kitty graphics" \
			--seconds "$(awk -v p="$PLACEHOLDER_SECONDS" -v c="$ACT7_SECONDS" 'BEGIN { print (c > 0 && c < p ? c : p) }')"
		clear_screen
		return
	fi
	local ipc=()
	[ -n "$notes_file" ] || notes_file=$(mktemp)
	if [ "$audio" = 1 ]; then
		local fade=()
		# The fade, no longer than the act, ends with it.
		[ "$ACT7_SECONDS" = 0 ] || [ "$ACT7_FADE" = 0 ] || fade=(--af="lavfi=[afade=t=out:$(awk \
			-v s="$ACT7_START" -v l="$ACT7_SECONDS" -v f="$ACT7_FADE" \
			'BEGIN { if (f > l) f = l; printf "st=%s:d=%s", s + l - f, f }')]")
		# A socket's path must fit 108 bytes: a short directory, not a
		# long TMPDIR. Should mpv still make none, the player follows a
		# wall clock from the soundtrack's start, and says so.
		local run=/tmp
		[ ! -d "${XDG_RUNTIME_DIR:-}" ] || [ ! -w "$XDG_RUNTIME_DIR" ] || run=$XDG_RUNTIME_DIR
		ipc_dir=$(mktemp -d -p "$run" kmscon-showcase.XXXXXX)
		audio_start "$ACT7_START" "$ACT7_SECONDS" --input-ipc-server="$ipc_dir/mpv.sock" "${fade[@]}"
		ipc=(--audio-ipc "$ipc_dir/mpv.sock" --audio-started "$audio_started")
	fi
	# The scroller draws with the cursor saved and restored around each frame,
	# and the player around each of its own.
	python3 -B "$fx_py" banner --text "$(banner_text)" --rows "$BANNER_ROWS" --font "$BANNER_FONT" \
		--fps "$BANNER_FPS" </dev/null &
	bg_pids=($!)
	# What the player says on stderr waits for the screen to come back.
	fx kitty --video="$BAD_APPLE" --start "$ACT7_START" --seconds "$ACT7_SECONDS" \
		--transport "$KITTY_TRANSPORT" --width "$KITTY_WIDTH" --cell-w "$cell_w" --cell-h "$cell_h" \
		--banner-rows "$BANNER_ROWS" --gap "$ACT7_GAP" --slide-at "$ACT7_SLIDE_AT" \
		--copy2-at "$ACT7_COPY2_AT" --copy3-at "$ACT7_COPY3_AT" --move "$ACT7_MOVE" \
		--bounce "$ACT7_BOUNCE" --report "$ACT7_REPORT" "${ipc[@]}" 2>>"$notes_file"
	stop_bg
	clear_screen
}

end_card() {
	[ -t 0 ] || return 0
	fx endcard --title "kmscon · github.com/aaronsb/kmscon" --text "$END_TEXT"
}

# ------------------------------------------------------------------- main

first=1
last=$LAST_ACT
audio=0
wait_key=$START_WAIT
banner_only=

pick_act() {
	[[ $1 =~ ^[1-9][0-9]*$ ]] && [ "$1" -le "$LAST_ACT" ] || die "no act '$1': the acts are 1 to $LAST_ACT (--list)"
}

run_fx() {
	local name=$1 report e known=
	for e in "${EFFECTS[@]}" "starfield|"; do
		[ "${e%%|*}" != "$name" ] || known=1
	done
	[ -n "$known" ] || die "no effect '$name': $(printf '%s\n' "${EFFECTS[@]}" | cut -d'|' -f1 | paste -sd' ')"
	have python3 || die "python3 is needed (pacman -S python)"
	[ -t 0 ] && [ -t 1 ] || die "--fx needs a terminal"
	report=$(mktemp)
	take_screen
	read_geometry
	local st=0
	case $name in
	banner) python3 -B "$fx_py" banner --text "$(banner_text)" --rows "$BANNER_ROWS" --font "$BANNER_FONT" \
		--fps "$BANNER_FPS" --seconds "$FX_SECONDS" --report "$report" || st=$? ;;
	*) python3 -B "$fx_py" effect "$name" --seconds "$FX_SECONDS" --title kmscon --font "$BIG_FONT" \
		--report "$report" || st=$? ;;
	esac
	restore_quietly
	cat "$report"
	rm -f "$report"
	[ "$st" = 0 ] || [ "$st" = 10 ] || exit "$st"
}

restore_quietly() {
	printf '%s[0m%s[2J%s[H%s[?25h%s[?1049l%s' "$E" "$E" "$E" "$E" "$E" "$MODES_OFF"
	stty "$saved_tty"
	screen_taken=
	trap - EXIT INT TERM HUP
	echo "screen ${screen_cols}x${screen_rows} cells"
}

main() {
	local mode=run fx_name='' n
	fx_py=$here/fx.py
	while [ $# -gt 0 ]; do
		case $1 in
		--list) mode=list ;;
		--dry-run) mode=dry ;;
		--prepare) mode=prepare ;;
		--act) [ $# -ge 2 ] || usage; pick_act "$2"; first=$2; last=$2; shift ;;
		--act=*) pick_act "${1#*=}"; first=${1#*=}; last=$first ;;
		--from) [ $# -ge 2 ] || usage; pick_act "$2"; first=$2; last=$LAST_ACT; shift ;;
		--from=*) pick_act "${1#*=}"; first=${1#*=}; last=$LAST_ACT ;;
		--fx) [ $# -ge 2 ] || usage; mode=fx; fx_name=$2; shift ;;
		--fx=*) mode=fx; fx_name=${1#*=} ;;
		--banner-only) banner_only=1 ;;
		--audio) audio=1 ;;
		--no-wait) wait_key=0 ;;
		-h | --help) usage ;;
		*) usage ;;
		esac
		shift
	done

	case $mode in
	list) list_acts; return ;;
	dry) dry_run; return ;;
	fx) run_fx "$fx_name"; return ;;
	esac

	if [ "$mode" = prepare ]; then
		first=1
		last=$LAST_ACT
	fi
	local video=0 missing=()
	for ((n = first; n <= last; n++)); do
		[ "${ACT_VIDEO[n]}" = 0 ] || { [ "$n" = 7 ] && [ -n "$banner_only" ]; } || video=1
	done
	if [ "$video" = 1 ]; then
		[ -n "${BAD_APPLE:-}" ] ||
			die "BAD_APPLE is unset: set it to your own copy of the Bad Apple!! video (BAD_APPLE=/path/to/video); it is never downloaded"
		[ -f "$BAD_APPLE" ] && [ -r "$BAD_APPLE" ] || die "BAD_APPLE=$BAD_APPLE is not a readable file"
	fi
	# What every act needs before it starts; mpv is needed only where the
	# terminal has the protocol, so acts 5 and 7 check for it later.
	have python3 || missing+=("python3 (python)")
	if [ "$video" = 1 ]; then
		have ffmpeg || missing+=("ffmpeg (ffmpeg)")
		have ffprobe || missing+=("ffprobe (ffmpeg)")
	fi
	if [ "$first" -le 3 ] && [ "$last" -ge 3 ]; then
		have img2txt || missing+=("img2txt (libcaca)")
	fi
	[ "$audio" != 1 ] || have mpv || missing+=("mpv, for --audio (mpv)")
	[ ${#missing[@]} -eq 0 ] || die "missing: ${missing[*]}"

	if [ "$mode" = prepare ]; then
		cache=$(cache_dir)
		mkdir -p "$cache"
		for n in 1 3 5; do
			prepare_act "$n"
		done
		echo "prepared: $cache"
		return
	fi

	[ -t 0 ] && [ -t 1 ] || die "the show needs a terminal on stdin and stdout"
	read_geometry
	# Acts 1, 3 and 5 play from caches; act 7 reads the video itself.
	local cached=
	for n in 1 3 5; do
		((n < first || n > last)) || cached=1
	done
	if [ -n "$cached" ]; then
		cache=$(cache_dir)
		mkdir -p "$cache"
		for n in 1 3 5; do
			((n < first || n > last)) || prepare_act "$n"
		done
	fi
	[ "$audio" != 1 ] || pick_audio

	take_screen
	read_geometry
	if (((first <= 5 && last >= 5) || (first <= 7 && last >= 7))); then
		probe
	fi
	clear_screen
	if [ "$wait_key" = 1 ]; then
		local t="press a key to start, q to stop at any time"
		printf '%s[%d;%dH%s[2m%s%s[0m' "$E" $((screen_rows / 2)) $(((screen_cols - ${#t}) / 2 + 1)) "$E" "$t" "$E"
		local k=
		IFS= read -rsn1 k || true
		[[ $k != [qQ] ]] || exit 0
		clear_screen
	fi
	for ((n = first; n <= last; n++)); do
		read_geometry
		"act_$n"
	done
	[ "$last" != "$LAST_ACT" ] || [ -n "$banner_only" ] || end_card
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
