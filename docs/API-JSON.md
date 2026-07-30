# The `chutni_call` JSON surface

`chutni_call` is one function:

```c
chutni_status chutni_call(chutni_store *store, const char *operation,
                          const char *arguments_json, char **result_json);
```

Every operation this document describes goes through it. A language binding
needs exactly four foreign functions to reach the whole store — `chutni_open`,
`chutni_call`, `chutni_free`, `chutni_close` — instead of mirroring every
typed struct in `include/chutni.h` field-for-field. This is what
[docs/tickets/T02-python-binding.md](tickets/T02-python-binding.md) and
[T11-typescript-binding.md](tickets/T11-typescript-binding.md) bind against.

The typed C API in `chutni.h` remains the primary, stable ABI other C
applications compile against. `chutni_call` is a second surface over the same
implementation underneath it — not a replacement, and not a separate source of
truth. Every operation below is a thin wrapper around a typed function that
already exists; if the two ever disagree, the typed function is right and this
is a bug.

Every example on this page is real, captured output from a real store —
running `build/call_surface <workdir>` reproduces it (your IDs, timestamps,
and hashes will differ; the shapes will not). See
[docs/tickets/T01-json-tool-surface.md](tickets/T01-json-tool-surface.md) for
why this exists.

## Calling convention

- `store` is an already-open handle from `chutni_open`, or `NULL` for the two
  store-less operations (`discover`, `capabilities`). Every other operation
  called with `store == NULL` fails with `CHUTNI_ERR_INVALID`.
- `arguments_json` is a JSON object, or `NULL`/`"{}"` for operations that take
  none. **Unrecognized keys are ignored.** This is deliberate: a caller can
  hand `chutni_call` the exact object it received over its own transport
  without stripping fields first — see how `chutni-mcp`'s wrappers pass
  `store_path` and `confirmed` straight through in `src/mcp.c`.
- Read-only vs. read-write is decided by how the caller opened the store, not
  by the operation name. A mutating operation on a read-only handle fails with
  `CHUTNI_ERR_READONLY`, exactly like the typed function underneath it.
- `chutni_call` enforces no permission or confirmation policy of its own.
  §27 disclosure rules and any "did the user actually approve this" gating
  belong to the host, above this layer.

## The error envelope

On success, `chutni_call` returns `CHUTNI_OK` and `*result_json` is the
operation's result object — no wrapper, no `"ok"` field mandated by this
layer (individual operations may include one; don't rely on its presence,
rely on the return status).

On failure — including an unrecognized operation name — the return value is
the failing `chutni_status`, and `*result_json` is:

```json
{"error": {"code": "<chutni_strerror string>", "message": "<detail>"}}
```

