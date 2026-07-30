# Historical Chutni implementation status and adoption backlog

> Archived after the product was narrowed to durable file-derived and
> standalone application memory. This document preserves the earlier protocol
> roadmap; it is not the current completion checklist. See `docs/TASKS.md`.

**Last updated:** 2026-07-30
**Spec:** [SPEC.md](../SPEC.md) v0.2-draft
**Evidence:** [v0.1 baseline](evidence/2026-07-29-v0.1/),
[generic artifact interchange](evidence/2026-07-29-generic-artifacts/report.md),
and [v0.2 hierarchical coverage](evidence/2026-07-30-v0.2-hierarchical-coverage/report.md)

Read [../CLAUDE.md](../CLAUDE.md) first. This document says what exists, what
does not, and what to do next. Keep it accurate — it is the thing that stops
the next session from claiming a feature that was never built.

---

## 1. What is built

All verified on macOS 15 (Darwin arm64, Apple clang 21.0.0) — **the only
machine this has ever run on**. `make test`: 105 BLAKE3 checks, 79 conformance
assertions, 32 CLI checks, and 90 reusable-service checks, with 0 failures and
1 declared gap. `make sanitize` re-runs the conformance, CLI, and service
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
| Generic atomic artifact batches from arbitrary hosts | §16, §20 | built, tested |
| Grouped source context with all producer interpretations | §20, §23 | built, tested |
| Supersession, multi-producer artifacts | §23 | built, tested |
| Base `file_metadata` for every scanned file | §15.2 | built, tested |
| Single-writer/many-reader coordination | §21, §28 | built, tested across processes |
| Forget modes | §24.3 | built, lightly tested |
| Lexical search over FTS5 | §19 | built, tested |
| Producer-supplied f32 representations and profile gating | §17, §22.6 | built, tested |
| Brute-force cosine semantic search | §19.1 | built in C API, tested |
| Store discovery and registry | §39 | built, tested |
| Application Host lifecycle and cross-host handoff | §30.5, §40 | specified; reference handoff tested |
| Reusable local service | §20, §40 | MCP stdio + native one-shot tool surface built, tested |
| Reader + Producer + Search Provider conformance | §30.1–30.3 | believed met, not independently audited |
| **Directory sources and `parent_source_id` hierarchy** | §12.5 | built, tested |
| **Normative depth bounds, memory goal, definition mode** | §11.1, §11.2 | built, tested |
| **Directory observation and listing hashes** | §13.5 | built, tested |
| **Validity across derivation inputs, with transitive cascade** | §13.3 | built, tested |
| **`directory_listing`, `source_definition`, `coverage_manifest`** | §15.5–§15.7 | built, tested |
| **Required local coverage on directory definitions** | §15.6 | built, enforced at write |
| **Bounded reconciliation of absent sources** | §24.4 | built, tested |
| **Hierarchy relations with per-relation provenance** | §18 | built, tested |
| **`observe_directory`, `list_children`, `get_coverage`, `scan(root)`** | §20 | built, tested via C, CLI, and MCP |
| **Coverage fields on search results** | §19.3 | built, tested |
| **v0.1 compatibility and capability gating** | §35.1 | built, tested |

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
- A directory's `content_hash` is the hash of its **observed listing** (names
  and kinds only, sorted, escaped — §13.5), not of anything inside it. File
  contents are deliberately excluded: including them would make every edit
  anywhere stale every enclosing directory up to the root. Content reaches a
  directory's definition through `derivation.input_refs_json` instead, which is
  the only mechanism that can say *which* input went stale.
- Media types are excluded from the listing hash for the same class of reason:
  they come from a table this implementation may extend, and a hash that moved
  when the table grew would stale every directory in every store on upgrade.
- `observation` is `enumerated` or `opaque`, and an opaque re-observation never
  erases a listing recorded earlier. Not looking inside is not evidence that
  the inside changed, so `chutni_directory_put` uses
  `COALESCE(?, content_hash)` and leaves `observation` alone when a listing
  already exists.
