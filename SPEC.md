# Chutni Protocol Specification

**Version:** 0.1-draft
**Status:** Proposed initial specification
**Scope:** Portable, source-backed memory artifacts for local files and AI applications

## 1. Abstract

Chutni is an open protocol for creating, storing, exchanging, updating, and querying AI-readable memory derived from a user’s local files.

A Chutni store is a persistent semantic map of files. It records where files exist, what they contain, how derived descriptions were produced, and whether those descriptions are still valid for the current file contents. The store may include extracted text, OCR, captions, summaries, chunks, metadata, embeddings, and disposable search indexes.

Chutni is not tied to Samosa, a particular operating system, a particular language model, or local inference. Any compatible application may create a Chutni store, read one created by another application, improve selected artifacts with a different model, or expose the store to a local or cloud model through a permission-controlled gateway.

The core design principle is:

> Prepare a user’s files for AI once, preserve the result as a user-owned artifact, and allow compatible applications to reuse or improve it without rebuilding everything.

## 2. Normative language

The terms **MUST**, **MUST NOT**, **REQUIRED**, **SHOULD**, **SHOULD NOT**, and **MAY** are normative.

## 3. Goals

Chutni v0.1 has the following goals:

1. **Portability** — a store created by one application can be consumed by another.
2. **Source traceability** — every derived artifact can be traced to the original file and, where possible, to a page, sheet, region, byte range, or time range.
3. **Producer transparency** — model-generated artifacts identify the model, revision, application, and processing recipe that produced them.
4. **Incremental updates** — changed files can be reprocessed without rebuilding the entire store.
5. **Multimodal support** — the format can represent text, documents, images, audio, video, spreadsheets, archives, and future file types.
6. **Model independence** — canonical artifacts remain useful across models; model-specific caches are explicitly labeled and disposable.
7. **Cross-platform use** — the same logical store works on macOS, Windows, and Linux.
8. **Local-first privacy** — the store remains local by default, and external models receive only explicitly disclosed context.
9. **Efficient retrieval** — consumers can locate likely files before opening or parsing the original source.
10. **Graceful improvement** — stronger models can replace or supplement artifacts created by weaker models without rescanning unrelated files.

## 4. Non-goals

Chutni v0.1 does not attempt to:

1. Standardize one universal ranking algorithm.
2. Guarantee that a summary, caption, OCR result, or extraction is correct.
3. Define a universal model-quality score.
4. Replace the original file as the authoritative source.
5. Standardize operating-system file-watching APIs.
6. Require one vector database, embedding model, parser, or runtime.
7. Store a complete copy of every source file.
8. Define conversational memory, personality memory, or agent identity.
9. Execute instructions found inside indexed files.
10. Require cloud connectivity.

## 5. System roles

A Chutni ecosystem contains the following roles.

### 5.1 Source

A source is an original item from which memory is derived. In v0.1, the primary source type is a local file. Sources may also represent directories, archive members, removable-drive files, or other locally addressable objects.

### 5.2 Producer

A producer discovers sources and creates artifacts. A producer may be:

* Samosa;
* another local AI application;
* a document parser;
* an OCR engine;
* a vision-language model;
* a speech-to-text model;
* a cloud model acting with user permission;
* a human editor.

### 5.3 Chutni store

A Chutni store contains source records, derived artifacts, provenance records, optional reusable representations, and optional search indexes.

### 5.4 Consumer

A consumer searches or reads a Chutni store. It may use the store to identify relevant files, retrieve excerpts, verify freshness, and decide whether to open the original source.

### 5.5 Verifier

A verifier checks whether an artifact still corresponds to the current source contents. A consumer may also act as the verifier.

### 5.6 Gateway

A gateway exposes selected Chutni operations to another process or model. A gateway may use a C API, Rust library, command-line interface, local IPC, HTTP, MCP, or another transport.

The transport is not the memory format. MCP, for example, may expose Chutni, but MCP is not a replacement for the Chutni store.

## 6. Core design rules

### 6.1 Memory is a map, not the source of truth

A Chutni artifact helps locate and understand a source. It MUST NOT silently replace the source as authoritative evidence.

For high-stakes or exact factual answers, consumers SHOULD verify the relevant source content before answering. A consumer MAY answer directly from an artifact when the user accepts that behavior, but it SHOULD disclose when the original source was not reopened.

### 6.2 No standardized self-confidence

Chutni v0.1 does not define a model-generated confidence field.

A producer MAY include private or extension-specific scoring, but consumers MUST NOT interpret such scoring as a standardized measure of correctness.

Instead, Chutni records objective provenance: who produced the artifact, which model or parser was used, which source version was processed, and which recipe created the output.

### 6.3 Canonical artifacts and disposable acceleration

Chutni separates durable, portable artifacts from model-specific acceleration data.

Portable artifacts include:

* file metadata;
* content hashes;
* extracted text;
* OCR text;
* captions;
* summaries;
* page, sheet, region, or time selectors;
* provenance;
* relationships between sources and artifacts.

Model-specific acceleration includes:

