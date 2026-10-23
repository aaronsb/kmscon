#!/bin/bash
# kit: sixel
# look-for: For five seconds mpv plays a test pattern full screen: colour bars,
# look-for: a moving rainbow band and squares, a counter in the top left
# look-for: rising by 30 a second. Then this page comes back with nothing of
# look-for: the video left on it. With --render-timer=osd its figures show.
# wrong-looks-like: A frozen, torn or flickering picture, the counter jumping
# wrong-looks-like: in large steps (frames lost), pieces of the video left on
# wrong-looks-like: this page afterwards, or the page not coming back.
# screen-text: none, as mpv draws on the alternate screen
# ADR-103's acceptance scenario A2: mpv --vo=sixel playing an animation full
# screen. The clip is ffmpeg's testsrc2 pattern, 640x360 at 30 frames per
# second for 5 seconds, generated as mpv plays it, which mpv scales to the
# screen and encodes through libsixel; it draws on the alternate screen and
# leaves it when the clip ends. Tier 3 measures the window: kmscon's render
# timer gives the frames presented, CPU time and damage per frame, and the
# script adds the frames mpv played and dropped (mpv-stats.lua) and kmscon's
# own CPU time over the window, parsing included. kmscon is not the
# bottleneck when it presents the frames mpv sends.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/acceptance.inc"
sixel_offered
needs mpv mpv
stats=$(mktemp)
play() {
	mpv_plain --vo=sixel --script="$here/mpv-stats.lua" --script-opts=t3-stats="$stats" \
		"$(clip 640x360 5)"
}
measure play
read -r frames dropped < <(mpv_counts "$stats")
rm -f "$stats"
printf 'mpv --vo=sixel played a 5-second test pattern full screen, 30 frames a\n'
printf 'second, and returned to this page.\n'
report mpv-sixel "mpv_frames=$frames mpv_dropped=$dropped kmscon_cpu_ms=$kmscon_ms"
