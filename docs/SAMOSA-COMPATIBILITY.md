# Samosa compatibility contract

This is the boundary between Chutni and Samosa. Chutni owns durable memory,
provenance, freshness, and search. Samosa owns the user interface, folder
authorization, extraction/model work, and deciding what context reaches a
model.

The contract is deliberately small. Samosa launches `chutni-mcp --call` and
uses these operations:

| Operation | Samosa relies on |
|---|---|
| `chutni_folder_status` | `action`, canonical adjacent `store_path` |
| `chutni_folder_activate` | confirmation gating, `store_path`, `scan`, `counts` |
| `chutni_store_info` | file/readability counters listed below |
| `chutni_list_sources` | file-only inventory under the exact authorized root |
| `chutni_scan` | scan progress and final counts |
| `chutni_search` | source-labelled snippets and `freshness: "current"` |
| `chutni_put_derived_artifact` | parser/OCR output with source freshness and provenance |
| `chutni_put_model_artifact` | model output with source freshness and model/application identity |

The `counts` object must retain:

- `content_artifacts`
- `metadata_artifacts`
- `content_readable_sources`
- `metadata_only_sources`
- `sources_files`
- `artifacts_active`

Inventory entries must retain `source_id`, `display_path`, `media_type`,
`state`, and `size_bytes`. Search hits must retain `source_id`, `artifact_id`,
`display_path`, `snippet`, and `freshness`.

New fields and tools are additive. In particular, `chutni_put_memory` adds
standalone notes, decisions, plans, conversation summaries, and work products;
it does not change the file-only meaning of Samosa's inventory and readability
counters. Samosa can adopt that operation later without migrating existing
folder stores.

## Gate

Run:

```sh
make test-samosa-compat
```

The test builds a real adjacent store and exercises the seven distinct
service paths Samosa depends on (activation covers status, scan, and counts in
one path). It is also part of `make test`.

Before replacing Samosa's bundled binary, additionally run its own compiled
gateway tests against the candidate `build/chutni-mcp`:

```sh
make chutni-gateway-test
```

Those tests are the final consumer-side authority. The Chutni-side gate exists
to catch a breaking response-shape change before a binary is copied into
Samosa.

## Upgrade rule

1. Finish and merge Chutni with `make test` green.
2. Run `make test-samosa-compat`.
3. Substitute the candidate binary into Samosa only for its gateway tests.
4. If both Samosa tests pass, update Samosa's pinned source/binary in a separate
   Samosa branch.
5. Re-run Samosa's full relevant test suite before merging there.

Chutni must not write Samosa chat state, and Samosa must not grow another
private implementation of the Chutni catalog.
