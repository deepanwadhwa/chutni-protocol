# T11 — TypeScript/Node binding

**Priority:** P2 · **Size:** M · **Depends on:** T01, T15 · **Spec:** §34, TASKS A4
**Status:** proposed 2026-07-30

## Why

After Python, the second-largest population of LLM-app builders writes
TypeScript (Electron desktop apps, VS Code extensions, Node MCP servers).
Thanks to T01, a binding is small: four foreign functions and JSON. This ticket
is deliberately **after** T02 and T15 — Python proves the JSON-surface design
and publication decides distribution (npm name, prebuilds); doing TS first
would mean designing distribution twice.

## Deliverables

1. **`bindings/typescript/`** — an N-API addon, not `ffi-napi`:
   - `ffi-napi` is unmaintained and breaks across Node majors; a hand-written
     N-API shim over exactly `chutni_open` / `chutni_open_ex` / `chutni_call`
     / `chutni_free` / `chutni_close` is ~200 lines of C and compiles with
     `node-gyp` against the installed `libchutni` + `chutni.h`;
   - TypeScript wrapper mirroring the Python surface: `Store.create/open`,
     `store.call(op, args)` low-level, typed convenience methods
     (`search`, `scan`, `children`, `coverage`, `putArtifacts`,
     `putRepresentation`, `observeDirectory`) generated from
     `docs/API-JSON.md` shapes;
   - `ChutniError` carrying the T01 envelope `code`; `BUSY` distinguishable
     for retry logic (pairs with T10's `chutni_open_ex`).
2. **Types as the contract**: `.d.ts` result types written from
   `docs/API-JSON.md`, with the honesty fields (`freshness`, `observation`,
   `coverage_manifest_id`, `depth`, `complete_for_policy`) required, not
   optional — a TS consumer should get a type error, not a silent undefined,
   when it forgets coverage exists.
3. **Async posture**: the C library is synchronous and per-handle
   single-threaded. v1 exposes sync calls plus an `AsyncStore` running calls
   on a `Worker` thread with its **own handle** (respecting the
   one-handle-per-thread rule) — no shared-handle async, ever.
4. **Example**: a ≤30-line Node MCP-less demo (create, bounded scan, search,
   print freshness + coverage) in the package README, actually run.
5. **Tests**: `make ts-test` (skipped with a notice when node/node-gyp are
   absent — the core build must never require Node).

## Acceptance criteria

- `npm install && npm test` green in `bindings/typescript/` on the reference
  machine against an installed libchutni.
- The example runs as written; a store written from TS is read back correctly
  by the CLI and by the Python binding (three-implementation agreement check —
  the first real §30 interop test between bindings).
- Node LTS current and previous both pass (state versions in evidence).
- Browser/WASM explicitly not attempted (flock, real filesystem semantics —
  recorded as out of scope, not as future-promise).

## Evidence required

Build + test + example transcript, Node versions named, under
`docs/evidence/`.

## Non-goals

- No npm publish before T15 settles naming and prebuilt-binary policy.
- No Electron-specific packaging; consumers own their rebuild story.
- No WASM.
