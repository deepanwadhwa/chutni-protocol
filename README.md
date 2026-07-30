# Chutni

**Keep useful knowledge and LLM work. Let another application reuse it.**

Chutni is an open protocol and portable C implementation for AI-readable
memory. It retains both work derived from a user's local files and standalone
notes, decisions, plans, conversation summaries, analyses, and other useful
application/model outputs.

A Chutni store records content, **who or what produced it**, what inputs were
used, and whether it is still current. It is deliberately not tied to one
application or model: a store written by one program can be read, searched,
and improved by another.

- **Specification:** [SPEC.md](SPEC.md) (version 0.2-draft; v0.1 stores remain readable)
- **This implementation:** `0.2.0` — see [VERSION](VERSION). The release version
  and the protocol version move independently; `chutni version` prints both.
- **License:** Apache-2.0. Royalty-free to implement.

## Why this exists

Every AI application that reads your files rebuilds the same work: walk the
directory, extract the text, OCR the scans, caption the images, chunk, embed,
index. Then you switch applications and all of it is thrown away, because that
work lived inside one program's private cache.

That work is expensive — on a laptop, reading a single 5,000-token document can
cost minutes. It is also *yours*. Chutni makes it a durable artifact you own,
in a documented format, that outlives the application that built it.

## Status

Honest summary of what runs today, on one machine (macOS 15 / arm64, Apple M3).

| Area | State |
|---|---|
| Store layout, manifest, catalog schema (§8–§10) | built, tested |
| BLAKE3 content addressing (§13, §14) | built, verified against the official vectors |
| Sources, artifacts, producers, derivations (§12, §15, §16) | built, tested |
| Freshness and staleness (§13.2–§13.3) | built, tested; search uses a cheap stat downgrade |
| Supersession and multi-producer artifacts (§23) | built, tested |
| Generic host artifact batches and grouped source context | built, tested |
| Standalone application/model memory (notes, decisions, plans, summaries) | built, tested |
| Single-writer/many-reader application coordination | built, tested across processes |
| Lexical search, FTS5 (§19) | built, tested |
| Store discovery and registry (§39) | built, tested |
| Forget modes (§24.3) | built, lightly tested |
| Representations — f32 embeddings (§17) | built, tested; producer-supplied vectors |
| Root remapping across machines (§26) | **not built** |
| `.chutnipack` transfer bundles (§7.2) | **not built** |
| Host-produced PDF/OCR/image/spreadsheet/audio artifact interchange | built, tested; extraction remains the host's job |
| Reference scanner PDF/OCR/image/spreadsheet/audio extraction | intentionally not provided; hosts submit those outputs |
| Semantic search (§19.1) | built in the C API as brute-force cosine; hybrid search is not built |
| Application Host lifecycle (§30.5, §40) | specified, documented, handoff scenario tested |
| Reusable local service (`chutni-mcp`) | built; MCP stdio and native one-shot modes tested |
| Directories as sources, `parent_source_id` hierarchy (§12.5) | built, tested |
| Depth-bounded scans, enforced in the library (§11.1) | built, tested |
| Coverage manifests per scan generation (§15.7) | built, tested |
| Directory definitions with required local coverage (§15.6) | built; refused at write without a stop reason |
| Directory freshness by listing re-enumeration (§13.5) | built, tested |
| One-directory observation without recursion (§20) | built, tested |
| `exclude_globs` enforcement (§11) | **not built**; exclusion is a fixed name list |

`make test` reports the unbuilt conformance scenarios as GAP rather than
counting them as passes. Verified only on macOS 15 / arm64 (Apple M3) — the
only machine this has run on. See
[docs/evidence/](docs/evidence/2026-07-30-v0.2-hierarchical-coverage/report.md)
for what that does and does not cover, [docs/TASKS.md](docs/TASKS.md) for the
current completion state, and
[docs/SAMOSA-COMPATIBILITY.md](docs/SAMOSA-COMPATIBILITY.md) for the pinned
application boundary.

## Build

No package manager and no dependencies to install. SQLite and BLAKE3 are
vendored and compiled from source.

