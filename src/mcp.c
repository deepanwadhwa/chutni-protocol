/* chutni-mcp — reusable local Chutni service.
 *
 * Default mode is newline-delimited JSON-RPC over stdio for MCP hosts. A
 * one-shot --call mode invokes the exact same tool implementation for native
 * host applications that already have a process supervisor, such as Samosa.
 *
 * stdout is protocol-only. Diagnostics belong on stderr.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "chutni.h"
#include "cj.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The implementation's release version comes from the VERSION file via the
 * Makefile. The fallback keeps a hand-rolled compile working without it, and says
 * plainly that it does not know the version rather than naming one it cannot
 * vouch for — this string ends up in producer records (§16.1). */
#ifndef CHUTNI_VERSION
#define CHUTNI_VERSION "0.0.0-unversioned"
#endif

#define CHUTNI_MCP_VERSION CHUTNI_VERSION
#define MCP_CURRENT_VERSION "2026-07-28"
#define MCP_LEGACY_VERSION "2025-11-25"

typedef struct {
    int selection_is_store;
    int store_exists;
    int store_valid;
    int root_matches;
    chutni_status store_status;
    char source_path[PATH_MAX];
    char store_path[PATH_MAX];
    char root_id[CHUTNI_ID_STRLEN];
    char store_id[CHUTNI_ID_STRLEN];
} folder_resolution;

static cj *json_clone(const cj *value) {
    if (!value) return cj_null();
    char *text = cj_dump(value, -1);
    if (!text) return NULL;
    const char *error = NULL;
    cj *copy = cj_parse(text, &error);
    (void)error;
    free(text);
    return copy;
}

static const char *argument_string(const cj *arguments, const char *key) {
    cj *value = cj_get(arguments, key);
    return value && value->type == CJ_STR ? value->str : NULL;
}

static int argument_bool(const cj *arguments, const char *key,
                         int default_value) {
    cj *value = cj_get(arguments, key);
    return value && value->type == CJ_BOOL ? value->bval : default_value;
}

static int argument_int(const cj *arguments, const char *key,
                        int default_value) {
    cj *value = cj_get(arguments, key);
    return value && value->type == CJ_NUM ? (int)value->num : default_value;
}

static cj *tool_error(const char *code, const char *message) {
    cj *result = cj_obj();
    if (!result) return NULL;
    cj_set(result, "ok", cj_bool(0));
    cj_set(result, "error", cj_str(code ? code : "chutni_error"));
    cj_set(result, "message", cj_str(message ? message : "Chutni operation failed."));
    return result;
}

static cj *status_error(const char *operation, chutni_status status,
                        chutni_store *store) {
    char message[768];
    const char *detail = chutni_last_error(store);
    snprintf(message, sizeof message, "%s: %s%s%s",
             operation ? operation : "Chutni operation failed",
             chutni_strerror(status),
             detail && *detail ? ": " : "",
             detail && *detail ? detail : "");
    return tool_error("chutni_error", message);
}

static int path_is_directory(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_exists(const char *path) {
    struct stat st;
    return path && lstat(path, &st) == 0;
}

static int directory_has_manifest(const char *path) {
    char manifest[PATH_MAX];
    return path &&
           (size_t)snprintf(manifest, sizeof manifest, "%s/manifest.json",
                            path) < sizeof manifest &&
           access(manifest, F_OK) == 0;
}

static int root_matches_source(chutni_store *store, const char *source_path,
                               char root_id[CHUTNI_ID_STRLEN]) {
    chutni_root_info *roots = NULL;
    size_t count = 0;
    if (chutni_roots_list(store, &roots, &count) != CHUTNI_OK) return 0;
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        if (roots[i].path && !strcmp(roots[i].path, source_path)) {
            if (root_id)
                snprintf(root_id, CHUTNI_ID_STRLEN, "%s", roots[i].root_id);
            found = 1;
            break;
        }
    }
    chutni_root_info_free(roots, count);
    return found;
}

static chutni_status resolve_folder(const char *path, folder_resolution *out) {
    if (!path || !*path || !out) return CHUTNI_ERR_INVALID;
    memset(out, 0, sizeof *out);

    char canonical[PATH_MAX];
    if (!realpath(path, canonical)) return CHUTNI_ERR_NOTFOUND;
    if (!path_is_directory(canonical)) return CHUTNI_ERR_INVALID;

    chutni_store *store = NULL;
    chutni_status status = chutni_open(canonical, 1, &store);
    if (status == CHUTNI_OK) {
        out->selection_is_store = 1;
        out->store_exists = 1;
        out->store_valid = 1;
        snprintf(out->store_path, sizeof out->store_path, "%s", canonical);
        snprintf(out->store_id, sizeof out->store_id, "%s",
                 chutni_store_id(store));
        chutni_close(store);
        return CHUTNI_OK;
    }
    if (directory_has_manifest(canonical)) {
        out->selection_is_store = 1;
        out->store_exists = 1;
        out->store_status = status;
        snprintf(out->store_path, sizeof out->store_path, "%s", canonical);
        return CHUTNI_OK;
    }

    snprintf(out->source_path, sizeof out->source_path, "%s", canonical);
    if ((size_t)snprintf(out->store_path, sizeof out->store_path, "%s.chutni",
                         canonical) >= sizeof out->store_path)
        return CHUTNI_ERR_INVALID;
    if (!path_exists(out->store_path)) return CHUTNI_OK;

    out->store_exists = 1;
    status = chutni_open(out->store_path, 1, &store);
    out->store_status = status;
    if (status != CHUTNI_OK) return CHUTNI_OK;
    out->store_valid = 1;
    snprintf(out->store_id, sizeof out->store_id, "%s",
             chutni_store_id(store));
    out->root_matches =
        root_matches_source(store, out->source_path, out->root_id);
    chutni_close(store);
    return CHUTNI_OK;
}

static const char *resolution_action(const folder_resolution *resolution) {
    if (resolution->selection_is_store) {
        if (resolution->store_valid) return "open_store";
        return resolution->store_status == CHUTNI_ERR_VERSION
                   ? "unsupported_store"
                   : "invalid_store";
    }
    if (!resolution->store_exists) return "create_store";
    if (!resolution->store_valid)
        return resolution->store_status == CHUTNI_ERR_VERSION
                   ? "unsupported_store"
                   : "path_collision";
    return resolution->root_matches ? "open_store" : "authorize_root";
}

static cj *resolution_json(const folder_resolution *resolution) {
    cj *result = cj_obj();
    if (!result) return NULL;
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "selection_kind",
           cj_str(resolution->selection_is_store ? "store" : "source_directory"));
    if (resolution->source_path[0])
        cj_set(result, "source_path", cj_str(resolution->source_path));
    cj_set(result, "store_path", cj_str(resolution->store_path));
    cj_set(result, "store_exists", cj_bool(resolution->store_exists));
    cj_set(result, "store_valid", cj_bool(resolution->store_valid));
    cj_set(result, "root_authorized", cj_bool(resolution->root_matches));
    cj_set(result, "action", cj_str(resolution_action(resolution)));
    if (resolution->store_id[0])
        cj_set(result, "store_id", cj_str(resolution->store_id));
    if (resolution->root_id[0])
        cj_set(result, "root_id", cj_str(resolution->root_id));
    if (resolution->store_status != CHUTNI_OK)
        cj_set(result, "store_error",
               cj_str(chutni_strerror(resolution->store_status)));
    return result;
}

static void set_scan_result(cj *object, const chutni_scan_result *scan) {
    cj *value = cj_obj();
    cj_set(value, "files_seen", cj_num((double)scan->files_seen));
    cj_set(value, "sources_indexed", cj_num((double)scan->sources_indexed));
    cj_set(value, "unchanged", cj_num((double)scan->unchanged));
    cj_set(value, "text_artifacts", cj_num((double)scan->text_artifacts));
    cj_set(value, "metadata_artifacts",
           cj_num((double)scan->metadata_artifacts));
    cj_set(value, "skipped", cj_num((double)scan->skipped));
    cj_set(value, "errors", cj_num((double)scan->errors));
    /* §15.7. An agent reading only the file counts would conclude a bounded
       scan had read the whole tree, so the directory side travels with them. */
    cj_set(value, "directories_observed",
           cj_num((double)scan->directories_observed));
    cj_set(value, "directories_enumerated",
           cj_num((double)scan->directories_enumerated));
    cj_set(value, "depth_limited_directories",
           cj_num((double)scan->depth_limited_directories));
    cj_set(value, "listing_artifacts", cj_num((double)scan->listing_artifacts));
    cj_set(value, "listings_reused", cj_num((double)scan->listings_reused));
    cj_set(value, "files_hashed", cj_num((double)scan->files_hashed));
    cj_set(value, "files_read", cj_num((double)scan->files_read));
    cj_set(value, "excluded_sources", cj_num((double)scan->excluded_sources));
    cj_set(value, "unsupported_sources",
           cj_num((double)scan->unsupported_sources));
    cj_set(value, "sources_marked_missing",
           cj_num((double)scan->sources_marked_missing));
    cj_set(value, "deepest_directory_enumerated",
           cj_num((double)scan->deepest_directory_enumerated));
    cj_set(value, "complete_for_policy", cj_bool(scan->complete_for_policy));
    if (scan->depth_limited_directories)
        cj_set(value, "note",
               cj_str("complete_for_policy reports that the bounded operation "
                      "finished. Directories past max_depth were recorded by "
                      "name and never opened; do not treat this as an "
                      "exhaustive index of the subtree."));
    cj_set(object, "scan", value);
}

