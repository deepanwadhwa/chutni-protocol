/* Chutni — portable, source-backed memory for local files.
 *
 * Reference implementation of the Chutni protocol, specification version 0.2.
 * See SPEC.md. This header is the stable C ABI; language bindings sit on top
 * of it rather than under it.
 *
 * v0.2 adds hierarchical sources, bounded coverage, and directory definitions
 * (§12.5, §11.1, §15.5–§15.7). Structs gained fields at the end rather than in
 * the middle, so the field order a v0.1 caller compiled against still holds;
 * the struct sizes changed, so callers recompile against this header.
 *
 * Threading: a chutni_store handle is not thread-safe. Open one handle per
 * thread, or serialize access. Multiple processes may hold read handles on the
 * same store concurrently. A read-write handle holds the store's advisory
 * single-writer lock until close, so another writer receives CHUTNI_ERR_BUSY
 * instead of interleaving catalog, object, and index changes.
 *
 * Ownership: every function returning heap memory documents its free function.
 * Strings are NUL-terminated UTF-8.
 */
#ifndef CHUTNI_H
#define CHUTNI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHUTNI_SPEC_VERSION "0.2"

/* A "blake3:" prefix plus 64 lowercase hex characters plus NUL. */
#define CHUTNI_HASH_STRLEN 72
/* A UUID in canonical 8-4-4-4-12 form plus NUL. */
#define CHUTNI_ID_STRLEN 37

typedef enum {
    CHUTNI_OK = 0,
    CHUTNI_ERR_IO = -1,        /* filesystem or syscall failure; see errno */
    CHUTNI_ERR_FORMAT = -2,    /* malformed manifest, catalog, or JSON */
    CHUTNI_ERR_VERSION = -3,   /* unsupported spec major version (§35) */
    CHUTNI_ERR_NOTFOUND = -4,
    CHUTNI_ERR_INVALID = -5,   /* caller passed something the API rejects */
    CHUTNI_ERR_DB = -6,        /* SQLite reported an error */
    CHUTNI_ERR_NOMEM = -7,
    CHUTNI_ERR_DENIED = -8,    /* policy or permission refused the operation */
    CHUTNI_ERR_EXISTS = -9,
    CHUTNI_ERR_READONLY = -10,
    CHUTNI_ERR_BUSY = -11       /* another process owns the store's writer lock */
} chutni_status;

typedef struct chutni_store chutni_store;

const char *chutni_strerror(chutni_status status);
const char *chutni_spec_version(void);
const char *chutni_library_version(void);

/* Last human-readable detail for the most recent failure on this store, or on
 * the library when store is NULL. Valid until the next call on that handle. */
const char *chutni_last_error(const chutni_store *store);

/* ---------------------------------------------------------------- discovery
 *
 * Any application may ask whether Chutni memory already exists on this
 * computer before deciding to create it. See SPEC.md §39. A host responding
 * to a user-selected source directory follows the complete §40 lifecycle; the
 * host, not the model, owns these calls and their permission boundary.
 */

typedef struct {
    char *store_path;    /* absolute path to the .chutni directory */
    char *store_id;
    char *label;         /* may be NULL */
    char *spec_version;
    int   readable;      /* nonzero when the catalog opened successfully */
} chutni_store_info;

/* Enumerate stores from $CHUTNI_STORE, then the registry, then conventional
 * locations. Never scans the whole filesystem. Sets *count to 0 and *out to
 * NULL when nothing is found; that is CHUTNI_OK, not an error. */
chutni_status chutni_discover(chutni_store_info **out, size_t *count);
void chutni_store_info_free(chutni_store_info *infos, size_t count);

/* Absolute path of the per-user registry file that chutni_discover consults. */
chutni_status chutni_registry_path(char *buf, size_t buflen);

/* Record a store in the registry so other applications can discover it. */
chutni_status chutni_registry_add(const char *store_path);
chutni_status chutni_registry_remove(const char *store_path);

/* ---------------------------------------------------------------- lifecycle */

