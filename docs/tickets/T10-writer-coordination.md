# T10 — Multi-app writer coordination

**Priority:** P1 · **Size:** S · **Depends on:** nothing · **Spec:** §21, §28; TASKS §2 (stress testing)
**Status:** proposed 2026-07-30

## Why

The whole pitch is *several* applications sharing one store — which means two
of them will eventually want to write in the same second. Today: one advisory
writer lock, held for the life of a read-write handle; the loser gets
`CHUTNI_ERR_BUSY` immediately. `chutni-mcp` already behaves well (verified in
source: it opens the store per tool call, so lock windows are milliseconds),
but the CLI holds the lock for a whole command, and nothing anywhere retries.
So the realistic failure is mundane: a background scan is running, the user's
chat app tries to record a summary, and the user sees an error that reads like
a bug. That experience, multiplied across apps, is how shared stores lose to
private silos.

The fix is not a daemon. It is: short lock windows as the blessed pattern,
polite retries at the edges, and spec text that says so.

## Deliverables

1. **Opt-in busy-wait in the library**: `chutni_open` honors
   `CHUTNI_LOCK_WAIT_MS` (env) and a new `chutni_open_ex(path, flags,
   wait_ms, &out)` — retry the advisory lock with jittered backoff (e.g.
   25–250 ms steps) until acquired or the budget expires, then return `BUSY`
   as today. Default stays 0: no behavior change for existing callers, and
   `BUSY` remains immediate for anyone who wants to handle it themselves.
2. **CLI and MCP defaults**: `chutni` CLI and `chutni-mcp` adopt a modest
   default wait (e.g. 2000 ms) so casual concurrent use just works; both gain
   `--lock-wait-ms` / config to tune it, and the timeout error message says
   *which process holds the lock* if that is cheaply knowable (write the
   holder's pid + app name into the lock file at acquisition — it's advisory
   metadata, not security).
3. **Spec addition (§28.x, "Writer coordination")**, normative SHOULDs:
   - writers SHOULD hold write handles per operation, not per session
     (documenting the pattern `chutni-mcp` already implements);
   - writers SHOULD tolerate `BUSY` with bounded retry rather than surfacing
     it raw to users;
   - long-running hosts sharing a store SHOULD prefer routing writes through
     one service instance (the broker pattern) and MAY treat `chutni-mcp` as
     that broker — blessed, not mandated;
   - readers never wait: concurrent read handles remain lock-free as today.
4. **The stress test the TASKS gap has been promising**: a conformance-level
   torture run — N=4 writer processes doing scan/put/verify loops against one
   store for ≥60 s with wait enabled, M=4 readers searching concurrently;
   assert zero corruption (full re-verify at the end), zero deadlocks, and
   that every writer eventually made progress. Runs under `make stress`
   (not in the default `make test` — it's 60 s), but sanitizer-clean.

## Acceptance criteria

- Two simultaneous `chutni scan` invocations on one store: second one waits
  and completes; with `--lock-wait-ms 0`, second one fails fast with the
  holder identified in the message.
- MCP suite unchanged; new unit checks for `chutni_open_ex` semantics
  (0 = current behavior, budget respected within jitter tolerance).
- Stress run passes on the reference machine and the transcript is filed;
  TASKS §2 "concurrency stress testing" row updated to point at it.
- Spec text lands in the same series; `make test`/`make sanitize` green.

## Evidence required

Stress-run transcript (with machine details and durations) and a two-terminal
concurrent-scan demo transcript, under `docs/evidence/`.

## Non-goals

- No daemonized chutni-mcp, no sockets, no cross-machine anything (§37).
- No writer *queue fairness* guarantees — bounded retry with jitter is the
  contract; starvation-freedom proofs are not.
- No change to the single-writer invariant itself. It is correct; the UX
  around it is what's being fixed.
