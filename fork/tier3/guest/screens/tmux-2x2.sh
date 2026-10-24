#!/bin/bash
# kit: sixel
# look-for: For five seconds tmux shows 4 panes, 2 by 2, every pane playing the
# look-for: test pattern inside its borders, its counter rising. Judged: every
# look-for: pane animates and kmscon keeps up. The frame rate is reported, not
# look-for: judged: tmux and mpv set it (under half the frames at 8K). Then
# look-for: this page cleared, with mpv's counts and rate in bold.
# wrong-looks-like: "SIXEL IMAGE" placeholders in place of video, a picture
# wrong-looks-like: across a pane border or in the wrong pane, frozen panes,
# wrong-looks-like: or pieces of video left on this page afterwards.
# screen-text: none, as tmux draws on the alternate screen
# ADR-103's acceptance scenario A4 in a 2 by 2 grid (tmux-grid.inc).
n=2
. "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/tmux-grid.inc"