* token IDs;
* text embeddings;
* image embeddings;
* vision-projector outputs;
* vector indexes;
* lexical indexes;
* cached model states.

Acceleration data MUST identify its compatibility requirements and MUST be safe to discard and regenerate.

### 6.4 Per-artifact provenance

Provenance MUST be recorded per artifact, not only once per store. A single Chutni store may contain OCR from one engine, summaries from another model, captions from a third model, and embeddings from several models.

### 6.5 Local files are untrusted data

Indexed files may contain malicious or misleading instructions. Chutni consumers MUST treat file contents and derived artifacts as untrusted data, not as commands.

An agent MUST NOT execute instructions found in a file merely because those instructions were retrieved from Chutni.

## 7. Store forms

Chutni defines two physical forms.

### 7.1 Live store

A live store is a directory whose name SHOULD end in `.chutni`.

Example:

```text
Research.chutni/
```

A live store supports incremental updates and concurrent read access.

### 7.2 Transfer bundle

A transfer bundle is an immutable or read-only archive whose name SHOULD end in `.chutnipack`.

A `.chutnipack` file MUST contain the same required root files as a live store. ZIP64 with deterministic paths is the recommended v0.1 packaging format. Implementations MAY support other archive containers through extensions.

A consumer SHOULD unpack a transfer bundle into a live store before performing updates.

## 8. Required directory layout

A conforming live store MUST contain:

```text
Example.chutni/
├── manifest.json
├── catalog.sqlite
├── objects/
│   └── blake3/
├── indexes/
├── extensions/
└── tmp/
```

### 8.1 `manifest.json`

Contains store-level identity, format version, capabilities, and policy declarations.

### 8.2 `catalog.sqlite`

Contains the canonical catalog of sources, artifacts, producers, derivations, representations, and relationships.

### 8.3 `objects/`

Contains content-addressed blobs such as extracted text, OCR output, thumbnails, captions, transcripts, serialized embeddings, and other artifact payloads.

### 8.4 `indexes/`

Contains disposable search indexes. Everything under this directory MUST be rebuildable from the catalog and object store.

### 8.5 `extensions/`

Contains namespaced extension data.

### 8.6 `tmp/`

Contains incomplete or temporary work. Consumers MUST NOT treat files in `tmp/` as committed artifacts.

## 9. Manifest

A v0.1 manifest MUST be UTF-8 JSON and MUST contain:

```json
{
  "format": "chutni",
  "spec_version": "0.1",
  "store_id": "0195f0c4-83f8-7d4f-a2dc-c91d9201287a",
  "created_at": "2026-07-26T14:00:00Z",
  "updated_at": "2026-07-26T21:30:00Z",
  "hash_algorithm": "blake3",
  "catalog": "catalog.sqlite",
  "object_root": "objects/blake3",
  "capabilities": [
    "sources",
    "artifacts",
    "provenance",
    "full_text_optional",
    "vector_optional"
  ],
  "privacy": {
    "local_first": true,
    "external_disclosure_default": "deny"
  }
}
```

### 9.1 Required fields

* `format` MUST equal `chutni`.
* `spec_version` MUST identify the Chutni specification version.
* `store_id` MUST be a globally unique identifier. UUIDv7 is recommended.
* `created_at` and `updated_at` MUST use RFC 3339 UTC timestamps.
* `hash_algorithm` MUST be `blake3` in v0.1.
* `catalog` MUST identify the catalog path.
* `object_root` MUST identify the content-addressed object path.
* `privacy.external_disclosure_default` MUST be `deny` unless the user explicitly changes it.

Unknown manifest fields MUST be preserved by applications that rewrite the manifest, unless the user explicitly requests cleanup.

## 10. Catalog schema

The canonical v0.1 catalog is SQLite. Implementations MUST use UTF-8 text and MUST enable foreign-key enforcement when writing.

The schema below defines the required logical tables. Implementations MAY add indexes, triggers, or namespaced extension tables.

