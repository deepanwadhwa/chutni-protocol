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

/* T01: every operation whose JSON shape is identical to a chutni_call (§20)
 * operation delegates to it through this one function, so result
 * construction for those operations lives in the library exactly once
 * rather than twice. `store` may be NULL for the two store-less operations.
 *
 * On failure, chutni_call's envelope ({"error":{"code","message"}}) is
 * reshaped into this service's existing flat tool_error shape
 * ({"ok":false,"error":"<code>","message":"<detail>"}), so nothing a client
 * already depends on changes: confirmation gating and store resolution
 * still happen in the caller, above this function, exactly as before. */
static cj *jcall_dispatch(chutni_store *store, const char *op,
                          const cj *arguments, int *is_error) {
    char *args_text = cj_dump(arguments, -1);
    char *result_text = NULL;
    chutni_status status = chutni_call(store, op, args_text ? args_text : "{}",
                                       &result_text);
    free(args_text);
    cj *result = result_text ? cj_parse(result_text, NULL) : NULL;
    chutni_free(result_text);
    if (status == CHUTNI_OK) return result;

    *is_error = 1;
    cj *error_obj = cj_get(result, "error");
    cj *out = tool_error(error_obj ? cj_get_str(error_obj, "code") : NULL,
                         error_obj ? cj_get_str(error_obj, "message") : NULL);
    cj_free(result);
    return out;
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

static void report_scan_progress(const chutni_scan_result *scan,
                                 const char *current_path, void *userdata) {
    FILE *stream = userdata ? (FILE *)userdata : stderr;
    cj *event = cj_obj();
    if (!event) return;
    cj_set(event, "type", cj_str("scan_progress"));
    cj_set(event, "files_seen", cj_num((double)scan->files_seen));
    cj_set(event, "sources_indexed", cj_num((double)scan->sources_indexed));
    cj_set(event, "unchanged", cj_num((double)scan->unchanged));
    cj_set(event, "text_artifacts", cj_num((double)scan->text_artifacts));
    cj_set(event, "metadata_artifacts",
           cj_num((double)scan->metadata_artifacts));
    cj_set(event, "skipped", cj_num((double)scan->skipped));
    cj_set(event, "errors", cj_num((double)scan->errors));
    if (current_path)
        cj_set(event, "current_path", cj_str(current_path));
    char *line = cj_dump(event, -1);
    cj_free(event);
    if (!line) return;
    fprintf(stream, "%s\n", line);
    fflush(stream);
    free(line);
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
    cj_set(value, "content_artifacts",
           cj_num((double)counts.content_artifacts));
    cj_set(value, "metadata_artifacts",
           cj_num((double)counts.metadata_artifacts));
    cj_set(value, "content_readable_sources",
           cj_num((double)counts.content_readable_sources));
    cj_set(value, "metadata_only_sources",
           cj_num((double)counts.metadata_only_sources));
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
    if (argument_bool(arguments, "report_progress", 0)) {
        options.progress_callback = report_scan_progress;
        options.progress_userdata = stderr;
    }
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
    return jcall_dispatch(NULL, "discover", arguments, is_error);
}

/* T01: the protocol/library capability facts (spec version, artifact kinds,
 * ...) come from chutni_call's "capabilities" operation — meaningful to any
 * consumer, not just this stdio service. What's added on top here is
 * genuinely service-level: which transports this process speaks and what
 * this particular reference scanner build can and cannot extract. */
static cj *tool_capabilities(const cj *arguments, int *is_error) {
    cj *result = jcall_dispatch(NULL, "capabilities", arguments, is_error);
    if (*is_error) return result;

    cj_set(result, "service_version", cj_str(CHUTNI_MCP_VERSION));
    cj *transports = cj_arr();
    cj_push(transports, cj_str("mcp_stdio"));
    cj_push(transports, cj_str("one_shot_json"));
    cj_set(result, "transports", transports);

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

static cj *tool_store_info(const cj *arguments, int *is_error) {
    const char *store_path = argument_string(arguments, "store_path");
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 1, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store", status, NULL);
    }
    cj *result = jcall_dispatch(store, "store_info", arguments, is_error);
    chutni_close(store);
    return result;
}

/* Applications need a bounded file inventory for enrichment and for
 * inventory-shaped questions that lexical search cannot answer. Hierarchical
 * Chutni stores also contain directory sources; those are excluded here so
 * existing hosts see the same file-only contract they used before v0.2. */
static cj *tool_list_sources(const cj *arguments, int *is_error) {
    const char *store_path = argument_string(arguments, "store_path");
    const char *source_path = argument_string(arguments, "source_path");
    if (!store_path || !source_path || !*source_path) {
        *is_error = 1;
        return tool_error(
            "invalid_arguments",
            "store_path and the selected source_path are required.");
    }
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 1, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store for source listing", status,
                            NULL);
    }
    char root_id[CHUTNI_ID_STRLEN];
    if (!root_matches_source(store, source_path, root_id)) {
        chutni_close(store);
        *is_error = 1;
        return tool_error(
            "root_not_authorized",
            "The selected source path is not an authorized root in this store.");
    }
    chutni_source_info *sources = NULL;
    size_t source_count = 0;
    status = chutni_sources_list(store, root_id, &sources, &source_count);
    if (status != CHUTNI_OK) {
        cj *error = status_error("Cannot list store sources", status, store);
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    size_t file_count = 0;
    for (size_t i = 0; i < source_count; ++i)
        if (!sources[i].source_kind ||
            !strcmp(sources[i].source_kind, "file"))
            file_count++;

    int limit = argument_int(arguments, "limit", 50);
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;
    int offset_value = argument_int(arguments, "offset", 0);
    if (offset_value < 0) offset_value = 0;
    size_t offset =
        (size_t)offset_value < file_count ? (size_t)offset_value : file_count;
    size_t wanted =
        file_count - offset < (size_t)limit ? file_count - offset : (size_t)limit;

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "store_path", cj_str(store_path));
    cj_set(result, "source_path", cj_str(source_path));
    cj_set(result, "count", cj_num((double)file_count));
    cj_set(result, "offset", cj_num((double)offset));
    cj_set(result, "returned", cj_num((double)wanted));
    cj_set(result, "truncated", cj_bool(offset + wanted < file_count));
    cj *items = cj_arr();
    size_t seen_files = 0, returned = 0;
    for (size_t i = 0; i < source_count && returned < wanted; ++i) {
        if (sources[i].source_kind &&
            strcmp(sources[i].source_kind, "file"))
            continue;
        if (seen_files++ < offset) continue;
        cj *item = cj_obj();
        if (sources[i].source_id)
            cj_set(item, "source_id", cj_str(sources[i].source_id));
        if (sources[i].display_path)
            cj_set(item, "display_path", cj_str(sources[i].display_path));
        if (sources[i].media_type)
            cj_set(item, "media_type", cj_str(sources[i].media_type));
        if (sources[i].state)
            cj_set(item, "state", cj_str(sources[i].state));
        cj_set(item, "size_bytes", cj_num((double)sources[i].size_bytes));
        cj_push(items, item);
        returned++;
    }
    cj_set(result, "sources", items);
    chutni_source_info_free(sources, source_count);
    chutni_close(store);
    return result;
}