static int set_store_counts(cj *object, chutni_store *store) {
    chutni_counts counts;
    if (chutni_store_counts(store, &counts) != CHUTNI_OK) return 0;
    cj *value = cj_obj();
    if (!value) return 0;
    cj_set(value, "roots", cj_num((double)counts.roots));
    cj_set(value, "sources", cj_num((double)counts.sources));
    cj_set(value, "sources_files", cj_num((double)counts.sources_files));
    cj_set(value, "sources_directories",
           cj_num((double)counts.sources_directories));
    cj_set(value, "sources_opaque_directories",
           cj_num((double)counts.sources_opaque_directories));
    cj_set(value, "relations", cj_num((double)counts.relations));
    cj_set(value, "artifacts", cj_num((double)counts.artifacts));
    cj_set(value, "artifacts_active",
           cj_num((double)counts.artifacts_active));
    cj_set(value, "artifacts_stale",
           cj_num((double)counts.artifacts_stale));
    cj_set(value, "objects", cj_num((double)counts.objects));
    cj_set(value, "producers", cj_num((double)counts.producers));
    cj_set(value, "derivations", cj_num((double)counts.derivations));
    cj_set(object, "counts", value);
    return 1;
}

static cj *tool_folder_status(const cj *arguments, int *is_error) {
    const char *path = argument_string(arguments, "path");
    folder_resolution resolution;
    chutni_status status = resolve_folder(path, &resolution);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot resolve selected folder", status, NULL);
    }
    return resolution_json(&resolution);
}

static cj *tool_folder_activate(const cj *arguments, int *is_error) {
    const char *path = argument_string(arguments, "path");
    if (!argument_bool(arguments, "confirmed", 0)) {
        *is_error = 1;
        return tool_error(
            "confirmation_required",
            "The user must confirm the store path and folder scan before activation.");
    }

    folder_resolution resolution;
    chutni_status status = resolve_folder(path, &resolution);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot resolve selected folder", status, NULL);
    }
    const char *action = resolution_action(&resolution);
    if (!strcmp(action, "unsupported_store") ||
        !strcmp(action, "invalid_store") ||
        !strcmp(action, "path_collision")) {
        *is_error = 1;
        return tool_error(action, "The candidate path cannot be activated safely.");
    }

    chutni_store *store = NULL;
    int created = 0;
    if (resolution.store_valid) {
        status = chutni_open(resolution.store_path, 0, &store);
    } else {
        const char *label = argument_string(arguments, "label");
        status = chutni_create(resolution.store_path, label, &store);
        created = status == CHUTNI_OK;
    }
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open or create store", status, store);
    }

    /* §11.1, §11.2. A host activating a folder is the moment the user's
       intent for it is known, so the depth bound and the memory goal are
       recorded on the root rather than passed to each later scan. Absent
       max_depth stays unbounded — the v0.1 meaning. */
    chutni_root_policy policy;
    chutni_root_policy_defaults(&policy);
    cj *depth_value = cj_get(arguments, "max_depth");
    if (depth_value && depth_value->type == CJ_NUM && depth_value->num >= 0)
        policy.max_depth = (int)depth_value->num;
    policy.memory_goal = argument_string(arguments, "memory_goal");
    policy.definition_mode = argument_string(arguments, "definition_mode");

    char root_id[CHUTNI_ID_STRLEN] = {0};
    if (resolution.source_path[0]) {
        status = chutni_root_add(store, resolution.source_path,
                                 argument_string(arguments, "label"), &policy,
                                 root_id);
        if (status != CHUTNI_OK) {
            cj *error = status_error("Cannot authorize selected root", status,
                                     store);
            chutni_close(store);
            *is_error = 1;
            return error;
        }
    }

    chutni_scan_options options;
    memset(&options, 0, sizeof options);
    options.app_name = argument_string(arguments, "app_name");
    options.app_version = argument_string(arguments, "app_version");
    cj *max_value = cj_get(arguments, "max_file_size_bytes");
    if (max_value && max_value->type == CJ_NUM && max_value->num > 0)
        options.max_file_size_bytes = (uint64_t)max_value->num;
    chutni_scan_result scan;
    status = chutni_scan(store, &options, &scan);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Cannot scan selected folder", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    char store_path[PATH_MAX], store_id[CHUTNI_ID_STRLEN];
    snprintf(store_path, sizeof store_path, "%s", chutni_store_path(store));
    snprintf(store_id, sizeof store_id, "%s", chutni_store_id(store));

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "action", cj_str(created ? "created" : "opened"));
    cj_set(result, "created", cj_bool(created));
    if (resolution.source_path[0])
        cj_set(result, "source_path", cj_str(resolution.source_path));
    cj_set(result, "store_path", cj_str(store_path));
    cj_set(result, "store_id", cj_str(store_id));
    if (root_id[0]) cj_set(result, "root_id", cj_str(root_id));
    set_scan_result(result, &scan);
    if (!set_store_counts(result, store)) {
        cj_free(result);
        cj *error = status_error("Cannot read store counts", CHUTNI_ERR_DB,
                                 store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    chutni_close(store);
    if (created && argument_bool(arguments, "register", 0))
        chutni_registry_add(store_path);
    return result;
}

static cj *tool_discover(const cj *arguments, int *is_error) {
    (void)arguments;
    chutni_store_info *stores = NULL;
    size_t count = 0;
    chutni_status status = chutni_discover(&stores, &count);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Store discovery failed", status, NULL);
    }
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "count", cj_num((double)count));
    cj *items = cj_arr();
    for (size_t i = 0; i < count; i++) {
        cj *item = cj_obj();
        cj_set(item, "store_path", cj_str(stores[i].store_path));
        if (stores[i].store_id)
            cj_set(item, "store_id", cj_str(stores[i].store_id));
        if (stores[i].label) cj_set(item, "label", cj_str(stores[i].label));
        if (stores[i].spec_version)
            cj_set(item, "spec_version", cj_str(stores[i].spec_version));
        cj_set(item, "readable", cj_bool(stores[i].readable));
        cj_push(items, item);
    }
    cj_set(result, "stores", items);
    chutni_store_info_free(stores, count);
    return result;
}

static cj *tool_capabilities(const cj *arguments, int *is_error) {
    (void)arguments;
    (void)is_error;
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "spec_version", cj_str(CHUTNI_SPEC_VERSION));
    cj_set(result, "library_version", cj_str(chutni_library_version()));
    cj_set(result, "service_version", cj_str(CHUTNI_MCP_VERSION));
    cj *transports = cj_arr();
    cj_push(transports, cj_str("mcp_stdio"));
    cj_push(transports, cj_str("one_shot_json"));
    cj_set(result, "transports", transports);

    cj *origins = cj_arr();
    static const char *origin_names[] = {
        "direct", "deterministic_transform", "model_generated", "human", NULL
    };
    for (const char **name = origin_names; *name; name++)
        cj_push(origins, cj_str(*name));
    cj_set(result, "artifact_origins", origins);

    cj *kinds = cj_arr();
    static const char *kind_names[] = {
        "file_metadata", "extracted_text", "page_text", "ocr_text",
        "transcript", "text_chunk", "summary_short", "summary_long",
        "image_caption", "document_title", "keywords", "entities",
        "table_schema", "sheet_summary", "archive_listing", "thumbnail",
        "language_detection", "content_warning", "processing_error",
        CHUTNI_KIND_DIRECTORY_LISTING, CHUTNI_KIND_SOURCE_DEFINITION,
        CHUTNI_KIND_COVERAGE_MANIFEST, NULL
    };
    for (const char **name = kind_names; *name; name++)
        cj_push(kinds, cj_str(*name));
    cj_set(result, "core_artifact_kinds", kinds);

    cj *capabilities = cj_arr();
    static const char *capability_names[] = {
        "sources", "artifacts", "provenance", "hierarchical_sources",
        "bounded_coverage", "directory_definitions", NULL
    };
    for (const char **name = capability_names; *name; name++)
        cj_push(capabilities, cj_str(*name));
    cj_set(result, "capabilities", capabilities);

    cj *stop_reasons = cj_arr();
    static const char *stop_names[] = {
        CHUTNI_STOP_MAX_DEPTH, CHUTNI_STOP_COHERENT, CHUTNI_STOP_BUDGET,
        CHUTNI_STOP_EXCLUDED, CHUTNI_STOP_UNSUPPORTED, CHUTNI_STOP_UNREADABLE,
        CHUTNI_STOP_USER_CANCELED, NULL
    };
    for (const char **name = stop_names; *name; name++)
        cj_push(stop_reasons, cj_str(*name));
    cj_set(result, "definition_stop_reasons", stop_reasons);

    cj *modes = cj_arr();
    cj_push(modes, cj_str(CHUTNI_DEFINITION_ADAPTIVE));
    cj_push(modes, cj_str(CHUTNI_DEFINITION_PER_SOURCE));
    cj_set(result, "definition_modes", modes);

    cj *selectors = cj_arr();
    static const char *selector_names[] = {
        "pages", "sheet_range", "image_region", "time_range",
        "byte_range", NULL
    };
    for (const char **name = selector_names; *name; name++)
        cj_push(selectors, cj_str(*name));
    cj_set(result, "selector_types", selectors);

    cj *scanner = cj_obj();
    cj_set(scanner, "file_metadata_for_every_file", cj_bool(1));
    cj_set(scanner, "text_like_utf8_extraction", cj_bool(1));
    cj_set(scanner, "host_artifact_submission", cj_bool(1));
    cj_set(scanner, "pdf_parser", cj_bool(0));
    cj_set(scanner, "ocr", cj_bool(0));
    cj_set(scanner, "image_understanding", cj_bool(0));
    cj_set(scanner, "spreadsheet_parser", cj_bool(0));
    cj_set(scanner, "speech_to_text", cj_bool(0));
    cj_set(scanner, "directory_sources", cj_bool(1));
    cj_set(scanner, "bounded_depth", cj_bool(1));
    cj_set(scanner, "coverage_manifest_per_scan", cj_bool(1));
    /* The scanner recognizes nothing. Categories like "application bundle" are
       a producer's claim and arrive through put_artifacts with provenance. */
    cj_set(scanner, "semantic_directory_classification", cj_bool(0));
    cj_set(result, "reference_scanner", scanner);

    cj_set(result, "semantic_validation", cj_str("not_performed"));
    cj_set(result, "writer_policy",
           cj_str("single_writer_many_readers"));
    return result;
}

