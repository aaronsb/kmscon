#!/bin/bash
# demo: Each CJK character takes two cells: the bars of the two last rows line up,
# demo: and no character is cut or overlaps its neighbour.
# Tier 3's cjk screen, run unchanged so the demo shows what tier 3 captures.
exec bash "$(dirname "$0")/../../tier3/guest/screens/cjk.sh"