```sql
CREATE TABLE roots (
    root_id TEXT PRIMARY KEY,
    locator_json TEXT NOT NULL,
    label TEXT,
    added_at TEXT NOT NULL,
    policy_json TEXT NOT NULL
);

CREATE TABLE sources (
    source_id TEXT PRIMARY KEY,
    root_id TEXT,
    parent_source_id TEXT,
    source_kind TEXT NOT NULL,
    locator_json TEXT NOT NULL,
    display_name TEXT,
    media_type TEXT,
    size_bytes INTEGER,
    content_hash TEXT,
    quick_hash TEXT,
    mtime_ns INTEGER,
    birthtime_ns INTEGER,
    file_identity_json TEXT,
    state TEXT NOT NULL,
    first_seen_at TEXT NOT NULL,
    last_seen_at TEXT NOT NULL,
    last_scanned_at TEXT,
    metadata_json TEXT,
    FOREIGN KEY(root_id) REFERENCES roots(root_id),
    FOREIGN KEY(parent_source_id) REFERENCES sources(source_id)
);

CREATE TABLE objects (
    object_hash TEXT PRIMARY KEY,
    algorithm TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    media_type TEXT,
    compression TEXT NOT NULL,
    relative_path TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE producers (
    producer_id TEXT PRIMARY KEY,
    producer_kind TEXT NOT NULL,
    name TEXT NOT NULL,
    version TEXT,
    model_id TEXT,
    model_revision TEXT,
    weights_hash TEXT,
    quantization TEXT,
    runtime TEXT,
    app_name TEXT,
    app_version TEXT,
    details_json TEXT
);

CREATE TABLE derivations (
    derivation_id TEXT PRIMARY KEY,
    producer_id TEXT NOT NULL,
    operation TEXT NOT NULL,
    recipe_hash TEXT,
    parameters_json TEXT,
    input_refs_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    FOREIGN KEY(producer_id) REFERENCES producers(producer_id)
);

CREATE TABLE artifacts (
    artifact_id TEXT PRIMARY KEY,
    source_id TEXT NOT NULL,
    artifact_kind TEXT NOT NULL,
    artifact_origin TEXT NOT NULL,
    media_type TEXT,
    object_hash TEXT,
    inline_text TEXT,
    selector_json TEXT,
    language TEXT,
    source_content_hash TEXT,
    derivation_id TEXT,
    status TEXT NOT NULL,
    supersedes_artifact_id TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    metadata_json TEXT,
    CHECK (object_hash IS NOT NULL OR inline_text IS NOT NULL),
    FOREIGN KEY(source_id) REFERENCES sources(source_id),
    FOREIGN KEY(object_hash) REFERENCES objects(object_hash),
    FOREIGN KEY(derivation_id) REFERENCES derivations(derivation_id),
    FOREIGN KEY(supersedes_artifact_id) REFERENCES artifacts(artifact_id)
);

CREATE TABLE representations (
    representation_id TEXT PRIMARY KEY,
    artifact_id TEXT NOT NULL,
    representation_kind TEXT NOT NULL,
    object_hash TEXT NOT NULL,
    model_id TEXT,
    model_revision TEXT,
    dimensions INTEGER,
    dtype TEXT,
    normalization TEXT,
    tokenizer_hash TEXT,
    projector_hash TEXT,
    source_artifact_hash TEXT NOT NULL,
    created_at TEXT NOT NULL,
    metadata_json TEXT,
    FOREIGN KEY(artifact_id) REFERENCES artifacts(artifact_id),
    FOREIGN KEY(object_hash) REFERENCES objects(object_hash)
);

CREATE TABLE relations (
    relation_id TEXT PRIMARY KEY,
    from_id TEXT NOT NULL,
    predicate TEXT NOT NULL,
    to_id TEXT NOT NULL,
    derivation_id TEXT,
    created_at TEXT NOT NULL,
    metadata_json TEXT,
    FOREIGN KEY(derivation_id) REFERENCES derivations(derivation_id)
);
```

## 11. Root records

A root is a user-approved indexing boundary, such as:

* a folder;
* a mounted drive;
* a home directory;
* an explicitly selected entire computer scope.

A producer MUST NOT index outside configured roots unless the user grants additional access.

`policy_json` SHOULD support:

```json
{
  "recursive": true,
  "follow_symlinks": false,
  "include_hidden": false,
  "retain_deleted_artifacts": true,
  "exclude_globs": [
    "**/.git/**",
    "**/node_modules/**",
    "**/.cache/**"
  ],
  "max_file_size_bytes": 2147483648
}
```

Producers MAY define additional policy fields.

## 12. Source identity and location

### 12.1 Source ID

Each source MUST have a stable `source_id` within the store. UUIDv7 is recommended.

A source ID is not a content hash. Two identical files at different locations MAY have different source IDs while sharing the same `content_hash`.

### 12.2 Locator

`locator_json` MUST contain enough information for the originating platform to attempt to reopen the source.

Example:

```json
{
  "scheme": "file",
  "platform": "windows",
  "display_path": "D:\\Research\\paper.docx",
  "file_uri": "file:///D:/Research/paper.docx",
  "volume_id": "optional-platform-volume-id",
  "native_path_b64": null
}
```

Required locator fields:

* `scheme`;
* `platform`;
* `display_path`.

`file_uri`, `volume_id`, and `native_path_b64` are optional.

On POSIX systems, `native_path_b64` MAY preserve a non-UTF-8 path. On Windows, implementations MAY store a lossless native path representation when required.

Consumers on another computer MUST treat a locator as a hint, not a guarantee. A transferred store may require root remapping.

### 12.3 File identity

`file_identity_json` MAY contain platform-specific identifiers, including:

* device and inode numbers on POSIX;
* Windows volume serial and file ID;
* macOS file-system identifiers.

These identifiers help detect renames but MUST NOT be assumed portable across machines or copies.

### 12.4 Source state

`state` MUST be one of:

* `present`;
* `missing`;
* `deleted`;
* `unreadable`;
* `excluded`;
* `unsupported`.

A producer SHOULD distinguish `missing` from `deleted` when the operating system provides enough evidence.

## 13. Hashing and freshness

### 13.1 Content hash

A fully processed regular file SHOULD have a BLAKE3 `content_hash` over its exact bytes.

