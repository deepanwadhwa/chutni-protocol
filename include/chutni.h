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
 * computer before deciding to create it. See SPEC.md §39.
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
    char  *freshness;
    double score;
    char  *score_type;
} chutni_search_result;

chutni_status chutni_search(chutni_store *store,
                            const chutni_search_request *request,
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