/* These are the storage primitives for a §40 Application Host. A host given an
 * ordinary source directory P checks the adjacent P.chutni path, obtains user
 * permission when creation is needed, calls chutni_create, then records P with
 * chutni_root_add. The library does not infer permission from a path string. */
chutni_status chutni_create(const char *path, const char *label,
                            chutni_store **out);
chutni_status chutni_open(const char *path, int read_only, chutni_store **out);
void chutni_close(chutni_store *store);

const char *chutni_store_id(const chutni_store *store);
const char *chutni_store_path(const chutni_store *store);

/* Raw manifest JSON. Unknown fields are preserved verbatim across rewrites,
 * as §9.1 requires. Caller frees with chutni_free. */
chutni_status chutni_manifest_json(chutni_store *store, char **out);

/* -------------------------------------------------------------------- roots */

/* An absent max_depth means legacy unbounded recursion, not depth zero (§11.1).
 * That distinction is load-bearing: a v0.1 store has no max_depth, and reading
 * its silence as "root only" would silently discard everything it indexed. */
#define CHUTNI_DEPTH_UNBOUNDED (-1)

typedef struct {
    int      recursive;
    int      follow_symlinks;
    int      include_hidden;
    int      retain_deleted_artifacts;
    uint64_t max_file_size_bytes;
    const char *const *exclude_globs;  /* NULL-terminated, may be NULL */
    /* Appended in ABI order after the original v0.1 fields. */

    /* §11.1. The selected root is depth 0. A directory at depth d may be
     * enumerated only when d <= max_depth; enumerating means observing that
     * directory's immediate files and child-directory names. Child directories
     * past the limit are recorded as opaque sources and MUST NOT be opened.
     *
     * The library enforces this, not the caller and not a language model. */
    int max_depth;                     /* CHUTNI_DEPTH_UNBOUNDED for legacy */

    /* §11.2. Why this memory is being built, and how thoroughly definitions
     * are expected to cover it. NULL leaves the field out of policy_json.
     *   memory_goal:     e.g. "define", "search", "archive"
     *   definition_mode: "adaptive" | "per_source"
     * Chutni standardizes what these mean for coverage accounting. It does not
     * standardize the prompt, the classifier, or the category vocabulary. */
    const char *memory_goal;
    const char *definition_mode;
} chutni_root_policy;

void chutni_root_policy_defaults(chutni_root_policy *policy);

/* Standard definition modes (§11.2). */
#define CHUTNI_DEFINITION_ADAPTIVE   "adaptive"
#define CHUTNI_DEFINITION_PER_SOURCE "per_source"

chutni_status chutni_root_add(chutni_store *store, const char *dir,
                              const char *label,
                              const chutni_root_policy *policy,
                              char root_id[CHUTNI_ID_STRLEN]);

typedef struct {
    char *root_id;
    char *path;
    char *label;       /* may be NULL */
    char *policy_json;
} chutni_root_info;

chutni_status chutni_roots_list(chutni_store *store, chutni_root_info **out,
                                size_t *count);
void chutni_root_info_free(chutni_root_info *roots, size_t count);

/* ------------------------------------------------------------------ objects */

/* Content-addressed blob store. The hash is computed over the uncompressed
 * logical payload (§14) and returned in "blake3:<hex>" form. Writing an object
 * that already exists is a no-op that still reports CHUTNI_OK. */
chutni_status chutni_object_put(chutni_store *store, const void *data,
                                size_t len, const char *media_type,
                                char hash_out[CHUTNI_HASH_STRLEN]);
chutni_status chutni_object_get(chutni_store *store, const char *hash,
                                void **data, size_t *len);

/* BLAKE3 over a file's exact bytes, in "blake3:<hex>" form. */
chutni_status chutni_hash_file(const char *path,
                               char hash_out[CHUTNI_HASH_STRLEN]);
chutni_status chutni_hash_bytes(const void *data, size_t len,
                                char hash_out[CHUTNI_HASH_STRLEN]);

/* ---------------------------------------------------------------- producers */

