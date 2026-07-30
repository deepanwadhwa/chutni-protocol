# Adoption track — tickets

**Goal (owner, 2026-07-30):** Chutni should be as useful as possible and
extremely easy to adopt. A user plugs Chutni into the LLM application of their
choice and stops paying for the same extraction, reading, and indexing work
twice — fewer tokens, less compute, faster answers.

Everything here is ranked against that goal. The strategic reading behind the
ranking: **adoption comes from code, not from the specification.** Nobody
"adopted the SQLite file format" — they embedded the library until the format
was a fact. The spec matters, but the products are the bindings, the first five
minutes, and the agent integration.

Each ticket follows the repo's evidence rules: acceptance criteria are
testable, anything user-visible needs a transcript under `docs/evidence/`, and
a claim that was not measured is not made.

## Index

| ID | Title | Priority | Size | Depends on |
|---|---|---|---|---|
| [T01](T01-json-tool-surface.md) | JSON-in/JSON-out tool surface in libchutni | P0 | M | — |
| [T02](T02-python-binding.md) | Python binding (stdlib-only ctypes) | P0 | M | T01 |
| [T03](T03-clean-room-reader.md) | Clean-room Reader from the spec alone | P0 | M | T06 |
| [T04](T04-enrichment-first-run.md) | First-run value: PDF text + embeddings | P0 | M | T01, T02 |
| [T05](T05-agent-integration.md) | Agent beachhead: Claude Code and MCP clients | P0 | M | — |
| [T06](T06-implementers-quickstart.md) | Implementer's quickstart (Reader in an afternoon) | P0 | S | — |
| [T07](T07-efficiency-evidence.md) | Measure the token-savings claim | P1 | M | T05 |
| [T08](T08-portability-pack-remap.md) | Make "portable" true: pack, unpack, remap | P1 | L | — |
| [T09](T09-fast-verify.md) | Fast verify at scale | P1 | M | — |
| [T10](T10-writer-coordination.md) | Multi-app writer coordination | P1 | S | — |
| [T11](T11-typescript-binding.md) | TypeScript/Node binding | P2 | M | T01, T15 |
| [T12](T12-producer-trust.md) | Producer trust and store pollution | P2 | M | — |
| [T13](T13-spec-tiering.md) | Spec tiering: normative core vs guidance | P2 | S | — |
| [T14](T14-windows.md) | Windows port | P2 | L | — |
| [T15](T15-publication.md) | Publish the repository (owner-gated) | P0* | S | owner |

*T15 is P0 in importance but blocked on the owner's explicit go — the repo
rules forbid creating or pushing a public repo without being asked.

**Priorities.** P0 unblocks adoption. P1 makes existing claims true
("portable", "efficient", "multi-app"). P2 is credibility and scale.

**Sizes** are relative effort (S < M < L), not calendar estimates.

## Dependency graph

```
T06 quickstart ──► T03 clean-room reader ──► (spec fixes)
T01 JSON surface ──► T02 Python ──► T04 enrichment
                └──► T11 TypeScript (after T15 publish)
T05 agent integration ──► T07 efficiency evidence
T08, T09, T10, T12, T13, T14 — independent
```

## Relationship to docs/TASKS.md phases

These tickets detail and extend the phase list; they do not replace it.

| Ticket | Overlapping phase items |
|---|---|
| T02, T11 | A4 (bindings) |
| T08 | P1, P2, P3, P4 (packing, remapping) |
| T09 | Open question "freshness costs a re-hash" (§13.2) |
| T10 | Concurrency stress testing (§2 gaps) |
| T14 | W1, W2, W3 |
| T05 | Open question "skills/ needs a real test" |

When a ticket lands, mark the phase item done and link the evidence, same as
always.
