/* Chutni — portable, source-backed memory for local files.
 *
 * Reference implementation of the Chutni protocol, specification version 0.1.
 * See SPEC.md. This header is the stable C ABI; language bindings sit on top
 * of it rather than under it.
 *
 * Threading: a chutni_store handle is not thread-safe. Open one handle per
 * thread, or serialize access. Multiple processes may hold read handles on the
 * same store concurrently; SQLite WAL mode arbitrates writers.
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

#define CHUTNI_SPEC_VERSION "0.1"

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
    CHUTNI_ERR_READONLY = -10
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

typedef struct {
    int      recursive;
    int      follow_symlinks;
    int      include_hidden;
    int      retain_deleted_artifacts;
    uint64_t max_file_size_bytes;
    const char *const *exclude_globs;  /* NULL-terminated, may be NULL */
} chutni_root_policy;

void chutni_root_policy_defaults(chutni_root_policy *policy);

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
 * "unknown". This is a read-only observation and does not change the catalog. */
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
} chutni_source_info;

/* All sources, or only those under root_id when it is non-NULL. */
chutni_status chutni_sources_list(chutni_store *store, const char *root_id,
                                  chutni_source_info **out, size_t *count);
void chutni_source_info_free(chutni_source_info *sources, size_t count);

chutni_status chutni_source_find(chutni_store *store, const char *path,
                                 char source_id[CHUTNI_ID_STRLEN]);

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
} chutni_scan_options;

typedef struct {
    uint64_t files_seen;
    uint64_t sources_indexed;
    uint64_t unchanged;
    uint64_t text_artifacts;
    uint64_t metadata_artifacts;
    uint64_t skipped;
    uint64_t errors;
} chutni_scan_result;

/* Scan every root recorded in the store, then rebuild disposable indexes.
 * A missing root is recorded through the source APIs on later refresh; the
 * scanner never broadens authorization beyond the catalog's roots. */
chutni_status chutni_scan(chutni_store *store,
                          const chutni_scan_options *options,
                          chutni_scan_result *result);

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
} chutni_artifact_info;

chutni_status chutni_list_artifacts(chutni_store *store, const char *source_id,
                                    chutni_artifact_info **out, size_t *count);
void chutni_artifact_info_free(chutni_artifact_info *artifacts, size_t count);

typedef struct {
    int64_t roots, sources, artifacts, objects, producers, derivations;
    int64_t artifacts_active, artifacts_stale, artifacts_superseded;
    int64_t object_bytes;
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

typedef struct {
    const char *source_id;
    const char *artifact_kind;    /* §15.2, or a namespaced kind */
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
