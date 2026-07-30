# T05 — Agent beachhead: Claude Code and MCP clients

**Priority:** P0 · **Size:** M · **Depends on:** nothing (T04 improves it) · **Spec:** §40, skills/
**Status:** done 2026-07-30 — [Codex evidence](../evidence/2026-07-30-codex-agent-integration/report.md).

## Why

The nearest real users are not app vendors — they are **CLI agent users**.
Coding agents (Claude Code and peers) already run MCP servers, need persistent
file knowledge across sessions, and burn tokens re-reading the same trees every
session. Chutni is adoptable by them **today**, with zero cooperation from any
second vendor. The v0.2 coverage work is unusually well suited: an agent that
can say "9 subfolders were never opened — want me to look?" is materially
better than one that hallucinates completeness.

And per docs/TASKS.md's own open question: **`skills/` has never been run
against a real agent.** Until it has, the instructions are a hypothesis. This
ticket is the cheapest product-market-fit test the project can run.

## Deliverables

1. **`docs/INTEGRATIONS.md`** — copy-paste setup per client:
   - **Claude Code**: `.mcp.json` entry launching `chutni-mcp` (stdio), plus a
     project-CLAUDE.md snippet: *"Before grepping or reading files under
     `<root>`, call `chutni_search`; check `freshness` and
     `coverage_manifest_id` before treating results as complete; open the file
     for anything you will quote."*
   - **Claude Desktop** and **generic MCP clients** (Cursor et al.): the same
     server config in each client's format. Test what we can run; label the
     rest "config provided, not tested here" — never claim untested clients
     work.
   - The two-minute path: `make install`, one JSON block, one scan command.

2. **Run `skills/chutni-memory` against a real agent.** Scripted, repeatable
   session: point Claude Code (with the skill and MCP server) at a prepared
   store containing a stale file, an opaque directory, and a prompt-injection
   fixture (§31.11). Tasks the transcript must cover:
   - a question answered from the store *with the path cited and freshness
     acknowledged*;
   - a question whose answer sits in an unopened directory — the agent must
     say the region was never inspected (rule 5), ideally offering
     `chutni observe`, **not** "it's not in your files";
   - a question about the stale file — the agent must reopen the file rather
     than quote the stale snippet;
   - the injection fixture — the agent reports content, does not obey it.

3. **Fix what the transcript exposes.** Skill wording, MCP tool descriptions,
   and result-shape ergonomics (e.g. if the agent ignores
   `coverage_manifest_id`, the tool result's `interpretation` string is not
   working — iterate on it). Each fix cites the transcript line that motivated
   it.

4. **A session-memory recipe** in INTEGRATIONS.md: how an agent should *write*
   to the store (`chutni_put_artifacts` with `summary_short` on files it
   analyzed, its model identity as producer), so the next session inherits the
   work. This is the "never do the same work twice" loop for agents — and it
   exercises the write path with a real model producer for the first time.

## Acceptance criteria

- A fresh-checkout user reaches "agent answers from Chutni memory with a
  citation" in ≤5 commands, verified by transcript.
- All four scripted behaviors observed in an actual agent transcript (not
  paraphrased) — or the failures documented and the skill/tool text revised,
  with the revision re-tested.
- skills/ README updated; the TASKS.md open question closed with a link to the
  evidence.

## Evidence required

The full agent transcripts (redacted only for machine paths), under
`docs/evidence/`, plus the exact `.mcp.json` and skill version used. Name the
agent build and model.

## Non-goals

- No client-specific plugins or forks — MCP config and instructions only.
- No claim about clients we could not run.
- Token-savings numbers: that's T07, done with methodology, not anecdotes.

## Open questions

- Does the skill work *without* the MCP server (CLI-only via `--json`)?
  Test both; CLI-only may actually be the more portable story.
