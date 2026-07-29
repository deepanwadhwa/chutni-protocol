/* chutni — command-line interface to a Chutni store.
 *
 * Every command that prints results accepts --json, because the primary
 * consumers of this tool are agents rather than people.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "chutni.h"
#include "cj.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUTNI_APP_VERSION "0.1.0"

static int  opt_json = 0;
static int  opt_limit = 20;
static const char *opt_store = NULL;
static const char *opt_label = NULL;
static const char *opt_mode = "catalog_only";
static const char *opt_kind = NULL;

static int usage(void) {
    fprintf(stderr,
"chutni %s — portable, source-backed memory for local files (spec %s)\n"
"\n"
"usage: chutni <command> [options]\n"
"\n"
"discovery\n"
"  discover                     list Chutni stores on this computer\n"
"  info                         show a store's manifest, roots, and counts\n"
"  register <path>              add a store to the discovery registry\n"
"  unregister <path>            remove one\n"
"\n"
"building\n"
"  init <path> [--label L]      create a store\n"
"  add-root <dir> [--label L]   authorize a directory for indexing\n"
"  roots                        list authorized roots\n"
"  scan                         index authorized roots\n"
"  rebuild-indexes              rebuild everything under indexes/\n"
"\n"
"using\n"
"  search <query> [--limit N] [--kind K]\n"
"  inspect <source-id|path>     show a source, its artifacts, and provenance\n"
"  verify [<source-id|path>]    re-hash sources and report freshness\n"
"  forget <source-id> [--mode catalog_only|artifacts|secure_logical_delete|purge]\n"
"\n"
"options\n"
"  --store <path>   store to operate on; defaults to $CHUTNI_STORE, or the\n"
"                   only store discovery finds\n"
"  --json           machine-readable output\n"
"\n", CHUTNI_APP_VERSION, CHUTNI_SPEC_VERSION);
    return 2;
}

static void die(const char *what, chutni_status st, chutni_store *store) {
    const char *detail = chutni_last_error(store);
    if (detail && *detail) fprintf(stderr, "chutni: %s: %s\n", what, detail);
    else fprintf(stderr, "chutni: %s: %s\n", what, chutni_strerror(st));
    exit(1);
}

/* Resolving the store is itself the discovery story: a tool should not make a
 * user name a path that the protocol can find on its own. */
static chutni_store *open_store(int read_only) {
    static char buf[PATH_MAX];
    const char *path = opt_store;
    if (!path) path = getenv("CHUTNI_STORE");
    if (!path) {
        chutni_store_info *infos = NULL;
        size_t n = 0;
        chutni_discover(&infos, &n);
        if (n == 1) {
            snprintf(buf, sizeof buf, "%s", infos[0].store_path);
            path = buf;
        } else if (n > 1) {
            fprintf(stderr, "chutni: %zu stores found; choose one with --store:\n", n);
            for (size_t i = 0; i < n; i++) fprintf(stderr, "  %s\n", infos[i].store_path);
            chutni_store_info_free(infos, n);
            exit(1);
        }
        chutni_store_info_free(infos, n);
    }
    if (!path) {
        fprintf(stderr, "chutni: no store found. Create one with:  chutni init ~/Memory.chutni\n");
        exit(1);
    }
    chutni_store *s = NULL;
    chutni_status st = chutni_open(path, read_only, &s);
    if (st != CHUTNI_OK) die("cannot open store", st, NULL);
    return s;
}

static void print_json(cj *v) {
    char *text = cj_dump(v, 2);
    if (text) { printf("%s\n", text); free(text); }
    cj_free(v);
}

static void set_opt_str(cj *o, const char *key, const char *v) {
    cj_set(o, key, v ? cj_str(v) : cj_null());
}

/* ---------------------------------------------------------------- discover */

