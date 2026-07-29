/* Chutni reference implementation — store, catalog, objects, search, discovery.
 *
 * Specification: SPEC.md (version 0.1). Section references in comments point
 * there.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "chutni.h"
#include "cj.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "blake3.h"
#include "sqlite3.h"

#define CHUTNI_LIB_VERSION "0.1.0"
#define ERRBUF 512

struct chutni_store {
    char     path[PATH_MAX];
    char     store_id[CHUTNI_ID_STRLEN];
    sqlite3 *db;
    cj      *manifest;      /* full tree, so unknown fields survive (§9.1) */
    int      read_only;
    int      have_index;    /* indexes/lexical.sqlite attached as "idx" */
    char     err[ERRBUF];
};

static char g_last_error[ERRBUF];

/* ------------------------------------------------------------------- errors */

static chutni_status fail(chutni_store *s, chutni_status code, const char *fmt, ...) {
    char buf[ERRBUF];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (s) snprintf(s->err, sizeof s->err, "%s", buf);
    else   snprintf(g_last_error, sizeof g_last_error, "%s", buf);
    return code;
}

const char *chutni_last_error(const chutni_store *store) {
    if (store) return store->err[0] ? store->err : "";
    return g_last_error[0] ? g_last_error : "";
}

const char *chutni_strerror(chutni_status status) {
    switch (status) {
    case CHUTNI_OK:            return "ok";
    case CHUTNI_ERR_IO:        return "filesystem error";
    case CHUTNI_ERR_FORMAT:    return "malformed store";
    case CHUTNI_ERR_VERSION:   return "unsupported specification version";
    case CHUTNI_ERR_NOTFOUND:  return "not found";
    case CHUTNI_ERR_INVALID:   return "invalid argument";
    case CHUTNI_ERR_DB:        return "catalog error";
    case CHUTNI_ERR_NOMEM:     return "out of memory";
    case CHUTNI_ERR_DENIED:    return "denied by policy";
    case CHUTNI_ERR_EXISTS:    return "already exists";
    case CHUTNI_ERR_READONLY:  return "store opened read-only";
    }
    return "unknown error";
}

const char *chutni_spec_version(void)    { return CHUTNI_SPEC_VERSION; }
const char *chutni_library_version(void) { return CHUTNI_LIB_VERSION; }
void        chutni_free(void *ptr)       { free(ptr); }

const char *chutni_store_id(const chutni_store *s)   { return s ? s->store_id : NULL; }
const char *chutni_store_path(const chutni_store *s) { return s ? s->path : NULL; }

const char *chutni_source_state_name(chutni_source_state st) {
    switch (st) {
    case CHUTNI_SOURCE_PRESENT:     return "present";
    case CHUTNI_SOURCE_MISSING:     return "missing";
    case CHUTNI_SOURCE_DELETED:     return "deleted";
    case CHUTNI_SOURCE_UNREADABLE:  return "unreadable";
    case CHUTNI_SOURCE_EXCLUDED:    return "excluded";
    case CHUTNI_SOURCE_UNSUPPORTED: return "unsupported";
    }
    return "unknown";
}

/* ---------------------------------------------------------------- utilities */

static void iso_now(char out[32]) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    time_t secs = ts.tv_sec;
    gmtime_r(&secs, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int fill_random(void *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (unsigned char *)buf + got, len - got);
        if (n <= 0) { close(fd); return 0; }
        got += (size_t)n;
    }
    close(fd);
    return 1;
}

/* UUIDv7: 48-bit millisecond timestamp, then randomness (§12.1 recommends it
 * so that identifiers sort by creation time). */
static int uuid7(char out[CHUTNI_ID_STRLEN]) {
    unsigned char b[16];
    if (!fill_random(b, sizeof b)) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
    b[0] = (unsigned char)(ms >> 40);
    b[1] = (unsigned char)(ms >> 32);
    b[2] = (unsigned char)(ms >> 24);
    b[3] = (unsigned char)(ms >> 16);
    b[4] = (unsigned char)(ms >> 8);
    b[5] = (unsigned char)(ms);
    b[6] = (unsigned char)(0x70 | (b[6] & 0x0F));   /* version 7 */
    b[8] = (unsigned char)(0x80 | (b[8] & 0x3F));   /* RFC 4122 variant */
    snprintf(out, CHUTNI_ID_STRLEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return 1;
}

static int path_join(char *out, size_t cap, const char *a, const char *b) {
    size_t need = strlen(a) + 1 + strlen(b) + 1;
    if (need > cap) return 0;
    snprintf(out, cap, "%s/%s", a, b);
    return 1;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    if (strlen(path) >= sizeof tmp) return 0;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return 0;
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return 0;
    return 1;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Nanosecond mtime, which every platform spells differently. */
static long long stat_mtime_ns(const struct stat *st) {
#if defined(__APPLE__)
    return (long long)st->st_mtimespec.tv_sec * 1000000000ll + st->st_mtimespec.tv_nsec;
#elif defined(st_mtime)
    return (long long)st->st_mtim.tv_sec * 1000000000ll + st->st_mtim.tv_nsec;
#else
    return (long long)st->st_mtime * 1000000000ll;
#endif
}

static int write_atomic(const char *path, const char *data, size_t len) {
    char tmp[PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) { close(fd); unlink(tmp); return 0; }
        off += (size_t)n;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return 0; }
    if (close(fd) != 0) { unlink(tmp); return 0; }
    if (rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

static char *read_whole(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

static void hex_of(const uint8_t *bytes, size_t n, char *out) {
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = d[bytes[i] >> 4];
        out[i * 2 + 1] = d[bytes[i] & 0x0F];
    }
    out[n * 2] = 0;
}

chutni_status chutni_hash_bytes(const void *data, size_t len,
                                char out[CHUTNI_HASH_STRLEN]) {
    if (!data && len) return CHUTNI_ERR_INVALID;
    blake3_hasher h;
    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data ? data : "", len);
    blake3_hasher_finalize(&h, digest, sizeof digest);
    memcpy(out, "blake3:", 7);
    hex_of(digest, sizeof digest, out + 7);
    return CHUTNI_OK;
}

chutni_status chutni_hash_file(const char *path, char out[CHUTNI_HASH_STRLEN]) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail(NULL, CHUTNI_ERR_IO, "cannot read %s: %s", path, strerror(errno));
    blake3_hasher h;
    blake3_hasher_init(&h);
    static unsigned char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) blake3_hasher_update(&h, buf, n);
    int bad = ferror(f);
    fclose(f);
    if (bad) return fail(NULL, CHUTNI_ERR_IO, "read error on %s", path);
    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, digest, sizeof digest);
    memcpy(out, "blake3:", 7);
    hex_of(digest, sizeof digest, out + 7);
    return CHUTNI_OK;
}

/* Object paths come from validated hashes only, never caller-supplied relative
 * paths (§28.8). */