The canonical string form is:

```text
blake3:<lowercase-hex-digest>
```

### 13.2 Quick hash

A producer MAY store a quick hash or fingerprint for change detection. A quick hash MUST NOT substitute for `content_hash` when establishing artifact validity.

### 13.3 Artifact validity

Every artifact derived from file contents MUST store `source_content_hash`.

An artifact is current only when:

```text
artifact.source_content_hash == source.content_hash
```

If the source hash changes, prior artifacts MUST be marked `stale` or superseded. They MUST NOT remain silently `active`.

### 13.4 Watchers

Producers MAY use FSEvents, the Windows USN journal, `ReadDirectoryChangesW`, `inotify`, `fanotify`, polling, or another mechanism.

File watchers are an optimization. A producer SHOULD periodically verify hashes because watchers can miss events, mounted drives can disappear, and stores can be transferred between systems.

## 14. Objects

Large artifact payloads SHOULD be stored as immutable content-addressed objects.

Recommended path:

```text
objects/blake3/ab/cd/<full-digest>
```

The object hash MUST be calculated over the uncompressed logical payload. `objects.compression` identifies storage encoding:

* `none`;
* `zstd`;
* a namespaced extension value.

Two artifacts with identical payloads MAY reference the same object.

Objects MUST NOT be modified in place. A changed payload creates a new object.

## 15. Artifact model

An artifact is a derived or recorded representation associated with a source.

### 15.1 Artifact origin

`artifact_origin` MUST be one of:

* `direct` — observed source metadata or bytes;
* `deterministic_transform` — parser or deterministic transformation output;
* `model_generated` — generated by a machine-learning model;
* `human` — supplied or corrected by a person.

This field describes how the artifact was created, not whether it is correct.

### 15.2 Core artifact kinds

Chutni v0.1 reserves the following artifact kinds:

* `file_metadata`;
* `extracted_text`;
* `page_text`;
* `ocr_text`;
* `transcript`;
* `text_chunk`;
* `summary_short`;
* `summary_long`;
* `image_caption`;
* `document_title`;
* `keywords`;
* `entities`;
* `table_schema`;
* `sheet_summary`;
* `archive_listing`;
* `thumbnail`;
* `language_detection`;
* `content_warning`;
* `processing_error`.

Applications MAY use namespaced artifact kinds, such as:

```text
org.samosa.parb_experiment_metadata
```

### 15.3 Selectors

`selector_json` identifies the part of the source represented by an artifact.

Examples:

Page range:

```json
{"type":"pages","start":12,"end":14}
```

Spreadsheet range:

```json
{"type":"sheet_range","sheet":"Force Data","range":"A1:G240"}
```

Image region:

```json
{"type":"image_region","x":0.12,"y":0.18,"width":0.54,"height":0.33,"units":"normalized"}
```

Audio or video interval:

```json
{"type":"time_range","start_ms":12500,"end_ms":48700}
```

Text byte range:

```json
{"type":"byte_range","start":2401,"end":8120}
```

If an artifact describes the entire source, `selector_json` MAY be null.

### 15.4 Artifact status

`status` MUST be one of:

* `active`;
* `stale`;
* `superseded`;
* `failed`;
* `deleted`.

Only `active` artifacts SHOULD be returned by default search operations.

## 16. Producer provenance

### 16.1 Producer kinds

`producer_kind` MUST be one of:

* `parser`;
* `model`;
* `application`;
* `human`;
* `pipeline`;
* `unknown`.

### 16.2 Model-generated artifacts

For `model_generated` artifacts, the producer record MUST include:

* `name`;
* `model_id`;
* `model_revision` or `weights_hash` when available;
* `app_name` and `app_version`;
* `runtime` when known.

It SHOULD include:

* quantization;
* tokenizer identity;
* vision encoder identity;
* projector identity;
* inference parameters that materially affect output.

Example:

```json
{
  "producer_kind": "model",
  "name": "Bonsai",
  "model_id": "example/Bonsai-1B",
  "model_revision": "8f12c4d",
  "weights_hash": "sha256:...",
  "quantization": "Q4",
  "runtime": "samosa-c-engine-0.8",
  "app_name": "Samosa",
  "app_version": "0.8.2"
}
```

A consumer MUST NOT infer that two records with the same marketing name use identical weights. Revision or weight identity matters.

### 16.3 Derivation record

A derivation connects an output artifact to its inputs and producer.

It MUST contain:

* producer ID;
* operation;
* input references;
* creation time.

It SHOULD contain:

* processing parameters;
* prompt or recipe hash;
* chunking strategy;
* preprocessing details.

A recipe hash allows consumers to distinguish artifacts produced by the same model using materially different prompts or preprocessing.

The full prompt MAY be stored, but Chutni does not require it because prompts may contain private or proprietary data.

## 17. Representations

A representation is acceleration data derived from an artifact.

### 17.1 Text embeddings

A text embedding record MUST identify:

* embedding model ID;
* model revision;
* dimensions;
* data type;
* normalization method;
* source artifact hash.