static int cmd_discover(void) {
    chutni_store_info *infos = NULL;
    size_t n = 0;
    chutni_status st = chutni_discover(&infos, &n);
    if (st != CHUTNI_OK) die("discovery failed", st, NULL);

    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "spec_version", cj_str(CHUTNI_SPEC_VERSION));
        cj_set(root, "count", cj_num((double)n));
        cj *arr = cj_arr();
        for (size_t i = 0; i < n; i++) {
            cj *o = cj_obj();
            set_opt_str(o, "path", infos[i].store_path);
            set_opt_str(o, "store_id", infos[i].store_id);
            set_opt_str(o, "spec_version", infos[i].spec_version);
            if (infos[i].label) cj_set(o, "label", cj_str(infos[i].label));
            cj_set(o, "readable", cj_bool(infos[i].readable));
            cj_push(arr, o);
        }
        cj_set(root, "stores", arr);
        print_json(root);
    } else if (n == 0) {
        printf("No Chutni memory found on this computer.\n");
        printf("Create one with:  chutni init ~/Memory.chutni\n");
    } else {
        for (size_t i = 0; i < n; i++)
            printf("%s\n  store_id %s  spec %s%s%s\n", infos[i].store_path,
                   infos[i].store_id, infos[i].spec_version,
                   infos[i].label ? "  label " : "", infos[i].label ? infos[i].label : "");
    }
    chutni_store_info_free(infos, n);
    return n == 0 && !opt_json ? 0 : 0;
}

/* -------------------------------------------------------------------- init */

static int cmd_init(const char *path) {
    if (!path) { fprintf(stderr, "chutni: init needs a path\n"); return 2; }
    chutni_store *s = NULL;
    chutni_status st = chutni_create(path, opt_label, &s);
    if (st != CHUTNI_OK) die("cannot create store", st, NULL);
    chutni_registry_add(chutni_store_path(s));
    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "path", cj_str(chutni_store_path(s)));
        cj_set(root, "store_id", cj_str(chutni_store_id(s)));
        cj_set(root, "spec_version", cj_str(CHUTNI_SPEC_VERSION));
        print_json(root);
    } else {
        printf("Created %s\n  store_id %s\n", chutni_store_path(s), chutni_store_id(s));
        printf("Registered so other applications can discover it.\n");
    }
    chutni_close(s);
    return 0;
}

/* -------------------------------------------------------------------- info */

static int cmd_info(void) {
    chutni_store *s = open_store(1);
    chutni_counts c;
    chutni_store_counts(s, &c);
    chutni_root_info *roots = NULL;
    size_t nroots = 0;
    chutni_roots_list(s, &roots, &nroots);

    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "path", cj_str(chutni_store_path(s)));
        cj_set(root, "store_id", cj_str(chutni_store_id(s)));
        cj *counts = cj_obj();
        cj_set(counts, "roots", cj_num((double)c.roots));
        cj_set(counts, "sources", cj_num((double)c.sources));
        cj_set(counts, "artifacts", cj_num((double)c.artifacts));
        cj_set(counts, "artifacts_active", cj_num((double)c.artifacts_active));
        cj_set(counts, "artifacts_stale", cj_num((double)c.artifacts_stale));
        cj_set(counts, "artifacts_superseded", cj_num((double)c.artifacts_superseded));
        cj_set(counts, "objects", cj_num((double)c.objects));
        cj_set(counts, "object_bytes", cj_num((double)c.object_bytes));
        cj_set(counts, "producers", cj_num((double)c.producers));
        cj_set(counts, "derivations", cj_num((double)c.derivations));
        cj_set(root, "counts", counts);
        cj *rarr = cj_arr();
        for (size_t i = 0; i < nroots; i++) {
            cj *o = cj_obj();
            set_opt_str(o, "root_id", roots[i].root_id);
            set_opt_str(o, "path", roots[i].path);
            if (roots[i].label) cj_set(o, "label", cj_str(roots[i].label));
            cj_push(rarr, o);
        }
        cj_set(root, "roots", rarr);
        print_json(root);
    } else {
        printf("%s\n  store_id %s  spec %s\n\n", chutni_store_path(s),
               chutni_store_id(s), CHUTNI_SPEC_VERSION);
        printf("  roots        %lld\n", (long long)c.roots);
        for (size_t i = 0; i < nroots; i++)
            printf("               %s\n", roots[i].path ? roots[i].path : "(unknown)");
        printf("  sources      %lld\n", (long long)c.sources);
        printf("  artifacts    %lld  (%lld active, %lld stale, %lld superseded)\n",
               (long long)c.artifacts, (long long)c.artifacts_active,
               (long long)c.artifacts_stale, (long long)c.artifacts_superseded);
        printf("  objects      %lld  (%lld bytes)\n", (long long)c.objects,
               (long long)c.object_bytes);
        printf("  producers    %lld\n", (long long)c.producers);
        printf("  derivations  %lld\n", (long long)c.derivations);
    }
    chutni_root_info_free(roots, nroots);
    chutni_close(s);
    return 0;
}

