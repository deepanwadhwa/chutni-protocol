# v0.2 hierarchical sources and bounded coverage

**Date:** 2026-07-30
**Machine:** macOS 15, Darwin arm64, Apple clang 21.0.0. **The only machine any
of this has run on.**
**Spec:** [SPEC.md](../../../SPEC.md) v0.2-draft

What was built: directories as first-class sources, normative depth bounds,
three new artifact kinds, required local coverage on directory definitions,
directory freshness, bounded reconciliation, hierarchy relations, four new
protocol operations, and the scanner rewrite behind all of it.

## Transcripts in this directory

| File | What it is |
|---|---|
| [make-test.txt](make-test.txt) | Full suite, exit 0 |
| [make-sanitize.txt](make-sanitize.txt) | Same suites under ASan + UBSan, exit 0 |
| [real-folder.txt](real-folder.txt) | Bounded scans of a real 464-directory tree |
| [v01-compatibility.txt](v01-compatibility.txt) | A store forged back to v0.1 shape, then scanned |

## Suite result

```
105 BLAKE3 official vectors
 79 conformance assertions, 0 failed, 1 declared gap   (was 52)
 32 CLI checks, 0 failed                               (was 16)
 90 MCP checks, 0 failed                               (was 44 claimed)
```

`make sanitize` re-runs the conformance, CLI, and MCP suites under
AddressSanitizer and UndefinedBehaviorSanitizer with no diagnostics.

The one remaining gap is unchanged and unrelated: scenario 3, moved-root
remapping (§26), still unimplemented and still reported as `GAP`.

Two counts moved for reasons worth stating rather than burying:

- **The MCP suite reported "44 passed" while `main()` ran 66 assertions.** The
  number was a hardcoded literal that had drifted. It now counts the assertions
  in `main()` from the source, so it cannot drift again. 90 is the real figure
  after the new checks, not an inflation of the old one.
- **Two CLI checks were failing before any of this work started**, on this
  machine only. `CHUTNI_HOME` relocates the registry, but §39 discovery also
  sweeps `$HOME` and `$HOME/Documents`, so "discover reports nothing on a fresh
  machine" was failing against the owner's own `~/Downloads.chutni` and
  `~/Documents/sehat.chutni`. Both suites now redirect `HOME` as well. This was
  a test-isolation defect, not a product defect, and it was hiding on any
  machine that had never created a real store.

## Depth is enforced, on a real tree

Not a fixture. The protocol repository itself: 464 directories, 362 files.

| `max_depth` | dirs enumerated | dirs recorded, never opened | files indexed | wall clock |
|---|---|---|---|---|
| 0 | 1 | 7 | 6 | 0.01 s |
| 1 | 8 | 9 | 24 | 0.06 s |
| 2 | 17 | 40 | 46 | 0.36 s |

Each is one run, `/usr/bin/time -p`, warm page cache, on the machine named
above. These are not benchmarks and nothing was measured repeatedly; they are
here to show the bound doing what it claims, and that a depth-0 refresh of a
464-directory tree is cheap because it opens one directory.

The distinction the feature exists for, visible at the boundary:

```
$ chutni children <repo>/third_party
directory opaque     .../third_party/blake3
directory opaque     .../third_party/sqlite
```

`opaque` means the name was observed and the directory was never opened.
Without it, "no results under `third_party/sqlite`" and "we never looked at
`third_party/sqlite`" are the same empty answer.

## What the conformance suite now proves

Scenarios 14–18, 27 assertions, all covering claims that would otherwise be
assertion rather than evidence:

- Depth 0 enumerates only the root; depth 1 adds immediate child directories
  and stops; grandchildren stay opaque with the right recorded depth.
- Directory sources carry correct `parent_source_id` and depth.
- A directory definition without `coverage.stop_reason` and
  `coverage.complete_for_policy` is **refused** — `CHUTNI_ERR_INVALID`.
- A definition goes stale when a required derivation input goes stale, **while
  the directory's own listing is unchanged**. This is the case that a
  listing-hash-only design gets wrong.
- A changed listing stales definitions bound to the old listing.
- An unchanged listing is reused, not rewritten (3 reused, 0 written).
- A shallow refresh does not mark deeper sources missing; the same shallow
  refresh does mark its own direct child missing.
- A second consumer, opening the store read-only with no shared code, reads the
  coverage manifest, distinguishes enumerated from opaque directories, and gets
  `coverage_manifest_id` on every search result.
- A v0.1 policy with no `max_depth` still scans unbounded.

## v0.1 compatibility, checked against a forged v0.1 store

`v01-compatibility.txt` takes a store, rewrites its manifest back to
`spec_version: "0.1"` with the v0.1 capability list and an unknown field, and
strips `max_depth` from `policy_json` the way a real v0.1 writer would have left
it. Then this build scans it.

- The absent `max_depth` scanned **unbounded**, reaching the depth-2 file. This
  is the misreading §35.1 exists to prevent, and it would have turned a full
  index into an apparently empty one.
- `some_future_field` survived the manifest rewrite (§9.1).
- `spec_version` and the hierarchical capabilities were added only once the
  store actually used the feature, not on open.

The same transcript shows directory freshness doing the thing the design turns
on: adding a file to `tree/sub` makes **`tree/sub` stale** while the files
inside it stay `current`. The directory's listing changed; nothing else did.

## Two defects found and fixed during this work

Both are recorded because the transcripts alone would not show them.

1. **Use-after-free in `chutni_observe_directory`.** `sc.root_id` pointed into
   the `chutni_root_info` array, which was freed before the walk ran. SQLite
   bound freed memory as `root_id`, the foreign key failed, and the operation
   reported "1 directory enumerated, 0 files" — a plausible-looking result that
   was entirely wrong. Caught by reading the counters, not by a crash; ASan did
   not flag it because the suite did not exercise that path yet. It does now.

2. **`chutni_source_put` had the §13.3 staling logic inlined**, and
   `chutni_source_refresh` had a second copy. Adding directories would have made
   three. They are now one function, `stale_artifacts_not_matching`, because
   the two defects this format exists to prevent were both freshness logic that
   had drifted apart between copies.

## What was not done

- **`exclude_globs` is still unenforced.** The listing hash is policy-relative
  and honors `include_hidden` and `follow_symlinks`; exclusions are still the
  hardcoded name list (`.git`, `node_modules`, …). A glob against a bare entry
  name cannot express `**/node_modules/**` anyway, so this needs a design, not
  a patch. Recorded in [TASKS.md](../../TASKS.md).
- **Not run on Windows or Linux.** Same POSIX-only limits as v0.1.
- **No producer has written a real directory definition.** The conformance
  suite writes one with a synthetic model producer. The path is proven; a
  language model actually classifying a directory is Samosa's work (Phase S).
- **No large-tree measurement.** The biggest tree scanned is 464 directories.
  Nothing here supports a claim about a home directory or a network volume.