static int hash_is_valid(const char *hash) {
    if (!hash || strncmp(hash, "blake3:", 7)) return 0;
    const char *hex = hash + 7;
    if (strlen(hex) != 64) return 0;
    for (int i = 0; i < 64; i++) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------ SQL utilities */

static int sql_exec(chutni_store *s, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(s->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fail(s, CHUTNI_ERR_DB, "%s", err ? err : sqlite3_errmsg(s->db));
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

static void bind_text_or_null(sqlite3_stmt *q, int idx, const char *v) {
    if (v) sqlite3_bind_text(q, idx, v, -1, SQLITE_TRANSIENT);
    else   sqlite3_bind_null(q, idx);
}

/* ------------------------------------------------------------------- schema */

/* Exactly the tables of §10, plus indexes an implementation MAY add. */
static const char *SCHEMA_SQL =
"PRAGMA foreign_keys=ON;"
"BEGIN IMMEDIATE;"
"CREATE TABLE IF NOT EXISTS roots ("
" root_id TEXT PRIMARY KEY, locator_json TEXT NOT NULL, label TEXT,"
" added_at TEXT NOT NULL, policy_json TEXT NOT NULL);"
"CREATE TABLE IF NOT EXISTS sources ("
" source_id TEXT PRIMARY KEY, root_id TEXT, parent_source_id TEXT,"
" source_kind TEXT NOT NULL, locator_json TEXT NOT NULL, display_name TEXT,"
" media_type TEXT, size_bytes INTEGER, content_hash TEXT, quick_hash TEXT,"
" mtime_ns INTEGER, birthtime_ns INTEGER, file_identity_json TEXT,"
" state TEXT NOT NULL, first_seen_at TEXT NOT NULL, last_seen_at TEXT NOT NULL,"
" last_scanned_at TEXT, metadata_json TEXT,"
" FOREIGN KEY(root_id) REFERENCES roots(root_id),"
" FOREIGN KEY(parent_source_id) REFERENCES sources(source_id));"
"CREATE TABLE IF NOT EXISTS objects ("
" object_hash TEXT PRIMARY KEY, algorithm TEXT NOT NULL, size_bytes INTEGER NOT NULL,"
" media_type TEXT, compression TEXT NOT NULL, relative_path TEXT NOT NULL,"
" created_at TEXT NOT NULL);"
"CREATE TABLE IF NOT EXISTS producers ("
" producer_id TEXT PRIMARY KEY, producer_kind TEXT NOT NULL, name TEXT NOT NULL,"
" version TEXT, model_id TEXT, model_revision TEXT, weights_hash TEXT,"
" quantization TEXT, runtime TEXT, app_name TEXT, app_version TEXT, details_json TEXT);"
"CREATE TABLE IF NOT EXISTS derivations ("
" derivation_id TEXT PRIMARY KEY, producer_id TEXT NOT NULL, operation TEXT NOT NULL,"
" recipe_hash TEXT, parameters_json TEXT, input_refs_json TEXT NOT NULL,"
" created_at TEXT NOT NULL,"
" FOREIGN KEY(producer_id) REFERENCES producers(producer_id));"
"CREATE TABLE IF NOT EXISTS artifacts ("
" artifact_id TEXT PRIMARY KEY, source_id TEXT NOT NULL, artifact_kind TEXT NOT NULL,"
" artifact_origin TEXT NOT NULL, media_type TEXT, object_hash TEXT, inline_text TEXT,"
" selector_json TEXT, language TEXT, source_content_hash TEXT, derivation_id TEXT,"
" status TEXT NOT NULL, supersedes_artifact_id TEXT, created_at TEXT NOT NULL,"
" updated_at TEXT NOT NULL, metadata_json TEXT,"
" CHECK (object_hash IS NOT NULL OR inline_text IS NOT NULL),"
" FOREIGN KEY(source_id) REFERENCES sources(source_id),"
" FOREIGN KEY(object_hash) REFERENCES objects(object_hash),"
" FOREIGN KEY(derivation_id) REFERENCES derivations(derivation_id),"
" FOREIGN KEY(supersedes_artifact_id) REFERENCES artifacts(artifact_id));"
"CREATE TABLE IF NOT EXISTS representations ("
" representation_id TEXT PRIMARY KEY, artifact_id TEXT NOT NULL,"
" representation_kind TEXT NOT NULL, object_hash TEXT NOT NULL, model_id TEXT,"
" model_revision TEXT, dimensions INTEGER, dtype TEXT, normalization TEXT,"
" tokenizer_hash TEXT, projector_hash TEXT, source_artifact_hash TEXT NOT NULL,"
" created_at TEXT NOT NULL, metadata_json TEXT,"
" FOREIGN KEY(artifact_id) REFERENCES artifacts(artifact_id),"
" FOREIGN KEY(object_hash) REFERENCES objects(object_hash));"
"CREATE TABLE IF NOT EXISTS relations ("
" relation_id TEXT PRIMARY KEY, from_id TEXT NOT NULL, predicate TEXT NOT NULL,"
" to_id TEXT NOT NULL, derivation_id TEXT, created_at TEXT NOT NULL, metadata_json TEXT,"
" FOREIGN KEY(derivation_id) REFERENCES derivations(derivation_id));"
"CREATE INDEX IF NOT EXISTS idx_sources_path ON sources(json_extract(locator_json,'$.display_path'));"
"CREATE INDEX IF NOT EXISTS idx_sources_hash ON sources(content_hash);"
"CREATE INDEX IF NOT EXISTS idx_sources_root ON sources(root_id);"
"CREATE INDEX IF NOT EXISTS idx_artifacts_source ON artifacts(source_id);"
"CREATE INDEX IF NOT EXISTS idx_artifacts_kind ON artifacts(artifact_kind, status);"
"CREATE INDEX IF NOT EXISTS idx_relations_from ON relations(from_id);"
"COMMIT;";

/* The lexical index lives outside catalog.sqlite so that deleting indexes/ and
 * rebuilding is exactly what §8.4 says it is: disposable. */
static const char *INDEX_SQL =
"CREATE VIRTUAL TABLE IF NOT EXISTS idx.artifacts_fts USING fts5("
" artifact_id UNINDEXED, source_id UNINDEXED, display_path, artifact_kind, text,"
" tokenize='unicode61 remove_diacritics 2');";

/* ---------------------------------------------------------------- discovery */

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return (h && *h) ? h : NULL;
}

chutni_status chutni_registry_path(char *buf, size_t buflen) {
    const char *override = getenv("CHUTNI_HOME");
    if (override && *override) {
        if ((size_t)snprintf(buf, buflen, "%s/registry.json", override) >= buflen)
            return CHUTNI_ERR_INVALID;
        return CHUTNI_OK;
    }
    const char *h = home_dir();
    if (!h) return fail(NULL, CHUTNI_ERR_IO, "HOME is not set");
    if ((size_t)snprintf(buf, buflen, "%s/.chutni/registry.json", h) >= buflen)
        return CHUTNI_ERR_INVALID;
    return CHUTNI_OK;
}

static cj *registry_load(char *path_out, size_t path_cap) {
    if (chutni_registry_path(path_out, path_cap) != CHUTNI_OK) return NULL;
    char *text = read_whole(path_out, NULL);
    cj *root = NULL;
    if (text) {
        root = cj_parse(text, NULL);
        free(text);
    }
    if (!root || root->type != CJ_OBJ) {
        cj_free(root);
        root = cj_obj();
        if (root) {
            cj_set(root, "format", cj_str("chutni-registry"));
            cj_set(root, "version", cj_str("1"));
            cj_set(root, "stores", cj_arr());
        }
    }
    if (root && !cj_get(root, "stores")) cj_set(root, "stores", cj_arr());
    return root;
}

static int registry_save(const char *path, cj *root) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; if (!mkdir_p(dir)) return 0; }
    char *text = cj_dump(root, 2);
    if (!text) return 0;
    size_t len = strlen(text);
    char *withnl = malloc(len + 2);
    if (!withnl) { free(text); return 0; }
    memcpy(withnl, text, len);
    withnl[len] = '\n';
    withnl[len + 1] = 0;
    int ok = write_atomic(path, withnl, len + 1);
    free(text);
    free(withnl);
    return ok;
}

static int abspath_of(const char *in, char out[PATH_MAX]) {
    if (realpath(in, out)) return 1;
    if (in[0] == '/') { snprintf(out, PATH_MAX, "%s", in); return 1; }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    if ((size_t)snprintf(out, PATH_MAX, "%s/%s", cwd, in) >= PATH_MAX) return 0;
    return 1;
}

chutni_status chutni_registry_add(const char *store_path) {
    char abs[PATH_MAX];
    if (!store_path || !abspath_of(store_path, abs))
        return fail(NULL, CHUTNI_ERR_INVALID, "bad store path");
    char rpath[PATH_MAX];
    cj *root = registry_load(rpath, sizeof rpath);
    if (!root) return CHUTNI_ERR_NOMEM;
    cj *stores = cj_get(root, "stores");
    for (size_t i = 0; i < stores->n; i++) {
        const char *p = cj_get_str(stores->items[i], "path");
        if (p && !strcmp(p, abs)) { cj_free(root); return CHUTNI_OK; }
    }
    cj *entry = cj_obj();
    cj_set(entry, "path", cj_str(abs));
    char now[32];
    iso_now(now);
    cj_set(entry, "registered_at", cj_str(now));
    cj_push(stores, entry);
    int ok = registry_save(rpath, root);
    cj_free(root);
    return ok ? CHUTNI_OK : fail(NULL, CHUTNI_ERR_IO, "cannot write registry %s", rpath);
}

chutni_status chutni_registry_remove(const char *store_path) {
    char abs[PATH_MAX];
    if (!store_path || !abspath_of(store_path, abs))
        return fail(NULL, CHUTNI_ERR_INVALID, "bad store path");
    char rpath[PATH_MAX];
    cj *root = registry_load(rpath, sizeof rpath);
    if (!root) return CHUTNI_ERR_NOMEM;
    cj *stores = cj_get(root, "stores");
    int removed = 0;
    for (size_t i = 0; i < stores->n; i++) {
        const char *p = cj_get_str(stores->items[i], "path");
        if (p && !strcmp(p, abs)) {
            cj_free(stores->items[i]);
            for (size_t j = i + 1; j < stores->n; j++) stores->items[j - 1] = stores->items[j];
            stores->n--;
            removed = 1;
            break;
        }
    }
    int ok = registry_save(rpath, root);
    cj_free(root);
    if (!ok) return fail(NULL, CHUTNI_ERR_IO, "cannot write registry");
    return removed ? CHUTNI_OK : CHUTNI_ERR_NOTFOUND;
}

/* A directory is a store when it holds a manifest.json naming format "chutni". */
static int probe_store(const char *dir, chutni_store_info *info) {
    char mpath[PATH_MAX];
    if (!path_join(mpath, sizeof mpath, dir, "manifest.json")) return 0;
    char *text = read_whole(mpath, NULL);
    if (!text) return 0;
    cj *m = cj_parse(text, NULL);
    free(text);
    if (!m) return 0;
    const char *format = cj_get_str(m, "format");
    if (!format || strcmp(format, "chutni")) { cj_free(m); return 0; }
    if (info) {
        char abs[PATH_MAX];
        if (!abspath_of(dir, abs)) snprintf(abs, sizeof abs, "%s", dir);
        const char *sid = cj_get_str(m, "store_id");
        const char *sv  = cj_get_str(m, "spec_version");
        const char *lbl = cj_get_str(m, "label");
        info->store_path   = strdup(abs);
        info->store_id     = strdup(sid ? sid : "");
        info->spec_version = strdup(sv ? sv : "");
        info->label        = lbl ? strdup(lbl) : NULL;
        char cpath[PATH_MAX];
        info->readable = path_join(cpath, sizeof cpath, dir, "catalog.sqlite") &&
                         access(cpath, R_OK) == 0;
    }
    cj_free(m);
    return 1;
}

typedef struct { chutni_store_info *v; size_t n, cap; } InfoVec;

static int info_seen(const InfoVec *vec, const char *path) {
    for (size_t i = 0; i < vec->n; i++)
        if (vec->v[i].store_path && !strcmp(vec->v[i].store_path, path)) return 1;
    return 0;
}

