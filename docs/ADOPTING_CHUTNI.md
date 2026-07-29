# Adopting Chutni in an AI application

This is the practical implementation guide for `SPEC.md` §40. It applies to an
application that hosts a local model, calls a cloud model, or exposes tools to a
model. Samosa is the first guinea-pig host, not a privileged implementation.

## Who does what

| Component | Responsibility |
|---|---|
| Host application | Finds, creates, opens, updates, searches, and validates the store; enforces permissions |
| Language model | Suggests queries or produces derived content; never writes catalog rows directly |
| Chutni store | Holds portable sources, artifacts, provenance, representations, and disposable indexes |
| `chutni-mcp` or another adapter | Maps MCP or a native process call onto the same Chutni operations |

The host can link `libchutni`, invoke a conforming local service, or implement
the format itself. The result on disk must be the same protocol-defined store.

## Recommended integration: ship the reusable service

An application can bundle the reference `chutni-mcp` executable instead of
maintaining its own store implementation. This is packaging, not a
Samosa-specific fork: every host calls the same tool contract and every call
ultimately reaches `libchutni`.

There are two transports:

- **MCP stdio** for hosts with MCP support. Launch `chutni-mcp --stdio` (or just
  `chutni-mcp`) and use `tools/list` / `tools/call`.
- **One-shot JSON** for native hosts that already supervise child processes.
  Launch `chutni-mcp --call TOOL JSON`. It invokes the same tool handler, emits
  one JSON object on stdout, and exits nonzero when the tool reports an error.

The service exposes this stable application surface:

| Tool | Purpose | Writes? |
|---|---|---|
| `chutni_folder_status` | Classify a selected source/store and report the safe next action | no |
| `chutni_folder_activate` | After confirmation, create/open, authorize, register, and scan | yes |
| `chutni_discover` | Find bounded, registered/conventional stores | no |
| `chutni_store_info` | Read identity, roots, and catalog counts | no |
| `chutni_scan` | Rescan already-authorized roots and retire stale artifacts | yes |
| `chutni_search` | Retrieve current bounded excerpts with paths and provenance | no |
| `chutni_put_model_artifact` | Retain approved model output with complete derivation provenance | yes |

The three write operations that scan or retain model output require an explicit
`confirmed: true`. A host must set it only after the corresponding user action;
the service does not treat a model tool call as permission.

### What the end user does in a bundled application

1. Install and open the application. There is no separate Chutni install.
2. Choose a folder in the application's memory UI.
3. Review the adjacent store path and scan policy returned by
   `chutni_folder_status`.
4. Approve **Create/Open memory**.
5. The application invokes `chutni_folder_activate`; the folder's portable
   sibling store appears as `Folder.chutni`.
6. Future turns search that store, and another Chutni-capable application can
   open the same store without migration.

Applications may also support an independently installed `chutni-mcp`; this is
useful for extensible hosts such as desktop MCP clients. It is not required for
an application that bundles the service.

## The selected-folder contract

Given an ordinary directory selected by the user:

```text
selected source: /Users/me/Research
default store:   /Users/me/Research.chutni
```

The host follows this state machine:

```text
classify selected path
├─ valid Chutni store → validate and open it
└─ ordinary source directory
   ├─ associated store exists → validate and open it
   ├─ multiple stores match → ask the user
   ├─ unsupported/colliding path → report it; do not overwrite
   └─ no store → ask permission, create P.chutni, add P as root, scan
```

The default store is a sibling, not a child of the source directory, so a
recursive scan cannot ingest the store itself. An explicitly selected custom
store path is allowed.

## Minimal read-write host loop

The names below describe the §20 operations; exact function names depend on the
binding or transport.

```text
on_folder_selected(path P):
    if is_chutni_store(P):
        store = open_store(P)
    else:
        candidate = P + ".chutni"
        matches = valid_associations(P, candidate, discover())

        if matches.count > 1:
            return needs_user_choice(matches)
        if matches.count == 1:
            store = open_store(matches[0])
        else:
            require_user_permission(candidate, P, root_policy)
            store = create_store(candidate)
            add_root(store, P, root_policy)
            optionally_register(store)

    validate_version_and_capabilities(store)
    scan_only_authorized_roots(store)
    return store
```

During a scan:

```text
for each allowed file:
    source = add_or_update_source(locator, blake3(file_bytes))

    if source bytes are unchanged:
        reuse current compatible artifacts
    else:
        stale artifacts for the prior hash
        run deterministic extraction first
        record producer + derivation + new artifacts

rebuild disposable indexes as needed
```

During a model turn:

```text
results = search(user_prompt, current_only=true, authorized_roots=...)
verify results that will support exact claims
context = bounded excerpts + paths + selectors + provenance
answer = model(user_prompt, context_as_untrusted_data)
```

If the answer, summary, caption, or embedding should become reusable memory,
the host records it after generation. A model-generated artifact needs the
model identity, revision, runtime/application identity, recipe, parameters,
inputs, source hash, and selector. Merely receiving model text is not a valid
Chutni write.

## Reference C API mapping

The current reference library maps the host loop to:

```text
chutni_discover
chutni_open / chutni_create
chutni_root_add
chutni_source_put / chutni_source_refresh
chutni_producer_put / chutni_derivation_put
chutni_artifact_put
chutni_rebuild_indexes
chutni_search / chutni_search_semantic
chutni_check_freshness
```

The host remains responsible for the UI permission step and for mapping the
selected source directory to the adjacent default store path.

When using `chutni-mcp`, `chutni_folder_status` and
`chutni_folder_activate` perform that mapping consistently. The host still owns
the UI, the user confirmation, which store is active for a conversation, and
what retrieved text may be disclosed to a remote model.

## What “works with Chutni” means

A read-write host is not considered interoperable merely because it creates a
directory named `.chutni`. It must pass this handoff:

1. Host A creates `Research.chutni` for selected folder `Research`.
2. Host A indexes a file and records provenance.
3. Host B opens the same store without migration and retrieves the artifact.
4. The source changes.
5. Host B marks the old artifact stale and writes a new current artifact.
6. Host A reopens the same store and retrieves Host B's update.

This is conformance scenario 13. Samosa's first proof is one side of this
handoff; the standalone Chutni reference implementation is the other.

## Closed and extensible applications

An application whose developers implement §40 can use Chutni directly. An
extensible application can use an adapter or gateway that exposes the §20
operations. An application with neither native support nor an extension
mechanism cannot automatically understand a Chutni store; the protocol defines
interoperability, but it does not bypass the application's security or plugin
model.