Consumers MUST reuse an embedding only when they understand and accept its exact compatibility profile.

### 17.2 Image embeddings

An image embedding MUST identify the image encoder and revision. If a projector or adapter is involved, it MUST also identify that projector.

Generic visual embeddings and VLM-specific projected embeddings MUST use different `representation_kind` values.

### 17.3 Token IDs

Token-ID representations MUST identify the tokenizer configuration or tokenizer hash. Token IDs from different tokenizers MUST NOT be treated as interchangeable.

### 17.4 Vision tokens

Vision tokens or projected vision embeddings are model-specific. They MUST identify:

* vision encoder;
* preprocessing recipe;
* resolution or patch configuration;
* projector;
* target language-model family when applicable.

### 17.5 Indexes

Indexes MAY be built from representations or artifacts. Index format is implementation-specific in v0.1.

An index MUST be treated as invalid if any of its declared source artifacts or representations have changed.

## 18. Relationships

Chutni may represent relationships using `relations`.

Core predicates include:

* `contains`;
* `derived_from`;
* `duplicate_of`;
* `near_duplicate_of`;
* `references`;
* `version_of`;
* `supersedes`;
* `same_document_as`;
* `attachment_of`;
* `thumbnail_of`.

Relations created by a model MUST carry a derivation ID.

A relation is not automatically a verified fact. Consumers should inspect its provenance.

## 19. Search model

Chutni standardizes a minimum query interface, but not one ranking algorithm.

### 19.1 Search modes

A search provider MAY support:

* `metadata`;
* `lexical`;
* `semantic`;
* `hybrid`;
* `image_similarity`;
* `relationship`.

### 19.2 Search request

A transport-neutral request has the following logical shape:

```json
{
  "query": "ParB condensation force with PEG",
  "modes": ["hybrid"],
  "limit": 20,
  "filters": {
    "media_types": ["application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"],
    "artifact_kinds": ["summary_short", "extracted_text", "ocr_text"],
    "status": ["active"]
  },
  "return": {
    "sources": true,
    "artifacts": true,
    "snippets": true,
    "provenance": true
  }
}
```

### 19.3 Search result

A result SHOULD contain:

```json
{
  "source_id": "...",
  "artifact_id": "...",
  "display_path": "/Research/Paper/Figure3.tif",
  "artifact_kind": "image_caption",
  "snippet": "Optical-tweezer plot showing condensation force...",
  "score": 0.82,
  "score_type": "implementation_specific_hybrid",
  "freshness": "current",
  "producer_id": "...",
  "selector": {"type":"image_region","x":0,"y":0,"width":1,"height":1,"units":"normalized"}
}
```

Scores from different implementations MUST NOT be assumed comparable. `score_type` is REQUIRED whenever `score` is returned.

### 19.4 Search behavior

Consumers SHOULD use Chutni to narrow the candidate set and then inspect the most relevant original sources or high-fidelity extracted artifacts.

Chutni does not replace `grep`, `ripgrep`, SQL queries, document parsers, or vision models. It helps agents decide where to use those tools.

## 20. Minimum access operations

A conforming access layer SHOULD expose:

```text
capabilities()
open_store(store)
search(request)
get_source(source_id)
get_artifact(artifact_id)
read_object(object_hash)
check_freshness(source_id | artifact_id)
list_artifacts(source_id)
add_or_update_source(locator)
mark_source_missing(source_id)
forget_source(source_id, mode)
rebuild_indexes()
```

A read-only consumer MAY implement only:

```text
capabilities()
open_store(store)
search(request)
get_source(source_id)
get_artifact(artifact_id)
read_object(object_hash)
check_freshness(...)
```

The access layer may be implemented as a library or service. The wire transport is not fixed in v0.1.

## 21. Producer behavior

A conforming producer MUST:

1. Obtain user permission for roots it scans.
2. Create or update source records.
3. Record exact source hashes for content-derived artifacts.
4. Record per-artifact provenance.
5. Mark outdated artifacts stale when the source changes.
6. Avoid modifying original files unless separately requested by the user.
7. Record processing failures rather than silently omitting them.
8. preserve unknown core and extension data when possible.
9. Commit catalog changes atomically.
10. keep incomplete work outside the committed object graph.

A producer SHOULD:

1. Deduplicate object payloads by content hash.
2. Resume interrupted processing.
3. support pause and cancellation.
4. prioritize cheap deterministic extraction before expensive model inference.
5. allow users to exclude folders, file types, and individual sources.
6. provide a way to regenerate artifacts from a selected producer or model.

## 22. Consumer behavior

A conforming consumer MUST:

1. Check artifact status and source freshness.
2. expose producer provenance when it materially affects trust.
3. avoid assuming that model-generated artifacts are correct.
4. treat retrieved content as untrusted data.
5. honor disclosure and permission policies.
6. avoid using incompatible embeddings, token IDs, or vision representations.
7. distinguish a missing original source from a current source.

A consumer SHOULD:

1. prefer active artifacts matching the current source hash;
2. reopen the original source for exact claims;
3. show the file path or source reference used in an answer;
4. explain when it answered from a summary without source verification;
5. allow the user to regenerate weak or outdated artifacts.