/* ---------------------------------------------------------------- add-root */

static int cmd_add_root(const char *dir) {
    if (!dir) { fprintf(stderr, "chutni: add-root needs a directory\n"); return 2; }
    chutni_store *s = open_store(0);
    char root_id[CHUTNI_ID_STRLEN];
    chutni_status st = chutni_root_add(s, dir, opt_label, NULL, root_id);
    if (st != CHUTNI_OK) die("cannot add root", st, s);
    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "root_id", cj_str(root_id));
        cj_set(root, "path", cj_str(dir));
        print_json(root);
    } else {
        printf("Authorized %s\n  root_id %s\n", dir, root_id);
    }
    chutni_close(s);
    return 0;
}

static int cmd_roots(void) {
    chutni_store *s = open_store(1);
    chutni_root_info *roots = NULL;
    size_t n = 0;
    chutni_roots_list(s, &roots, &n);
    if (opt_json) {
        cj *arr = cj_arr();
        for (size_t i = 0; i < n; i++) {
            cj *o = cj_obj();
            set_opt_str(o, "root_id", roots[i].root_id);
            set_opt_str(o, "path", roots[i].path);
            if (roots[i].label) cj_set(o, "label", cj_str(roots[i].label));
            cj_push(arr, o);
        }
        cj *root = cj_obj();
        cj_set(root, "roots", arr);
        print_json(root);
    } else {
        for (size_t i = 0; i < n; i++)
            printf("%s  %s\n", roots[i].root_id, roots[i].path ? roots[i].path : "");
    }
    chutni_root_info_free(roots, n);
    chutni_close(s);
    return 0;
}

/* -------------------------------------------------------------------- scan */

typedef struct {
    chutni_store *store;
    const char   *root_id;
    char          derivation_text[CHUTNI_ID_STRLEN];
    char          derivation_meta[CHUTNI_ID_STRLEN];
    unsigned long seen, indexed, unchanged, text_artifacts, meta_artifacts, skipped, errors;
    unsigned long max_bytes;
} Scan;

/* Names that are nearly always machine output rather than user documents. */
static int excluded_name(const char *name) {
    static const char *skip[] = {
        ".git", ".svn", ".hg", "node_modules", ".cache", "__pycache__",
        ".venv", "venv", "target", ".Trash", NULL
    };
    for (const char **p = skip; *p; p++) if (!strcmp(name, *p)) return 1;
    return 0;
}

static int looks_texty(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    static const char *exts[] = {
        ".txt",".md",".markdown",".json",".csv",".tsv",".log",".rst",
        ".c",".h",".cc",".cpp",".hpp",".py",".rs",".js",".ts",".go",".rb",
        ".sh",".yaml",".yml",".toml",".ini",".html",".htm",".xml",".tex", NULL
    };
    for (const char **e = exts; *e; e++) if (!strcasecmp(ext, *e)) return 1;
    return 0;
}

