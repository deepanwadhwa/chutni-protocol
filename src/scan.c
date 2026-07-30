/* Chutni reference whole-folder ingestion.
 *
 * Kept in libchutni so the CLI, MCP server, and native hosts all create the
 * same sources, artifacts, and provenance instead of growing private scanners.
 *
 * v0.2: the walk is hierarchical and bounded. Directories are sources, depth
 * is enforced here rather than trusted to a caller, and every committed scan
 * records a coverage manifest saying what it actually reached.
 *
 * This scanner recognizes no semantic categories. It does not know what an
 * application bundle, a photo library, or a test suite is, and it must not
 * learn: that classification is a producer's claim and belongs in a
 * source_definition with provenance attached, not in the deterministic layer
 * that everyone else's trust rests on.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "chutni.h"
#include "cj.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* The implementation's release version comes from the VERSION file via the
 * Makefile. The fallback keeps a hand-rolled compile working without it, and says
 * plainly that it does not know the version rather than naming one it cannot
 * vouch for — this string ends up in producer records (§16.1). */
#ifndef CHUTNI_VERSION
#define CHUTNI_VERSION "0.0.0-unversioned"
#endif

#define REFERENCE_SCANNER_VERSION CHUTNI_VERSION
#define DEFAULT_MAX_FILE_BYTES (64ull * 1024ull * 1024ull)
#define MAX_WALK_DEPTH 64

typedef struct {
    chutni_store *store;
    const char *root_id;
    chutni_root_policy policy;
    int max_depth;                    /* effective, after recursive/override */
    char derivation_text[CHUTNI_ID_STRLEN];
    char derivation_meta[CHUTNI_ID_STRLEN];
    char derivation_listing[CHUTNI_ID_STRLEN];
    char derivation_coverage[CHUTNI_ID_STRLEN];
    chutni_scan_result *result;
    uint64_t max_bytes;
    chutni_scan_progress_callback progress_callback;
    void *progress_userdata;
} scan_context;

static int looks_texty(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    static const char *exts[] = {
        ".txt", ".md", ".markdown", ".json", ".csv", ".tsv", ".log", ".rst",
        ".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".rs", ".js", ".ts",
        ".go", ".rb", ".sh", ".yaml", ".yml", ".toml", ".ini", ".html",
        ".htm", ".xml", ".tex", NULL
    };
    for (const char **e = exts; *e; e++)
        if (!strcasecmp(ext, *e)) return 1;
    return 0;
}

/* §11.1. Absent means unbounded, which is the v0.1 behavior; reading a missing
   field as depth zero would silently discard everything an existing store
   indexed. `recursive: false` is depth zero said the older way. */