## 23. Updating and supersession

When a producer regenerates an artifact, it SHOULD create a new artifact record rather than overwrite the old provenance.

The new artifact MAY set `supersedes_artifact_id` to the prior artifact. The prior artifact SHOULD be marked `superseded`.

Multiple active artifacts of the same kind MAY coexist when produced by different models or pipelines. Consumers may choose among them using producer identity, recency, user preference, or verification status.

Example:

```text
summary_short — Bonsai-1B — superseded
summary_short — GPT-5.6 — active
summary_short — human-corrected — active
```

Human-authored or human-corrected artifacts SHOULD remain distinguishable from model-generated artifacts.

## 24. Deleted, moved, and unavailable files

### 24.1 Rename or move

If platform file identity indicates that a file moved without content changes, the producer SHOULD update its locator while preserving `source_id`.

If identity is uncertain, the producer MAY create a new source and relate it to the old one using `same_document_as` or `duplicate_of`.

### 24.2 Deleted source

When a file is deleted, its source state SHOULD become `deleted` or `missing`.

The store MAY retain derived artifacts to preserve historical memory, depending on root policy. Retained artifacts MUST clearly indicate that the original source is unavailable.

### 24.3 Forget operation

`forget_source` MUST support at least:

* `catalog_only` — remove source and metadata while leaving unreferenced objects for later garbage collection;
* `artifacts` — remove source-derived artifacts and representations;
* `secure_logical_delete` — remove references and schedule object deletion;
* `purge` — remove records and unshared object payloads immediately when possible.

The protocol cannot guarantee physical erasure on copy-on-write file systems, SSDs, backups, or synchronized storage. Applications MUST not claim guaranteed forensic erasure.

## 25. Multimodal ingestion guidance

The protocol does not require one ingestion pipeline, but recommends staged processing.

### 25.1 Text documents

```text
file → parser → extracted text → chunks → summaries → embeddings/indexes
```

### 25.2 Images

```text
image → metadata → perceptual hash → thumbnail → OCR → caption → visual embedding
```

For large image collections, producers SHOULD perform cheap operations first, batch expensive vision inference, and cache reusable outputs.

### 25.3 Audio and video

```text
media → metadata → speech transcript → time-aligned chunks → summary → embeddings
```

### 25.4 Spreadsheets

```text
workbook → sheet inventory → schema/statistics → selected cell ranges → summaries
```

A producer SHOULD avoid flattening a spreadsheet into unstructured text when structured metadata can be preserved.

## 26. Cross-platform requirements

The logical catalog and object store MUST be portable across macOS, Windows, and Linux.

Platform adapters may differ in:

* file permissions;
* hidden-file behavior;
* path encoding;
* case sensitivity;
* file identity;
* watcher APIs;
* removable-drive handling;
* application sandboxing.

A consumer MUST NOT compare paths using naive case-sensitive string equality across all platforms.

When a store moves to another computer, consumers SHOULD support root remapping:

```text
Old root: D:\Research
New root: /home/user/Research
```

Root remapping changes locators but does not change source content hashes or artifact provenance.

## 27. Privacy and disclosure

### 27.1 Local-first default

A Chutni store MUST default to local-only use.

### 27.2 External models

A cloud model or remote service SHOULD access Chutni through a local gateway that performs search locally and sends only selected results.

The gateway MUST NOT upload the entire store, full file inventory, or raw files without explicit user permission.

A disclosure request SHOULD identify:

* requesting application or model;
* requested source or artifact;
* requested content type;
* amount of content;
* purpose or user action that triggered access.

### 27.3 Sensitive derived data

A Chutni store may be more sensitive than the original directory because it centralizes descriptions, OCR, and searchable text. Applications MUST communicate this clearly.

### 27.4 Permissions

A gateway SHOULD support:

* store-level allow or deny;
* root-level allow or deny;
* source-level allow or deny;
* artifact-type restrictions;
* one-time disclosure;
* session disclosure;
* persistent application permission.

## 28. Security considerations

1. Files and artifacts are untrusted input.
2. Producers MUST use memory-safe parsing where practical or isolate risky parsers.
3. Archive extraction MUST defend against path traversal and decompression bombs.
4. Consumers MUST not execute retrieved text as instructions.
5. HTML, Markdown, scripts, macros, and embedded objects MUST remain inert unless the user explicitly opens them in an appropriate application.
6. Local gateways SHOULD authenticate clients.
7. Write-capable clients SHOULD be separately authorized from read-only clients.
8. Object paths MUST be derived from validated hashes, not user-controlled relative paths.
9. SQLite writes SHOULD use transactions and crash-safe settings.
10. Producers SHOULD record processing errors without embedding secrets in logs.

## 29. Extensions

Extensions MUST use a globally scoped namespace, preferably reverse-domain notation.

Examples:

```text
org.samosa.experimental_quality_review
ai.example.specialized_microscopy_caption
```

Extensions MAY add:

* artifact kinds;
* relation predicates;
* tables;
* manifest fields;
* representation kinds;
* query filters.