static cj *store_info_json(chutni_store *store) {
    chutni_root_info *roots = NULL;
    size_t root_count = 0;
    if (chutni_roots_list(store, &roots, &root_count) != CHUTNI_OK)
        return NULL;

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "store_path", cj_str(chutni_store_path(store)));
    cj_set(result, "store_id", cj_str(chutni_store_id(store)));
    cj_set(result, "spec_version", cj_str(CHUTNI_SPEC_VERSION));
    if (!set_store_counts(result, store)) {
        cj_free(result);
        chutni_root_info_free(roots, root_count);
        return NULL;
    }
    cj *root_json = cj_arr();
    for (size_t i = 0; i < root_count; i++) {
        cj *item = cj_obj();
        cj_set(item, "root_id", cj_str(roots[i].root_id));
        if (roots[i].path) cj_set(item, "path", cj_str(roots[i].path));
        if (roots[i].label) cj_set(item, "label", cj_str(roots[i].label));
        cj_push(root_json, item);
    }
    cj_set(result, "roots", root_json);
    chutni_root_info_free(roots, root_count);
    return result;
}

static cj *tool_store_info(const cj *arguments, int *is_error) {
    const char *store_path = argument_string(arguments, "store_path");
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 1, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store", status, NULL);
    }
    cj *result = store_info_json(store);
    if (!result) {
        *is_error = 1;
        result = status_error("Cannot read store information",
                              CHUTNI_ERR_DB, store);
    }
    chutni_close(store);
    return result;
}

static cj *tool_scan(const cj *arguments, int *is_error) {
    if (!argument_bool(arguments, "confirmed", 0)) {
        *is_error = 1;
        return tool_error("confirmation_required",
                          "The user must confirm the rescan.");
    }
    const char *store_path = argument_string(arguments, "store_path");
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 0, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store for scanning", status, NULL);
    }
    chutni_scan_options options;
    memset(&options, 0, sizeof options);
    options.app_name = argument_string(arguments, "app_name");
    options.app_version = argument_string(arguments, "app_version");
    cj *max_value = cj_get(arguments, "max_file_size_bytes");
    if (max_value && max_value->type == CJ_NUM && max_value->num > 0)
        options.max_file_size_bytes = (uint64_t)max_value->num;
    /* §11.1: a depth given here overrides the root's stored policy for this
       run only. Absent leaves the policy alone, which is the normal case — the
       bound is the user's decision, recorded when the root was authorized. */
    cj *depth_value = cj_get(arguments, "max_depth");
    if (depth_value && depth_value->type == CJ_NUM && depth_value->num >= 0) {
        options.use_override_max_depth = 1;
        options.override_max_depth = (int)depth_value->num;
    }
    chutni_scan_result scan;
    status = chutni_scan(store, &options, &scan);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Store scan failed", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "store_path", cj_str(chutni_store_path(store)));
    set_scan_result(result, &scan);
    if (!set_store_counts(result, store)) {
        cj_free(result);
        cj *error = status_error("Cannot read store counts", CHUTNI_ERR_DB,
                                 store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    chutni_close(store);
    return result;
}

/* Resolve a source by id or by path, since a caller will have whichever is
   closer to hand. */
static int resolve_source_argument(chutni_store *store, const cj *arguments,
                                   char out[CHUTNI_ID_STRLEN]) {
    const char *id = argument_string(arguments, "source_id");
    const char *path = argument_string(arguments, "source_path");
    if (id && *id) {
        snprintf(out, CHUTNI_ID_STRLEN, "%s", id);
        return 1;
    }
    if (path && *path && chutni_source_find(store, path, out) == CHUTNI_OK)
        return 1;
    return 0;
}

static cj *source_summary_json(const chutni_source_info *source) {
    cj *item = cj_obj();
    cj_set(item, "source_id", cj_str(source->source_id ? source->source_id : ""));
    if (source->display_path)
        cj_set(item, "display_path", cj_str(source->display_path));
    cj_set(item, "source_kind",
           cj_str(source->source_kind ? source->source_kind : "file"));
    if (source->parent_source_id)
        cj_set(item, "parent_source_id", cj_str(source->parent_source_id));
    if (source->media_type) cj_set(item, "media_type", cj_str(source->media_type));
    if (source->state) cj_set(item, "state", cj_str(source->state));
    /* §12.5. "opaque" is the field an agent must read before assuming this
       directory's contents are anywhere in the store. */
    if (source->observation)
        cj_set(item, "observation", cj_str(source->observation));
    cj_set(item, "depth",
           source->depth < 0 ? cj_null() : cj_num((double)source->depth));
    return item;
}

static cj *tool_children(const cj *arguments, int *is_error) {
    const char *store_path = argument_string(arguments, "store_path");
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 1, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store", status, NULL);
    }
    char source_id[CHUTNI_ID_STRLEN];
    if (!resolve_source_argument(store, arguments, source_id)) {
        chutni_close(store);
        *is_error = 1;
        return tool_error("invalid_arguments",
                          "Provide source_id or a source_path this store knows.");
    }
    chutni_source_info *children = NULL;
    size_t count = 0;
    status = chutni_list_children(store, source_id, &children, &count);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Cannot list children", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    cj *result = cj_obj();
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "count", cj_num((double)count));
    cj *array = cj_arr();
    for (size_t i = 0; i < count; i++)
        cj_push(array, source_summary_json(&children[i]));
    cj_set(result, "children", array);
    if (count == 0)
        cj_set(result, "note",
               cj_str("No children are recorded. This directory may never have "
                      "been enumerated; chutni_observe_directory opens exactly "
                      "one directory without recursing."));
    chutni_source_info_free(children, count);
    chutni_close(store);
    return result;
}

static cj *tool_observe_directory(const cj *arguments, int *is_error) {
    if (!argument_bool(arguments, "confirmed", 0)) {
        *is_error = 1;
        return tool_error("confirmation_required",
                          "The user must confirm reading this directory.");
    }
    const char *store_path = argument_string(arguments, "store_path");
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 0, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store for observation", status, NULL);
    }
    char source_id[CHUTNI_ID_STRLEN];
    if (!resolve_source_argument(store, arguments, source_id)) {
        chutni_close(store);
        *is_error = 1;
        return tool_error("invalid_arguments",
                          "Provide source_id or a source_path this store knows.");
    }
    chutni_scan_options options;
    memset(&options, 0, sizeof options);
    options.app_name = argument_string(arguments, "app_name");
    options.app_version = argument_string(arguments, "app_version");
    chutni_scan_result scan;
    status = chutni_observe_directory(store, source_id, &options, &scan);
    if (status != CHUTNI_OK) {
        cj *error = status_error(
            status == CHUTNI_ERR_DENIED
                ? "That directory is not inside an authorized root"
                : "Cannot observe directory",
            status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "source_id", cj_str(source_id));
    set_scan_result(result, &scan);
    set_store_counts(result, store);
    chutni_close(store);
    return result;
}

static cj *tool_coverage(const cj *arguments, int *is_error) {
    const char *store_path = argument_string(arguments, "store_path");
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 1, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store", status, NULL);
    }
    char target[CHUTNI_ID_STRLEN] = "";
    const char *root_id = argument_string(arguments, "root_id");
    if (root_id && *root_id) snprintf(target, sizeof target, "%s", root_id);
    else if (!resolve_source_argument(store, arguments, target)) {
        /* With one root and no argument, there is no ambiguity to resolve. */
        chutni_root_info *roots = NULL;
        size_t count = 0;
        chutni_roots_list(store, &roots, &count);
        if (count == 1 && roots[0].root_id)
            snprintf(target, sizeof target, "%s", roots[0].root_id);
        chutni_root_info_free(roots, count);
    }
    if (!target[0]) {
        chutni_close(store);
        *is_error = 1;
        return tool_error("invalid_arguments",
                          "Provide root_id, source_id, or source_path.");
    }

    char *json = NULL;
    status = chutni_get_coverage(store, target, &json);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Cannot read coverage", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    cj *coverage = cj_parse(json, NULL);
    chutni_free(json);
    chutni_close(store);
    if (!coverage) {
        *is_error = 1;
        return tool_error("chutni_error", "Coverage could not be encoded.");
    }
    /* The sentence that stops a bounded scan being read as an exhaustive one.
       An agent that skips the numbers still sees this. */
    cj_set(coverage, "interpretation",
           cj_str("complete_for_policy means the requested bounded operation "
                  "finished. It does not mean the subtree was read. Directories "
                  "whose observation is \"opaque\" were named but never opened, "
                  "and nothing in this store describes their contents."));
    return coverage;
}

