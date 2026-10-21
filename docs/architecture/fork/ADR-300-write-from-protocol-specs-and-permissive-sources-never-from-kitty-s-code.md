---
contract: adr/v1
kind: decision
verb: constrain
capability: upstream
basis:
  - operator: aaronsb
    level: directed
    said: "without taking any code"
    via: Claude Code session, on taking inspiration from kitty's architecture
  - operator: aaronsb
    level: directed
    said: "What's the license? I would like to be unencumbered to allow anyone to have access to it"
    via: Claude Code session, while choosing a base terminal to extend
  - upstream: kmscon and libtsm are MIT-licensed (COPYING); kitty is GPL-3.0
  - standard: kitty terminal graphics protocol, https://sw.kovidgoyal.net/kitty/graphics-protocol/
agent:
  name: claude
  model: claude-opus-5-5
considered:
  - operator: aaronsb
    said: "I read through the ADRs and they make sense to me and line up with what we discussed. I don't see any surprise decisions"
    via: Claude Code session, reviewing PR aaronsb/kmscon#1
status: accepted
date: 2026-10-20
deciders:
  - aaronsb
related: []
---

# ADR-300: Write from protocol specs and permissive sources, never from kitty's code

## Summary

- **Decided:** Code in this fork is written from published protocol specifications, architecture described in our own words, and permissively licensed implementations. No source from a GPL-licensed terminal, kitty above all, is read while writing it, by a person or by an agent.
- **Trades away:** The fastest route to kitty-compatible behaviour, which is reading how kitty does it. Edge cases the spec leaves open have to be found by testing against kitty as a black box.
- **One-way?** Yes. GPL code that enters the tree cannot be removed by a later commit alone; the fork's licence and its path upstream both depend on it never entering.
- **Probes:** *Confident:* the fork stays MIT, so kmscon can take any of it and anyone can use it. *Not confident:* that the kitty protocol docs answer every edge case without consulting kitty's behaviour, which black-box testing has to cover.
- **Inversion:** Between "ignore kitty" and "port kitty". The answer sits outside both: take kitty's protocol and its architectural ideas, which are not copyrightable, and none of its expression.

## Context

The fork adds terminal graphics and text features that kitty defined or popularised: the kitty graphics protocol, styled and coloured underlines, and the cell, sprite and image-layer architecture kitty uses to draw them. kitty is GPL-3.0. kmscon and libtsm are MIT, and the work is meant to stay usable by anyone and acceptable to kmscon upstream. Code derived from kitty's source, including a translation into C or a close paraphrase of its structure, would carry the GPL into the fork.

## Decision

Sources are classed by what they may contribute.

| Source | May be used | How |
|---|---|---|
| Protocol specifications: kitty's graphics, keyboard and underline docs; ECMA-48; xterm ctlseqs; DEC STD 070 and VT340 sixel documentation | Yes | Implement from the described behaviour and cite the document. Do not copy its prose into code comments or docs. |
| Architecture described at the level of ideas: "a sprite cache keyed by glyph", "placements with a z-index", "the screen notifies the image layer on scroll" | Yes | Write it in our own words in a record; implement from the record. |
| Permissively licensed implementations: Ghostty (MIT), WezTerm (MIT), foot (MIT), libsixel (MIT) | Yes, as reference | Attribute anything adapted, in the file and the commit. |
| kitty's source: its C, Python and GLSL | No | Not opened while working on this fork. |
| kitty as a running program | Yes | Black-box comparison: send the same bytes to kitty and to this fork, compare the screens. |

Every agent brief for work in this fork states the rule in the "No" row, so a helper cannot fill a gap by looking at kitty's code. A file adapted from a permissive source names the source and its licence in its header.

The constraint covers all code and documentation in this fork. It ends only if the fork deliberately relicenses to GPL-3.0, which this record does not anticipate.

## Consequences

### Positive

- The fork stays MIT, the same licence as kmscon and libtsm, so any part of it can be offered upstream without a licence question.
- Anyone, including closed-source projects, can use the work.
- Recording the rule makes it checkable: a reviewer can ask where a piece of behaviour came from.

### Negative

- Behaviour the specifications leave open has to be discovered by black-box testing against kitty, which is slower than reading its code.
- Permissive implementations are in other languages (Zig, Rust), so they guide design but rarely supply code directly.

### Neutral

- The conformance harness gains a role: kitty as a running reference for the graphics protocol.

## Alternatives Considered

- **Port kitty's implementation.** Fastest and most faithful, and rejected because it makes the fork GPL-3.0, which conflicts with both the "anyone can use it" goal and an upstream that is MIT.
- **Avoid kitty entirely, protocol included.** Rejected: the kitty graphics protocol is the most capable inline image protocol and is implemented independently by several MIT terminals. Its specification is free to implement.
- **Use kitty's source "only for reference".** Rejected: reading the code and then writing "from memory" is how derived code gets in without anyone intending it, and it cannot be audited afterwards.