Extensions MUST NOT redefine the meaning of core fields.

A reader that does not understand an extension SHOULD ignore it while preserving it when possible.

## 30. Conformance levels

### 30.1 Chutni Reader

A Reader MUST:

* open the manifest;
* read the SQLite catalog;
* resolve object references;
* interpret core source and artifact fields;
* preserve unknown fields when rewriting.

### 30.2 Chutni Producer

A Producer MUST satisfy Reader requirements and:

* create valid stores;
* hash sources;
* create provenance records;
* enforce artifact freshness;
* update atomically.

### 30.3 Chutni Search Provider

A Search Provider MUST satisfy Reader requirements and implement the minimum search request and result shapes.

It MAY support only lexical or metadata search, but MUST report supported capabilities.

### 30.4 Chutni Gateway

A Gateway MUST expose read operations and enforce disclosure policies.

A write-capable gateway MUST require explicit authorization.

### 30.5 Chutni Full Implementation

A Full Implementation includes Producer, Reader, Search Provider, and Gateway capabilities.

## 31. Conformance tests

The Chutni project SHOULD publish a test suite containing:

1. A minimal valid store.
2. A store with unknown extension fields.
3. A moved-root scenario.
4. A changed-source scenario with stale artifacts.
5. Multiple summaries from different models.
6. Shared content-addressed objects.
7. Deleted and missing sources.
8. Text, image, spreadsheet, and audio examples.
9. Invalid object hashes.
10. Path-encoding edge cases.
11. Prompt-injection text that consumers must treat as data.
12. Representation compatibility and incompatibility cases.

A conforming implementation SHOULD publish which conformance level and optional capabilities it passes.

## 32. Example: image artifact

Source record:

```json
{
  "source_id": "0195f0d2-8f70-7aa0-9179-f7831f4a8af3",
  "source_kind": "file",
  "locator": {
    "scheme": "file",
    "platform": "macos",
    "display_path": "/Users/deepan/Research/setup.jpg"
  },
  "media_type": "image/jpeg",
  "content_hash": "blake3:1f2a...",
  "state": "present"
}
```

Caption artifact:

```json
{
  "artifact_id": "0195f0d3-caf5-76ab-8ee7-b182a7a19ddd",
  "source_id": "0195f0d2-8f70-7aa0-9179-f7831f4a8af3",
  "artifact_kind": "image_caption",
  "artifact_origin": "model_generated",
  "media_type": "text/plain; charset=utf-8",
  "inline_text": "Laboratory optical-tweezer setup with two monitors and a microscope enclosure.",
  "source_content_hash": "blake3:1f2a...",
  "derivation_id": "0195f0d3-a6f1-7634-bca7-cd6d3661ed86",
  "status": "active"
}
```

Producer:

```json
{
  "producer_id": "0195f0d3-63c6-7229-a74c-995fd38f27c0",
  "producer_kind": "model",
  "name": "Bonsai",
  "model_id": "example/Bonsai-1B",
  "model_revision": "8f12c4d",
  "quantization": "Q4",
  "runtime": "Samosa Vision Runtime 0.1",
  "app_name": "Samosa",
  "app_version": "0.8.2"
}
```

A different application may keep this caption, add a stronger caption from another model, or regenerate it. It does not need to repeat the image hash, thumbnail generation, or OCR unless those artifacts are missing or incompatible.

## 33. Example workflows

### 33.1 Initial preparation

```text
User selects folders
→ producer scans files
→ deterministic parsers run
→ expensive model jobs run gradually
→ artifacts and provenance are committed
→ indexes are built
```

The user may continue using the computer while preparation runs. An interrupted producer SHOULD resume from committed state.

### 33.2 Local question

```text
User asks a question
→ consumer searches Chutni
→ likely sources are returned
→ consumer opens the best source or excerpt
→ consumer verifies freshness
→ model answers with source references
```

### 33.3 Cloud-model question

```text
Cloud model requests search
→ local gateway searches Chutni
→ gateway returns a small candidate list
→ user or policy authorizes selected excerpts
→ only those excerpts are disclosed
→ cloud model answers
```

### 33.4 Application replacement

```text
Samosa creates Chutni
→ user uninstalls Samosa
→ Chutni remains in a user-owned location
→ another compatible application opens it
→ compatible artifacts are reused
→ incompatible embeddings are regenerated
```

### 33.5 Stronger-model upgrade

```text
Store contains Bonsai-generated summaries
→ new application identifies producer metadata
→ user requests reprocessing with a stronger model
→ only selected summaries are regenerated
→ old artifacts are superseded but provenance remains
```

## 34. Recommended reference implementation

The Chutni project should provide:

1. A portable C99 core library.
2. A stable C ABI.
3. A command-line tool.
4. A minimal local service.
5. A conformance-test package.
6. Example producers and consumers.

> **Amended in 0.1-draft (2026-07-29).** Item 1 previously read "a Rust core
> library". The reference implementation in this repository is C99 instead, so
> that applications written in C — which includes several local inference
> engines — can link it directly without a foreign-function boundary or a
> second toolchain. Bindings in Rust and other languages sit **on top of** the
> C ABI in item 2 rather than underneath it. This is a decision about the
> reference implementation only; the protocol itself constrains no language.

