#!/bin/bash
# look-for: Left: bars red to grey over a checkerboard, TEXT cut into it as
# look-for: text. Right: a blue disc in an orange ring, the text behind it showing.
# wrong-looks-like: No pictures (stock kmscon has no sixel; the page says so),
# wrong-looks-like: bars out of order, a box hiding the text around the ring, or bars behind TEXT.
# screen-text: none, sixel pictures and cursor moves do not print as rows
# Sixel graphics (DCS q ... ST) drawn through the pty, as a program such as
# img2sixel sends them. Both pictures are made here, 96x48 and 48x48 pixels,
# from fixed colours. The left one is opaque: the letters written over it
# afterwards remove the picture under their cells. The right one is drawn
# with transparent pixels (P2=1) over text already on the screen, which
# shows outside the ring. The last line is the cell size the terminal
# reports (XTWINOPS 16), the one it places pictures with.
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

# Eight bars of 12 pixels in registers 1 to 8 over four bands, then a
# checkerboard of 12-pixel squares in register 9 on register 0 (black).
bars() {
	local s=$esc'Pq"1;1;96;48'
	local rgb=(100,0,0 100,100,0 0,100,0 0,100,100 0,0,100 100,0,100 100,100,100 50,50,50)
	local i b r
	for i in 0 1 2 3 4 5 6 7; do
		s+="#$((i + 1));2;${rgb[i]//,/;}"
	done
	s+='#9;2;100;100;100'
	for b in 0 1 2 3; do
		for i in 0 1 2 3 4 5 6 7; do
			s+="#$((i + 1))"
			((i)) && s+="!$((i * 12))?"
			s+='!12~$'
		done
		s+='-'
	done
	for b in 0 1 2 3; do
		s+='#9'
		for i in 0 1 2 3 4 5 6 7; do
			r=$(((i + b / 2) % 2))
			((r)) && s+='!12?' || s+='!12~'
		done
		s+='-'
	done
	printf '%s' "$s$esc\\"
}

# A blue disc of radius 12 in an orange ring of radius 24, the rest
# transparent: a pixel is set when (2x-47)^2 + (2y-47)^2 is within the
# circle's doubled radius squared.
disc() {
	local s=$esc'P0;1q"1;1;48;48#1;2;100;50;0#2;2;0;0;100'
	local b x k y dx dy d mask reg lo hi
	for b in 0 1 2 3 4 5 6 7; do
		for reg in 1 2; do
			((reg == 1)) && lo=24 hi=48 || lo=-1 hi=24
			s+="#$reg"
			for ((x = 0; x < 48; x++)); do
				mask=0
				for k in 0 1 2 3 4 5; do
					y=$((b * 6 + k))
					dx=$((2 * x - 47))
					dy=$((2 * y - 47))
					d=$((dx * dx + dy * dy))
					((d <= hi * hi && d > lo * lo)) && mask=$((mask | 1 << k))
				done
				s+=${six:mask:1}
			done
			s+='$'
		done
		s+='-'
	done
	printf '%s' "$s$esc\\"
}

printf 'Sixel pictures, sent through the pty as a program sends them:\n\n'
# the text the right picture is drawn over, four rows of it
for i in 1 2 3 4; do
	printf '%38s%s\n' '' 'text behind the ring shows past its edge'
done
printf '%s[4A%s7  ' "$esc" "$esc"
bars
printf '%s8%s[B%s[6CTEXT%s8%s[38C' "$esc" "$esc" "$esc" "$esc" "$esc"
disc
# below the four rows of text, which are taller than the pictures
printf '%s8%s[4B\n' "$esc" "$esc"
IFS=';' read -r _ ch cw <<<"$(ask "$esc[16t" t)"
printf 'Cells of %sx%s pixels, as XTWINOPS 16 reports them.\n' "${cw:-?}" "${ch:-?}"
