# Chutni — agent guide

Open protocol for portable, provenance-bearing file and standalone application
memory, plus a reference implementation in portable C99. No package manager,
no dependencies to install, no build system beyond `make`.

## Start here

1. **[SPEC.md](SPEC.md)** is the protocol, version 0.2-draft. It is the source
   of truth. Section numbers (§8, §13.3, §39) are referenced throughout the
   code and this document — use them. v0.2 added hierarchical sources,
   bounded coverage, and standalone memory (§11.1, §12.5–§12.6, §13.5,
   §15.5–§15.7, §24.4, §35.1); v0.1 stores stay readable and are not
   rewritten on open.
2. **[docs/TASKS.md](docs/TASKS.md)** is what is built, what is not, and what
   to work on next.
3. **[docs/evidence/](docs/evidence/)** holds test transcripts. Claims about
   what works come from there.

This repository is **not** Samosa. It was deliberately split out on 2026-07-29
so the protocol can be governed independently and implemented by applications
unrelated to Samosa (§36). Do not add Samosa-specific behavior, and do not give
any application privileged extensions.

## Build, test, run

```sh
make            # build/chutni and build/libchutni.a
make test       # BLAKE3 official vectors + conformance suite + CLI checks
make sanitize   # both suites under ASan + UBSan
make install    # PREFIX=/usr/local
```

`make test` must be green before anything is called done. It takes ~2 seconds.

**Two versions, deliberately separate.** `VERSION` (currently `0.3.0`) is this
implementation's release; `CHUTNI_SPEC_VERSION` in `include/chutni.h` (currently
`0.2`) is the protocol a store is written in. They move independently — a bug
fix bumps one and not the other. `VERSION` is the single source: the Makefile
reads it and compiles it in as `CHUTNI_VERSION`, which becomes the library, CLI,
service, and reference-scanner version. Do not reintroduce a version literal in
a source file; the scanner's version lands in producer records (§16.1), so a
stale copy misattributes artifacts to a build that never made them.

```sh
chutni discover                      # does memory exist on this computer?
chutni init ~/Memory.chutni
chutni add-root ~/Documents --max-depth 1 --goal define
chutni scan
chutni search "condensation force"
chutni inspect <path-or-source-id>   # what was derived, and by what
chutni children <dir-path-or-id>     # immediate entries; opaque means never opened
chutni observe <dir-path-or-id>      # enumerate exactly one directory
chutni coverage                      # what the last scan reached, and what it did not
chutni verify                        # re-observe sources, retire stale artifacts
```

Every command accepts `--json`; agents are the primary consumers.

## Layout

| Path | What |
|---|---|
| `VERSION` | the implementation's release version, and the only place it is written |
| `SPEC.md` | the protocol |
| `docs/IMPLEMENTERS.md` | the Reader contract in ~15 rows, for anyone implementing against the format rather than using this CLI |
| `include/chutni.h` | the stable C ABI — the contract other apps bind to |
| `src/chutni.c` | store, catalog, objects, sources, artifacts, search, discovery |
| `src/cj.{c,h}` | JSON document model (a DOM, not an extractor — see below) |
| `src/cli.c` | the `chutni` command |
| `tests/conformance/` | §31 scenarios, C API level and CLI level |
| `docs/tickets/` | archived T01–T15 design history; not the active queue |
| `skills/` | drop-in instructions teaching an agent to use a store |
| `third_party/` | BLAKE3 and SQLite, vendored, compiled from source, unmodified |

`src/cj.c` is a full document model rather than a value extractor on purpose:
§9.1 requires that unknown manifest fields survive a rewrite, which means
parsing the whole tree and serializing it back including keys this
implementation has never heard of. Do not replace it with a lighter parser.

## Non-negotiables

Inherited from the owner's standards and they override convenience.

- **Evidence, not assertion.** If you did not run it, say "not run". Paste the
  command and its output. Transcripts go under `docs/evidence/<slug>/`.
- **Builds ≠ tests pass ≠ works.** `make test` passing is not the same as
  working on a user's real folder. Say which you did.
- **Never overstate.** Scope every claim to what was measured, and name the
  machine: OS, arch, compiler. "It's fast" is not a sentence.
- **Gaps are reported, never hidden.** The conformance suite prints `GAP` with
  a reason for unimplemented scenarios and never counts them as passes. Keep
  that. A suite that hides its coverage holes is worse than no suite.
- **First-party code compiles under `-Werror`.** Vendored code is compiled as
  its authors shipped it; do not patch upstream to satisfy our flags.
- **Outward publishing waits for explicit confirmation.** The repo is public at
  `github.com/deepanwadhwa/chutni-protocol` and `main` is pushed there. That is
  a reason for more care, not less: anything committed is one owner push away
  from being public. Do not push, tag, or open issues/PRs without being asked.