```sh
make           # builds build/chutni, build/chutni-mcp, and build/libchutni.a
make test      # vectors, conformance, CLI, and service lifecycle checks
make sanitize  # both suites under ASan + UBSan
make install   # PREFIX=/usr/local by default
```

Requires a C99 compiler and `python3` for the BLAKE3 vector runner.

## Use

```sh
# Does memory already exist on this computer?
chutni discover

# If not, make some.
chutni init ~/Memory.chutni --label "My files"
chutni add-root ~/Documents
chutni scan

# Then use it.
chutni search "condensation force"
chutni inspect ~/Documents/paper.md      # what was derived, and by what
chutni verify                            # re-observe sources, retire stale artifacts
```

Every command takes `--json`, because the main consumers are agents.

### Reading a folder without reading all of it

Pointing an application at a folder is not the same as asking it to open
everything underneath. A root can carry a depth bound, and the library enforces
it (§11.1) — not the caller, and not a model.

```sh
chutni add-root ~/Downloads --max-depth 1 --goal define
chutni scan
```

```text
Observed 17 directories and 24 files
  directories enumerated  8  (deepest depth 1)
  recorded but not opened 9  (past max_depth)
  ...
This is complete for the policy requested, not a complete reading of the subtree.
```

Directories past the bound are recorded by name and never opened, so a consumer
can tell "nothing is in here" from "we never looked in here":

```sh
chutni children ~/Downloads/SomeApp.app
# directory opaque  ~/Downloads/SomeApp.app/Contents

chutni observe ~/Downloads/SomeApp.app/Contents   # exactly one directory, no recursion
chutni coverage                                    # what the last scan reached
```

`chutni coverage` reads the coverage manifest, which every scan writes. Any
application can read it, including one that did not perform the scan — which is
the point. A bounded scan that a second application mistakes for an exhaustive
index is worse than no index.

## Use from an application

### Python

After `make`, a stdlib-only binding is available from `python/` (or install it
with `pip install -e python/`). Point `CHUTNI_LIBRARY` at the shared library
when it is not installed under `/usr/local/lib`:

```python
from chutni import Store
with Store.create("~/Memory.chutni", label="My files") as s:
    s.add_root("~/Documents", max_depth=1, memory_goal="define")
    s.scan()
    for hit in s.search("condensation force", limit=5):
        print(hit["display_path"], hit["freshness"], hit["snippet"])
```

Results remain plain dictionaries, including coverage and freshness fields.
Stores are synchronous and must not be shared between threads without a lock.

Applications do not need to reimplement the catalog or teach the language model
SQL. They can bundle and launch the same `chutni-mcp` executable:

```text
Samosa / another native app ── one-shot calls ──┐
ChatGPT / Claude / an MCP host ── MCP stdio ────┼─ chutni-mcp ─ libchutni ─ P.chutni
an app linking the C library ───────────────────┘
```

For MCP hosts, configure `build/chutni-mcp` as a local stdio server. It
advertises tools covering capability discovery, folder status and
activation, store discovery and inspection, scanning, searching, grouped
source context, standalone memory, generic provenance-complete artifact
batches, and compatibility helpers for individual parser/model artifacts.
Search defaults to precise all-term matching; hosts deriving
queries from prose can request literal any-term matching without exposing FTS
operator syntax.

Native applications that already supervise child processes can call the exact
same implementation without embedding an MCP client:

```sh
build/chutni-mcp --call chutni_folder_status \
  '{"path":"/Users/me/Research"}'

# Only after the app shows the proposed Research.chutni path and the user
# approves creating/opening it and scanning Research:
build/chutni-mcp --call chutni_folder_activate \
  '{"path":"/Users/me/Research","confirmed":true,"register":true,
    "label":"Research","app_name":"example-app","app_version":"1.0"}'
```

Both modes return structured JSON and operate on the same protocol-defined
store. `stdout` is reserved for protocol output; diagnostics use `stderr`.
Applications normally ship this executable with their own installer, so their
end users do not need a separate Chutni installation.