typedef struct {
    const char *producer_kind;   /* parser | model | application | human | pipeline | unknown */
    const char *name;
    const char *version;
    const char *model_id;
    const char *model_revision;
    const char *weights_hash;
    const char *quantization;
    const char *runtime;
    const char *app_name;
    const char *app_version;
    const char *details_json;
} chutni_producer;

/* Producers are deduplicated by their identity fields, so recording the same
 * producer repeatedly returns the same producer_id. */
chutni_status chutni_producer_put(chutni_store *store,
                                  const chutni_producer *producer,
                                  char producer_id[CHUTNI_ID_STRLEN]);

chutni_status chutni_derivation_put(chutni_store *store,
                                    const char *producer_id,
                                    const char *operation,
                                    const char *recipe_hash,
                                    const char *parameters_json,
                                    const char *input_refs_json,
                                    char derivation_id[CHUTNI_ID_STRLEN]);

/* ------------------------------------------------------------------ sources */

typedef enum {
    CHUTNI_SOURCE_PRESENT = 0,
    CHUTNI_SOURCE_MISSING,
    CHUTNI_SOURCE_DELETED,
    CHUTNI_SOURCE_UNREADABLE,
    CHUTNI_SOURCE_EXCLUDED,
    CHUTNI_SOURCE_UNSUPPORTED
} chutni_source_state;

const char *chutni_source_state_name(chutni_source_state state);

/* Insert or update the source for an absolute path, hashing its bytes. When
 * the content hash changes, artifacts derived from the old bytes are marked
 * stale, as §13.3 requires. Pass hash_file = 0 to record metadata only.
 *
 * *changed, when non-NULL, reports whether the content hash differs from what
 * the catalog already held. */
chutni_status chutni_source_put(chutni_store *store, const char *root_id,
                                const char *path, int hash_file,
                                char source_id[CHUTNI_ID_STRLEN],
                                int *changed);

chutni_status chutni_source_set_state(chutni_store *store,
                                      const char *source_id,
                                      chutni_source_state state);

/* Freshness of a source or artifact: "current", "stale", "missing", or
 * "unknown". This is a read-only observation and does not change the catalog.
 *
 * §13.3 as amended in v0.2: an artifact is current when
 *
 *     the source observation it was derived from is still current
 *     AND every required input in its derivation is still current.
 *
 * The second clause is what makes a directory definition honest. A definition
 * that read three child summaries is a claim about those summaries; if one of
 * them describes bytes that no longer exist, the definition is describing them
 * too, however untouched the directory's own listing looks. */
chutni_status chutni_check_freshness(chutni_store *store, const char *id,
                                     const char **out_state);

/* Re-hash a source's bytes and record what was found: on drift the source's
 * content_hash is updated and artifacts derived from the old bytes are marked
 * stale, so they stop being returned as current.
 *
 * Detection alone is not enough. §13.3 forbids an artifact whose source has
 * changed from staying silently active, and a checker that observes drift
 * without recording it leaves exactly that state in the store. */
chutni_status chutni_source_refresh(chutni_store *store, const char *source_id,
                                    const char **out_state);

typedef struct {
    char *source_id;
    char *display_path;
    char *media_type;
    char *content_hash;
    char *state;
    int64_t size_bytes;
    /* Appended in ABI order after the original v0.1 fields (§12.5). */
    char *source_kind;        /* "file" or "directory" */
    char *parent_source_id;   /* NULL at a root's own directory source */
    /* "enumerated" — this directory's immediate entries were observed;
     * "opaque"     — its name was observed but it was never opened (§11.1);
     * NULL         — a file, or a source predating hierarchical scanning. */
    char *observation;
    /* Depth below the root directory source; the root itself is 0. Negative
     * when the source was recorded without hierarchy information. */
    int depth;
} chutni_source_info;

/* All sources, or only those under root_id when it is non-NULL. */
chutni_status chutni_sources_list(chutni_store *store, const char *root_id,
                                  chutni_source_info **out, size_t *count);
void chutni_source_info_free(chutni_source_info *sources, size_t count);

chutni_status chutni_source_find(chutni_store *store, const char *path,
                                 char source_id[CHUTNI_ID_STRLEN]);