static char *json_value_text(const cj *value) {
    return value ? cj_dump(value, -1) : NULL;
}

/* Find the active artifact produced by the same operation for the same
 * source slice. Exact repeats are reused; changed output supersedes the prior
 * artifact. This makes refresh safe and bounded for native host integrations. */
static char *matching_current_artifact(
    chutni_store *store, const char *source_id, const char *artifact_kind,
    const char *producer_name, const char *producer_kind,
    const char *model_id, const char *model_revision, const char *operation,
    const char *source_hash, const char *selector_json, const char *text,
    int *reused) {
    *reused = 0;
    chutni_artifact_info *artifacts = NULL;
    size_t count = 0;
    if (chutni_list_artifacts(store, source_id, &artifacts, &count) != CHUTNI_OK)
        return NULL;
    char *prior = NULL;
    for (size_t i = 0; i < count; ++i) {
        chutni_artifact_info *item = &artifacts[i];
        if (!item->status || strcmp(item->status, "active") ||
            !item->artifact_kind || strcmp(item->artifact_kind, artifact_kind) ||
            !item->producer_name || strcmp(item->producer_name, producer_name) ||
            !item->producer_kind || strcmp(item->producer_kind, producer_kind) ||
            !item->operation || strcmp(item->operation, operation))
            continue;
        if (model_id && (!item->model_id || strcmp(item->model_id, model_id)))
            continue;
        if (model_revision &&
            (!item->model_revision ||
             strcmp(item->model_revision, model_revision)))
            continue;
        if ((selector_json || item->selector_json) &&
            (!selector_json || !item->selector_json ||
             strcmp(item->selector_json, selector_json)))
            continue;
        free(prior);
        prior = item->artifact_id ? strdup(item->artifact_id) : NULL;
        if (item->inline_text && !strcmp(item->inline_text, text) &&
            item->source_content_hash &&
            !strcmp(item->source_content_hash, source_hash)) {
            *reused = 1;
            break;
        }
    }
    chutni_artifact_info_free(artifacts, count);
    return prior;
}

