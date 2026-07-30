# T02 — Python binding (stdlib-only ctypes)

**Priority:** P0 · **Size:** M · **Depends on:** T01 · **Spec:** §34, TASKS A4
**Status:** done 2026-07-30 — `python/chutni/`; tested by `make python-test`.

## Why

The audience that would adopt Chutni — indie LLM-app developers, RAG builders,
agent authors — writes Python first. A C ABI with no bindings is a foundation
with no door. This is the single highest-leverage adoption item in the repo:
every other adoption story (T04 enrichment, most third-party producers) sits on
top of it.

## Deliverables

1. **`python/chutni/__init__.py`** — one file, stdlib only (`ctypes`, `json`).
   No pip dependencies, matching the repo's no-dependency culture. Loads
   `libchutni` from, in order: `$CHUTNI_LIBRARY`, the installed location
   (`$PREFIX/lib`), an adjacent `build/` (developer checkout).

   Because of T01, the binding declares **four** foreign functions and is
   mostly ergonomics:

   ```python
   from chutni import Store, discover

   stores = discover()                       # [] on a fresh machine
   with Store.create("~/Memory.chutni", label="My files") as s:
       s.add_root("~/Documents", max_depth=1, memory_goal="define")
       result = s.scan()                     # dict, incl. coverage counters
       for hit in s.search("condensation force", limit=5):
           print(hit["display_path"], hit["freshness"], hit["snippet"])
   ```

2. **Pythonic surface** (thin, honest — no behavior the library doesn't have):
   - `Store.create / Store.open(read_only=) / close`, context manager.
   - `scan`, `observe(source)`, `children(source)`, `coverage(id=None)`,
     `search`, `search_semantic(vector, profile, ...)`, `verify(source=None)`,
     `put_artifacts(producer, operation, artifacts, input_refs=None, ...)`,
     `put_representation(artifact_id, profile, vector)`, `inspect(source)`,
     `forget(source, mode)`.
   - Results are plain dicts/lists straight from `chutni_call` JSON — do not
     invent wrapper classes that hide fields; `freshness`, `observation`,
     `coverage_manifest_id` must stay visible, they are the honesty features.
   - Errors raise `ChutniError(code, message)` from the T01 error envelope.
     `BUSY` gets its own subclass so hosts can retry (see T10).

3. **Packaging**: `pyproject.toml` for `pip install -e python/`. Wheels with a
   bundled dylib/so are **out of scope** until T15 (publication) decides
   distribution; until then, README documents `make && make install` first.

4. **Tests**: `make python-test` target running `python3 -m unittest` over a
   scratch store — create, scan a fixture tree with a depth bound, search,
   assert an opaque directory is reported opaque, submit a model artifact with
   provenance, read coverage. No pytest dependency.

5. **README section**: the ≤10-line example above, verbatim and actually run.

## Design notes

- Target Python ≥ 3.9. No asyncio — the library is synchronous; wrapping it
  is the caller's business.
- Memory rule: every `char **out` is copied to a Python `str` then freed with
  `chutni_free` before returning; never hand ctypes pointers to users.
- The GIL makes the "one handle per thread" C rule easy to respect; document
  that a `Store` must not be shared across threads without a lock.
- Semantic search takes a plain `list[float]`; the binding does not compute
  embeddings (Chutni never does) — T04 shows where embeddings come from.

## Acceptance criteria

- `make python-test` green on the reference machine; added to `make test`.
- The README example runs as written on a fresh checkout after
  `make && make install` (or with `CHUTNI_LIBRARY` pointed at `build/`).
- Every field the CLI's `--json` exposes is reachable from Python (spot-check
  list in the test: `freshness`, `observation`, `depth`,
  `coverage_manifest_id`, `complete_for_policy`, `stop_reason`).
- A deliberately wrong call (unknown operation, malformed args) raises
  `ChutniError` with the envelope code — no segfault path reachable from
  Python. Run the unittest suite once under the sanitized build.

## Evidence required

Transcript: fresh scratch dir, the README example run end-to-end, plus
`make python-test` output. Under `docs/evidence/`.

## Non-goals

- No PyPI upload before T15.
- No coverage of the typed struct API from Python — `chutni_call` only.
- No Windows paths (T14).
