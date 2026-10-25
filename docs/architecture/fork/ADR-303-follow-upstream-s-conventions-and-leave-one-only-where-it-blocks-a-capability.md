---
contract: adr/v1
kind: decision
verb: constrain
capability: [upstream, process]
basis:
  - operator: aaronsb
    level: directed
    said: "we should try to stay as close to kmscon's conventions, but of course, we need to leave them if they prevent us from implementing our new capabilities or otherwise impinge on them"
    via: Claude Code session, 2026-10-24, steering the gltex image work, relayed verbatim by the team lead
  - operator: aaronsb
    level: directed
    said: "it should be the same hw accel method that kmscon already uses - not anything new or special. that should be in the adr"
    via: Claude Code session, 2026-10-24, about hardware acceleration for images, relayed verbatim by the team lead; the session answered that ADR-108 already records it, and it is cited here as an instance of the general rule
  - operator: aaronsb
    level: guided
    said: "let's avoid growing beyond getting all the parts working first. publishing shape and prepping for upstream merge isn't our focus yet"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead
  - operator: aaronsb
    level: guided
    said: "my theory is the upstream will likely not converge on the work we've been doing, but we've been performing the necessary changes to facilitate the expansion of graphics and text capabilities"
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead while ADR-303 was in review
  - operator: aaronsb
    level: guided
    said: "but if we keep an eye on how it's constructed, we follow the conventions and the upstream changes shouldn't be as disruptive."
    via: Claude Code session, 2026-10-24, relayed verbatim by the team lead while ADR-303 was in review, said right after the statement above
  - operator: aaronsb
    level: guided
    said: "we need to prove the merit of our implementation - all the planned capabilities we're working on right now. when we figure that out and it fits our criteira, the heavy development phase will be relatively complete - thats when cleaning it up and making it appealing for a possible upstream merge happens (which is why its not in this session)"
    via: Claude Code session, 2026-10-25, relayed verbatim by the team lead while ADR-303 was in review
  - operator: aaronsb
    level: guided
    said: "if you consider images, gpu path (with cpu fall back) and modes, then we've basically maintained consistency with upstream kmscon and libtsm. it should be relatively straightforward to bring their changes into our fork moving forward, and when we have some turns in our history of doing that, thats when I bring up the work we've been doing and show it."
    via: Claude Code session, 2026-10-25, relayed verbatim by the team lead while ADR-303 was in review
  - precedent: ADR-108
  - precedent: ADR-101
  - precedent: ADR-102
  - evidence: "aaronsb/kmscon#133: the image settings join upstream's option table in its style. `sixel` and `kitty-graphics` are CONF_OPTION_BOOL options, negated with the `no-` prefix upstream's conf code adds to every boolean, and the settings xterm already has keep xterm's names (`max-graphic-size`, `num-color-registers`, `private-color-registers`). Read in src/config.c and src/misc/conf.c at 15ee162"
  - evidence: "aaronsb/kmscon#62, the first upstream sync (merge commit e2aab94): upstream's W cell-width calibration and half-pixel size request were taken as upstream wrote them, though they moved every tier 2 golden; the fork's font_box.c was kept over upstream's new blocks.c, whose hook takes one codepoint where the fork passes clusters, and blocks.c was deleted because nothing called it. The merge message records both choices"
agent:
  name: claude
  model: claude-opus-5-5
considered:
  - operator: aaronsb
    said: "Picked \"Yes\" to: when kmscon already has a way of doing something, new work uses it even when a fresh design would be cleaner or cheap to build, and the fork goes its own way only when kmscon's way would stop the new feature or hobble it."
    via: Claude Code session, 2026-10-24, the team lead put both probes to the operator as multiple-choice questions
    paraphrase: true
    covers: [inconvenient-not-enough]
  - operator: aaronsb
    said: "Picked \"User-visible only\": internal departures (an interface, file layout) are recorded without asking; anything users would notice (a changed default, a new or renamed option, a different fallback) comes to the operator before merge."
    via: Claude Code session, 2026-10-24, the team lead put both probes to the operator as multiple-choice questions
    paraphrase: true
    covers: [sign-off]
  - operator: aaronsb
    said: "Picked \"Case by case\": when a sync brings upstream's own solution to something the fork solved differently, which one stays is decided at that sync and recorded in the merge message."
    via: Claude Code session, 2026-10-24, the team lead put the probes and the review's questions to the operator as multiple-choice questions
    paraphrase: true
    covers: [sync-rule]
  - operator: aaronsb
    said: "Picked \"No\": a measured performance shortfall alone does not qualify as impinging; only a capability that cannot be built, or would be misleading or unusable, does."
    via: Claude Code session, 2026-10-24, the team lead put the probes and the review's questions to the operator as multiple-choice questions
    paraphrase: true
    covers: [performance]
status: proposed
date: 2026-10-24
deciders:
  - aaronsb
related: []
---