static cj *reused_artifact_json(const char *store_path, const char *source_id,
                                const char *artifact_id) {
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "reused", cj_bool(1));
    cj_set(result, "store_path", cj_str(store_path));
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "artifact_id", cj_str(artifact_id));
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
    if (argument_bool(arguments, "report_progress", 0)) {
        chutni_scan_options options;
        memset(&options, 0, sizeof options);
        options.app_name = argument_string(arguments, "app_name");
        options.app_version = argument_string(arguments, "app_version");
        options.progress_callback = report_scan_progress;
        options.progress_userdata = stderr;
        cj *max_value = cj_get(arguments, "max_file_size_bytes");
        if (max_value && max_value->type == CJ_NUM && max_value->num > 0)
            options.max_file_size_bytes = (uint64_t)max_value->num;
        cj *depth_value = cj_get(arguments, "max_depth");
        if (depth_value && depth_value->type == CJ_NUM &&
            depth_value->num >= 0) {
            options.override_max_depth = (int)depth_value->num;
            options.use_override_max_depth = 1;
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
            cj *error = status_error(
                "Cannot read store counts", CHUTNI_ERR_DB, store);
            chutni_close(store);
            *is_error = 1;
            return error;
        }
        chutni_close(store);
        return result;
    }
    cj *result = jcall_dispatch(store, "scan", arguments, is_error);
    chutni_close(store);
    return result;
}

static cj *tool_children(const cj *arguments, int *is_error) {
    const char *store_path = argument_string(arguments, "store_path");
    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 1, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store", status, NULL);
    }
    cj *result = jcall_dispatch(store, "children", arguments, is_error);
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
    cj *result = jcall_dispatch(store, "observe_directory", arguments, is_error);
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
    cj *result = jcall_dispatch(store, "coverage", arguments, is_error);
    chutni_close(store);
    return result;
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
    cj *result = jcall_dispatch(store, "search", arguments, is_error);
    chutni_close(store);
    return result;
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

/* T01: the source resolution, refresh, and expected-hash comparison below
 * stay here rather than moving into chutni_call's lower-level "put_artifacts"
 * operation. They are a host-level safety gate — "does the caller's belief
 * about the source's version match reality, with one friendly top-level
 * error" — not part of the write itself: chutni_artifacts_put (via
 * chutni_call) already refuses any artifact whose source_content_hash does
 * not match at write time (§13.3); this is a nicer error in front of that,
 * appropriate here because chutni-mcp's callers only ever write one source
 * per call. What does move to chutni_call is everything after the gate:
 * producer/derivation/artifact construction and the actual write. */
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

    cj *details = cj_get(producer_json, "details");
    if (details && details->type != CJ_OBJ) {
        chutni_close(store);
        *is_error = 1;
        return tool_error("invalid_arguments",
                          "producer.details must be an object.");
    }
    cj *parameters = cj_get(arguments, "parameters");
    if (parameters && parameters->type != CJ_OBJ) {
        chutni_close(store);
        *is_error = 1;
        return tool_error("invalid_arguments",
                          "parameters must be an object.");
    }
    cj *inputs = cj_get(arguments, "inputs");
    if (inputs && inputs->type != CJ_ARR) {
        chutni_close(store);
        *is_error = 1;
        return tool_error("invalid_arguments", "inputs must be an array.");
    }

    /* Build the lower-level call_args object chutni_call's "put_artifacts"
       expects: each artifact carries its own source_id/source_content_hash,
       both filled in here from the resolved, verified snapshot above. */
    cj *call_args = cj_obj();
    cj_set(call_args, "operation", cj_str(operation));
    const char *recipe_hash = argument_string(arguments, "recipe_hash");
    if (recipe_hash) cj_set(call_args, "recipe_hash", cj_str(recipe_hash));
    cj_set(call_args, "producer", json_clone(producer_json));
    if (parameters) cj_set(call_args, "parameters", json_clone(parameters));
    if (inputs) {
        cj_set(call_args, "inputs", json_clone(inputs));
    } else {
        cj *default_inputs = cj_arr();
        cj *input = cj_obj();
        cj_set(input, "source_id", cj_str(source.source_id));
        cj_set(input, "source_content_hash", cj_str(source.content_hash));
        cj_push(default_inputs, input);
        cj_set(call_args, "inputs", default_inputs);
    }

    cj *artifacts_out = cj_arr();
    int valid = 1;
    for (size_t i = 0; i < artifact_json->n; i++) {
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
        cj *out_item = json_clone(item);
        cj_set(out_item, "source_id", cj_str(source.source_id));
        cj_set(out_item, "source_content_hash", cj_str(source.content_hash));
        if (!argument_string(item, "media_type"))
            cj_set(out_item, "media_type", cj_str("text/plain; charset=utf-8"));
        cj_push(artifacts_out, out_item);
    }
    if (!valid) {
        cj_free(call_args);
        cj_free(artifacts_out);
        chutni_close(store);
        *is_error = 1;
        return tool_error(
            "invalid_arguments",
            "Each artifact requires text, artifact_kind, and artifact_origin; selector and metadata must be objects.");
    }
    cj_set(call_args, "artifacts", artifacts_out);

    cj *result = jcall_dispatch(store, "put_artifacts", call_args, is_error);
    cj_free(call_args);
    if (!*is_error) {
        cj_set(result, "store_path", cj_str(store_path));
        cj_set(result, "source_id", cj_str(source.source_id));
        cj_set(result, "source_content_hash", cj_str(source.content_hash));
    }
    chutni_close(store);
    return result;
}