/* ------------------------------------------------------- directory sources
 *
 * §12.5. A directory is a source in its own right, with a stable source ID,
 * a parent, definitions, provenance, and freshness — not merely a path prefix
 * on a file's locator. `parent_source_id` is the canonical filesystem
 * hierarchy; a file's parent is the directory source that contains it.
 *
 * A directory has no bytes, so its content_hash is the hash of one observed
 * immediate listing (§13.5), which is what artifacts derived from a directory
 * bind to. An opaque directory has no listing and therefore no content_hash:
 * its name was seen, its inside was not.
 */

/* Record a directory source. `parent_source_id` may be NULL for a root's own
 * directory source. `listing_hash` binds the directory to one observed listing
 * and may be NULL to record the directory as opaque.
 *
 * Creating a directory source advertises the hierarchical capabilities in the
 * store manifest (§9.1, §35.1), because a reader must be able to tell a store
 * that records hierarchy from one that never did. */
chutni_status chutni_directory_put(chutni_store *store, const char *root_id,
                                   const char *path,
                                   const char *parent_source_id,
                                   const char *listing_hash, int depth,
                                   char source_id[CHUTNI_ID_STRLEN]);

typedef struct {
    char   *name;         /* entry name, not a path */
    char   *source_kind;  /* "file" or "directory" */
    char   *media_type;   /* NULL for directories */
    int64_t size_bytes;   /* -1 for directories */
} chutni_dir_entry;

/* Read one directory's immediate entries under `policy`, and hash them.
 *
 * Enumeration, canonicalization, and hashing live together in one function on
 * purpose. The scanner records a listing and freshness re-derives it later; if
 * those two ever disagreed about which entries a policy admits or how they
 * serialize, every directory in the store would read as permanently stale.
 *
 * `excluded` and `unsupported` count what the policy turned away and what was
 * neither a file nor a directory, so a coverage manifest can report them
 * instead of quietly rounding them off. Any out parameter may be NULL.
 * Caller frees *out with chutni_dir_entry_free. */
chutni_status chutni_read_directory(const char *dir,
                                    const chutni_root_policy *policy,
                                    chutni_dir_entry **out, size_t *count,
                                    uint64_t *excluded, uint64_t *unsupported,
                                    char hash_out[CHUTNI_HASH_STRLEN]);
void chutni_dir_entry_free(chutni_dir_entry *entries, size_t count);

/* Hash of one immediate directory listing, in "blake3:<hex>" form (§13.5).
 *
 * The listing is policy-relative: it holds exactly the entries the policy
 * admits, so changing a root's policy changes what a listing observes and
 * therefore its hash. That is the intended meaning — a different policy is a
 * different observation, not the same observation retold. */
chutni_status chutni_directory_listing_hash(const char *dir,
                                            const chutni_root_policy *policy,
                                            char hash_out[CHUTNI_HASH_STRLEN]);

/* Immediate children of a directory source, ordered by display name. */
chutni_status chutni_list_children(chutni_store *store, const char *source_id,
                                   chutni_source_info **out, size_t *count);

/* Place an existing source in the hierarchy. Used for files, whose source
 * records are created by chutni_source_put before their containing directory
 * is known. Pass depth < 0 to leave the recorded depth alone. */
chutni_status chutni_source_set_parent(chutni_store *store, const char *source_id,
                                       const char *parent_source_id, int depth);

/* A fresh UUIDv7 in canonical form, for callers that need to label something
 * — a scan generation, a batch — with an id the store will accept. */
chutni_status chutni_new_id(char id[CHUTNI_ID_STRLEN]);

/* --------------------------------------------------------------- ingestion
 *
 * Reference whole-folder scanner used by the CLI and local services. It
 * indexes only roots already authorized in the store. Text-like UTF-8 files
 * become extracted_text artifacts; other regular files receive honest
 * file_metadata artifacts rather than fabricated extraction.
 */