static void scan_file(Scan *sc, const char *path, const struct stat *st) {
    sc->seen++;
    if ((unsigned long)st->st_size > sc->max_bytes) { sc->skipped++; return; }

    char source_id[CHUTNI_ID_STRLEN];
    int changed = 0;
    if (chutni_source_put(sc->store, sc->root_id, path, 1, source_id, &changed) != CHUTNI_OK) {
        sc->errors++;
        return;
    }
    sc->indexed++;
    /* Unchanged bytes mean the existing artifacts are still current; redoing
       extraction would only burn I/O (§4 incremental updates). */
    if (!changed) { sc->unchanged++; return; }

    char content_hash[CHUTNI_HASH_STRLEN];
    if (chutni_hash_file(path, content_hash) != CHUTNI_OK) { sc->errors++; return; }

    chutni_artifact a;
    memset(&a, 0, sizeof a);
    a.source_id = source_id;
    a.source_content_hash = content_hash;

    char artifact_id[CHUTNI_ID_STRLEN];
    char *text = NULL;
    if (looks_texty(path) && st->st_size > 0) {
        FILE *f = fopen(path, "rb");
        if (f) {
            text = malloc((size_t)st->st_size + 1);
            if (text) {
                size_t got = fread(text, 1, (size_t)st->st_size, f);
                text[got] = 0;
                /* A NUL byte means this is not text, whatever the extension said. */
                if (memchr(text, 0, got)) { free(text); text = NULL; }
            }
            fclose(f);
        }
    }

    if (text) {
        a.artifact_kind = "extracted_text";
        a.artifact_origin = "deterministic_transform";
        a.media_type = "text/plain; charset=utf-8";
        a.inline_text = text;
        a.derivation_id = sc->derivation_text;
        if (chutni_artifact_put(sc->store, &a, artifact_id) == CHUTNI_OK) sc->text_artifacts++;
        else sc->errors++;
        free(text);
    } else {
        /* Everything else is recorded honestly as metadata. A model-backed
           producer can add captions, OCR, or summaries later without this
           scanner pretending it understood the file. */
        char meta[256];
        snprintf(meta, sizeof meta, "{\"size_bytes\":%lld}", (long long)st->st_size);
        a.artifact_kind = "file_metadata";
        a.artifact_origin = "direct";
        a.media_type = "application/json";
        a.inline_text = meta;
        a.derivation_id = sc->derivation_meta;
        if (chutni_artifact_put(sc->store, &a, artifact_id) == CHUTNI_OK) sc->meta_artifacts++;
        else sc->errors++;
    }
}

static void scan_dir(Scan *sc, const char *dir, int depth) {
    if (depth > 64) return;
    DIR *d = opendir(dir);
    if (!d) { sc->errors++; return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.') continue;            /* include_hidden: false */
        if (excluded_name(e->d_name)) continue;
        char full[PATH_MAX];
        if ((size_t)snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= sizeof full) continue;
        struct stat st;
        if (lstat(full, &st) != 0) { sc->errors++; continue; }
        if (S_ISLNK(st.st_mode)) continue;            /* follow_symlinks: false */
        if (S_ISDIR(st.st_mode)) scan_dir(sc, full, depth + 1);
        else if (S_ISREG(st.st_mode)) scan_file(sc, full, &st);
    }
    closedir(d);
}

