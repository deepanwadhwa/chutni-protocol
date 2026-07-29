/* Chutni reference whole-folder ingestion.
 *
 * Kept in libchutni so the CLI, MCP server, and native hosts all create the
 * same sources, artifacts, and provenance instead of growing private scanners.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "chutni.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define REFERENCE_SCANNER_VERSION "0.1.0"
#define DEFAULT_MAX_FILE_BYTES (64ull * 1024ull * 1024ull)

typedef struct {
    chutni_store *store;
    const char *root_id;
    char derivation_text[CHUTNI_ID_STRLEN];
    char derivation_meta[CHUTNI_ID_STRLEN];
    chutni_scan_result *result;
    uint64_t max_bytes;
} scan_context;

static int excluded_name(const char *name) {
    static const char *skip[] = {
        ".git", ".svn", ".hg", "node_modules", ".cache", "__pycache__",
        ".venv", "venv", "target", ".Trash", NULL
    };
    for (const char **p = skip; *p; p++)
        if (!strcmp(name, *p)) return 1;
    return 0;
}

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

static void scan_file(scan_context *sc, const char *path,
                      const struct stat *st) {
    sc->result->files_seen++;
    if ((uint64_t)st->st_size > sc->max_bytes) {
        sc->result->skipped++;
        return;
    }

    char source_id[CHUTNI_ID_STRLEN];
    int changed = 0;
    if (chutni_source_put(sc->store, sc->root_id, path, 1, source_id,
                          &changed) != CHUTNI_OK) {
        sc->result->errors++;
        return;
    }
    sc->result->sources_indexed++;

    char content_hash[CHUTNI_HASH_STRLEN];
    if (chutni_hash_file(path, content_hash) != CHUTNI_OK) {
        sc->result->errors++;
        return;
    }

    int need_metadata = 1;
    int need_text = looks_texty(path) && st->st_size > 0;
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
        if (!need_metadata && !need_text) return;
    }

    char artifact_id[CHUTNI_ID_STRLEN];
    if (need_metadata) {
        char metadata[256];
        snprintf(metadata, sizeof metadata, "{\"size_bytes\":%lld}",
                 (long long)st->st_size);
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

    if (!need_text) return;

    char *text = NULL;
    FILE *file = fopen(path, "rb");
    if (file) {
        text = malloc((size_t)st->st_size + 1);
        if (text) {
            size_t got = fread(text, 1, (size_t)st->st_size, file);
            text[got] = 0;
            if (memchr(text, 0, got)) {
                free(text);
                text = NULL;
            }
        }
        fclose(file);
    }

    if (text) {
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
}

static void scan_dir(scan_context *sc, const char *dir, int depth) {
    if (depth > 64) {
        sc->result->errors++;
        return;
    }
    DIR *directory = opendir(dir);
    if (!directory) {
        sc->result->errors++;
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (entry->d_name[0] == '.' || excluded_name(entry->d_name))
            continue;
        char full[PATH_MAX];
        if ((size_t)snprintf(full, sizeof full, "%s/%s", dir,
                             entry->d_name) >= sizeof full) {
            sc->result->errors++;
            continue;
        }
        struct stat st;
        if (lstat(full, &st) != 0) {
            sc->result->errors++;
            continue;
        }
        if (S_ISLNK(st.st_mode))
            continue;
        if (S_ISDIR(st.st_mode))
            scan_dir(sc, full, depth + 1);
        else if (S_ISREG(st.st_mode))
            scan_file(sc, full, &st);
    }
    closedir(directory);
}

chutni_status chutni_scan(chutni_store *store,
                          const chutni_scan_options *options,
                          chutni_scan_result *result) {
    if (!store || !result) return CHUTNI_ERR_INVALID;
    memset(result, 0, sizeof *result);

    const char *app_name =
        options && options->app_name ? options->app_name : "chutni";
    const char *app_version =
        options && options->app_version ? options->app_version :
        REFERENCE_SCANNER_VERSION;

    scan_context sc;
    memset(&sc, 0, sizeof sc);
    sc.store = store;
    sc.result = result;
    sc.max_bytes = options && options->max_file_size_bytes
                       ? options->max_file_size_bytes
                       : DEFAULT_MAX_FILE_BYTES;

    chutni_producer producer;
    memset(&producer, 0, sizeof producer);
    producer.producer_kind = "parser";
    producer.name = "chutni-reference-scanner";
    producer.version = REFERENCE_SCANNER_VERSION;
    producer.app_name = app_name;
    producer.app_version = app_version;
    char producer_id[CHUTNI_ID_STRLEN];
    chutni_status status =
        chutni_producer_put(store, &producer, producer_id);
    if (status != CHUTNI_OK) return status;
    status = chutni_derivation_put(
        store, producer_id, "extract_text", NULL,
        "{\"strategy\":\"whole_file_utf8\"}", "[]", sc.derivation_text);
    if (status != CHUTNI_OK) return status;
    status = chutni_derivation_put(
        store, producer_id, "record_file_metadata", NULL,
        "{\"fields\":[\"size_bytes\"]}", "[]", sc.derivation_meta);
    if (status != CHUTNI_OK) return status;

    chutni_root_info *roots = NULL;
    size_t root_count = 0;
    status = chutni_roots_list(store, &roots, &root_count);
    if (status != CHUTNI_OK) return status;
    if (root_count == 0) {
        chutni_root_info_free(roots, root_count);
        return CHUTNI_ERR_INVALID;
    }

    for (size_t i = 0; i < root_count; i++) {
        if (!roots[i].path) continue;
        sc.root_id = roots[i].root_id;
        scan_dir(&sc, roots[i].path, 0);
    }
    chutni_root_info_free(roots, root_count);
    return chutni_rebuild_indexes(store);
}