static cj *tool_search(const cj *arguments, int *is_error) {
    const char *store_path = argument_string(arguments, "store_path");
    const char *query = argument_string(arguments, "query");
    if (!store_path || !query || !*query) {
        *is_error = 1;
        return tool_error("invalid_arguments",
                          "store_path and a non-empty query are required.");
    }
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 1, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store for search", status, NULL);
    }
    int limit = argument_int(arguments, "limit", 10);
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;
    chutni_search_request request;
    memset(&request, 0, sizeof request);
    request.query = query;
    request.limit = limit;
    request.include_stale = argument_bool(arguments, "include_stale", 0);
    request.match_any = argument_bool(arguments, "match_any", 0);
    chutni_search_result *results = NULL;
    size_t count = 0;
    status = chutni_search(store, &request, &results, &count);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Store search failed", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "query", cj_str(query));
    cj_set(result, "count", cj_num((double)count));
    cj *items = cj_arr();
    for (size_t i = 0; i < count; i++) {
        cj *item = cj_obj();
        if (results[i].source_id)
            cj_set(item, "source_id", cj_str(results[i].source_id));
        if (results[i].artifact_id)
            cj_set(item, "artifact_id", cj_str(results[i].artifact_id));
        if (results[i].display_path)
            cj_set(item, "display_path", cj_str(results[i].display_path));
        if (results[i].artifact_kind)
            cj_set(item, "artifact_kind", cj_str(results[i].artifact_kind));
        if (results[i].snippet)
            cj_set(item, "snippet", cj_str(results[i].snippet));
        if (results[i].producer_id)
            cj_set(item, "producer_id", cj_str(results[i].producer_id));
        if (results[i].selector_json) {
            const char *parse_error = NULL;
            cj *selector = cj_parse(results[i].selector_json, &parse_error);
            if (selector)
                cj_set(item, "selector", selector);
            else
                cj_set(item, "selector_json",
                       cj_str(results[i].selector_json));
        }
        if (results[i].freshness)
            cj_set(item, "freshness", cj_str(results[i].freshness));
        cj_set(item, "score", cj_num(results[i].score));
        if (results[i].score_type)
            cj_set(item, "score_type", cj_str(results[i].score_type));
        /* §19.3. Without these an agent sees a path and a snippet and has no
           way to tell a hit inside a one-level scan from a hit inside an
           exhaustive index; coverage_manifest_id is where it goes to find out. */
        if (results[i].source_kind)
            cj_set(item, "source_kind", cj_str(results[i].source_kind));
        if (results[i].parent_source_id)
            cj_set(item, "parent_source_id", cj_str(results[i].parent_source_id));
        if (results[i].coverage_manifest_id)
            cj_set(item, "coverage_manifest_id",
                   cj_str(results[i].coverage_manifest_id));
        cj_set(item, "depth", results[i].depth < 0
                                  ? cj_null()
                                  : cj_num((double)results[i].depth));
        cj_push(items, item);
    }
    cj_set(result, "results", items);
    chutni_search_result_free(results, count);
    chutni_close(store);
    return result;
}

static char *json_value_text(const cj *value, const char *fallback) {
    if (!value) return strdup(fallback ? fallback : "null");
    return cj_dump(value, -1);
}

typedef struct {
    char source_id[CHUTNI_ID_STRLEN];
    char display_path[PATH_MAX];
    char content_hash[CHUTNI_HASH_STRLEN];
    char media_type[160];
    char state[32];
    int64_t size_bytes;
} source_snapshot;

static chutni_status source_snapshot_load(chutni_store *store,
                                          const cj *arguments,
                                          source_snapshot *snapshot) {
    memset(snapshot, 0, sizeof *snapshot);
    const char *source_id_arg = argument_string(arguments, "source_id");
    const char *source_path = argument_string(arguments, "source_path");
    if (source_id_arg && *source_id_arg) {
        snprintf(snapshot->source_id, sizeof snapshot->source_id, "%s",
                 source_id_arg);
    } else if (source_path && *source_path) {
        chutni_status status =
            chutni_source_find(store, source_path, snapshot->source_id);
        if (status != CHUTNI_OK) return status;
    } else {
        return CHUTNI_ERR_INVALID;
    }

    chutni_source_info *sources = NULL;
    size_t source_count = 0;
    chutni_status status =
        chutni_sources_list(store, NULL, &sources, &source_count);
    if (status != CHUTNI_OK) return status;
    int found = 0;
    for (size_t i = 0; i < source_count; i++) {
        if (!sources[i].source_id ||
            strcmp(sources[i].source_id, snapshot->source_id))
            continue;
        if (sources[i].display_path)
            snprintf(snapshot->display_path,
                     sizeof snapshot->display_path, "%s",
                     sources[i].display_path);
        if (sources[i].content_hash)
            snprintf(snapshot->content_hash,
                     sizeof snapshot->content_hash, "%s",
                     sources[i].content_hash);
        if (sources[i].media_type)
            snprintf(snapshot->media_type, sizeof snapshot->media_type,
                     "%s", sources[i].media_type);
        if (sources[i].state)
            snprintf(snapshot->state, sizeof snapshot->state, "%s",
                     sources[i].state);
        snapshot->size_bytes = sources[i].size_bytes;
        found = 1;
        break;
    }
    chutni_source_info_free(sources, source_count);
    return found ? CHUTNI_OK : CHUTNI_ERR_NOTFOUND;
}

static void set_json_or_text(cj *object, const char *name,
                             const char *json_text) {
    if (!json_text) return;
    const char *error = NULL;
    cj *value = cj_parse(json_text, &error);
    (void)error;
    if (value)
        cj_set(object, name, value);
    else
        cj_set(object, name, cj_str(json_text));
}

static cj *tool_put_artifacts(const cj *arguments, int *is_error) {
    if (!argument_bool(arguments, "confirmed", 0)) {
        *is_error = 1;
        return tool_error(
            "confirmation_required",
            "The host must confirm that these outputs should become reusable memory.");
    }
    const char *store_path = argument_string(arguments, "store_path");
    const char *expected_hash =
        argument_string(arguments, "source_content_hash");
    const char *operation = argument_string(arguments, "operation");
    cj *producer_json = cj_get(arguments, "producer");
    cj *artifact_json = cj_get(arguments, "artifacts");
    if (!store_path || !expected_hash || !operation || !*operation ||
        !producer_json || producer_json->type != CJ_OBJ ||
        !artifact_json || artifact_json->type != CJ_ARR ||
        artifact_json->n == 0 || artifact_json->n > 128) {
        *is_error = 1;
        return tool_error(
            "invalid_arguments",
            "store_path, source_id or source_path, source_content_hash, producer, operation, and 1-128 artifacts are required.");
    }

    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 0, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store for artifact submission",
                            status, NULL);
    }
    source_snapshot source;
    status = source_snapshot_load(store, arguments, &source);
    if (status == CHUTNI_OK) {
        const char *freshness = NULL;
        status = chutni_source_refresh(store, source.source_id, &freshness);
        (void)freshness;
    }
    if (status == CHUTNI_OK)
        status = source_snapshot_load(store, arguments, &source);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Cannot resolve submitted artifact source",
                                 status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    if (!source.content_hash[0] ||
        strcmp(expected_hash, source.content_hash)) {
        chutni_close(store);
        *is_error = 1;
        return tool_error(
            "source_version_mismatch",
            "The submitted artifacts do not describe the source's current BLAKE3 hash.");
    }

    chutni_producer producer;
    memset(&producer, 0, sizeof producer);
    producer.producer_kind =
        argument_string(producer_json, "producer_kind");
    producer.name = argument_string(producer_json, "name");
    producer.version = argument_string(producer_json, "version");
    producer.model_id = argument_string(producer_json, "model_id");
    producer.model_revision =
        argument_string(producer_json, "model_revision");
    producer.weights_hash = argument_string(producer_json, "weights_hash");
    producer.quantization = argument_string(producer_json, "quantization");
    producer.runtime = argument_string(producer_json, "runtime");
    producer.app_name = argument_string(producer_json, "app_name");
    producer.app_version = argument_string(producer_json, "app_version");

    char *details_json = NULL;
    cj *details = cj_get(producer_json, "details");
    if (details) {
        if (details->type != CJ_OBJ) {
            chutni_close(store);
            *is_error = 1;
            return tool_error("invalid_arguments",
                              "producer.details must be an object.");
        }
        details_json = json_value_text(details, "{}");
        producer.details_json = details_json;
    }

    char *parameters_json = NULL;
    cj *parameters = cj_get(arguments, "parameters");
    if (parameters && parameters->type != CJ_OBJ) {
        free(details_json);
        chutni_close(store);
        *is_error = 1;
        return tool_error("invalid_arguments",
                          "parameters must be an object.");
    }
    parameters_json = json_value_text(parameters, "{}");

    char *inputs_json = NULL;
    cj *inputs = cj_get(arguments, "inputs");
    if (inputs && inputs->type != CJ_ARR) {
        free(details_json);
        free(parameters_json);
        chutni_close(store);
        *is_error = 1;
        return tool_error("invalid_arguments", "inputs must be an array.");
    }
    if (inputs) {
        inputs_json = json_value_text(inputs, "[]");
    } else {
        cj *default_inputs = cj_arr();
        cj *input = cj_obj();
        cj_set(input, "source_id", cj_str(source.source_id));
        cj_set(input, "source_content_hash", cj_str(source.content_hash));
        cj_push(default_inputs, input);
        inputs_json = json_value_text(default_inputs, "[]");
        cj_free(default_inputs);
    }

    size_t artifact_count = artifact_json->n;
    chutni_artifact *artifacts =
        calloc(artifact_count, sizeof *artifacts);
    char (*artifact_ids)[CHUTNI_ID_STRLEN] =
        calloc(artifact_count, sizeof *artifact_ids);
    char **selector_json = calloc(artifact_count, sizeof *selector_json);
    char **metadata_json = calloc(artifact_count, sizeof *metadata_json);
    int valid = artifacts && artifact_ids && selector_json && metadata_json &&
                (!details || details_json) &&
                parameters_json && inputs_json;
    for (size_t i = 0; valid && i < artifact_count; i++) {
        cj *item = artifact_json->items[i];
        const char *text = argument_string(item, "text");
        const char *kind = argument_string(item, "artifact_kind");
        const char *origin = argument_string(item, "artifact_origin");
        cj *selector = cj_get(item, "selector");
        cj *metadata = cj_get(item, "metadata");
        if (!item || item->type != CJ_OBJ || !text || !kind || !origin ||
            (selector && selector->type != CJ_OBJ) ||
            (metadata && metadata->type != CJ_OBJ)) {
            valid = 0;
            break;
        }
        selector_json[i] =
            selector ? json_value_text(selector, NULL) : NULL;
        metadata_json[i] =
            metadata ? json_value_text(metadata, NULL) : NULL;
        if ((selector && !selector_json[i]) ||
            (metadata && !metadata_json[i])) {
            valid = 0;
            break;
        }
        artifacts[i].source_id = source.source_id;
        artifacts[i].artifact_kind = kind;
        artifacts[i].artifact_origin = origin;
        artifacts[i].media_type =
            argument_string(item, "media_type")
                ? argument_string(item, "media_type")
                : "text/plain; charset=utf-8";
        artifacts[i].inline_text = text;
        artifacts[i].selector_json = selector_json[i];
        artifacts[i].language = argument_string(item, "language");
        artifacts[i].source_content_hash = source.content_hash;
        artifacts[i].supersedes_artifact_id =
            argument_string(item, "supersedes_artifact_id");
        artifacts[i].metadata_json = metadata_json[i];
    }

    char producer_id[CHUTNI_ID_STRLEN] = "";
    char derivation_id[CHUTNI_ID_STRLEN] = "";
    if (valid)
        status = chutni_artifacts_put(
            store, &producer, operation,
            argument_string(arguments, "recipe_hash"), parameters_json,
            inputs_json, artifacts, artifact_count, producer_id,
            derivation_id, artifact_ids);
    else
        status = CHUTNI_ERR_INVALID;

    for (size_t i = 0; i < artifact_count; i++) {
        free(selector_json ? selector_json[i] : NULL);
        free(metadata_json ? metadata_json[i] : NULL);
    }
    free(selector_json);
    free(metadata_json);
    free(artifacts);
    free(details_json);
    free(parameters_json);
    free(inputs_json);

    if (status != CHUTNI_OK) {
        cj *error =
            valid ? status_error("Cannot store artifact batch", status, store)
                  : tool_error(
                        "invalid_arguments",
                        "Each artifact requires text, artifact_kind, and artifact_origin; selector and metadata must be objects.");
        free(artifact_ids);
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "store_path", cj_str(store_path));
    cj_set(result, "source_id", cj_str(source.source_id));
    cj_set(result, "source_content_hash", cj_str(source.content_hash));
    cj_set(result, "producer_id", cj_str(producer_id));
    cj_set(result, "derivation_id", cj_str(derivation_id));
    cj_set(result, "semantic_validation", cj_str("not_performed"));
    cj *ids = cj_arr();
    for (size_t i = 0; i < artifact_count; i++) {
        cj *item = cj_obj();
        cj_set(item, "artifact_id", cj_str(artifact_ids[i]));
        cj_set(item, "artifact_kind",
               cj_str(artifact_json->items[i]->type == CJ_OBJ
                          ? argument_string(artifact_json->items[i],
                                            "artifact_kind")
                          : ""));
        cj_push(ids, item);
    }
    cj_set(result, "artifacts", ids);
    free(artifact_ids);
    chutni_close(store);
    return result;
}

