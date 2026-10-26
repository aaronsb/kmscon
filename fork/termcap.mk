# The XTGETTCAP table kmscon gives libtsm (src/terminal_termcap.h), generated
# from the terminfo entry. make test checks it against the entry; a change to
# scripts/terminfo/kmscon.ti is followed by make termcap in the same commit.

.PHONY: termcap
termcap: image ## Rewrite src/terminal_termcap.h from scripts/terminfo/kmscon.ti
	$(RUN) python3 scripts/terminfo/gen_termcap.py scripts/terminfo/kmscon.ti \
		src/terminal_termcap.h
