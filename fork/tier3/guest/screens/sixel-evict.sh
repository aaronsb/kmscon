#!/bin/bash
# kit: sixel
# look-for: The last of N pictures under its caption. Logo+= turns the screen
# look-for: and the picture shows turned. Logo+- turns it back; then
# look-for: Shift+PageUp to the top: picture 1's caption over blank cells,
# look-for: picture 2 on drawn.
# wrong-looks-like: After Logo+=, the last picture missing, its caption over
# wrong-looks-like: blank cells (left out for the budget); in scrollback, a
# wrong-looks-like: picture after the first blank, or the first still drawn.
# screen-text: none, sixel pictures do not print as rows
# The image budget under pressure (ADR-103's Q1 note): a turned or zoomed
# screen draws a scaled copy of each picture on it, and the copy for a
# picture on screen may evict pictures in scrollback when that lets it fit.
# N pictures (12, or more on a short screen), each a band the screen's width
# in one colour, fill image-storage-limit=auto as the terminal's size gives
# it, four frames in ARGB of its cells from 16 MiB to 192 MiB, to within a
# picture's rounding, so a scaled copy fits only once the oldest picture,
# picture 1, has gone. Each sits under its caption; the last starts below a
# gap taller than the screen is wide in rows, so that turning the screen,
# which pulls lines back from scrollback onto the taller screen, leaves the
# others wholly in scrollback. With a set image-storage-limit the pictures
# do not fill it, and nothing is evicted. Scrolled back while turned, each
# picture that comes into view needs its own copy and evicts the oldest
# again, so the page asks to turn the screen back before scrolling.
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/sixel-common.inc"
sixel_offered
cell_size
term_size

# the auto limit for a display the size of the terminal's cells
W=$((cols * cw))
H=$((rows * ch))
limit=$((W * H * 16))
((limit < 16 << 20)) && limit=$((16 << 20))
((limit > 192 << 20)) && limit=$((192 << 20))

# N pictures W wide, each the limit's share less its record's 512 bytes, at
# most the screen less eight rows tall
n=12
max=$(((rows - 8) * ch))
((n < limit / (4 * W * max + 512) + 1)) && n=$((limit / (4 * W * max + 512) + 1))
h=$(((limit / n - 512) / (4 * W)))
hrows=$(((h + ch - 1) / ch))
# lines from the second-last picture to the last: more than a turned screen has rows
gap=$((W / ch + 2))

colours=(100,0,0 0,100,0 0,0,100 100,100,0 0,100,100 100,0,100
	100,50,0 50,0,100 0,100,50 100,0,50 50,100,0 0,50,100)
for ((k = 1; k <= n; k++)); do
	((k == n)) && printf '%*s' "$gap" '' | tr ' ' '\n'
	printf 'picture %d of %d, %dx%d pixels\n' "$k" "$n" "$W" "$h"
	swatch "$W" "$h" "${colours[(k - 1) % 12]}"
	printf '\n'
	((k < n)) && printf '\n'
done
printf 'Logo+= turns the screen: the last picture stays drawn, turned.\n'
printf 'Logo+- turns it back. Then Shift+PageUp to the top:\n'
printf 'picture 1 blank under its caption, picture 2 on drawn.\n'