static void info_try_add(InfoVec *vec, const char *dir) {
    char abs[PATH_MAX];
    if (!abspath_of(dir, abs)) return;
    if (info_seen(vec, abs)) return;
    chutni_store_info info;
    memset(&info, 0, sizeof info);
    if (!probe_store(abs, &info)) return;
    if (vec->n == vec->cap) {
        size_t cap = vec->cap ? vec->cap * 2 : 8;
        chutni_store_info *t = realloc(vec->v, cap * sizeof *t);
        if (!t) { free(info.store_path); free(info.store_id); free(info.label); free(info.spec_version); return; }
        vec->v = t;
        vec->cap = cap;
    }
    vec->v[vec->n++] = info;
}

/* One level of a directory, looking only for *.chutni entries. Never a
 * filesystem-wide scan (§39). */
static void scan_dir_for_stores(InfoVec *vec, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t len = strlen(e->d_name);
        if (len < 8 || strcmp(e->d_name + len - 7, ".chutni")) continue;
        char full[PATH_MAX];
        if (!path_join(full, sizeof full, dir, e->d_name)) continue;
        if (is_dir(full)) info_try_add(vec, full);
    }
    closedir(d);
}

chutni_status chutni_discover(chutni_store_info **out, size_t *count) {
    if (!out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;
    InfoVec vec = { NULL, 0, 0 };

    const char *env = getenv("CHUTNI_STORE");
    if (env && *env) info_try_add(&vec, env);

    char rpath[PATH_MAX];
    if (chutni_registry_path(rpath, sizeof rpath) == CHUTNI_OK) {
        char *text = read_whole(rpath, NULL);
        if (text) {
            cj *root = cj_parse(text, NULL);
            free(text);
            cj *stores = cj_get(root, "stores");
            if (stores && stores->type == CJ_ARR)
                for (size_t i = 0; i < stores->n; i++) {
                    const char *p = cj_get_str(stores->items[i], "path");
                    if (p) info_try_add(&vec, p);
                }
            cj_free(root);
        }
    }

    const char *h = home_dir();
    if (h) {
        char sub[PATH_MAX];
        scan_dir_for_stores(&vec, h);
        if (path_join(sub, sizeof sub, h, "Documents")) scan_dir_for_stores(&vec, sub);
        if (path_join(sub, sizeof sub, h, ".chutni")) scan_dir_for_stores(&vec, sub);
    }

    *out = vec.v;
    *count = vec.n;
    return CHUTNI_OK;
}

void chutni_store_info_free(chutni_store_info *infos, size_t count) {
    if (!infos) return;
    for (size_t i = 0; i < count; i++) {
        free(infos[i].store_path);
        free(infos[i].store_id);
        free(infos[i].label);
        free(infos[i].spec_version);
    }
    free(infos);
}

/* ---------------------------------------------------------------- lifecycle */

static int attach_index(chutni_store *s) {
    char idx_dir[PATH_MAX], idx_db[PATH_MAX];
    if (!path_join(idx_dir, sizeof idx_dir, s->path, "indexes")) return 0;
    if (!mkdir_p(idx_dir)) return 0;
    if (!path_join(idx_db, sizeof idx_db, idx_dir, "lexical.sqlite")) return 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, "ATTACH DATABASE ?1 AS idx", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(q, 1, idx_db, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return 0;
    if (!s->read_only && !sql_exec(s, INDEX_SQL)) return 0;
    s->have_index = 1;
    return 1;
}

static chutni_status open_catalog(chutni_store *s, int create) {
    char cpath[PATH_MAX];
    if (!path_join(cpath, sizeof cpath, s->path, "catalog.sqlite"))
        return fail(s, CHUTNI_ERR_INVALID, "store path too long");
    int flags = s->read_only ? SQLITE_OPEN_READONLY
                             : (SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0));
    if (sqlite3_open_v2(cpath, &s->db, flags, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "cannot open catalog: %s",
                    s->db ? sqlite3_errmsg(s->db) : "unknown");
    sqlite3_busy_timeout(s->db, 5000);
    if (!s->read_only) {
        if (!sql_exec(s, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"))
            return CHUTNI_ERR_DB;
        if (create && !sql_exec(s, SCHEMA_SQL)) return CHUTNI_ERR_DB;
    } else {
        sql_exec(s, "PRAGMA foreign_keys=ON;");
    }
    attach_index(s);   /* absence of a lexical index is not fatal */
    return CHUTNI_OK;
}

static int manifest_save(chutni_store *s) {
    char now[32];
    iso_now(now);
    cj_set(s->manifest, "updated_at", cj_str(now));
    char *text = cj_dump(s->manifest, 2);
    if (!text) return 0;
    size_t len = strlen(text);
    char *withnl = malloc(len + 2);
    if (!withnl) { free(text); return 0; }
    memcpy(withnl, text, len);
    withnl[len] = '\n';
    withnl[len + 1] = 0;
    char mpath[PATH_MAX];
    int ok = path_join(mpath, sizeof mpath, s->path, "manifest.json") &&
             write_atomic(mpath, withnl, len + 1);
    free(text);
    free(withnl);
    return ok;
}

chutni_status chutni_create(const char *path, const char *label, chutni_store **out) {
    if (!path || !out) return CHUTNI_ERR_INVALID;
    *out = NULL;
    char abs[PATH_MAX];
    if (!abspath_of(path, abs)) return fail(NULL, CHUTNI_ERR_INVALID, "bad path");

    char mpath[PATH_MAX];
    if (path_join(mpath, sizeof mpath, abs, "manifest.json") && access(mpath, F_OK) == 0)
        return fail(NULL, CHUTNI_ERR_EXISTS, "a store already exists at %s", abs);

    /* §8: the required layout, created up front so readers never see a partial
       store shape. */
    static const char *dirs[] = { "", "objects", "objects/blake3", "indexes", "extensions", "tmp" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        char d[PATH_MAX];
        if (dirs[i][0] == 0) snprintf(d, sizeof d, "%s", abs);
        else if (!path_join(d, sizeof d, abs, dirs[i])) return CHUTNI_ERR_INVALID;
        if (!mkdir_p(d)) return fail(NULL, CHUTNI_ERR_IO, "cannot create %s: %s", d, strerror(errno));
    }

    chutni_store *s = calloc(1, sizeof *s);
    if (!s) return CHUTNI_ERR_NOMEM;
    snprintf(s->path, sizeof s->path, "%s", abs);

    if (!uuid7(s->store_id)) { free(s); return fail(NULL, CHUTNI_ERR_IO, "no entropy source"); }

    char now[32];
    iso_now(now);
    s->manifest = cj_obj();
    if (!s->manifest) { free(s); return CHUTNI_ERR_NOMEM; }
    cj_set(s->manifest, "format", cj_str("chutni"));
    cj_set(s->manifest, "spec_version", cj_str(CHUTNI_SPEC_VERSION));
    cj_set(s->manifest, "store_id", cj_str(s->store_id));
    if (label) cj_set(s->manifest, "label", cj_str(label));
    cj_set(s->manifest, "created_at", cj_str(now));
    cj_set(s->manifest, "updated_at", cj_str(now));
    cj_set(s->manifest, "hash_algorithm", cj_str("blake3"));
    cj_set(s->manifest, "catalog", cj_str("catalog.sqlite"));
    cj_set(s->manifest, "object_root", cj_str("objects/blake3"));
    cj *caps = cj_arr();
    cj_push(caps, cj_str("sources"));
    cj_push(caps, cj_str("artifacts"));
    cj_push(caps, cj_str("provenance"));
    cj_push(caps, cj_str("full_text_optional"));
    cj_set(s->manifest, "capabilities", caps);
    cj *privacy = cj_obj();
    cj_set(privacy, "local_first", cj_bool(1));
    cj_set(privacy, "external_disclosure_default", cj_str("deny"));  /* §9.1 */
    cj_set(s->manifest, "privacy", privacy);
    cj *producer = cj_obj();
    cj_set(producer, "name", cj_str("chutni"));
    cj_set(producer, "version", cj_str(CHUTNI_LIB_VERSION));
    cj_set(s->manifest, "created_by", producer);

    chutni_status st = open_catalog(s, 1);
    if (st != CHUTNI_OK) { chutni_close(s); return st; }
    if (!manifest_save(s)) { chutni_close(s); return fail(NULL, CHUTNI_ERR_IO, "cannot write manifest"); }

    *out = s;
    return CHUTNI_OK;
}

chutni_status chutni_open(const char *path, int read_only, chutni_store **out) {
    if (!path || !out) return CHUTNI_ERR_INVALID;
    *out = NULL;
    char abs[PATH_MAX];
    if (!abspath_of(path, abs)) return fail(NULL, CHUTNI_ERR_NOTFOUND, "no such path: %s", path);

    char mpath[PATH_MAX];
    if (!path_join(mpath, sizeof mpath, abs, "manifest.json"))
        return fail(NULL, CHUTNI_ERR_INVALID, "path too long");
    char *text = read_whole(mpath, NULL);
    if (!text) return fail(NULL, CHUTNI_ERR_NOTFOUND, "no Chutni store at %s", abs);

    const char *perr = NULL;
    cj *m = cj_parse(text, &perr);
    free(text);
    if (!m || m->type != CJ_OBJ) {
        cj_free(m);
        return fail(NULL, CHUTNI_ERR_FORMAT, "manifest.json is not valid JSON: %s",
                    perr ? perr : "parse error");
    }

    const char *format = cj_get_str(m, "format");
    if (!format || strcmp(format, "chutni")) {
        cj_free(m);
        return fail(NULL, CHUTNI_ERR_FORMAT, "manifest format is not \"chutni\"");
    }
    /* §35: readers MUST reject unsupported major versions. */
    const char *sv = cj_get_str(m, "spec_version");
    if (!sv) { cj_free(m); return fail(NULL, CHUTNI_ERR_FORMAT, "manifest has no spec_version"); }
    long major = strtol(sv, NULL, 10);
    if (major > 0) {
        cj_free(m);
        return fail(NULL, CHUTNI_ERR_VERSION,
                    "store declares spec_version %s; this build implements %s",
                    sv, CHUTNI_SPEC_VERSION);
    }
    const char *halg = cj_get_str(m, "hash_algorithm");
    if (!halg || strcmp(halg, "blake3")) {
        cj_free(m);
        return fail(NULL, CHUTNI_ERR_FORMAT,
                    "hash_algorithm is \"%s\"; v0.1 requires blake3",
                    halg ? halg : "(absent)");
    }

    chutni_store *s = calloc(1, sizeof *s);
    if (!s) { cj_free(m); return CHUTNI_ERR_NOMEM; }
    snprintf(s->path, sizeof s->path, "%s", abs);
    s->manifest = m;
    s->read_only = read_only ? 1 : 0;
    const char *sid = cj_get_str(m, "store_id");
    snprintf(s->store_id, sizeof s->store_id, "%s", sid ? sid : "");

    chutni_status st = open_catalog(s, 0);
    if (st != CHUTNI_OK) { char e[ERRBUF]; snprintf(e, sizeof e, "%s", s->err); chutni_close(s); return fail(NULL, st, "%s", e); }

    *out = s;
    return CHUTNI_OK;
}

void chutni_close(chutni_store *s) {
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    cj_free(s->manifest);
    free(s);
}

chutni_status chutni_manifest_json(chutni_store *s, char **out) {
    if (!s || !out) return CHUTNI_ERR_INVALID;
    *out = cj_dump(s->manifest, 2);
    return *out ? CHUTNI_OK : CHUTNI_ERR_NOMEM;
}

/* -------------------------------------------------------------------- roots */

void chutni_root_policy_defaults(chutni_root_policy *p) {
    if (!p) return;
    p->recursive = 1;
    p->follow_symlinks = 0;               /* §11: symlinks are not followed by default */
    p->include_hidden = 0;
    p->retain_deleted_artifacts = 1;
    p->max_file_size_bytes = 2147483648ull;
    p->exclude_globs = NULL;
}

static char *policy_to_json(const chutni_root_policy *p) {
    cj *o = cj_obj();
    if (!o) return NULL;
    cj_set(o, "recursive", cj_bool(p->recursive));
    cj_set(o, "follow_symlinks", cj_bool(p->follow_symlinks));
    cj_set(o, "include_hidden", cj_bool(p->include_hidden));
    cj_set(o, "retain_deleted_artifacts", cj_bool(p->retain_deleted_artifacts));
    cj_set(o, "max_file_size_bytes", cj_num((double)p->max_file_size_bytes));
    cj *globs = cj_arr();
    if (p->exclude_globs)
        for (const char *const *g = p->exclude_globs; *g; g++) cj_push(globs, cj_str(*g));
    cj_set(o, "exclude_globs", globs);
    char *text = cj_dump(o, -1);
    cj_free(o);
    return text;
}

static char *locator_json_for(const char *abs_path) {
    cj *o = cj_obj();
    if (!o) return NULL;
    cj_set(o, "scheme", cj_str("file"));
#if defined(__APPLE__)
    cj_set(o, "platform", cj_str("macos"));
#elif defined(_WIN32)
    cj_set(o, "platform", cj_str("windows"));
#else
    cj_set(o, "platform", cj_str("linux"));
#endif
    cj_set(o, "display_path", cj_str(abs_path));
    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof uri, "file://%s", abs_path);
    cj_set(o, "file_uri", cj_str(uri));
    char *text = cj_dump(o, -1);
    cj_free(o);
    return text;
}

chutni_status chutni_root_add(chutni_store *s, const char *dir, const char *label,
                              const chutni_root_policy *policy,
                              char root_id[CHUTNI_ID_STRLEN]) {
    if (!s || !dir || !root_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    char abs[PATH_MAX];
    if (!abspath_of(dir, abs)) return fail(s, CHUTNI_ERR_NOTFOUND, "no such directory: %s", dir);
    if (!is_dir(abs)) return fail(s, CHUTNI_ERR_INVALID, "not a directory: %s", abs);

    /* An existing root for the same directory is reused rather than duplicated. */
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT root_id FROM roots WHERE json_extract(locator_json,'$.display_path')=?1",
            -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, abs, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            snprintf(root_id, CHUTNI_ID_STRLEN, "%s", (const char *)sqlite3_column_text(q, 0));
            sqlite3_finalize(q);
            return CHUTNI_OK;
        }
        sqlite3_finalize(q);
    }

    chutni_root_policy defaults;
    if (!policy) { chutni_root_policy_defaults(&defaults); policy = &defaults; }
    char *pol = policy_to_json(policy);
    char *loc = locator_json_for(abs);
    if (!pol || !loc) { free(pol); free(loc); return CHUTNI_ERR_NOMEM; }
    if (!uuid7(root_id)) { free(pol); free(loc); return fail(s, CHUTNI_ERR_IO, "no entropy"); }
    char now[32];
    iso_now(now);

    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO roots(root_id,locator_json,label,added_at,policy_json) VALUES(?1,?2,?3,?4,?5)",
            -1, &q, NULL) != SQLITE_OK) {
        free(pol); free(loc);
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    }
    sqlite3_bind_text(q, 1, root_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, loc, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 3, label);
    sqlite3_bind_text(q, 4, now, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 5, pol, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    free(pol);
    free(loc);
    if (rc != SQLITE_DONE) return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    manifest_save(s);
    return CHUTNI_OK;
}

static char *dup_col(sqlite3_stmt *q, int col) {
    const unsigned char *v = sqlite3_column_text(q, col);
    return v ? strdup((const char *)v) : NULL;
}

chutni_status chutni_roots_list(chutni_store *s, chutni_root_info **out, size_t *count) {
    if (!s || !out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT root_id, json_extract(locator_json,'$.display_path'), label, policy_json"
            " FROM roots ORDER BY added_at", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    chutni_root_info *vec = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            size_t c = cap ? cap * 2 : 8;
            chutni_root_info *t = realloc(vec, c * sizeof *t);
            if (!t) break;
            vec = t;
            cap = c;
        }
        vec[n].root_id     = dup_col(q, 0);
        vec[n].path        = dup_col(q, 1);
        vec[n].label       = dup_col(q, 2);
        vec[n].policy_json = dup_col(q, 3);
        n++;
    }
    sqlite3_finalize(q);
    *out = vec;
    *count = n;
    return CHUTNI_OK;
}