/* Compatibility convenience for native hosts such as Samosa. It resolves
 * and verifies the source, then maps the old single-artifact request onto the
 * generic provenance-complete batch operation. */
static cj *tool_put_derived_artifact(const cj *arguments, int *is_error) {
    if (!argument_bool(arguments, "confirmed", 0)) {
        *is_error = 1;
        return tool_error(
            "confirmation_required",
            "The host must confirm that this derived output should become reusable memory.");
    }
    const char *store_path = argument_string(arguments, "store_path");
    const char *source_path = argument_string(arguments, "source_path");
    const char *text = argument_string(arguments, "text");
    const char *artifact_kind = argument_string(arguments, "artifact_kind");
    const char *producer_name = argument_string(arguments, "producer_name");
    const char *producer_version =
        argument_string(arguments, "producer_version");
    const char *app_name = argument_string(arguments, "app_name");
    const char *app_version = argument_string(arguments, "app_version");
    if (!store_path || !source_path || !text || !artifact_kind ||
        !producer_name || !producer_version || !app_name || !app_version) {
        *is_error = 1;
        return tool_error(
            "invalid_arguments",
            "store_path, source_path, text, artifact_kind, producer_name, producer_version, app_name, and app_version are required.");
    }

    chutni_store *store = NULL;
    chutni_status status = chutni_open(store_path, 0, &store);
    if (status != CHUTNI_OK) {
        *is_error = 1;
        return status_error("Cannot open store for artifact write", status,
                            NULL);
    }
    source_snapshot source;
    status = source_snapshot_load(store, arguments, &source);
    const char *freshness = NULL;
    if (status == CHUTNI_OK)
        status = chutni_source_refresh(store, source.source_id, &freshness);
    if (status == CHUTNI_OK)
        status = source_snapshot_load(store, arguments, &source);
    if (status != CHUTNI_OK || !freshness ||
        strcmp(freshness, "current") || !source.content_hash[0]) {
        cj *error = tool_error(
            "source_not_current",
            "The source is missing or changed; scan it before storing derived output.");
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    const char *operation = argument_string(arguments, "operation")
                                ? argument_string(arguments, "operation")
                                : "derive_artifact";
    const char *producer_kind = argument_string(arguments, "producer_kind")
                                    ? argument_string(arguments, "producer_kind")
                                    : "parser";
    char *selector_text = json_value_text(cj_get(arguments, "selector"));
    int reused = 0;
    char *prior = matching_current_artifact(
        store, source.source_id, artifact_kind, producer_name, producer_kind,
        NULL, NULL, operation, source.content_hash, selector_text, text,
        &reused);
    free(selector_text);
    if (reused && prior) {
        cj *result =
            reused_artifact_json(store_path, source.source_id, prior);
        free(prior);
        chutni_close(store);
        return result;
    }
    chutni_close(store);

    cj *batch = cj_obj();
    cj_set(batch, "store_path", cj_str(store_path));
    cj_set(batch, "source_path", cj_str(source_path));
    cj_set(batch, "source_content_hash", cj_str(source.content_hash));
    cj_set(batch, "confirmed", cj_bool(1));
    cj_set(batch, "operation", cj_str(operation));
    const char *recipe_hash = argument_string(arguments, "recipe_hash");
    if (recipe_hash) cj_set(batch, "recipe_hash", cj_str(recipe_hash));
    cj *parameters = cj_get(arguments, "parameters");
    if (parameters) cj_set(batch, "parameters", json_clone(parameters));

    cj *producer = cj_obj();
    cj_set(producer, "producer_kind", cj_str(producer_kind));
    cj_set(producer, "name", cj_str(producer_name));
    cj_set(producer, "version", cj_str(producer_version));
    const char *runtime = argument_string(arguments, "runtime");
    if (runtime) cj_set(producer, "runtime", cj_str(runtime));
    cj_set(producer, "app_name", cj_str(app_name));
    cj_set(producer, "app_version", cj_str(app_version));
    cj_set(batch, "producer", producer);

    cj *items = cj_arr();
    cj *item = cj_obj();
    cj_set(item, "artifact_kind", cj_str(artifact_kind));
    cj_set(item, "artifact_origin", cj_str("deterministic_transform"));
    cj_set(item, "text", cj_str(text));
    const char *media_type = argument_string(arguments, "media_type");
    if (media_type) cj_set(item, "media_type", cj_str(media_type));
    cj *selector = cj_get(arguments, "selector");
    if (selector) cj_set(item, "selector", json_clone(selector));
    const char *explicit_supersedes =
        argument_string(arguments, "supersedes_artifact_id");
    if (explicit_supersedes)
        cj_set(item, "supersedes_artifact_id",
               cj_str(explicit_supersedes));
    else if (prior)
        cj_set(item, "supersedes_artifact_id", cj_str(prior));
    cj_push(items, item);
    cj_set(batch, "artifacts", items);
    free(prior);

    cj *result = tool_put_artifacts(batch, is_error);
    cj_free(batch);
    if (!*is_error && result) {
        cj *written = cj_get(result, "artifacts");
        if (written && written->type == CJ_ARR && written->n == 1) {
            const char *artifact_id =
                argument_string(written->items[0], "artifact_id");
            if (artifact_id)
                cj_set(result, "artifact_id", cj_str(artifact_id));
        }
    }
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
    cj *result = jcall_dispatch(store, "source_context", arguments, is_error);
    if (!*is_error) cj_set(result, "store_path", cj_str(store_path));
    chutni_close(store);
    return result;
}

/* T01: jcall_op_put_model_artifact in src/chutni.c performs this exact
 * sequence — resolve by source_path, refresh and require "current", hash,
 * record producer/derivation/artifact, rebuild indexes — so this wrapper is
 * pure argument pre-validation plus store lifecycle. Two error codes this
 * service used to return here ("source_not_current", "out_of_memory") become
 * chutni_call's generic envelope codes after migration; nothing asserts
 * either string, so nothing observable changes. */
static cj *tool_put_model_artifact(const cj *arguments, int *is_error) {
    if (!argument_bool(arguments, "confirmed", 0)) {
        *is_error = 1;
        return tool_error(
            "confirmation_required",
            "The host must confirm that this model output should become reusable memory.");
    }
    const char *store_path = argument_string(arguments, "store_path");
    if (!store_path || !argument_string(arguments, "source_path") ||
        !argument_string(arguments, "text") ||
        !argument_string(arguments, "model_id") ||
        !argument_string(arguments, "model_revision") ||
        !argument_string(arguments, "app_name") ||
        !argument_string(arguments, "app_version")) {
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

    source_snapshot source;
    status = source_snapshot_load(store, arguments, &source);
    const char *freshness = NULL;
    if (status == CHUTNI_OK)
        status = chutni_source_refresh(store, source.source_id, &freshness);
    if (status == CHUTNI_OK)
        status = source_snapshot_load(store, arguments, &source);
    if (status != CHUTNI_OK || !freshness ||
        strcmp(freshness, "current") || !source.content_hash[0]) {
        cj *error = tool_error(
            "source_not_current",
            "The source is missing or changed; scan it before storing model output.");
        chutni_close(store);
        *is_error = 1;
        return error;
    }

    const char *artifact_kind = argument_string(arguments, "artifact_kind")
                                    ? argument_string(arguments, "artifact_kind")
                                    : "summary_short";
    const char *producer_name = argument_string(arguments, "producer_name")
                                    ? argument_string(arguments, "producer_name")
                                    : argument_string(arguments, "model_id");
    const char *operation = argument_string(arguments, "operation")
                                ? argument_string(arguments, "operation")
                                : "generate_artifact";
    char *selector_text = json_value_text(cj_get(arguments, "selector"));
    int reused = 0;
    char *prior = matching_current_artifact(
        store, source.source_id, artifact_kind, producer_name, "model",
        argument_string(arguments, "model_id"),
        argument_string(arguments, "model_revision"), operation,
        source.content_hash, selector_text, argument_string(arguments, "text"),
        &reused);
    free(selector_text);
    if (reused && prior) {
        cj *result =
            reused_artifact_json(store_path, source.source_id, prior);
        free(prior);
        chutni_close(store);
        return result;
    }

    cj *write_arguments = json_clone(arguments);
    if (prior && !argument_string(arguments, "supersedes_artifact_id"))
        cj_set(write_arguments, "supersedes_artifact_id", cj_str(prior));
    free(prior);
    cj *result =
        jcall_dispatch(store, "put_model_artifact", write_arguments, is_error);
    cj_free(write_arguments);
    if (!*is_error) cj_set(result, "store_path", cj_str(store_path));
    chutni_close(store);
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
    else if (name && !strcmp(name, "chutni_list_sources"))
        result = tool_list_sources(arguments, is_error);
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
    else if (name && !strcmp(name, "chutni_put_derived_artifact"))
        result = tool_put_derived_artifact(arguments, is_error);
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
        cj_set(properties, "source_path",
               schema_string("Exact authorized source root whose indexed files may be listed."));
        cj_set(properties, "limit",
               schema_integer("Maximum number of file records returned.", 1, 200));
        cj_set(properties, "offset",
               schema_integer("Zero-based file-record offset for pagination.", 0, -1));
        const char *required[] = {"store_path", "source_path", NULL};
        cj_push(tools, tool_definition(
            "chutni_list_sources", "List indexed Chutni files",
            "Return a bounded file-only inventory for one exact authorized source root without walking the live filesystem.",
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
        cj_set(properties, "source_path",
               schema_string("Absolute path of a source already indexed in the store."));
        cj_set(properties, "text",
               schema_string("Deterministically derived artifact text to retain."));
        cj_set(properties, "artifact_kind",
               schema_string("Core or namespaced artifact kind."));
        cj_set(properties, "media_type",
               schema_string("Artifact media type; defaults to UTF-8 text."));
        cj_set(properties, "producer_kind",
               schema_string("Protocol producer kind; defaults to parser."));
        cj_set(properties, "producer_name",
               schema_string("Human-readable parser or pipeline name."));
        cj_set(properties, "producer_version",
               schema_string("Exact parser or pipeline version/fingerprint."));
        cj_set(properties, "runtime",
               schema_string("Runtime used for deterministic extraction."));
        cj_set(properties, "app_name",
               schema_string("Host application committing the artifact."));
        cj_set(properties, "app_version",
               schema_string("Host application version."));
        cj_set(properties, "operation",
               schema_string("Derivation operation; defaults to derive_artifact."));
        cj_set(properties, "recipe_hash",
               schema_string("Hash or stable identifier for the deterministic recipe."));
        cj *parameters = cj_obj();
        cj_set(parameters, "type", cj_str("object"));
        cj_set(properties, "parameters", parameters);
        cj *selector = cj_obj();
        cj_set(selector, "type", cj_str("object"));
        cj_set(properties, "selector", selector);
        cj_set(properties, "supersedes_artifact_id",
               schema_string("Optional prior artifact superseded by this one."));
        cj_set(properties, "confirmed",
               schema_boolean("Must be true when the host intends to retain this derived output."));
        const char *required[] = {
            "store_path", "source_path", "text", "artifact_kind",
            "producer_name", "producer_version", "app_name", "app_version",
            "confirmed", NULL
        };
        cj_push(tools, tool_definition(
            "chutni_put_derived_artifact", "Store derived artifact",
            "Commit deterministic parser or OCR output with exact source freshness, producer identity, selectors, and provenance.",
            input_schema(properties, required), 0, 0, 0));
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
