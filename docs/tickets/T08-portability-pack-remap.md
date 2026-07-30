# T08 — Make "portable" true: pack, unpack, remap

**Priority:** P1 · **Size:** L · **Depends on:** nothing · **Spec:** §7.2, §12.3, §26; TASKS P1–P4
**Status:** proposed 2026-07-30

## Why

"Portable" is in the project's first sentence, and today a store cannot
actually move: there is no transfer bundle (`chutni pack` doesn't exist) and a
copied store's roots point at paths that don't exist on the new machine with no
way to re-point them. Conformance scenario 3 (moved root) has been a declared
GAP since v0.1. Either portability becomes real or the tagline changes; this
ticket makes it real. It consolidates TASKS P1–P4 into one design because the
pieces interlock: pack without remap produces a store that arrives broken.

## Deliverables

1. **`chutni pack <store> <out.chutnipack>`** (§7.2):
   - ZIP64; entries in deterministic sorted order; `manifest.json` first so a
     receiver can identify/reject before extracting anything;
   - contents: manifest, catalog, `objects/**`, `extensions/**`; **excludes**
     `indexes/` (disposable by definition, §8.4) and `tmp/`;
   - verifies every object's hash against its name while packing — a pack that
     copies corruption forward is worse than an error;
   - refuses to pack a store that is open read-write elsewhere (take the
     writer lock for the duration).
2. **`chutni unpack <in.chutnipack> <dest>`**:
   - refuses an existing destination (§40.3's never-overwrite rule applies);
   - zip-slip defense: every entry path must resolve inside dest, no
     absolute paths, no `..` (§28.3 finally gets its first teeth);
   - re-verifies object hashes on extraction; rebuilds indexes at the end;
   - prints, unprompted, that roots likely need `chutni remap` — the arriving
     user should not have to discover that from a confusing `verify`.
3. **`chutni remap <old-prefix> <new-prefix>` (and `--root <id> <new-path>`)**
   (§26):
   - rewrites `roots.locator_json` and every affected source's
     `display_path`/`file_uri` by prefix, transactionally;
   - **clears `file_identity_json`** on remapped sources — `posix_dev`/`ino`
     are meaningless on another machine, and §12.3 already says identity is
     not portable. Stale identity that accidentally matches a foreign inode is
     the dangerous case; null is honest (this settles P4);
   - touches no hash, artifact, derivation, or object: provenance is
     machine-independent and must survive byte-identical;
   - reports counts: roots remapped, sources rewritten, identities cleared.
4. **Freshness after arrival**: nothing special — `chutni verify` re-hashes
   against the new paths and the §13 rules do the rest. Document this
   explicitly in the pack/unpack help text: *remap does not claim currency;
   verify establishes it* (§13.2's spirit applied to migration).
5. **SPEC.md §7.2 finalized**: exact member list, ordering rule, the
   manifest-first requirement, and a `pack_format: 1` field so the format can
   evolve.
6. **Conformance scenario 3 implemented** — the GAP retires: create store on
   "machine A" (directory A), pack, unpack into directory B, remap, verify →
   all current, search hits identical, provenance intact, a subsequent edit
   still stales correctly.

## Acceptance criteria

- Scenario 3 passes; the suite's GAP count drops by one and the GAP line is
  removed rather than hidden.
- Round-trip byte check: object store contents identical pre/post
  (hash-verified), catalog row counts identical except locator/identity
  columns.
- A malicious pack fixture (entry with `../escape`) is refused with a clear
  error; add it to the conformance suite permanently.
- Packing a 1 GB-object store doesn't hold everything in memory (streamed);
  state peak RSS in the evidence.
- `make test` and `make sanitize` green.

## Evidence required

Transcript of a real two-directory migration including the failed-before-remap
`verify`, the remap, and the clean verify after; plus the malicious-pack
refusal. Under `docs/evidence/`.

## Non-goals

- No network transfer, no sync, no merge of divergent stores (§37 territory).
- No re-encryption or credential handling — a pack is as sensitive as the
  store it came from; say so in the help text.

## Open questions

- Vendored minizip-style writer vs. hand-rolled ZIP64 writer: hand-rolled is
  ~600 lines and keeps the no-new-dependency rule; decide at implementation
  with the zip-slip tests written first either way.