static int cmd_scan(void) {
    chutni_store *s = open_store(0);

    Scan sc;
    memset(&sc, 0, sizeof sc);
    sc.store = s;
    sc.max_bytes = 64ul * 1024 * 1024;

    char producer_id[CHUTNI_ID_STRLEN];
    chutni_producer p;
    memset(&p, 0, sizeof p);
    p.producer_kind = "parser";
    p.name = "chutni-reference-scanner";
    p.version = CHUTNI_APP_VERSION;
    p.app_name = "chutni";
    p.app_version = CHUTNI_APP_VERSION;
    if (chutni_producer_put(s, &p, producer_id) != CHUTNI_OK)
        die("cannot record producer", CHUTNI_ERR_DB, s);
    if (chutni_derivation_put(s, producer_id, "extract_text", NULL,
                              "{\"strategy\":\"whole_file_utf8\"}", "[]",
                              sc.derivation_text) != CHUTNI_OK)
        die("cannot record derivation", CHUTNI_ERR_DB, s);
    if (chutni_derivation_put(s, producer_id, "record_file_metadata", NULL,
                              "{\"fields\":[\"size_bytes\"]}", "[]",
                              sc.derivation_meta) != CHUTNI_OK)
        die("cannot record derivation", CHUTNI_ERR_DB, s);

    /* Roots come from the catalog, never the command line: a producer MUST NOT
       index outside authorized roots (§11). */
    chutni_root_info *roots = NULL;
    size_t nroots = 0;
    chutni_roots_list(s, &roots, &nroots);
    if (nroots == 0) {
        fprintf(stderr, "chutni: no authorized roots. Add one with:  chutni add-root <dir>\n");
        chutni_close(s);
        return 1;
    }
    for (size_t i = 0; i < nroots; i++) {
        if (!roots[i].path) continue;
        sc.root_id = roots[i].root_id;
        scan_dir(&sc, roots[i].path, 0);
    }
    chutni_root_info_free(roots, nroots);

    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "files_seen", cj_num((double)sc.seen));
        cj_set(root, "sources_indexed", cj_num((double)sc.indexed));
        cj_set(root, "unchanged", cj_num((double)sc.unchanged));
        cj_set(root, "text_artifacts", cj_num((double)sc.text_artifacts));
        cj_set(root, "metadata_artifacts", cj_num((double)sc.meta_artifacts));
        cj_set(root, "skipped", cj_num((double)sc.skipped));
        cj_set(root, "errors", cj_num((double)sc.errors));
        print_json(root);
    } else {
        printf("Scanned %lu files\n", sc.seen);
        printf("  sources indexed     %lu  (%lu already current)\n", sc.indexed, sc.unchanged);
        printf("  text artifacts      %lu\n", sc.text_artifacts);
        printf("  metadata artifacts  %lu\n", sc.meta_artifacts);
        if (sc.skipped) printf("  skipped (too large) %lu\n", sc.skipped);
        if (sc.errors)  printf("  errors              %lu\n", sc.errors);
    }
    chutni_close(s);
    return 0;
}

/* ------------------------------------------------------------------ search */

static int cmd_search(const char *query) {
    if (!query) { fprintf(stderr, "chutni: search needs a query\n"); return 2; }
    chutni_store *s = open_store(1);
    const char *kinds[2] = { opt_kind, NULL };
    chutni_search_request req;
    memset(&req, 0, sizeof req);
    req.query = query;
    req.limit = opt_limit;
    req.artifact_kinds = opt_kind ? kinds : NULL;

    chutni_search_result *res = NULL;
    size_t n = 0;
    chutni_status st = chutni_search(s, &req, &res, &n);
    if (st != CHUTNI_OK) die("search failed", st, s);

    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "query", cj_str(query));
        cj_set(root, "count", cj_num((double)n));
        cj *arr = cj_arr();
        for (size_t i = 0; i < n; i++) {
            cj *o = cj_obj();
            set_opt_str(o, "source_id", res[i].source_id);
            set_opt_str(o, "artifact_id", res[i].artifact_id);
            set_opt_str(o, "display_path", res[i].display_path);
            set_opt_str(o, "artifact_kind", res[i].artifact_kind);
            set_opt_str(o, "snippet", res[i].snippet);
            cj_set(o, "score", cj_num(res[i].score));
            set_opt_str(o, "score_type", res[i].score_type);
            set_opt_str(o, "freshness", res[i].freshness);
            if (res[i].producer_id) cj_set(o, "producer_id", cj_str(res[i].producer_id));
            cj_push(arr, o);
        }
        cj_set(root, "results", arr);
        print_json(root);
    } else if (n == 0) {
        printf("No matches.\n");
    } else {
        for (size_t i = 0; i < n; i++) {
            printf("%s\n", res[i].display_path ? res[i].display_path : "(unknown path)");
            printf("  %s  %s  score %.3f\n", res[i].artifact_kind ? res[i].artifact_kind : "",
                   res[i].freshness ? res[i].freshness : "", res[i].score);
            if (res[i].snippet && *res[i].snippet) printf("  %s\n", res[i].snippet);
            printf("\n");
        }
    }
    chutni_search_result_free(res, n);
    chutni_close(s);
    return 0;
}

