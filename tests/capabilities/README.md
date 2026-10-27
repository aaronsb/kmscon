# Capability probes

One small check per terminal feature, so `make test` says what kmscon does and what it is missing. The `works` rows come first and matter most: they guard the features the fork built (colon sub-parameters, underline styles and colours, clusters, REP, erase colours, per-display frame age and more) against regression, a second guard beside libtsm's own suite, run against the libtsm kmscon actually pins. The `missing` rows record features of other terminals kmscon lacks, each with its issue. Probes are light on purpose: one probe proves a feature is there, and the feature's own tests cover its behaviour.

[COVERAGE.md](COVERAGE.md) maps every feature the fork built to the tests that guard it, by font engine, renderer and video backend, and names the issue for each gap.

| Path | What it is |
|---|---|
| `capabilities.list` | Every probe with its recorded status (`works` or `missing`), the issue that tracks a missing feature (`#N` on aaronsb/kmscon, `libtsm#N` on aaronsb/libtsm), and a note. |
| `COVERAGE.md` | The fork's features, the tests that guard each, and the gaps. |
| `probes.replay` | The terminal's probes, each a stanza of bytes to feed and the lines or reply to expect (The replay probes, below). |
| `run.py` | Runs every probe and judges it against the list. Meson runs it as the `capabilities` test, and with `--parity` as `capabilities-parity`. |
| `../test_capabilities.c`, `../test_capabilities_images.c` | The same probes as C programs, through libtsm's public API and kmscon's own vte, held to the replay's results by `capabilities-parity`. |
| `../test_capabilities_font.c` | Probes of the freetype backend: one glyph drawn with and without an attribute, with fonts pinned from `tests/fonts`. |

## The replay probes

`probes.replay` is replayed by a program that meets libtsm's replay interface, `test/conformance/INTERFACE.md` in libtsm, which libtsm's meson build exports as `replay_spec`. Here the program is libtsm's `tsm-replay`, from the pinned subproject, so the build needs `-Dlibtsm:tests=true`, the default. vtium runs the same file through `run.py --replay` with its own replay program. `--describe` says which interface, views and features the program has: a probe that needs a feature it does not declare fails as absent, and a program of another major version is refused.

A stanza opens with `probe NAME`, builds a byte stream and the cuts of INTERFACE.md's command line, and judges the blocks the replay prints at the cuts. A line starting with `#` is a comment.

| Line | Meaning |
|---|---|
| `size COLS ROWS` | The screen the replay starts with. Required. |
| `view NAME` | `screen` (when not given), `frame` or `draw2`: the view the replay prints. |
| `needs FEATURE...` | Features the probe needs beyond those its view and commands imply. |
| `fixture NAME` | A file the probe's fixture directory holds, given to `run.py` by `--fixture`. Without it the probe is skipped. |
| `send "..."` | Appends bytes. `\e`, `\r`, `\n`, `\t`, `\\`, `\"` and `\xHH` stand for one byte each, as in INTERFACE.md's worked examples, and every other character for its UTF-8. |
| `cmd TEXT` | Appends the test command TEXT, `ESC _ tsm-test;TEXT ESC \`. |
| `check` | Puts a cut after the bytes so far. The assertions that follow judge the block printed there. |
| `want LINE` | The block holds LINE. |
| `want-re REGEX` | A line of the block matches REGEX whole. |
| `reject LINE` | The block does not hold LINE. |
| `reject-re REGEX` | No line of the block matches REGEX whole. Takes an `example`. |
| `reply "..."` | The block's replies, its `reply` lines decoded and joined, are these bytes. |
| `reply-re "..."` | The replies match these bytes, read as a regular expression, whole. A metacharacter meant literally takes a backslash, written `\\`. |
| `example "..."` | Follows a `want-re`, `reject-re` or `reply-re`: a line or reply, written as for `send`, that the expression must match. Required after a `reject-re`, and in a missing probe. |
| `placeholder REASON` | The probe has nothing to run, and fails with REASON. |

A `test` line belongs to the block whose `checkpoint` follows it, and the `want` and `want-re` assertions on `test` lines take them in order: each matches a line after the one the assertion before it matched. A regular expression uses no backreference or lookaround, as in INTERFACE.md's grammar.

Every line the replay prints must parse under the spec's grammar for the view, and the replay must print a block at each cut, headed by its `checkpoint` line, and one at the end. Otherwise the replay could not judge the probe, which fails the run whatever the list records. Unless the probe needs `damage-exact` and the replay declares it, the output and the `want` and `reject` lines are masked as INTERFACE.md's "Comparing with the goldens" says: a nonzero `changed` or `frame-since` count becomes `N`, and a placement's `changed` flag goes. An expectation writes such a count as `N`.

`run.py` checks every stanza before anything runs. Each `want` and `reject` line, and each `want-re` and `reject-re` example, must parse under the spec's grammar for the probe's view, with the reserved keys (`overline`, `superscript`, `subscript`) allowed only in a missing probe, and each `want-re`, `reject-re` and `reply-re` must match its example. A missing probe's expectations are then lines and replies the feature could print, so a typo reads as malformed instead of as still missing. A `reject-re` is then a pattern some printable line matches: a typo, or a pattern written against a count or flag the runner masks, would otherwise reject nothing and always pass. An exact `reply` has no grammar to check it against, and a typo in one reads as still missing. A nonzero count or a placement's `changed` flag in a probe that does not need `damage-exact` is refused with the advice to write `N`. Each assertion follows a `check` with no input between, and each probe has a `want`, `want-re`, `reply` or `reply-re`.

## Terminfo probes

Terminfo probes are defined in `run.py`: the entry in `scripts/terminfo/kmscon.ti` is compiled with `tic` and read back with `infocmp`, and each probe names the capabilities it needs (`TERMINFO`) or the bytes `tput` must print for a string capability with given parameters (`TERMINFO_STRINGS`).

## Verdicts

The `capabilities` test fails when:

- a probe listed as `works` fails;
- a probe listed as `missing` passes, so the change that adds a feature also sets its line to `works` and closes its issue;
- a probe exists that the list does not name, or the list names a probe nothing defines.

A probe whose source is not built (the freetype backend) or whose tool is not installed (`tic`, `infocmp`, `tput`) is printed as `skip` and does not fail the run. Without `tic` and `infocmp` the meson build makes no termcap fixture, and `xtgettcap` is skipped.

The `capabilities-parity` test fails when a probe passes as a C program and fails as a stanza of `probes.replay`, or the other way, or when one source has a probe the other lacks.

## Placeholders

Some features have no interface to observe them through yet (OSC 8, OSC 52, OSC 133, bidirectional text). Their probes are placeholders that always fail and say so; the change that adds the feature replaces the placeholder with a real probe in the same commit that sets the line to `works`. Two probes expect keys interface 1 reserves for features it lacks (`overline`, `superscript`, `subscript`); their C twins check frame flags libtsm does not define yet (`TSM_FRAME_OVERLINE`, `TSM_FRAME_SUPERSCRIPT`/`SUBSCRIPT`).

## Adding a probe

Add a stanza to `probes.replay` (or a function in `test_capabilities_font.c`, or a `TERMINFO` or `TERMINFO_STRINGS` entry in `run.py`), and a line in `capabilities.list`. Until the C probe programs retire, a stanza needs its twin in `test_capabilities.c` or `test_capabilities_images.c`, or `capabilities-parity` fails. Plant the defect it exists to catch and see it red before trusting it, as for any check (`docs/fork/development.md`, A planted defect).
