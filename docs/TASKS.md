# Chutni — implementation status and work list

**Last updated:** 2026-07-29
**Spec:** [SPEC.md](../SPEC.md) v0.1-draft
**Evidence:** [evidence/2026-07-29-v0.1/](evidence/2026-07-29-v0.1/)

Read [../CLAUDE.md](../CLAUDE.md) first. This document says what exists, what
does not, and what to do next. Keep it accurate — it is the thing that stops
the next session from claiming a feature that was never built.

---

## 1. What is built

All verified on macOS 15 (Darwin arm64, Apple clang 21.0.0) — **the only
machine this has ever run on**. `make test`: 105 BLAKE3 checks, 41 conformance
assertions, 16 CLI checks, and 25 reusable-service checks, with 0 failures and
2 declared gaps. `make sanitize` re-runs the conformance, CLI, and service
suites under ASan + UBSan, also clean.

| Area | Spec | State |
|---|---|---|
| Store layout, create, open | §8 | built, tested |
| Manifest, unknown-field preservation | §9 | built, tested |
| Catalog schema — all 8 tables | §10 | built, tested |
| Roots and indexing authorization | §11 | built, tested |
| Sources, locators, file identity | §12 | built, tested |
| BLAKE3 hashing, freshness, staleness | §13 | built, tested |
| Content-addressed objects, corruption detection | §14 | built, tested |
| Artifacts, origins, statuses | §15 | built, tested |
| Producers, derivations, per-artifact provenance | §16 | built, tested |
| Supersession, multi-producer artifacts | §23 | built, tested |
| Forget modes | §24.3 | built, lightly tested |
| Lexical search over FTS5 | §19 | built, tested |
| Producer-supplied f32 representations and profile gating | §17, §22.6 | built, tested |
| Brute-force cosine semantic search | §19.1 | built in C API, tested |
| Store discovery and registry | §39 | built, tested |
| Application Host lifecycle and cross-host handoff | §30.5, §40 | specified; reference handoff tested |
| Reusable local service | §20, §40 | MCP stdio + native one-shot tool surface built, tested |
| Reader + Producer + Search Provider conformance | §30.1–30.3 | believed met, not independently audited |

Non-obvious properties worth preserving:

- The lexical index lives in `indexes/lexical.sqlite`, attached as `idx`, not
  inside `catalog.sqlite`. That makes "delete `indexes/` and rebuild" literally
  true, which is what §8.4 claims.
- FTS5 auxiliary functions (`bm25`, `snippet`) and `MATCH` resolve against a
  hidden column carrying the table's own name, so the FTS table must **not** be
  aliased in queries. Aliasing it breaks both, confusingly.
- User queries are escaped into quoted FTS5 terms, so a stray quote or the word
  `AND` in a question cannot become a syntax error or silently change meaning.
- `model_generated` artifacts are refused without a `derivation_id` (§16.4).
  An untraceable model artifact defeats the point of the format.

## 2. What is not built

Do not describe any of these as working. The conformance suite reports the
relevant §31 scenarios as `GAP`.

| Missing | Spec | Notes |
|---|---|---|
| **Hybrid and other search modes** | §19.1 | Hybrid fusion, metadata-only, image-similarity, and relationship search are unimplemented. |
| **Root remapping** across machines | §26 | A store copied to another computer cannot have its roots re-pointed. |
| **`.chutnipack` transfer bundles** | §7.2 | `chutni pack` does not exist. |
| **Relations** | §18 | Table exists; nothing writes or reads it. No `duplicate_of`, `contains`, etc. |
| **Non-text ingestion** — images, audio, spreadsheets, archives | §25.2–25.4 | Non-text files get a `file_metadata` artifact only. No OCR, captions, transcripts, or sheet inventories. |
| **Selectors in practice** | §15.3 | Column is written and read, but nothing produces page/region/time selectors because nothing parses those formats. |
| **Disclosure enforcement** | §27, §30.4 | The manifest records `external_disclosure_default: deny`; the local service never sends network traffic, but a cloud-facing host still has to enforce disclosure policy before forwarding excerpts. |
| **Archive safety** | §28.3 | No archive extraction exists yet, so no zip-bomb or traversal defenses. Needed before §25 archive support. |
| **Windows support** | §26 | POSIX-only: `/dev/urandom`, `lstat`, `realpath`, forward-slash paths. Never compiled on Windows. |
| **Concurrency testing** | — | WAL is on and busy-timeout is set, but multi-process access has never been tested. |
| **Fuzzing** | — | `make sanitize` is clean, but only over the suite's own fixtures. No malformed manifest, truncated catalog, or hostile object has been fuzzed at the parsers. |

## 3. Work list

Ordered by what unblocks the most. Each phase needs a conformance test that
fails before and passes after, plus a transcript under `evidence/`.

### Phase R — representations and semantic search (§17, §19)

R1–R3 and scenario 12 are complete. The remaining R-phase item is hybrid
ranking; lexical search still cannot find "crowding agent" in a file that only
says "PEG".

- ~~**R1**~~ — **done 2026-07-29.** `chutni_representation_put/get` records
  model id, revision, dimensions, dtype, normalization, tokenizer/projector
  compatibility fields, and source artifact hash; incompatible profiles are
  refused (§22.6).
- ~~**R2**~~ — **done 2026-07-29.** Vectors are content-addressed objects using
  the documented `CHUTVEC1` little-endian f32 format (SPEC §17.6).
