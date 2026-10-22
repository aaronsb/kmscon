#!/bin/bash
# look-for: Lines join cell to cell with no gaps and corners meet; the
# look-for: shades and blocks fill whole cells.
# wrong-looks-like: Gaps or dots between line segments, corners that miss,
# wrong-looks-like: or a dark line between rows of vertical lines (#46).
# Tier 3's boxdraw screen, run unchanged so the demo shows what tier 3 captures.
exec bash "$(dirname "$0")/../../tier3/guest/screens/boxdraw.sh"
