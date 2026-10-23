# Capability probes

One small check per terminal feature, so `make test` says what kmscon does and what it is missing. The `works` rows come first and matter most: they guard the features the fork built (colon sub-parameters, underline styles and colours, clusters, REP, erase colours, per-display frame age and more) against regression, a second guard beside libtsm's own suite, run against the libtsm kmscon actually pins. The `missing` rows record features of other terminals kmscon lacks, each with its issue. Probes are light on purpose: one probe proves a feature is there, and the feature's own tests cover its behaviour.

[COVERAGE.md](COVERAGE.md) maps every feature the fork built to the tests that guard it, by font engine, renderer and video backend, and names the issue for each gap.

| Path | What it is |
|---|---|
| `capabilities.list` | Every probe with its recorded status (`works` or `missing`), the issue that tracks a missing feature (`#N` on aaronsb/kmscon, `libtsm#N` on aaronsb/libtsm), and a note. |
| `COVERAGE.md` | The fork's features, the tests that guard each, and the gaps. |
| `run.py` | Runs every probe and judges it against the list. Meson runs it as the `capabilities` test. |
| `../test_capabilities.c` | Probes through libtsm's public API: feed the bytes a program sends, check the reply or the screen state. |
| `../test_capabilities_font.c` | Probes of the freetype backend: one glyph drawn with and without an attribute, with fonts pinned from `tests/fonts`. |

Terminfo probes are defined in `run.py`: the entry in `scripts/terminfo/kmscon.ti` is compiled with `tic` and read back with `infocmp`, and each probe names the capabilities it needs.

## Verdicts

The `capabilities` test fails when:

- a probe listed as `works` fails;
- a probe listed as `missing` passes, so the change that adds a feature also sets its line to `works` and closes its issue;
- a probe exists that the list does not name, or the list names a probe nothing defines.

A probe whose source is not built (the freetype backend) or whose tool is not installed (`tic`, `infocmp`) is printed as `skip` and does not fail the run.

## Placeholders

Some features have no interface to observe them through yet (focus reporting, OSC 8, OSC 52, OSC 133, bidirectional text). Their probes are placeholders that always fail and say so; the change that adds the feature replaces the placeholder with a real probe in the same commit that sets the line to `works`. Two probes check a frame flag libtsm does not define yet (`TSM_FRAME_OVERLINE`, `TSM_FRAME_SUPERSCRIPT`/`SUBSCRIPT`); a change that names the flag differently renames it in the probe.

## Adding a probe

Add a function and a table entry in `test_capabilities.c` (or `test_capabilities_font.c`, or a `TERMINFO` entry in `run.py`), and a line in `capabilities.list`. Plant the defect it exists to catch and see it red before trusting it, as for any check (`docs/fork/development.md`, A planted defect).
