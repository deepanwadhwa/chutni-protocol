# Chutni

**Prepare a user's files for AI once. Keep the result. Let any application reuse it.**

Chutni is an open protocol for AI-readable memory derived from a user's local
files, plus a reference implementation in portable C.

A Chutni store records where files are, what they contain, **who or what
produced each description**, and whether that description is still valid for the
file's current bytes. It is deliberately not tied to one application, one model,
or one operating system: a store built by one program can be read, searched, and
improved by another.

- **Specification:** [SPEC.md](SPEC.md) (version 0.1-draft)
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
| Lexical search, FTS5 (§19) | built, tested |
| Store discovery and registry (§39) | built, tested |
| Forget modes (§24.3) | built, lightly tested |
| Representations — f32 embeddings (§17) | built, tested; producer-supplied vectors |
| Root remapping across machines (§26) | **not built** |
| `.chutnipack` transfer bundles (§7.2) | **not built** |
| Image, audio, spreadsheet ingestion (§25.2–§25.4) | **not built** |
| Semantic search (§19.1) | built in the C API as brute-force cosine; hybrid search is not built |
| Application Host lifecycle (§30.5, §40) | specified, documented, handoff scenario tested |

`make test` reports the unbuilt conformance scenarios as GAP rather than
counting them as passes. Verified only on macOS 15 / arm64 (Apple M3) — the
only machine this has run on. See
[docs/evidence/](docs/evidence/2026-07-29-v0.1/report.md) for what that does and
does not cover, and [docs/TASKS.md](docs/TASKS.md) for the work list.

## Build

No package manager and no dependencies to install. SQLite and BLAKE3 are
vendored and compiled from source.

```sh
make           # builds build/chutni and build/libchutni.a
make test      # BLAKE3 vectors, conformance suite, CLI checks
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
chutni verify                            # re-hash sources, retire stale artifacts
```

Every command takes `--json`, because the main consumers are agents.

## For AI applications

The application implements Chutni; the language model does not write the store
itself. When a user selects an ordinary folder `P`, a conforming host checks for
the adjacent `P.chutni` store. It opens and reuses that store when present, or
offers to create it, add `P` as an authorized root, and scan it when absent.

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
  "spec_version": "0.1",
  "count": 1,
  "stores": [
    {
      "path": "/Users/deepan/Memory.chutni",
      "store_id": "0195f0c4-83f8-7d4f-a2dc-c91d9201287a",
      "spec_version": "0.1",
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

## Three rules worth reading before you build on this

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
