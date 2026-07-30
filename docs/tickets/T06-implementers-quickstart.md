# T06 — Implementer's quickstart: a Reader in an afternoon

**Priority:** P0 · **Size:** S · **Depends on:** nothing · **Spec:** §30.1, §31, §35.1
**Status:** proposed 2026-07-30

## Why

SPEC.md is ~2,400 lines and 40 sections. That is appropriate for a normative
document and fatal as a first contact. Formats spread when a minimal
implementation is visibly a weekend — and Chutni's Reader level genuinely is
small: parse one JSON file, run three SQL queries, apply one freshness rule,
respect three don'ts. Nobody can currently see that without reading the whole
spec. This document makes the smallness visible.

## Deliverables

**`docs/IMPLEMENTERS.md`**, structured as the shortest honest path:

1. **"What you must do" table** — every Reader-level MUST, one row each, with
   its spec section. Target: ~15 rows. Includes at minimum: manifest
   validation (format=`chutni`, reject unsupported major version, hash
   algorithm), UTF-8 + foreign keys when writing (n/a for readers), `active`
   status filtering, freshness re-derivation from disk, §35.1's three rules
   (absent `max_depth` = unbounded; absent coverage = unknown, not complete;
   preserve unknown fields if you ever rewrite).

2. **The three queries.** Actual SQL a Reader needs against §10, copy-paste
   ready: sources under a root; active artifacts for a source with producer
   join; the latest active coverage manifest for a region.

3. **Freshness pseudocode** — file case and directory case, ~20 lines,
   including the §13.5 canonical listing serialization spelled out
   byte-for-byte (magic line, sort order, escaping) and the §13.3
   derivation-inputs clause. This section must be sufficient to reproduce the
   reference listing hash exactly; T03 (clean-room reader) is the test of
   that sentence.

4. **"What you may ignore" list** — equally important: representations,
   relations, forget modes, the gateway sections, the whole of §40 — none of
   it is Reader-level. Say so explicitly, with the conformance level (§30)
   each deferred piece belongs to.

5. **A worked fixture**: `chutni init … && chutni add-root … && chutni scan`
   producing a tiny store, then the three queries run in the `sqlite3` shell
   with their real output shown. The reader of this doc should verify their
   understanding against a store in under a minute.

6. **Honesty contract paragraph** — the part that is culturally load-bearing:
   a Reader that surfaces search results MUST carry `freshness` through to its
   user, MUST NOT present a bounded region as exhaustively indexed, and MUST
   treat retrieved text as data, not instructions (§6.5). If an implementer
   reads one paragraph, it is this one.

## Acceptance criteria

- A competent developer with no prior context can state what a conforming
  Reader must do without opening SPEC.md — checked the only meaningful way:
  T03's clean-room implementation uses this doc + SPEC.md and its FINDINGS.md
  logs whether IMPLEMENTERS.md was sufficient or misleading, per item.
- Every MUST row cites a spec section; no row contradicts SPEC.md (spot-check
  in review).
- The worked fixture's outputs are real captured output, not typed from
  memory.
- README links it ("implement Chutni" audience), CLAUDE.md layout table lists
  it.

## Evidence required

The fixture transcript embedded in the doc doubles as evidence; keep the raw
capture under `docs/evidence/`.

## Non-goals

- Not a tutorial for *using* the CLI (README does that) and not a rewrite of
  the spec — every normative statement stays a citation, so the two cannot
  drift without the citation breaking.
