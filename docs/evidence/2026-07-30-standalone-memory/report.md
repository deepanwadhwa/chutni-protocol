# Standalone application and model memory

**Date:** 2026-07-30
**Machine:** macOS 15, Darwin 25.5.0 arm64, Apple clang 21.0.0 (clang-2100.1.1.101).
**The only machine any of this has run on.**
**Spec:** [SPEC.md](../../../SPEC.md) v0.2-draft, §12.6, §15.2, §20
**Implementation:** `0.3.0`

What was built: a store can now retain knowledge and work products that were
never derived from a file — notes, decisions, plans, conversation summaries,
analyses. `chutni_memory_put` (typed C), `put_memory` (JSON call surface), and
`chutni_put_memory` (MCP), plus the Python binding that follows the C ABI.

## Transcripts in this directory

| File | What it is |
|---|---|
| [memory-round-trip.txt](memory-round-trip.txt) | A real store: write, refuse, search, inspect, verify, forget |
| [make-test.txt](make-test.txt) | Full suite, exit 0 |
| [make-sanitize.txt](make-sanitize.txt) | Conformance, call surface, CLI and MCP under ASan + UBSan, exit 0 |

## Suite result

```
105 BLAKE3 official vectors, 0 failures
 79 conformance assertions, 0 failed, 1 declared gap
 42 call-surface checks, 0 failed
 32 CLI checks, 0 failed
124 MCP checks, 0 failed
  7 Samosa compatibility operations passed
  1 Python binding test, OK
```

`make test` and `make sanitize` both exit 0. The sanitize run produced no
AddressSanitizer, UndefinedBehaviorSanitizer, or LeakSanitizer diagnostics.

The one gap is unchanged and unrelated: scenario 3, moved-root remapping
(§26), still unimplemented and still reported as `GAP` rather than counted as
a pass.

`make sanitize` covers the conformance, call-surface, CLI, and MCP suites. It
does **not** run the Samosa compatibility contract or the Python binding under
sanitizers.

## What the round trip actually shows

Full output in [memory-round-trip.txt](memory-round-trip.txt). Eight steps on a
store created for this run:

1. **Retention requires host confirmation.** `chutni_put_memory` without
   `confirmed` returns `confirmation_required` and writes nothing. Consent is
   not implied by calling the tool.
2. **A confirmed write returns the whole provenance chain** — `memory_id`,
   `source_id`, `artifact_id`, `producer_id`, `derivation_id` — not just an
   opaque handle. `memory_id` *is* the source ID, which is why `search`,
   `inspect`, `verify`, and `forget` need no memory-specific variants.
3. **An anonymous model producer is refused:** `model producers require
   model_id, app_name, and app_version (§16.2)`. Standalone memory gets no
   identity discount relative to file-derived artifacts.
4. **It is findable by content.** The memory has no filename; search matched
   on body text and returned `source_kind: "memory"`, `artifact_kind:
   "memory"`, `freshness: "current"`, and `display_path` from the title.
5. **Provenance survives the round trip.** `inspect` reports
   `produced by claude-opus-5 / anthropic/claude-opus-5 via record_decision`,
   marked `model_generated`.
6. **Forgetting is explicit and honestly described.** `forget --mode purge`
   unlinks payloads and says so without overclaiming: *"This is not forensic
   erasure: copies may remain in backups, snapshots, or unallocated blocks."*
   A subsequent search returns `No matches.`

## Freshness when there are no bytes on disk

This is the part worth reviewing. A standalone memory is born inside Chutni, so
there is no external file to re-observe, and the rule the format exists to
enforce — never trust cached state over the bytes on disk — has no disk to
appeal to.

The implementation does not add a second freshness path. `observe_source`
remains the single place a source's current observation is re-derived; for
`source_kind = "memory"` it re-hashes the source's own active `memory`
artifact ([src/chutni.c](../../../src/chutni.c), `observe_source`). The content
hash still binds source to text, and the answer still comes from stored
content rather than from two catalog columns agreeing with each other — which
is precisely the defect described in [CLAUDE.md](../../../CLAUDE.md).

Consequence, verified in step 7 of the transcript: a memory reads `current`
until it is forgotten or a required artifact input goes stale. It never goes
stale on its own, because nothing outside the store can change it.

## Not run

- Any platform other than the one named above. No Linux, no Windows, no x86-64.
- No concurrency test specific to memory writes; the existing single-writer
  coordination test covers the store, not this operation in particular.
- No test of memory artifacts as derivation inputs to *other* memories beyond
  what conformance already exercises for artifact-input cascade staleness.