static cj *tool_source_context(const cj *arguments, int *is_error) {
    const char *store_path = argument_string(arguments, "store_path");
    if (!store_path ||
        (!argument_string(arguments, "source_id") &&
         !argument_string(arguments, "source_path"))) {
        *is_error = 1;
        return tool_error(
            "invalid_arguments",
            "store_path and either source_id or source_path are required.");
    }
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 1, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store for source context", status,
                            NULL);
    }
    source_snapshot source;
    status = source_snapshot_load(store, arguments, &source);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Cannot find source context", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    chutni_artifact_info *artifacts = NULL;
    size_t artifact_count = 0;
    status = chutni_list_artifacts(store, source.source_id, &artifacts,
                                   &artifact_count);
    if (status != CHUTNI_OK) {
        cj *error =
            status_error("Cannot list source artifacts", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    int include_stale = argument_bool(arguments, "include_stale", 0);
    int max_text_chars =
        argument_int(arguments, "max_text_chars", 32768);
    if (max_text_chars < 0) max_text_chars = 0;
    if (max_text_chars > 262144) max_text_chars = 262144;

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "store_path", cj_str(store_path));
    cj *source_json = cj_obj();
    cj_set(source_json, "source_id", cj_str(source.source_id));
    cj_set(source_json, "display_path", cj_str(source.display_path));
    if (source.media_type[0])
        cj_set(source_json, "media_type", cj_str(source.media_type));
    if (source.content_hash[0])
        cj_set(source_json, "content_hash", cj_str(source.content_hash));
    if (source.state[0])
        cj_set(source_json, "state", cj_str(source.state));
    cj_set(source_json, "size_bytes", cj_num((double)source.size_bytes));
    const char *source_freshness = NULL;
    if (chutni_check_freshness(store, source.source_id,
                               &source_freshness) == CHUTNI_OK &&
        source_freshness)
        cj_set(source_json, "freshness", cj_str(source_freshness));
    cj_set(result, "source", source_json);

    cj *items = cj_arr();
    size_t returned = 0;
    for (size_t i = 0; i < artifact_count; i++) {
        if (!include_stale &&
            (!artifacts[i].status ||
             strcmp(artifacts[i].status, "active")))
            continue;
        cj *item = cj_obj();
        cj_set(item, "artifact_id", cj_str(artifacts[i].artifact_id));
        cj_set(item, "artifact_kind", cj_str(artifacts[i].artifact_kind));
        cj_set(item, "artifact_origin", cj_str(artifacts[i].artifact_origin));
        if (artifacts[i].media_type)
            cj_set(item, "media_type", cj_str(artifacts[i].media_type));
        if (artifacts[i].status)
            cj_set(item, "status", cj_str(artifacts[i].status));
        if (artifacts[i].created_at)
            cj_set(item, "created_at", cj_str(artifacts[i].created_at));
        if (artifacts[i].source_content_hash)
            cj_set(item, "source_content_hash",
                   cj_str(artifacts[i].source_content_hash));
        if (artifacts[i].language)
            cj_set(item, "language", cj_str(artifacts[i].language));
        set_json_or_text(item, "selector", artifacts[i].selector_json);
        set_json_or_text(item, "metadata", artifacts[i].metadata_json);
        if (artifacts[i].supersedes_artifact_id)
            cj_set(item, "supersedes_artifact_id",
                   cj_str(artifacts[i].supersedes_artifact_id));
        const char *freshness = NULL;
        if (chutni_check_freshness(store, artifacts[i].artifact_id,
                                   &freshness) == CHUTNI_OK &&
            freshness)
            cj_set(item, "freshness", cj_str(freshness));
        cj_set(item, "semantic_validation", cj_str("not_performed"));

        const char *content = artifacts[i].inline_text;
        void *loaded = NULL;
        size_t content_length = content ? strlen(content) : 0;
        if (!content && artifacts[i].object_hash &&
            artifacts[i].media_type &&
            !strncmp(artifacts[i].media_type, "text/", 5)) {
            if (chutni_object_get(store, artifacts[i].object_hash, &loaded,
                                  &content_length) == CHUTNI_OK)
                content = loaded;
        }
        if (content && max_text_chars > 0) {
            size_t shown = content_length;
            if (shown > (size_t)max_text_chars)
                shown = (size_t)max_text_chars;
            char *bounded = malloc(shown + 1);
            if (bounded) {
                memcpy(bounded, content, shown);
                bounded[shown] = 0;
                cj_set(item, "content", cj_str(bounded));
                cj_set(item, "content_truncated",
                       cj_bool(shown < content_length));
                free(bounded);
            }
        }
        free(loaded);

        cj *provenance = cj_obj();
        cj *producer = cj_obj();
        if (artifacts[i].producer_id)
            cj_set(producer, "producer_id",
                   cj_str(artifacts[i].producer_id));
        if (artifacts[i].producer_kind)
            cj_set(producer, "producer_kind",
                   cj_str(artifacts[i].producer_kind));
        if (artifacts[i].producer_name)
            cj_set(producer, "name", cj_str(artifacts[i].producer_name));
        if (artifacts[i].producer_version)
            cj_set(producer, "version",
                   cj_str(artifacts[i].producer_version));
        if (artifacts[i].model_id)
            cj_set(producer, "model_id", cj_str(artifacts[i].model_id));
        if (artifacts[i].model_revision)
            cj_set(producer, "model_revision",
                   cj_str(artifacts[i].model_revision));
        if (artifacts[i].weights_hash)
            cj_set(producer, "weights_hash",
                   cj_str(artifacts[i].weights_hash));
        if (artifacts[i].quantization)
            cj_set(producer, "quantization",
                   cj_str(artifacts[i].quantization));
        if (artifacts[i].runtime)
            cj_set(producer, "runtime", cj_str(artifacts[i].runtime));
        if (artifacts[i].app_name)
            cj_set(producer, "app_name", cj_str(artifacts[i].app_name));
        if (artifacts[i].app_version)
            cj_set(producer, "app_version",
                   cj_str(artifacts[i].app_version));
        set_json_or_text(producer, "details",
                         artifacts[i].producer_details_json);
        cj_set(provenance, "producer", producer);

        cj *derivation = cj_obj();
        if (artifacts[i].derivation_id)
            cj_set(derivation, "derivation_id",
                   cj_str(artifacts[i].derivation_id));
        if (artifacts[i].operation)
            cj_set(derivation, "operation",
                   cj_str(artifacts[i].operation));
        if (artifacts[i].recipe_hash)
            cj_set(derivation, "recipe_hash",
                   cj_str(artifacts[i].recipe_hash));
        if (artifacts[i].derivation_created_at)
            cj_set(derivation, "created_at",
                   cj_str(artifacts[i].derivation_created_at));
        set_json_or_text(derivation, "parameters",
                         artifacts[i].parameters_json);
        set_json_or_text(derivation, "inputs",
                         artifacts[i].input_refs_json);
        cj_set(provenance, "derivation", derivation);
        cj_set(item, "provenance", provenance);
        cj_push(items, item);
        returned++;
    }
    cj_set(result, "artifact_count", cj_num((double)returned));
    cj_set(result, "artifacts", items);
    chutni_artifact_info_free(artifacts, artifact_count);
    chutni_close(store);
    return result;
}