void chutni_root_info_free(chutni_root_info *roots, size_t count) {
    if (!roots) return;
    for (size_t i = 0; i < count; i++) {
        free(roots[i].root_id);
        free(roots[i].path);
        free(roots[i].label);
        free(roots[i].policy_json);
    }
    free(roots);
}

chutni_status chutni_sources_list(chutni_store *s, const char *root_id,
                                  chutni_source_info **out, size_t *count) {
    if (!s || !out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;
    const char *sql = root_id
        ? "SELECT source_id, json_extract(locator_json,'$.display_path'), media_type,"
          " content_hash, state, COALESCE(size_bytes,0) FROM sources WHERE root_id=?1"
          " ORDER BY json_extract(locator_json,'$.display_path')"
        : "SELECT source_id, json_extract(locator_json,'$.display_path'), media_type,"
          " content_hash, state, COALESCE(size_bytes,0) FROM sources"
          " ORDER BY json_extract(locator_json,'$.display_path')";
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    if (root_id) sqlite3_bind_text(q, 1, root_id, -1, SQLITE_TRANSIENT);
    chutni_source_info *vec = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            size_t c = cap ? cap * 2 : 32;
            chutni_source_info *t = realloc(vec, c * sizeof *t);
            if (!t) break;
            vec = t;
            cap = c;
        }
        vec[n].source_id    = dup_col(q, 0);
        vec[n].display_path = dup_col(q, 1);
        vec[n].media_type   = dup_col(q, 2);
        vec[n].content_hash = dup_col(q, 3);
        vec[n].state        = dup_col(q, 4);
        vec[n].size_bytes   = sqlite3_column_int64(q, 5);
        n++;
    }
    sqlite3_finalize(q);
    *out = vec;
    *count = n;
    return CHUTNI_OK;
}

void chutni_source_info_free(chutni_source_info *sources, size_t count) {
    if (!sources) return;
    for (size_t i = 0; i < count; i++) {
        free(sources[i].source_id);
        free(sources[i].display_path);
        free(sources[i].media_type);
        free(sources[i].content_hash);
        free(sources[i].state);
    }
    free(sources);
}

chutni_status chutni_source_find(chutni_store *s, const char *path,
                                 char source_id[CHUTNI_ID_STRLEN]) {
    if (!s || !path || !source_id) return CHUTNI_ERR_INVALID;
    char abs[PATH_MAX];
    if (!abspath_of(path, abs)) snprintf(abs, sizeof abs, "%s", path);
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT source_id FROM sources WHERE json_extract(locator_json,'$.display_path')=?1",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, abs, -1, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(q) == SQLITE_ROW) {
        snprintf(source_id, CHUTNI_ID_STRLEN, "%s", (const char *)sqlite3_column_text(q, 0));
        found = 1;
    }
    sqlite3_finalize(q);
    return found ? CHUTNI_OK : CHUTNI_ERR_NOTFOUND;
}

