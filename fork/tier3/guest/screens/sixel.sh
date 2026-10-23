#!/bin/bash
# look-for: Left: bars red, yellow, green, cyan, blue, magenta, white and grey
# look-for: over a checkerboard, TEXT across the checkerboard as text. Right: a
# look-for: blue disc in an orange ring, the text behind it showing past its edge.
# look-for: Below: six discs, each above its name: red, green, blue, cyan,
# look-for: magenta and yellow. Each picture fills whole cells.
# wrong-looks-like: No pictures (stock kmscon has no sixel; the page says so), a
# wrong-looks-like: disc not the colour of its name, a ring that is not orange,
# wrong-looks-like: bars out of order, a box hiding the text around the ring, bars
# wrong-looks-like: behind TEXT, or a picture running into its label or the row below.
# screen-text: none, sixel pictures and cursor moves do not print as rows
# Sixel graphics (DCS q ... ST) drawn through the pty, as a program such as
# img2sixel sends them. The pictures are made here at run time, sized in
# whole cells from the cell size the terminal reports (XTWINOPS 16): the bars
# 8 columns by 2 rows, the ring 4 by 2 and each disc 3 by 2, so they line up
# with the text at any font size. Without an answer they are made for 12x24
# cells, which the page says. The bars are opaque: the letters written over
# them afterwards remove the picture under their cells. The ring and the
# discs have transparent pixels (P2=1); the ring is drawn over text already
# on the screen, which shows outside it. The six discs are the primaries and
# secondaries at full strength, as sixel RGB percentages (#R;2;r;g;b):
# red 100;0;0, green 0;100;0, blue 0;0;100, cyan 0;100;100, magenta
# 100;0;100, yellow 100;100;0. The ring is orange, 100;50;0, around blue
# 0;0;100. The tier 3 goldens are compared exactly, so they pin each colour.
# The last line is the cell size the terminal reports.
esc=$'\033'
# the sixel data characters, for the six-pixel columns 0 to 63
six='?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`abcdefghijklmnopqrstuvwxyz{|}~'

# The terminal's answer to a query, up to its final byte; empty when it does
# not answer within a second or this is not a terminal. Echo is off before
# the query goes out, so an answer that arrives at once is not printed.
ask() {
	local reply= mode
	if [ -t 0 ] && mode=$(stty -g </dev/tty 2>/dev/null); then
		stty -echo </dev/tty
		printf '%s' "$1" >/dev/tty
		IFS= read -rsd "$2" -t 1 reply </dev/tty
		stty "$mode" </dev/tty
	fi
	printf '%s' "$reply"
}

# Sixel needs DA1 attribute 4.
da1=$(ask "$esc[c" c)
if [[ ";${da1##*\?};" != *";4;"* ]]; then
	printf 'This terminal does not offer sixel graphics: its DA1 reply has\n'
	printf 'no attribute 4 (%s), so no picture is sent.\n' "${da1:-no reply}"
	exit 0
fi