typedef struct {
    /* Zero selects the reference scanner's 64 MiB safety cap. */
    uint64_t max_file_size_bytes;
    /* The host application invoking the shared scanner. */
    const char *app_name;
    const char *app_version;
    /* Appended in ABI order after the original v0.1 fields.
     *
     * Overrides the depth recorded in the root's policy for this run only; the
     * stored policy is not rewritten. Leave zero to use the stored policy,
     * which is what a host should normally do. */
    int  override_max_depth;
    int  use_override_max_depth;
} chutni_scan_options;

typedef struct {
    uint64_t files_seen;
    uint64_t sources_indexed;
    uint64_t unchanged;
    uint64_t text_artifacts;
    uint64_t metadata_artifacts;
    uint64_t skipped;
    uint64_t errors;
    /* Appended in ABI order after the original v0.1 fields (§11.1, §15.7).
     * Directories are counted separately from files because a caller that
     * cannot tell them apart cannot report coverage honestly. */
    uint64_t directories_observed;     /* directory sources touched this scan */
    uint64_t directories_enumerated;   /* ... whose entries were actually read */
    uint64_t depth_limited_directories;/* ... recorded opaque at the boundary */
    uint64_t listing_artifacts;        /* directory_listing artifacts written */
    uint64_t listings_reused;          /* unchanged listings left alone */
    uint64_t files_hashed;
    uint64_t files_read;               /* contents read for extraction */
    uint64_t excluded_sources;
    uint64_t unsupported_sources;
    uint64_t sources_marked_missing;   /* only within the covered region (§24.4) */
    int      deepest_directory_enumerated;
    int      complete_for_policy;
} chutni_scan_result;

/* Scan every root recorded in the store, then rebuild disposable indexes.
 * A missing root is recorded through the source APIs on later refresh; the
 * scanner never broadens authorization beyond the catalog's roots. */
chutni_status chutni_scan(chutni_store *store,
                          const chutni_scan_options *options,
                          chutni_scan_result *result);

/* One root, so a host can refresh a single selected folder without touching
 * the rest of the store. Writes one coverage_manifest per committed scan. */
chutni_status chutni_scan_root(chutni_store *store, const char *root_id,
                               const chutni_scan_options *options,
                               chutni_scan_result *result);

/* Enumerate exactly one authorized directory and stop.
 *
 * This is the operation a host needs in order to stay in control of how far it
 * goes: it observes this directory's immediate files and child-directory names,
 * records a directory_listing, and recursively opens nothing. The host then
 * decides, child by child, whether to call this again. Nothing about the
 * decision is delegated to a model.
 *
 * The directory must already be a source in this store under an authorized
 * root (§11); observing does not broaden authorization. Reconciliation of
 * vanished children is confined to this one directory (§24.4). */
chutni_status chutni_observe_directory(chutni_store *store,
                                       const char *source_id,
                                       const chutni_scan_options *options,
                                       chutni_scan_result *result);

/* Coverage for a root, or for the root region containing a source (§15.7).
 *
 * Returns the current coverage_manifest payload plus, when `id` names a source
 * that carries one, that source's own local coverage block. This is how a
 * consumer that did not perform the scan finds out exactly what was and was
 * not inspected. Caller frees *json with chutni_free. */
chutni_status chutni_get_coverage(chutni_store *store, const char *id,
                                  char **json);

typedef struct {
    char *artifact_id;
    char *artifact_kind;
    char *artifact_origin;
    char *media_type;
    char *status;
    char *object_hash;
    char *inline_text;
    char *selector_json;
    char *source_content_hash;
    char *created_at;
    /* Producer identity resolved through the artifact's derivation, so that a
     * consumer can honor §22.2 without three more queries. */
    char *producer_name;
    char *producer_kind;
    char *model_id;
    char *model_revision;
    char *operation;
    /* Appended in ABI order after the original v0.1 fields. */
    char *language;
    char *metadata_json;
    char *supersedes_artifact_id;
    char *producer_id;
    char *producer_version;
    char *weights_hash;
    char *quantization;
    char *runtime;
    char *app_name;
    char *app_version;
    char *producer_details_json;
    char *derivation_id;
    char *recipe_hash;
    char *parameters_json;
    char *input_refs_json;
    char *derivation_created_at;
} chutni_artifact_info;

