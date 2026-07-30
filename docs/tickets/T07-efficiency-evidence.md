# T07 — Measure the token-savings claim

**Priority:** P1 · **Size:** M · **Depends on:** T05 · **Spec:** repo culture ("evidence, not assertion")
**Status:** proposed 2026-07-30

## Why

The project's pitch — the owner's own words — is *save tokens, don't do the
same work twice, run efficiently*. Right now that claim has **zero
measurements** behind it, and this repo's first non-negotiable is that claims
are scoped to what was measured. Either the numbers exist and become the
strongest line in the README, or they don't materialize and the pitch needs
rewording. Both outcomes are worth having.

## What to measure

Three separate claims, measured separately — do not blend them:

1. **Agent token savings (retrieval).** Fixed corpus, fixed question set,
   same agent, same model, two conditions:
   - **A (baseline):** Claude Code with filesystem tools only (grep/read).
   - **B (chutni):** the same, plus the T05 MCP integration and skill, store
     pre-scanned (scan cost accounted separately, see 3).

   Per question: input tokens, output tokens, tool calls, wall time, and
   whether the answer cited a correct source. Tokens come from the agent's
   usage reporting, not estimates. ≥10 questions spanning: needle-in-file,
   cross-file synthesis, "not in the corpus" (honesty check), and a stale-file
   trap.

2. **Extraction reuse (the "never twice" claim).** Time and bytes processed
   for: first `scan` + T04 enrichment of a PDF-heavy folder, versus re-run on
   the unchanged folder, versus re-run after touching one file. The counters
   already exist (`unchanged`, `listings_reused`, artifact reuse); this turns
   them into a table. Then the cross-app version: a second "application" (the
   Python binding acting as a different host) opens the same store and answers
   without re-extracting anything — proving the *shared* half.

3. **Amortization honesty.** The store isn't free: report scan + enrichment
   cost (time, and tokens if any model producer is used) and compute the
   break-even — after how many questions does condition B win? If the honest
   answer is "three sessions", print that; it is a fine answer.

## Deliverables

1. **`contrib/bench/`** — corpus manifest (public documents only, pinned by
   hash so the run is reproducible), question set with expected-source
   annotations, run scripts for both conditions, and a results table
   generator. No hidden knobs: every model/agent version pinned in the output.
2. **Evidence report** under `docs/evidence/` — methodology, raw per-question
   table, the three summary tables, and a limitations section written before
   the numbers are quoted anywhere (single machine, single model, N=10,
   corpus composition bias, cache effects).
3. **README update** quoting only what was measured, with a link to the
   report. Format: "On <corpus>, <agent+model>, condition B used X% fewer
   input tokens across N questions (see evidence)" — never "saves tokens"
   unqualified.

## Acceptance criteria

- A second run of the harness on the same machine reproduces the tables
  (within stated noise for wall time; token counts should be near-exact for
  condition B and may vary for A — report both runs).
- The stale-file trap and the "not in corpus" question are scored for
  *honesty*, not just tokens, and reported even if unfavorable.
- If condition B loses on any axis, that row ships anyway. A benchmark that
  can only say yes is marketing.

## Non-goals

- No cross-model leaderboard, no comparisons against other RAG stacks — this
  measures Chutni against *not using it*, nothing else.
- No synthetic token estimates; if usage reporting is unavailable for some
  client, that client is out of scope for this ticket.

## Open questions

- Question-set size vs. cost: 10 is the floor for a first report; decide
  whether 30 is worth it after seeing variance.
