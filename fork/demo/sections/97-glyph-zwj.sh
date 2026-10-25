#!/bin/bash
# kit: glyphs
# look-for: A page of ZWJ emoji, families, people at work in five skin tones,
# look-for: flags and keycaps, each one picture in colour; then the line
# look-for: under them is rewritten 2000 times, the counter rising, and ends
# look-for: showing the same emoji. Underlined text at the bottom keeps its
# look-for: underline throughout.
# wrong-looks-like: A sequence in pieces, a grey or blank emoji, another
# wrong-looks-like: emoji in a cell, or an underline that goes missing.
# Every rewrite of a cluster gives it a new key in libtsm, so the rewritten
# line churns the colour pools with keys that are never drawn again; they
# must age out without pushing out what stays on screen (ADR-102).
zwj=$'\xe2\x80\x8d'    # U+200D ZERO WIDTH JOINER
vs16=$'\xef\xb8\x8f'   # U+FE0F VARIATION SELECTOR-16
keycap=$'\xe2\x83\xa3' # U+20E3 COMBINING ENCLOSING KEYCAP
tones=($'\xf0\x9f\x8f\xbb' $'\xf0\x9f\x8f\xbc' $'\xf0\x9f\x8f\xbd' $'\xf0\x9f\x8f\xbe' $'\xf0\x9f\x8f\xbf')
jobs=('🔬' '💻' '🚀' '🎨' '🍳' '🔧' '🏫' '🎓')

printf '  families:  %s %s %s %s\n' "👨$zwj👩$zwj👧" "👩$zwj👩$zwj👦" "👨$zwj👨$zwj👧$zwj👦" "👩$zwj👦"
for p in '🧑' '👩' '👨'; do
	printf '  %s at work:' "$p"
	for t in "${tones[@]}"; do
		for j in "${jobs[@]:0:4}"; do
			printf ' %s' "$p$t$zwj$j"
		done
	done
	printf '\n'
done
printf '  flags:     🇳🇴 🇯🇵 🇧🇷 🇰🇪 🇺🇦 🇮🇳 🇨🇦 🏳%s%s🌈\n' "$vs16" "$zwj"
printf '  keycaps:  '
for d in 0 1 2 3 4 5 6 7 8 9 '#' '*'; do
	printf ' %s' "$d$vs16$keycap"
done
printf '\n\n  \033[4munderlined: this line keeps its underline\033[0m\n\n'
# paced on a terminal, so each rewrite is a frame; at once into a pipe (tests)
pace=0
[ ! -t 1 ] || pace=0.005
for i in $(seq 1 2000); do
	t=${tones[i % 5]}
	printf '\r  rewrite %4d: %s %s %s' "$i" "👩$t$zwj${jobs[i % 8]}" "🧑$t$zwj🚀" "👨$zwj👩$zwj👧"
	[ "$pace" = 0 ] || sleep "$pace"
done
printf '\n'