`code` is one of the twelve strings `chutni_strerror` returns (`"invalid
argument"`, `"not found"`, `"denied by policy"`, `"store opened read-only"`,
…) — coarse and stable across versions, not a fine-grained per-operation
vocabulary. `message` is the specific detail, meant for logs and error
messages, not for `switch`-style matching; match on `code` (or the numeric
`chutni_status`, if you're calling from C) when you need to branch.

```json
{"error":{"code":"invalid argument","message":"unknown operation: not_a_real_operation"}}
{"error":{"code":"invalid argument","message":"operation \"search\" requires an open store"}}
{"error":{"code":"store opened read-only","message":"store is read-only"}}
```

`*result_json` is always set when this function can form a response at all
(essentially always); free it with `chutni_free`.

## Store-less operations

### `discover`

No arguments. Wraps `chutni_discover` (§39). Enumerates `$CHUTNI_STORE`, the
registry, then conventional locations — never the whole filesystem.

```
chutni_call(NULL, "discover", "{}", &result)
{"ok":true,"count":3,"stores":[
  {"store_path":"/Users/deepan/Documents/projects/notes.chutni",
   "store_id":"019fafce-...","label":"notes","spec_version":"0.1","readable":true},
  ...
]}
```

### `capabilities`

No arguments. Protocol/library facts: spec version, artifact kinds, relation
predicates, definition modes and stop reasons, selector types. Meaningful to
any consumer, independent of any particular store.

```
chutni_call(NULL, "capabilities", "{}", &result)
{"ok":true,"spec_version":"0.2","library_version":"0.3.0",
 "artifact_origins":["direct","deterministic_transform","model_generated","human"],
 "core_artifact_kinds":["file_metadata","extracted_text",...,"coverage_manifest",
                        "memory"],
 "capabilities":["sources","artifacts","provenance","hierarchical_sources",
                 "bounded_coverage","directory_definitions","standalone_memory"],
 "definition_stop_reasons":["max_depth_reached","producer_classified_coherent",...],
 "definition_modes":["adaptive","per_source"],
 "selector_types":["pages","sheet_range","image_region","time_range","byte_range"],
 "semantic_validation":"not_performed","writer_policy":"single_writer_many_readers"}
```

`chutni-mcp`'s own `chutni_capabilities` tool adds service-specific fields
(`transports`, `reference_scanner` limits) on top of this — those describe
the stdio service, not the protocol, so they aren't part of this operation.

## Read operations (store required, read-only handle is enough)

### `store_info`

No arguments. Store identity, counts, and every authorized root with its
policy.

```
chutni_call(s, "store_info", "{}", &result)
{"ok":true,"store_path":"/private/tmp/x/store.chutni","store_id":"019fb3b1-...",
 "spec_version":"0.2",
 "counts":{"roots":1,"sources":4,"sources_files":2,"sources_directories":2,
           "sources_opaque_directories":0,"relations":5,"artifacts":7,
           "artifacts_active":7,"artifacts_stale":0,"objects":3,
           "producers":1,"derivations":4},
 "roots":[{"root_id":"019fb3b1-...","path":"/private/tmp/x/tree","label":"tree",
           "policy":{"recursive":true,"follow_symlinks":false,"include_hidden":false,
                     "retain_deleted_artifacts":true,"max_file_size_bytes":2147483648,
                     "exclude_globs":[],"max_depth":null}}]}
```

`policy.max_depth: null` means unbounded (§11.1) — never read it as zero.

### `search`

Lexical search (§19), wrapping `chutni_search`.

| Argument | Type | Default | Notes |
|---|---|---|---|
| `query` | string | required | |
| `limit` | int | 10 | clamped to 1–100 |
| `kind` | string | none | filter to one `artifact_kind` |
| `include_stale` | bool | false | §15.4 |
| `match_any` | bool | false | OR literal terms instead of AND |

```
chutni_call(s, "search", "{\"query\":\"condensation force\",\"limit\":5}", &result)
{"ok":true,"query":"condensation force","count":1,"results":[
  {"source_id":"019fb3b1-...","artifact_id":"019fb3b1-...",
   "display_path":"/private/tmp/x/tree/parb.md","artifact_kind":"extracted_text",
   "snippet":"The condensation force was measured at 12 pN using PEG.\n",
   "producer_id":"019fb3b1-...","freshness":"current",
   "score":1.5908449623596481,"score_type":"bm25_fts5_negated",
   "source_kind":"file","parent_source_id":"019fb3b1-...",
   "coverage_manifest_id":"019fb3b1-...","depth":1}]}
```

`source_kind`, `parent_source_id`, `coverage_manifest_id`, and `depth` are the
§19.3 fields a store advertising `bounded_coverage` returns on every result —
read `coverage_manifest_id` (via `coverage`, below) before treating a hit as
evidence the surrounding region was exhaustively indexed.

### `search_semantic`

Brute-force cosine search (§17, §19.1) over stored representations, wrapping
`chutni_search_semantic`. **New** — no MCP tool exposed this before T01.

| Argument | Type | Notes |
|---|---|---|
| `vector` | array of numbers | required, the query embedding |
| `profile` | object | required; see [`put_representation`](#put_representation) |
| `limit` | int | default 10, clamped to 1–100 |
| `kind` | string | filter to one `artifact_kind` |
| `include_stale` | bool | default false |

Only representations matching `profile` exactly are compared (§22.6) — a
mismatched profile returns zero results, not an error.

```
chutni_call(s, "search_semantic",
  "{\"vector\":[0.11,0.19,0.31,0.39],\"profile\":{\"representation_kind\":\"text_embedding\",\"model_id\":\"example/embed\",\"model_revision\":\"1\",\"dtype\":\"f32\",\"normalization\":\"none\"},\"limit\":5}",
  &result)
{"ok":true,"count":1,"results":[
  {"source_id":"019fb3b1-...","artifact_id":"019fb3b1-...",
   "display_path":"/private/tmp/x/tree/Sub/inner.md","artifact_kind":"summary_short",
   "snippet":"A note about marsupial pouches.","freshness":"current",
   "score":0.99934751520422371,"score_type":"cosine_bruteforce"}]}
```

`score_type` is always `"cosine_bruteforce"` in this implementation; §19.3
forbids comparing it against another implementation's scores.

### `children`

Immediate entries of a directory source (§20 `list_children`).

| Argument | Type | Notes |
|---|---|---|
| `source_id` | string | either this |
| `source_path` | string | or this |

```
chutni_call(s, "children", "{\"source_path\":\"/private/tmp/x/tree\"}", &result)
{"ok":true,"source_id":"019fb3b1-...","count":2,"children":[
  {"source_id":"019fb3b1-...","display_path":"/private/tmp/x/tree/Sub",
   "source_kind":"directory","parent_source_id":"019fb3b1-...",
   "content_hash":"blake3:7e324c...","state":"present",
   "observation":"enumerated","depth":1,"size_bytes":0},
  {"source_id":"019fb3b1-...","display_path":"/private/tmp/x/tree/parb.md",
   "source_kind":"file","parent_source_id":"019fb3b1-...",
   "media_type":"text/markdown","content_hash":"blake3:97b0c3...",
   "state":"present","depth":1,"size_bytes":56}]}
```

`observation: "opaque"` on a directory child means its name was observed and
it was never opened (§12.5) — nothing in the store describes what's inside it.
An empty `children` array carries a `note` saying exactly that, rather than
letting silence be misread as "this directory is empty".

### `coverage`

What a scan actually reached for a region (§15.7), wrapping
`chutni_get_coverage`.

| Argument | Type | Notes |
|---|---|---|
| `root_id` | string | a root to report on |
| `source_id` / `source_path` | string | a source whose region to report on |
| *(none)* | | falls back to the store's one root, if there is exactly one |

```
chutni_call(s, "coverage", "{}", &result)
{"source_id":"019fb3b1-...","source_kind":"directory","state":"present",
 "observation":"enumerated","depth":0,"root_source_id":"019fb3b1-...",
 "coverage_manifest_id":"019fb3b1-...",
 "coverage_manifest":{
   "scan_generation":"019fb3b1-...","root_source_id":"019fb3b1-...",
   "policy":{"max_depth":null,"recursive":true,"follow_symlinks":false,"include_hidden":false},
   "coverage":{"deepest_directory_enumerated":1,"directories_observed":2,
               "directories_enumerated":2,"directories_defined":0,
               "directories_collapsed":0,"files_observed":2,"files_hashed":2,
               "files_read":2,"files_defined":0,"depth_limited_directories":0,
               "excluded_sources":0,"unsupported_sources":0,
               "sources_marked_missing":0,"errors":0},
   "complete_for_policy":true},
 "definition":null,
 "interpretation":"complete_for_policy means the requested bounded operation finished. It does not mean the subtree was read. Directories whose observation is \"opaque\" were named but never opened, and nothing in this store describes their contents."}
```

Read `interpretation` before quoting `complete_for_policy` to anyone —
that field exists specifically so an agent that only reads one string still
gets the caveat.

### `get_source`

A single source's full record, by id or path.

| Argument | Type |
|---|---|
| `source_id` | string, either this |
| `source_path` | string, or this |

```
chutni_call(s, "get_source", "{\"source_path\":\"/private/tmp/x/tree\"}", &result)
{"source_id":"019fb3b1-...","display_path":"/private/tmp/x/tree",
 "source_kind":"directory","content_hash":"blake3:4cef8e...","state":"present",
 "observation":"enumerated","depth":0,"size_bytes":0}
```

### `get_artifact`

A single artifact by id — the one §20 operation with no typed-API equivalent
(`chutni_list_artifacts` filters by *source*, not artifact id).

| Argument | Type |
|---|---|
| `artifact_id` | string, required |

```
chutni_call(s, "get_artifact", "{\"artifact_id\":\"019fb3b1-...\"}", &result)
{"artifact_id":"019fb3b1-...","artifact_kind":"extracted_text",
 "artifact_origin":"deterministic_transform",
 "media_type":"text/plain; charset=utf-8","status":"active",
 "created_at":"2026-07-30T15:42:42Z","source_content_hash":"blake3:97b0c3...",
 "producer_name":"chutni-reference-scanner","producer_kind":"parser",
 "operation":"extract_text","derivation_id":"019fb3b1-...",
 "source_id":"019fb3b1-..."}
```

### `read_object`

Resolves a content-addressed object (§14) to its payload.

| Argument | Type |
|---|---|
| `object_hash` | string, required, `"blake3:<hex>"` form |

Text and `application/json` media types come back as `text`; everything else
comes back as `data_base64` (standard, padded base64) — JSON has no byte
string type.

```
chutni_call(s, "read_object", "{\"object_hash\":\"blake3:31a9b9...\"}", &result)
{"object_hash":"blake3:31a9b9...","media_type":"application/json","size_bytes":563,
 "text":"{\"scan_generation\":\"...\", ...}"}
```

### `check_freshness`

Freshness of a source or artifact (§13.3), re-derived from disk — never a
stored value.

| Argument | Type |
|---|---|
| `source_id` / `artifact_id` | string, one required |
| `source_path` | string, alternative to `source_id` |

```
chutni_call(s, "check_freshness", "{\"artifact_id\":\"019fb3b1-...\"}", &result)
{"id":"019fb3b1-...","freshness":"current"}
```

`freshness` is `"current"`, `"stale"`, `"missing"`, or `"unknown"`.

### `list_artifacts`

Every artifact for one source — the raw rows, without `source_context`'s
content-loading or per-item freshness checks.

| Argument | Type |
|---|---|
| `source_id` / `source_path` | string, one required |
| `include_stale` | bool, default false |

```
chutni_call(s, "list_artifacts", "{\"source_path\":\"/private/tmp/x/tree/parb.md\"}", &result)
{"source_id":"019fb3b1-...","count":2,"artifacts":[
  {"artifact_id":"...","artifact_kind":"file_metadata","artifact_origin":"direct",
   "media_type":"application/json","status":"active",
   "created_at":"2026-07-30T15:42:42Z","source_content_hash":"blake3:97b0c3...",
   "producer_name":"chutni-reference-scanner","producer_kind":"parser",
   "operation":"record_file_metadata","derivation_id":"..."},
  {"artifact_id":"...","artifact_kind":"extracted_text", "..." : "..."}]}
```

### `source_context`

Every current interpretation of a source, grouped, with content and full
provenance — the richest read operation, and the most expensive.

| Argument | Type |
|---|---|
| `source_id` / `source_path` | string, one required |
| `include_stale` | bool, default false |
| `max_text_chars` | int, default 32768, clamped to 0–262144 |

```
chutni_call(s, "source_context", "{\"source_path\":\"/private/tmp/x/tree/parb.md\"}", &result)
{"ok":true,
 "source":{"source_id":"...","display_path":"/private/tmp/x/tree/parb.md",
           "media_type":"text/markdown","content_hash":"blake3:97b0c3...",
           "state":"present","size_bytes":56,"freshness":"current"},
 "artifact_count":2,
 "artifacts":[
   {"artifact_id":"...","artifact_kind":"extracted_text", ...,
    "freshness":"current","semantic_validation":"not_performed",
    "content":"The condensation force was measured at 12 pN using PEG.\n",
    "content_truncated":false,
    "provenance":{
      "producer":{"producer_id":"...","app_name":"call-surface",
                  "app_version":"0.3.0","producer_kind":"parser",
                  "name":"chutni-reference-scanner"},
      "derivation":{"derivation_id":"...","operation":"extract_text",
                    "created_at":"...","parameters":{"strategy":"whole_file_utf8"},
                    "inputs":[]}}}, ...]}
```

`semantic_validation: "not_performed"` appears on every artifact, always
(§6.2, §16.4) — structural validity is not a claim that the content is true.

## Write operations (store required, must be opened read-write)

### `scan`

Depth-bounded scan of one root or the whole store (§11.1, §20 `scan`).

| Argument | Type | Notes |
|---|---|---|
| `root_id` | string | one root; omit to scan every authorized root |
| `max_depth` | int | overrides the root's stored policy for this run only |
| `max_file_size_bytes` | int | per-file safety cap |
| `app_name` / `app_version` | string | recorded in scanner provenance |

```
chutni_call(s, "scan", "{\"app_name\":\"my-app\"}", &result)
{"ok":true,"store_path":"/private/tmp/x/store.chutni",
 "scan":{"files_seen":2,"sources_indexed":2,"unchanged":0,"text_artifacts":2,
         "metadata_artifacts":2,"skipped":0,"errors":0,
         "directories_observed":2,"directories_enumerated":2,
         "depth_limited_directories":0,"listing_artifacts":2,
         "listings_reused":0,"files_hashed":2,"files_read":2,
         "excluded_sources":0,"unsupported_sources":0,
         "sources_marked_missing":0,"deepest_directory_enumerated":1,
         "complete_for_policy":true},
 "counts":{"roots":1,"sources":4, "...": "..."}}
```

`complete_for_policy: true` means the requested bounded operation finished —
**not** that the whole subtree was read. Check `depth_limited_directories`;
nonzero means directories were recorded by name and never opened.

### `observe_directory`

Enumerate exactly one already-authorized directory. Recurses into nothing
(§11.1, §20) — the host decides child by child whether to go deeper.

| Argument | Type |
|---|---|
| `source_id` / `source_path` | string, one required |
| `app_name` / `app_version` | string, optional |

```
chutni_call(s, "observe_directory", "{\"source_path\":\"/private/tmp/x/tree/Sub\"}", &result)
{"ok":true,"source_id":"019fb3b1-...",
 "scan":{"files_seen":1, "..." :"...", "directories_enumerated":1,
         "complete_for_policy":true},
 "counts":{"roots":1,"sources":4, "...":"..."}}
```

### `add_source`

Record or refresh a single file source under an authorized root (§20
`add_or_update_source`).

| Argument | Type | Notes |
|---|---|---|
| `root_id` | string, required | |
| `path` | string, required | |
| `hash_file` | bool | default true; false records metadata only |

```
chutni_call(s, "add_source",
  "{\"root_id\":\"019fb3b1-...\",\"path\":\"/private/tmp/x/tree/Extra/e.txt\"}", &result)
{"source_id":"019fb3b1-...","changed":true}
```

### `put_artifacts`

The generic host-ingestion primitive (§16, §20 `put_artifacts`), wrapping
`chutni_artifacts_put`. **Lower-level than `chutni-mcp`'s `chutni_put_artifacts`
tool**: each artifact here carries its own `source_id` and
`source_content_hash` directly, mirroring the typed `chutni_artifact` struct,
rather than resolving one shared source from a top-level path. The MCP tool
layers that single-source convenience — resolve by path, refresh, verify an
expected hash — on top of this primitive; a binding calling `chutni_call`
directly does that resolution itself first (`get_source` / `check_freshness`
above), then builds this call.

| Argument | Type | Notes |
|---|---|---|
| `operation` | string, required | |
| `producer` | object, required | matches `chutni_producer` fields |
| `recipe_hash` | string | optional |
| `parameters` | object | optional, defaults to `{}` |
| `inputs` | array | optional, defaults to `[]` |
| `artifacts` | array, required, 1–128 items | each: `source_id`, `source_content_hash`, `text`, `artifact_kind`, `artifact_origin` required; `media_type`, `selector`, `language`, `supersedes_artifact_id`, `metadata` optional |

```
chutni_call(s, "put_artifacts",
  "{\"operation\":\"summarize\",\"producer\":{\"producer_kind\":\"model\",\"name\":\"my-model\",\"model_id\":\"example/x\",\"app_name\":\"my-app\",\"app_version\":\"1\"},\"artifacts\":[{\"source_id\":\"019fb3b1-...\",\"source_content_hash\":\"blake3:b9f64d...\",\"text\":\"A note about marsupial pouches.\",\"artifact_kind\":\"summary_short\",\"artifact_origin\":\"model_generated\"}]}",
  &result)
{"ok":true,"producer_id":"019fb3b1-...","derivation_id":"019fb3b1-...",
 "semantic_validation":"not_performed",
 "artifacts":[{"artifact_id":"019fb3b1-...","artifact_kind":"summary_short"}]}
```

A batch where any artifact's `source_content_hash` no longer matches the
source's current bytes is refused (§13.3), same as the typed API:

```
{"error":{"code":"invalid argument",
 "message":"each artifact requires source_id, source_content_hash, text, artifact_kind, and artifact_origin"}}
```

### `put_memory`

Store reusable knowledge or work that was not derived from a file. This wraps
`chutni_memory_put` and creates one catalog-native source with
`source_kind: "memory"`, one `memory` artifact, and complete producer and
derivation records. The returned `memory_id` is the source ID, so existing
`search`, `source_context`, `check_freshness`, and `forget_source` operations
work without a parallel memory subsystem.

| Argument | Type | Notes |
|---|---|---|
| `memory_kind` | string, required | open type such as `note`, `decision`, `plan`, or `conversation_summary` |
| `title` | string | optional display title |
| `scope` | string | optional application, workspace, project, or conversation scope |
| `text` | string, required | non-empty reusable content |
| `language` | string | optional BCP 47 tag |
| `producer` | object, required | matches `chutni_producer`; model producers require `model_id`, `app_name`, and `app_version` |
| `operation` | string, required | how the memory was made |
| `recipe_hash` | string | optional |
| `parameters` | object | optional, defaults to `{}` |
| `inputs` | array | optional references to sources, artifacts, messages, or application records |

```c
chutni_call(s, "put_memory",
  "{\"memory_kind\":\"decision\",\"title\":\"Launch decision\","
  "\"scope\":\"my-app/conversation/42\","
  "\"text\":\"Ship after legal review.\","
  "\"producer\":{\"producer_kind\":\"model\",\"name\":\"my-model\","
  "\"model_id\":\"example/x\",\"app_name\":\"my-app\",\"app_version\":\"1\"},"
  "\"operation\":\"record_decision\","
  "\"inputs\":[{\"message_id\":\"message-42\"}]}",
  &result)
```

```json
{"ok":true,"memory_id":"019fb3b1-...","source_id":"019fb3b1-...",
 "artifact_id":"019fb3b1-...","producer_id":"019fb3b1-...",
 "derivation_id":"019fb3b1-...","memory_kind":"decision",
 "semantic_validation":"not_performed"}
```

Standalone memory has no external bytes to re-observe. Freshness re-hashes its
active memory artifact, so the content hash binds the source and artifact to
the text committed in the same transaction. It remains `"current"` until
explicitly forgotten or until a required artifact input becomes stale.

### `put_model_artifact`

Convenience wrapper for one model-generated artifact against one source
resolved by filesystem path — this is what `chutni-mcp`'s
`chutni_put_model_artifact` tool delegates to directly.

| Argument | Type | Notes |
|---|---|---|
| `source_path` | string, required | resolved via `chutni_source_find`; must already be indexed |
| `text` | string, required | |
| `model_id` / `model_revision` | string, required | |
| `app_name` / `app_version` | string, required | |
| `producer_name` | string | defaults to `model_id` |
| `weights_hash` / `quantization` / `runtime` | string | optional |
| `operation` | string | default `"generate_artifact"` |
| `recipe_hash` | string | optional |
| `parameters` | object | optional |
| `selector` | object | optional |
| `artifact_kind` | string | default `"summary_short"` |
| `supersedes_artifact_id` | string | optional |

The source must refresh to exactly `"current"` (§13.3) or the call is refused
— this operation has no earlier snapshot to compare an expected hash against,
so it requires currency outright rather than accepting a caller-supplied hash.

```
chutni_call(s, "put_model_artifact",
  "{\"source_path\":\"/private/tmp/x/tree/parb.md\",\"text\":\"...\",\"model_id\":\"example/x\",\"model_revision\":\"1\",\"app_name\":\"my-app\",\"app_version\":\"1\"}",
  &result)
{"ok":true,"source_id":"...","artifact_id":"...","producer_id":"...",
 "derivation_id":"...","semantic_validation":"not_performed"}
```

### `put_representation`

Store a vector against an artifact (§17), wrapping
`chutni_representation_put`. **New** — no MCP tool exposed this before T01.

| Argument | Type | Notes |
|---|---|---|
| `artifact_id` | string, required | |
| `profile` | object, required | `representation_kind`, `model_id`, `model_revision`, `dimensions` (defaults to the vector's length), `dtype`, `normalization`, `tokenizer_hash`, `projector_hash` |
| `vector` | array of numbers, required | |

```
chutni_call(s, "put_representation",
  "{\"artifact_id\":\"019fb3b1-...\",\"profile\":{\"representation_kind\":\"text_embedding\",\"model_id\":\"example/embed\",\"model_revision\":\"1\",\"dtype\":\"f32\",\"normalization\":\"none\"},\"vector\":[0.10,0.20,0.30,0.40]}",
  &result)
{"representation_id":"019fb3b1-...","artifact_id":"019fb3b1-...","dimensions":4}
```

### `mark_source_missing`

| Argument | Type |
|---|---|
| `source_id` / `source_path` | string, one required |

```
chutni_call(s, "mark_source_missing", "{\"source_path\":\"/private/tmp/x/tree/Extra/e.txt\"}", &result)
{"source_id":"019fb3b1-...","state":"missing"}
```

### `forget_source`

§24.3.

| Argument | Type | Notes |
|---|---|---|
| `source_id` / `source_path` | string, one required | |
| `mode` | string | `catalog_only` (default), `artifacts`, `secure_logical_delete`, or `purge` |

```
chutni_call(s, "forget_source", "{\"source_path\":\"/private/tmp/x/tree/Extra/e.txt\"}", &result)
{"source_id":"019fb3b1-...","forgotten":true}
```

None of these modes is guaranteed forensic erasure — see §24.3 and
`chutni forget --help`.

### `rebuild_indexes`

No arguments. Rebuilds everything under `indexes/` (§8.4).

```
chutni_call(s, "rebuild_indexes", "{}", &result)
{"ok":true}
```

## Operations without a `chutni_call` equivalent

`chutni pack`/`unpack` and root remapping (§7.2, §26) are portability
operations on a store as a whole, not per-store data operations, and don't
exist yet at all (they're [T08](tickets/T08-portability-pack-remap.md)).
`chutni_folder_status` / `chutni_folder_activate` — the pre-store lifecycle
of classifying a user-selected path and creating or opening the adjacent
store — stay `chutni-mcp`-specific (`src/mcp.c`), because they operate
*before* a store handle exists, which `chutni_call`'s signature assumes.
