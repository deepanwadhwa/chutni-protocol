# T03 — Clean-room Reader from the spec alone

**Priority:** P0 · **Size:** M · **Depends on:** T06 · **Spec:** §30.1, §31
**Status:** proposed 2026-07-30

## Why

A protocol whose spec has never been implemented by someone who didn't write
the reference code is a hypothesis. Chutni currently has exactly one
implementation, by one author, and every ambiguity in SPEC.md is invisible
because the reference implementation resolves it silently. The cheapest way to
find those ambiguities — and to prove the "Reader in an afternoon" claim T06
will make — is to implement a Reader **using only SPEC.md and
docs/IMPLEMENTERS.md**, with the reference sources off-limits.

This also produces the strongest possible review of the v0.2 additions: the
listing-hash canonicalization (§13.5) and the §35.1 compatibility rules are
exactly the kind of prose that reads clearly to its author and ambiguously to
everyone else.

## Deliverables

1. **`contrib/clean-room-reader/reader.py`** — a §30.1 Reader in ~150–300
   lines of Python. Allowed inputs: SPEC.md, IMPLEMENTERS.md, a store produced
   by the reference implementation. Forbidden: `src/`, `include/`, the
   existing tests. Allowed dependency: the `blake3` pip package (hashing is
   not the part of the spec under test; note `hashlib` has no BLAKE3).

   Must implement:
   - manifest validation (format, major version, hash algorithm);
   - listing sources and **active** artifacts via its own SQL against §10;
   - freshness for a file source and for an artifact, re-hashing the file
     (§13.3), including the derivation-inputs clause;
   - directory freshness by independently re-deriving the §13.5 listing hash
     — this is the sharpest spec test in the ticket;
   - the §35.1 rules: absent `max_depth` reported as unbounded; absent
     coverage manifest reported as **unknown**, never complete.

2. **`contrib/clean-room-reader/FINDINGS.md`** — every place the spec was
   ambiguous, silent, or wrong, written down *as encountered*, not
   reconstructed afterward. Each finding becomes either a SPEC.md fix or an
   explicit "the spec is right, the reader misread it" note. An empty findings
   file is a red flag, not a success.

3. **Cross-check harness**: a script that builds a fixture store with the
   reference CLI (bounded depth, a stale file, an opaque directory, a
   directory definition with inputs), then asserts the clean-room reader and
   `chutni --json` agree on: source list, active artifact set, each source's
   freshness, each directory's listing hash, and the coverage summary.

## Process rule

The clean-room constraint is the point. Whoever (or whatever agent) writes
`reader.py` must not open the reference sources; the cross-check harness is
written by someone else / a separate session that may. Record in FINDINGS.md
which session wrote what, so the claim "implemented from the spec alone" is
checkable.

## Acceptance criteria

- Cross-check harness passes: full agreement with the reference on the fixture
  store, including byte-identical listing hashes for every enumerated
  directory.
- FINDINGS.md exists with each item resolved (spec patch committed, or
  explicitly closed as reader error).
- Any SPEC.md changes ship in the same commit series with conformance
  coverage where they change behavior.

## Evidence required

Harness transcript plus FINDINGS.md itself, under `docs/evidence/`.

## Non-goals

- Not a supported product. `contrib/` status, clearly labeled: this exists to
  test the spec, and stays unmaintained by design (or graduates into T02's
  test suite if it earns it).
- No write operations — Reader level only (§30.1).

## Open questions

- If the listing hash cannot be reproduced from §13.5's prose alone, that is a
  **spec bug with data-compat consequences** — fix the prose, not the hash.
