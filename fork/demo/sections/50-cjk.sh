#!/bin/bash
# look-for: Each CJK character takes two cells, whole and not overlapping
# look-for: its neighbour; the bars of the last two rows line up.
# wrong-looks-like: Characters cut, squeezed into one cell or overlapping,
# wrong-looks-like: bars of the last two rows out of line, or empty boxes
# wrong-looks-like: for a missing font.
# Tier 3's cjk screen, run unchanged so the demo shows what tier 3 captures.
exec bash "$(dirname "$0")/../../tier3/guest/screens/cjk.sh"