chutni_status chutni_list_artifacts(chutni_store *store, const char *source_id,
                                    chutni_artifact_info **out, size_t *count);
void chutni_artifact_info_free(chutni_artifact_info *artifacts, size_t count);

typedef struct {
    int64_t roots, sources, artifacts, objects, producers, derivations;
    int64_t artifacts_active, artifacts_stale, artifacts_superseded;
    int64_t object_bytes;
    /* Appended in ABI order after the original v0.1 fields. Files and
     * directories are reported separately because "sources: 77" tells a
     * consumer nothing about how much of a tree was actually opened. */
    int64_t sources_files, sources_directories, sources_opaque_directories;
    int64_t relations;
} chutni_counts;

chutni_status chutni_store_counts(chutni_store *store, chutni_counts *out);

typedef enum {
    CHUTNI_FORGET_CATALOG_ONLY = 0,
    CHUTNI_FORGET_ARTIFACTS,
    CHUTNI_FORGET_SECURE_LOGICAL_DELETE,
    CHUTNI_FORGET_PURGE
} chutni_forget_mode;

chutni_status chutni_forget_source(chutni_store *store, const char *source_id,
                                   chutni_forget_mode mode);

/* ---------------------------------------------------------------- artifacts */

/* v0.2 core kinds (§15.5–§15.7), on top of the §15.2 set.
 *
 * A `source_definition` on a directory MUST carry a `coverage` object in
 * metadata_json with a `stop_reason`, and chutni_artifact_put refuses it
 * otherwise. Without that, a definition written from four filenames and a
 * definition written from the whole subtree are the same record, and the
 * difference between them is the entire point. */
#define CHUTNI_KIND_DIRECTORY_LISTING "directory_listing"
#define CHUTNI_KIND_SOURCE_DEFINITION "source_definition"
#define CHUTNI_KIND_COVERAGE_MANIFEST "coverage_manifest"

/* Standard stop reasons for a definition's local coverage (§15.6). The set is
 * open; these are the ones every consumer is expected to understand. */
#define CHUTNI_STOP_MAX_DEPTH    "max_depth_reached"
#define CHUTNI_STOP_COHERENT     "producer_classified_coherent"
#define CHUTNI_STOP_BUDGET       "budget_reached"
#define CHUTNI_STOP_EXCLUDED     "excluded_by_policy"
#define CHUTNI_STOP_UNSUPPORTED  "unsupported"
#define CHUTNI_STOP_UNREADABLE   "unreadable"
#define CHUTNI_STOP_USER_CANCELED "user_canceled"

typedef struct {
    const char *source_id;
    const char *artifact_kind;    /* §15.2, §15.5–§15.7, or a namespaced kind */
    const char *artifact_origin;  /* direct | deterministic_transform | model_generated | human */
    const char *media_type;
    const char *object_hash;      /* either this ... */
    const char *inline_text;      /* ... or this MUST be set (§10 CHECK) */
    const char *selector_json;
    const char *language;
    const char *source_content_hash;
    const char *derivation_id;
    const char *supersedes_artifact_id;
    const char *metadata_json;
} chutni_artifact;

chutni_status chutni_artifact_put(chutni_store *store,
                                  const chutni_artifact *artifact,
                                  char artifact_id[CHUTNI_ID_STRLEN]);

/* Atomically record one producer, one derivation, and one or more artifacts.
 *
 * This is the generic host-ingestion primitive: PDF parsers, OCR engines,
 * multimodal models, spreadsheet readers, speech recognizers, and humans all
 * submit the same protocol records. Chutni validates structure, referential
 * integrity, source-version binding, and provenance completeness. It does not
 * judge whether submitted text, captions, summaries, or other claims are true.
 *
 * Every artifact in the batch receives the created derivation_id. Each must
 * identify an existing source and its exact current source_content_hash.
 * `artifact_ids` points to artifact_count caller-owned ID buffers.
 */