/* ----------------------------------------------------------------- inspect */

/* Accepts a source id or a path, because a person or an agent will have
 * whichever one is closer to hand. */
static int resolve_source(chutni_store *s, const char *arg, char out[CHUTNI_ID_STRLEN]) {
    chutni_artifact_info *tmp = NULL;
    size_t n = 0;
    if (chutni_list_artifacts(s, arg, &tmp, &n) == CHUTNI_OK && n > 0) {
        chutni_artifact_info_free(tmp, n);
        snprintf(out, CHUTNI_ID_STRLEN, "%s", arg);
        return 1;
    }
    chutni_artifact_info_free(tmp, n);
    if (chutni_source_find(s, arg, out) == CHUTNI_OK) return 1;
    /* An id with no artifacts is still a valid source. */
    chutni_source_info *sources = NULL;
    size_t sn = 0;
    chutni_sources_list(s, NULL, &sources, &sn);
    int found = 0;
    for (size_t i = 0; i < sn; i++) {
        if (sources[i].source_id && !strcmp(sources[i].source_id, arg)) {
            snprintf(out, CHUTNI_ID_STRLEN, "%s", arg);
            found = 1;
            break;
        }
    }
    chutni_source_info_free(sources, sn);
    return found;
}

static int cmd_inspect(const char *arg) {
    if (!arg) { fprintf(stderr, "chutni: inspect needs a source id or path\n"); return 2; }
    chutni_store *s = open_store(1);
    char source_id[CHUTNI_ID_STRLEN];
    if (!resolve_source(s, arg, source_id)) {
        fprintf(stderr, "chutni: no source matching %s\n", arg);
        chutni_close(s);
        return 1;
    }
    chutni_artifact_info *arts = NULL;
    size_t n = 0;
    chutni_list_artifacts(s, source_id, &arts, &n);
    const char *fresh = "unknown";
    chutni_check_freshness(s, source_id, &fresh);

    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "source_id", cj_str(source_id));
        cj_set(root, "freshness", cj_str(fresh));
        cj *arr = cj_arr();
        for (size_t i = 0; i < n; i++) {
            cj *o = cj_obj();
            set_opt_str(o, "artifact_id", arts[i].artifact_id);
            set_opt_str(o, "artifact_kind", arts[i].artifact_kind);
            set_opt_str(o, "artifact_origin", arts[i].artifact_origin);
            set_opt_str(o, "status", arts[i].status);
            set_opt_str(o, "media_type", arts[i].media_type);
            set_opt_str(o, "created_at", arts[i].created_at);
            cj *prov = cj_obj();
            set_opt_str(prov, "producer_name", arts[i].producer_name);
            set_opt_str(prov, "producer_kind", arts[i].producer_kind);
            set_opt_str(prov, "model_id", arts[i].model_id);
            set_opt_str(prov, "model_revision", arts[i].model_revision);
            set_opt_str(prov, "operation", arts[i].operation);
            cj_set(o, "provenance", prov);
            cj_push(arr, o);
        }
        cj_set(root, "artifacts", arr);
        print_json(root);
    } else {
        printf("source %s  (%s)\n\n", source_id, fresh);
        for (size_t i = 0; i < n; i++) {
            printf("  %s  [%s, %s]\n", arts[i].artifact_kind ? arts[i].artifact_kind : "",
                   arts[i].artifact_origin ? arts[i].artifact_origin : "",
                   arts[i].status ? arts[i].status : "");
            printf("    artifact_id %s\n", arts[i].artifact_id ? arts[i].artifact_id : "");
            if (arts[i].producer_name)
                printf("    produced by %s%s%s via %s\n", arts[i].producer_name,
                       arts[i].model_id ? " / " : "", arts[i].model_id ? arts[i].model_id : "",
                       arts[i].operation ? arts[i].operation : "?");
            printf("\n");
        }
        if (n == 0) printf("  (no artifacts)\n");
    }
    chutni_artifact_info_free(arts, n);
    chutni_close(s);
    return 0;
}