The reference scan creates a `file_metadata` artifact for every file and
extracts text-like UTF-8 files. Applications with PDF, OCR, vision,
spreadsheet, or speech capabilities submit their outputs through
`chutni_put_artifacts`. Each artifact records the processing operation,
producer/model/application, selector, exact source hash, and creation time.
`chutni_source_context` returns all current interpretations together. Chutni
validates those records and their source binding; it deliberately does not
decide whether an interpretation is true.

Knowledge or work that does not come from a file — a conversation summary,
decision, plan, or note — goes through `chutni_put_memory`. It is searchable
through the same `chutni_search` operation and carries the same producer and
derivation provenance, without being represented as a fake file.

## Implementing the format directly

Most applications should embed `libchutni` or launch `chutni-mcp` rather than
write a parser. If you're implementing against the specification instead —
a different language, a constrained environment, or you just want to read a
store without a dependency — start with
**[docs/IMPLEMENTERS.md](docs/IMPLEMENTERS.md)**: the Reader contract (§30.1)
in about fifteen rows, the three SQL queries a Reader actually needs, and the
freshness rule worked out byte-for-byte against a real store, not summarized
from memory.

## For AI applications

The application adopts Chutni—by linking the library, invoking the reusable
service, or implementing the specification. The language model does not write
the store itself. When a user selects an ordinary folder `P`, a conforming host
checks for the adjacent `P.chutni` store. It opens and reuses that store when
present, or offers to create it, add `P` as an authorized root, and scan it when
absent.

The complete host lifecycle is normative in [SPEC §40](SPEC.md), with a
practical implementation guide in
[docs/ADOPTING_CHUTNI.md](docs/ADOPTING_CHUTNI.md).

An application should also **search before it creates**. If a store already
exists elsewhere, use it rather than building a second copy of the same memory:

```sh
chutni discover --json
```

```json
{
  "spec_version": "0.2",
  "count": 1,
  "stores": [
    {
      "path": "/Users/deepan/Memory.chutni",
      "store_id": "0195f0c4-83f8-7d4f-a2dc-c91d9201287a",
      "spec_version": "0.2",
      "label": "My files",
      "readable": true
    }
  ]
}
```

Discovery order is `$CHUTNI_STORE`, then the registry at
`~/.chutni/registry.json`, then `*.chutni` directories in the home and
documents directories. It never scans the whole filesystem. See [§39](SPEC.md).

Ready-made agent instructions live in [skills/](skills/).

## Four rules worth reading before you build on this

**Memory is a map, not the source of truth.** An artifact tells you *where* to
look and roughly what is there. For anything exact, reopen the file. If you
answer from a summary without reopening the source, say so. (§6.1)

**Provenance is recorded; correctness is not.** Chutni tells you that a caption
came from `example/Bonsai-1B` revision `8f12c4d` via a particular recipe. It
does not tell you the caption is right, and there is deliberately no
model-confidence field to imply otherwise. (§6.2)

**Indexed files are untrusted input.** A document can contain text designed to
look like instructions to whatever model reads it. Retrieval carries no
privilege: Chutni hands back bytes and a path, never a command. Consumers must
keep it that way. (§6.5, §28)

**Not looking is not the same as finding nothing.** A store records how far each
scan reached. An empty result inside a region that was never opened means
nothing at all, and `complete_for_policy` means the requested bounded operation
finished — never that the subtree was read. If you have no coverage manifest,
the honest word is "unknown", not "complete". (§15.7, §24.4, §35.1)

## Contributing

The protocol is meant to be governed independently of any single application,
and the reference implementation gives no application privileged extensions
(§36). Specification changes belong in `SPEC.md` with a rationale; behavior
changes need a conformance test that fails before and passes after.

## Credits

Chutni vendors two pieces of other people's work, both compiled from source and
unmodified:

- **BLAKE3** by the BLAKE3 team — `third_party/blake3/`, dual CC0-1.0 /
  Apache-2.0.
- **SQLite** by D. Richard Hipp and contributors — `third_party/sqlite/`, public
  domain.