chutni_status chutni_artifacts_put(
    chutni_store *store,
    const chutni_producer *producer,
    const char *operation,
    const char *recipe_hash,
    const char *parameters_json,
    const char *input_refs_json,
    const chutni_artifact *artifacts,
    size_t artifact_count,
    char producer_id[CHUTNI_ID_STRLEN],
    char derivation_id[CHUTNI_ID_STRLEN],
    char (*artifact_ids)[CHUTNI_ID_STRLEN]);

/* ----------------------------------------------------------- relationships
 *
 * §18. `parent_source_id` is the canonical filesystem hierarchy; relations
 * carry everything else, including the hierarchy restated as `contains` for
 * consumers that traverse relations rather than columns.
 *
 * Standard v0.2 predicates, alongside the v0.1 set:
 *   directory source  --contains-->     child source
 *   definition artifact --summarizes--> source
 *   source            --defined_by-->   definition artifact
 *   source            --observed_in-->  coverage_manifest artifact
 */
#define CHUTNI_REL_CONTAINS    "contains"
#define CHUTNI_REL_SUMMARIZES  "summarizes"
#define CHUTNI_REL_DEFINED_BY  "defined_by"
#define CHUTNI_REL_OBSERVED_IN "observed_in"

/* §18 requires a model-created relation to carry a derivation ID. An
 * implementation cannot tell from the outside which relations came from a
 * model, so every relation requires one: a claim about how two things relate
 * is exactly as much of a claim as an artifact is. Both ids must already name
 * a source or an artifact in this store. */
chutni_status chutni_relation_put(chutni_store *store, const char *from_id,
                                  const char *predicate, const char *to_id,
                                  const char *derivation_id,
                                  const char *metadata_json,
                                  char relation_id[CHUTNI_ID_STRLEN]);

typedef struct {
    char *relation_id;
    char *from_id;
    char *predicate;
    char *to_id;
    char *derivation_id;
    char *created_at;
    char *metadata_json;
} chutni_relation_info;

/* Relations out of `from_id`, optionally filtered to one predicate. Pass
 * from_id = NULL to list every relation with that predicate. */
chutni_status chutni_relations_list(chutni_store *store, const char *from_id,
                                    const char *predicate,
                                    chutni_relation_info **out, size_t *count);
void chutni_relation_info_free(chutni_relation_info *relations, size_t count);

/* ----------------------------------------------------------- representations
 *
 * Acceleration data derived from an artifact (§17): embeddings, token IDs,
 * projected vision tokens. Representations are disposable by design (§6.3) —
 * they mean nothing except to a consumer that understands the exact model and
 * preprocessing that produced them, and MUST be regenerated rather than
 * reinterpreted when that profile does not match.
 *
 * Chutni does not compute embeddings. A producer supplies the vector.
 */

typedef struct {
    /* §17.2 requires generic visual embeddings and VLM-projected embeddings to
     * use different kinds; §17.3 says the same of token IDs across tokenizers. */
    const char *representation_kind;   /* e.g. "text_embedding" */
    const char *model_id;
    const char *model_revision;
    int         dimensions;
    const char *dtype;                 /* "f32" is the only value v0.1 stores */
    const char *normalization;         /* "none" or "l2" */
    const char *tokenizer_hash;        /* optional; see §17.3 */
    const char *projector_hash;        /* optional; see §17.4 */
} chutni_representation_profile;

/* Store a vector against an artifact. The vector is serialized as a
 * content-addressed object, so identical vectors share storage.
 *
 * The artifact's current payload is hashed into source_artifact_hash, which is
 * what later makes it possible to tell that a representation describes text the
 * artifact no longer holds. */
chutni_status chutni_representation_put(chutni_store *store,
                                        const char *artifact_id,
                                        const chutni_representation_profile *profile,
                                        const float *vector, size_t dimensions,
                                        char representation_id[CHUTNI_ID_STRLEN]);

/* Read a vector back. `accepted` declares the profile the caller can actually
 * use; a representation that does not match it is refused with
 * CHUTNI_ERR_DENIED rather than returned.
 *
 * This is deliberately not a convenience check the caller may skip. §22.6
 * forbids a consumer from using incompatible embeddings, and comparing vectors
 * from two different models produces confident nonsense rather than an error,
 * so the API will not hand them over without being told what is usable.
 *
 * *vector is heap memory owned by the caller; free with chutni_free. */
