#!/bin/bash
# demo: The 16 colours as text and as background, then bold, underline, reverse,
# demo: faint and italic; the 256-colour cube and a smooth 24-bit ramp below.
# Tier 3's sgr screen, run unchanged so the demo shows what tier 3 captures.
exec bash "$(dirname "$0")/../../tier3/guest/screens/sgr.sh"
