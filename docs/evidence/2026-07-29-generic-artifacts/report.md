# Generic artifact interchange evidence

**Date:** 2026-07-29
**Platform:** macOS 15, Darwin arm64, Apple clang

## Scope

This pass tested the application-neutral producer/consumer boundary:

- every reference-scanned file receives `file_metadata`;
- hosts submit deterministic or model-generated artifacts through one atomic
  producer/derivation/artifact batch;
- submitted artifacts bind to the exact current source hash;
- Chutni records but does not semantically verify producer claims;
- artifacts from different producers coexist;
- a later consumer retrieves all interpretations with timestamps, selectors,
  processing operation, application/model identity, and derivation inputs;
- concurrent readers are allowed while a second local writer is refused with
  `CHUTNI_ERR_BUSY`.

The rich-artifact fixture intentionally stores the false model claim “This PDF
is about a dog” against a source about orbital mechanics. The test passes only
when that claim survives verbatim with model provenance and without a Chutni
correctness claim.

## Commands and results

`make test`:

- 105 BLAKE3 vector checks passed;
- 52 conformance assertions passed;
- 16 CLI checks passed;
- 44 reusable-service/MCP checks passed;
- 0 failures;
- 1 declared gap: moved-root remapping.

`make sanitize` reran the conformance, CLI, and service suites under Address
Sanitizer and UndefinedBehaviorSanitizer with the same results and no sanitizer
findings.

## Responsibility boundary

PDF parsing, OCR, image understanding, spreadsheet reading, and speech
recognition are performed by host applications or optional producer packages.
Chutni validates record shape, references, payload integrity, source-version
binding, and provenance. It does not judge the semantic truth of an artifact.
