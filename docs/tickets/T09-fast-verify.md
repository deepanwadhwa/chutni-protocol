# T09 — Fast verify at scale

**Priority:** P1 · **Size:** M · **Depends on:** nothing · **Spec:** §13.2; TASKS open question
**Status:** proposed 2026-07-30

## Why

`chutni verify` re-hashes every file's full contents. Correct, and the reason
the freshness story is trustworthy — but on a 200k-file home directory it is a
coffee-break operation, and the plug-it-into-your-app goal implies stores that
big. The largest tree this project has ever measured is 464 directories; the
scale story is currently a guess, which the repo's own rules say we must not
ship as a claim.

The design constraint is already in the spec and must not be negotiated away:
**§13.2 permits a quick check to *withdraw* a claim of currency, never to
*establish* one.** A stat can prove a file changed; it cannot prove it didn't.

## Deliverables

1. **`chutni verify --quick`** with exactly these semantics:
   - stat every file source; where `size_bytes` **or** `mtime_ns` differs from
     the catalog, do the full §13 re-hash + staling for that source;
   - where stat matches, **do not** update `last_scanned_at` and do not report
     "current" — report a distinct `unchanged_by_stat` bucket, with the
     summary line stating plainly: *stat cannot prove currency (§13.2); run a
     full verify to establish it*;
   - directories: keep full listing re-enumeration always — it is already
     cheap (one readdir), and the listing hash *is* the cheap check;
   - exit code: nonzero only on findings a full verify would also flag
     (stale/missing), never on `unchanged_by_stat`.
2. **The documented escape hatch it closes over**: a same-size, same-mtime
   content change (deliberate `touch -r` forgery, or sub-mtime-granularity
   writes) escapes `--quick`. Put this in the help text and SPEC §13.2 note —
   the mode is for routine hygiene, full verify remains the truth operation
   and the default.
3. **A scale fixture and the first real numbers**: a generator script
   (`contrib/bench/make-tree.sh`) building a synthetic tree (target: 100k
   files / 10k directories, mixed sizes), then measured on the reference
   machine: full `verify`, `--quick` on unchanged, `--quick` with 1% drift,
   `scan` re-run. Peak RSS and wall time, published with machine details.
4. **Incidental fix this will force**: `verify` currently loads the full
   source list into memory via `chutni_sources_list`; at 100k+ sources decide
   whether an iterating cursor API is needed or the array is fine — measure
   first, then decide, and record the number either way.

## Acceptance criteria

- New conformance checks: (a) `--quick` catches a size-changed and an
  mtime-changed file and stales their artifacts identically to full verify;
  (b) `--quick` on an untouched store reports 0 current-claims established
  (the output literally must not contain "current" for stat-matched files);
  (c) a forged same-size/same-mtime edit is caught by full verify and — the
  honest part — **missed** by `--quick`, asserted as such so the limitation
  stays documented-by-test.
- The 100k-file numbers exist in evidence with the generator pinned, and
  README/TASKS scale claims cite them rather than adjectives.
- `make test` and `make sanitize` green.

## Evidence required

Benchmark transcript (generator invocation, timings table, RSS, machine and
compiler named) plus the conformance additions, under `docs/evidence/`.

## Non-goals

- No filesystem watchers (§13.4 is a separate, later concern).
- No change to search's existing stat-downgrade behavior ("unverified") — it
  already follows the same §13.2 asymmetry and stays as is.
- No parallel hashing in this ticket; measure single-threaded first so the
  baseline is understood before adding threads.
