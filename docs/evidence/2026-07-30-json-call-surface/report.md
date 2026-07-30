# T01 — JSON-in/JSON-out call surface

**Date:** 2026-07-30  
**Machine:** macOS 15, Darwin arm64, Apple clang 21.0.0. This is the only
machine used for this work.

T01 makes `libchutni` practical to bind from another language. The stable C
ABI now exposes `chutni_call(store, operation, arguments_json, result_json)`:
JSON object in, JSON object out. A binding needs only `chutni_open`,
`chutni_call`, `chutni_free`, and `chutni_close`; it does not mirror the
evolving C structs.

The API contract and every operation/result shape are documented in
[`docs/API-JSON.md`](../../API-JSON.md). `chutni-mcp` now delegates its
shared operations to the same surface, retaining only MCP transport, schemas,
path/store lifecycle, and confirmation gates. This removes a second result
serialization from the service without moving permission decisions into the
library.

## End-to-end transcript

`make test` runs `build/call_surface` against a fresh store and a redirected
`HOME`/`CHUTNI_HOME`, followed by the existing suites. Its result was:

```
79 conformance assertions passed, 0 failed, 1 declared GAP
34 JSON call-surface assertions passed, 0 failed
32 CLI checks passed, 0 failed
90 MCP checks passed, 0 failed
```

The JSON harness performs the whole lifecycle through `chutni_call`: capability
and store discovery; scan; lexical and semantic search; children and coverage;
source/artifact/object reads; artifact and representation writes; missing and
forget lifecycle; and a read-only write refusal. It cross-checks search,
children, coverage, artifact ingestion, and representation storage against the
typed API on the same store.

It also exercises the interface boundaries that a binding cannot safely guess:

```
chutni_call(scan, {not json})
  -> status=-5 {"error":{"code":"invalid argument",
                  "message":"arguments_json must be a valid JSON object"}}

chutni_call(store_info, [])
  -> status=-5 {"error":{"code":"invalid argument",
                  "message":"arguments_json must be a JSON object"}}

chutni_call(put_representation, ... "vector":["not-a-number"])
  -> status=-5 {"error":{"code":"invalid argument",
                  "message":"vector must contain numbers only"}}
```

These checks matter because a malformed argument string must never silently
become `{}` and trigger a default write such as a whole-store scan, and because
an embedding vector must not silently change a non-number into zero.

The complete sanitizer leg was rebuilt from source with AddressSanitizer and
UndefinedBehaviorSanitizer, then run with the same four test legs: **79
conformance assertions (one declared gap), 34 JSON-surface assertions, 32 CLI
checks, and 90 MCP checks; no sanitizer diagnostics**. The sandbox runner
interrupted the monolithic `make sanitize` recipe between compiler invocations,
so the same documented compiler commands were run individually before those
four commands; no objects from the normal build were mixed in.

## Scope not claimed

- This is an in-process FFI surface, not a network protocol or permission
  system. Hosts still enforce §27 confirmation and disclosure rules.
- It adds no language binding yet; T02 (Python) is the first consumer.
- The pre-store `chutni_folder_status` and `chutni_folder_activate` lifecycle
  remains in `chutni-mcp`, because it necessarily runs before a store handle
  exists.
- The pre-existing moved-root conformance gap (§26) remains unrelated and
  unimplemented.
