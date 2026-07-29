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
machine this has ever run on**. `make test`: 105 BLAKE3 checks, 28 conformance
assertions, 15 CLI checks, 0 failures. `make sanitize` re-runs both suites under
ASan + UBSan, also clean.

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
| Store discovery and registry | §39 | built, tested |
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
| **Representations** — embeddings, token IDs, vision tokens | §17 | No API at all. Blocks semantic search. The largest single gap. |
| **Semantic and hybrid search** | §19.1 | Only `lexical` works. `metadata`, `semantic`, `hybrid`, `image_similarity`, `relationship` are unimplemented. |
| **Root remapping** across machines | §26 | A store copied to another computer cannot have its roots re-pointed. |
| **`.chutnipack` transfer bundles** | §7.2 | `chutni pack` does not exist. |
| **Relations** | §18 | Table exists; nothing writes or reads it. No `duplicate_of`, `contains`, etc. |
| **Non-text ingestion** — images, audio, spreadsheets, archives | §25.2–25.4 | Non-text files get a `file_metadata` artifact only. No OCR, captions, transcripts, or sheet inventories. |
| **Selectors in practice** | §15.3 | Column is written and read, but nothing produces page/region/time selectors because nothing parses those formats. |
| **Gateway and disclosure enforcement** | §27, §30.4 | The manifest records `external_disclosure_default: deny`; no code enforces it because there is no gateway. |
| **Archive safety** | §28.3 | No archive extraction exists yet, so no zip-bomb or traversal defenses. Needed before §25 archive support. |
| **Windows support** | §26 | POSIX-only: `/dev/urandom`, `lstat`, `realpath`, forward-slash paths. Never compiled on Windows. |
| **Concurrency testing** | — | WAL is on and busy-timeout is set, but multi-process access has never been tested. |
| **Fuzzing** | — | `make sanitize` is clean, but only over the suite's own fixtures. No malformed manifest, truncated catalog, or hostile object has been fuzzed at the parsers. |

## 3. Work list

Ordered by what unblocks the most. Each phase needs a conformance test that
fails before and passes after, plus a transcript under `evidence/`.

### Phase R — representations and semantic search (§17, §19)

The biggest gap and the most-requested capability. Lexical search cannot find
"crowding agent" in a file that only says "PEG".

- **R1** — `chutni_representation_put/get`, with the compatibility fields §17.1
  requires: model id, revision, dimensions, dtype, normalization, source
  artifact hash. Refuse to return a representation whose profile the caller has
  not declared it accepts (§22.6).
- **R2** — serialize vectors as objects; decide and document the on-disk format.
- **R3** — a brute-force cosine search over stored vectors. Correctness first;
  no ANN index yet. Report `score_type` honestly.
- **R4** — hybrid ranking that fuses lexical and semantic. §19 deliberately
  does not standardize ranking, so document the formula and keep `score_type`
  distinct from bm25's.
- **R5** — conformance scenario 12 (representation compatibility and
  incompatibility), which currently reports GAP.

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

### Phase G — gateway and disclosure (§27, §30.4)

Needed before any cloud model touches a store.

- **G1** — a local read-only service exposing the §20 minimum operations.
- **G2** — enforce the permission levels in §27.4: store, root, source,
  artifact-type, one-time, session, persistent.
- **G3** — client authentication (§28.6) and separate authorization for
  write-capable clients (§28.7).

### Phase S — Samosa as reader and writer

The owner's stated goal: Samosa should both read and write this format. Samosa
has substantial extraction, OCR, and scanning code that a conforming producer
could reuse; its **storage layer** is what differs.

- **S1** — link `libchutni` into Samosa and implement discovery, so Samosa can
  find an existing store instead of always building its own.
- **S2** — a Samosa producer that writes conformant artifacts, with its models
  recorded as §16.2 producers (model id, revision, quantization, runtime).
- **S3** — decide the fate of Samosa's schema-v2 sidecar: migrate it, run both,
  or retire it. This is an owner decision, not an agent one.
- **S4** — real-store gate: Samosa reads a store built by `chutni`, and
  `chutni` reads a store built by Samosa. Until that runs, neither is
  described as compatible.

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
- **Search reports catalog freshness, not disk truth.** Between a file changing
  and the next `verify`, a search result can still say `current`. This is
  inherent to any index, and §6.1 answers it by telling consumers to reopen the
  source for exact claims. Worth revisiting whether search should cheaply stat
  files and downgrade obviously-drifted results.
- **No conflict resolution for simultaneous writers** (§37.7). Two applications
  writing one store concurrently is untested.
- **`skills/` needs a real test.** The instructions have never been run through
  an actual agent against a real store. Until they have, they are a hypothesis.
