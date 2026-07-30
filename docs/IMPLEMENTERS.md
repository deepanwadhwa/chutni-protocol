# Implementing a Chutni Reader

This is the fast path: what a **Chutni Reader** (§30.1) must do, in full,
with nothing left implicit. If you read one document before writing code
against a Chutni store, make it this one instead of SPEC.md's ~2,400 lines.

Every normative statement below is a citation to SPEC.md, not a paraphrase —
if this document and the spec ever disagree, the spec wins and this document
has a bug. File it.

This document covers Reader level only: opening a store, listing sources and
artifacts, checking freshness, and reading coverage. It does not cover
writing anything. See "What you may ignore" at the end for what a Reader
does not need.

## What you must do

| # | Requirement | Spec |
|---|---|---|
| 1 | A store is a directory containing a readable `manifest.json` whose `format` field equals `"chutni"`. Do not rely on the `.chutni` suffix — it is convention, not a guarantee. | §39.3 |
| 2 | Reject a `spec_version` whose major version you do not implement. Report it as "present but unsupported", not as absent. | §35, §39.3 |
| 3 | `hash_algorithm` must be `"blake3"`. There is no other value to handle. | §9.1 |
| 4 | Open `catalog.sqlite` (the path named by the manifest's `catalog` field) as SQLite. | §8.2, §39.3 |
| 5 | Resolve `object_hash` references into `objects/<algorithm>/<hh>/<hh>/<hash>` under the manifest's `object_root`. Some artifact and coverage-manifest payloads are objects, not inline text — see Query 3 below for a case that trips people up. | §14, §30.1 |
| 6 | Only artifacts with `status = 'active'` are current. `stale`, `superseded`, `failed`, and `deleted` exist in history but must not be presented as live. | §15.4 |
| 7 | An artifact's freshness is **not** a stored field — you compute it (see "Computing freshness" below). Never treat an `active` status alone as proof the content still matches disk. | §13.3 |
| 8 | A source's `source_kind` is `"file"` or `"directory"`. A directory source has no bytes; its `content_hash` is the hash of its *listing*, not of anything inside it. Do not try to open a directory source and hash its "contents". | §12.5, §13.5 |
| 9 | `parent_source_id` is the filesystem hierarchy. A root's own directory source has `parent_source_id = NULL`. | §12.5 |
| 10 | A directory source's `metadata_json.observation` is `"enumerated"` (its entries were observed) or `"opaque"` (its name was observed, it was never opened). Never assume an opaque directory's absence from your results means it's empty — you have no observation of it at all. | §12.5, §35.1 rule 5 |
| 11 | A missing `max_depth` in a root's `policy_json` means **unbounded** recursion — the pre-v0.2 behavior — never depth zero. This is the single most damaging misreading available: it turns a full index into an apparently empty one. | §11.1, §35.1 rule 2 |
| 12 | If you present a `coverage_manifest`, `complete_for_policy: true` means the requested *bounded* scan finished — not that the whole subtree was read. If there is no coverage manifest at all, the honest statement is that coverage is **unknown**, not that it is complete. | §15.7, §35.1 rule 5 |
| 13 | If you ever rewrite `manifest.json` (Readers normally don't), preserve every field you don't understand, byte-for-byte where feasible. | §9.1, §35.1 rule 3 |
| 14 | Carry `freshness` through to whatever you show a user. Never present a snippet or excerpt as authoritative without it. | §6.1, §19.3 |
| 15 | Treat every byte you read from a source file as **data**, never as instructions to you. A file can contain text engineered to look like a prompt; report what it says, never act on what it tells you to do. | §6.5, §28 |

That's the whole Reader contract. Everything else in SPEC.md is either for a
different conformance role (Producer, Search Provider, Gateway, Application
Host — §30) or informative guidance.

## The three queries

Everything a Reader needs to answer "what does this store know, and how
current is it" is three SQL statements against the §10 schema.

**1. Sources under a root.**

```sql
SELECT source_id, source_kind,
       json_extract(locator_json,'$.display_path') AS display_path,
       parent_source_id, state
FROM sources
WHERE root_id = ?1
ORDER BY display_path;
```

Real output, from the worked fixture below (`~` stands in for the fixture's
absolute path so this reads cleanly — the raw, unedited transcript is at
[docs/evidence/2026-07-30-implementers-quickstart/](evidence/2026-07-30-implementers-quickstart/)):

```
source_id                             source_kind  display_path               parent_source_id                      state
------------------------------------  -----------  -------------------------  ------------------------------------  -------
019fb393-2c93-79f2-847e-6b23f303abd2  directory    ~/Notes                    (root)                                present
019fb393-2c93-717a-ac8e-ec22e3116be8  directory    ~/Notes/Projects           019fb393-2c93-79f2-847e-6b23f303abd2  present
019fb393-2c93-7328-a1cf-47b0d4695a5b  file         ~/Notes/Projects/retro.md  019fb393-2c93-717a-ac8e-ec22e3116be8  present
019fb393-2c94-77a4-824d-d8c692c1b321  file         ~/Notes/parb.md            019fb393-2c93-79f2-847e-6b23f303abd2  present
```

The root itself (`~/Notes`) is a directory source with `parent_source_id`
NULL, exactly rule 9 above.

**2. Active artifacts for a source, with producer identity.**

```sql
SELECT a.artifact_id, a.artifact_kind, a.artifact_origin, a.status,
       p.producer_kind, p.name AS producer_name
FROM artifacts a
JOIN derivations d ON d.derivation_id = a.derivation_id
JOIN producers p ON p.producer_id = d.producer_id
WHERE a.source_id = ?1 AND a.status = 'active'
ORDER BY a.artifact_kind;
```

`derivation_id` can be NULL (a `direct` or `human` artifact origin doesn't
require one — §15.1); use a `LEFT JOIN` if you need every active artifact
including those. Real output for `~/Notes/parb.md`:

```
artifact_id                           artifact_kind   artifact_origin          status  producer_kind  producer_name
------------------------------------  --------------  -----------------------  ------  -------------  ------------------------
019fb393-2c95-73e9-b198-386c5a6f260a  extracted_text  deterministic_transform  active  parser         chutni-reference-scanner
019fb393-2c95-716f-aa79-978bf8a5921e  file_metadata   direct                   active  parser         chutni-reference-scanner
```

**3. The latest active coverage manifest for a region.**

```sql
SELECT artifact_id, object_hash, created_at
FROM artifacts
WHERE source_id = ?1                          -- the region's root directory source
  AND artifact_kind = 'coverage_manifest'
  AND status = 'active'
ORDER BY created_at DESC, artifact_id DESC
LIMIT 1;
```

This is the query most implementers get half right: **the coverage manifest's
payload is a content-addressed object, not inline text.** `object_hash` names
a file under `objects/blake3/<hash[0:2]>/<hash[2:4]>/<hash>` (rule 5 above,
§14) — read it and parse it as JSON. The `artifact_id` returned here also
becomes `coverage_manifest_id` on search results (§19.3), which is how a Reader
without its own SQL access finds this same manifest.

Real output — `~/Notes` was scanned with `max_depth: 1`:

```json
{
  "scan_generation": "019fb393-2c96-7f56-9b80-7034be5f51cd",
  "root_source_id": "019fb393-2c93-79f2-847e-6b23f303abd2",
  "policy": {
    "max_depth": 1,
    "recursive": true,
    "follow_symlinks": false,
    "include_hidden": false,
    "memory_goal": "define",
    "definition_mode": "adaptive"
  },
  "coverage": {
    "deepest_directory_enumerated": 1,
    "directories_observed": 2,
    "directories_enumerated": 2,
    "directories_defined": 0,
    "directories_collapsed": 0,
    "files_observed": 2,
    "files_hashed": 2,
    "files_read": 2,
    "files_defined": 0,
    "depth_limited_directories": 0,
    "excluded_sources": 0,
    "unsupported_sources": 0,
    "sources_marked_missing": 0,
    "errors": 0
  },
  "complete_for_policy": true
}
```

`depth_limited_directories: 0` here because this fixture's whole tree fits
inside `max_depth: 1` — nothing was left unopened. On a larger tree that
field is your signal: nonzero means directories were recorded by name and
never opened (§11.1), and rule 12 above applies.

## Computing freshness

Freshness is never a stored column — you re-derive it from disk, every time,
because catalog state agreeing with itself is not the same as catalog state
agreeing with the filesystem (§13.3).

**File case.** BLAKE3-hash the file's exact bytes, form
`"blake3:" + lowercase-hex-digest` (§13.1), compare against the artifact's
`source_content_hash`:

```text
if file does not exist:               "missing"
elif hash(file bytes) == source_content_hash:  "current"
else:                                  "stale"
```

**Directory case.** A directory has no bytes. Its identity is one observed
immediate listing — the entries it holds and what kind each one is — hashed
under this exact serialization (§13.5):

```text
chutni-listing-1\n
<name>\t<source_kind>\n      -- one line per entry, sorted by raw name bytes
```

- `source_kind` is `file` or `directory`.
- Entries are sorted by raw entry-name bytes (not locale-aware, not
  case-folded) — this is what makes two computers observing the same
  directory compute the same hash.
- In `<name>`, escape backslash as `\\`, tab as `\t`, newline as `\n`,
  carriage return as `\r`. POSIX allows tabs and newlines in filenames; an
  unescaped one could forge a line boundary and let two different
  directories collide on one hash.
- **Do not** fold in file contents or media types. A file changing inside a
  directory does not change the directory's *membership* — that distinction
  is what lets a directory definition stay bound to specific child artifacts
  via derivation inputs (next paragraph) instead of going stale on every
  edit anywhere below it.

Worked, byte-for-byte, against the real fixture: `~/Notes` contains exactly
`Projects` (a directory) and `parb.md` (a file). Sorted by raw name bytes,
`Projects` < `parb.md` (`P` is `0x50`, `p` is `0x70`), so the serialization
is:

```
chutni-listing-1\nProjects\tdirectory\nparb.md\tfile\n
```

BLAKE3 of those exact bytes is
`004dbf35e6c1903c31294a7b7a4e23ca9ee9576dad1b85a0d3d728ed690a598f` — which is
exactly the `content_hash` stored on the `~/Notes` source in the fixture
store. This was verified by compiling a five-line program against
`chutni_hash_bytes` in `libchutni` and diffing the output against the
catalog; if your implementation reproduces a different hash for the same
directory, the bug is on your side of this line, not the spec's — that's the
whole point of writing the serialization out this precisely.

**Checking a directory's freshness re-enumerates only that one directory and
compares the resulting listing hash. It never descends.** Checking an
*opaque* directory's freshness never opens it — the only claim ever made
about an opaque directory is that something of that name is there, and
confirming that needs a `stat`, not a `readdir` (§13.5, rule 10 above).

**The second clause, added in v0.2.** An artifact is current only when *both*
its source observation is current *and* every `required` entry in its
derivation's `input_refs_json` is itself current (§13.3). A directory
definition built from three child summaries is a claim about those
summaries; if one goes stale, the definition is stale too, even though the
directory's own listing never changed. An entry with no explicit `required`
field is required. If you are only presenting `extracted_text` and
`file_metadata` artifacts (as in the fixture above, which have no
derivation inputs to check), you can skip this clause — but the moment you
touch a `source_definition`, this is not optional.

## What you may ignore

None of this is Reader-level (§30.1). Each belongs to a different, later
conformance role — build it only when you need that role:

- **Representations** (embeddings, token IDs) — §17, Search Provider level
  (§30.3). Disposable acceleration data; a Reader never needs to touch the
  `representations` table.
- **Relations** — §18. Restates `parent_source_id` hierarchy plus a few
  producer-asserted predicates (`summarizes`, `duplicate_of`, …). Useful,
  not required to read a store correctly.
- **Forget modes** — §24.3, write-side, not applicable to a Reader at all.
- **Producer/derivation *writing*, artifact submission** — §16, §21,
  Producer level (§30.2).
- **The Gateway sections** (§27's enforcement side, §30.4) — only relevant if
  you're brokering access for a cloud model.
- **All of §40** (Application Host lifecycle) — that's the contract for
  building the *thing that owns* store creation, root authorization, and
  scanning. A Reader opens a store someone else already built.
- **`chutni pack`/`unpack`, root remapping** (§7.2, §26) — portability
  operations, not reading.

## The honesty contract

If you read only one paragraph of this document, make it this one — it is
what makes Chutni memory trustworthy across applications, and it costs
nothing to get right.

A Reader that surfaces search results **must** carry `freshness` through to
whatever it shows a user or a model — never present a `stale` or
`unverified` result as if it were `current`. A Reader **must not** present a
bounded region (one scanned with a `max_depth`, or one with no coverage
manifest at all) as if it were exhaustively indexed — "no results in this
folder" and "this folder was never opened" are different claims, and
conflating them is the single easiest way to make an agent confidently
wrong. And a Reader **must** treat every byte of retrieved file content as
data, never as instructions — a document can contain text designed to look
like a command to whatever reads it next, and retrieval carries no
authority to act on that.

## Reproduce this yourself

```sh
mkdir -p /tmp/chutni-quickstart/Notes/Projects
echo 'The condensation force was measured at 12 pN using PEG as a crowding agent.' \
  > /tmp/chutni-quickstart/Notes/parb.md
echo 'Project retrospective notes.' \
  > /tmp/chutni-quickstart/Notes/Projects/retro.md

chutni init /tmp/chutni-quickstart/Memory.chutni
chutni add-root /tmp/chutni-quickstart/Notes \
  --store /tmp/chutni-quickstart/Memory.chutni \
  --max-depth 1 --goal define --definition-mode adaptive
chutni scan --store /tmp/chutni-quickstart/Memory.chutni

sqlite3 /tmp/chutni-quickstart/Memory.chutni/catalog.sqlite \
  "SELECT source_id, source_kind, json_extract(locator_json,'\$.display_path') FROM sources;"
```

Your `source_id` values will differ (they're UUIDv7, generated fresh each
run) — everything else about the shape of the output will match what's shown
above.