- ~~**R3**~~ — **done 2026-07-29.** The C API performs brute-force cosine search
  over matching representations and reports `cosine_bruteforce`; there is no
  ANN index yet.
- **R4** — hybrid ranking that fuses lexical and semantic. §19 deliberately
  does not standardize ranking, so document the formula and keep `score_type`
  distinct from bm25's.
- ~~**R5**~~ — **done 2026-07-29.** Conformance scenario 12 now covers
  round-trip serialization, exact profile gating, compatibility reporting, and
  cosine ranking; it no longer reports GAP.

Chutni does not compute embeddings; a producer supplies them. Keep it that way.

### Phase P — packing and portability (§7.2, §26)

- **P1** — `chutni pack` / `chutni unpack`, ZIP64 with deterministic paths.
- **P2** — root remapping: `chutni remap <old> <new>`, updating locators while
  leaving content hashes and provenance untouched (§26).
- **P3** — conformance scenario 3 (moved root), currently GAP.
- **P4** — decide what a store carried between machines does about
  `file_identity_json`, which §12.3 says is not portable.

### Phase M — multimodal ingestion (§25)

Each of these makes a real difference to whether the store is useful, and each
needs a parser the project does not currently have.

- **M1** — images: metadata, perceptual hash, thumbnail. No model needed.
- **M2** — spreadsheets: sheet inventory and schema, *without* flattening to
  text (§25.4 is explicit about this).
- **M3** — PDFs: page text and `page_text` artifacts with page selectors.
- **M4** — archives: listing only, with the §28.3 defenses in place first.
- **M5** — conformance scenario 8, currently GAP.

### Phase G — service and disclosure (§27, §30.4)

Needed before any cloud model touches a store.

- ~~**G1**~~ — **done 2026-07-29.** `chutni-mcp` exposes the shared host
  lifecycle through MCP stdio and a native one-shot JSON mode. Read tools are
  non-mutating; scan/create/retention tools require explicit confirmation.
- **G2** — enforce the permission levels in §27.4: store, root, source,
  artifact-type, one-time, session, persistent.
- **G3** — client authentication (§28.6) and separate authorization for
  write-capable network clients (§28.7). Stdio inherits the launching host's
  local process boundary.

### Phase A — application adoption (§30.5, §40)

This is the generic contract every host implements. Samosa is the first
guinea-pig client; nothing in this phase is Samosa-specific.

- ~~**A1**~~ — **done 2026-07-29.** Specify the selected-folder state machine:
  classify store vs source selection, check the adjacent `P.chutni` store,
  create only with permission, and never overwrite collisions.
- ~~**A2**~~ — **done 2026-07-29.** Specify the host/model responsibility
  boundary and the complete create, open, reuse, update, retrieval, and failure
  lifecycle.
- ~~**A3**~~ — **done 2026-07-29.** Publish the implementation guide and add
  conformance scenario 13 for cross-host creation, update, and round-trip reuse.
- **A4** — add language bindings or a small transport-neutral host SDK after
  the C guinea-pig integration identifies which convenience operations are
  genuinely shared.

### Phase S — Samosa as reader and writer

Samosa is the first §40 Application Host and conformance guinea pig. It has
substantial extraction, OCR, scanning, and local-model code that a conforming
producer can reuse; its **storage layer** is what differs.

- ~~**S1**~~ — **done 2026-07-29.** Samosa pins and bundles `chutni-mcp`;
  selecting `P` checks, creates/opens, scans, and searches the adjacent
  `P.chutni` through the shared service.
- **S2** — a Samosa producer that writes conformant artifacts, with its models
  recorded as §16.2 producers (model id, revision, quantization, runtime).
- ~~**S3**~~ — **done 2026-07-29.** The Samosa gateway retired its schema-v2
  sidecar from the live path. It keeps presentation/job metadata only; the
  legacy source remains for standalone migration tests and is not packaged.
- ~~**S4**~~ — **done 2026-07-29.** Samosa's gateway integration test creates
  and queries a real adjacent store, the standalone service reads and updates
  it, and Samosa retrieves that update and supplies bounded evidence to a
  local-model chat turn.

### Phase W — Windows (§26)

- **W1** — replace `/dev/urandom`, `realpath`, `lstat`, and path separators
  behind a platform shim.
- **W2** — decide the registry location on Windows (§39.2 permits roaming
  app-data) and document it.
- **W3** — lossless native path storage via `native_path_b64` (§12.2).

## 4. Open questions

Not blocking, but they will need answers before v0.2.

- **Freshness costs a re-hash.** `chutni verify` re-reads every file. On a large
  root that is expensive. §13.2 permits a quick hash for change detection but
  forbids substituting it for `content_hash` when establishing validity — the
  cheap path is worth building, carefully.
- ~~**Search reports catalog freshness, not disk truth.**~~ **Answered
  2026-07-29.** Search now stats each hit and reports `unverified` when size or
  mtime no longer match what the scan recorded, so the window between a file
  changing and the next `verify` no longer produces a confident `current`. A
  stat may only withdraw a claim, never establish one (§13.2), so drifted
  results are not called `stale` either — proving that needs a re-hash, which is
  what `chutni verify` is for. CLI check: "edited file is not called current
  before verify".
- **No conflict resolution for simultaneous writers** (§37.7). Two applications
  writing one store concurrently is untested.
- **`skills/` needs a real test.** The instructions have never been run through
  an actual agent against a real store. Until they have, they are a hypothesis.
