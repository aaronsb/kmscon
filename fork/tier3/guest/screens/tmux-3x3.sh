#!/bin/bash
# kit: sixel
# look-for: For five seconds tmux shows 9 panes, 3 by 3, each playing the
# look-for: test pattern with its own counter rising by 30 a second, inside the
# look-for: pane's borders. Then this page on a cleared screen, with nothing
# look-for: of the videos left on it.
# wrong-looks-like: "SIXEL IMAGE" placeholders in place of video, a picture
# wrong-looks-like: across a pane border or in the wrong pane, frozen panes,
# wrong-looks-like: or pieces of video left on this page afterwards.
# screen-text: none, as tmux draws on the alternate screen
# ADR-103's acceptance scenario A4 in a 3 by 3 grid (tmux-grid.inc).
n=3
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/tmux-grid.inc"