# The cell size in pixels, from the reply ESC [ 6 ; height ; width t.
IFS=';' read -r _ rh rw <<<"$(ask "$esc[16t" t)"
cw=$rw ch=$rh
if [[ ! $cw =~ ^[1-9][0-9]*$ || ! $ch =~ ^[1-9][0-9]*$ ]]; then
	cw=12 ch=24
	note='XTWINOPS 16 gave no cell size, so the pictures are made for 12x24 cells.'
fi

# The bits of the sixel band starting at row $1 that rows $2 to $3 cover, in m.
bits() {
	local lo=$(($2 - $1)) hi=$(($3 - $1))
	((lo < 0)) && lo=0
	((hi > 5)) && hi=5
	m=0
	((lo <= hi)) && m=$(((1 << (hi + 1)) - (1 << lo)))
}

# Eight bars, one cell wide and one row tall, in registers 1 to 8; under
# them a checkerboard of squares a cell wide and half a row tall, register
# 9 on register 0 (black).
bars() {
	local w=$((8 * cw)) h=$((2 * ch)) half=$((ch + ch / 2))
	local s=$esc'Pq"1;1;'$w';'$h
	local rgb=(100,0,0 100,100,0 0,100,0 0,100,100 0,0,100 100,0,100 100,100,100 50,50,50)
	local i y bar ma mb
	for i in 0 1 2 3 4 5 6 7; do
		s+="#$((i + 1));2;${rgb[i]//,/;}"
	done
	s+='#9;2;100;100;100'
	for ((y = 0; y < h; y += 6)); do
		bits $y 0 $((ch - 1))
		bar=$m
		for ((i = 0; bar && i < 8; i++)); do
			s+="#$((i + 1))"
			((i)) && s+="!$((i * cw))?"
			s+="!$cw${six:bar:1}\$"
		done
		bits $y $ch $((half - 1))
		ma=$m
		bits $y $half $((h - 1))
		mb=$m
		if ((ma || mb)); then
			s+='#9'
			for ((i = 0; i < 8; i++)); do
				((i % 2)) && m=$mb || m=$ma
				s+="!$cw${six:m:1}"
			done
			s+='$'
		fi
		s+='-'
	done
	printf '%s' "$s$esc\\"
}

# A picture $1 by $2 pixels of circles centred in it, the rest transparent.
# The arguments after the size are a colour (r,g,b percentages) and a
# diameter each, the outermost circle first; a pixel takes the colour of the
# innermost circle it is in. A pixel is in a circle of diameter d when
# (2x + 1 - w)^2 + (2y + 1 - h)^2 < d^2.
circles() {
	local w=$1 h=$2 s=$esc'P0;1q"1;1;'$1';'$2
	shift 2
	local n=0 top=() end=() x j dx r t u y m0
	while (($#)); do
		s+="#$((n + 1));2;${1//,/;}"
		# each column's first and last row inside circle n
		for ((x = 0; x < w; x++)); do
			dx=$((2 * x + 1 - w))
			r=$(($2 * $2 - dx * dx - 1))
			top[n * w + x]=1 end[n * w + x]=0
			((r < 0)) && continue
			# t = floor(sqrt(r)), by Newton's method
			t=$r
			((r > 1)) && for ((u = (r + 1) / 2; u < t; u = (t + r / t) / 2)); do
				t=$u
			done
			top[n * w + x]=$(((h - t) / 2)) end[n * w + x]=$(((h + t - 1) / 2))
		done
		((n++))
		shift 2
	done
	for ((y = 0; y < h; y += 6)); do
		for ((j = 0; j < n; j++)); do
			s+="#$((j + 1))"
			for ((x = 0; x < w; x++)); do
				bits $y ${top[j * w + x]} ${end[j * w + x]}
				if ((j + 1 < n)); then
					m0=$m
					bits $y ${top[(j + 1) * w + x]} ${end[(j + 1) * w + x]}
					m=$((m0 & ~m))
				fi
				s+=${six:m:1}
			done
			s+='$'
		done
		s+='-'
	done
	printf '%s' "$s$esc\\"
}

# The text first, then the pictures over it, each placed from the saved
# cursor at the start of the third row.
printf 'Sixel pictures, sent through the pty as a program sends them:\n\n'
for i in 1 2 3 4; do
	printf '%38s%s\n' '' 'text behind the ring shows past its edge'
done
printf '%38s%s\n\n' '' 'The ring is orange: red 100%, green 50%.'
printf 'Primaries and secondaries, each component at 100%% or 0%%:\n\n\n'
printf '  %-10s%-10s%-10s%-10s%-10s%s\n' red green blue cyan magenta yellow
printf '%s[10A%s7  ' "$esc" "$esc"
bars
printf '%s8%s[B%s[6CTEXT' "$esc" "$esc" "$esc"
d=$((4 * cw < 2 * ch ? 4 * cw : 2 * ch))
printf '%s8%s[B%s[40C' "$esc" "$esc" "$esc"
circles $((4 * cw)) $((2 * ch)) 100,50,0 $d 0,0,100 $((d / 2))
d=$((3 * cw < 2 * ch ? 3 * cw : 2 * ch))
i=2
for rgb in 100,0,0 0,100,0 0,0,100 0,100,100 100,0,100 100,100,0; do
	printf '%s8%s[7B%s[%dC' "$esc" "$esc" "$esc" $i
	circles $((3 * cw)) $((2 * ch)) $rgb $d
	((i += 10))
done
printf '%s8%s[11B' "$esc" "$esc"
[ "$note" ] && printf '%s\n' "$note"
printf 'Cells of %sx%s pixels, as XTWINOPS 16 reports them.\n' "${rw:-?}" "${rh:-?}"