static int effective_max_depth(const char *policy_json,
                               const chutni_scan_options *options,
                               chutni_root_policy *policy_out) {
    chutni_root_policy_defaults(policy_out);
    int depth = CHUTNI_DEPTH_UNBOUNDED;
    cj *policy = policy_json ? cj_parse(policy_json, NULL) : NULL;
    if (policy && policy->type == CJ_OBJ) {
        cj *v = cj_get(policy, "recursive");
        if (v && v->type == CJ_BOOL) policy_out->recursive = v->bval;
        v = cj_get(policy, "follow_symlinks");
        if (v && v->type == CJ_BOOL) policy_out->follow_symlinks = v->bval;
        v = cj_get(policy, "include_hidden");
        if (v && v->type == CJ_BOOL) policy_out->include_hidden = v->bval;
        v = cj_get(policy, "max_depth");
        if (v && v->type == CJ_NUM) depth = (int)v->num;
    }
    cj_free(policy);
    if (!policy_out->recursive) depth = 0;
    if (options && options->use_override_max_depth)
        depth = options->override_max_depth;
    policy_out->max_depth = depth;
    return depth;
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

/* ------------------------------------------------------------------- files */

/* The caller places the resulting source in the hierarchy, because
   chutni_source_put keys on the path and does not know the containing
   directory's source id until observe() has created it. */
static void scan_file(scan_context *sc, const char *path, int depth,
                      int64_t size_bytes, char source_id[CHUTNI_ID_STRLEN]) {
    source_id[0] = 0;
    sc->result->files_seen++;

    if ((uint64_t)size_bytes > sc->max_bytes) {
        /* Recorded rather than skipped in silence. Under definition_mode
           per_source every reached file needs an artifact or an explicit
           status, and "too large to read" is a status. */
        if (chutni_source_put(sc->store, sc->root_id, path, 0, source_id, NULL)
                == CHUTNI_OK) {
            chutni_source_set_state(sc->store, source_id, CHUTNI_SOURCE_EXCLUDED);
            sc->result->sources_indexed++;
        } else {
            sc->result->errors++;
        }
        sc->result->skipped++;
        goto done;
    }

    int changed = 0;
    if (chutni_source_put(sc->store, sc->root_id, path, 1, source_id,
                          &changed) != CHUTNI_OK) {
        sc->result->errors++;
        goto done;
    }
    sc->result->sources_indexed++;
    sc->result->files_hashed++;

    char content_hash[CHUTNI_HASH_STRLEN];
    if (chutni_hash_file(path, content_hash) != CHUTNI_OK) {
        sc->result->errors++;
        goto done;
    }

    int need_metadata = 1;
    int need_text = looks_texty(path) && size_bytes > 0;
    if (!changed) {
        sc->result->unchanged++;
        chutni_artifact_info *existing = NULL;
        size_t existing_count = 0;
        if (chutni_list_artifacts(sc->store, source_id, &existing,
                                  &existing_count) == CHUTNI_OK) {
            for (size_t i = 0; i < existing_count; i++) {
                if (!existing[i].status ||
                    strcmp(existing[i].status, "active") ||
                    !existing[i].source_content_hash ||
                    strcmp(existing[i].source_content_hash, content_hash))
                    continue;
                if (existing[i].artifact_kind &&
                    !strcmp(existing[i].artifact_kind, "file_metadata"))
                    need_metadata = 0;
                if (existing[i].artifact_kind &&
                    !strcmp(existing[i].artifact_kind, "extracted_text"))
                    need_text = 0;
            }
        }
        chutni_artifact_info_free(existing, existing_count);
        if (!need_metadata && !need_text) goto done;
    }

    char artifact_id[CHUTNI_ID_STRLEN];
    if (need_metadata) {
        char metadata[256];
        snprintf(metadata, sizeof metadata,
                 "{\"size_bytes\":%lld,\"depth\":%d}",
                 (long long)size_bytes, depth);
        chutni_artifact artifact;
        memset(&artifact, 0, sizeof artifact);
        artifact.source_id = source_id;
        artifact.source_content_hash = content_hash;
        artifact.artifact_kind = "file_metadata";
        artifact.artifact_origin = "direct";
        artifact.media_type = "application/json";
        artifact.inline_text = metadata;
        artifact.derivation_id = sc->derivation_meta;
        if (chutni_artifact_put(sc->store, &artifact, artifact_id) == CHUTNI_OK)
            sc->result->metadata_artifacts++;
        else
            sc->result->errors++;
    }

    if (!need_text) goto done;

    char *text = NULL;
    FILE *file = fopen(path, "rb");
    if (file) {
        text = malloc((size_t)size_bytes + 1);
        if (text) {
            size_t got = fread(text, 1, (size_t)size_bytes, file);
            text[got] = 0;
            if (memchr(text, 0, got)) {
                free(text);
                text = NULL;
            }
        }
        fclose(file);
    }

    if (text) {
        sc->result->files_read++;
        chutni_artifact artifact;
        memset(&artifact, 0, sizeof artifact);
        artifact.source_id = source_id;
        artifact.source_content_hash = content_hash;
        artifact.artifact_kind = "extracted_text";
        artifact.artifact_origin = "deterministic_transform";
        artifact.media_type = "text/plain; charset=utf-8";
        artifact.inline_text = text;
        artifact.derivation_id = sc->derivation_text;
        if (chutni_artifact_put(sc->store, &artifact, artifact_id) == CHUTNI_OK)
            sc->result->text_artifacts++;
        else
            sc->result->errors++;
        free(text);
    }
done:
    if (sc->progress_callback)
        sc->progress_callback(sc->result, path, sc->progress_userdata);
}

/* ------------------------------------------------------------- directories */

/* §24.4. Absence means something only inside the region this scan actually
   covered. A depth-zero refresh may notice that a direct child is gone; it
   knows nothing about grandchildren it never looked at, and marking those
   missing would be inventing an observation. Reconciliation therefore runs
   per enumerated directory and considers only that directory's own children. */
static void reconcile_children(scan_context *sc, const char *dir_source_id,
                               const char *dir_path,
                               const chutni_dir_entry *entries, size_t count) {
    chutni_source_info *known = NULL;
    size_t known_count = 0;
    if (chutni_list_children(sc->store, dir_source_id, &known, &known_count)
            != CHUTNI_OK)
        return;

    for (size_t i = 0; i < known_count; i++) {
        if (!known[i].display_path || !known[i].source_id) continue;
        const char *name = basename_of(known[i].display_path);
        int still_here = 0;
        for (size_t e = 0; e < count; e++)
            if (!strcmp(entries[e].name, name)) { still_here = 1; break; }
        if (still_here) continue;

        /* Not in the listing is not the same as gone: the policy may have
           stopped admitting it, or it may have become something that is
           neither a file nor a directory. Say which. */
        char full[PATH_MAX];
        struct stat st;
        chutni_source_state state = CHUTNI_SOURCE_MISSING;
        if ((size_t)snprintf(full, sizeof full, "%s/%s", dir_path, name) < sizeof full &&
            lstat(full, &st) == 0)
            state = (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
                        ? CHUTNI_SOURCE_EXCLUDED
                        : CHUTNI_SOURCE_UNSUPPORTED;

        if (known[i].state && !strcmp(known[i].state,
                                      chutni_source_state_name(state)))
            continue;
        if (chutni_source_set_state(sc->store, known[i].source_id, state) == CHUTNI_OK &&
            state == CHUTNI_SOURCE_MISSING)
            sc->result->sources_marked_missing++;
    }
    chutni_source_info_free(known, known_count);
}

static void write_listing_artifact(scan_context *sc, const char *dir_source_id,
                                   const char *listing_hash,
                                   const chutni_dir_entry *entries,
                                   char **child_ids, size_t count) {
    /* An unchanged listing is the same observation, not a new one. Rewriting
       it every scan would churn objects and reset created_at on a fact that
       did not change. */
    chutni_artifact_info *existing = NULL;
    size_t existing_count = 0;
    int already = 0;
    if (chutni_list_artifacts(sc->store, dir_source_id, &existing,
                              &existing_count) == CHUTNI_OK) {
        for (size_t i = 0; i < existing_count; i++)
            if (existing[i].artifact_kind &&
                !strcmp(existing[i].artifact_kind, CHUTNI_KIND_DIRECTORY_LISTING) &&
                existing[i].status && !strcmp(existing[i].status, "active") &&
                existing[i].source_content_hash &&
                !strcmp(existing[i].source_content_hash, listing_hash))
                already = 1;
    }
    chutni_artifact_info_free(existing, existing_count);
    if (already) { sc->result->listings_reused++; return; }

    cj *payload = cj_obj();
    cj *array = cj_arr();
    for (size_t i = 0; i < count; i++) {
        cj *entry = cj_obj();
        if (child_ids[i]) cj_set(entry, "source_id", cj_str(child_ids[i]));
        cj_set(entry, "name", cj_str(entries[i].name));
        cj_set(entry, "source_kind", cj_str(entries[i].source_kind));
        if (entries[i].media_type)
            cj_set(entry, "media_type", cj_str(entries[i].media_type));
        if (entries[i].size_bytes >= 0)
            cj_set(entry, "size_bytes", cj_num((double)entries[i].size_bytes));
        cj_push(array, entry);
    }
    cj_set(payload, "entries", array);
    cj_set(payload, "listing_hash", cj_str(listing_hash));
    /* §15.5: this describes one immediate listing and implies nothing about
       what is inside the directories it names. */
    cj_set(payload, "immediate_only", cj_bool(1));
    char *text = cj_dump(payload, -1);
    cj_free(payload);
    if (!text) { sc->result->errors++; return; }

    char object_hash[CHUTNI_HASH_STRLEN];
    chutni_status status = chutni_object_put(sc->store, text, strlen(text),
                                             "application/json", object_hash);
    free(text);
    if (status != CHUTNI_OK) { sc->result->errors++; return; }

    chutni_artifact artifact;
    memset(&artifact, 0, sizeof artifact);
    artifact.source_id = dir_source_id;
    artifact.artifact_kind = CHUTNI_KIND_DIRECTORY_LISTING;
    artifact.artifact_origin = "deterministic_transform";
    artifact.media_type = "application/json";
    artifact.object_hash = object_hash;
    artifact.source_content_hash = listing_hash;
    artifact.derivation_id = sc->derivation_listing;
    char artifact_id[CHUTNI_ID_STRLEN];
    if (chutni_artifact_put(sc->store, &artifact, artifact_id) == CHUTNI_OK)
        sc->result->listing_artifacts++;
    else
        sc->result->errors++;
}

static void observe(scan_context *sc, const char *dir_path,
                    const char *parent_source_id, int depth, int may_expand,
                    char dir_source_id[CHUTNI_ID_STRLEN]);

/* Record a child directory. Enumerated when depth allows and the caller is
   expanding; recorded as an opaque source otherwise — its name was observed,
   its inside was not, and §11.1 forbids opening it to find out. */
static void handle_child_directory(scan_context *sc, const char *path,
                                   const char *parent_source_id, int depth,
                                   int may_expand,
                                   char child_id[CHUTNI_ID_STRLEN]) {
    child_id[0] = 0;
    int within_depth = sc->max_depth == CHUTNI_DEPTH_UNBOUNDED ||
                       depth <= sc->max_depth;
    if (may_expand && within_depth && depth < MAX_WALK_DEPTH) {
        observe(sc, path, parent_source_id, depth, 1, child_id);
        return;
    }
    sc->result->directories_observed++;
    sc->result->depth_limited_directories++;
    if (chutni_directory_put(sc->store, sc->root_id, path, parent_source_id,
                             NULL, depth, child_id) != CHUTNI_OK)
        sc->result->errors++;
}

static void observe(scan_context *sc, const char *dir_path,
                    const char *parent_source_id, int depth, int may_expand,
                    char dir_source_id[CHUTNI_ID_STRLEN]) {
    dir_source_id[0] = 0;

    chutni_dir_entry *entries = NULL;
    size_t count = 0;
    uint64_t excluded = 0, unsupported = 0;
    char listing_hash[CHUTNI_HASH_STRLEN];
    if (chutni_read_directory(dir_path, &sc->policy, &entries, &count,
                              &excluded, &unsupported, listing_hash) != CHUTNI_OK) {
        sc->result->errors++;
        /* Unreadable is still an observation worth recording: the directory
           exists and we could not open it. */
        chutni_directory_put(sc->store, sc->root_id, dir_path, parent_source_id,
                             NULL, depth, dir_source_id);
        if (dir_source_id[0])
            chutni_source_set_state(sc->store, dir_source_id,
                                    CHUTNI_SOURCE_UNREADABLE);
        return;
    }
    sc->result->excluded_sources += excluded;
    sc->result->unsupported_sources += unsupported;
    sc->result->directories_observed++;
    sc->result->directories_enumerated++;
    if (depth > sc->result->deepest_directory_enumerated)
        sc->result->deepest_directory_enumerated = depth;

    if (chutni_directory_put(sc->store, sc->root_id, dir_path, parent_source_id,
                             listing_hash, depth, dir_source_id) != CHUTNI_OK) {
        sc->result->errors++;
        chutni_dir_entry_free(entries, count);
        return;
    }

    char **child_ids = count ? calloc(count, sizeof *child_ids) : NULL;
    if (count && !child_ids) {
        sc->result->errors++;
        chutni_dir_entry_free(entries, count);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        char full[PATH_MAX];
        if ((size_t)snprintf(full, sizeof full, "%s/%s", dir_path,
                             entries[i].name) >= sizeof full) {
            sc->result->errors++;
            continue;
        }
        char child_id[CHUTNI_ID_STRLEN];
        if (!strcmp(entries[i].source_kind, "directory"))
            handle_child_directory(sc, full, dir_source_id, depth + 1,
                                   may_expand, child_id);
        else
            scan_file(sc, full, depth + 1, entries[i].size_bytes, child_id);
        if (!child_id[0]) continue;
        child_ids[i] = strdup(child_id);
        /* A file's source record is created before its containing directory is
           known, so its place in the hierarchy is set here; a directory got
           its parent from chutni_directory_put. */
        if (strcmp(entries[i].source_kind, "directory"))
            chutni_source_set_parent(sc->store, child_id, dir_source_id, depth + 1);
        char relation_id[CHUTNI_ID_STRLEN];
        chutni_relation_put(sc->store, dir_source_id, CHUTNI_REL_CONTAINS,
                            child_id, sc->derivation_listing, NULL, relation_id);
    }

    write_listing_artifact(sc, dir_source_id, listing_hash, entries,
                           child_ids, count);
    reconcile_children(sc, dir_source_id, dir_path, entries, count);

    for (size_t i = 0; i < count; i++) free(child_ids[i]);
    free(child_ids);
    chutni_dir_entry_free(entries, count);
}

/* ------------------------------------------------------- coverage manifest */

/* Counted from the store rather than from this run's tally, because
   definitions are producer work that may have happened between scans. What the
   manifest reports is the coverage the store actually holds right now. */
static void count_definitions(chutni_store *store, const char *root_id,
                              uint64_t *dirs_defined, uint64_t *files_defined,
                              uint64_t *dirs_collapsed) {
    *dirs_defined = *files_defined = *dirs_collapsed = 0;
    chutni_source_info *sources = NULL;
    size_t count = 0;
    if (chutni_sources_list(store, root_id, &sources, &count) != CHUTNI_OK)
        return;

    char *defined = calloc(count ? count : 1, 1);
    if (!defined) { chutni_source_info_free(sources, count); return; }

    for (size_t i = 0; i < count; i++) {
        chutni_artifact_info *artifacts = NULL;
        size_t n = 0;
        if (chutni_list_artifacts(store, sources[i].source_id, &artifacts, &n)
                != CHUTNI_OK)
            continue;
        for (size_t a = 0; a < n; a++)
            if (artifacts[a].artifact_kind &&
                !strcmp(artifacts[a].artifact_kind, CHUTNI_KIND_SOURCE_DEFINITION) &&
                artifacts[a].status && !strcmp(artifacts[a].status, "active"))
                defined[i] = 1;
        chutni_artifact_info_free(artifacts, n);
        if (!defined[i]) continue;
        if (sources[i].source_kind && !strcmp(sources[i].source_kind, "directory"))
            (*dirs_defined)++;
        else
            (*files_defined)++;
    }

    /* A directory inside a defined subtree that has no definition of its own is
       represented by an ancestor's — collapsed, in the adaptive sense of §11.2,
       rather than undescribed. */
    for (size_t i = 0; i < count; i++) {
        if (defined[i]) continue;
        if (!sources[i].source_kind || strcmp(sources[i].source_kind, "directory"))
            continue;
        const char *parent = sources[i].parent_source_id;
        for (int hop = 0; parent && hop < MAX_WALK_DEPTH; hop++) {
            size_t found = count;
            for (size_t j = 0; j < count; j++)
                if (sources[j].source_id && !strcmp(sources[j].source_id, parent)) {
                    found = j;
                    break;
                }
            if (found == count) break;
            if (defined[found]) { (*dirs_collapsed)++; break; }
            parent = sources[found].parent_source_id;
        }
    }

    free(defined);
    chutni_source_info_free(sources, count);
}

static void write_coverage_manifest(scan_context *sc, const char *root_source_id,
                                    const char *root_path,
                                    const char *scan_generation) {
    char listing_hash[CHUTNI_HASH_STRLEN];
    if (chutni_directory_listing_hash(root_path, &sc->policy, listing_hash)
            != CHUTNI_OK) {
        sc->result->errors++;
        return;
    }

    uint64_t dirs_defined = 0, files_defined = 0, dirs_collapsed = 0;
    count_definitions(sc->store, sc->root_id, &dirs_defined, &files_defined,
                      &dirs_collapsed);

    /* "Complete for policy" means the bounded operation the policy asked for
       ran to completion. It does not mean the subtree was read, and §15.7
       requires consumers to be told the difference. */
    sc->result->complete_for_policy = sc->result->errors == 0;

    cj *payload = cj_obj();
    cj_set(payload, "scan_generation", cj_str(scan_generation));
    cj_set(payload, "root_source_id", cj_str(root_source_id));

    cj *policy = cj_obj();
    cj_set(policy, "max_depth", sc->max_depth < 0 ? cj_null()
                                                  : cj_num((double)sc->max_depth));
    cj_set(policy, "recursive", cj_bool(sc->policy.recursive));
    cj_set(policy, "follow_symlinks", cj_bool(sc->policy.follow_symlinks));
    cj_set(policy, "include_hidden", cj_bool(sc->policy.include_hidden));
    {
        chutni_root_info *roots = NULL;
        size_t n = 0;
        if (chutni_roots_list(sc->store, &roots, &n) == CHUTNI_OK) {
            for (size_t i = 0; i < n; i++) {
                if (!roots[i].root_id || strcmp(roots[i].root_id, sc->root_id) ||
                    !roots[i].policy_json)
                    continue;
                cj *stored = cj_parse(roots[i].policy_json, NULL);
                const char *goal = cj_get_str(stored, "memory_goal");
                const char *mode = cj_get_str(stored, "definition_mode");
                if (goal) cj_set(policy, "memory_goal", cj_str(goal));
                if (mode) cj_set(policy, "definition_mode", cj_str(mode));
                cj_free(stored);
            }
            chutni_root_info_free(roots, n);
        }
    }
    cj_set(payload, "policy", policy);

    cj *coverage = cj_obj();
    cj_set(coverage, "deepest_directory_enumerated",
           cj_num((double)sc->result->deepest_directory_enumerated));
    cj_set(coverage, "directories_observed",
           cj_num((double)sc->result->directories_observed));
    cj_set(coverage, "directories_enumerated",
           cj_num((double)sc->result->directories_enumerated));
    cj_set(coverage, "directories_defined", cj_num((double)dirs_defined));
    cj_set(coverage, "directories_collapsed", cj_num((double)dirs_collapsed));
    cj_set(coverage, "files_observed", cj_num((double)sc->result->files_seen));
    cj_set(coverage, "files_hashed", cj_num((double)sc->result->files_hashed));
    cj_set(coverage, "files_read", cj_num((double)sc->result->files_read));
    cj_set(coverage, "files_defined", cj_num((double)files_defined));
    cj_set(coverage, "depth_limited_directories",
           cj_num((double)sc->result->depth_limited_directories));
    cj_set(coverage, "excluded_sources",
           cj_num((double)(sc->result->excluded_sources + sc->result->skipped)));
    cj_set(coverage, "unsupported_sources",
           cj_num((double)sc->result->unsupported_sources));
    cj_set(coverage, "sources_marked_missing",
           cj_num((double)sc->result->sources_marked_missing));
    cj_set(coverage, "errors", cj_num((double)sc->result->errors));
    cj_set(payload, "coverage", coverage);
    cj_set(payload, "complete_for_policy",
           cj_bool(sc->result->complete_for_policy));

    char *text = cj_dump(payload, -1);
    cj_free(payload);
    if (!text) { sc->result->errors++; return; }

    char object_hash[CHUTNI_HASH_STRLEN];
    chutni_status status = chutni_object_put(sc->store, text, strlen(text),
                                             "application/json", object_hash);
    free(text);
    if (status != CHUTNI_OK) { sc->result->errors++; return; }

    /* The previous generation's manifest is superseded, not deleted: how far a
       past scan reached is part of the store's history (§23). */
    char previous[CHUTNI_ID_STRLEN] = "";
    chutni_artifact_info *existing = NULL;
    size_t existing_count = 0;
    if (chutni_list_artifacts(sc->store, root_source_id, &existing,
                              &existing_count) == CHUTNI_OK)
        for (size_t i = 0; i < existing_count; i++)
            if (existing[i].artifact_kind &&
                !strcmp(existing[i].artifact_kind, CHUTNI_KIND_COVERAGE_MANIFEST) &&
                existing[i].status && !strcmp(existing[i].status, "active") &&
                existing[i].artifact_id)
                snprintf(previous, sizeof previous, "%s", existing[i].artifact_id);
    chutni_artifact_info_free(existing, existing_count);

    chutni_artifact artifact;
    memset(&artifact, 0, sizeof artifact);
    artifact.source_id = root_source_id;
    artifact.artifact_kind = CHUTNI_KIND_COVERAGE_MANIFEST;
    artifact.artifact_origin = "deterministic_transform";
    artifact.media_type = "application/json";
    artifact.object_hash = object_hash;
    artifact.source_content_hash = listing_hash;
    artifact.derivation_id = sc->derivation_coverage;
    artifact.supersedes_artifact_id = previous[0] ? previous : NULL;
    char artifact_id[CHUTNI_ID_STRLEN];
    if (chutni_artifact_put(sc->store, &artifact, artifact_id) != CHUTNI_OK) {
        sc->result->errors++;
        return;
    }

    /* §18: which sources this generation actually looked at. Recorded for
       directories, which is what bounds the region; a file's membership is
       already carried by its parent's `contains` edge. */
    chutni_source_info *sources = NULL;
    size_t source_count = 0;
    if (chutni_sources_list(sc->store, sc->root_id, &sources, &source_count)
            == CHUTNI_OK) {
        for (size_t i = 0; i < source_count; i++) {
            if (!sources[i].source_kind ||
                strcmp(sources[i].source_kind, "directory"))
                continue;
            char relation_id[CHUTNI_ID_STRLEN];
            chutni_relation_put(sc->store, sources[i].source_id,
                                CHUTNI_REL_OBSERVED_IN, artifact_id,
                                sc->derivation_coverage, NULL, relation_id);
        }
        chutni_source_info_free(sources, source_count);
    }
}

/* ------------------------------------------------------------ entry points */

static chutni_status prepare_producer(chutni_store *store,
                                      const chutni_scan_options *options,
                                      scan_context *sc) {
    const char *app_name =
        options && options->app_name ? options->app_name : "chutni";
    const char *app_version =
        options && options->app_version ? options->app_version
                                        : REFERENCE_SCANNER_VERSION;
    chutni_producer producer;
    memset(&producer, 0, sizeof producer);
    producer.producer_kind = "parser";
    producer.name = "chutni-reference-scanner";
    producer.version = REFERENCE_SCANNER_VERSION;
    producer.app_name = app_name;
    producer.app_version = app_version;
    char producer_id[CHUTNI_ID_STRLEN];
    chutni_status status = chutni_producer_put(store, &producer, producer_id);
    if (status != CHUTNI_OK) return status;
    status = chutni_derivation_put(store, producer_id, "extract_text", NULL,
                                   "{\"strategy\":\"whole_file_utf8\"}", "[]",
                                   sc->derivation_text);
    if (status != CHUTNI_OK) return status;
    status = chutni_derivation_put(store, producer_id, "record_file_metadata", NULL,
                                   "{\"fields\":[\"size_bytes\",\"depth\"]}", "[]",
                                   sc->derivation_meta);
    if (status != CHUTNI_OK) return status;
    status = chutni_derivation_put(store, producer_id, "observe_directory", NULL,
                                   "{\"scope\":\"immediate_entries\"}", "[]",
                                   sc->derivation_listing);
    if (status != CHUTNI_OK) return status;
    return chutni_derivation_put(store, producer_id, "record_coverage", NULL,
                                 "{\"scope\":\"scan_generation\"}", "[]",
                                 sc->derivation_coverage);
}

static chutni_status scan_one_root(chutni_store *store, const chutni_root_info *root,
                                   const chutni_scan_options *options,
                                   chutni_scan_result *result) {
    if (!root->path || !root->root_id) return CHUTNI_ERR_INVALID;

    scan_context sc;
    memset(&sc, 0, sizeof sc);
    sc.store = store;
    sc.root_id = root->root_id;
    sc.result = result;
    sc.max_bytes = options && options->max_file_size_bytes
                       ? options->max_file_size_bytes
                       : DEFAULT_MAX_FILE_BYTES;
    sc.progress_callback = options ? options->progress_callback : NULL;
    sc.progress_userdata = options ? options->progress_userdata : NULL;
    sc.max_depth = effective_max_depth(root->policy_json, options, &sc.policy);
    result->deepest_directory_enumerated = 0;

    chutni_status status = prepare_producer(store, options, &sc);
    if (status != CHUTNI_OK) return status;

    char root_source_id[CHUTNI_ID_STRLEN];
    observe(&sc, root->path, NULL, 0, 1, root_source_id);
    if (!root_source_id[0]) return CHUTNI_ERR_IO;

    /* One manifest per committed scan, whether or not anything changed:
       "we looked again and it was the same" is a different fact from "nobody
       has looked", and only the manifest can tell them apart. */
    char scan_generation[CHUTNI_ID_STRLEN];
    if (chutni_new_id(scan_generation) != CHUTNI_OK) return CHUTNI_ERR_IO;
    write_coverage_manifest(&sc, root_source_id, root->path, scan_generation);
    return CHUTNI_OK;
}

chutni_status chutni_scan_root(chutni_store *store, const char *root_id,
                               const chutni_scan_options *options,
                               chutni_scan_result *result) {
    if (!store || !root_id || !result) return CHUTNI_ERR_INVALID;
    memset(result, 0, sizeof *result);

    chutni_root_info *roots = NULL;
    size_t count = 0;
    chutni_status status = chutni_roots_list(store, &roots, &count);
    if (status != CHUTNI_OK) return status;

    status = CHUTNI_ERR_NOTFOUND;
    for (size_t i = 0; i < count; i++)
        if (roots[i].root_id && !strcmp(roots[i].root_id, root_id))
            status = scan_one_root(store, &roots[i], options, result);
    chutni_root_info_free(roots, count);
    if (status != CHUTNI_OK) return status;
    return chutni_rebuild_indexes(store);
}

chutni_status chutni_scan(chutni_store *store,
                          const chutni_scan_options *options,
                          chutni_scan_result *result) {
    if (!store || !result) return CHUTNI_ERR_INVALID;
    memset(result, 0, sizeof *result);

    chutni_root_info *roots = NULL;
    size_t root_count = 0;
    chutni_status status = chutni_roots_list(store, &roots, &root_count);
    if (status != CHUTNI_OK) return status;
    if (root_count == 0) {
        chutni_root_info_free(roots, root_count);
        return CHUTNI_ERR_INVALID;
    }

    int deepest = 0;
    for (size_t i = 0; i < root_count; i++) {
        chutni_status one = scan_one_root(store, &roots[i], options, result);
        if (one != CHUTNI_OK) result->errors++;
        if (result->deepest_directory_enumerated > deepest)
            deepest = result->deepest_directory_enumerated;
    }
    result->deepest_directory_enumerated = deepest;
    result->complete_for_policy = result->errors == 0;
    chutni_root_info_free(roots, root_count);
    return chutni_rebuild_indexes(store);
}

chutni_status chutni_observe_directory(chutni_store *store, const char *source_id,
                                       const chutni_scan_options *options,
                                       chutni_scan_result *result) {
    if (!store || !source_id || !result) return CHUTNI_ERR_INVALID;
    memset(result, 0, sizeof *result);

    /* Observation does not broaden authorization. The directory must already
       be a source under a root the user approved (§11). */
    chutni_source_info *sources = NULL;
    size_t count = 0;
    chutni_status status = chutni_sources_list(store, NULL, &sources, &count);
    if (status != CHUTNI_OK) return status;

    char path[PATH_MAX] = "";
    char parent[CHUTNI_ID_STRLEN] = "";
    int depth = 0, is_directory = 0, found = 0;
    for (size_t i = 0; i < count; i++) {
        if (!sources[i].source_id || strcmp(sources[i].source_id, source_id))
            continue;
        found = 1;
        is_directory = sources[i].source_kind &&
                       !strcmp(sources[i].source_kind, "directory");
        if (sources[i].display_path)
            snprintf(path, sizeof path, "%s", sources[i].display_path);
        if (sources[i].parent_source_id)
            snprintf(parent, sizeof parent, "%s", sources[i].parent_source_id);
        depth = sources[i].depth >= 0 ? sources[i].depth : 0;
    }
    chutni_source_info_free(sources, count);
    if (!found) return CHUTNI_ERR_NOTFOUND;
    if (!is_directory || !path[0]) return CHUTNI_ERR_INVALID;

    chutni_root_info *roots = NULL;
    size_t root_count = 0;
    status = chutni_roots_list(store, &roots, &root_count);
    if (status != CHUTNI_OK) return status;

    /* The innermost authorized root containing this path. */
    const chutni_root_info *owner = NULL;
    size_t best = 0;
    for (size_t i = 0; i < root_count; i++) {
        if (!roots[i].path) continue;
        size_t len = strlen(roots[i].path);
        if (strncmp(path, roots[i].path, len)) continue;
        if (path[len] && path[len] != '/') continue;
        if (!owner || len > best) { owner = &roots[i]; best = len; }
    }
    if (!owner) {
        chutni_root_info_free(roots, root_count);
        return CHUTNI_ERR_DENIED;
    }

    scan_context sc;
    memset(&sc, 0, sizeof sc);
    sc.store = store;
    sc.result = result;
    sc.max_bytes = options && options->max_file_size_bytes
                       ? options->max_file_size_bytes
                       : DEFAULT_MAX_FILE_BYTES;
    sc.progress_callback = options ? options->progress_callback : NULL;
    sc.progress_userdata = options ? options->progress_userdata : NULL;
    effective_max_depth(owner->policy_json, options, &sc.policy);
    /* Owned by this frame: `owner` points into `roots`, which is released
       before the walk begins. */
    char owner_root_id[CHUTNI_ID_STRLEN];
    snprintf(owner_root_id, sizeof owner_root_id, "%s",
             owner->root_id ? owner->root_id : "");
    sc.root_id = owner_root_id;
    /* One directory, and no recursion into what it names. That is the whole
       operation: the host decides what to open next. */
    sc.max_depth = depth;
    status = prepare_producer(store, options, &sc);
    chutni_root_info_free(roots, root_count);
    if (status != CHUTNI_OK) return status;

    char observed[CHUTNI_ID_STRLEN];
    observe(&sc, path, parent[0] ? parent : NULL, depth, 0, observed);
    result->complete_for_policy = result->errors == 0;
    return chutni_rebuild_indexes(store);
}