/* ------------------------------------------------------------------ verify */

/* Verification writes: observing that a source drifted and not recording it
 * would leave artifacts marked active that no longer describe the file (§13.3).
 * A store that cannot be opened for writing is still verifiable, read-only. */
static int cmd_verify(const char *arg) {
    chutni_store *s = open_store(0);
    int exit_code = 0;

    if (arg) {
        char source_id[CHUTNI_ID_STRLEN];
        if (!resolve_source(s, arg, source_id)) {
            fprintf(stderr, "chutni: no source matching %s\n", arg);
            chutni_close(s);
            return 1;
        }
        const char *fresh = "unknown";
        chutni_source_refresh(s, source_id, &fresh);
        if (opt_json) {
            cj *root = cj_obj();
            cj_set(root, "source_id", cj_str(source_id));
            cj_set(root, "freshness", cj_str(fresh));
            print_json(root);
        } else {
            printf("%s  %s\n", source_id, fresh);
        }
        if (strcmp(fresh, "current")) exit_code = 1;
        chutni_close(s);
        return exit_code;
    }

    chutni_source_info *sources = NULL;
    size_t n = 0;
    chutni_sources_list(s, NULL, &sources, &n);
    size_t current = 0, stale = 0, missing = 0, other = 0;
    cj *arr = opt_json ? cj_arr() : NULL;
    for (size_t i = 0; i < n; i++) {
        const char *fresh = "unknown";
        chutni_source_refresh(s, sources[i].source_id, &fresh);
        if (!strcmp(fresh, "current")) current++;
        else if (!strcmp(fresh, "stale")) stale++;
        else if (!strcmp(fresh, "missing")) missing++;
        else other++;
        if (arr && strcmp(fresh, "current")) {
            cj *o = cj_obj();
            set_opt_str(o, "source_id", sources[i].source_id);
            set_opt_str(o, "display_path", sources[i].display_path);
            cj_set(o, "freshness", cj_str(fresh));
            cj_push(arr, o);
        } else if (!arr && strcmp(fresh, "current")) {
            printf("%-8s %s\n", fresh, sources[i].display_path ? sources[i].display_path : "");
        }
    }
    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "checked", cj_num((double)n));
        cj_set(root, "current", cj_num((double)current));
        cj_set(root, "stale", cj_num((double)stale));
        cj_set(root, "missing", cj_num((double)missing));
        cj_set(root, "other", cj_num((double)other));
        cj_set(root, "not_current", arr);
        print_json(root);
    } else {
        printf("\n%zu sources: %zu current, %zu stale, %zu missing, %zu other\n",
               n, current, stale, missing, other);
    }
    if (stale || missing) exit_code = 1;
    chutni_source_info_free(sources, n);
    chutni_close(s);
    return exit_code;
}

/* ------------------------------------------------------------------ forget */