chutni_status chutni_list_artifacts(chutni_store *s, const char *source_id,
                                    chutni_artifact_info **out, size_t *count) {
    if (!s || !source_id || !out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT a.artifact_id, a.artifact_kind, a.artifact_origin, a.media_type, a.status,"
            "       a.object_hash, a.inline_text, a.selector_json, a.source_content_hash,"
            "       a.created_at, p.name, p.producer_kind, p.model_id, p.model_revision, d.operation"
            " FROM artifacts a"
            " LEFT JOIN derivations d ON d.derivation_id=a.derivation_id"
            " LEFT JOIN producers p ON p.producer_id=d.producer_id"
            " WHERE a.source_id=?1 ORDER BY a.created_at", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
    chutni_artifact_info *vec = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            size_t c = cap ? cap * 2 : 8;
            chutni_artifact_info *t = realloc(vec, c * sizeof *t);
            if (!t) break;
            vec = t;
            cap = c;
        }
        vec[n].artifact_id         = dup_col(q, 0);
        vec[n].artifact_kind       = dup_col(q, 1);
        vec[n].artifact_origin     = dup_col(q, 2);
        vec[n].media_type          = dup_col(q, 3);
        vec[n].status              = dup_col(q, 4);
        vec[n].object_hash         = dup_col(q, 5);
        vec[n].inline_text         = dup_col(q, 6);
        vec[n].selector_json       = dup_col(q, 7);
        vec[n].source_content_hash = dup_col(q, 8);
        vec[n].created_at          = dup_col(q, 9);
        vec[n].producer_name       = dup_col(q, 10);
        vec[n].producer_kind       = dup_col(q, 11);
        vec[n].model_id            = dup_col(q, 12);
        vec[n].model_revision      = dup_col(q, 13);
        vec[n].operation           = dup_col(q, 14);
        n++;
    }
    sqlite3_finalize(q);
    *out = vec;
    *count = n;
    return CHUTNI_OK;
}

void chutni_artifact_info_free(chutni_artifact_info *a, size_t count) {
    if (!a) return;
    for (size_t i = 0; i < count; i++) {
        free(a[i].artifact_id);
        free(a[i].artifact_kind);
        free(a[i].artifact_origin);
        free(a[i].media_type);
        free(a[i].status);
        free(a[i].object_hash);
        free(a[i].inline_text);
        free(a[i].selector_json);
        free(a[i].source_content_hash);
        free(a[i].created_at);
        free(a[i].producer_name);
        free(a[i].producer_kind);
        free(a[i].model_id);
        free(a[i].model_revision);
        free(a[i].operation);
    }
    free(a);
}

static int64_t scalar(chutni_store *s, const char *sql) {
    sqlite3_stmt *q = NULL;
    int64_t v = 0;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK) return 0;
    if (sqlite3_step(q) == SQLITE_ROW) v = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return v;
}

chutni_status chutni_store_counts(chutni_store *s, chutni_counts *c) {
    if (!s || !c) return CHUTNI_ERR_INVALID;
    memset(c, 0, sizeof *c);
    c->roots       = scalar(s, "SELECT COUNT(*) FROM roots");
    c->sources     = scalar(s, "SELECT COUNT(*) FROM sources");
    c->artifacts   = scalar(s, "SELECT COUNT(*) FROM artifacts");
    c->objects     = scalar(s, "SELECT COUNT(*) FROM objects");
    c->producers   = scalar(s, "SELECT COUNT(*) FROM producers");
    c->derivations = scalar(s, "SELECT COUNT(*) FROM derivations");
    c->artifacts_active     = scalar(s, "SELECT COUNT(*) FROM artifacts WHERE status='active'");
    c->artifacts_stale      = scalar(s, "SELECT COUNT(*) FROM artifacts WHERE status='stale'");
    c->artifacts_superseded = scalar(s, "SELECT COUNT(*) FROM artifacts WHERE status='superseded'");
    c->object_bytes = scalar(s, "SELECT COALESCE(SUM(size_bytes),0) FROM objects");
    return CHUTNI_OK;
}

/* ------------------------------------------------------------------ objects */

static int object_rel_path(const char *hash, char out[PATH_MAX]) {
    const char *hex = hash + 7;
    snprintf(out, PATH_MAX, "objects/blake3/%c%c/%c%c/%s", hex[0], hex[1], hex[2], hex[3], hex);
    return 1;
}

chutni_status chutni_object_put(chutni_store *s, const void *data, size_t len,
                                const char *media_type, char hash_out[CHUTNI_HASH_STRLEN]) {
    if (!s || !hash_out || (!data && len)) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    chutni_status st = chutni_hash_bytes(data, len, hash_out);
    if (st != CHUTNI_OK) return st;

    char rel[PATH_MAX], full[PATH_MAX], dir[PATH_MAX];
    object_rel_path(hash_out, rel);
    if (!path_join(full, sizeof full, s->path, rel)) return CHUTNI_ERR_INVALID;
    snprintf(dir, sizeof dir, "%s", full);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (!mkdir_p(dir)) return fail(s, CHUTNI_ERR_IO, "cannot create %s", dir);

    /* Objects are immutable: an identical payload is already correct on disk
       and rewriting it would be pointless work (§14). */
    if (access(full, F_OK) != 0 && !write_atomic(full, (const char *)data, len))
        return fail(s, CHUTNI_ERR_IO, "cannot write object: %s", strerror(errno));

    char now[32];
    iso_now(now);
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT OR IGNORE INTO objects(object_hash,algorithm,size_bytes,media_type,"
            "compression,relative_path,created_at) VALUES(?1,'blake3',?2,?3,'none',?4,?5)",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, hash_out, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)len);
    bind_text_or_null(q, 3, media_type);
    sqlite3_bind_text(q, 4, rel, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 5, now, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? CHUTNI_OK : fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
}

chutni_status chutni_object_get(chutni_store *s, const char *hash, void **data, size_t *len) {
    if (!s || !hash || !data || !len) return CHUTNI_ERR_INVALID;
    if (!hash_is_valid(hash)) return fail(s, CHUTNI_ERR_INVALID, "malformed object hash");
    char rel[PATH_MAX], full[PATH_MAX];
    object_rel_path(hash, rel);
    if (!path_join(full, sizeof full, s->path, rel)) return CHUTNI_ERR_INVALID;
    size_t n = 0;
    char *buf = read_whole(full, &n);
    if (!buf) return fail(s, CHUTNI_ERR_NOTFOUND, "object %s is not in this store", hash);

    /* Content addressing is only a guarantee if it is checked on the way out. */
    char actual[CHUTNI_HASH_STRLEN];
    chutni_hash_bytes(buf, n, actual);
    if (strcmp(actual, hash)) {
        free(buf);
        return fail(s, CHUTNI_ERR_FORMAT, "object %s is corrupt: content hashes to %s", hash, actual);
    }
    *data = buf;
    *len = n;
    return CHUTNI_OK;
}

/* ---------------------------------------------------------------- producers */

chutni_status chutni_producer_put(chutni_store *s, const chutni_producer *p,
                                  char producer_id[CHUTNI_ID_STRLEN]) {
    if (!s || !p || !producer_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (!p->producer_kind || !p->name)
        return fail(s, CHUTNI_ERR_INVALID, "producer_kind and name are required (§16.1)");

    static const char *kinds[] = { "parser","model","application","human","pipeline","unknown",NULL };
    int known = 0;
    for (const char **k = kinds; *k; k++) if (!strcmp(*k, p->producer_kind)) { known = 1; break; }
    if (!known) return fail(s, CHUTNI_ERR_INVALID, "producer_kind \"%s\" is not one of §16.1", p->producer_kind);

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT producer_id FROM producers WHERE producer_kind IS ?1 AND name IS ?2 AND"
            " version IS ?3 AND model_id IS ?4 AND model_revision IS ?5 AND weights_hash IS ?6 AND"
            " quantization IS ?7 AND runtime IS ?8 AND app_name IS ?9 AND app_version IS ?10",
            -1, &q, NULL) == SQLITE_OK) {
        bind_text_or_null(q, 1, p->producer_kind);
        bind_text_or_null(q, 2, p->name);
        bind_text_or_null(q, 3, p->version);
        bind_text_or_null(q, 4, p->model_id);
        bind_text_or_null(q, 5, p->model_revision);
        bind_text_or_null(q, 6, p->weights_hash);
        bind_text_or_null(q, 7, p->quantization);
        bind_text_or_null(q, 8, p->runtime);
        bind_text_or_null(q, 9, p->app_name);
        bind_text_or_null(q, 10, p->app_version);
        if (sqlite3_step(q) == SQLITE_ROW) {
            snprintf(producer_id, CHUTNI_ID_STRLEN, "%s", (const char *)sqlite3_column_text(q, 0));
            sqlite3_finalize(q);
            return CHUTNI_OK;
        }
        sqlite3_finalize(q);
    }

    if (!uuid7(producer_id)) return fail(s, CHUTNI_ERR_IO, "no entropy");
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO producers(producer_id,producer_kind,name,version,model_id,model_revision,"
            "weights_hash,quantization,runtime,app_name,app_version,details_json)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, producer_id, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 2, p->producer_kind);
    bind_text_or_null(q, 3, p->name);
    bind_text_or_null(q, 4, p->version);
    bind_text_or_null(q, 5, p->model_id);
    bind_text_or_null(q, 6, p->model_revision);
    bind_text_or_null(q, 7, p->weights_hash);
    bind_text_or_null(q, 8, p->quantization);
    bind_text_or_null(q, 9, p->runtime);
    bind_text_or_null(q, 10, p->app_name);
    bind_text_or_null(q, 11, p->app_version);
    bind_text_or_null(q, 12, p->details_json);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? CHUTNI_OK : fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
}