# ADR-303: Follow upstream's conventions, and leave one only where it blocks a capability

## Summary

- **Decided:** New fork work follows upstream kmscon's and libtsm's conventions by default: options and their defaults, fallbacks, backend and renderer selection, naming, file layout, and behaviour users already rely on. The fork leaves a convention only where following it would make a capability the fork is adding impossible to build, misleading or unusable, and each departure states that failure as something a reviewer can observe. A cleaner design does not qualify, and neither does a measured performance shortfall alone. Where upstream has no way of doing something, there is no convention and this record does not apply. When an upstream sync brings upstream's own solution to something the fork solved another way, which one stays is decided at that sync. The operator's two reasons: users keep the behaviour they expect, and a fork built the way upstream is built takes upstream's changes with less disruption, though upstream is not expected to converge on the fork's work.
- **Trades away:** Freedom to redesign upstream's surfaces where they are merely inconvenient or slower. Some fork code will be shaped around an upstream choice the fork would not have made.
- **One-way?** No. Reversible: this record changes no code and no interface, and a later record can loosen or tighten the test.
- **Probes:**
  1. *Confident (inconvenient-not-enough):* When kmscon already has a way of doing something, new work uses it even when a fresh design would be cleaner or cheap to build; the fork only goes its own way when kmscon's way would stop the new feature or hobble it. Right? Answered yes.
  2. *Not confident (sign-off):* When the fork does have to leave one of kmscon's conventions, it is written down in the record or PR that needs it, and brought to you as a question before merge only when users would notice: a changed default, a new or renamed option, a different fallback. Departures users cannot see, such as an internal interface or a file layout the new code needs, are written down without asking you. Is that the line you want, or do you want to hear about every departure? Answered "user-visible only".
  3. *Not confident (sync-rule):* When an upstream sync brings upstream's own solution to something the fork already solved another way, does the fork's code always stay, does the fork always move to upstream's, or is it decided case by case at that sync? Answered "case by case", with the choice recorded in the merge message.
  4. *Not confident (performance):* If kmscon's way works for a new feature but is measurably slower, is that enough on its own to leave it? Answered no.
- **Inversion:** Between "fork freely" and "never depart from upstream". Upstream's convention is the default, and a capability is the only reason to leave it.

## Context

The fork adds capabilities upstream does not have, such as inline images, a rebuilt cell model and new text styles, and each one meets an upstream convention it could follow or replace. The operator has given two reasons to follow one. Users keep the behaviour they expect: kmscon's options, defaults and fallbacks behave as users already know them, in the operator's words for ADR-108, "this way we're not changing expected behaviors". And a fork built the way upstream is built takes upstream's changes with less disruption: "if we keep an eye on how it's constructed, we follow the conventions and the upstream changes shouldn't be as disruptive."

The record does not assume upstream will converge on the fork's work. The operator expects it will not: "my theory is the upstream will likely not converge on the work we've been doing, but we've been performing the necessary changes to facilitate the expansion of graphics and text capabilities". The departures are those changes, made for the graphics and text capabilities. For the same reason, upstream gaining an equivalent does not end a departure on its own, and each overlap a sync brings is decided at that sync (point 6).

The fork is in its heavy development phase, proving the planned capabilities against their criteria. Cleanup, and shaping the fork for a possible upstream merge, come in a later phase: "we need to prove the merit of our implementation - all the planned capabilities we're working on right now. when we figure that out and it fits our criteira [sic], the heavy development phase will be relatively complete - thats when cleaning it up and making it appealing for a possible upstream merge happens (which is why its not in this session)". Offering work upstream is therefore not a reason this record gives, and the record adds no upstreaming work now; as the operator put it earlier, "let's avoid growing beyond getting all the parts working first. publishing shape and prepping for upstream merge isn't our focus yet".

Following upstream's conventions is what keeps regular upstream syncs straightforward, and a history of such syncs comes before the fork's work is shown upstream. The operator judges that the fork has kept that consistency so far, and sets the order: "if you consider images, gpu path (with cpu fall back) and modes, then we've basically maintained consistency with upstream kmscon and libtsm. it should be relatively straightforward to bring their changes into our fork moving forward, and when we have some turns in our history of doing that, thats when I bring up the work we've been doing and show it." Routine upstream syncs during development, such as [development.md](../../fork/development.md)'s bring-home sync, build that history and are part of this phase; shaping the fork for an upstream merge is not. Presenting the fork's work upstream is the operator's step, taken after that history exists.

This record makes explicit a practice the fork's merged work already follows:

- #175, ADR-108: images join kmscon's `hwaccel` setting, its off default and its fallback, with no image setting of their own.
- #133: the image settings join upstream's option table in its style: `sixel` and `kitty-graphics` are booleans negated with upstream's `no-` prefix, and the settings xterm already has keep xterm's names.
- #1, ADR-101: libtsm's screen model has no room for colour glyphs, styled underlines or inline images (ADR-101's Context), so the fork replaces it and keeps kmscon's platform layer and libtsm's parser.
- #20, ADR-102: clusters cannot form under upstream's per-codepoint `wcwidth`, so Unicode grapheme width becomes the default. Upstream's method stays available as `grapheme-width=wcwidth`, and the default was put to the operator before it was decided.
- #62, the first upstream sync: upstream's cell-width calibration was taken as upstream wrote it, though it moved every tier 2 golden, and the fork's `font_box.c` was kept over upstream's new `blocks.c`, which takes one codepoint where the fork passes clusters. The merge message records both choices.

## Decision

1. **Upstream's convention is the default.** A convention is what upstream kmscon and libtsm do today, as brought in by the latest upstream sync: options and their defaults, fallbacks, backend and renderer selection, naming, file layout, and behaviour users already rely on. New work follows it unless point 2 applies. Where upstream has no way of doing something, there is no convention to follow and this record does not apply; the new design is the capability's own decision.
2. **A departure needs a capability the convention blocks.** The fork leaves a convention only where following it would make a capability the fork is adding impossible to build, or would leave it misleading or unusable. A design that is cleaner, more consistent with the fork's other code, or cheap to build does not qualify, and neither does a measured performance shortfall alone.
3. **Every departure is recorded, with its failure stated as an observable.** The record names the convention; what goes wrong under it, as something a reviewer can check: a behaviour that cannot be produced, or a state that misleads users or programs or leaves the capability unusable; the scope of the departure (which options, files, interfaces or behaviours); and the condition that would end it, such as the capability being retired. It goes in the decision record for the capability, or in the PR body where no record covers the change. The departing commit's message names the departure as well, or a one-line comment at the departure points to the record or PR, so `git blame` reaches it from the code.
4. **A departure users would notice is raised with the operator.** A departure that changes a default, adds or renames an option, changes a fallback, or changes other behaviour users rely on is raised as a numbered question before merge. A departure users cannot see is recorded under point 3 without that question.
5. **The departure stays as small as the capability needs.** The fork replaces the part of a convention that blocks the capability and keeps the rest of it.
6. **An upstream sync decides each overlap case by case.** When a sync brings upstream's own solution to something the fork solved another way, which one stays is decided at that sync, and the merge message records the choice and its reason. Upstream gaining an equivalent does not end a departure by itself, and a sync that changes a convention does not by itself rework fork code that followed the old one; new work follows the convention as the sync leaves it.

[docs/fork/development.md](../../fork/development.md)'s "Changes the operator decides" raises a departure from an accepted ADR, or from a PR's plan where no ADR covers it, with the operator as a change of approach, and raises a choice an ADR leaves open when users or other terminals would notice it. Leaving an upstream convention under point 2 follows this record, so that rule alone would not reach it; point 4 applies the same noticeability line to it. A departure that also changes an accepted ADR or the PR's plan is still a change of approach under development.md. Point 6 is the rule development.md's "The upstream sync" follows for overlapping solutions.

## Consequences

### Positive

- Users moving between upstream kmscon and the fork find the same options, defaults and fallbacks wherever the fork adds nothing.
- Upstream syncs disrupt less, because the fork is built the way upstream is built wherever a capability does not require otherwise.
- A later session can tell a deliberate departure from drift, because each departure names its reason and its end condition, and the code leads to it.

### Negative

- Some new code is shaped around upstream choices the fork would not have made, and carries their limits, including where upstream's way is slower.
- Whether a convention makes a capability misleading or unusable is still a judgement. The observable failure the departure states is what review checks.
- A sync that overlaps fork work needs a decision at the sync, recorded in its merge message, and no standing rule settles it in advance.

### Neutral

- Departures made before this record, such as ADR-101's replaced screen model and ADR-102's cell layout, already have their reasons in their own records and are not revisited.
- A sync that changes a convention changes what new work follows; existing fork code moves only where a sync decides it (point 6).
- The record adds no work to shape the fork for an upstream merge. That belongs to a later phase, once the planned capabilities are proven against their criteria and the fork has a history of regular upstream syncs. Until then the fork neither presents nor prepares work for upstream.

## Alternatives Considered

- **Fork freely.** Redesign any upstream surface the fork touches. Rejected: it changes behaviour users rely on without a capability to show for it, and makes every upstream sync more disruptive.
- **Never depart from upstream.** Rejected: several of the fork's capabilities cannot be built inside upstream's screen model and draw API (ADR-101, ADR-102).
- **Record departures without a test.** Allow any departure as long as it is written down. Rejected: a record alone does not stop departures made for taste, and the operator's test is whether the convention blocks a capability.
- **Let a measured performance shortfall qualify.** Rejected by the operator's answer to the performance probe.
- **A fixed sync rule.** Always keep the fork's code, or always move to upstream's, when a sync overlaps fork work. Rejected by the operator's answer to the sync-rule probe; development.md's earlier rule kept the fork's code every time.
