---
contract: adr/v1
kind: decision
verb: change
capability: process
amends: "ADR-301#amendment-2026-10-20-three-verdicts-and-marked-differences"
basis:
  - operator: aaronsb
    level: directed
    said: "compose in a markdown file with comparison contact image sheets, rather than a giant high resolution page that doesn't make much sense to read if you don't know the context"
    via: Claude Code session, relayed to the implementing agent by the team lead
  - operator: aaronsb
    level: directed
    said: "this should be able to be auto generated"
    via: Claude Code session, relayed to the implementing agent by the team lead
  - operator: aaronsb
    level: directed
    said: "we really only need to make these when we've sort of 'brought home' all the workstreams/branch merges"
    via: Claude Code session, relayed to the implementing agent by the team lead
  - operator: aaronsb
    level: directed
    said: "unless we're performing comparative analysis that needs it as part of ongoing dev on the branch"
    via: Claude Code session, relayed to the implementing agent by the team lead
  - precedent: ADR-301
agent:
  name: claude
  model: claude-opus-5-5
status: proposed
date: 2026-10-22
deciders:
  - aaronsb
related: []
---

# ADR-302: Record conformance runs as markdown pages, committed at bring-home

## Summary

- **Decided:** A conformance run is recorded as a generated markdown page with small images beside it, replacing the single SVG contact sheet. The page is written for a reader who was not there: what the run marks, the commits it measured, what changed since the previous recorded run, and for each case what it checks and its verdict. A committed record, a page plus its ledger row, is made once per working session, on main, after the session's branches have merged. Pages for comparing runs during work on a branch are generated as often as needed and never committed.
- **Trades away:** The ledger no longer records every notable run, only one per session; a regression that appears and is fixed within a session leaves no committed page. The tooling is bigger than a sheet drawer: it reads git history for golden changes and keeps a per-case record for the next run.
- **One-way?** No. The pages are generated from reports, the three committed SVG sheets stay as they are, and the cadence is a convention backed by one check in `make sheet`.
- **Probes:** *Confident (cadence):* one committed record per session at bring-home is what you want, with previews and comparison pages covering in-session needs. *Not confident (old sheets):* leaving the three SVG sheets as they are, with no pages generated for them, is acceptable.
- **Inversion:** Between "commit a record of every run" (the ledger and the repository grow with every change, and most records are noise) and "commit nothing until a release" (no trail of how conformance moved). The answer sits between: one record per session, at the point where the session's work has landed.

## Context

ADR-301's amendment of 2026-10-20 made the run record a contact sheet: one SVG with a tile per case, differing regions outlined. In practice the sheet was a single very large high-resolution image, and it made sense only to someone who already knew the harness. The first committed run of the new page format also showed that a ledger row per pull request records work still in flight, while the meaningful point is when a session's branches have all merged.

## Decision

- **The run record is a markdown page.** `make sheet` writes `docs/conformance/runs/<date>-<kmscon>-<libtsm>/README.md` and its PNGs. Images are cropped to the drawn part of each screen and kept small; tier 1 is shown as text. A failing case, or one whose golden changed since the previous recorded run, shows golden, actual and difference. The page is generated from the run's reports alone, the same reports give the same bytes, and captions come from where each case is defined.
- **Differences are marked as the reader needs them.** On the run page a difference image shows each differing pixel in red. On a comparison page between two runs, each differing region is outlined, numbered, and named by terminal row, column and the text drawn there. This replaces the amended rule that the contact sheet outlines regions on the golden and actual images, and that tier 1 outlines cells; tier 1 failures are shown as a text diff.
- **A committed record is made at bring-home.** Once per working session, after the session's branches have merged and main is green, `make sheet NOTE="..."` is the first thing run on main. It refuses a tree with uncommitted changes and a HEAD that is not `origin/main`; `SHEET_ANY_COMMIT=1` overrides the second check.
- **Comparative analysis is never committed.** `make sheet-preview` writes the run page under `fork/build/`. `make sheet-save` keeps a run, and `make sheet-compare` pairs two saved runs screen by screen. Both run as often as work on a branch needs.

## Consequences

### Positive

- A reader who was not there can tell what a run shows and why each case matters.
- The ledger records points where the fork's state is settled, not work in flight.
- The comparison page can set upstream kmscon beside the fork as evidence, and before and after on a branch.

### Negative

- A run page reads git history (`golden-history.sh`), which runs on the host and fetches the previous libtsm commit into a shallow subproject.
- One record per session depends on the session reaching its bring-home point; a session that ends with branches unmerged records nothing.

### Neutral

- Each page writes a `cases.json` that the next run compares against. The three earlier SVG runs have none, so the first page judges their verdicts from their pass counts.

## Alternatives Considered

- **Keep the SVG and make it smaller.** Rejected: the problem was the form as much as the size. One image cannot carry the explanation, captions and text diffs a reader without context needs.
- **Commit a page with every pull request.** Rejected by the operator's cadence: it records work still in flight and churns the ledger.
- **Generate pages only for releases.** Rejected: it leaves no trail between releases, and the fork's case rests on showing conformance as upstream moves.
