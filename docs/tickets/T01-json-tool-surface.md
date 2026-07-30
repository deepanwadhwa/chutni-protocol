# T01 — JSON-in/JSON-out tool surface in libchutni

**Priority:** P0 · **Size:** M · **Depends on:** nothing · **Spec:** §20
**Status:** done 2026-07-30 — see
[`docs/API-JSON.md`](../API-JSON.md) and
[evidence](../evidence/2026-07-30-json-call-surface/report.md).

## Why

Every language binding written against the current C ABI must mirror C structs
(`chutni_search_request`, `chutni_artifact_info`, …) field-for-field. That is
the expensive, fragile part of FFI: struct layouts must match exactly, they
changed size in v0.2, and every new field means every binding updates or
silently corrupts memory. This is the single biggest cost multiplier on T02
(Python), T11 (TypeScript), and every binding anyone else ever writes.

Meanwhile the repo already contains **three** hand-built JSON serializations of
the same data: `src/cli.c` builds JSON for `--json` output, `src/mcp.c` builds
JSON for tool results, and `chutni_get_coverage` builds JSON in the library.
Three copies drift; §19.3's new fields had to be added to each separately.

One change fixes both problems: give libchutni a single JSON-in/JSON-out entry
point, and make the CLI and MCP server consumers of it.

## Deliverables

1. **`chutni_call`** in the stable ABI:

   ```c
   /* Execute a named operation with JSON arguments, returning JSON.
    * Operation names and argument shapes match the chutni-mcp tool surface
    * exactly (chutni_search, chutni_children, chutni_observe_directory,
    * chutni_coverage, chutni_put_artifacts, ...), minus transport concerns.
    * *result_json is caller-owned; free with chutni_free. Errors return the
    * failing status AND a {"error":{"code","message"}} envelope so bindings
    * need only one code path. */
   chutni_status chutni_call(chutni_store *store, const char *operation,
                             const char *arguments_json, char **result_json);
   ```

   A binding in any language then needs exactly four foreign functions:
   `chutni_open`, `chutni_call`, `chutni_free`, `chutni_close` — strings in,
   strings out, no struct mirroring at all.

2. **Operation coverage.** Every §20 minimum-access operation, including the
   v0.2 additions, plus `chutni_representation_put` / semantic search — the
   MCP surface currently has **no representation tool**, and T04 (embeddings)
   needs one. Vectors travel as JSON float arrays; at ≤4096 dims this is fine,
   and the CHUTVEC1 object encoding underneath is unchanged.

3. **`src/mcp.c` reimplemented on top of `chutni_call`.** The MCP server
   becomes transport + confirmation-gating + tool schemas; result construction
   lives in the library once. This is the proof of parity: if the MCP tests
   still pass, the surface is complete.

4. **Result-shape documentation** in a new `docs/API-JSON.md`: every operation,
   arguments, result shape, error envelope. This document is what T02/T11 bind
   against and what the MCP tool schemas cite.

## Design notes

- **Do not** route permission decisions through the library. `confirmed` flags
  and §27 gating stay in the host/MCP layer; `chutni_call` executes what it is
  told against an already-open handle with the handle's read/write mode.
  A read-only handle refuses mutating operations with `CHUTNI_ERR_READONLY`,
  same as today.
- Keep the existing typed C API intact and primary — it is the ABI other C
  applications already compile against. `chutni_call` is a layer above it, in
  the same library.
- Error envelope is part of the contract: stable `code` strings (reuse
  `chutni_strerror` names), human `message` from `chutni_last_error`.
- `cli.c` migration to `chutni_call` is optional and can be a follow-up; do it
  only where it deletes code without changing output the tests pin.

## Acceptance criteria

- `chutni_call` handles every operation in `docs/API-JSON.md`; unknown
  operation returns the error envelope, not a crash.
- `src/mcp.c` result construction is gone; MCP suite passes unchanged
  (90 checks at time of writing).
- A new conformance section drives search, children, coverage, put_artifacts,
  and representation round-trip **through `chutni_call` only**, comparing
  against the typed API's answers.
- `make test` and `make sanitize` green.

## Evidence required

Transcript under `docs/evidence/` showing a shell session driving a store
end-to-end through `chutni_call` (via a tiny test harness), plus the suite
output.

## Non-goals

- No wire protocol, no sockets, no versioned RPC — this is an in-process call.
- No breaking changes to existing typed functions.

## Open questions

- Should `chutni_call` accept a NULL store for store-less operations
  (`discover`, `capabilities`)? Leaning yes — bindings want those before they
  have a handle.
