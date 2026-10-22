Each `NAME.sh` here draws one tier 3 screen, run by `t3 NAME` in the guest after the console is cleared. Its leading comment lines are the screen's caption on the run and comparison pages.

The scripts also run on the host, when `t3_evaluate.py` evaluates a run: their output, with colours removed, names the text drawn in each differing region of a comparison. So a script prints its screen top to bottom, reads no input, finishes at once, and uses no escape sequence other than colours (SGR). A cursor move, a tab or a character-set switch leaves the pages without that screen's text.