- Agent shells have no push credentials. Commit and stop.

## The rule the whole format exists to enforce

**Never trust cached state over the bytes on disk.** Both defects found while
building v0.1 were this same mistake, and the second was introduced while
fixing the first:

- Freshness compared the artifact's recorded source hash against the catalog's
  stored source hash. Both are catalog state, so when a file changed and
  nothing had rescanned it, the two agreed with each other and a stale artifact
  reported itself `current` while describing content that no longer existed —
  exactly the state §13.3 forbids. `chutni_check_freshness` now re-hashes the
  file, because the file is the only authority.
- Then `scan` broke the other way. Once verification recorded the new hash,
  scan compared new-to-new, concluded nothing had changed, and never
  re-extracted, so the new content was never indexed. "Changed" now means *no
  active artifact describes these exact bytes* — the question a scanner is
  actually asking — not "the hash differs from the last one recorded".

Both are covered by regression tests (conformance scenario 4, and the CLI check
"rescan picks up new content"). If you touch freshness, staleness, or scan,
re-read `chutni_source_put`, `chutni_source_refresh`, and
`chutni_check_freshness` together. They are one mechanism in three places.

v0.2 extended that mechanism rather than duplicating it, and the duplication is
what to watch for:

- `observe_source` is the single place that re-derives a source's current
  observation from disk — a file's bytes, or a directory's listing. Nothing
  else should be reading `sources.content_hash` and calling it the truth.
- `stale_artifacts_not_matching` is the single place that demotes artifacts
  bound to a superseded observation. It was inlined twice before v0.2; a third
  copy for directories is exactly how the two defects above happened.
- `cascade_stale_dependents` propagates §13.3's second clause: an artifact whose
  required derivation input is no longer active is describing that input's old
  content. It runs to a fixpoint, because one pass per verification would leave
  a chain of derived artifacts half-withdrawn.
- `chutni_read_directory` is the single place that enumerates, canonicalizes,
  and hashes a listing. The scanner records one and freshness re-derives it
  later; if those ever disagreed about which entries a policy admits or how they
  serialize, every directory in every store would read as permanently stale.

## The second rule v0.2 adds

**Not looking is not the same as finding nothing.** A bounded scan knows only
about the region it opened. §24.4 makes this normative: reconciliation runs per
enumerated directory and touches only that directory's own children, so a
depth-0 refresh can notice a deleted direct child and cannot say anything about
grandchildren. A directory that was named but never opened is recorded
`opaque`, and `complete_for_policy` means the requested bounded operation
finished — never that the subtree was read.

If you add a code path that marks sources missing, prunes, or reports totals,
ask which region it actually observed. Conformance scenario 16 fails when this
is got wrong in either direction.

## Relationship to Samosa

Samosa (`~/Documents/samosa-chat`) has a sidecar also called Chutni. **It is a
different, earlier, incompatible design** and predates this protocol:

| | Samosa's sidecar | This protocol |
|---|---|---|
| Hash | SHA-256 | BLAKE3 (§9.1 requires it) |
| Layout | `~/.samosa/chutni/scopes/<id>/` | `<Name>.chutni/` per §8 |
| Tables | `files`, `contents`, `chunks`, `memory_cards`, … | `roots`, `sources`, `objects`, `producers`, `derivations`, `artifacts`, … |
| Provenance | fingerprint columns | per-artifact `producers` + `derivations` (§6.4) |
| Discovery | none | §39 |
| Hierarchy | path strings on file rows | directory sources + `parent_source_id` (§12.5) |
| Coverage | not represented | bounded depth + coverage manifests (§11.1, §15.7) |

**Neither can read the other. Samosa is not Chutni-compatible**, and must not
be described as such until conversion work exists and has been run on a real
store. None is started. The owner's intent (2026-07-29) is that Samosa
eventually becomes both a reader and a writer of this format — see
[docs/TASKS.md](docs/TASKS.md) Phase S.

## Two amendments to the owner's original draft

Both are marked in `SPEC.md`; do not silently revert them.

- **§39 Store discovery** was added. The draft defined what a store *is* but
  never where one lives or how an application finds one, so "does Chutni memory
  exist on this computer?" had no answer and every application would have built
  its own store. §39 defines the resolution order, the registry, and the rule
  that a registry entry is a **hint, not a grant** — discovery says memory
  exists; §27 still governs whether you may read it.
- **§34** now specifies a C99 core rather than a Rust one, so C applications
  (including several local inference engines) can link it without an FFI
  boundary or a second toolchain. Bindings sit *on top of* the C ABI.