static cj *tool_put_model_artifact(const cj *arguments, int *is_error) {
    if (!argument_bool(arguments, "confirmed", 0)) {
        *is_error = 1;
        return tool_error(
            "confirmation_required",
            "The host must confirm that this model output should become reusable memory.");
    }
    const char *store_path = argument_string(arguments, "store_path");
    const char *source_path = argument_string(arguments, "source_path");
    const char *text = argument_string(arguments, "text");
    const char *model_id = argument_string(arguments, "model_id");
    const char *model_revision = argument_string(arguments, "model_revision");
    const char *app_name = argument_string(arguments, "app_name");
    const char *app_version = argument_string(arguments, "app_version");
    if (!store_path || !source_path || !text || !model_id ||
        !model_revision || !app_name || !app_version) {
        *is_error = 1;
        return tool_error(
            "invalid_arguments",
            "store_path, source_path, text, model_id, model_revision, app_name, and app_version are required.");
    }

    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 0, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store for artifact write", status,
                            NULL);
    }
    char source_id[CHUTNI_ID_STRLEN];
    status = chutni_source_find(store, source_path, source_id);
    if (status != CHUTNI_OK) {
        cj *error = status_error(
            "The source must already be indexed in this store", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    const char *freshness = NULL;
    status = chutni_source_refresh(store, source_id, &freshness);
    if (status != CHUTNI_OK || !freshness || strcmp(freshness, "current")) {
        cj *error = tool_error(
            "source_not_current",
            "The source is missing or changed; scan it before storing model output.");
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    char source_hash[CHUTNI_HASH_STRLEN];
    status = chutni_hash_file(source_path, source_hash);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Cannot hash source", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    chutni_producer producer;
    memset(&producer, 0, sizeof producer);
    producer.producer_kind = "model";
    producer.name = argument_string(arguments, "producer_name")
                        ? argument_string(arguments, "producer_name")
                        : model_id;
    producer.model_id = model_id;
    producer.model_revision = model_revision;
    producer.weights_hash = argument_string(arguments, "weights_hash");
    producer.quantization = argument_string(arguments, "quantization");
    producer.runtime = argument_string(arguments, "runtime");
    producer.app_name = app_name;
    producer.app_version = app_version;
    char producer_id[CHUTNI_ID_STRLEN];
    status = chutni_producer_put(store, &producer, producer_id);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Cannot record model producer", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    cj *input_array = cj_arr();
    cj *input = cj_obj();
    cj_set(input, "source_id", cj_str(source_id));
    cj_set(input, "source_content_hash", cj_str(source_hash));
    cj_push(input_array, input);
    char *input_refs = cj_dump(input_array, -1);
    cj_free(input_array);
    char *parameters =
        json_value_text(cj_get(arguments, "parameters"), "{}");
    if (!input_refs || !parameters) {
        free(input_refs);
        free(parameters);
        chutni_close(store);
        *is_error = 1;
        return tool_error("out_of_memory", "Cannot encode derivation.");
    }
    char derivation_id[CHUTNI_ID_STRLEN];
    status = chutni_derivation_put(
        store, producer_id,
        argument_string(arguments, "operation")
            ? argument_string(arguments, "operation")
            : "generate_artifact",
        argument_string(arguments, "recipe_hash"), parameters, input_refs,
        derivation_id);
    free(parameters);
    free(input_refs);
    if (status != CHUTNI_OK) {
        cj *error =
            status_error("Cannot record model derivation", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    char *selector_text =
        cj_get(arguments, "selector")
            ? json_value_text(cj_get(arguments, "selector"), NULL)
            : NULL;
    chutni_artifact artifact;
    memset(&artifact, 0, sizeof artifact);
    artifact.source_id = source_id;
    artifact.artifact_kind =
        argument_string(arguments, "artifact_kind")
            ? argument_string(arguments, "artifact_kind")
            : "summary_short";
    artifact.artifact_origin = "model_generated";
    artifact.media_type = "text/plain; charset=utf-8";
    artifact.inline_text = text;
    artifact.selector_json = selector_text;
    artifact.source_content_hash = source_hash;
    artifact.derivation_id = derivation_id;
    artifact.supersedes_artifact_id =
        argument_string(arguments, "supersedes_artifact_id");
    char artifact_id[CHUTNI_ID_STRLEN];
    status = chutni_artifact_put(store, &artifact, artifact_id);
    free(selector_text);
    if (status == CHUTNI_OK) status = chutni_rebuild_indexes(store);
    if (status != CHUTNI_OK) {
        cj *error =
            status_error("Cannot store model artifact", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }
    chutni_close(store);

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "store_path", cj_str(store_path));
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "artifact_id", cj_str(artifact_id));
    cj_set(result, "producer_id", cj_str(producer_id));
    cj_set(result, "derivation_id", cj_str(derivation_id));
    cj_set(result, "semantic_validation", cj_str("not_performed"));
    return result;
}

static cj *dispatch_tool(const char *name, const cj *arguments, int *is_error) {
    *is_error = 0;
    cj *empty = NULL;
    if (!arguments || arguments->type != CJ_OBJ) {
        empty = cj_obj();
        arguments = empty;
    }
    cj *result = NULL;
    if (name && !strcmp(name, "chutni_folder_status"))
        result = tool_folder_status(arguments, is_error);
    else if (name && !strcmp(name, "chutni_folder_activate"))
        result = tool_folder_activate(arguments, is_error);
    else if (name && !strcmp(name, "chutni_discover"))
        result = tool_discover(arguments, is_error);
    else if (name && !strcmp(name, "chutni_capabilities"))
        result = tool_capabilities(arguments, is_error);
    else if (name && !strcmp(name, "chutni_store_info"))
        result = tool_store_info(arguments, is_error);
    else if (name && !strcmp(name, "chutni_scan"))
        result = tool_scan(arguments, is_error);
    else if (name && !strcmp(name, "chutni_search"))
        result = tool_search(arguments, is_error);
    else if (name && !strcmp(name, "chutni_children"))
        result = tool_children(arguments, is_error);
    else if (name && !strcmp(name, "chutni_observe_directory"))
        result = tool_observe_directory(arguments, is_error);
    else if (name && !strcmp(name, "chutni_coverage"))
        result = tool_coverage(arguments, is_error);
    else if (name && !strcmp(name, "chutni_source_context"))
        result = tool_source_context(arguments, is_error);
    else if (name && !strcmp(name, "chutni_put_artifacts"))
        result = tool_put_artifacts(arguments, is_error);
    else if (name && !strcmp(name, "chutni_put_model_artifact"))
        result = tool_put_model_artifact(arguments, is_error);
    else {
        *is_error = 1;
        result = tool_error("unknown_tool", "The requested Chutni tool does not exist.");
    }
    cj_free(empty);
    return result;
}

static cj *schema_string(const char *description) {
    cj *schema = cj_obj();
    cj_set(schema, "type", cj_str("string"));
    if (description) cj_set(schema, "description", cj_str(description));
    return schema;
}

static cj *schema_boolean(const char *description) {
    cj *schema = cj_obj();
    cj_set(schema, "type", cj_str("boolean"));
    if (description) cj_set(schema, "description", cj_str(description));
    return schema;
}

static cj *schema_integer(const char *description, int minimum, int maximum) {
    cj *schema = cj_obj();
    cj_set(schema, "type", cj_str("integer"));
    if (description) cj_set(schema, "description", cj_str(description));
    if (minimum >= 0) cj_set(schema, "minimum", cj_num((double)minimum));
    if (maximum >= 0) cj_set(schema, "maximum", cj_num((double)maximum));
    return schema;
}

static cj *input_schema(cj *properties, const char *const *required) {
    cj *schema = cj_obj();
    cj_set(schema, "type", cj_str("object"));
    cj_set(schema, "properties", properties);
    cj_set(schema, "additionalProperties", cj_bool(0));
    if (required) {
        cj *items = cj_arr();
        for (const char *const *name = required; *name; name++)
            cj_push(items, cj_str(*name));
        cj_set(schema, "required", items);
    }
    return schema;
}

static cj *tool_definition(const char *name, const char *title,
                           const char *description, cj *schema,
                           int read_only, int destructive, int idempotent) {
    cj *tool = cj_obj();
    cj_set(tool, "name", cj_str(name));
    cj_set(tool, "title", cj_str(title));
    cj_set(tool, "description", cj_str(description));
    cj_set(tool, "inputSchema", schema);
    cj *annotations = cj_obj();
    cj_set(annotations, "readOnlyHint", cj_bool(read_only));
    cj_set(annotations, "destructiveHint", cj_bool(destructive));
    cj_set(annotations, "idempotentHint", cj_bool(idempotent));
    cj_set(annotations, "openWorldHint", cj_bool(0));
    cj_set(tool, "annotations", annotations);
    return tool;
}

static cj *tools_list(void) {
    cj *result = cj_obj();
    cj *tools = cj_arr();

    {
        cj *properties = cj_obj();
        cj_set(properties, "path",
               schema_string("A user-selected source directory or Chutni store path."));
        const char *required[] = {"path", NULL};
        cj_push(tools, tool_definition(
            "chutni_folder_status", "Check Chutni folder",
            "Classify a user-selected directory and report whether its adjacent P.chutni store will be opened, created, or refused. This does not scan or write.",
            input_schema(properties, required), 1, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "path",
               schema_string("The exact source directory or store path the user selected."));
        cj_set(properties, "confirmed",
               schema_boolean("Must be true only after the user approves the reported store path and scan."));
        cj_set(properties, "label", schema_string("Optional user-visible label."));
        cj_set(properties, "register",
               schema_boolean("Register a newly created store for discovery."));
        cj_set(properties, "app_name",
               schema_string("Host application name recorded in scanner provenance."));
        cj_set(properties, "app_version",
               schema_string("Host application version recorded in scanner provenance."));
        cj_set(properties, "max_file_size_bytes",
               schema_integer("Per-file scanner safety cap.", 1, -1));
        cj_set(properties, "max_depth",
               schema_integer("Depth bound recorded on the new root. The selected folder is depth 0; a directory at depth d is enumerated only when d <= max_depth, and deeper directories are recorded by name without being opened. Omit for unbounded recursion.", 0, -1));
        cj_set(properties, "memory_goal",
               schema_string("Why this memory is being built, e.g. \"define\". Recorded on the root and echoed in every coverage manifest."));
        cj_set(properties, "definition_mode",
               schema_string("\"adaptive\" lets one directory definition represent a coherent directory; \"per_source\" requires every reached supported file to receive an artifact or an explicit processing status."));
        const char *required[] = {"path", "confirmed", NULL};
        cj_push(tools, tool_definition(
            "chutni_folder_activate", "Create or open Chutni memory",
            "After explicit user confirmation, create or open the adjacent P.chutni store, authorize P as a root, and scan it using the shared reference scanner.",
            input_schema(properties, required), 0, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_push(tools, tool_definition(
            "chutni_discover", "Discover Chutni stores",
            "List stores from Chutni's bounded environment, registry, and conventional-location discovery.",
            input_schema(properties, NULL), 1, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_push(tools, tool_definition(
            "chutni_capabilities", "Inspect Chutni capabilities",
            "Report protocol/service versions, transports, artifact origins and kinds, selector types, writer policy, semantic-validation boundary, and the exact limits of the reference scanner.",
            input_schema(properties, NULL), 1, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        const char *required[] = {"store_path", NULL};
        cj_push(tools, tool_definition(
            "chutni_store_info", "Inspect Chutni store",
            "Return store identity, roots, and source/artifact/object/provenance counts.",
            input_schema(properties, required), 1, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        cj_set(properties, "confirmed",
               schema_boolean("Must be true after the user requests or approves the rescan."));
        cj_set(properties, "app_name",
               schema_string("Host application name recorded in scanner provenance."));
        cj_set(properties, "app_version",
               schema_string("Host application version recorded in scanner provenance."));
        cj_set(properties, "max_file_size_bytes",
               schema_integer("Per-file scanner safety cap.", 1, -1));
        cj_set(properties, "max_depth",
               schema_integer("Override the root's stored depth bound for this run only. Omit to use the policy the user authorized.", 0, -1));
        const char *required[] = {"store_path", "confirmed", NULL};
        cj_push(tools, tool_definition(
            "chutni_scan", "Update Chutni store",
            "Rescan only the roots already authorized in a store, reuse unchanged artifacts, retire changed artifacts, and rebuild disposable indexes. Reports directories and files separately, and marks sources missing only inside the region this scan actually covered.",
            input_schema(properties, required), 0, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        cj_set(properties, "source_id",
               schema_string("The directory source to list."));
        cj_set(properties, "source_path",
               schema_string("Alternative to source_id: a path this store already knows."));
        const char *required[] = {"store_path", NULL};
        cj_push(tools, tool_definition(
            "chutni_children", "List directory children",
            "List a directory source's immediate children. Each child reports its source_kind and, for directories, whether it was enumerated or is still opaque — named but never opened.",
            input_schema(properties, required), 1, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        cj_set(properties, "source_id",
               schema_string("The directory source to enumerate."));
        cj_set(properties, "source_path",
               schema_string("Alternative to source_id: a path this store already knows."));
        cj_set(properties, "confirmed",
               schema_boolean("Must be true after the user approves reading this directory."));
        cj_set(properties, "app_name",
               schema_string("Host application name recorded in scanner provenance."));
        cj_set(properties, "app_version",
               schema_string("Host application version recorded in scanner provenance."));
        const char *required[] = {"store_path", "confirmed", NULL};
        cj_push(tools, tool_definition(
            "chutni_observe_directory", "Open one directory",
            "Enumerate exactly one already-authorized directory: record its immediate files and child-directory names, and recurse into nothing. Call it again per child to go deeper, so the decision about how far to read stays with the host rather than being taken by a walk.",
            input_schema(properties, required), 0, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        cj_set(properties, "root_id", schema_string("A root to report coverage for."));
        cj_set(properties, "source_id",
               schema_string("A source whose region's coverage should be reported."));
        cj_set(properties, "source_path",
               schema_string("Alternative to source_id: a path this store already knows."));
        const char *required[] = {"store_path", NULL};
        cj_push(tools, tool_definition(
            "chutni_coverage", "Read scan coverage",
            "Report what a scan actually reached: the depth requested, the depth achieved, how many directories were enumerated versus recorded and never opened, and whether the bounded operation completed. Read this before describing a store's contents as complete.",
            input_schema(properties, required), 1, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        cj_set(properties, "query", schema_string("Lexical search query."));
        cj_set(properties, "limit",
               schema_integer("Maximum number of results.", 1, 100));
        cj_set(properties, "include_stale",
               schema_boolean("Include stale artifacts; false by default."));
        cj_set(properties, "match_any",
               schema_boolean("Match any literal query term instead of requiring every term; false by default."));
        const char *required[] = {"store_path", "query", NULL};
        cj_push(tools, tool_definition(
            "chutni_search", "Search Chutni memory",
            "Search active artifacts and return bounded snippets with source paths, selectors, freshness, provenance IDs, and honest score types.",
            input_schema(properties, required), 1, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        cj_set(properties, "source_id",
               schema_string("Source identifier returned by search or submission."));
        cj_set(properties, "source_path",
               schema_string("Absolute path of a source in the store; use this when source_id is unavailable."));
        cj_set(properties, "include_stale",
               schema_boolean("Include stale and superseded historical artifacts; false by default."));
        cj_set(properties, "max_text_chars",
               schema_integer("Maximum text characters returned per artifact; zero omits payload text.", 0, 262144));
        const char *required[] = {"store_path", NULL};
        cj_push(tools, tool_definition(
            "chutni_source_context", "Read all interpretations of a source",
            "Return a source and its current artifacts together, including creation time, selectors, processing operation, producer/model/application provenance, and an explicit statement that Chutni did not verify semantic truth.",
            input_schema(properties, required), 1, 0, 1));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        cj_set(properties, "source_id",
               schema_string("Existing source identifier."));
        cj_set(properties, "source_path",
               schema_string("Absolute path of an existing indexed source."));
        cj_set(properties, "source_content_hash",
               schema_string("Exact BLAKE3 hash of the source bytes used to create every artifact."));

        cj *producer_properties = cj_obj();
        cj_set(producer_properties, "producer_kind",
               schema_string("parser, model, application, human, pipeline, or unknown."));
        cj_set(producer_properties, "name",
               schema_string("Human-readable producer identity."));
        cj_set(producer_properties, "version", schema_string("Producer version."));
        cj_set(producer_properties, "model_id", schema_string("Exact model identifier."));
        cj_set(producer_properties, "model_revision", schema_string("Model revision."));
        cj_set(producer_properties, "weights_hash", schema_string("Model weights hash."));
        cj_set(producer_properties, "quantization", schema_string("Model quantization."));
        cj_set(producer_properties, "runtime", schema_string("Runtime used."));
        cj_set(producer_properties, "app_name", schema_string("Host application name."));
        cj_set(producer_properties, "app_version", schema_string("Host application version."));
        cj *producer_details = cj_obj();
        cj_set(producer_details, "type", cj_str("object"));
        cj_set(producer_properties, "details", producer_details);
        const char *producer_required[] = {
            "producer_kind", "name", NULL
        };
        cj *producer_schema =
            input_schema(producer_properties, producer_required);
        cj_set(properties, "producer", producer_schema);

        cj_set(properties, "operation",
               schema_string("Processing method, such as pdf_text_extract, ocr, image_caption, transcribe, or summarize."));
        cj_set(properties, "recipe_hash",
               schema_string("Optional hash or stable identifier for the extraction recipe or prompt."));
        cj *parameters = cj_obj();
        cj_set(parameters, "type", cj_str("object"));
        cj_set(properties, "parameters", parameters);
        cj *inputs = cj_obj();
        cj_set(inputs, "type", cj_str("array"));
        cj_set(properties, "inputs", inputs);

        cj *artifact_properties = cj_obj();
        cj_set(artifact_properties, "artifact_kind",
               schema_string("Core or namespaced artifact kind."));
        cj_set(artifact_properties, "artifact_origin",
               schema_string("direct, deterministic_transform, model_generated, or human."));
        cj_set(artifact_properties, "text",
               schema_string("Artifact payload. Chutni records provenance but does not judge its truth."));
        cj_set(artifact_properties, "media_type",
               schema_string("Payload media type; defaults to UTF-8 text."));
        cj_set(artifact_properties, "language",
               schema_string("Optional BCP 47 language tag."));
        cj *selector = cj_obj();
        cj_set(selector, "type", cj_str("object"));
        cj_set(artifact_properties, "selector", selector);
        cj *metadata = cj_obj();
        cj_set(metadata, "type", cj_str("object"));
        cj_set(artifact_properties, "metadata", metadata);
        cj_set(artifact_properties, "supersedes_artifact_id",
               schema_string("Optional prior artifact from the same source explicitly superseded by this producer."));
        const char *artifact_required[] = {
            "artifact_kind", "artifact_origin", "text", NULL
        };
        cj *artifact_item_schema =
            input_schema(artifact_properties, artifact_required);
        cj *artifact_array = cj_obj();
        cj_set(artifact_array, "type", cj_str("array"));
        cj_set(artifact_array, "items", artifact_item_schema);
        cj_set(artifact_array, "minItems", cj_num(1));
        cj_set(artifact_array, "maxItems", cj_num(128));
        cj_set(properties, "artifacts", artifact_array);
        cj_set(properties, "confirmed",
               schema_boolean("Must be true when the host intends to retain these outputs as reusable memory."));
        const char *required[] = {
            "store_path", "source_content_hash", "producer", "operation",
            "artifacts", "confirmed", NULL
        };
        cj_push(tools, tool_definition(
            "chutni_put_artifacts", "Store processed artifacts",
            "Atomically store one or more host-produced artifacts with exact source-version binding and complete processing provenance. Chutni validates structure and integrity only; it never verifies whether OCR, captions, summaries, or other claims are semantically true.",
            input_schema(properties, required), 0, 0, 0));
    }
    {
        cj *properties = cj_obj();
        cj_set(properties, "store_path",
               schema_string("Absolute path to a .chutni store."));
        cj_set(properties, "source_path",
               schema_string("Absolute path of a source already indexed in the store."));
        cj_set(properties, "text",
               schema_string("Model-generated artifact text to retain."));
        cj_set(properties, "artifact_kind",
               schema_string("Core or namespaced artifact kind; defaults to summary_short."));
        cj_set(properties, "model_id", schema_string("Exact model identifier."));
        cj_set(properties, "model_revision",
               schema_string("Exact model revision."));
        cj_set(properties, "producer_name",
               schema_string("Human-readable model or pipeline name."));
        cj_set(properties, "weights_hash",
               schema_string("Optional model weights hash."));
        cj_set(properties, "quantization",
               schema_string("Optional quantization identifier."));
        cj_set(properties, "runtime",
               schema_string("Runtime used for generation."));
        cj_set(properties, "app_name",
               schema_string("Host application committing the artifact."));
        cj_set(properties, "app_version",
               schema_string("Host application version."));
        cj_set(properties, "operation",
               schema_string("Derivation operation; defaults to generate_artifact."));
        cj_set(properties, "recipe_hash",
               schema_string("Hash or stable identifier for the prompt/recipe."));
        cj *parameters = cj_obj();
        cj_set(parameters, "type", cj_str("object"));
        cj_set(properties, "parameters", parameters);
        cj *selector = cj_obj();
        cj_set(selector, "type", cj_str("object"));
        cj_set(properties, "selector", selector);
        cj_set(properties, "supersedes_artifact_id",
               schema_string("Optional prior artifact superseded by this one."));
        cj_set(properties, "confirmed",
               schema_boolean("Must be true when the host intends to retain this output as reusable memory."));
        const char *required[] = {
            "store_path", "source_path", "text", "model_id",
            "model_revision", "app_name", "app_version", "confirmed", NULL
        };
        cj_push(tools, tool_definition(
            "chutni_put_model_artifact", "Store model artifact",
            "Commit explicitly approved model output with exact source freshness, producer identity, derivation parameters, and provenance. This never writes the original source.",
            input_schema(properties, required), 0, 0, 0));
    }
    cj_set(result, "tools", tools);
    return result;
}

static cj *server_capabilities(void) {
    cj *capabilities = cj_obj();
    cj_set(capabilities, "tools", cj_obj());
    return capabilities;
}

static cj *server_info(void) {
    cj *info = cj_obj();
    cj_set(info, "name", cj_str("chutni-mcp"));
    cj_set(info, "title", cj_str("Chutni local memory"));
    cj_set(info, "version", cj_str(CHUTNI_MCP_VERSION));
    return info;
}

static int known_legacy_version(const char *version) {
    return version &&
           (!strcmp(version, "2024-11-05") ||
            !strcmp(version, "2025-03-26") ||
            !strcmp(version, "2025-06-18") ||
            !strcmp(version, "2025-11-25"));
}

static cj *initialize_result(const cj *request) {
    const cj *params = cj_get(request, "params");
    const char *requested = cj_get_str(params, "protocolVersion");
    cj *result = cj_obj();
    cj_set(result, "protocolVersion",
           cj_str(known_legacy_version(requested) ? requested :
                  MCP_LEGACY_VERSION));
    cj_set(result, "capabilities", server_capabilities());
    cj_set(result, "serverInfo", server_info());
    cj_set(result, "instructions",
           cj_str("Use chutni_folder_status before activation. Never create, scan, or retain model output without the user's explicit approval."));
    return result;
}

static cj *discover_result(void) {
    cj *result = cj_obj();
    cj *versions = cj_arr();
    cj_push(versions, cj_str(MCP_CURRENT_VERSION));
    cj_push(versions, cj_str(MCP_LEGACY_VERSION));
    cj_push(versions, cj_str("2025-06-18"));
    cj_set(result, "supportedVersions", versions);
    cj_set(result, "capabilities", server_capabilities());
    cj_set(result, "instructions",
           cj_str("Local Chutni tools for user-approved folder memory."));
    cj_set(result, "ttlMs", cj_num(300000));
    cj_set(result, "cacheScope", cj_str("private"));
    cj_set(result, "resultType", cj_str("result"));
    cj *meta = cj_obj();
    cj_set(meta, "io.modelcontextprotocol/serverInfo", server_info());
    cj_set(result, "_meta", meta);
    return result;
}

static cj *tool_call_result(cj *payload, int is_error) {
    cj *result = cj_obj();
    char *text = cj_dump(payload, -1);
    cj *content = cj_arr();
    cj *block = cj_obj();
    cj_set(block, "type", cj_str("text"));
    cj_set(block, "text", cj_str(text ? text : "{}"));
    cj_push(content, block);
    cj_set(result, "content", content);
    cj *structured = json_clone(payload);
    if (structured) cj_set(result, "structuredContent", structured);
    if (is_error) cj_set(result, "isError", cj_bool(1));
    free(text);
    cj_free(payload);
    return result;
}

static cj *response_base(const cj *request) {
    cj *response = cj_obj();
    cj_set(response, "jsonrpc", cj_str("2.0"));
    cj *id = json_clone(cj_get(request, "id"));
    cj_set(response, "id", id ? id : cj_null());
    return response;
}

static cj *protocol_error(const cj *request, int code, const char *message) {
    cj *response = response_base(request);
    cj *error = cj_obj();
    cj_set(error, "code", cj_num((double)code));
    cj_set(error, "message", cj_str(message));
    cj_set(response, "error", error);
    return response;
}

static cj *handle_request(const cj *request) {
    if (!request || request->type != CJ_OBJ)
        return protocol_error(request, -32600, "Invalid Request");
    const char *jsonrpc = cj_get_str(request, "jsonrpc");
    const char *method = cj_get_str(request, "method");
    if (!jsonrpc || strcmp(jsonrpc, "2.0") || !method)
        return protocol_error(request, -32600, "Invalid Request");

    cj *response = response_base(request);
    cj *result = NULL;
    if (!strcmp(method, "initialize")) {
        result = initialize_result(request);
    } else if (!strcmp(method, "server/discover")) {
        result = discover_result();
    } else if (!strcmp(method, "ping")) {
        result = cj_obj();
    } else if (!strcmp(method, "tools/list")) {
        result = tools_list();
    } else if (!strcmp(method, "tools/call")) {
        cj *params = cj_get(request, "params");
        const char *name = cj_get_str(params, "name");
        cj *arguments = cj_get(params, "arguments");
        if (!name) {
            cj_free(response);
            return protocol_error(request, -32602, "Tool name is required.");
        }
        int is_error = 0;
        cj *payload = dispatch_tool(name, arguments, &is_error);
        if (!payload) {
            cj_free(response);
            return protocol_error(request, -32603, "Internal error");
        }
        result = tool_call_result(payload, is_error);
    } else {
        cj_free(response);
        return protocol_error(request, -32601, "Method not found");
    }
    cj_set(response, "result", result);
    return response;
}

static int emit_json_line(cj *value) {
    char *text = cj_dump(value, -1);
    if (!text) {
        cj_free(value);
        return 0;
    }
    int ok = printf("%s\n", text) >= 0 && fflush(stdout) == 0;
    free(text);
    cj_free(value);
    return ok;
}

static int stdio_server(void) {
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, stdin) >= 0) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' ||
                          line[length - 1] == '\r'))
            line[--length] = 0;
        if (!length) continue;
        const char *parse_error = NULL;
        cj *request = cj_parse(line, &parse_error);
        if (!request) {
            cj *synthetic = cj_obj();
            cj_set(synthetic, "jsonrpc", cj_str("2.0"));
            cj_set(synthetic, "id", cj_null());
            cj *response =
                protocol_error(synthetic, -32700,
                               parse_error ? parse_error : "Parse error");
            cj_free(synthetic);
            if (!emit_json_line(response)) {
                free(line);
                return 1;
            }
            continue;
        }
        /* Notifications have no id and receive no response. */
        if (!cj_get(request, "id")) {
            cj_free(request);
            continue;
        }
        cj *response = handle_request(request);
        cj_free(request);
        if (!emit_json_line(response)) {
            free(line);
            return 1;
        }
    }
    free(line);
    return ferror(stdin) ? 1 : 0;
}

static int one_shot_call(const char *name, const char *json_arguments) {
    const char *parse_error = NULL;
    cj *arguments = cj_parse(json_arguments ? json_arguments : "{}",
                             &parse_error);
    if (!arguments || arguments->type != CJ_OBJ) {
        fprintf(stderr, "chutni-mcp: invalid arguments JSON: %s\n",
                parse_error ? parse_error : "expected object");
        cj_free(arguments);
        return 2;
    }
    int is_error = 0;
    cj *result = dispatch_tool(name, arguments, &is_error);
    cj_free(arguments);
    if (!result || !emit_json_line(result)) return 1;
    return is_error ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--stdio")))
        return stdio_server();
    if (argc == 4 && !strcmp(argv[1], "--call"))
        return one_shot_call(argv[2], argv[3]);
    fprintf(stderr,
            "usage:\n"
            "  chutni-mcp [--stdio]\n"
            "  chutni-mcp --call <tool-name> '<json-arguments>'\n");
    return 2;
}
