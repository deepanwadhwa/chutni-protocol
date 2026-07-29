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

#define CHUTNI_MCP_VERSION "0.1.0"
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
    cj_set(object, "scan", value);
}

static int set_store_counts(cj *object, chutni_store *store) {
    chutni_counts counts;
    if (chutni_store_counts(store, &counts) != CHUTNI_OK) return 0;
    cj *value = cj_obj();
    if (!value) return 0;
    cj_set(value, "roots", cj_num((double)counts.roots));
    cj_set(value, "sources", cj_num((double)counts.sources));
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

    char root_id[CHUTNI_ID_STRLEN] = {0};
    if (resolution.source_path[0]) {
        status = chutni_root_add(store, resolution.source_path,
                                 argument_string(arguments, "label"), NULL,
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
    else if (name && !strcmp(name, "chutni_store_info"))
        result = tool_store_info(arguments, is_error);
    else if (name && !strcmp(name, "chutni_scan"))
        result = tool_scan(arguments, is_error);
    else if (name && !strcmp(name, "chutni_search"))
        result = tool_search(arguments, is_error);
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
        const char *required[] = {"store_path", "confirmed", NULL};
        cj_push(tools, tool_definition(
            "chutni_scan", "Update Chutni store",
            "Rescan only the roots already authorized in a store, reuse unchanged artifacts, retire changed artifacts, and rebuild disposable indexes.",
            input_schema(properties, required), 0, 0, 1));
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
