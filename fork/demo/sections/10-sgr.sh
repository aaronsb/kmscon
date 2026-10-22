#!/bin/bash
# look-for: 16 different colours as text, then as backgrounds (the rows
# look-for: wrap at 80 columns); each attribute word drawn as it says; a
# look-for: smooth ramp.
# wrong-looks-like: Two swatches in one colour, an attribute word drawn
# wrong-looks-like: plain, uneven swatches in the cube, or steps or gaps in
# wrong-looks-like: the bottom ramp.
# Tier 3's sgr screen, run unchanged so the demo shows what tier 3 captures.
exec bash "$(dirname "$0")/../../tier3/guest/screens/sgr.sh"
