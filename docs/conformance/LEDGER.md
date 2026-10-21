# Conformance ledger

One row per committed contact sheet, oldest first. Each sheet is drawn from a single run of the harness (ADR-301): `make sheet NOTE="..."` runs the tiers, writes the sheet under `sheets/`, and appends its row here. The kmscon and libtsm columns name the commits that run measured.

| Date | Note | kmscon | libtsm | Tier 1 | Tier 2 | Tier 3 | Sheet |
|---|---|---|---|---|---|---|---|
| 2026-10-21 | Baseline: tier 1 harness against upstream libtsm v4.8.0 behaviour | `84135a3` | `c2feb28` | 26/26 | — | — | [2026-10-21-84135a3-c2feb28.svg](sheets/2026-10-21-84135a3-c2feb28.svg) |
