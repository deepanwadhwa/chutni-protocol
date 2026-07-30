# T12 — Producer trust and store pollution

**Priority:** P2 · **Size:** M · **Depends on:** nothing · **Spec:** §16.4, §22, §23, §37.4
**Status:** proposed 2026-07-30

## Why

A shared store is only as trustworthy as its least careful writer. §16.4 is
clear that integrity ≠ semantic truth, but the practical consequence is
unaddressed: once two or three apps write to one store, a single sloppy or
malicious producer can flood it with junk summaries, wrong captions, or
adversarial text — and today a consumer has no ergonomic way to say "ignore
that producer", nor a user any way to evict one producer's output wholesale.
Left unsolved, apps will rationally prefer private silos to contamination,
which kills the shared-memory premise. The answer must stay inside Chutni's
philosophy: **no reputation scores, no standardized confidence** (§6.2) —
attribution plus consumer-side choice, not protocol-level judgment.

## Deliverables

1. **Producer filtering in search** (§19.2 addition): `producers` allow-list
   and `exclude_producers` deny-list on the search request (IDs, plus
   name-match convenience), implemented in `chutni_search` /
   `chutni_search_semantic`, exposed through `chutni_call` (T01), the CLI
   (`--producer`, `--exclude-producer`), and the MCP search tool. Results
   already carry `producer_id`; this closes the loop from "I can see who said
   it" to "I can act on that".
2. **Producer-scoped forget**: `chutni forget --producer <id> [--mode …]` —
   retire or purge every artifact whose derivation resolves to that producer,
   with the same §24.3 modes and the same honest non-erasure caveats. Must
   handle the cascade correctly: artifacts *derived from* the evicted
   producer's artifacts via `input_refs_json` go stale through the existing
   §13.3 machinery, not by special-casing.
3. **Producer inventory**: `chutni producers` — list producers with artifact
   counts (active/stale), kinds produced, first/last seen; the view a user
   needs before deciding to evict. JSON as always.
4. **Spec section: "Multi-producer hygiene"** (new, normative-light):
   - a consumer SHOULD attribute cross-producer artifacts when presenting
     them, and MUST NOT present another producer's `model_generated` text as
     verified (restating §22 with the multi-app framing);
   - a host SHOULD offer per-producer filtering to its user;
   - write access is store-wide by design in v0.2; per-producer write ACLs
     are explicitly deferred (§37.8) — say so rather than imply isolation
     that doesn't exist;
   - explicitly rejected: reputation scores, quality fields, trust ranks
     (§6.2 rationale extended). Attribution + user choice is the mechanism.
5. **Conformance scenario**: two producers write conflicting summaries for
   one source; a consumer filters to producer A only; producer B is evicted
   with `forget --producer`; A's artifacts survive untouched, B's dependents
   go stale, search never returns B afterward.

## Acceptance criteria

- The scenario above passes at C-API and CLI level; `make test` green.
- Filtering composes with existing filters (kinds, media types, stale) and
  with semantic search's profile gating — one combined test proves it.
- `forget --producer` on the reference store leaves counts consistent
  (`chutni info` totals reconcile before/after; no orphaned FTS rows —
  the existing index-consistency checks extended to cover it).
- Spec text and `docs/API-JSON.md` updated in the same series.

## Evidence required

Transcript of the two-producer conflict → filter → evict flow, under
`docs/evidence/`.

## Non-goals

- No signing/attestation of producer identity (§28.6 territory; a local
  process can claim any name today — say it plainly in the spec section).
- No automatic quality detection of any kind.
