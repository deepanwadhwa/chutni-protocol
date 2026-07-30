# T13 — Spec tiering: normative core vs guidance

**Priority:** P2 · **Size:** S · **Depends on:** nothing (pairs with T06) · **Spec:** all of SPEC.md, §30
**Status:** proposed 2026-07-30

## Why

SPEC.md mixes three kinds of text with equal visual weight: the normative core
(store shape, schema, hashing, validity — the part that defines
compatibility), role-conditional requirements (what a Producer or Gateway
must do), and guidance (§25 multimodal advice, §33 workflows, worked
examples). An implementer cannot tell at a glance which MUSTs apply to them,
so the effective cost of the spec is its full length. T06 solves this for
Readers with a separate doc; this ticket solves it *inside* the spec for
everyone else — without splitting the file, because section numbers (§13.3,
§39, …) are load-bearing references across the codebase and must not move.

## Deliverables

1. **A tier label on every section**, one line under each heading:
   - `Normative — Core` (any conforming store/reader touches this);
   - `Normative — <Role>` (Producer / Search Provider / Gateway / Application
     Host, matching §30's levels);
   - `Informative` (guidance, examples, rationale).
   Labels only — **zero renumbering, zero moved text, zero reworded
   requirements**. A diff reviewer must be able to confirm nothing normative
   changed by seeing only added label lines.
2. **Appendix: conformance checklists** — one table per §30 level listing
   every MUST that applies to it, each row citing its section. This is the
   machine-checkable inverse of the labels: a claim of "Chutni Producer 0.2"
   means every row in that table, nothing more. T06's Reader table becomes a
   generated-from/checked-against subset of this appendix rather than a
   second hand-maintained list.
3. **A conformance-language audit**, done while labeling: every MUST/SHOULD
   in an `Informative` section is a bug (either promote the section or demote
   the verb); every unimplemented MUST gets a row in TASKS.md §2 if it isn't
   there already (known cases at writing: parts of §27.4 permission levels,
   §28 client auth). The audit's findings list ships in the commit message.
4. **A short "how to read this spec" preamble** (~10 lines) after the
   abstract: what the tiers mean, where the checklists are, and the pointer
   to IMPLEMENTERS.md for the fast path.

## Acceptance criteria

- Every §-level section carries exactly one tier label; a grep for headings
  without labels returns empty (add that grep to `tests/conformance/run.sh`
  as a docs check so the invariant survives future sections).
- The appendix tables cite only sections labeled with the matching tier; each
  MUST in a normative section appears in at least one checklist row
  (spot-check in review; perfection not claimed).
- No section renumbering: every existing §-reference in `src/`, `include/`,
  `tests/`, and `docs/` still resolves (grep-verified list in the evidence).
- The audit found-and-fixed list is in the commit message, including verbs
  demoted or sections promoted.

## Evidence required

The grep transcripts (label coverage, reference integrity) under
`docs/evidence/` — this is a docs change, but the invariants are checkable
and should be checked.

## Non-goals

- No file split, no renumbering, no requirement changes beyond the
  verb-in-informative-section fixes the audit surfaces (each called out
  individually).
- No RFC-2119 boilerplate expansion beyond what §2 already says.
