# T04 — First-run value: PDF text and embeddings

**Priority:** P0 · **Size:** M · **Depends on:** T01, T02 · **Spec:** §15, §17, §25
**Status:** proposed 2026-07-30

## Why

Today, someone who builds Chutni and points it at a real Documents folder gets:
UTF-8 text extraction for code/notes, honest `file_metadata` stubs for
everything else, and keyword-only search. PDFs — the #1 personal-document
format — contribute nothing searchable. No embeddings exist, so semantic search
(built and tested in the C API) is unreachable. First impression: "grep with
extra steps."

The architecture is deliberately right — extraction and embedding are *host*
work, the core stays dependency-free — but the first five minutes decide
adoption. The fix is not to bloat the core; it is to make the host role
instantly playable: a shell-level ingestion door, plus one optional companion
script that uses tools people already have (`pdftotext`, Ollama).

The goal state, verbatim from the owner: *plug Chutni into your LLM app of
choice and stop doing the same work twice.* Extraction and embedding are
exactly the work people currently redo per-app.

## Deliverables

1. **`chutni submit`** — the shell-level ingestion door. Reads one JSON batch
   from stdin and calls `chutni_artifacts_put` (via T01):

   ```sh
   pdftotext report.pdf - | jq -Rs '{
     producer: {producer_kind:"parser", name:"pdftotext", version:"24.02",
                app_name:"my-pipeline", app_version:"1"},
     operation: "extract_text",
     artifacts: [{source_path: "report.pdf",
                  artifact_kind: "extracted_text",
                  media_type: "text/plain", inline_text: .}]
   }' | chutni submit
   ```

   `chutni submit` resolves `source_path` → source_id + current content hash
   (creating/refreshing the source if needed), so a pipeline never handles
   hashes by hand — but the artifact is still bound to the exact bytes, per
   §13.3. Refuses batches whose file changed mid-pipeline (`ERR_DENIED`),
   which is the honest failure.

2. **`contrib/enrich/enrich.py`** — optional companion, built on T02:
   - finds sources whose media type is `application/pdf` (extendable) with no
     active `extracted_text`;
   - extracts via `pdftotext` if on PATH, else `pypdf` if importable, else
     records an honest `processing_error` artifact (§15.2) — never silence;
   - `--embed --ollama-url http://localhost:11434 --model nomic-embed-text`:
     embeds every active `extracted_text`/`page_text` artifact lacking a
     representation for that profile, via `put_representation`, recording the
     full §17 profile (model id, revision if the server reports one,
     dimensions, normalization);
   - idempotent: re-running does nothing when everything is current — reuse is
     the entire point of the store;
   - every write carries real provenance: producer = the actual tool + version
     (`pdftotext --version` output), never "enrich.py".

3. **`chutni search --semantic`** wiring documented honestly: the CLI cannot
   embed a query. `enrich.py query "text"` does the query-side embedding
   against the same profile and calls semantic search, printing the standard
   result shape. Document that mixing profiles is refused by the library
   (§22.6) — that refusal is a feature; say so where users will hit it.

4. **README: "First five minutes" section** — the full real flow:

   ```sh
   chutni init ~/Memory.chutni && chutni add-root ~/Documents --max-depth 2
   chutni scan
   python3 contrib/enrich/enrich.py --embed          # needs ollama running
   python3 contrib/enrich/enrich.py query "that lease clause about subletting"
   ```

## Design notes

- Nothing new is vendored; the core keeps zero dependencies. `contrib/` may
  import from PATH/pip but must degrade with honest errors, not fake results.
- Page-level PDF extraction (`page_text` + §15.3 `pages` selector) is better
  than whole-file when `pdftotext` gives page breaks (`\f`): split on form
  feeds, one artifact per page with a `pages` selector. Do this from the
  start — page citations are the token-saving move for agents.
- OCR, image captioning, audio: **out of scope** here; the pattern this ticket
  establishes (find gap → run tool → submit with provenance) is what makes
  those easy later.

## Acceptance criteria

- Fixture folder with ≥2 real PDFs: after the four-command flow, a phrase that
  appears only inside a PDF is found lexically **and** semantically, with a
  page selector on the hit.
- Re-running `enrich.py` on an unchanged store submits nothing (counters
  prove reuse).
- Editing a PDF, `chutni verify`, re-running enrich: old artifacts stale, new
  extraction bound to new hash — the §13.3 loop closes through a third-party
  tool.
- Unavailable Ollama → clear error, store untouched.
- `chutni submit` rejects a batch whose `source_path` bytes changed since the
  pipeline read them.

## Evidence required

Full transcript of the first-five-minutes flow on a real folder with real
PDFs (named machine, tool versions), under `docs/evidence/`.

## Non-goals

- No bundled models, no network calls from libchutni, ever (§27).
- No claim that `enrich.py` output is *correct* — provenance yes, truth no
  (§16.4).
