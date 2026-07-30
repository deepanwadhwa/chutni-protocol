# Chutni — current completion state

**Goal:** Chutni is a separate, local memory system where knowledge and useful
LLM/application work persist so an application such as Samosa can reuse them.

Chutni is not an agent framework, a universal conversation engine, or a second
application. It stores, searches, attributes, verifies, and forgets memory.

## Ready now

- File and directory memory with bounded scanning, freshness, provenance,
  lexical search, grouped source context, and explicit forgetting.
- Host-produced PDF/OCR/image/spreadsheet/audio artifacts through the generic
  artifact API; extraction remains the host application's job.
- Standalone notes, decisions, plans, conversation summaries, analyses, and
  work products through `put_memory` / `chutni_put_memory`.
- Typed C, one JSON call surface, a stdlib-only Python binding, CLI, MCP stdio,
  and native one-shot service access.
- The application surface Samosa currently consumes, protected by
  `make test-samosa-compat`.
- Single-writer/many-reader coordination and producer/model/application
  identity on retained outputs.

## Verification

The completion gate is:

```sh
make test
make sanitize
make test-samosa-compat
```

The exact Samosa boundary and upgrade procedure are in
[SAMOSA-COMPATIBILITY.md](SAMOSA-COMPATIBILITY.md). The JSON application
contract is in [API-JSON.md](API-JSON.md).

## Known limits, not blockers for Samosa adoption

- Root remapping and `.chutnipack` transfer bundles are not implemented.
- Windows is not supported.
- Chutni does not ship PDF/OCR/vision/spreadsheet/audio extractors.
- Chutni accepts producer claims with provenance; it does not certify that a
  model summary or OCR result is true.
- Semantic search accepts producer-supplied embeddings and uses brute-force
  cosine; hybrid ranking is not implemented.
- Rich conversation synchronization, personality, and agent identity are
  outside the standalone-memory record implemented here.

These limits must remain honest in the README and specification, but they are
not a mandate to expand Chutni before Samosa adopts the current build.

## Next action

Chutni's next product task is outside this repository: update Samosa's pinned
Chutni build in a Samosa branch, run Samosa's gateway tests, then teach Samosa
to use `chutni_put_memory` for durable conversation/work memory.

The former fifteen-ticket adoption roadmap is preserved under
[archive/TASKS-2026-07-30-adoption-backlog.md](archive/TASKS-2026-07-30-adoption-backlog.md)
and [tickets/](tickets/). It is historical reference, not the active queue.