- Enumeration, canonicalization, and hashing live in one function
  (`chutni_read_directory`). The scanner records a listing and freshness
  re-derives it later; if those two ever disagreed about which entries a policy
  admits or how they serialize, every directory in the store would read as
  permanently stale.

## 2. What is not built

Do not describe any of these as working. The conformance suite reports the
relevant §31 scenarios as `GAP`.

| Missing | Spec | Notes |
|---|---|---|
| **Hybrid and other search modes** | §19.1 | Hybrid fusion, metadata-only, image-similarity, and relationship search are unimplemented. |
| **Root remapping** across machines | §26 | A store copied to another computer cannot have its roots re-pointed. |
| **`.chutnipack` transfer bundles** | §7.2 | `chutni pack` does not exist. |
| **Most relation predicates** | §18 | `contains` and `observed_in` are written by the scanner and readable through `chutni_relations_list`. `summarizes`, `defined_by`, `duplicate_of`, `near_duplicate_of`, `references`, `version_of`, and `same_document_as` have API support but nothing produces them. |
| **`exclude_globs` enforcement** | §11 | Recorded in `policy_json` and still never consulted. Exclusion is a fixed name list (`.git`, `node_modules`, `.cache`, …) in `excluded_entry_name`. Note that a glob cannot be evaluated against a bare entry name, so `**/node_modules/**` needs a path-aware design rather than an `fnmatch` call — and because listings are policy-relative (§13.5), turning globs on will change listing hashes and stale every directory. |
| **Definitions from an actual model** | §15.6 | The write path is enforced and tested with a synthetic producer. No language model has classified a real directory; that is Samosa's work (Phase S). |
| **Convenience extractors beyond UTF-8 text** | §25 | The reference scanner does not parse PDFs, run OCR, caption images, inspect spreadsheets, or transcribe audio. This is intentionally host work, not a protocol gap; hosts can submit all of those artifacts and selectors through the generic service. |
| **Disclosure enforcement** | §27, §30.4 | The manifest records `external_disclosure_default: deny`; the local service never sends network traffic, but a cloud-facing host still has to enforce disclosure policy before forwarding excerpts. |
| **Archive safety** | §28.3 | No archive extraction exists yet, so no zip-bomb or traversal defenses. Needed before §25 archive support. |
| **Windows support** | §26 | POSIX-only: `/dev/urandom`, `lstat`, `realpath`, forward-slash paths. Never compiled on Windows. |
| **Concurrency stress testing** | — | Cross-process writer exclusion and concurrent readers are tested. Long-running many-reader/write and crash-interruption stress tests remain. |
| **Fuzzing** | — | `make sanitize` is clean, but only over the suite's own fixtures. No malformed manifest, truncated catalog, or hostile object has been fuzzed at the parsers. |

## 3. Work list

Ordered by what unblocks the most. Each phase needs a conformance test that
fails before and passes after, plus a transcript under `evidence/`.

**The adoption track lives in [tickets/](tickets/)** — fifteen detailed
tickets (T01–T15) covering bindings, first-run value, agent integration,
portability, and the measurement of the token-savings claim. The tickets are
the detailed form; the index there maps each one to the phase items it absorbs
(A4, P1–P4, W1–W3, and two open questions below). When a ticket lands, mark
the phase item done here and link the evidence, as always.

### Phase H — hierarchical sources and bounded coverage (§11.1–§35.1)

**Done 2026-07-30.** Evidence:
[v0.2 hierarchical coverage](evidence/2026-07-30-v0.2-hierarchical-coverage/report.md).

- ~~**H1**~~ — directories are sources with stable IDs and `parent_source_id`.
- ~~**H2**~~ — `max_depth` with normative semantics, enforced in the library.
- ~~**H3**~~ — `memory_goal` and `definition_mode` recorded on the root and
  echoed in every coverage manifest.
- ~~**H4**~~ — `directory_listing`, `source_definition`, `coverage_manifest`.
- ~~**H5**~~ — local coverage with a stop reason required on every directory
  definition, refused at write time.