Suggested commands:

```bash
chutni init ~/Memory.chutni
chutni add-root ~/Documents
chutni scan
chutni search "ParB condensation force"
chutni inspect <source-id>
chutni verify <source-id>
chutni forget <source-id>
chutni pack ~/Memory.chutni ~/Memory.chutnipack
```

The reference implementation should be open source and should not give Samosa private extensions preferential treatment.

## 35. Versioning

Chutni uses semantic specification versions.

* Patch versions clarify behavior without changing compatibility.
* Minor versions add backward-compatible fields or capabilities.
* Major versions may change required structure or semantics.

Readers MUST reject unsupported major versions unless operating in an explicit best-effort recovery mode.

Writers SHOULD write the oldest specification version that accurately represents the features used.

## 36. Governance principles

The protocol should be governed independently from any single application.

Recommended principles:

1. Public specification repository.
2. Open issue and proposal process.
3. Royalty-free implementation rights.
4. Public conformance tests.
5. No requirement to use Samosa.
6. No model-vendor preference.
7. User ownership of stores.
8. Backward compatibility as a priority.

## 37. Open questions for v0.2

The following are intentionally not finalized in v0.1:

1. A standard encrypted-store profile.
2. A standard remote synchronization protocol.
3. A common vector-index interchange format.
4. Standard producer reputation or external benchmarking metadata.
5. Human verification and correction workflows.
6. Distributed stores spanning several computers.
7. Conflict resolution for simultaneous writers.
8. Standardized access-control lists inside the store.
9. Portable filesystem snapshots or source bundling.
10. Conversation and agent-memory interoperability.

## 38. One-sentence definition

> **Chutni is an open, source-backed, model-transparent memory protocol that lets AI applications prepare a user’s local files once and reuse that memory across models, applications, and operating systems.**

## 39. Store discovery

A memory bank that applications cannot find is not shared memory. Sections 7
and 8 define what a store *is*; this section defines how an application
determines whether one already exists on a computer, so that a second
application does not rebuild memory a first application already produced.

An application MUST be able to answer "does Chutni memory exist here?" without
scanning the filesystem and without any prior agreement with whichever
application created the store.

### 39.1 Resolution order

A consumer looking for a store SHOULD consult these sources, in order, and
SHOULD combine the results rather than stopping at the first hit:

1. **Explicit selection.** A path given on the command line or through an
   application's own configuration.
2. **The `CHUTNI_STORE` environment variable**, naming one store directory.
3. **The user registry**, described in §39.2.
4. **Conventional locations**: entries whose names end in `.chutni` directly
   inside the user's home directory, inside the user's documents directory, and
   inside `~/.chutni/`.

A consumer MUST NOT walk the whole filesystem looking for stores. Discovery is
a bounded lookup, not a search.

### 39.2 The user registry

The registry is a UTF-8 JSON file recording stores the user has chosen to make
discoverable. Its location is:

* `$CHUTNI_HOME/registry.json` when `CHUTNI_HOME` is set; otherwise
* `~/.chutni/registry.json`.

Windows implementations MAY use the roaming application-data directory instead,
and SHOULD document which path they use.

```json
{
  "format": "chutni-registry",
  "version": "1",
  "stores": [
    { "path": "/Users/deepan/Research.chutni", "registered_at": "2026-07-29T10:00:00Z" }
  ]
}
```

Required entry field: `path`, an absolute path to a store directory.

A registry entry is a **hint, not a grant**. It records that a store exists at a
path; it does not authorize any application to read it. Consumers MUST still
apply §27's disclosure and permission rules, and MUST treat a registry naming a
store the user has not authorized for that application as a store they may not
open.

Producers SHOULD register the stores they create. A producer MUST NOT register
a store the user did not ask to be discoverable, and MUST provide a way to
unregister one.

An entry whose `path` no longer holds a store MUST be ignored rather than
treated as an error; stores move and are deleted.

### 39.3 Identifying a directory as a store

A directory is a Chutni store when it contains a readable `manifest.json` whose
`format` field equals `chutni`. Consumers MUST verify that field rather than
relying on the `.chutni` suffix, which is a convention (§7.1) and not a
guarantee.

A consumer that finds a store whose `spec_version` has a major version it does
not implement MUST report it as present but unsupported (§35), rather than
omitting it. A user is better served by "memory exists here but this
application is too old to read it" than by silence.

### 39.4 Creating memory when none exists

An application that finds no store MAY offer to create one. It MUST NOT create
a store, add roots, or index files without user permission (§21.1, §11).

Where several applications share one computer, creating **one** store the user
owns is preferable to each application creating its own. An application SHOULD
therefore search before it creates, and SHOULD default to the store it found.

### 39.5 What discovery does not settle

Discovery reports that memory exists. It does not establish that the memory is
current (§13), that its artifacts are correct (§4.2, §6.2), or that the
requesting application is entitled to read it (§27). A consumer MUST perform
those checks separately.