chutni_status chutni_derivation_put(chutni_store *s, const char *producer_id,
                                    const char *operation, const char *recipe_hash,
                                    const char *parameters_json, const char *input_refs_json,
                                    char derivation_id[CHUTNI_ID_STRLEN]) {
    if (!s || !producer_id || !operation || !derivation_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (!uuid7(derivation_id)) return fail(s, CHUTNI_ERR_IO, "no entropy");
    char now[32];
    iso_now(now);
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO derivations(derivation_id,producer_id,operation,recipe_hash,"
            "parameters_json,input_refs_json,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7)",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, derivation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, producer_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 3, operation, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 4, recipe_hash);
    bind_text_or_null(q, 5, parameters_json);
    sqlite3_bind_text(q, 6, input_refs_json ? input_refs_json : "[]", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 7, now, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? CHUTNI_OK : fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
}

/* ------------------------------------------------------------------ sources */

static const char *media_type_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    struct { const char *ext, *type; } map[] = {
        { ".txt", "text/plain" }, { ".md", "text/markdown" },
        { ".pdf", "application/pdf" }, { ".json", "application/json" },
        { ".csv", "text/csv" }, { ".html", "text/html" }, { ".htm", "text/html" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" }, { ".tif", "image/tiff" }, { ".tiff", "image/tiff" },
        { ".webp", "image/webp" }, { ".heic", "image/heic" },
        { ".mp3", "audio/mpeg" }, { ".wav", "audio/wav" }, { ".m4a", "audio/mp4" },
        { ".mp4", "video/mp4" }, { ".mov", "video/quicktime" },
        { ".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
        { ".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
        { ".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
        { ".zip", "application/zip" }, { ".c", "text/x-c" }, { ".h", "text/x-c" },
        { ".py", "text/x-python" }, { ".rs", "text/x-rust" }, { ".js", "text/javascript" },
        { NULL, NULL }
    };
    for (int i = 0; map[i].ext; i++) if (!strcasecmp(dot, map[i].ext)) return map[i].type;
    return "application/octet-stream";
}

chutni_status chutni_source_put(chutni_store *s, const char *root_id, const char *path,
                                int hash_file, char source_id[CHUTNI_ID_STRLEN], int *changed) {
    if (!s || !path || !source_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (changed) *changed = 0;

    char abs[PATH_MAX];
    if (!abspath_of(path, abs)) return fail(s, CHUTNI_ERR_NOTFOUND, "no such file: %s", path);
    struct stat st;
    if (lstat(abs, &st) != 0) return fail(s, CHUTNI_ERR_NOTFOUND, "cannot stat %s", abs);

    char content_hash[CHUTNI_HASH_STRLEN];
    content_hash[0] = 0;
    if (hash_file && S_ISREG(st.st_mode)) {
        chutni_status hs = chutni_hash_file(abs, content_hash);
        if (hs != CHUTNI_OK) return fail(s, hs, "cannot hash %s", abs);
    }

    char prev_id[CHUTNI_ID_STRLEN];
    char prev_hash[CHUTNI_HASH_STRLEN];
    prev_id[0] = prev_hash[0] = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT source_id, COALESCE(content_hash,'') FROM sources"
            " WHERE json_extract(locator_json,'$.display_path')=?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, abs, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            snprintf(prev_id, sizeof prev_id, "%s", (const char *)sqlite3_column_text(q, 0));
            snprintf(prev_hash, sizeof prev_hash, "%s", (const char *)sqlite3_column_text(q, 1));
        }
        sqlite3_finalize(q);
    }

    char now[32];
    iso_now(now);
    char *loc = locator_json_for(abs);
    if (!loc) return CHUTNI_ERR_NOMEM;

    cj *ident = cj_obj();
    cj_set(ident, "posix_dev", cj_num((double)st.st_dev));
    cj_set(ident, "posix_ino", cj_num((double)st.st_ino));
    char *ident_json = cj_dump(ident, -1);
    cj_free(ident);

    const char *base = strrchr(abs, '/');
    base = base ? base + 1 : abs;
    long long mtime_ns = stat_mtime_ns(&st);

    if (prev_id[0]) {
        snprintf(source_id, CHUTNI_ID_STRLEN, "%s", prev_id);
        if (sqlite3_prepare_v2(s->db,
                "UPDATE sources SET root_id=COALESCE(?2,root_id), locator_json=?3, display_name=?4,"
                " media_type=?5, size_bytes=?6, content_hash=?7, mtime_ns=?8, file_identity_json=?9,"
                " state='present', last_seen_at=?10, last_scanned_at=?10 WHERE source_id=?1",
                -1, &q, NULL) != SQLITE_OK) {
            free(loc); free(ident_json);
            return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
        }
    } else {
        if (!uuid7(source_id)) { free(loc); free(ident_json); return fail(s, CHUTNI_ERR_IO, "no entropy"); }
        if (sqlite3_prepare_v2(s->db,
                "INSERT INTO sources(source_id,root_id,source_kind,locator_json,display_name,"
                "media_type,size_bytes,content_hash,mtime_ns,file_identity_json,state,"
                "first_seen_at,last_seen_at,last_scanned_at)"
                " VALUES(?1,?2,'file',?3,?4,?5,?6,?7,?8,?9,'present',?10,?10,?10)",
                -1, &q, NULL) != SQLITE_OK) {
            free(loc); free(ident_json);
            return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
        }
    }
    sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 2, root_id);
    sqlite3_bind_text(q, 3, loc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 4, base, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 5, media_type_for(abs), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 6, (sqlite3_int64)st.st_size);
    bind_text_or_null(q, 7, content_hash[0] ? content_hash : NULL);
    sqlite3_bind_int64(q, 8, mtime_ns);
    sqlite3_bind_text(q, 9, ident_json ? ident_json : "{}", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 10, now, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    free(loc);
    free(ident_json);
    if (rc != SQLITE_DONE) return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));

    /* §13.3: when the bytes change, artifacts derived from the old bytes stop
       being current. They are marked stale rather than deleted, so provenance
       and history survive. */
    if (content_hash[0] && prev_id[0] && prev_hash[0] && strcmp(prev_hash, content_hash)) {
        if (sqlite3_prepare_v2(s->db,
                "UPDATE artifacts SET status='stale', updated_at=?3"
                " WHERE source_id=?1 AND status='active' AND source_content_hash IS NOT NULL"
                " AND source_content_hash<>?2", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 2, content_hash, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 3, now, -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
        /* The lexical index indexes active artifacts only. */
        if (s->have_index &&
            sqlite3_prepare_v2(s->db,
                "DELETE FROM idx.artifacts_fts WHERE artifact_id IN"
                " (SELECT artifact_id FROM artifacts WHERE source_id=?1 AND status<>'active')",
                -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
    }

    /* What a caller means by "changed" is "does this still need extracting?",
       and the honest answer is whether an active artifact already describes
       these exact bytes — not whether the hash differs from the last one
       recorded. Those come apart whenever something else has already updated
       the source's hash, such as a verification pass that marked the old
       artifacts stale: the hashes then agree while no usable artifact exists. */
    if (changed) {
        if (!content_hash[0]) {
            *changed = !prev_id[0];
        } else {
            *changed = 1;
            if (sqlite3_prepare_v2(s->db,
                    "SELECT 1 FROM artifacts WHERE source_id=?1 AND status='active'"
                    " AND source_content_hash=?2 LIMIT 1", -1, &q, NULL) == SQLITE_OK) {
                sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(q, 2, content_hash, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(q) == SQLITE_ROW) *changed = 0;
                sqlite3_finalize(q);
            }
        }
    }
    return CHUTNI_OK;
}

chutni_status chutni_source_set_state(chutni_store *s, const char *source_id,
                                      chutni_source_state state) {
    if (!s || !source_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    char now[32];
    iso_now(now);
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE sources SET state=?2, last_scanned_at=?3 WHERE source_id=?1",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, chutni_source_state_name(state), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 3, now, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    int rows = sqlite3_changes(s->db);
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    return rows ? CHUTNI_OK : CHUTNI_ERR_NOTFOUND;
}

chutni_status chutni_check_freshness(chutni_store *s, const char *id, const char **out_state) {
    if (!s || !id || !out_state) return CHUTNI_ERR_INVALID;
    *out_state = "unknown";
    sqlite3_stmt *q = NULL;

    /* An artifact is current when the bytes it was derived from are still the
       bytes on disk (§13.3).

       Comparing the artifact's hash against the catalog's stored source hash is
       not sufficient: both are catalog state, so if the file changed and
       nothing has rescanned it yet, the two agree with each other and the
       artifact reads as current while describing content that is gone. The
       source's own bytes are the only authority, so this re-hashes the file. */
    if (sqlite3_prepare_v2(s->db,
            "SELECT a.status, a.source_content_hash,"
            "       json_extract(sc.locator_json,'$.display_path')"
            " FROM artifacts a JOIN sources sc ON sc.source_id=a.source_id"
            " WHERE a.artifact_id=?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            const char *status = (const char *)sqlite3_column_text(q, 0);
            const unsigned char *ah = sqlite3_column_text(q, 1);
            const unsigned char *ap = sqlite3_column_text(q, 2);
            char artifact_hash[CHUTNI_HASH_STRLEN], source_path[PATH_MAX];
            snprintf(artifact_hash, sizeof artifact_hash, "%s", ah ? (const char *)ah : "");
            snprintf(source_path, sizeof source_path, "%s", ap ? (const char *)ap : "");
            int demoted = status && strcmp(status, "active");
            sqlite3_finalize(q);

            if (demoted) { *out_state = "stale"; return CHUTNI_OK; }
            if (!artifact_hash[0] || !source_path[0]) { *out_state = "unknown"; return CHUTNI_OK; }
            if (access(source_path, R_OK) != 0) { *out_state = "missing"; return CHUTNI_OK; }
            char actual[CHUTNI_HASH_STRLEN];
            if (chutni_hash_file(source_path, actual) != CHUTNI_OK) {
                *out_state = "unreadable";
                return CHUTNI_OK;
            }
            *out_state = strcmp(actual, artifact_hash) ? "stale" : "current";
            return CHUTNI_OK;
        }
        sqlite3_finalize(q);
    }

    if (sqlite3_prepare_v2(s->db,
            "SELECT content_hash, json_extract(locator_json,'$.display_path'), state"
            " FROM sources WHERE source_id=?1", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return fail(s, CHUTNI_ERR_NOTFOUND, "no source or artifact with id %s", id);
    }
    const unsigned char *stored = sqlite3_column_text(q, 0);
    const unsigned char *path = sqlite3_column_text(q, 1);
    char stored_copy[CHUTNI_HASH_STRLEN], path_copy[PATH_MAX];
    snprintf(stored_copy, sizeof stored_copy, "%s", stored ? (const char *)stored : "");
    snprintf(path_copy, sizeof path_copy, "%s", path ? (const char *)path : "");
    sqlite3_finalize(q);

    if (!path_copy[0]) return CHUTNI_OK;
    if (access(path_copy, R_OK) != 0) { *out_state = "missing"; return CHUTNI_OK; }
    if (!stored_copy[0]) return CHUTNI_OK;
    char actual[CHUTNI_HASH_STRLEN];
    if (chutni_hash_file(path_copy, actual) != CHUTNI_OK) { *out_state = "unreadable"; return CHUTNI_OK; }
    *out_state = strcmp(actual, stored_copy) ? "stale" : "current";
    return CHUTNI_OK;
}

chutni_status chutni_source_refresh(chutni_store *s, const char *source_id,
                                    const char **out_state) {
    if (!s || !source_id || !out_state) return CHUTNI_ERR_INVALID;
    *out_state = "unknown";
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(content_hash,''), json_extract(locator_json,'$.display_path')"
            " FROM sources WHERE source_id=?1", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return fail(s, CHUTNI_ERR_NOTFOUND, "no source with id %s", source_id);
    }
    char stored[CHUTNI_HASH_STRLEN], path[PATH_MAX];
    snprintf(stored, sizeof stored, "%s", (const char *)sqlite3_column_text(q, 0));
    const unsigned char *p = sqlite3_column_text(q, 1);
    snprintf(path, sizeof path, "%s", p ? (const char *)p : "");
    sqlite3_finalize(q);

    char now[32];
    iso_now(now);

    if (!path[0] || access(path, R_OK) != 0) {
        chutni_source_set_state(s, source_id, CHUTNI_SOURCE_MISSING);
        *out_state = "missing";
        return CHUTNI_OK;
    }
    char actual[CHUTNI_HASH_STRLEN];
    if (chutni_hash_file(path, actual) != CHUTNI_OK) {
        chutni_source_set_state(s, source_id, CHUTNI_SOURCE_UNREADABLE);
        *out_state = "unreadable";
        return CHUTNI_OK;
    }
    if (stored[0] && !strcmp(stored, actual)) {
        if (sqlite3_prepare_v2(s->db,
                "UPDATE sources SET last_scanned_at=?2, state='present' WHERE source_id=?1",
                -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 2, now, -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
        *out_state = "current";
        return CHUTNI_OK;
    }

    /* The bytes moved on. Record the new hash and demote every artifact that
       described the old ones. */
    if (!sql_exec(s, "BEGIN IMMEDIATE")) return CHUTNI_ERR_DB;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE sources SET content_hash=?2, last_scanned_at=?3, state='present'"
            " WHERE source_id=?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 2, actual, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 3, now, -1, SQLITE_TRANSIENT);
        sqlite3_step(q);
        sqlite3_finalize(q);
    }
    if (sqlite3_prepare_v2(s->db,
            "UPDATE artifacts SET status='stale', updated_at=?3 WHERE source_id=?1"
            " AND status='active' AND source_content_hash IS NOT NULL"
            " AND source_content_hash<>?2", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 2, actual, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 3, now, -1, SQLITE_TRANSIENT);
        sqlite3_step(q);
        sqlite3_finalize(q);
    }
    /* Keep the lexical index in step: it indexes active artifacts only. */
    if (s->have_index &&
        sqlite3_prepare_v2(s->db,
            "DELETE FROM idx.artifacts_fts WHERE artifact_id IN"
            " (SELECT artifact_id FROM artifacts WHERE source_id=?1 AND status<>'active')",
            -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(q);
        sqlite3_finalize(q);
    }
    if (!sql_exec(s, "COMMIT")) return CHUTNI_ERR_DB;
    *out_state = "stale";
    return CHUTNI_OK;
}

chutni_status chutni_forget_source(chutni_store *s, const char *source_id,
                                   chutni_forget_mode mode) {
    if (!s || !source_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (!sql_exec(s, "BEGIN IMMEDIATE")) return CHUTNI_ERR_DB;

    sqlite3_stmt *q = NULL;
    if (s->have_index &&
        sqlite3_prepare_v2(s->db, "DELETE FROM idx.artifacts_fts WHERE source_id=?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(q);
        sqlite3_finalize(q);
    }
    if (sqlite3_prepare_v2(s->db,
            "DELETE FROM representations WHERE artifact_id IN"
            " (SELECT artifact_id FROM artifacts WHERE source_id=?1)", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(q);
        sqlite3_finalize(q);
    }
    if (sqlite3_prepare_v2(s->db, "DELETE FROM artifacts WHERE source_id=?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(q);
        sqlite3_finalize(q);
    }
    if (mode != CHUTNI_FORGET_ARTIFACTS) {
        if (sqlite3_prepare_v2(s->db, "DELETE FROM sources WHERE source_id=?1", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
    }
    if (!sql_exec(s, "COMMIT")) return CHUTNI_ERR_DB;

    /* §24.3 purge: drop object payloads no artifact or representation still
       references. The spec is explicit that this is not forensic erasure. */
    if (mode == CHUTNI_FORGET_PURGE || mode == CHUTNI_FORGET_SECURE_LOGICAL_DELETE) {
        if (sqlite3_prepare_v2(s->db,
                "SELECT object_hash, relative_path FROM objects WHERE object_hash NOT IN"
                " (SELECT object_hash FROM artifacts WHERE object_hash IS NOT NULL"
                "  UNION SELECT object_hash FROM representations)", -1, &q, NULL) == SQLITE_OK) {
            while (sqlite3_step(q) == SQLITE_ROW) {
                const char *rel = (const char *)sqlite3_column_text(q, 1);
                char full[PATH_MAX];
                if (rel && path_join(full, sizeof full, s->path, rel)) unlink(full);
            }
            sqlite3_finalize(q);
        }
        sql_exec(s, "DELETE FROM objects WHERE object_hash NOT IN"
                    " (SELECT object_hash FROM artifacts WHERE object_hash IS NOT NULL"
                    "  UNION SELECT object_hash FROM representations)");
    }
    return CHUTNI_OK;
}

/* ---------------------------------------------------------------- artifacts */

static void fts_insert(chutni_store *s, const char *artifact_id, const char *source_id,
                       const char *kind, const char *text) {
    if (!s->have_index || !text || !*text) return;
    char display_path[PATH_MAX] = "";
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT json_extract(locator_json,'$.display_path') FROM sources WHERE source_id=?1",
            -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            const unsigned char *p = sqlite3_column_text(q, 0);
            if (p) snprintf(display_path, sizeof display_path, "%s", (const char *)p);
        }
        sqlite3_finalize(q);
    }
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO idx.artifacts_fts(artifact_id,source_id,display_path,artifact_kind,text)"
            " VALUES(?1,?2,?3,?4,?5)", -1, &q, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(q, 1, artifact_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 3, display_path, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 4, kind);
    sqlite3_bind_text(q, 5, text, -1, SQLITE_TRANSIENT);
    sqlite3_step(q);
    sqlite3_finalize(q);
}

chutni_status chutni_artifact_put(chutni_store *s, const chutni_artifact *a,
                                  char artifact_id[CHUTNI_ID_STRLEN]) {
    if (!s || !a || !artifact_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (!a->source_id || !a->artifact_kind || !a->artifact_origin)
        return fail(s, CHUTNI_ERR_INVALID, "source_id, artifact_kind, artifact_origin are required");
    if (!a->object_hash && !a->inline_text)
        return fail(s, CHUTNI_ERR_INVALID, "artifact needs object_hash or inline_text (§10)");

    static const char *origins[] = { "direct","deterministic_transform","model_generated","human",NULL };
    int known = 0;
    for (const char **o = origins; *o; o++) if (!strcmp(*o, a->artifact_origin)) { known = 1; break; }
    if (!known)
        return fail(s, CHUTNI_ERR_INVALID, "artifact_origin \"%s\" is not one of §15.1", a->artifact_origin);

    /* §16.4 is the point of the format: a model-generated artifact without a
       derivation cannot be traced to the model that made it. */
    if (!strcmp(a->artifact_origin, "model_generated") && !a->derivation_id)
        return fail(s, CHUTNI_ERR_INVALID,
                    "model_generated artifacts require a derivation_id (§16.4)");

    if (!uuid7(artifact_id)) return fail(s, CHUTNI_ERR_IO, "no entropy");
    char now[32];
    iso_now(now);

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO artifacts(artifact_id,source_id,artifact_kind,artifact_origin,media_type,"
            "object_hash,inline_text,selector_json,language,source_content_hash,derivation_id,"
            "status,supersedes_artifact_id,created_at,updated_at,metadata_json)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,'active',?12,?13,?13,?14)",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, artifact_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, a->source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 3, a->artifact_kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 4, a->artifact_origin, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 5, a->media_type);
    bind_text_or_null(q, 6, a->object_hash);
    bind_text_or_null(q, 7, a->inline_text);
    bind_text_or_null(q, 8, a->selector_json);
    bind_text_or_null(q, 9, a->language);
    bind_text_or_null(q, 10, a->source_content_hash);
    bind_text_or_null(q, 11, a->derivation_id);
    bind_text_or_null(q, 12, a->supersedes_artifact_id);
    sqlite3_bind_text(q, 13, now, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 14, a->metadata_json);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));

    /* §23: superseding preserves the old record, it does not overwrite it. */
    if (a->supersedes_artifact_id) {
        if (sqlite3_prepare_v2(s->db,
                "UPDATE artifacts SET status='superseded', updated_at=?2 WHERE artifact_id=?1",
                -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, a->supersedes_artifact_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 2, now, -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
        if (s->have_index &&
            sqlite3_prepare_v2(s->db, "DELETE FROM idx.artifacts_fts WHERE artifact_id=?1",
                               -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, a->supersedes_artifact_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
    }

    const char *text = a->inline_text;
    char *loaded = NULL;
    if (!text && a->object_hash) {
        void *data = NULL;
        size_t len = 0;
        if (chutni_object_get(s, a->object_hash, &data, &len) == CHUTNI_OK) {
            loaded = data;
            if (a->media_type && !strncmp(a->media_type, "text/", 5)) text = loaded;
        }
    }
    fts_insert(s, artifact_id, a->source_id, a->artifact_kind, text);
    free(loaded);
    return CHUTNI_OK;
}

/* ------------------------------------------------------------------- search */

/* User input is treated as literal terms rather than FTS5 operator syntax: a
 * stray quote or AND in a question should not become a query error or silently
 * change the meaning of the search. */
static char *fts_query_escape(const char *query) {
    size_t cap = strlen(query) * 3 + 8;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t len = 0;
    const char *p = query;
    int wrote_term = 0;
    while (*p) {
        while (*p && (unsigned char)*p <= ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && (unsigned char)*p > ' ') p++;
        if (wrote_term) out[len++] = ' ';
        out[len++] = '"';
        for (const char *c = start; c < p; c++) {
            if (*c == '"') out[len++] = '"';   /* FTS5 escapes a quote by doubling */
            out[len++] = *c;
        }
        out[len++] = '"';
        wrote_term = 1;
    }
    out[len] = 0;
    if (!wrote_term) { free(out); return NULL; }
    return out;
}

static int str_in_list(const char *v, const char *const *list) {
    if (!list || !*list) return 1;
    for (const char *const *p = list; *p; p++) if (!strcmp(v, *p)) return 1;
    return 0;
}

chutni_status chutni_search(chutni_store *s, const chutni_search_request *req,
                            chutni_search_result **out, size_t *count) {
    if (!s || !req || !out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;
    if (!req->query || !*req->query) return fail(s, CHUTNI_ERR_INVALID, "empty query");
    if (!s->have_index) return fail(s, CHUTNI_ERR_NOTFOUND, "no lexical index; run rebuild-indexes");

    char *fts = fts_query_escape(req->query);
    if (!fts) return fail(s, CHUTNI_ERR_INVALID, "query has no searchable terms");

    int limit = req->limit > 0 ? req->limit : 20;
    /* The FTS5 table is deliberately not aliased: MATCH resolves against a
       hidden column that keeps the table's own name, and the auxiliary
       functions take that same name, so an alias breaks both. */
    const char *sql =
        "SELECT artifacts_fts.artifact_id, artifacts_fts.source_id,"
        "       artifacts_fts.display_path, artifacts_fts.artifact_kind,"
        "       snippet(artifacts_fts, 4, '', '', '…', 12),"
        "       bm25(artifacts_fts),"
        "       a.status, a.source_content_hash, a.selector_json, s.content_hash, s.media_type,"
        "       d.producer_id, s.size_bytes, s.mtime_ns"
        " FROM idx.artifacts_fts"
        " JOIN artifacts a ON a.artifact_id=artifacts_fts.artifact_id"
        " JOIN sources s ON s.source_id=a.source_id"
        " LEFT JOIN derivations d ON d.derivation_id=a.derivation_id"
        " WHERE artifacts_fts MATCH ?1"
        " ORDER BY bm25(artifacts_fts) LIMIT ?2";

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK) {
        free(fts);
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    }
    sqlite3_bind_text(q, 1, fts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(q, 2, limit * 4);   /* overfetch, then apply filters below */

    chutni_search_result *vec = NULL;
    size_t n = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW && (int)n < limit) {
        const char *status = (const char *)sqlite3_column_text(q, 6);
        if (!req->include_stale && status && strcmp(status, "active")) continue;

        const char *kind = (const char *)sqlite3_column_text(q, 3);
        if (kind && !str_in_list(kind, req->artifact_kinds)) continue;
        const char *mt = (const char *)sqlite3_column_text(q, 10);
        if (mt && !str_in_list(mt, req->media_types)) continue;

        if (n == cap) {
            size_t c = cap ? cap * 2 : 16;
            chutni_search_result *t = realloc(vec, c * sizeof *t);
            if (!t) break;
            vec = t;
            cap = c;
        }
        chutni_search_result *r = &vec[n];
        memset(r, 0, sizeof *r);
        const unsigned char *v;
        v = sqlite3_column_text(q, 0); r->artifact_id  = strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 1); r->source_id    = strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 2); r->display_path = strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 3); r->artifact_kind= strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 4); r->snippet      = strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 8); r->selector_json= v ? strdup((const char *)v) : NULL;
        v = sqlite3_column_text(q, 11); r->producer_id = v ? strdup((const char *)v) : NULL;

        /* bm25 is negative with better matches more negative; negate so that a
           larger score is a better result, and say so in score_type. */
        r->score = -sqlite3_column_double(q, 5);
        r->score_type = strdup("bm25_fts5_negated");

        const unsigned char *ah = sqlite3_column_text(q, 7);
        const unsigned char *sh = sqlite3_column_text(q, 9);
        const char *fresh = "unknown";
        if (status && strcmp(status, "active")) fresh = "stale";
        else if (ah && sh) fresh = strcmp((const char *)ah, (const char *)sh) ? "stale" : "current";

        /* Both hashes above are catalog state, so they agree with each other
           even when the file has moved on and nothing has rescanned it — the
           mistake this format exists to prevent. Re-hashing every hit is too
           expensive for search, but a stat is not: if size or mtime no longer
           match what the scan recorded, the catalog's "current" is unproven.

           Such a result is reported "unverified", not "stale". §13.2 forbids a
           quick signal from establishing validity, and that cut both ways here:
           a stat cannot prove the bytes differ either, because touching a file
           changes mtime without changing content. So a stat may only withdraw
           a claim, never make one. Callers who need the answer re-hash with
           chutni_check_freshness or chutni verify. */
        if (!strcmp(fresh, "current") && r->display_path[0]) {
            struct stat st;
            if (stat(r->display_path, &st) != 0) {
                fresh = "missing";
            } else if ((int64_t)st.st_size != sqlite3_column_int64(q, 12) ||
                       (long long)stat_mtime_ns(&st) != (long long)sqlite3_column_int64(q, 13)) {
                fresh = "unverified";
            }
        }
        r->freshness = strdup(fresh);
        n++;
    }
    sqlite3_finalize(q);
    free(fts);
    *out = vec;
    *count = n;
    return CHUTNI_OK;
}

void chutni_search_result_free(chutni_search_result *results, size_t count) {
    if (!results) return;
    for (size_t i = 0; i < count; i++) {
        free(results[i].source_id);
        free(results[i].artifact_id);
        free(results[i].display_path);
        free(results[i].artifact_kind);
        free(results[i].snippet);
        free(results[i].producer_id);
        free(results[i].selector_json);
        free(results[i].freshness);
        free(results[i].score_type);
    }
    free(results);
}

chutni_status chutni_rebuild_indexes(chutni_store *s) {
    if (!s) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (!s->have_index && !attach_index(s))
        return fail(s, CHUTNI_ERR_IO, "cannot open the lexical index");
    if (!sql_exec(s, "DROP TABLE IF EXISTS idx.artifacts_fts;")) return CHUTNI_ERR_DB;
    if (!sql_exec(s, INDEX_SQL)) return CHUTNI_ERR_DB;

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT a.artifact_id, a.source_id, a.artifact_kind, a.inline_text, a.object_hash,"
            "       a.media_type FROM artifacts a WHERE a.status='active'",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *aid  = (const char *)sqlite3_column_text(q, 0);
        const char *sid  = (const char *)sqlite3_column_text(q, 1);
        const char *kind = (const char *)sqlite3_column_text(q, 2);
        const char *text = (const char *)sqlite3_column_text(q, 3);
        const char *hash = (const char *)sqlite3_column_text(q, 4);
        const char *mt   = (const char *)sqlite3_column_text(q, 5);
        char *loaded = NULL;
        if (!text && hash && mt && !strncmp(mt, "text/", 5)) {
            void *data = NULL;
            size_t len = 0;
            if (chutni_object_get(s, hash, &data, &len) == CHUTNI_OK) { loaded = data; text = loaded; }
        }
        fts_insert(s, aid, sid, kind, text);
        free(loaded);
    }
    sqlite3_finalize(q);
    return CHUTNI_OK;
}