- ~~**H6**~~ — directory freshness by listing re-enumeration, plus artifact
  validity extended to required derivation inputs with a transitive cascade.
- ~~**H7**~~ — reconciliation confined to the region a scan actually covered.
- ~~**H8**~~ — `contains` / `observed_in` written with provenance;
  `summarizes` / `defined_by` specified and available.
- ~~**H9**~~ — `observe_directory`, `list_children`, `get_coverage`,
  `scan(root, policy)`, in the C ABI, the CLI, and the MCP service.
- ~~**H10**~~ — scanner rewritten; conformance scenarios 14–18.

Remaining in this area, none of it blocking:

- **H11** — `exclude_globs` enforcement. Needs a path-aware matcher, and a
  migration answer, because listings are policy-relative and enabling globs
  changes every listing hash.
- **H12** — incremental coverage. A rescan recounts definitions across the
  whole root through the public API, one `list_artifacts` call per source. That
  is correct and linear and fine at 464 directories; it has not been measured
  anywhere larger.
- **H13** — `observed_in` is written for directory sources only, and every
  generation adds a row per directory. Nothing prunes them. Decide whether old
  generations' edges should be retired once their manifest is superseded.

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

### Phase M — rich artifact interchange (§15, §16, §20, §25)

Chutni records outputs from host parsers and models; it does not own those
extractors or judge their semantic truth.

- ~~**M1**~~ — **done 2026-07-29.** `chutni_artifacts_put` and
  `chutni_put_artifacts` atomically record producer, derivation, exact source
  hash, selector, timestamp, and one or more artifacts.
- ~~**M2**~~ — **done 2026-07-29.** `chutni_source_context` returns all current
  interpretations together with complete processing provenance and an explicit
  `semantic_validation: "not_performed"` marker.
- ~~**M3**~~ — **done 2026-07-29.** Every reference-scanned file receives base
  `file_metadata`, whether or not the scanner can extract its contents.
- ~~**M4**~~ — **done 2026-07-29.** Conformance scenario 8 proves that one host
  can submit PDF page text, a second can append a model interpretation, and a
  third can consume both without either being declared correct by Chutni.

PDF parsing, OCR, image understanding, spreadsheet reading, archive parsing,
and speech recognition belong in host applications or optional producer
packages. They are not pending Chutni core work.

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
- **S2** — external host-adoption work: Samosa may submit its own parser/model
  outputs through `chutni_put_artifacts`. No Samosa code belongs in this
  repository.
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

Not blocking, but they will need answers before v0.3.

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
- ~~**Uncoordinated simultaneous writers.**~~ **Answered 2026-07-29.** A
  read-write handle now owns an advisory store lock; concurrent readers remain
  allowed and a second process receives `CHUTNI_ERR_BUSY`. Distributed merge
  or synchronized multi-writer conflict resolution remains a v0.2 question,
  not a local-store requirement.
- ~~**`skills/` needs a real test.**~~ **Answered 2026-07-30.** Codex followed
  the skill against a bounded store with current, unverified, opaque, and
  prompt-injection fixtures; see [T05 evidence](evidence/2026-07-30-codex-agent-integration/report.md).
- **Directory freshness costs an enumeration.** `chutni verify` now re-reads
  every directory's entries as well as re-hashing every file. That is cheap
  relative to the hashing, but on a store with many directories and few files
  the ratio inverts. A quick check (mtime on the directory inode) could
  withdraw a claim cheaply; per §13.2 it must never establish one.
- **A shallow scan cannot retire a deep region.** §24.4 is deliberately
  conservative: sources under directories the scan never opened keep their last
  known state indefinitely. If a user deletes a whole subtree and only ever
  runs depth-0 refreshes, those sources stay `present` forever. Correct, and
  possibly surprising. A `chutni verify` pass does re-observe them; whether the
  scanner should offer an explicit "reconcile everything I know about" mode is
  open.