static int cmd_forget(const char *arg) {
    if (!arg) { fprintf(stderr, "chutni: forget needs a source id or path\n"); return 2; }
    chutni_store *s = open_store(0);
    char source_id[CHUTNI_ID_STRLEN];
    if (!resolve_source(s, arg, source_id)) {
        fprintf(stderr, "chutni: no source matching %s\n", arg);
        chutni_close(s);
        return 1;
    }
    chutni_forget_mode mode = CHUTNI_FORGET_CATALOG_ONLY;
    if (!strcmp(opt_mode, "artifacts")) mode = CHUTNI_FORGET_ARTIFACTS;
    else if (!strcmp(opt_mode, "secure_logical_delete")) mode = CHUTNI_FORGET_SECURE_LOGICAL_DELETE;
    else if (!strcmp(opt_mode, "purge")) mode = CHUTNI_FORGET_PURGE;
    else if (strcmp(opt_mode, "catalog_only")) {
        fprintf(stderr, "chutni: unknown --mode %s\n", opt_mode);
        chutni_close(s);
        return 2;
    }
    chutni_status st = chutni_forget_source(s, source_id, mode);
    if (st != CHUTNI_OK) die("forget failed", st, s);
    if (opt_json) {
        cj *root = cj_obj();
        cj_set(root, "source_id", cj_str(source_id));
        cj_set(root, "mode", cj_str(opt_mode));
        cj_set(root, "forgotten", cj_bool(1));
        print_json(root);
    } else {
        printf("Forgot %s (%s)\n", source_id, opt_mode);
        if (mode == CHUTNI_FORGET_PURGE || mode == CHUTNI_FORGET_SECURE_LOGICAL_DELETE)
            printf("Object payloads were unlinked. This is not forensic erasure: "
                   "copies may remain in backups, snapshots, or unallocated blocks.\n");
    }
    chutni_close(s);
    return 0;
}

/* --------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    const char *pos = NULL;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) opt_json = 1;
        else if (!strcmp(argv[i], "--store") && i + 1 < argc) opt_store = argv[++i];
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) opt_label = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) opt_mode = argv[++i];
        else if (!strcmp(argv[i], "--kind") && i + 1 < argc) opt_kind = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) opt_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return usage();
        else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "chutni: unknown option %s\n", argv[i]);
            return 2;
        }
        else if (!pos) pos = argv[i];
    }

    if (!strcmp(cmd, "discover"))   return cmd_discover();
    if (!strcmp(cmd, "init"))       return cmd_init(pos);
    if (!strcmp(cmd, "info"))       return cmd_info();
    if (!strcmp(cmd, "add-root"))   return cmd_add_root(pos);
    if (!strcmp(cmd, "roots"))      return cmd_roots();
    if (!strcmp(cmd, "scan"))       return cmd_scan();
    if (!strcmp(cmd, "search"))     return cmd_search(pos);
    if (!strcmp(cmd, "inspect"))    return cmd_inspect(pos);
    if (!strcmp(cmd, "verify"))     return cmd_verify(pos);
    if (!strcmp(cmd, "forget"))     return cmd_forget(pos);
    if (!strcmp(cmd, "register")) {
        if (!pos) { fprintf(stderr, "chutni: register needs a path\n"); return 2; }
        chutni_status st = chutni_registry_add(pos);
        if (st != CHUTNI_OK) die("cannot register", st, NULL);
        printf("Registered %s\n", pos);
        return 0;
    }
    if (!strcmp(cmd, "unregister")) {
        if (!pos) { fprintf(stderr, "chutni: unregister needs a path\n"); return 2; }
        chutni_status st = chutni_registry_remove(pos);
        if (st != CHUTNI_OK) die("cannot unregister", st, NULL);
        printf("Unregistered %s\n", pos);
        return 0;
    }
    if (!strcmp(cmd, "rebuild-indexes")) {
        chutni_store *s = open_store(0);
        chutni_status st = chutni_rebuild_indexes(s);
        if (st != CHUTNI_OK) die("rebuild failed", st, s);
        printf("Rebuilt indexes.\n");
        chutni_close(s);
        return 0;
    }
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
        printf("chutni %s (spec %s, library %s)\n", CHUTNI_APP_VERSION,
               CHUTNI_SPEC_VERSION, chutni_library_version());
        return 0;
    }
    return usage();
}
