---
contract: adr/v1
kind: decision
verb: constrain
capability: upstream
basis:
  - operator: aaronsb
    level: directed
    said: "we should try to stay as close to kmscon's conventions, but of course, we need to leave them if they prevent us from implementing our new capabilities or otherwise impinge on them"
    via: Claude Code session, 2026-10-25, steering the gltex image work, relayed verbatim by the team lead
  - operator: aaronsb
    level: directed
    said: "it should be the same hw accel method that kmscon already uses - not anything new or special. that should be in the adr"
    via: Claude Code session, 2026-10-25, about hardware acceleration for images, relayed verbatim by the team lead
  - precedent: ADR-108
  - precedent: ADR-101
agent:
  name: claude
  model: claude-opus-5-5
status: proposed
date: 2026-10-25
deciders:
  - aaronsb
related: []
---

# ADR-303: Follow upstream's conventions, and leave one only where it blocks a capability

## Summary

- **Decided:** New fork work follows upstream kmscon's and libtsm's conventions by default: options and their defaults, fallbacks, backend and renderer selection, naming, file layout, and behaviour users already rely on. The fork leaves a convention only where it prevents a capability the fork is adding or impinges on it, and records each departure: which convention, why it blocks the capability, how far the departure reaches, and what would end it. A departure that is only nicer does not qualify.
- **Trades away:** Freedom to redesign upstream's surfaces where they are merely inconvenient. Some fork code will be shaped around an upstream choice the fork would not have made.
- **One-way?** No. Reversible: this record changes no code and no interface, and a later record can loosen or tighten the test.
- **Probes:**
  1. *Confident (inconvenient-not-enough):* When kmscon already has a way of doing something, new work uses it even when a fresh design would be cleaner or cheap to build; the fork only goes its own way when kmscon's way would stop the new feature or hobble it. Right?
  2. *Not confident (sign-off):* When the fork does have to leave one of kmscon's conventions, it is written down in the record or PR that needs it, and brought to you as a question before merge only when users would notice: a changed default, a new or renamed option, a different fallback. Departures users cannot see, such as an internal interface or a file layout the new code needs, are written down without asking you. Is that the line you want, or do you want to hear about every departure?
- **Inversion:** Between "fork freely" and "never depart from upstream". The answer sits between: upstream's convention is the default and a capability is the only reason to leave it.

## Context

The fork adds capabilities upstream does not have, such as inline images, a rebuilt cell model and new text styles, and each one meets an upstream convention it could follow or replace. Users already know kmscon's options, defaults and fallbacks, upstream syncs merge more easily where the fork's code has upstream's shape, and work offered back is easier to take when it fits upstream's conventions. ADR-108 is the worked example: images join kmscon's existing `hwaccel` setting, its off default and its fallback, with no image setting of their own. ADR-101 shows a departure the test allows: libtsm's screen model cannot carry clusters, sprites and an image layer, so the fork replaces it and keeps kmscon's platform layer.

## Decision

1. **Upstream's convention is the default.** A convention is what upstream kmscon and libtsm do today, as brought in by the latest upstream sync: options and their defaults, fallbacks, backend and renderer selection, naming, file layout, and behaviour users already rely on. New work follows it unless point 2 applies.
2. **A departure needs a capability it unblocks.** The fork leaves a convention only where following it would prevent a capability the fork is adding, or impinge on it: the capability could be built but would be crippled, misleading or unusable. A design that is cleaner, more consistent with the fork's other code, or cheap to build is not enough on its own.
3. **Every departure is recorded.** The record names the convention, why it blocks the capability, the scope of the departure (which options, files, interfaces or behaviours), and the condition that would end it, such as upstream gaining an equivalent or the capability being retired. It goes in the decision record for the capability, or in the PR body where no record covers the change.
4. **A departure users would notice is raised with the operator.** A departure that changes a default, adds or renames an option, changes a fallback, or changes other behaviour users rely on is raised as a numbered question before merge. A departure users cannot see is recorded under point 3 without that question.
5. **The departure stays as small as the capability needs.** The fork replaces the part of a convention that blocks the capability and keeps the rest of it.

[docs/fork/development.md](../../fork/development.md)'s "Changes the operator decides" raises a departure from an accepted ADR, or from a PR's plan where no ADR covers it, with the operator as a change of approach, and raises a choice an ADR leaves open when users or other terminals would notice it. Leaving an upstream convention under point 2 follows this record, so that rule alone would not reach it; point 4 applies the same noticeability line to it. A departure that also changes an accepted ADR or the PR's plan is still a change of approach under development.md.

## Consequences

### Positive

- Users moving between upstream kmscon and the fork find the same options, defaults and fallbacks wherever the fork adds nothing.
- Upstream syncs conflict less, and fork work offered upstream fits upstream's shape.
- A later session can tell a deliberate departure from drift, because each departure names its reason and its end condition.

### Negative

- Some new code is shaped around upstream choices the fork would not have made, and carries their limits.
- Whether a convention impinges on a capability is a judgement, and two sessions can draw the line differently. The recorded reason is what review checks.

### Neutral

- Departures made before this record, such as ADR-101's replaced screen model and ADR-102's cell layout, already have their reasons in their own records and are not revisited.
- When an upstream sync changes a convention, the fork's default moves with it.

## Alternatives Considered

- **Fork freely.** Redesign any upstream surface the fork touches. Rejected: it changes behaviour users rely on without a capability to show for it, and it widens the diff every sync and every offer upstream has to cross.
- **Never depart from upstream.** Rejected: several of the fork's capabilities cannot be built inside upstream's screen model and draw API (ADR-101, ADR-102).
- **Record departures without a test.** Allow any departure as long as it is written down. Rejected: a record alone does not stop departures made for taste, and the operator's test is whether the convention blocks a capability.