chutni_status chutni_representation_get(chutni_store *store,
                                        const char *representation_id,
                                        const chutni_representation_profile *accepted,
                                        float **vector, size_t *dimensions);

typedef struct {
    char *representation_id;
    char *artifact_id;
    char *representation_kind;
    char *model_id;
    char *model_revision;
    char *dtype;
    char *normalization;
    int   dimensions;
    /* Nonzero when the artifact's payload still hashes to what this
     * representation was built from. A zero here means regenerate, not
     * reinterpret. */
    int   compatible_with_artifact;
} chutni_representation_info;

chutni_status chutni_representations_list(chutni_store *store,
                                          const char *artifact_id,
                                          chutni_representation_info **out,
                                          size_t *count);
void chutni_representation_info_free(chutni_representation_info *reps, size_t count);

/* ------------------------------------------------------------------- search */

typedef struct {
    const char *query;
    const char *const *artifact_kinds;  /* NULL-terminated filter, may be NULL */
    const char *const *media_types;     /* NULL-terminated filter, may be NULL */
    int limit;
    int include_stale;                  /* default 0: only active artifacts (§15.4) */
    /* Default 0 preserves precise all-term matching. When true, literal terms
     * are joined with OR; useful when a host derives a query from prose. */
    int match_any;
} chutni_search_request;

typedef struct {
    char  *source_id;
    char  *artifact_id;
    char  *display_path;
    char  *artifact_kind;
    char  *snippet;
    char  *producer_id;
    char  *selector_json;
    /* "current", "stale", "missing", "unverified", or "unknown".
     *
     * "unverified" means the catalog considers the artifact current but the
     * file's size or mtime no longer match what the scan recorded, so that
     * judgement rests on catalog state the disk contradicts. Search does not
     * re-hash — that is what chutni_check_freshness is for — and a stat can
     * withdraw a claim of currency but never establish one (§13.2). Treat
     * "unverified" as "reopen the source before quoting it". */
    char  *freshness;
    double score;
    char  *score_type;
    /* Appended in ABI order after the original v0.1 fields (§19.3).
     *
     * A hit inside a bounded scan is not a hit inside an exhaustive index, and
     * a consumer has no way to tell the two apart from a path and a snippet.
     * These fields are how it finds out: what kind of thing matched, where it
     * sits in the hierarchy, and which coverage manifest governs the region it
     * came from. */
    char *source_kind;           /* "file" or "directory" */
    char *parent_source_id;      /* may be NULL */
    char *coverage_manifest_id;  /* NULL when the region has no manifest */
    int   depth;                 /* negative when unknown */
} chutni_search_result;

chutni_status chutni_search(chutni_store *store,
                            const chutni_search_request *request,
                            chutni_search_result **out, size_t *count);

typedef struct {
    const float *vector;                /* the query embedding */
    size_t       dimensions;
    /* The profile the caller can use. Only representations matching it are
     * compared; the rest are invisible rather than silently mixed in (§22.6). */
    const chutni_representation_profile *profile;
    const char *const *artifact_kinds;  /* NULL-terminated filter, may be NULL */
    int limit;
    int include_stale;
} chutni_semantic_request;

/* Brute-force cosine similarity over stored vectors. There is no approximate
 * index in v0.1: every matching representation is loaded and scored, which is
 * correct and linear. score_type is reported as "cosine_bruteforce", and §19.3
 * forbids comparing it against another implementation's scores. */
chutni_status chutni_search_semantic(chutni_store *store,
                                     const chutni_semantic_request *request,
                                     chutni_search_result **out, size_t *count);

void chutni_search_result_free(chutni_search_result *results, size_t count);

/* Rebuild everything under indexes/ from the catalog and object store (§8.4). */
chutni_status chutni_rebuild_indexes(chutni_store *store);

/* Free memory returned by APIs documented as using it. */
void chutni_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* CHUTNI_H */
