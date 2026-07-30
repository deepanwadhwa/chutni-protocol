# T15 — Publish the repository (owner-gated)

**Priority:** P0 in importance, **blocked on the owner's explicit go**
**Size:** S · **Depends on:** owner decision · **Spec:** §36
**Status:** proposed 2026-07-30 — **do not execute any outward step without
the owner asking. Repo rule: outward publishing waits for explicit
confirmation, and agent shells have no push credentials.**

## Why

Protocols die in private. Every adoption ticket in this directory has a
ceiling until SPEC.md has a public URL, an issue tracker exists, and a
stranger can file the first bug. §36 already commits to public-spec
governance; this ticket is the checklist that makes the flip deliberate
instead of eventual — prepared now, executed only on the owner's word.

## Pre-flight checklist (all doable privately, now)

1. **Name collision sweep**: GitHub org/repo, PyPI (`chutni`), npm, crates.io,
   Homebrew formula namespace; record what's taken and the fallback names
   (`chutni-protocol`, `libchutni`). Reserve nothing until the go — but know
   the answer before choosing.
2. **License hygiene**: LICENSE is Apache-2.0, NOTICE exists; verify vendored
   third-party licenses (SQLite public domain, BLAKE3 CC0/Apache dual) are
   reproduced per their terms in `third_party/` and referenced from NOTICE.
3. **History scrub**: `git log -p` sweep for absolute home paths, machine
   names, anything personal in committed evidence transcripts (they contain
   `/Users/<name>/…` today — decide: acceptable, or rewrite evidence with a
   neutral prefix *going forward* and accept old history as-is; rewriting
   published history later is worse).
4. **Community files**: `CONTRIBUTING.md` — must encode this repo's actual
   culture or the culture dies on contact with the first PR: evidence
   transcripts required for behavior claims, conformance test that fails
   before and passes after, GAPs declared never hidden, no vendored-code
   patches, `-Werror` on first-party code. Plus `SECURITY.md` (private report
   channel; §28 threat model summary) and a minimal issue template asking for
   store `spec_version`, `chutni version` output, and a transcript.
5. **Governance starter** (§36): a short `GOVERNANCE.md` — spec changes via
   PR + open issue, the two amendment precedents (§39, §34-C99) as worked
   examples of how changes get recorded, no vendor-privilege rule restated.
6. **CI definition, ready to enable**: GitHub Actions matrix — macOS arm64,
   Ubuntu x64 (first Linux run ever — expect and fix findings *before*
   publish; that alone justifies doing this ticket's prep early), `make test`
   + `make sanitize`, and the docs-invariant greps from T13 when it lands.
7. **Tag/release plan**: publish with history, `v0.2.0` tag already exists;
   release notes drawn from the evidence reports, scoped with the usual
   honesty ("verified on: …", GAP list included).

## On the go signal (owner executes or explicitly delegates)

- Create the repo, push `main` + tags; enable CI; verify green publicly.
- Turn on issues/discussions; pin a "state of the project" issue pointing at
  TASKS.md, this tickets directory, and the conformance GAP list — leading
  with what does **not** work yet is the credibility move this repo's culture
  has already paid for.
- Only after CI is green: reserve the package names chosen in pre-flight
  (placeholder releases pointing at the repo; real packages ship with
  T02/T11).

## Acceptance criteria

- Pre-flight items 1–7 complete and recorded in this file (edited in place,
  checklist ticked, findings linked) **without** any outward action.
- The Linux run from item 6 is green locally (or its findings are fixed) —
  publishing a "portable C99" project whose first Linux build fails in public
  is the avoidable embarrassment this checklist exists to avoid.
- Post-go: public CI green on both matrix legs; clone-and-`make test` works
  for a stranger (verified by doing it from a fresh clone outside the
  dev machine's checkout).

## Non-goals

- No announcement/marketing plan here; that's the owner's call entirely.
- No package publishing beyond name decisions — packages belong to T02/T11.
- No conduct-policy authoring beyond a standard template unless the owner
  wants otherwise.
