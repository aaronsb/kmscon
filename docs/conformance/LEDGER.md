# Conformance ledger

One row per recorded run of the harness (ADR-301), oldest first. `make sheet NOTE="..."` runs the tiers, writes the run's page under `runs/`, and appends its row here. The kmscon and libtsm columns name the commits that run measured. The first three runs were drawn as SVG contact sheets, since removed from the tree; their rows link to each sheet at commit `627f3d1`.

| Date (UTC) | Note | kmscon | libtsm | Tier 1 | Tier 2 | Tier 3 | Run |
|---|---|---|---|---|---|---|---|
| 2026-10-21 | Baseline: tier 1 harness against upstream libtsm v4.8.0 behaviour | `84135a3` | `c2feb28` | 26/26 | — | — | [2026-10-21-84135a3-c2feb28.svg](https://github.com/aaronsb/kmscon/blob/627f3d1/docs/conformance/sheets/2026-10-21-84135a3-c2feb28.svg) |
| 2026-10-21 | Tier 2 joins: renderer pixels in memory through the real drm2d blend | `9cf178b` | `c2feb28` | 26/26 | 6/6 | — | [2026-10-21-9cf178b-c2feb28.svg](https://github.com/aaronsb/kmscon/blob/627f3d1/docs/conformance/sheets/2026-10-21-9cf178b-c2feb28.svg) |
| 2026-10-21 | Tier 3 joins: installed package in a KVM guest, 2D variants | `2862aeb` | `c2feb28` | 26/26 | 6/6 | 10/10 | [2026-10-21-2862aeb-c2feb28.svg](https://github.com/aaronsb/kmscon/blob/627f3d1/docs/conformance/sheets/2026-10-21-2862aeb-c2feb28.svg) |
