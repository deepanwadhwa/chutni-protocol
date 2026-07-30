/* Chutni reference implementation — store, catalog, objects, search, discovery.
 *
 * Specification: SPEC.md (version 0.2). Section references in comments point
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
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "blake3.h"
#include "sqlite3.h"

/* The implementation's release version comes from the VERSION file via the
 * Makefile. The fallback keeps a hand-rolled compile working without it, and says
 * plainly that it does not know the version rather than naming one it cannot
 * vouch for — this string ends up in producer records (§16.1). */
#ifndef CHUTNI_VERSION
#define CHUTNI_VERSION "0.0.0-unversioned"
#endif

#define CHUTNI_LIB_VERSION CHUTNI_VERSION
#define ERRBUF 512

struct chutni_store {
    char     path[PATH_MAX];
    char     store_id[CHUTNI_ID_STRLEN];
    sqlite3 *db;
    cj      *manifest;      /* full tree, so unknown fields survive (§9.1) */
    int      read_only;
    int      have_index;    /* indexes/lexical.sqlite attached as "idx" */
    int      writer_lock_fd;
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
    case CHUTNI_ERR_BUSY:      return "store is busy";
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

static chutni_status acquire_writer_lock(chutni_store *s) {
    char path[PATH_MAX];
    if (!path_join(path, sizeof path, s->path, "tmp/write.lock"))
        return fail(s, CHUTNI_ERR_INVALID, "store path is too long");
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return fail(s, CHUTNI_ERR_IO, "cannot open writer lock: %s",
                    strerror(errno));
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int lock_errno = errno;
        close(fd);
        if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN)
            return fail(s, CHUTNI_ERR_BUSY,
                        "another application is writing this Chutni store");
        return fail(s, CHUTNI_ERR_IO, "cannot acquire writer lock: %s",
                    strerror(lock_errno));
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    s->writer_lock_fd = fd;
    return CHUTNI_OK;
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

static int json_text_has_type(const char *text, cj_type type) {
    if (!text) return 1;
    const char *error = NULL;
    cj *value = cj_parse(text, &error);
    (void)error;
    int valid = value && value->type == type;
    cj_free(value);
    return valid;
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
"CREATE INDEX IF NOT EXISTS idx_sources_parent ON sources(parent_source_id);"
"CREATE INDEX IF NOT EXISTS idx_sources_kind ON sources(source_kind);"
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
    s->writer_lock_fd = -1;
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

    chutni_status st = acquire_writer_lock(s);
    if (st != CHUTNI_OK) { chutni_close(s); return st; }
    st = open_catalog(s, 1);
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
    s->writer_lock_fd = -1;
    snprintf(s->path, sizeof s->path, "%s", abs);
    s->manifest = m;
    s->read_only = read_only ? 1 : 0;
    const char *sid = cj_get_str(m, "store_id");
    snprintf(s->store_id, sizeof s->store_id, "%s", sid ? sid : "");

    chutni_status st = CHUTNI_OK;
    if (!s->read_only) st = acquire_writer_lock(s);
    if (st == CHUTNI_OK) st = open_catalog(s, 0);
    if (st != CHUTNI_OK) { char e[ERRBUF]; snprintf(e, sizeof e, "%s", s->err); chutni_close(s); return fail(NULL, st, "%s", e); }

    *out = s;
    return CHUTNI_OK;
}

void chutni_close(chutni_store *s) {
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    if (s->writer_lock_fd >= 0) {
        flock(s->writer_lock_fd, LOCK_UN);
        close(s->writer_lock_fd);
    }
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
    /* §11.1: unbounded is the v0.1 behavior, and defaults must not silently
       change what an existing caller's scan covers. A host that wants a
       bounded scan says so. */
    p->max_depth = CHUTNI_DEPTH_UNBOUNDED;
    p->memory_goal = NULL;
    p->definition_mode = NULL;
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
    /* Explicit null rather than an omitted key: a reader can then tell a policy
       that considered depth and chose unbounded from a v0.1 policy that never
       had the concept. Both mean unbounded; only one of them said so. */
    cj_set(o, "max_depth", p->max_depth < 0 ? cj_null() : cj_num((double)p->max_depth));
    if (p->memory_goal) cj_set(o, "memory_goal", cj_str(p->memory_goal));
    if (p->definition_mode) cj_set(o, "definition_mode", cj_str(p->definition_mode));
    char *text = cj_dump(o, -1);
    cj_free(o);
    return text;
}

/* §35.1: a store that records hierarchy must say so, because a consumer cannot
   distinguish "this tree has no subdirectories" from "this writer never looked"
   without being told which one it is reading. Idempotent. */
static void advertise_hierarchy(chutni_store *s) {
    if (!s || s->read_only || !s->manifest) return;
    static const char *wanted[] = { "hierarchical_sources", "bounded_coverage",
                                    "directory_definitions", NULL };
    cj *caps = cj_get(s->manifest, "capabilities");
    if (!caps || caps->type != CJ_ARR) {
        caps = cj_arr();
        if (!caps || !cj_set(s->manifest, "capabilities", caps)) return;
    }
    int changed = 0;
    for (const char **w = wanted; *w; w++) {
        int present = 0;
        for (size_t i = 0; i < caps->n; i++)
            if (caps->items[i]->type == CJ_STR && caps->items[i]->str &&
                !strcmp(caps->items[i]->str, *w)) { present = 1; break; }
        if (!present && cj_push(caps, cj_str(*w))) changed = 1;
    }
    /* Writers record the oldest version that describes what they used (§35).
       Hierarchy is a 0.2 feature, so using it moves the store to 0.2. */
    const char *sv = cj_get_str(s->manifest, "spec_version");
    if (!sv || strcmp(sv, CHUTNI_SPEC_VERSION)) {
        cj_set(s->manifest, "spec_version", cj_str(CHUTNI_SPEC_VERSION));
        changed = 1;
    }
    if (changed) manifest_save(s);
}

static void advertise_capability(chutni_store *s, const char *capability) {
    if (!s || s->read_only || !s->manifest || !capability) return;
    cj *caps = cj_get(s->manifest, "capabilities");
    if (!caps || caps->type != CJ_ARR) {
        caps = cj_arr();
        if (!caps || !cj_set(s->manifest, "capabilities", caps)) return;
    }
    for (size_t i = 0; i < caps->n; i++)
        if (caps->items[i]->type == CJ_STR && caps->items[i]->str &&
            !strcmp(caps->items[i]->str, capability))
            return;
    if (cj_push(caps, cj_str(capability))) manifest_save(s);
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

/* Every source listing returns the same ten columns in the same order, so that
   hierarchy fields cannot be present on one path and quietly absent on
   another. `bind` is bound to ?1 when non-NULL. */
#define SOURCE_COLUMNS \
    "SELECT source_id, json_extract(locator_json,'$.display_path'), media_type," \
    " content_hash, state, COALESCE(size_bytes,0), source_kind, parent_source_id," \
    " json_extract(metadata_json,'$.observation')," \
    " COALESCE(json_extract(metadata_json,'$.depth'),-1) FROM sources "

static chutni_status sources_query(chutni_store *s, const char *sql,
                                   const char *bind, chutni_source_info **out,
                                   size_t *count) {
    *out = NULL;
    *count = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    if (bind) sqlite3_bind_text(q, 1, bind, -1, SQLITE_TRANSIENT);
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
        vec[n].source_id        = dup_col(q, 0);
        vec[n].display_path     = dup_col(q, 1);
        vec[n].media_type       = dup_col(q, 2);
        vec[n].content_hash     = dup_col(q, 3);
        vec[n].state            = dup_col(q, 4);
        vec[n].size_bytes       = sqlite3_column_int64(q, 5);
        vec[n].source_kind      = dup_col(q, 6);
        vec[n].parent_source_id = dup_col(q, 7);
        vec[n].observation      = dup_col(q, 8);
        vec[n].depth            = sqlite3_column_int(q, 9);
        n++;
    }
    sqlite3_finalize(q);
    *out = vec;
    *count = n;
    return CHUTNI_OK;
}

chutni_status chutni_sources_list(chutni_store *s, const char *root_id,
                                  chutni_source_info **out, size_t *count) {
    if (!s || !out || !count) return CHUTNI_ERR_INVALID;
    return sources_query(s,
        root_id ? SOURCE_COLUMNS "WHERE root_id=?1"
                  " ORDER BY json_extract(locator_json,'$.display_path')"
                : SOURCE_COLUMNS
                  " ORDER BY json_extract(locator_json,'$.display_path')",
        root_id, out, count);
}

void chutni_source_info_free(chutni_source_info *sources, size_t count) {
    if (!sources) return;
    for (size_t i = 0; i < count; i++) {
        free(sources[i].source_id);
        free(sources[i].display_path);
        free(sources[i].media_type);
        free(sources[i].content_hash);
        free(sources[i].state);
        free(sources[i].source_kind);
        free(sources[i].parent_source_id);
        free(sources[i].observation);
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
            "       a.object_hash, a.inline_text, a.selector_json, a.language,"
            "       a.source_content_hash, a.created_at, a.metadata_json,"
            "       a.supersedes_artifact_id,"
            "       p.producer_id, p.name, p.producer_kind, p.version,"
            "       p.model_id, p.model_revision, p.weights_hash, p.quantization,"
            "       p.runtime, p.app_name, p.app_version, p.details_json,"
            "       d.derivation_id, d.operation, d.recipe_hash, d.parameters_json,"
            "       d.input_refs_json, d.created_at"
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
        vec[n].language            = dup_col(q, 8);
        vec[n].source_content_hash = dup_col(q, 9);
        vec[n].created_at          = dup_col(q, 10);
        vec[n].metadata_json       = dup_col(q, 11);
        vec[n].supersedes_artifact_id = dup_col(q, 12);
        vec[n].producer_id         = dup_col(q, 13);
        vec[n].producer_name       = dup_col(q, 14);
        vec[n].producer_kind       = dup_col(q, 15);
        vec[n].producer_version    = dup_col(q, 16);
        vec[n].model_id            = dup_col(q, 17);
        vec[n].model_revision      = dup_col(q, 18);
        vec[n].weights_hash        = dup_col(q, 19);
        vec[n].quantization        = dup_col(q, 20);
        vec[n].runtime             = dup_col(q, 21);
        vec[n].app_name            = dup_col(q, 22);
        vec[n].app_version         = dup_col(q, 23);
        vec[n].producer_details_json = dup_col(q, 24);
        vec[n].derivation_id       = dup_col(q, 25);
        vec[n].operation           = dup_col(q, 26);
        vec[n].recipe_hash         = dup_col(q, 27);
        vec[n].parameters_json     = dup_col(q, 28);
        vec[n].input_refs_json     = dup_col(q, 29);
        vec[n].derivation_created_at = dup_col(q, 30);
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
        free(a[i].language);
        free(a[i].source_content_hash);
        free(a[i].created_at);
        free(a[i].metadata_json);
        free(a[i].supersedes_artifact_id);
        free(a[i].producer_id);
        free(a[i].producer_name);
        free(a[i].producer_kind);
        free(a[i].producer_version);
        free(a[i].model_id);
        free(a[i].model_revision);
        free(a[i].weights_hash);
        free(a[i].quantization);
        free(a[i].runtime);
        free(a[i].app_name);
        free(a[i].app_version);
        free(a[i].producer_details_json);
        free(a[i].derivation_id);
        free(a[i].operation);
        free(a[i].recipe_hash);
        free(a[i].parameters_json);
        free(a[i].input_refs_json);
        free(a[i].derivation_created_at);
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
    /* Sources predating hierarchical scanning have no source_kind of their
       own; they were files, so count them as files rather than dropping them. */
    c->sources_files = scalar(s,
        "SELECT COUNT(*) FROM sources WHERE COALESCE(source_kind,'file')='file'");
    c->sources_directories = scalar(s,
        "SELECT COUNT(*) FROM sources WHERE source_kind='directory'");
    c->sources_opaque_directories = scalar(s,
        "SELECT COUNT(*) FROM sources WHERE source_kind='directory'"
        " AND json_extract(metadata_json,'$.observation')='opaque'");
    c->relations = scalar(s, "SELECT COUNT(*) FROM relations");

    /* Application-facing readability counters. Keep this vocabulary aligned
       with the kinds search can return as reusable document content. Scan
       structure artifacts are deliberately excluded. */
    const char *content_kinds =
        "'extracted_text','page_text','ocr_text','transcript','text_chunk',"
        "'summary_short','summary_long','image_caption','document_title',"
        "'keywords','entities','table_schema','sheet_summary','archive_listing'";
    char sql[1024];
    snprintf(sql, sizeof sql,
             "SELECT COUNT(*) FROM artifacts a JOIN sources s USING(source_id) "
             "WHERE a.status='active' "
             "AND COALESCE(s.source_kind,'file')='file' "
             "AND a.artifact_kind IN (%s)", content_kinds);
    c->content_artifacts = scalar(s, sql);
    c->metadata_artifacts = scalar(
        s, "SELECT COUNT(*) FROM artifacts a JOIN sources s USING(source_id) "
           "WHERE a.status='active' "
           "AND COALESCE(s.source_kind,'file')='file' "
           "AND a.artifact_kind='file_metadata'");
    snprintf(sql, sizeof sql,
             "SELECT COUNT(DISTINCT a.source_id) "
             "FROM artifacts a JOIN sources s USING(source_id) "
             "WHERE a.status='active' "
             "AND COALESCE(s.source_kind,'file')='file' "
             "AND a.artifact_kind IN (%s)", content_kinds);
    c->content_readable_sources = scalar(s, sql);
    snprintf(sql, sizeof sql,
             "SELECT COUNT(*) FROM sources s "
             "WHERE COALESCE(s.source_kind,'file')='file' "
             "AND EXISTS (SELECT 1 FROM artifacts a "
             "            WHERE a.source_id=s.source_id AND a.status='active') "
             "AND NOT EXISTS (SELECT 1 FROM artifacts a "
             "                WHERE a.source_id=s.source_id "
             "                AND a.status='active' "
             "                AND a.artifact_kind IN (%s))",
             content_kinds);
    c->metadata_only_sources = scalar(s, sql);
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
    if (p->details_json && !json_text_has_type(p->details_json, CJ_OBJ))
        return fail(s, CHUTNI_ERR_INVALID,
                    "producer details_json must be a JSON object");
    if (!strcmp(p->producer_kind, "model") &&
        (!p->model_id || !p->app_name || !p->app_version))
        return fail(s, CHUTNI_ERR_INVALID,
                    "model producers require model_id, app_name, and app_version (§16.2)");

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
    if (parameters_json && !json_text_has_type(parameters_json, CJ_OBJ))
        return fail(s, CHUTNI_ERR_INVALID,
                    "derivation parameters_json must be a JSON object");
    if (input_refs_json && !json_text_has_type(input_refs_json, CJ_ARR))
        return fail(s, CHUTNI_ERR_INVALID,
                    "derivation input_refs_json must be a JSON array");
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

/* -------------------------------------------------- directory observation
 *
 * §13.5. A directory has no bytes to hash, so what identifies it is one
 * observed immediate listing: the names it held and what kind each one was.
 *
 * The listing deliberately excludes file contents. A file changing inside a
 * directory does not change the directory's membership, and folding content
 * hashes in here would make every edit anywhere invalidate every enclosing
 * listing up to the root. Content changes reach a directory's definition
 * through derivation inputs instead (§13.3), which is the path that can say
 * which input went stale.
 *
 * Media types are also excluded. They are derived from the name by a table
 * this implementation may extend, and a listing hash that moved when that
 * table grew would stale every directory in every store on upgrade.
 */

/* Names the reference implementation never descends into. This is a fixed
 * list, not policy_json's exclude_globs, which remains unenforced — see
 * docs/TASKS.md. A caller that needs different exclusions cannot express them
 * yet, and pretending otherwise would put a policy field in a listing hash
 * that nothing actually consults. */
static int excluded_entry_name(const char *name) {
    static const char *skip[] = {
        ".git", ".svn", ".hg", "node_modules", ".cache", "__pycache__",
        ".venv", "venv", "target", ".Trash", NULL
    };
    for (const char **p = skip; *p; p++)
        if (!strcmp(name, *p)) return 1;
    return 0;
}

static int dir_entry_cmp(const void *a, const void *b) {
    const chutni_dir_entry *x = a, *y = b;
    return strcmp(x->name, y->name);
}

/* Escapes so that a name containing a tab or a newline cannot forge an entry
 * boundary. POSIX permits both in filenames, and a listing hash that two
 * different directories could collide on is not an identity. */
static void append_escaped(char **buf, size_t *len, size_t *cap, const char *s) {
    for (const char *p = s; *p; p++) {
        char out[2];
        size_t n = 1;
        switch (*p) {
            case '\\': out[0] = '\\'; out[1] = '\\'; n = 2; break;
            case '\t': out[0] = '\\'; out[1] = 't';  n = 2; break;
            case '\n': out[0] = '\\'; out[1] = 'n';  n = 2; break;
            case '\r': out[0] = '\\'; out[1] = 'r';  n = 2; break;
            default:   out[0] = *p; break;
        }
        if (*len + n + 1 > *cap) {
            size_t c = *cap ? *cap * 2 : 256;
            while (c < *len + n + 1) c *= 2;
            char *t = realloc(*buf, c);
            if (!t) return;
            *buf = t;
            *cap = c;
        }
        memcpy(*buf + *len, out, n);
        *len += n;
        (*buf)[*len] = 0;
    }
}

static void append_raw(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t c = *cap ? *cap * 2 : 256;
        while (c < *len + n + 1) c *= 2;
        char *t = realloc(*buf, c);
        if (!t) return;
        *buf = t;
        *cap = c;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

void chutni_dir_entry_free(chutni_dir_entry *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].source_kind);
        free(entries[i].media_type);
    }
    free(entries);
}

chutni_status chutni_read_directory(const char *dir,
                                    const chutni_root_policy *policy,
                                    chutni_dir_entry **out, size_t *count,
                                    uint64_t *excluded, uint64_t *unsupported,
                                    char hash_out[CHUTNI_HASH_STRLEN]) {
    if (!dir) return CHUTNI_ERR_INVALID;
    if (out) *out = NULL;
    if (count) *count = 0;

    chutni_root_policy defaults;
    if (!policy) { chutni_root_policy_defaults(&defaults); policy = &defaults; }

    DIR *d = opendir(dir);
    if (!d) return CHUTNI_ERR_IO;

    chutni_dir_entry *vec = NULL;
    size_t n = 0, cap = 0;
    uint64_t skipped = 0, odd = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && !policy->include_hidden) { skipped++; continue; }
        if (excluded_entry_name(e->d_name)) { skipped++; continue; }

        char full[PATH_MAX];
        if ((size_t)snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= sizeof full) {
            odd++;
            continue;
        }
        struct stat st;
        if (lstat(full, &st) != 0) { odd++; continue; }
        if (S_ISLNK(st.st_mode)) {
            if (!policy->follow_symlinks) { skipped++; continue; }
            if (stat(full, &st) != 0) { odd++; continue; }
        }
        int is_directory = S_ISDIR(st.st_mode) != 0;
        if (!is_directory && !S_ISREG(st.st_mode)) { odd++; continue; }

        if (n == cap) {
            size_t c = cap ? cap * 2 : 32;
            chutni_dir_entry *t = realloc(vec, c * sizeof *t);
            if (!t) break;
            vec = t;
            cap = c;
        }
        memset(&vec[n], 0, sizeof vec[n]);
        vec[n].name = strdup(e->d_name);
        vec[n].source_kind = strdup(is_directory ? "directory" : "file");
        vec[n].media_type = is_directory ? NULL : strdup(media_type_for(e->d_name));
        vec[n].size_bytes = is_directory ? -1 : (int64_t)st.st_size;
        if (!vec[n].name || !vec[n].source_kind) {
            chutni_dir_entry_free(vec, n + 1);
            closedir(d);
            return CHUTNI_ERR_NOMEM;
        }
        n++;
    }
    closedir(d);

    /* Readdir order is not defined and differs between filesystems, so the
       canonical form sorts. Two computers observing the same directory must
       compute the same hash or freshness means nothing across a copied store. */
    if (vec) qsort(vec, n, sizeof *vec, dir_entry_cmp);

    if (hash_out) {
        char *buf = NULL;
        size_t len = 0, bcap = 0;
        append_raw(&buf, &len, &bcap, "chutni-listing-1\n");
        for (size_t i = 0; i < n; i++) {
            append_escaped(&buf, &len, &bcap, vec[i].name);
            append_raw(&buf, &len, &bcap, "\t");
            append_raw(&buf, &len, &bcap, vec[i].source_kind);
            append_raw(&buf, &len, &bcap, "\n");
        }
        if (!buf) {
            chutni_dir_entry_free(vec, n);
            return CHUTNI_ERR_NOMEM;
        }
        chutni_status hs = chutni_hash_bytes(buf, len, hash_out);
        free(buf);
        if (hs != CHUTNI_OK) {
            chutni_dir_entry_free(vec, n);
            return hs;
        }
    }

    if (excluded) *excluded = skipped;
    if (unsupported) *unsupported = odd;
    if (out) { *out = vec; if (count) *count = n; }
    else chutni_dir_entry_free(vec, n);
    return CHUTNI_OK;
}

chutni_status chutni_directory_listing_hash(const char *dir,
                                            const chutni_root_policy *policy,
                                            char hash_out[CHUTNI_HASH_STRLEN]) {
    if (!dir || !hash_out) return CHUTNI_ERR_INVALID;
    return chutni_read_directory(dir, policy, NULL, NULL, NULL, NULL, hash_out);
}

/* The policy a source's root was authorized under. Freshness must re-enumerate
   under the same rules the scan used, or it re-derives a different listing and
   calls every directory stale. */
static void policy_for_source(chutni_store *s, const char *source_id,
                              chutni_root_policy *out) {
    chutni_root_policy_defaults(out);
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT r.policy_json FROM sources sc JOIN roots r ON r.root_id=sc.root_id"
            " WHERE sc.source_id=?1", -1, &q, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
    char json[4096] = "";
    if (sqlite3_step(q) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(q, 0);
        if (p) snprintf(json, sizeof json, "%s", (const char *)p);
    }
    sqlite3_finalize(q);
    if (!json[0]) return;
    cj *o = cj_parse(json, NULL);
    if (!o) return;
    cj *v = cj_get(o, "follow_symlinks");
    if (v && v->type == CJ_BOOL) out->follow_symlinks = v->bval;
    v = cj_get(o, "include_hidden");
    if (v && v->type == CJ_BOOL) out->include_hidden = v->bval;
    cj_free(o);
}

/* §13.3, applied to whatever a source's current version happens to be: a
   file's bytes, or a directory's observed listing. One rule, one place — the
   two defects this format exists to prevent were both freshness logic that had
   drifted apart between copies. */
static void stale_artifacts_not_matching(chutni_store *s, const char *source_id,
                                         const char *hash, const char *now) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE artifacts SET status='stale', updated_at=?3"
            " WHERE source_id=?1 AND status='active' AND source_content_hash IS NOT NULL"
            " AND source_content_hash<>?2", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 2, hash, -1, SQLITE_TRANSIENT);
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

/* §13.3 second clause: an artifact whose derivation required an input that is
   no longer active is describing that input's old content, whatever its own
   source looks like. Repeats to a fixpoint so a chain of derived artifacts
   withdraws all the way down, not one level per verification pass. */
static void cascade_stale_dependents(chutni_store *s, const char *now) {
    for (int round = 0; round < 16; round++) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(s->db,
                "UPDATE artifacts SET status='stale', updated_at=?1"
                " WHERE status='active' AND derivation_id IN ("
                "   SELECT d.derivation_id FROM derivations d, json_each(d.input_refs_json) je"
                "   JOIN artifacts inp"
                "     ON inp.artifact_id = json_extract(je.value,'$.artifact_id')"
                "   WHERE json_valid(d.input_refs_json)"
                "     AND json_type(je.value)='object'"
                "     AND COALESCE(json_extract(je.value,'$.required'), 1) NOT IN (0,'false')"
                "     AND inp.status<>'active')",
                -1, &q, NULL) != SQLITE_OK)
            return;
        sqlite3_bind_text(q, 1, now, -1, SQLITE_TRANSIENT);
        sqlite3_step(q);
        int changed = sqlite3_changes(s->db);
        sqlite3_finalize(q);
        if (!changed) return;
        /* The lexical index carries active artifacts only, so anything this
           round demoted has to leave it. */
        if (s->have_index)
            sql_exec(s, "DELETE FROM idx.artifacts_fts WHERE artifact_id IN"
                        " (SELECT artifact_id FROM artifacts WHERE status<>'active')");
    }
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
        stale_artifacts_not_matching(s, source_id, content_hash, now);
        cascade_stale_dependents(s, now);
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

/* Merge hierarchy facts into a source without disturbing metadata_json keys
   this implementation did not write (§9.1 applies to source metadata too). */
static void source_set_hierarchy(chutni_store *s, const char *source_id,
                                 const char *parent_source_id, int depth,
                                 const char *observation) {
    sqlite3_stmt *q = NULL;
    char existing[4096] = "";
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(metadata_json,'') FROM sources WHERE source_id=?1",
            -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW)
            snprintf(existing, sizeof existing, "%s",
                     (const char *)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    cj *meta = existing[0] ? cj_parse(existing, NULL) : NULL;
    if (!meta || meta->type != CJ_OBJ) { cj_free(meta); meta = cj_obj(); }
    if (!meta) return;
    if (depth >= 0) cj_set(meta, "depth", cj_num((double)depth));
    if (observation) cj_set(meta, "observation", cj_str(observation));
    char *text = cj_dump(meta, -1);
    cj_free(meta);
    if (!text) return;

    if (sqlite3_prepare_v2(s->db,
            "UPDATE sources SET parent_source_id=COALESCE(?2,parent_source_id),"
            " metadata_json=?3 WHERE source_id=?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        bind_text_or_null(q, 2, parent_source_id);
        sqlite3_bind_text(q, 3, text, -1, SQLITE_TRANSIENT);
        sqlite3_step(q);
        sqlite3_finalize(q);
    }
    free(text);
}

chutni_status chutni_directory_put(chutni_store *s, const char *root_id,
                                   const char *path,
                                   const char *parent_source_id,
                                   const char *listing_hash, int depth,
                                   char source_id[CHUTNI_ID_STRLEN]) {
    if (!s || !path || !source_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (listing_hash && !hash_is_valid(listing_hash))
        return fail(s, CHUTNI_ERR_INVALID, "listing hash is malformed");

    char abs[PATH_MAX];
    if (!abspath_of(path, abs))
        return fail(s, CHUTNI_ERR_NOTFOUND, "no such directory: %s", path);
    struct stat st;
    if (lstat(abs, &st) != 0)
        return fail(s, CHUTNI_ERR_NOTFOUND, "cannot stat %s", abs);
    if (!is_dir(abs))
        return fail(s, CHUTNI_ERR_INVALID, "not a directory: %s", abs);

    char prev_id[CHUTNI_ID_STRLEN], prev_hash[CHUTNI_HASH_STRLEN];
    prev_id[0] = prev_hash[0] = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT source_id, COALESCE(content_hash,'') FROM sources"
            " WHERE json_extract(locator_json,'$.display_path')=?1",
            -1, &q, NULL) == SQLITE_OK) {
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
    base = base && base[1] ? base + 1 : abs;
    long long mtime_ns = stat_mtime_ns(&st);

    if (prev_id[0]) {
        snprintf(source_id, CHUTNI_ID_STRLEN, "%s", prev_id);
        /* An opaque re-observation must not erase a listing recorded earlier:
           not looking inside is not evidence that the inside changed. */
        if (sqlite3_prepare_v2(s->db,
                "UPDATE sources SET root_id=COALESCE(?2,root_id), source_kind='directory',"
                " locator_json=?3, display_name=?4, media_type=NULL, size_bytes=NULL,"
                " content_hash=COALESCE(?5,content_hash), mtime_ns=?6, file_identity_json=?7,"
                " state='present', last_seen_at=?8, last_scanned_at=?8 WHERE source_id=?1",
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
                " VALUES(?1,?2,'directory',?3,?4,NULL,NULL,?5,?6,?7,'present',?8,?8,?8)",
                -1, &q, NULL) != SQLITE_OK) {
            free(loc); free(ident_json);
            return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
        }
    }
    sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 2, root_id);
    sqlite3_bind_text(q, 3, loc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 4, base, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 5, listing_hash);
    sqlite3_bind_int64(q, 6, mtime_ns);
    sqlite3_bind_text(q, 7, ident_json ? ident_json : "{}", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 8, now, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    free(loc);
    free(ident_json);
    if (rc != SQLITE_DONE) return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));

    if (listing_hash && prev_hash[0] && strcmp(prev_hash, listing_hash))
        stale_artifacts_not_matching(s, source_id, listing_hash, now);

    source_set_hierarchy(s, source_id, parent_source_id, depth,
                         listing_hash ? "enumerated" : "opaque");
    advertise_hierarchy(s);
    return CHUTNI_OK;
}

chutni_status chutni_list_children(chutni_store *s, const char *source_id,
                                   chutni_source_info **out, size_t *count) {
    if (!s || !source_id || !out || !count) return CHUTNI_ERR_INVALID;
    return sources_query(s, SOURCE_COLUMNS
                            "WHERE parent_source_id=?1 ORDER BY display_name",
                         source_id, out, count);
}

chutni_status chutni_source_set_parent(chutni_store *s, const char *source_id,
                                       const char *parent_source_id, int depth) {
    if (!s || !source_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (parent_source_id && !strcmp(parent_source_id, source_id))
        return fail(s, CHUTNI_ERR_INVALID, "a source cannot contain itself");
    source_set_hierarchy(s, source_id, parent_source_id, depth, NULL);
    advertise_hierarchy(s);
    return CHUTNI_OK;
}

chutni_status chutni_new_id(char id[CHUTNI_ID_STRLEN]) {
    if (!id) return CHUTNI_ERR_INVALID;
    return uuid7(id) ? CHUTNI_OK : CHUTNI_ERR_IO;
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

/* Re-derive a source's current observation from the filesystem.
 *
 * Never from the catalog. Both defects found while building v0.1 came from
 * comparing one piece of catalog state against another and finding, correctly,
 * that they agreed — while the disk said something else entirely. A file's
 * observation is the hash of its bytes; an enumerated directory's is the hash
 * of its listing, re-read here rather than remembered.
 *
 * Returns 0 and sets *state when there is nothing to compare against. */
static int observe_source(chutni_store *s, const char *source_id,
                          char out[CHUTNI_HASH_STRLEN], const char **state) {
    sqlite3_stmt *q = NULL;
    char kind[32] = "", path[PATH_MAX] = "", observation[32] = "";
    char stored_state[32] = "";
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(source_kind,'file'),"
            " json_extract(locator_json,'$.display_path'),"
            " COALESCE(json_extract(metadata_json,'$.observation'),''), state"
            " FROM sources WHERE source_id=?1", -1, &q, NULL) != SQLITE_OK) {
        *state = "unknown";
        return 0;
    }
    sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
    int found = sqlite3_step(q) == SQLITE_ROW;
    if (found) {
        snprintf(kind, sizeof kind, "%s", (const char *)sqlite3_column_text(q, 0));
        const unsigned char *p = sqlite3_column_text(q, 1);
        snprintf(path, sizeof path, "%s", p ? (const char *)p : "");
        snprintf(observation, sizeof observation, "%s",
                 (const char *)sqlite3_column_text(q, 2));
        snprintf(stored_state, sizeof stored_state, "%s",
                 (const char *)sqlite3_column_text(q, 3));
    }
    sqlite3_finalize(q);
    if (!found) { *state = "unknown"; return 0; }

    /* A standalone memory is born inside Chutni; there is no external file to
       stat. Re-hash the active memory artifact itself so check_freshness still
       detects catalog corruption instead of comparing two remembered hashes.
       The catalog is the original data store here, not a cache of bytes living
       somewhere else. */
    if (!strcmp(kind, "memory")) {
        if (strcmp(stored_state, "present")) {
            *state = !strcmp(stored_state, "missing") ? "missing" : "unknown";
            return 0;
        }
        if (sqlite3_prepare_v2(s->db,
                "SELECT inline_text, object_hash FROM artifacts"
                " WHERE source_id=?1 AND artifact_kind='memory'"
                " AND status='active' ORDER BY created_at DESC LIMIT 1",
                -1, &q, NULL) != SQLITE_OK) {
            *state = "unknown";
            return 0;
        }
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) != SQLITE_ROW) {
            sqlite3_finalize(q);
            *state = "unknown";
            return 0;
        }
        const void *inline_text = sqlite3_column_text(q, 0);
        int inline_bytes = sqlite3_column_bytes(q, 0);
        if (inline_text) {
            chutni_hash_bytes(inline_text, (size_t)inline_bytes, out);
            sqlite3_finalize(q);
            return 1;
        }
        const unsigned char *stored_object = sqlite3_column_text(q, 1);
        char object_hash[CHUTNI_HASH_STRLEN] = "";
        if (stored_object)
            snprintf(object_hash, sizeof object_hash, "%s",
                     (const char *)stored_object);
        sqlite3_finalize(q);
        void *payload = NULL;
        size_t payload_len = 0;
        if (!object_hash[0] ||
            chutni_object_get(s, object_hash, &payload, &payload_len) != CHUTNI_OK) {
            free(payload);
            *state = "unknown";
            return 0;
        }
        chutni_hash_bytes(payload, payload_len, out);
        free(payload);
        return 1;
    }

    if (!path[0]) { *state = "unknown"; return 0; }
    if (access(path, R_OK) != 0) { *state = "missing"; return 0; }

    if (!strcmp(kind, "directory")) {
        if (!is_dir(path)) { *state = "missing"; return 0; }
        /* An opaque directory was never opened, so the only thing ever claimed
           about it is that it is there — and it is. Re-enumerating to check
           would open a directory the policy said not to open (§11.1). */
        if (!strcmp(observation, "opaque")) { *state = "current"; return 0; }
        chutni_root_policy policy;
        policy_for_source(s, source_id, &policy);
        if (chutni_directory_listing_hash(path, &policy, out) != CHUTNI_OK) {
            *state = "unreadable";
            return 0;
        }
        return 1;
    }
    if (chutni_hash_file(path, out) != CHUTNI_OK) { *state = "unreadable"; return 0; }
    return 1;
}

static chutni_status artifact_freshness(chutni_store *s, const char *artifact_id,
                                        int depth, const char **out_state);

/* §13.3, second clause. A derived artifact is a claim about its inputs as much
   as about its source: a directory definition written from three child
   summaries is describing those summaries, so if one of them no longer
   describes anything real, neither does the definition. */
static int derivation_inputs_current(chutni_store *s, const char *artifact_id,
                                     int depth, const char **out_state) {
    char refs[8192] = "";
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(d.input_refs_json,'[]') FROM artifacts a"
            " JOIN derivations d ON d.derivation_id=a.derivation_id"
            " WHERE a.artifact_id=?1", -1, &q, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_text(q, 1, artifact_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) == SQLITE_ROW)
        snprintf(refs, sizeof refs, "%s", (const char *)sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
    if (!refs[0]) return 1;

    cj *list = cj_parse(refs, NULL);
    if (!list || list->type != CJ_ARR) { cj_free(list); return 1; }
    int all_current = 1;
    for (size_t i = 0; i < list->n && all_current; i++) {
        cj *entry = list->items[i];
        if (!entry || entry->type != CJ_OBJ) continue;
        const char *input = cj_get_str(entry, "artifact_id");
        if (!input) continue;
        cj *required = cj_get(entry, "required");
        if (required && required->type == CJ_BOOL && !required->bval) continue;
        const char *input_state = "unknown";
        artifact_freshness(s, input, depth + 1, &input_state);
        if (strcmp(input_state, "current")) {
            all_current = 0;
            /* Report the input's condition rather than a generic "stale", so a
               host can tell a vanished input from a rewritten one. */
            *out_state = !strcmp(input_state, "missing") ? "missing" : "stale";
        }
    }
    cj_free(list);
    return all_current;
}

static chutni_status artifact_freshness(chutni_store *s, const char *artifact_id,
                                        int depth, const char **out_state) {
    *out_state = "unknown";
    /* Derivation graphs are acyclic in a well-formed store, but nothing stops
       a writer from recording a cycle. Refusing to answer past a bound is
       honest; recursing forever is not, and guessing "current" would establish
       a claim from a failure to check (§13.2). */
    if (depth > 16) return CHUTNI_OK;

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT a.status, COALESCE(a.source_content_hash,''), a.source_id"
            " FROM artifacts a WHERE a.artifact_id=?1", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, artifact_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return CHUTNI_ERR_NOTFOUND;
    }
    char status[32], artifact_hash[CHUTNI_HASH_STRLEN], source_id[CHUTNI_ID_STRLEN];
    snprintf(status, sizeof status, "%s", (const char *)sqlite3_column_text(q, 0));
    snprintf(artifact_hash, sizeof artifact_hash, "%s", (const char *)sqlite3_column_text(q, 1));
    snprintf(source_id, sizeof source_id, "%s", (const char *)sqlite3_column_text(q, 2));
    sqlite3_finalize(q);

    if (strcmp(status, "active")) { *out_state = "stale"; return CHUTNI_OK; }
    if (!artifact_hash[0]) return CHUTNI_OK;

    char observed[CHUTNI_HASH_STRLEN];
    const char *reason = "unknown";
    if (!observe_source(s, source_id, observed, &reason)) {
        *out_state = reason;
        return CHUTNI_OK;
    }
    if (strcmp(observed, artifact_hash)) { *out_state = "stale"; return CHUTNI_OK; }

    const char *input_state = "stale";
    if (!derivation_inputs_current(s, artifact_id, depth, &input_state)) {
        *out_state = input_state;
        return CHUTNI_OK;
    }
    *out_state = "current";
    return CHUTNI_OK;
}

chutni_status chutni_check_freshness(chutni_store *s, const char *id, const char **out_state) {
    if (!s || !id || !out_state) return CHUTNI_ERR_INVALID;
    *out_state = "unknown";

    chutni_status st = artifact_freshness(s, id, 0, out_state);
    if (st == CHUTNI_OK) return CHUTNI_OK;
    if (st != CHUTNI_ERR_NOTFOUND) return st;

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(content_hash,'') FROM sources WHERE source_id=?1",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return fail(s, CHUTNI_ERR_NOTFOUND, "no source or artifact with id %s", id);
    }
    char stored[CHUTNI_HASH_STRLEN];
    snprintf(stored, sizeof stored, "%s", (const char *)sqlite3_column_text(q, 0));
    sqlite3_finalize(q);

    char observed[CHUTNI_HASH_STRLEN];
    const char *reason = "unknown";
    if (!observe_source(s, id, observed, &reason)) { *out_state = reason; return CHUTNI_OK; }
    if (!stored[0]) return CHUTNI_OK;
    *out_state = strcmp(observed, stored) ? "stale" : "current";
    return CHUTNI_OK;
}

chutni_status chutni_source_refresh(chutni_store *s, const char *source_id,
                                    const char **out_state) {
    if (!s || !source_id || !out_state) return CHUTNI_ERR_INVALID;
    *out_state = "unknown";
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(content_hash,'') FROM sources WHERE source_id=?1",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return fail(s, CHUTNI_ERR_NOTFOUND, "no source with id %s", source_id);
    }
    char stored[CHUTNI_HASH_STRLEN];
    snprintf(stored, sizeof stored, "%s", (const char *)sqlite3_column_text(q, 0));
    sqlite3_finalize(q);

    char now[32];
    iso_now(now);

    /* A file's bytes or a directory's listing, re-read from disk either way. */
    char actual[CHUTNI_HASH_STRLEN];
    const char *reason = "unknown";
    if (!observe_source(s, source_id, actual, &reason)) {
        if (!strcmp(reason, "missing"))
            chutni_source_set_state(s, source_id, CHUTNI_SOURCE_MISSING);
        else if (!strcmp(reason, "unreadable"))
            chutni_source_set_state(s, source_id, CHUTNI_SOURCE_UNREADABLE);
        *out_state = reason;
        /* An opaque directory has nothing to compare, and its artifacts still
           need their derivation inputs checked. */
        if (!strcmp(reason, "current")) {
            if (!sql_exec(s, "BEGIN IMMEDIATE")) return CHUTNI_ERR_DB;
            cascade_stale_dependents(s, now);
            if (!sql_exec(s, "COMMIT")) return CHUTNI_ERR_DB;
        }
        return CHUTNI_OK;
    }

    int drifted = !(stored[0] && !strcmp(stored, actual));
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
    /* The observation moved on. Demote every artifact that described the old
       one, then everything derived from those (§13.3). The cascade runs even
       when this source held still, because a source it depends on may not
       have. */
    if (drifted) stale_artifacts_not_matching(s, source_id, actual, now);
    cascade_stale_dependents(s, now);
    if (!sql_exec(s, "COMMIT")) return CHUTNI_ERR_DB;
    *out_state = drifted ? "stale" : "current";
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

/* §15.5–§15.7. The three hierarchical kinds mean specific things about what was
 * looked at, so the store enforces that they are attached to something that
 * could have been looked at, and that a definition says how far it looked.
 *
 * The coverage requirement is the whole reason these kinds are worth adding. A
 * definition written from four filenames and a definition written from the
 * entire subtree are otherwise the same record, and a second application
 * reading the store has no way to tell which one it is trusting. */
static chutni_status validate_hierarchical_artifact(chutni_store *s,
                                                    const chutni_artifact *a,
                                                    int is_directory,
                                                    const char *source_hash,
                                                    const char *observation) {
    int listing  = !strcmp(a->artifact_kind, CHUTNI_KIND_DIRECTORY_LISTING);
    int manifest = !strcmp(a->artifact_kind, CHUTNI_KIND_COVERAGE_MANIFEST);
    int definition = !strcmp(a->artifact_kind, CHUTNI_KIND_SOURCE_DEFINITION);

    if ((listing || manifest) && !is_directory)
        return fail(s, CHUTNI_ERR_INVALID,
                    "%s belongs to a directory source (§15.5, §15.7)",
                    a->artifact_kind);

    /* An unopened directory has no observation to bind a claim to. A producer
       that wants to describe one observes it first — one directory, no
       recursion — rather than describing the inside of a box it never opened. */
    if (is_directory && a->source_content_hash && !source_hash[0])
        return fail(s, CHUTNI_ERR_DENIED,
                    "directory source is %s; observe it before recording derived artifacts (§12.5)",
                    observation && *observation ? observation : "not enumerated");

    if (!definition || !is_directory) return CHUTNI_OK;

    if (!a->metadata_json)
        return fail(s, CHUTNI_ERR_INVALID,
                    "a directory source_definition must record its local coverage (§15.6)");
    cj *meta = cj_parse(a->metadata_json, NULL);
    cj *coverage = cj_get(meta, "coverage");
    const char *stop = coverage ? cj_get_str(coverage, "stop_reason") : NULL;
    cj *complete = coverage ? cj_get(coverage, "complete_for_policy") : NULL;
    int ok = coverage && coverage->type == CJ_OBJ && stop && *stop &&
             complete && complete->type == CJ_BOOL;
    cj_free(meta);
    if (!ok)
        return fail(s, CHUTNI_ERR_INVALID,
                    "a directory source_definition needs coverage.stop_reason and "
                    "coverage.complete_for_policy (§15.6)");
    return CHUTNI_OK;
}

static chutni_status validate_artifact(chutni_store *s,
                                       const chutni_artifact *a) {
    if (!a->source_id || !a->artifact_kind || !a->artifact_origin)
        return fail(s, CHUTNI_ERR_INVALID,
                    "source_id, artifact_kind, and artifact_origin are required");
    if (!a->object_hash && !a->inline_text)
        return fail(s, CHUTNI_ERR_INVALID,
                    "artifact needs object_hash or inline_text (§10)");
    if (a->object_hash && a->inline_text)
        return fail(s, CHUTNI_ERR_INVALID,
                    "artifact must use object_hash or inline_text, not both");

    static const char *origins[] = {
        "direct", "deterministic_transform", "model_generated", "human", NULL
    };
    int known = 0;
    for (const char **origin = origins; *origin; origin++)
        if (!strcmp(*origin, a->artifact_origin)) {
            known = 1;
            break;
        }
    if (!known)
        return fail(s, CHUTNI_ERR_INVALID,
                    "artifact_origin \"%s\" is not one of §15.1",
                    a->artifact_origin);

    int machine_derived =
        !strcmp(a->artifact_origin, "deterministic_transform") ||
        !strcmp(a->artifact_origin, "model_generated");
    if (machine_derived && !a->derivation_id)
        return fail(s, CHUTNI_ERR_INVALID,
                    "%s artifacts require processing provenance (§16.3)",
                    a->artifact_origin);
    if (machine_derived && !a->source_content_hash)
        return fail(s, CHUTNI_ERR_INVALID,
                    "%s artifacts require source_content_hash (§13.3)",
                    a->artifact_origin);

    if (a->selector_json &&
        !json_text_has_type(a->selector_json, CJ_OBJ))
        return fail(s, CHUTNI_ERR_INVALID,
                    "selector_json must be a JSON object");
    if (a->metadata_json &&
        !json_text_has_type(a->metadata_json, CJ_OBJ))
        return fail(s, CHUTNI_ERR_INVALID,
                    "artifact metadata_json must be a JSON object");
    if (a->source_content_hash && !hash_is_valid(a->source_content_hash))
        return fail(s, CHUTNI_ERR_INVALID,
                    "artifact source_content_hash is malformed");
    if (a->object_hash && !hash_is_valid(a->object_hash))
        return fail(s, CHUTNI_ERR_INVALID, "artifact object_hash is malformed");

    sqlite3_stmt *query = NULL;
    if (sqlite3_prepare_v2(
            s->db,
            "SELECT state, content_hash, COALESCE(source_kind,'file'),"
            " COALESCE(json_extract(metadata_json,'$.observation'),'')"
            " FROM sources WHERE source_id=?1",
            -1, &query, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(query, 1, a->source_id, -1, SQLITE_TRANSIENT);
    int row = sqlite3_step(query);
    char source_state[32] = "";
    char source_hash[CHUTNI_HASH_STRLEN] = "";
    char source_kind[32] = "";
    char observation[32] = "";
    if (row == SQLITE_ROW) {
        const unsigned char *state = sqlite3_column_text(query, 0);
        const unsigned char *hash = sqlite3_column_text(query, 1);
        if (state) snprintf(source_state, sizeof source_state, "%s",
                            (const char *)state);
        if (hash) snprintf(source_hash, sizeof source_hash, "%s",
                           (const char *)hash);
        snprintf(source_kind, sizeof source_kind, "%s",
                 (const char *)sqlite3_column_text(query, 2));
        snprintf(observation, sizeof observation, "%s",
                 (const char *)sqlite3_column_text(query, 3));
    }
    sqlite3_finalize(query);
    if (row != SQLITE_ROW)
        return fail(s, CHUTNI_ERR_NOTFOUND,
                    "artifact source_id is not in this store");

    int is_directory = !strcmp(source_kind, "directory");
    chutni_status hierarchy = validate_hierarchical_artifact(s, a, is_directory,
                                                             source_hash, observation);
    if (hierarchy != CHUTNI_OK) return hierarchy;

    if (a->source_content_hash &&
        (!source_hash[0] || strcmp(a->source_content_hash, source_hash)))
        return fail(s, CHUTNI_ERR_DENIED,
                    "artifact describes a different source version");
    if (a->source_content_hash && strcmp(source_state, "present"))
        return fail(s, CHUTNI_ERR_DENIED,
                    "artifact source is not currently present");

    if (a->derivation_id) {
        if (sqlite3_prepare_v2(
                s->db,
                "SELECT 1 FROM derivations WHERE derivation_id=?1",
                -1, &query, NULL) != SQLITE_OK)
            return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
        sqlite3_bind_text(query, 1, a->derivation_id, -1,
                          SQLITE_TRANSIENT);
        row = sqlite3_step(query);
        sqlite3_finalize(query);
        if (row != SQLITE_ROW)
            return fail(s, CHUTNI_ERR_NOTFOUND,
                        "artifact derivation_id is not in this store");
    }
    if (!strcmp(a->artifact_origin, "model_generated")) {
        if (sqlite3_prepare_v2(
                s->db,
                "SELECT p.model_id, p.app_name, p.app_version"
                " FROM derivations d JOIN producers p"
                " ON p.producer_id=d.producer_id"
                " WHERE d.derivation_id=?1",
                -1, &query, NULL) != SQLITE_OK)
            return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
        sqlite3_bind_text(query, 1, a->derivation_id, -1,
                          SQLITE_TRANSIENT);
        row = sqlite3_step(query);
        int complete =
            row == SQLITE_ROW &&
            sqlite3_column_text(query, 0) &&
            sqlite3_column_text(query, 1) &&
            sqlite3_column_text(query, 2);
        sqlite3_finalize(query);
        if (!complete)
            return fail(s, CHUTNI_ERR_INVALID,
                        "model-generated artifacts require model and host application identity (§16.2)");
    }
    if (a->object_hash) {
        if (sqlite3_prepare_v2(
                s->db, "SELECT 1 FROM objects WHERE object_hash=?1",
                -1, &query, NULL) != SQLITE_OK)
            return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
        sqlite3_bind_text(query, 1, a->object_hash, -1, SQLITE_TRANSIENT);
        row = sqlite3_step(query);
        sqlite3_finalize(query);
        if (row != SQLITE_ROW)
            return fail(s, CHUTNI_ERR_NOTFOUND,
                        "artifact object_hash is not in this store");
    }
    if (a->supersedes_artifact_id) {
        if (sqlite3_prepare_v2(
                s->db,
                "SELECT source_id FROM artifacts WHERE artifact_id=?1",
                -1, &query, NULL) != SQLITE_OK)
            return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
        sqlite3_bind_text(query, 1, a->supersedes_artifact_id, -1,
                          SQLITE_TRANSIENT);
        row = sqlite3_step(query);
        int same_source =
            row == SQLITE_ROW &&
            sqlite3_column_text(query, 0) &&
            !strcmp((const char *)sqlite3_column_text(query, 0),
                    a->source_id);
        sqlite3_finalize(query);
        if (!same_source)
            return fail(s, CHUTNI_ERR_INVALID,
                        "an artifact may only supersede one from the same source");
    }
    return CHUTNI_OK;
}

chutni_status chutni_artifact_put(chutni_store *s, const chutni_artifact *a,
                                  char artifact_id[CHUTNI_ID_STRLEN]) {
    if (!s || !a || !artifact_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    chutni_status validation = validate_artifact(s, a);
    if (validation != CHUTNI_OK) return validation;

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

chutni_status chutni_artifacts_put(
    chutni_store *s,
    const chutni_producer *producer,
    const char *operation,
    const char *recipe_hash,
    const char *parameters_json,
    const char *input_refs_json,
    const chutni_artifact *artifacts,
    size_t artifact_count,
    char producer_id[CHUTNI_ID_STRLEN],
    char derivation_id[CHUTNI_ID_STRLEN],
    char (*artifact_ids)[CHUTNI_ID_STRLEN]) {
    if (!s || !producer || !operation || !*operation || !artifacts ||
        !artifact_count || !producer_id || !derivation_id || !artifact_ids)
        return CHUTNI_ERR_INVALID;
    if (s->read_only)
        return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    for (size_t i = 0; i < artifact_count; i++)
        if (!artifacts[i].source_content_hash)
            return fail(s, CHUTNI_ERR_INVALID,
                        "generic submissions require source_content_hash for every artifact");

    if (!sql_exec(s, "BEGIN IMMEDIATE")) return CHUTNI_ERR_DB;
    chutni_status status =
        chutni_producer_put(s, producer, producer_id);
    if (status == CHUTNI_OK)
        status = chutni_derivation_put(
            s, producer_id, operation, recipe_hash, parameters_json,
            input_refs_json, derivation_id);
    for (size_t i = 0; status == CHUTNI_OK && i < artifact_count; i++) {
        chutni_artifact item = artifacts[i];
        item.derivation_id = derivation_id;
        status = chutni_artifact_put(s, &item, artifact_ids[i]);
    }
    if (status == CHUTNI_OK && sql_exec(s, "COMMIT"))
        return CHUTNI_OK;
    if (status == CHUTNI_OK) status = CHUTNI_ERR_DB;

    char detail[ERRBUF];
    snprintf(detail, sizeof detail, "%s", chutni_last_error(s));
    sql_exec(s, "ROLLBACK");
    return fail(s, status, "%s",
                detail[0] ? detail : chutni_strerror(status));
}

chutni_status chutni_memory_put(
    chutni_store *s,
    const chutni_memory *memory,
    char source_id[CHUTNI_ID_STRLEN],
    char artifact_id[CHUTNI_ID_STRLEN],
    char producer_id[CHUTNI_ID_STRLEN],
    char derivation_id[CHUTNI_ID_STRLEN]) {
    if (!s || !memory || !source_id || !artifact_id || !producer_id ||
        !derivation_id)
        return CHUTNI_ERR_INVALID;
    if (s->read_only)
        return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (!memory->memory_kind || !*memory->memory_kind ||
        !memory->text || !*memory->text || !memory->producer ||
        !memory->operation || !*memory->operation)
        return fail(s, CHUTNI_ERR_INVALID,
                    "memory_kind, text, producer, and operation are required");
    if (memory->parameters_json &&
        !json_text_has_type(memory->parameters_json, CJ_OBJ))
        return fail(s, CHUTNI_ERR_INVALID,
                    "memory parameters_json must be a JSON object");
    if (memory->input_refs_json &&
        !json_text_has_type(memory->input_refs_json, CJ_ARR))
        return fail(s, CHUTNI_ERR_INVALID,
                    "memory input_refs_json must be a JSON array");

    if (!uuid7(source_id))
        return fail(s, CHUTNI_ERR_IO, "no entropy");
    char content_hash[CHUTNI_HASH_STRLEN];
    chutni_status status =
        chutni_hash_bytes(memory->text, strlen(memory->text), content_hash);
    if (status != CHUTNI_OK) return status;

    char fallback_title[CHUTNI_ID_STRLEN + 8];
    snprintf(fallback_title, sizeof fallback_title, "memory:%s", source_id);
    cj *locator = cj_obj();
    cj_set(locator, "scheme", cj_str("chutni-memory"));
    cj_set(locator, "memory_id", cj_str(source_id));
    cj_set(locator, "display_path",
           cj_str(memory->title && *memory->title
                      ? memory->title
                      : fallback_title));
    char *locator_json = cj_dump(locator, -1);
    cj_free(locator);

    cj *metadata = cj_obj();
    cj_set(metadata, "memory_kind", cj_str(memory->memory_kind));
    cj_set(metadata, "standalone", cj_bool(1));
    if (memory->title && *memory->title)
        cj_set(metadata, "title", cj_str(memory->title));
    if (memory->scope && *memory->scope)
        cj_set(metadata, "scope", cj_str(memory->scope));
    char *metadata_json = cj_dump(metadata, -1);
    cj_free(metadata);
    if (!locator_json || !metadata_json) {
        free(locator_json);
        free(metadata_json);
        return CHUTNI_ERR_NOMEM;
    }

    char now[32];
    iso_now(now);
    if (!sql_exec(s, "BEGIN IMMEDIATE")) {
        free(locator_json);
        free(metadata_json);
        return CHUTNI_ERR_DB;
    }

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO sources(source_id,root_id,parent_source_id,source_kind,"
            "locator_json,display_name,media_type,size_bytes,content_hash,state,"
            "first_seen_at,last_seen_at,last_scanned_at,metadata_json)"
            " VALUES(?1,NULL,NULL,'memory',?2,?3,'text/plain; charset=utf-8',"
            "?4,?5,'present',?6,?6,?6,?7)", -1, &q, NULL) != SQLITE_OK) {
        status = fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    } else {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 2, locator_json, -1, SQLITE_TRANSIENT);
        bind_text_or_null(q, 3, memory->title);
        sqlite3_bind_int64(q, 4, (sqlite3_int64)strlen(memory->text));
        sqlite3_bind_text(q, 5, content_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 6, now, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 7, metadata_json, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(q);
        sqlite3_finalize(q);
        q = NULL;
        status = rc == SQLITE_DONE
                     ? CHUTNI_OK
                     : fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    }

    if (status == CHUTNI_OK)
        status = chutni_producer_put(s, memory->producer, producer_id);
    if (status == CHUTNI_OK)
        status = chutni_derivation_put(
            s, producer_id, memory->operation, memory->recipe_hash,
            memory->parameters_json ? memory->parameters_json : "{}",
            memory->input_refs_json ? memory->input_refs_json : "[]",
            derivation_id);
    if (status == CHUTNI_OK) {
        const char *producer_kind = memory->producer->producer_kind;
        const char *origin =
            producer_kind && !strcmp(producer_kind, "model")
                ? "model_generated"
                : producer_kind && !strcmp(producer_kind, "human")
                      ? "human"
                      : "direct";
        chutni_artifact artifact;
        memset(&artifact, 0, sizeof artifact);
        artifact.source_id = source_id;
        artifact.artifact_kind = CHUTNI_KIND_MEMORY;
        artifact.artifact_origin = origin;
        artifact.media_type = "text/plain; charset=utf-8";
        artifact.inline_text = memory->text;
        artifact.language = memory->language;
        artifact.source_content_hash = content_hash;
        artifact.derivation_id = derivation_id;
        artifact.metadata_json = metadata_json;
        status = chutni_artifact_put(s, &artifact, artifact_id);
    }

    free(locator_json);
    free(metadata_json);
    if (status == CHUTNI_OK && sql_exec(s, "COMMIT")) {
        advertise_capability(s, "standalone_memory");
        return CHUTNI_OK;
    }
    if (status == CHUTNI_OK) status = CHUTNI_ERR_DB;
    char detail[ERRBUF];
    snprintf(detail, sizeof detail, "%s", chutni_last_error(s));
    sql_exec(s, "ROLLBACK");
    return fail(s, status, "%s",
                detail[0] ? detail : chutni_strerror(status));
}

/* ------------------------------------------------------------- relationships */

static int id_exists(chutni_store *s, const char *id) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT 1 FROM sources WHERE source_id=?1"
            " UNION ALL SELECT 1 FROM artifacts WHERE artifact_id=?1",
            -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
    int found = sqlite3_step(q) == SQLITE_ROW;
    sqlite3_finalize(q);
    return found;
}

chutni_status chutni_relation_put(chutni_store *s, const char *from_id,
                                  const char *predicate, const char *to_id,
                                  const char *derivation_id,
                                  const char *metadata_json,
                                  char relation_id[CHUTNI_ID_STRLEN]) {
    if (!s || !from_id || !predicate || !*predicate || !to_id || !relation_id)
        return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    /* §18 requires a model-created relation to carry a derivation ID, and
       nothing downstream can tell which relations came from a model. Requiring
       one from everybody is the only version of that rule a store can actually
       enforce, and a relation is a claim like any other. */
    if (!derivation_id)
        return fail(s, CHUTNI_ERR_INVALID,
                    "a relation requires processing provenance (§18)");
    if (metadata_json && !json_text_has_type(metadata_json, CJ_OBJ))
        return fail(s, CHUTNI_ERR_INVALID, "relation metadata_json must be a JSON object");
    if (!id_exists(s, from_id))
        return fail(s, CHUTNI_ERR_NOTFOUND, "relation from_id is not in this store");
    if (!id_exists(s, to_id))
        return fail(s, CHUTNI_ERR_NOTFOUND, "relation to_id is not in this store");

    /* Re-asserting a relation is not a new fact about the world. Repeating a
       scan would otherwise multiply every `contains` edge by the scan count. */
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT relation_id FROM relations"
            " WHERE from_id=?1 AND predicate=?2 AND to_id=?3 AND derivation_id IS ?4",
            -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, from_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 2, predicate, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 3, to_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 4, derivation_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            snprintf(relation_id, CHUTNI_ID_STRLEN, "%s",
                     (const char *)sqlite3_column_text(q, 0));
            sqlite3_finalize(q);
            return CHUTNI_OK;
        }
        sqlite3_finalize(q);
    }

    if (!uuid7(relation_id)) return fail(s, CHUTNI_ERR_IO, "no entropy");
    char now[32];
    iso_now(now);
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO relations(relation_id,from_id,predicate,to_id,derivation_id,"
            "created_at,metadata_json) VALUES(?1,?2,?3,?4,?5,?6,?7)",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, relation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, from_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 3, predicate, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 4, to_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 5, derivation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 6, now, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 7, metadata_json);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    return CHUTNI_OK;
}

chutni_status chutni_relations_list(chutni_store *s, const char *from_id,
                                    const char *predicate,
                                    chutni_relation_info **out, size_t *count) {
    if (!s || !out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT relation_id, from_id, predicate, to_id, derivation_id,"
            " created_at, metadata_json FROM relations"
            " WHERE (?1 IS NULL OR from_id=?1) AND (?2 IS NULL OR predicate=?2)"
            " ORDER BY created_at, relation_id", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    bind_text_or_null(q, 1, from_id);
    bind_text_or_null(q, 2, predicate);
    chutni_relation_info *vec = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            size_t c = cap ? cap * 2 : 16;
            chutni_relation_info *t = realloc(vec, c * sizeof *t);
            if (!t) break;
            vec = t;
            cap = c;
        }
        vec[n].relation_id   = dup_col(q, 0);
        vec[n].from_id       = dup_col(q, 1);
        vec[n].predicate     = dup_col(q, 2);
        vec[n].to_id         = dup_col(q, 3);
        vec[n].derivation_id = dup_col(q, 4);
        vec[n].created_at    = dup_col(q, 5);
        vec[n].metadata_json = dup_col(q, 6);
        n++;
    }
    sqlite3_finalize(q);
    *out = vec;
    *count = n;
    return CHUTNI_OK;
}

void chutni_relation_info_free(chutni_relation_info *relations, size_t count) {
    if (!relations) return;
    for (size_t i = 0; i < count; i++) {
        free(relations[i].relation_id);
        free(relations[i].from_id);
        free(relations[i].predicate);
        free(relations[i].to_id);
        free(relations[i].derivation_id);
        free(relations[i].created_at);
        free(relations[i].metadata_json);
    }
    free(relations);
}

/* ----------------------------------------------------------------- coverage */

/* An artifact's payload as text, whether it was stored inline or as an object. */
static char *artifact_payload_text(chutni_store *s, const char *artifact_id) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT inline_text, object_hash FROM artifacts WHERE artifact_id=?1",
            -1, &q, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(q, 1, artifact_id, -1, SQLITE_TRANSIENT);
    char *text = NULL, hash[CHUTNI_HASH_STRLEN] = "";
    if (sqlite3_step(q) == SQLITE_ROW) {
        const unsigned char *inline_text = sqlite3_column_text(q, 0);
        const unsigned char *object_hash = sqlite3_column_text(q, 1);
        if (inline_text) text = strdup((const char *)inline_text);
        else if (object_hash) snprintf(hash, sizeof hash, "%s", (const char *)object_hash);
    }
    sqlite3_finalize(q);
    if (text || !hash[0]) return text;

    void *data = NULL;
    size_t len = 0;
    if (chutni_object_get(s, hash, &data, &len) != CHUTNI_OK) return NULL;
    char *copy = malloc(len + 1);
    if (copy) { memcpy(copy, data, len); copy[len] = 0; }
    free(data);
    return copy;
}

/* The directory source at the top of a region: the one a coverage manifest is
   attached to. Walks parents rather than trusting root_id alone, because a
   source may predate hierarchical scanning and have no parent recorded. */
static int region_root_source(chutni_store *s, const char *source_id,
                              char out[CHUTNI_ID_STRLEN]) {
    char current[CHUTNI_ID_STRLEN];
    snprintf(current, sizeof current, "%s", source_id);
    for (int hop = 0; hop < 256; hop++) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(s->db,
                "SELECT COALESCE(parent_source_id,'') FROM sources WHERE source_id=?1",
                -1, &q, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_text(q, 1, current, -1, SQLITE_TRANSIENT);
        int found = sqlite3_step(q) == SQLITE_ROW;
        char parent[CHUTNI_ID_STRLEN] = "";
        if (found) snprintf(parent, sizeof parent, "%s",
                            (const char *)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
        if (!found) return 0;
        if (!parent[0]) {
            snprintf(out, CHUTNI_ID_STRLEN, "%s", current);
            return 1;
        }
        snprintf(current, sizeof current, "%s", parent);
    }
    return 0;
}

/* The current coverage manifest for a region, or "" when there is none. */
static void current_coverage_manifest(chutni_store *s, const char *root_source_id,
                                      char out[CHUTNI_ID_STRLEN]) {
    out[0] = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT artifact_id FROM artifacts WHERE source_id=?1"
            " AND artifact_kind='" CHUTNI_KIND_COVERAGE_MANIFEST "' AND status='active'"
            " ORDER BY created_at DESC, artifact_id DESC LIMIT 1",
            -1, &q, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(q, 1, root_source_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) == SQLITE_ROW)
        snprintf(out, CHUTNI_ID_STRLEN, "%s", (const char *)sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
}

chutni_status chutni_get_coverage(chutni_store *s, const char *id, char **json) {
    if (!s || !id || !json) return CHUTNI_ERR_INVALID;
    *json = NULL;

    /* A root id and a source id are both reasonable things for a caller to
       have; resolve either to the directory source the manifest hangs off. */
    char source_id[CHUTNI_ID_STRLEN] = "";
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT source_id FROM sources WHERE root_id=?1 AND parent_source_id IS NULL"
            " AND source_kind='directory' ORDER BY first_seen_at LIMIT 1",
            -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW)
            snprintf(source_id, sizeof source_id, "%s",
                     (const char *)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    if (!source_id[0]) snprintf(source_id, sizeof source_id, "%s", id);

    char kind[32] = "", observation[32] = "", state[32] = "";
    int depth = -1, found = 0;
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(source_kind,'file'),"
            " COALESCE(json_extract(metadata_json,'$.observation'),''),"
            " COALESCE(json_extract(metadata_json,'$.depth'),-1), state"
            " FROM sources WHERE source_id=?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            found = 1;
            snprintf(kind, sizeof kind, "%s", (const char *)sqlite3_column_text(q, 0));
            snprintf(observation, sizeof observation, "%s",
                     (const char *)sqlite3_column_text(q, 1));
            depth = sqlite3_column_int(q, 2);
            snprintf(state, sizeof state, "%s", (const char *)sqlite3_column_text(q, 3));
        }
        sqlite3_finalize(q);
    }
    if (!found) return fail(s, CHUTNI_ERR_NOTFOUND, "no root or source with id %s", id);

    char region[CHUTNI_ID_STRLEN] = "", manifest_id[CHUTNI_ID_STRLEN] = "";
    if (region_root_source(s, source_id, region))
        current_coverage_manifest(s, region, manifest_id);

    cj *root = cj_obj();
    cj_set(root, "source_id", cj_str(source_id));
    cj_set(root, "source_kind", cj_str(kind));
    cj_set(root, "state", cj_str(state));
    if (observation[0]) cj_set(root, "observation", cj_str(observation));
    cj_set(root, "depth", depth < 0 ? cj_null() : cj_num((double)depth));
    cj_set(root, "root_source_id", region[0] ? cj_str(region) : cj_null());
    cj_set(root, "coverage_manifest_id", manifest_id[0] ? cj_str(manifest_id) : cj_null());

    if (manifest_id[0]) {
        char *payload = artifact_payload_text(s, manifest_id);
        cj *parsed = payload ? cj_parse(payload, NULL) : NULL;
        free(payload);
        cj_set(root, "coverage_manifest", parsed ? parsed : cj_null());
    } else {
        /* An explicit null, not an omitted key. Silence here would read as
           "nothing was covered"; it means nobody recorded coverage, which a
           consumer must handle differently (§35.1). */
        cj_set(root, "coverage_manifest", cj_null());
    }

    /* A directory's own definition carries a local coverage block the
       region-wide manifest does not: how far that one definition looked. */
    cj *local = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT artifact_id, metadata_json FROM artifacts WHERE source_id=?1"
            " AND artifact_kind='" CHUTNI_KIND_SOURCE_DEFINITION "' AND status='active'"
            " ORDER BY created_at DESC LIMIT 1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, source_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            const unsigned char *aid = sqlite3_column_text(q, 0);
            const unsigned char *meta = sqlite3_column_text(q, 1);
            local = cj_obj();
            cj_set(local, "artifact_id", cj_str(aid ? (const char *)aid : ""));
            cj *parsed = meta ? cj_parse((const char *)meta, NULL) : NULL;
            cj *coverage = cj_get(parsed, "coverage");
            /* Re-serialize rather than hand out a subtree the parse tree owns. */
            char *text = coverage ? cj_dump(coverage, -1) : NULL;
            cj_free(parsed);
            cj *copy = text ? cj_parse(text, NULL) : NULL;
            free(text);
            cj_set(local, "coverage", copy ? copy : cj_null());
        }
        sqlite3_finalize(q);
    }
    cj_set(root, "definition", local ? local : cj_null());

    *json = cj_dump(root, 2);
    cj_free(root);
    return *json ? CHUTNI_OK : CHUTNI_ERR_NOMEM;
}

/* ----------------------------------------------------------- representations */

/* A stored vector is a self-describing object, so a reader that finds one
 * without its catalog row can still tell what it is:
 *
 *   offset  size  meaning
 *        0     8  magic "CHUTVEC1"
 *        8     4  dimensions, little-endian uint32
 *       12     1  dtype: 1 = IEEE-754 binary32
 *       13     1  normalization: 0 = none, 1 = l2
 *       14     2  reserved, zero
 *       16   4*d  the vector itself, little-endian binary32
 *
 * Byte order is written out explicitly rather than memcpy'd wholesale, so that
 * a store written on one architecture reads correctly on another (§26). This
 * assumes float is IEEE-754 binary32, which holds on every platform this
 * targets; a machine where it does not would need a conversion here rather than
 * a bit copy. */
#define VEC_MAGIC     "CHUTVEC1"
#define VEC_MAGIC_LEN 8
#define VEC_HEADER    16
#define VEC_DTYPE_F32 1
#define VEC_MEDIA_TYPE "application/vnd.chutni.vector"

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_f32le(uint8_t *p, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    put_u32le(p, bits);
}

static float get_f32le(const uint8_t *p) {
    uint32_t bits = get_u32le(p);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static int norm_code(const char *normalization) {
    return normalization && !strcmp(normalization, "l2") ? 1 : 0;
}

static uint8_t *vector_encode(const float *v, size_t dims, const char *normalization,
                              size_t *len_out) {
    size_t len = VEC_HEADER + dims * 4;
    uint8_t *buf = calloc(1, len);
    if (!buf) return NULL;
    memcpy(buf, VEC_MAGIC, VEC_MAGIC_LEN);
    put_u32le(buf + 8, (uint32_t)dims);
    buf[12] = VEC_DTYPE_F32;
    buf[13] = (uint8_t)norm_code(normalization);
    for (size_t i = 0; i < dims; i++) put_f32le(buf + VEC_HEADER + i * 4, v[i]);
    *len_out = len;
    return buf;
}

/* Decodes into a caller-owned float array. The header's own dimension count is
 * checked against the payload length, so a truncated object is rejected rather
 * than read past. */
static chutni_status vector_decode(const uint8_t *buf, size_t len, const char *normalization,
                                   float **out, size_t *dims_out) {
    if (len < VEC_HEADER || memcmp(buf, VEC_MAGIC, VEC_MAGIC_LEN))
        return CHUTNI_ERR_FORMAT;
    if (buf[12] != VEC_DTYPE_F32 || buf[13] > 1 || buf[14] || buf[15] ||
        buf[13] != (uint8_t)norm_code(normalization)) return CHUTNI_ERR_FORMAT;
    uint32_t dims = get_u32le(buf + 8);
    if (!dims || (size_t)dims > (len - VEC_HEADER) / 4) return CHUTNI_ERR_FORMAT;
    if (len - VEC_HEADER != (size_t)dims * 4) return CHUTNI_ERR_FORMAT;
    float *v = malloc((size_t)dims * sizeof *v);
    if (!v) return CHUTNI_ERR_NOMEM;
    for (uint32_t i = 0; i < dims; i++) v[i] = get_f32le(buf + VEC_HEADER + i * 4);
    *out = v;
    *dims_out = dims;
    return CHUTNI_OK;
}

/* The hash of what a representation was actually built from. An object-backed
 * artifact is already content-addressed, so its object hash is that answer and
 * costs nothing; inline text has to be hashed. */
static chutni_status artifact_payload_hash(const char *inline_text, const char *object_hash,
                                           char out[CHUTNI_HASH_STRLEN]) {
    if (object_hash && *object_hash) {
        snprintf(out, CHUTNI_HASH_STRLEN, "%s", object_hash);
        return CHUTNI_OK;
    }
    if (!inline_text) return CHUTNI_ERR_FORMAT;
    return chutni_hash_bytes(inline_text, strlen(inline_text), out);
}

static chutni_status artifact_payload_hash_of(chutni_store *s, const char *artifact_id,
                                              char out[CHUTNI_HASH_STRLEN]) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT inline_text, object_hash FROM artifacts WHERE artifact_id=?1",
            -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, artifact_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return fail(s, CHUTNI_ERR_NOTFOUND, "no artifact with id %s", artifact_id);
    }
    const unsigned char *txt = sqlite3_column_text(q, 0);
    const unsigned char *obj = sqlite3_column_text(q, 1);
    char text_copy[4096], obj_copy[CHUTNI_HASH_STRLEN];
    char *heap = NULL;
    const char *text = NULL;
    obj_copy[0] = 0;
    if (obj) snprintf(obj_copy, sizeof obj_copy, "%s", (const char *)obj);
    if (txt && !obj_copy[0]) {
        size_t n = strlen((const char *)txt);
        if (n < sizeof text_copy) {
            memcpy(text_copy, txt, n + 1);
            text = text_copy;
        } else {
            heap = malloc(n + 1);
            if (!heap) { sqlite3_finalize(q); return CHUTNI_ERR_NOMEM; }
            memcpy(heap, txt, n + 1);
            text = heap;
        }
    }
    sqlite3_finalize(q);
    chutni_status st = artifact_payload_hash(text, obj_copy, out);
    free(heap);
    if (st != CHUTNI_OK)
        return fail(s, st, "artifact %s has no payload to hash", artifact_id);
    return CHUTNI_OK;
}

chutni_status chutni_representation_put(chutni_store *s, const char *artifact_id,
                                        const chutni_representation_profile *p,
                                        const float *vector, size_t dimensions,
                                        char representation_id[CHUTNI_ID_STRLEN]) {
    if (!s || !artifact_id || !p || !vector || !representation_id) return CHUTNI_ERR_INVALID;
    if (s->read_only) return fail(s, CHUTNI_ERR_READONLY, "store is read-only");
    if (!dimensions) return fail(s, CHUTNI_ERR_INVALID, "a vector needs at least one dimension");
    if (!p->representation_kind)
        return fail(s, CHUTNI_ERR_INVALID, "representation_kind is required (§17.2)");

    /* §17.1. Without these a consumer cannot tell whether the vector is usable,
       and an unusable vector that looks usable is worse than none at all. */
    if (!p->model_id || !p->model_revision || !p->dtype || !p->normalization ||
        p->dimensions <= 0)
        return fail(s, CHUTNI_ERR_INVALID,
                    "model_id, model_revision, dimensions, dtype and normalization "
                    "are required (§17.1)");
    if (strcmp(p->dtype, "f32"))
        return fail(s, CHUTNI_ERR_INVALID,
                    "v0.1 serializes f32 vectors, not \"%s\"", p->dtype);
    if (strcmp(p->normalization, "none") && strcmp(p->normalization, "l2"))
        return fail(s, CHUTNI_ERR_INVALID,
                    "normalization must be \"none\" or \"l2\", not \"%s\"", p->normalization);
    if (p->dimensions && (size_t)p->dimensions != dimensions)
        return fail(s, CHUTNI_ERR_INVALID,
                    "profile declares %d dimensions but %zu were supplied",
                    p->dimensions, dimensions);
    if (dimensions > UINT32_MAX || dimensions > (SIZE_MAX - VEC_HEADER) / 4)
        return fail(s, CHUTNI_ERR_INVALID, "vector dimensions are too large for v0.1 encoding");

    char payload_hash[CHUTNI_HASH_STRLEN];
    chutni_status st = artifact_payload_hash_of(s, artifact_id, payload_hash);
    if (st != CHUTNI_OK) return st;

    size_t len = 0;
    uint8_t *blob = vector_encode(vector, dimensions, p->normalization, &len);
    if (!blob) return CHUTNI_ERR_NOMEM;
    char object_hash[CHUTNI_HASH_STRLEN];
    st = chutni_object_put(s, blob, len, VEC_MEDIA_TYPE, object_hash);
    free(blob);
    if (st != CHUTNI_OK) return st;

    if (!uuid7(representation_id)) return fail(s, CHUTNI_ERR_IO, "no entropy");
    char now[32];
    iso_now(now);

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO representations(representation_id,artifact_id,representation_kind,"
            "object_hash,model_id,model_revision,dimensions,dtype,normalization,"
            "tokenizer_hash,projector_hash,source_artifact_hash,created_at)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, representation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, artifact_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 3, p->representation_kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 4, object_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 5, p->model_id, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 6, p->model_revision);
    sqlite3_bind_int64(q, 7, (sqlite3_int64)dimensions);
    sqlite3_bind_text(q, 8, p->dtype, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 9, p->normalization, -1, SQLITE_TRANSIENT);
    bind_text_or_null(q, 10, p->tokenizer_hash);
    bind_text_or_null(q, 11, p->projector_hash);
    sqlite3_bind_text(q, 12, payload_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 13, now, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? CHUTNI_OK : fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
}

/* Profile fields match only when both sides say the same thing, including both
 * saying nothing. A caller that leaves a field unset has not declared it
 * compatible, so a stored representation carrying that field is refused rather
 * than assumed acceptable — §16.2's warning that two records sharing a
 * marketing name need not share weights applies to every one of these. */
static int prof_eq(const char *stored, const char *accepted) {
    if (!stored && !accepted) return 1;
    if (!stored || !accepted) return 0;
    return !strcmp(stored, accepted);
}

chutni_status chutni_representation_get(chutni_store *s, const char *representation_id,
                                        const chutni_representation_profile *accepted,
                                        float **vector, size_t *dimensions) {
    if (!s || !representation_id || !accepted || !vector || !dimensions)
        return CHUTNI_ERR_INVALID;
    *vector = NULL;
    *dimensions = 0;

    if (!accepted->representation_kind || !accepted->model_id || !accepted->dtype ||
        !accepted->normalization || accepted->dimensions <= 0)
        return fail(s, CHUTNI_ERR_INVALID,
                    "the accepted profile must state representation_kind, model_id, "
                    "dimensions, dtype and normalization (§22.6)");

    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT representation_kind, model_id, model_revision, dimensions, dtype,"
            "       normalization, tokenizer_hash, projector_hash, object_hash,"
            "       source_artifact_hash, artifact_id"
            " FROM representations WHERE representation_id=?1", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, representation_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return fail(s, CHUTNI_ERR_NOTFOUND, "no representation with id %s", representation_id);
    }

    char kind[128], model[256], rev[128], dtype[32], norm[32];
    char tok[CHUTNI_HASH_STRLEN], proj[CHUTNI_HASH_STRLEN];
    char objhash[CHUTNI_HASH_STRLEN], srchash[CHUTNI_HASH_STRLEN], art[CHUTNI_ID_STRLEN];
    int have_rev, have_tok, have_proj;
#define COPY_COL(idx, buf, flag)                                                    \
    do {                                                                            \
        const unsigned char *v_ = sqlite3_column_text(q, (idx));                    \
        flag = v_ != NULL;                                                          \
        snprintf((buf), sizeof(buf), "%s", v_ ? (const char *)v_ : "");             \
    } while (0)
    int ignored;
    COPY_COL(0, kind, ignored);
    COPY_COL(1, model, ignored);
    COPY_COL(2, rev, have_rev);
    int dims = sqlite3_column_int(q, 3);
    COPY_COL(4, dtype, ignored);
    COPY_COL(5, norm, ignored);
    COPY_COL(6, tok, have_tok);
    COPY_COL(7, proj, have_proj);
    COPY_COL(8, objhash, ignored);
    COPY_COL(9, srchash, ignored);
    COPY_COL(10, art, ignored);
#undef COPY_COL
    (void)ignored;
    sqlite3_finalize(q);

    if (!prof_eq(kind, accepted->representation_kind) ||
        !prof_eq(model, accepted->model_id) ||
        !prof_eq(have_rev ? rev : NULL, accepted->model_revision) ||
        !prof_eq(dtype, accepted->dtype) ||
        !prof_eq(norm, accepted->normalization) ||
        !prof_eq(have_tok ? tok : NULL, accepted->tokenizer_hash) ||
        !prof_eq(have_proj ? proj : NULL, accepted->projector_hash) ||
        dims != accepted->dimensions)
        return fail(s, CHUTNI_ERR_DENIED,
                    "representation is %s/%s rev %s, %d-dim %s %s — the caller "
                    "did not declare that profile acceptable (§22.6)",
                    kind, model, have_rev ? rev : "(none)", dims, dtype, norm);

    /* §17.5's rule for indexes applies to a single vector too: if the artifact
       no longer holds the payload this was computed from, the vector describes
       something that is gone and must be regenerated, not reinterpreted. */
    char current[CHUTNI_HASH_STRLEN];
    if (artifact_payload_hash_of(s, art, current) == CHUTNI_OK && strcmp(current, srchash))
        return fail(s, CHUTNI_ERR_INVALID,
                    "representation was computed from a payload artifact %s no longer "
                    "holds; regenerate it (§17.5)", art);

    void *data = NULL;
    size_t len = 0;
    chutni_status st = chutni_object_get(s, objhash, &data, &len);
    if (st != CHUTNI_OK) return st;
    st = vector_decode(data, len, norm, vector, dimensions);
    free(data);
    if (st != CHUTNI_OK)
        return fail(s, st, "vector object %s is malformed", objhash);
    return CHUTNI_OK;
}

chutni_status chutni_representations_list(chutni_store *s, const char *artifact_id,
                                          chutni_representation_info **out, size_t *count) {
    if (!s || !out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;

    const char *sql = artifact_id
        ? "SELECT representation_id,artifact_id,representation_kind,model_id,model_revision,"
          " dtype,normalization,dimensions,source_artifact_hash FROM representations"
          " WHERE artifact_id=?1 ORDER BY created_at"
        : "SELECT representation_id,artifact_id,representation_kind,model_id,model_revision,"
          " dtype,normalization,dimensions,source_artifact_hash FROM representations"
          " ORDER BY created_at";
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    if (artifact_id) sqlite3_bind_text(q, 1, artifact_id, -1, SQLITE_TRANSIENT);

    chutni_representation_info *vec = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            size_t c = cap ? cap * 2 : 8;
            chutni_representation_info *t = realloc(vec, c * sizeof *t);
            if (!t) break;
            vec = t;
            cap = c;
        }
        chutni_representation_info *r = &vec[n];
        memset(r, 0, sizeof *r);
        r->representation_id   = dup_col(q, 0);
        r->artifact_id         = dup_col(q, 1);
        r->representation_kind = dup_col(q, 2);
        r->model_id            = dup_col(q, 3);
        r->model_revision      = dup_col(q, 4);
        r->dtype               = dup_col(q, 5);
        r->normalization       = dup_col(q, 6);
        r->dimensions          = sqlite3_column_int(q, 7);
        const unsigned char *sh = sqlite3_column_text(q, 8);
        char current[CHUTNI_HASH_STRLEN];
        r->compatible_with_artifact =
            sh && r->artifact_id &&
            artifact_payload_hash_of(s, r->artifact_id, current) == CHUTNI_OK &&
            !strcmp(current, (const char *)sh);
        n++;
    }
    sqlite3_finalize(q);
    *out = vec;
    *count = n;
    return CHUTNI_OK;
}

void chutni_representation_info_free(chutni_representation_info *reps, size_t count) {
    if (!reps) return;
    for (size_t i = 0; i < count; i++) {
        free(reps[i].representation_id);
        free(reps[i].artifact_id);
        free(reps[i].representation_kind);
        free(reps[i].model_id);
        free(reps[i].model_revision);
        free(reps[i].dtype);
        free(reps[i].normalization);
    }
    free(reps);
}

/* ------------------------------------------------------------------- search */

/* User input is treated as literal terms rather than FTS5 operator syntax: a
 * stray quote or AND in a question should not become a query error or silently
 * change the meaning of the search. */
static char *fts_query_escape(const char *query, int match_any) {
    size_t cap = strlen(query) * 4 + 8;
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
        if (wrote_term) {
            const char *joiner = match_any ? " OR " : " ";
            size_t joiner_len = strlen(joiner);
            memcpy(out + len, joiner, joiner_len);
            len += joiner_len;
        }
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

/* What a search result may honestly claim about a hit's freshness.
 *
 * The two hashes are both catalog state, so they agree with each other even
 * when the file has moved on and nothing has rescanned it — the mistake this
 * format exists to prevent. Re-hashing every hit is too expensive for search,
 * but a stat is not: when size or mtime no longer match what the scan recorded,
 * the catalog's "current" is unproven.
 *
 * Such a result is reported "unverified", not "stale". §13.2 forbids a quick
 * signal from establishing validity, and that cuts both ways here — touching a
 * file changes its mtime without changing its content, so a stat cannot prove
 * drift any more than it can prove currency. It may only withdraw a claim.
 * Settling the question needs a re-hash: chutni_check_freshness or verify. */
static const char *search_freshness(const char *status, const char *artifact_hash,
                                    const char *source_hash, const char *display_path,
                                    int64_t size_bytes, int64_t mtime_ns,
                                    const char *source_kind) {
    if (status && strcmp(status, "active")) return "stale";
    if (!artifact_hash || !source_hash) return "unknown";
    if (strcmp(artifact_hash, source_hash)) return "stale";
    if (source_kind && !strcmp(source_kind, "memory")) return "current";
    if (!display_path || !*display_path) return "current";

    struct stat st;
    if (stat(display_path, &st) != 0) return "missing";
    if ((int64_t)st.st_size != size_bytes || stat_mtime_ns(&st) != mtime_ns)
        return "unverified";
    return "current";
}

static int profile_matches_columns(const chutni_representation_profile *p,
                                   const char *kind, const char *model,
                                   const char *revision, int dimensions,
                                   const char *dtype, const char *normalization,
                                   const char *tokenizer_hash,
                                   const char *projector_hash) {
    return p && prof_eq(kind, p->representation_kind) &&
           prof_eq(model, p->model_id) &&
           prof_eq(revision, p->model_revision) &&
           dimensions == p->dimensions && prof_eq(dtype, p->dtype) &&
           prof_eq(normalization, p->normalization) &&
           prof_eq(tokenizer_hash, p->tokenizer_hash) &&
           prof_eq(projector_hash, p->projector_hash);
}

static int semantic_result_cmp(const void *a, const void *b) {
    const chutni_search_result *left = a;
    const chutni_search_result *right = b;
    if (left->score < right->score) return 1;
    if (left->score > right->score) return -1;
    if (!left->artifact_id || !right->artifact_id) return 0;
    return strcmp(left->artifact_id, right->artifact_id);
}

static void search_result_clear(chutni_search_result *r) {
    free(r->source_id);
    free(r->artifact_id);
    free(r->display_path);
    free(r->artifact_kind);
    free(r->snippet);
    free(r->producer_id);
    free(r->selector_json);
    free(r->freshness);
    free(r->score_type);
    free(r->source_kind);
    free(r->parent_source_id);
    free(r->coverage_manifest_id);
}

/* §19.3. A path and a snippet cannot tell a consumer whether the region the
   hit came from was exhaustively indexed or read one level deep, and a
   consumer that cannot tell will assume the former. Every result carries the
   coverage manifest governing its region so the question has an answer.

   One walk up the parent chain per result: result sets are small and bounded
   by `limit`, and caching a hierarchy that a concurrent writer may be changing
   is exactly the kind of trusted cached state this format exists to avoid. */
static void fill_hierarchy_fields(chutni_store *s, chutni_search_result *r) {
    r->depth = -1;
    if (!r->source_id || !*r->source_id) return;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(source_kind,'file'), parent_source_id,"
            " COALESCE(json_extract(metadata_json,'$.depth'),-1)"
            " FROM sources WHERE source_id=?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, r->source_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            r->source_kind      = dup_col(q, 0);
            r->parent_source_id = dup_col(q, 1);
            r->depth            = sqlite3_column_int(q, 2);
        }
        sqlite3_finalize(q);
    }
    char region[CHUTNI_ID_STRLEN] = "", manifest[CHUTNI_ID_STRLEN] = "";
    if (region_root_source(s, r->source_id, region)) {
        current_coverage_manifest(s, region, manifest);
        if (manifest[0]) r->coverage_manifest_id = strdup(manifest);
    }
}

chutni_status chutni_search_semantic(chutni_store *s,
                                     const chutni_semantic_request *req,
                                     chutni_search_result **out, size_t *count) {
    if (!s || !req || !out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;
    if (!req->vector || !req->dimensions || !req->profile ||
        !req->profile->representation_kind || !req->profile->model_id ||
        !req->profile->dtype || !req->profile->normalization ||
        req->profile->dimensions <= 0 ||
        (size_t)req->profile->dimensions != req->dimensions)
        return fail(s, CHUTNI_ERR_INVALID,
                    "semantic search needs a complete profile matching the query dimensions");
    if (strcmp(req->profile->dtype, "f32"))
        return fail(s, CHUTNI_ERR_INVALID, "v0.1 semantic search accepts f32 profiles only");

    double query_norm = 0.0;
    for (size_t i = 0; i < req->dimensions; i++) {
        if (!isfinite(req->vector[i]))
            return fail(s, CHUTNI_ERR_INVALID, "query vector contains a non-finite value");
        query_norm += (double)req->vector[i] * req->vector[i];
    }
    query_norm = sqrt(query_norm);
    if (!(query_norm > 0.0) || !isfinite(query_norm))
        return fail(s, CHUTNI_ERR_INVALID, "query vector must have a nonzero norm");

    int limit = req->limit > 0 ? req->limit : 20;
    const char *sql =
        "SELECT r.representation_id, r.representation_kind, r.model_id,"
        "       r.model_revision, r.dimensions, r.dtype, r.normalization,"
        "       r.tokenizer_hash, r.projector_hash, a.artifact_id, a.source_id,"
        "       a.artifact_kind, a.inline_text, a.object_hash, a.status,"
        "       a.source_content_hash, s.content_hash, s.size_bytes, s.mtime_ns,"
        "       json_extract(s.locator_json,'$.display_path'), d.producer_id,"
        "       a.selector_json, s.media_type, s.source_kind"
        " FROM representations r"
        " JOIN artifacts a ON a.artifact_id=r.artifact_id"
        " JOIN sources s ON s.source_id=a.source_id"
        " LEFT JOIN derivations d ON d.derivation_id=a.derivation_id"
        " ORDER BY r.created_at";
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));

    chutni_search_result *vec = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(q, 14);
        const char *kind = (const char *)sqlite3_column_text(q, 11);
        if ((!req->include_stale && status && strcmp(status, "active")) ||
            (kind && !str_in_list(kind, req->artifact_kinds))) continue;

        const char *stored_kind = (const char *)sqlite3_column_text(q, 1);
        const char *stored_model = (const char *)sqlite3_column_text(q, 2);
        const char *stored_revision = (const char *)sqlite3_column_text(q, 3);
        const char *stored_dtype = (const char *)sqlite3_column_text(q, 5);
        const char *stored_normalization = (const char *)sqlite3_column_text(q, 6);
        const char *stored_tokenizer = (const char *)sqlite3_column_text(q, 7);
        const char *stored_projector = (const char *)sqlite3_column_text(q, 8);
        if (!profile_matches_columns(req->profile, stored_kind, stored_model,
                                     stored_revision, sqlite3_column_int(q, 4),
                                     stored_dtype, stored_normalization,
                                     stored_tokenizer, stored_projector)) continue;

        const char *rid = (const char *)sqlite3_column_text(q, 0);
        float *candidate = NULL;
        size_t candidate_dims = 0;
        chutni_status st = chutni_representation_get(s, rid, req->profile,
                                                     &candidate, &candidate_dims);
        if (st != CHUTNI_OK) {
            /* A profile mismatch or a changed payload makes this representation
             * unusable for this query; other catalog/object errors are also
             * treated as an absent candidate so one bad row cannot corrupt the
             * rest of a search. */
            continue;
        }

        double candidate_norm = 0.0, dot = 0.0;
        int finite = 1;
        for (size_t i = 0; i < candidate_dims; i++) {
            if (!isfinite(candidate[i])) { finite = 0; break; }
            candidate_norm += (double)candidate[i] * candidate[i];
            dot += (double)req->vector[i] * candidate[i];
        }
        if (!finite || !(candidate_norm > 0.0) || !isfinite(candidate_norm)) {
            free(candidate);
            continue;
        }
        double score = dot / (query_norm * sqrt(candidate_norm));
        free(candidate);
        if (!isfinite(score)) continue;

        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 16;
            chutni_search_result *grown = realloc(vec, new_cap * sizeof *grown);
            if (!grown) {
                chutni_search_result_free(vec, n);
                sqlite3_finalize(q);
                return CHUTNI_ERR_NOMEM;
            }
            vec = grown;
            cap = new_cap;
        }
        chutni_search_result *r = &vec[n];
        memset(r, 0, sizeof *r);
        const unsigned char *v;
        v = sqlite3_column_text(q, 9);  r->artifact_id = strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 10); r->source_id = strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 19); r->display_path = strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 11); r->artifact_kind = strdup(v ? (const char *)v : "");
        v = sqlite3_column_text(q, 12); r->snippet = v ? strdup((const char *)v) : NULL;
        v = sqlite3_column_text(q, 20); r->producer_id = v ? strdup((const char *)v) : NULL;
        v = sqlite3_column_text(q, 21); r->selector_json = v ? strdup((const char *)v) : NULL;
        r->score = score;
        r->score_type = strdup("cosine_bruteforce");
        r->freshness = strdup(search_freshness(
            status,
            (const char *)sqlite3_column_text(q, 15),
            (const char *)sqlite3_column_text(q, 16),
            r->display_path,
            sqlite3_column_int64(q, 17),
            sqlite3_column_int64(q, 18),
            (const char *)sqlite3_column_text(q, 23)));
        fill_hierarchy_fields(s, r);
        n++;
    }
    sqlite3_finalize(q);
    qsort(vec, n, sizeof *vec, semantic_result_cmp);
    if ((int)n > limit) {
        for (size_t i = (size_t)limit; i < n; i++) search_result_clear(&vec[i]);
        chutni_search_result *shrunk = realloc(vec, (size_t)limit * sizeof *vec);
        if (shrunk) vec = shrunk;
        n = (size_t)limit;
    }
    *out = vec;
    *count = n;
    return CHUTNI_OK;
}

chutni_status chutni_search(chutni_store *s, const chutni_search_request *req,
                            chutni_search_result **out, size_t *count) {
    if (!s || !req || !out || !count) return CHUTNI_ERR_INVALID;
    *out = NULL;
    *count = 0;
    if (!req->query || !*req->query) return fail(s, CHUTNI_ERR_INVALID, "empty query");
    if (!s->have_index) return fail(s, CHUTNI_ERR_NOTFOUND, "no lexical index; run rebuild-indexes");

    char *fts = fts_query_escape(req->query, req->match_any);
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
        "       d.producer_id, s.size_bytes, s.mtime_ns, s.source_kind"
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

        r->freshness = strdup(search_freshness(
            status,
            (const char *)sqlite3_column_text(q, 7),
            (const char *)sqlite3_column_text(q, 9),
            r->display_path,
            sqlite3_column_int64(q, 12),
            sqlite3_column_int64(q, 13),
            (const char *)sqlite3_column_text(q, 14)));
        fill_hierarchy_fields(s, r);
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
    for (size_t i = 0; i < count; i++) search_result_clear(&results[i]);
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

/* ================================================================ chutni_call
 *
 * §20, plus representations and semantic search, as one JSON-in/JSON-out
 * entry point. See include/chutni.h for the contract and docs/API-JSON.md
 * for every operation's argument and result shape.
 *
 * Design rule that keeps this from becoming a second, drifting copy of the
 * typed API: every op_* function below is a thin JSON wrapper around a
 * function already defined earlier in this file. None of them touch SQL
 * directly except the two lookups (get_artifact, read_object's media-type
 * fetch) that have no typed equivalent to wrap, and even those reuse the
 * existing dup_col/bind_text_or_null helpers rather than inventing new ones.
 */

static const char *jarg_str(const cj *args, const char *key) {
    cj *v = cj_get(args, key);
    return v && v->type == CJ_STR ? v->str : NULL;
}
static int jarg_bool(const cj *args, const char *key, int def) {
    cj *v = cj_get(args, key);
    return v && v->type == CJ_BOOL ? v->bval : def;
}
static int jarg_int(const cj *args, const char *key, int def) {
    cj *v = cj_get(args, key);
    return v && v->type == CJ_NUM ? (int)v->num : def;
}

/* Standard base64 (RFC 4648, padded). Objects can be arbitrary binary, and
   JSON has no byte-string type; this is how read_object hands one back. */
static char *base64_encode(const unsigned char *data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i + 3 <= len) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[j++] = table[(n >> 18) & 0x3F];
        out[j++] = table[(n >> 12) & 0x3F];
        out[j++] = table[(n >> 6) & 0x3F];
        out[j++] = table[n & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[j++] = table[(n >> 18) & 0x3F];
        out[j++] = table[(n >> 12) & 0x3F];
        out[j++] = '=';
        out[j++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[j++] = table[(n >> 18) & 0x3F];
        out[j++] = table[(n >> 12) & 0x3F];
        out[j++] = table[(n >> 6) & 0x3F];
        out[j++] = '=';
    }
    out[j] = 0;
    return out;
}

static int jcall_resolve_source(chutni_store *s, const cj *args,
                                char out[CHUTNI_ID_STRLEN]) {
    const char *id = jarg_str(args, "source_id");
    const char *path = jarg_str(args, "source_path");
    if (id && *id) { snprintf(out, CHUTNI_ID_STRLEN, "%s", id); return 1; }
    if (path && *path && chutni_source_find(s, path, out) == CHUTNI_OK) return 1;
    return 0;
}

static cj *jcall_source_json(const chutni_source_info *src) {
    cj *item = cj_obj();
    cj_set(item, "source_id", cj_str(src->source_id ? src->source_id : ""));
    if (src->display_path) cj_set(item, "display_path", cj_str(src->display_path));
    cj_set(item, "source_kind", cj_str(src->source_kind ? src->source_kind : "file"));
    if (src->parent_source_id)
        cj_set(item, "parent_source_id", cj_str(src->parent_source_id));
    if (src->media_type) cj_set(item, "media_type", cj_str(src->media_type));
    if (src->content_hash) cj_set(item, "content_hash", cj_str(src->content_hash));
    if (src->state) cj_set(item, "state", cj_str(src->state));
    if (src->observation) cj_set(item, "observation", cj_str(src->observation));
    cj_set(item, "depth", src->depth < 0 ? cj_null() : cj_num((double)src->depth));
    cj_set(item, "size_bytes", cj_num((double)src->size_bytes));
    return item;
}

static void jcall_set_scan(cj *obj, const chutni_scan_result *r) {
    cj *v = cj_obj();
    cj_set(v, "files_seen", cj_num((double)r->files_seen));
    cj_set(v, "sources_indexed", cj_num((double)r->sources_indexed));
    cj_set(v, "unchanged", cj_num((double)r->unchanged));
    cj_set(v, "text_artifacts", cj_num((double)r->text_artifacts));
    cj_set(v, "metadata_artifacts", cj_num((double)r->metadata_artifacts));
    cj_set(v, "skipped", cj_num((double)r->skipped));
    cj_set(v, "errors", cj_num((double)r->errors));
    cj_set(v, "directories_observed", cj_num((double)r->directories_observed));
    cj_set(v, "directories_enumerated", cj_num((double)r->directories_enumerated));
    cj_set(v, "depth_limited_directories", cj_num((double)r->depth_limited_directories));
    cj_set(v, "listing_artifacts", cj_num((double)r->listing_artifacts));
    cj_set(v, "listings_reused", cj_num((double)r->listings_reused));
    cj_set(v, "files_hashed", cj_num((double)r->files_hashed));
    cj_set(v, "files_read", cj_num((double)r->files_read));
    cj_set(v, "excluded_sources", cj_num((double)r->excluded_sources));
    cj_set(v, "unsupported_sources", cj_num((double)r->unsupported_sources));
    cj_set(v, "sources_marked_missing", cj_num((double)r->sources_marked_missing));
    cj_set(v, "deepest_directory_enumerated", cj_num((double)r->deepest_directory_enumerated));
    cj_set(v, "complete_for_policy", cj_bool(r->complete_for_policy));
    if (r->depth_limited_directories)
        cj_set(v, "note",
               cj_str("complete_for_policy reports that the bounded operation "
                      "finished. Directories past max_depth were recorded by "
                      "name and never opened; do not treat this as an "
                      "exhaustive index of the subtree."));
    cj_set(obj, "scan", v);
}

static int jcall_set_counts(cj *obj, chutni_store *s) {
    chutni_counts c;
    if (chutni_store_counts(s, &c) != CHUTNI_OK) return 0;
    cj *v = cj_obj();
    cj_set(v, "roots", cj_num((double)c.roots));
    cj_set(v, "sources", cj_num((double)c.sources));
    cj_set(v, "sources_files", cj_num((double)c.sources_files));
    cj_set(v, "sources_directories", cj_num((double)c.sources_directories));
    cj_set(v, "sources_opaque_directories", cj_num((double)c.sources_opaque_directories));
    cj_set(v, "relations", cj_num((double)c.relations));
    cj_set(v, "artifacts", cj_num((double)c.artifacts));
    cj_set(v, "artifacts_active", cj_num((double)c.artifacts_active));
    cj_set(v, "artifacts_stale", cj_num((double)c.artifacts_stale));
    cj_set(v, "objects", cj_num((double)c.objects));
    cj_set(v, "producers", cj_num((double)c.producers));
    cj_set(v, "derivations", cj_num((double)c.derivations));
    cj_set(v, "content_artifacts", cj_num((double)c.content_artifacts));
    cj_set(v, "metadata_artifacts", cj_num((double)c.metadata_artifacts));
    cj_set(v, "content_readable_sources",
           cj_num((double)c.content_readable_sources));
    cj_set(v, "metadata_only_sources",
           cj_num((double)c.metadata_only_sources));
    cj_set(obj, "counts", v);
    return 1;
}

static cj *jcall_artifact_json(const chutni_artifact_info *a) {
    cj *item = cj_obj();
    cj_set(item, "artifact_id", cj_str(a->artifact_id ? a->artifact_id : ""));
    if (a->artifact_kind) cj_set(item, "artifact_kind", cj_str(a->artifact_kind));
    if (a->artifact_origin) cj_set(item, "artifact_origin", cj_str(a->artifact_origin));
    if (a->media_type) cj_set(item, "media_type", cj_str(a->media_type));
    if (a->status) cj_set(item, "status", cj_str(a->status));
    if (a->created_at) cj_set(item, "created_at", cj_str(a->created_at));
    if (a->source_content_hash)
        cj_set(item, "source_content_hash", cj_str(a->source_content_hash));
    if (a->language) cj_set(item, "language", cj_str(a->language));
    if (a->supersedes_artifact_id)
        cj_set(item, "supersedes_artifact_id", cj_str(a->supersedes_artifact_id));
    if (a->producer_name) cj_set(item, "producer_name", cj_str(a->producer_name));
    if (a->producer_kind) cj_set(item, "producer_kind", cj_str(a->producer_kind));
    if (a->model_id) cj_set(item, "model_id", cj_str(a->model_id));
    if (a->model_revision) cj_set(item, "model_revision", cj_str(a->model_revision));
    if (a->operation) cj_set(item, "operation", cj_str(a->operation));
    if (a->derivation_id) cj_set(item, "derivation_id", cj_str(a->derivation_id));
    return item;
}

/* ---------------------------------------------------------- store-less ops */

static chutni_status jcall_op_discover(const cj *args, cj **out) {
    (void)args;
    chutni_store_info *stores = NULL;
    size_t count = 0;
    chutni_status status = chutni_discover(&stores, &count);
    if (status != CHUTNI_OK) return fail(NULL, status, "store discovery failed");
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "count", cj_num((double)count));
    cj *items = cj_arr();
    for (size_t i = 0; i < count; i++) {
        cj *item = cj_obj();
        cj_set(item, "store_path", cj_str(stores[i].store_path));
        if (stores[i].store_id) cj_set(item, "store_id", cj_str(stores[i].store_id));
        if (stores[i].label) cj_set(item, "label", cj_str(stores[i].label));
        if (stores[i].spec_version)
            cj_set(item, "spec_version", cj_str(stores[i].spec_version));
        cj_set(item, "readable", cj_bool(stores[i].readable));
        cj_push(items, item);
    }
    cj_set(result, "stores", items);
    chutni_store_info_free(stores, count);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_capabilities(const cj *args, cj **out) {
    (void)args;
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "spec_version", cj_str(CHUTNI_SPEC_VERSION));
    cj_set(result, "library_version", cj_str(chutni_library_version()));

    cj *origins = cj_arr();
    static const char *origin_names[] = {
        "direct", "deterministic_transform", "model_generated", "human", NULL
    };
    for (const char **n = origin_names; *n; n++) cj_push(origins, cj_str(*n));
    cj_set(result, "artifact_origins", origins);

    cj *kinds = cj_arr();
    static const char *kind_names[] = {
        "file_metadata", "extracted_text", "page_text", "ocr_text",
        "transcript", "text_chunk", "summary_short", "summary_long",
        "image_caption", "document_title", "keywords", "entities",
        "table_schema", "sheet_summary", "archive_listing", "thumbnail",
        "language_detection", "content_warning", "processing_error",
        CHUTNI_KIND_DIRECTORY_LISTING, CHUTNI_KIND_SOURCE_DEFINITION,
        CHUTNI_KIND_COVERAGE_MANIFEST, CHUTNI_KIND_MEMORY, NULL
    };
    for (const char **n = kind_names; *n; n++) cj_push(kinds, cj_str(*n));
    cj_set(result, "core_artifact_kinds", kinds);

    cj *capabilities = cj_arr();
    static const char *capability_names[] = {
        "sources", "artifacts", "provenance", "hierarchical_sources",
        "bounded_coverage", "directory_definitions", "standalone_memory", NULL
    };
    for (const char **n = capability_names; *n; n++) cj_push(capabilities, cj_str(*n));
    cj_set(result, "capabilities", capabilities);

    cj *stop_reasons = cj_arr();
    static const char *stop_names[] = {
        CHUTNI_STOP_MAX_DEPTH, CHUTNI_STOP_COHERENT, CHUTNI_STOP_BUDGET,
        CHUTNI_STOP_EXCLUDED, CHUTNI_STOP_UNSUPPORTED, CHUTNI_STOP_UNREADABLE,
        CHUTNI_STOP_USER_CANCELED, NULL
    };
    for (const char **n = stop_names; *n; n++) cj_push(stop_reasons, cj_str(*n));
    cj_set(result, "definition_stop_reasons", stop_reasons);

    cj *modes = cj_arr();
    cj_push(modes, cj_str(CHUTNI_DEFINITION_ADAPTIVE));
    cj_push(modes, cj_str(CHUTNI_DEFINITION_PER_SOURCE));
    cj_set(result, "definition_modes", modes);

    cj *selectors = cj_arr();
    static const char *selector_names[] = {
        "pages", "sheet_range", "image_region", "time_range", "byte_range", NULL
    };
    for (const char **n = selector_names; *n; n++) cj_push(selectors, cj_str(*n));
    cj_set(result, "selector_types", selectors);

    cj_set(result, "semantic_validation", cj_str("not_performed"));
    cj_set(result, "writer_policy", cj_str("single_writer_many_readers"));
    *out = result;
    return CHUTNI_OK;
}

/* ---------------------------------------------------------------- store ops */

static chutni_status jcall_op_store_info(chutni_store *s, const cj *args, cj **out) {
    (void)args;
    chutni_root_info *roots = NULL;
    size_t root_count = 0;
    chutni_status status = chutni_roots_list(s, &roots, &root_count);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "store_path", cj_str(chutni_store_path(s)));
    cj_set(result, "store_id", cj_str(chutni_store_id(s)));
    cj_set(result, "spec_version", cj_str(CHUTNI_SPEC_VERSION));
    if (!jcall_set_counts(result, s)) {
        cj_free(result);
        chutni_root_info_free(roots, root_count);
        return fail(s, CHUTNI_ERR_DB, "cannot read store counts");
    }
    cj *root_json = cj_arr();
    for (size_t i = 0; i < root_count; i++) {
        cj *item = cj_obj();
        cj_set(item, "root_id", cj_str(roots[i].root_id));
        if (roots[i].path) cj_set(item, "path", cj_str(roots[i].path));
        if (roots[i].label) cj_set(item, "label", cj_str(roots[i].label));
        cj *policy = roots[i].policy_json ? cj_parse(roots[i].policy_json, NULL) : NULL;
        cj_set(item, "policy", policy ? policy : cj_null());
        cj_push(root_json, item);
    }
    cj_set(result, "roots", root_json);
    chutni_root_info_free(roots, root_count);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_add_root(chutni_store *s, const cj *args, cj **out) {
    const char *path = jarg_str(args, "path");
    if (!path || !*path) return fail(s, CHUTNI_ERR_INVALID, "path is required");
    chutni_root_policy policy;
    chutni_root_policy_defaults(&policy);
    cj *p = cj_get(args, "policy");
    if (p && p->type != CJ_OBJ)
        return fail(s, CHUTNI_ERR_INVALID, "policy must be a JSON object");
    if (p) {
        policy.recursive = jarg_bool(p, "recursive", policy.recursive);
        policy.follow_symlinks = jarg_bool(p, "follow_symlinks", policy.follow_symlinks);
        policy.include_hidden = jarg_bool(p, "include_hidden", policy.include_hidden);
        policy.retain_deleted_artifacts =
            jarg_bool(p, "retain_deleted_artifacts", policy.retain_deleted_artifacts);
        cj *n = cj_get(p, "max_file_size_bytes");
        if (n && n->type == CJ_NUM && n->num > 0) policy.max_file_size_bytes = (uint64_t)n->num;
        n = cj_get(p, "max_depth");
        if (n && n->type == CJ_NUM && n->num >= 0) policy.max_depth = (int)n->num;
        policy.memory_goal = jarg_str(p, "memory_goal");
        policy.definition_mode = jarg_str(p, "definition_mode");
    }
    char root_id[CHUTNI_ID_STRLEN];
    chutni_status status = chutni_root_add(s, path, jarg_str(args, "label"),
                                           &policy, root_id);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "root_id", cj_str(root_id));
    cj_set(result, "path", cj_str(path));
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_scan(chutni_store *s, const cj *args, cj **out) {
    chutni_scan_options options;
    memset(&options, 0, sizeof options);
    options.app_name = jarg_str(args, "app_name");
    options.app_version = jarg_str(args, "app_version");
    cj *max_value = cj_get(args, "max_file_size_bytes");
    if (max_value && max_value->type == CJ_NUM && max_value->num > 0)
        options.max_file_size_bytes = (uint64_t)max_value->num;
    cj *depth_value = cj_get(args, "max_depth");
    if (depth_value && depth_value->type == CJ_NUM && depth_value->num >= 0) {
        options.use_override_max_depth = 1;
        options.override_max_depth = (int)depth_value->num;
    }
    const char *root_id = jarg_str(args, "root_id");
    chutni_scan_result scan;
    chutni_status status = root_id && *root_id
                               ? chutni_scan_root(s, root_id, &options, &scan)
                               : chutni_scan(s, &options, &scan);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "store_path", cj_str(chutni_store_path(s)));
    jcall_set_scan(result, &scan);
    if (!jcall_set_counts(result, s)) {
        cj_free(result);
        return fail(s, CHUTNI_ERR_DB, "cannot read store counts");
    }
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_children(chutni_store *s, const cj *args, cj **out) {
    char source_id[CHUTNI_ID_STRLEN];
    if (!jcall_resolve_source(s, args, source_id))
        return fail(s, CHUTNI_ERR_INVALID,
                    "provide source_id or a source_path this store knows");
    chutni_source_info *children = NULL;
    size_t count = 0;
    chutni_status status = chutni_list_children(s, source_id, &children, &count);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "count", cj_num((double)count));
    cj *array = cj_arr();
    for (size_t i = 0; i < count; i++) cj_push(array, jcall_source_json(&children[i]));
    cj_set(result, "children", array);
    if (count == 0)
        cj_set(result, "note",
               cj_str("No children are recorded. This directory may never have "
                      "been enumerated; observe_directory opens exactly one "
                      "directory without recursing."));
    chutni_source_info_free(children, count);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_observe_directory(chutni_store *s, const cj *args, cj **out) {
    char source_id[CHUTNI_ID_STRLEN];
    if (!jcall_resolve_source(s, args, source_id))
        return fail(s, CHUTNI_ERR_INVALID,
                    "provide source_id or a source_path this store knows");
    chutni_scan_options options;
    memset(&options, 0, sizeof options);
    options.app_name = jarg_str(args, "app_name");
    options.app_version = jarg_str(args, "app_version");
    chutni_scan_result scan;
    chutni_status status = chutni_observe_directory(s, source_id, &options, &scan);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "source_id", cj_str(source_id));
    jcall_set_scan(result, &scan);
    jcall_set_counts(result, s);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_coverage(chutni_store *s, const cj *args, cj **out) {
    char target[CHUTNI_ID_STRLEN] = "";
    const char *root_id = jarg_str(args, "root_id");
    if (root_id && *root_id) snprintf(target, sizeof target, "%s", root_id);
    else if (!jcall_resolve_source(s, args, target)) {
        chutni_root_info *roots = NULL;
        size_t count = 0;
        chutni_roots_list(s, &roots, &count);
        if (count == 1 && roots[0].root_id)
            snprintf(target, sizeof target, "%s", roots[0].root_id);
        chutni_root_info_free(roots, count);
    }
    if (!target[0])
        return fail(s, CHUTNI_ERR_INVALID, "provide root_id, source_id, or source_path");

    char *json = NULL;
    chutni_status status = chutni_get_coverage(s, target, &json);
    if (status != CHUTNI_OK) return status;
    cj *coverage = cj_parse(json, NULL);
    chutni_free(json);
    if (!coverage) return fail(s, CHUTNI_ERR_NOMEM, "coverage could not be encoded");
    cj_set(coverage, "interpretation",
           cj_str("complete_for_policy means the requested bounded operation "
                  "finished. It does not mean the subtree was read. Directories "
                  "whose observation is \"opaque\" were named but never opened, "
                  "and nothing in this store describes their contents."));
    *out = coverage;
    return CHUTNI_OK;
}

static chutni_status jcall_op_search(chutni_store *s, const cj *args, cj **out) {
    const char *query = jarg_str(args, "query");
    if (!query || !*query)
        return fail(s, CHUTNI_ERR_INVALID, "a non-empty query is required");
    int limit = jarg_int(args, "limit", 10);
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;
    const char *kind = jarg_str(args, "kind");
    const char *kinds[2] = { kind, NULL };
    chutni_search_request request;
    memset(&request, 0, sizeof request);
    request.query = query;
    request.limit = limit;
    request.include_stale = jarg_bool(args, "include_stale", 0);
    request.match_any = jarg_bool(args, "match_any", 0);
    request.artifact_kinds = kind ? kinds : NULL;
    chutni_search_result *results = NULL;
    size_t count = 0;
    chutni_status status = chutni_search(s, &request, &results, &count);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "query", cj_str(query));
    cj_set(result, "count", cj_num((double)count));
    cj *items = cj_arr();
    for (size_t i = 0; i < count; i++) {
        cj *item = cj_obj();
        if (results[i].source_id) cj_set(item, "source_id", cj_str(results[i].source_id));
        if (results[i].artifact_id) cj_set(item, "artifact_id", cj_str(results[i].artifact_id));
        if (results[i].display_path) cj_set(item, "display_path", cj_str(results[i].display_path));
        if (results[i].artifact_kind) cj_set(item, "artifact_kind", cj_str(results[i].artifact_kind));
        if (results[i].snippet) cj_set(item, "snippet", cj_str(results[i].snippet));
        if (results[i].producer_id) cj_set(item, "producer_id", cj_str(results[i].producer_id));
        if (results[i].selector_json) {
            cj *selector = cj_parse(results[i].selector_json, NULL);
            if (selector) cj_set(item, "selector", selector);
            else cj_set(item, "selector_json", cj_str(results[i].selector_json));
        }
        if (results[i].freshness) cj_set(item, "freshness", cj_str(results[i].freshness));
        cj_set(item, "score", cj_num(results[i].score));
        if (results[i].score_type) cj_set(item, "score_type", cj_str(results[i].score_type));
        if (results[i].source_kind) cj_set(item, "source_kind", cj_str(results[i].source_kind));
        if (results[i].parent_source_id)
            cj_set(item, "parent_source_id", cj_str(results[i].parent_source_id));
        if (results[i].coverage_manifest_id)
            cj_set(item, "coverage_manifest_id", cj_str(results[i].coverage_manifest_id));
        cj_set(item, "depth", results[i].depth < 0 ? cj_null() : cj_num((double)results[i].depth));
        cj_push(items, item);
    }
    cj_set(result, "results", items);
    chutni_search_result_free(results, count);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_search_semantic(chutni_store *s, const cj *args, cj **out) {
    cj *vector_json = cj_get(args, "vector");
    if (!vector_json || vector_json->type != CJ_ARR || vector_json->n == 0)
        return fail(s, CHUTNI_ERR_INVALID, "vector must be a non-empty array of numbers");
    cj *profile_json = cj_get(args, "profile");
    if (!profile_json || profile_json->type != CJ_OBJ)
        return fail(s, CHUTNI_ERR_INVALID, "profile is required");

    size_t dims = vector_json->n;
    float *vector = malloc(dims * sizeof *vector);
    if (!vector) return CHUTNI_ERR_NOMEM;
    for (size_t i = 0; i < dims; i++) {
        cj *v = vector_json->items[i];
        if (!v || v->type != CJ_NUM) {
            free(vector);
            return fail(s, CHUTNI_ERR_INVALID,
                        "vector must contain numbers only");
        }
        vector[i] = (float)v->num;
    }

    chutni_representation_profile profile;
    memset(&profile, 0, sizeof profile);
    profile.representation_kind = jarg_str(profile_json, "representation_kind");
    profile.model_id = jarg_str(profile_json, "model_id");
    profile.model_revision = jarg_str(profile_json, "model_revision");
    profile.dimensions = jarg_int(profile_json, "dimensions", (int)dims);
    profile.dtype = jarg_str(profile_json, "dtype");
    profile.normalization = jarg_str(profile_json, "normalization");
    profile.tokenizer_hash = jarg_str(profile_json, "tokenizer_hash");
    profile.projector_hash = jarg_str(profile_json, "projector_hash");

    const char *kind = jarg_str(args, "kind");
    const char *kinds[2] = { kind, NULL };
    int limit = jarg_int(args, "limit", 10);
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;

    chutni_semantic_request request;
    memset(&request, 0, sizeof request);
    request.vector = vector;
    request.dimensions = dims;
    request.profile = &profile;
    request.artifact_kinds = kind ? kinds : NULL;
    request.limit = limit;
    request.include_stale = jarg_bool(args, "include_stale", 0);

    chutni_search_result *results = NULL;
    size_t count = 0;
    chutni_status status = chutni_search_semantic(s, &request, &results, &count);
    free(vector);
    if (status != CHUTNI_OK) return status;

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "count", cj_num((double)count));
    cj *items = cj_arr();
    for (size_t i = 0; i < count; i++) {
        cj *item = cj_obj();
        if (results[i].source_id) cj_set(item, "source_id", cj_str(results[i].source_id));
        if (results[i].artifact_id) cj_set(item, "artifact_id", cj_str(results[i].artifact_id));
        if (results[i].display_path) cj_set(item, "display_path", cj_str(results[i].display_path));
        if (results[i].artifact_kind) cj_set(item, "artifact_kind", cj_str(results[i].artifact_kind));
        if (results[i].snippet) cj_set(item, "snippet", cj_str(results[i].snippet));
        if (results[i].freshness) cj_set(item, "freshness", cj_str(results[i].freshness));
        cj_set(item, "score", cj_num(results[i].score));
        if (results[i].score_type) cj_set(item, "score_type", cj_str(results[i].score_type));
        cj_push(items, item);
    }
    cj_set(result, "results", items);
    chutni_search_result_free(results, count);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_get_source(chutni_store *s, const cj *args, cj **out) {
    char source_id[CHUTNI_ID_STRLEN];
    if (!jcall_resolve_source(s, args, source_id))
        return fail(s, CHUTNI_ERR_INVALID,
                    "provide source_id or a source_path this store knows");
    chutni_source_info *sources = NULL;
    size_t count = 0;
    chutni_status status = chutni_sources_list(s, NULL, &sources, &count);
    if (status != CHUTNI_OK) return status;
    cj *result = NULL;
    for (size_t i = 0; i < count; i++)
        if (sources[i].source_id && !strcmp(sources[i].source_id, source_id)) {
            result = jcall_source_json(&sources[i]);
            break;
        }
    chutni_source_info_free(sources, count);
    if (!result) return fail(s, CHUTNI_ERR_NOTFOUND, "no source with id %s", source_id);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_get_artifact(chutni_store *s, const cj *args, cj **out) {
    const char *artifact_id = jarg_str(args, "artifact_id");
    if (!artifact_id || !*artifact_id)
        return fail(s, CHUTNI_ERR_INVALID, "artifact_id is required");
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT a.artifact_id, a.artifact_kind, a.artifact_origin, a.media_type, a.status,"
            "       a.created_at, a.source_content_hash, a.language, a.supersedes_artifact_id,"
            "       p.name, p.producer_kind, p.model_id, p.model_revision,"
            "       d.operation, d.derivation_id, a.source_id"
            " FROM artifacts a"
            " LEFT JOIN derivations d ON d.derivation_id=a.derivation_id"
            " LEFT JOIN producers p ON p.producer_id=d.producer_id"
            " WHERE a.artifact_id=?1", -1, &q, NULL) != SQLITE_OK)
        return fail(s, CHUTNI_ERR_DB, "%s", sqlite3_errmsg(s->db));
    sqlite3_bind_text(q, 1, artifact_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return fail(s, CHUTNI_ERR_NOTFOUND, "no artifact with id %s", artifact_id);
    }
    cj *result = cj_obj();
    char *v;
    v = dup_col(q, 0); cj_set(result, "artifact_id", cj_str(v ? v : "")); free(v);
    v = dup_col(q, 1); if (v) { cj_set(result, "artifact_kind", cj_str(v)); free(v); }
    v = dup_col(q, 2); if (v) { cj_set(result, "artifact_origin", cj_str(v)); free(v); }
    v = dup_col(q, 3); if (v) { cj_set(result, "media_type", cj_str(v)); free(v); }
    v = dup_col(q, 4); if (v) { cj_set(result, "status", cj_str(v)); free(v); }
    v = dup_col(q, 5); if (v) { cj_set(result, "created_at", cj_str(v)); free(v); }
    v = dup_col(q, 6); if (v) { cj_set(result, "source_content_hash", cj_str(v)); free(v); }
    v = dup_col(q, 7); if (v) { cj_set(result, "language", cj_str(v)); free(v); }
    v = dup_col(q, 8); if (v) { cj_set(result, "supersedes_artifact_id", cj_str(v)); free(v); }
    v = dup_col(q, 9); if (v) { cj_set(result, "producer_name", cj_str(v)); free(v); }
    v = dup_col(q, 10); if (v) { cj_set(result, "producer_kind", cj_str(v)); free(v); }
    v = dup_col(q, 11); if (v) { cj_set(result, "model_id", cj_str(v)); free(v); }
    v = dup_col(q, 12); if (v) { cj_set(result, "model_revision", cj_str(v)); free(v); }
    v = dup_col(q, 13); if (v) { cj_set(result, "operation", cj_str(v)); free(v); }
    v = dup_col(q, 14); if (v) { cj_set(result, "derivation_id", cj_str(v)); free(v); }
    v = dup_col(q, 15); cj_set(result, "source_id", cj_str(v ? v : "")); free(v);
    sqlite3_finalize(q);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_read_object(chutni_store *s, const cj *args, cj **out) {
    const char *hash = jarg_str(args, "object_hash");
    if (!hash || !*hash)
        return fail(s, CHUTNI_ERR_INVALID, "object_hash is required");
    char media_type[160] = "";
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT COALESCE(media_type,''), size_bytes FROM objects WHERE object_hash=?1",
            -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, hash, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW)
            snprintf(media_type, sizeof media_type, "%s",
                     (const char *)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    void *data = NULL;
    size_t len = 0;
    chutni_status status = chutni_object_get(s, hash, &data, &len);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "object_hash", cj_str(hash));
    if (media_type[0]) cj_set(result, "media_type", cj_str(media_type));
    cj_set(result, "size_bytes", cj_num((double)len));
    if (!strncmp(media_type, "text/", 5) || !strcmp(media_type, "application/json")) {
        char *text = malloc(len + 1);
        if (text) {
            memcpy(text, data, len);
            text[len] = 0;
            cj_set(result, "text", cj_str(text));
            free(text);
        }
    } else {
        char *b64 = base64_encode(data, len);
        if (b64) { cj_set(result, "data_base64", cj_str(b64)); free(b64); }
    }
    free(data);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_check_freshness(chutni_store *s, const cj *args, cj **out) {
    const char *id = jarg_str(args, "source_id");
    if (!id || !*id) id = jarg_str(args, "artifact_id");
    if (!id || !*id) {
        char resolved[CHUTNI_ID_STRLEN];
        if (jcall_resolve_source(s, args, resolved)) id = resolved;
    }
    char id_copy[CHUTNI_ID_STRLEN] = "";
    if (id) snprintf(id_copy, sizeof id_copy, "%s", id);
    if (!id_copy[0])
        return fail(s, CHUTNI_ERR_INVALID,
                    "provide source_id, artifact_id, or source_path");
    const char *freshness = NULL;
    chutni_status status = chutni_check_freshness(s, id_copy, &freshness);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "id", cj_str(id_copy));
    cj_set(result, "freshness", cj_str(freshness ? freshness : "unknown"));
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_list_artifacts(chutni_store *s, const cj *args, cj **out) {
    char source_id[CHUTNI_ID_STRLEN];
    if (!jcall_resolve_source(s, args, source_id))
        return fail(s, CHUTNI_ERR_INVALID,
                    "provide source_id or a source_path this store knows");
    int include_stale = jarg_bool(args, "include_stale", 0);
    chutni_artifact_info *artifacts = NULL;
    size_t count = 0;
    chutni_status status = chutni_list_artifacts(s, source_id, &artifacts, &count);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "source_id", cj_str(source_id));
    cj *items = cj_arr();
    size_t returned = 0;
    for (size_t i = 0; i < count; i++) {
        if (!include_stale && (!artifacts[i].status || strcmp(artifacts[i].status, "active")))
            continue;
        cj_push(items, jcall_artifact_json(&artifacts[i]));
        returned++;
    }
    cj_set(result, "count", cj_num((double)returned));
    cj_set(result, "artifacts", items);
    chutni_artifact_info_free(artifacts, count);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_add_source(chutni_store *s, const cj *args, cj **out) {
    const char *root_id = jarg_str(args, "root_id");
    const char *path = jarg_str(args, "path");
    if (!root_id || !*root_id || !path || !*path)
        return fail(s, CHUTNI_ERR_INVALID, "root_id and path are required");
    int hash_file = jarg_bool(args, "hash_file", 1);
    char source_id[CHUTNI_ID_STRLEN];
    int changed = 0;
    chutni_status status = chutni_source_put(s, root_id, path, hash_file, source_id, &changed);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "changed", cj_bool(changed));
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_mark_source_missing(chutni_store *s, const cj *args, cj **out) {
    char source_id[CHUTNI_ID_STRLEN];
    if (!jcall_resolve_source(s, args, source_id))
        return fail(s, CHUTNI_ERR_INVALID,
                    "provide source_id or a source_path this store knows");
    chutni_status status = chutni_source_set_state(s, source_id, CHUTNI_SOURCE_MISSING);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "state", cj_str("missing"));
    *out = result;
    return CHUTNI_OK;
}

static int jcall_forget_mode(const char *name, chutni_forget_mode *out) {
    if (!name || !strcmp(name, "catalog_only")) { *out = CHUTNI_FORGET_CATALOG_ONLY; return 1; }
    if (!strcmp(name, "artifacts")) { *out = CHUTNI_FORGET_ARTIFACTS; return 1; }
    if (!strcmp(name, "secure_logical_delete")) { *out = CHUTNI_FORGET_SECURE_LOGICAL_DELETE; return 1; }
    if (!strcmp(name, "purge")) { *out = CHUTNI_FORGET_PURGE; return 1; }
    return 0;
}

static chutni_status jcall_op_forget_source(chutni_store *s, const cj *args, cj **out) {
    char source_id[CHUTNI_ID_STRLEN];
    if (!jcall_resolve_source(s, args, source_id))
        return fail(s, CHUTNI_ERR_INVALID,
                    "provide source_id or a source_path this store knows");
    chutni_forget_mode mode;
    if (!jcall_forget_mode(jarg_str(args, "mode"), &mode))
        return fail(s, CHUTNI_ERR_INVALID,
                    "mode must be catalog_only, artifacts, secure_logical_delete, or purge");
    chutni_status status = chutni_forget_source(s, source_id, mode);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "forgotten", cj_bool(1));
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_rebuild_indexes(chutni_store *s, const cj *args, cj **out) {
    (void)args;
    chutni_status status = chutni_rebuild_indexes(s);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    *out = result;
    return CHUTNI_OK;
}

typedef struct {
    char source_id[CHUTNI_ID_STRLEN];
    char display_path[PATH_MAX];
    char content_hash[CHUTNI_HASH_STRLEN];
    char media_type[160];
    char state[32];
    int64_t size_bytes;
} jcall_source_snapshot;

static chutni_status jcall_source_snapshot_load(chutni_store *s, const cj *args,
                                                jcall_source_snapshot *snap) {
    memset(snap, 0, sizeof *snap);
    if (!jcall_resolve_source(s, args, snap->source_id)) return CHUTNI_ERR_INVALID;
    chutni_source_info *sources = NULL;
    size_t count = 0;
    chutni_status status = chutni_sources_list(s, NULL, &sources, &count);
    if (status != CHUTNI_OK) return status;
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        if (!sources[i].source_id || strcmp(sources[i].source_id, snap->source_id)) continue;
        if (sources[i].display_path)
            snprintf(snap->display_path, sizeof snap->display_path, "%s", sources[i].display_path);
        if (sources[i].content_hash)
            snprintf(snap->content_hash, sizeof snap->content_hash, "%s", sources[i].content_hash);
        if (sources[i].media_type)
            snprintf(snap->media_type, sizeof snap->media_type, "%s", sources[i].media_type);
        if (sources[i].state)
            snprintf(snap->state, sizeof snap->state, "%s", sources[i].state);
        snap->size_bytes = sources[i].size_bytes;
        found = 1;
        break;
    }
    chutni_source_info_free(sources, count);
    return found ? CHUTNI_OK : CHUTNI_ERR_NOTFOUND;
}

/* Generic host-ingestion primitive, matching chutni_artifacts_put closely:
   each artifact in the array carries its own source_id and
   source_content_hash, exactly as the typed struct requires (§13.3). This is
   deliberately lower-level than chutni-mcp's chutni_put_artifacts tool, which
   layers a single-source convenience (resolve-by-path, verify-then-write) on
   top of it — that convenience belongs in the host, not in this primitive. */
static chutni_status jcall_op_put_artifacts(chutni_store *s, const cj *args, cj **out) {
    const char *operation = jarg_str(args, "operation");
    cj *producer_json = cj_get(args, "producer");
    cj *artifact_json = cj_get(args, "artifacts");
    if (!operation || !*operation || !producer_json || producer_json->type != CJ_OBJ ||
        !artifact_json || artifact_json->type != CJ_ARR || artifact_json->n == 0 ||
        artifact_json->n > 128)
        return fail(s, CHUTNI_ERR_INVALID,
                    "operation, producer, and 1-128 artifacts are required");

    chutni_producer producer;
    memset(&producer, 0, sizeof producer);
    producer.producer_kind = jarg_str(producer_json, "producer_kind");
    producer.name = jarg_str(producer_json, "name");
    producer.version = jarg_str(producer_json, "version");
    producer.model_id = jarg_str(producer_json, "model_id");
    producer.model_revision = jarg_str(producer_json, "model_revision");
    producer.weights_hash = jarg_str(producer_json, "weights_hash");
    producer.quantization = jarg_str(producer_json, "quantization");
    producer.runtime = jarg_str(producer_json, "runtime");
    producer.app_name = jarg_str(producer_json, "app_name");
    producer.app_version = jarg_str(producer_json, "app_version");
    cj *details = cj_get(producer_json, "details");
    char *details_json = details ? cj_dump(details, -1) : NULL;
    producer.details_json = details_json;

    cj *parameters = cj_get(args, "parameters");
    char *parameters_json = parameters ? cj_dump(parameters, -1) : strdup("{}");
    cj *inputs = cj_get(args, "inputs");
    char *inputs_json = inputs ? cj_dump(inputs, -1) : strdup("[]");

    size_t n = artifact_json->n;
    chutni_artifact *artifacts = calloc(n, sizeof *artifacts);
    char (*artifact_ids)[CHUTNI_ID_STRLEN] = calloc(n, sizeof *artifact_ids);
    char **selector_texts = calloc(n, sizeof *selector_texts);
    char **metadata_texts = calloc(n, sizeof *metadata_texts);
    int valid = artifacts && artifact_ids && selector_texts && metadata_texts &&
               parameters_json && inputs_json;
    for (size_t i = 0; valid && i < n; i++) {
        cj *item = artifact_json->items[i];
        const char *source_id = jarg_str(item, "source_id");
        const char *hash = jarg_str(item, "source_content_hash");
        const char *text = jarg_str(item, "text");
        const char *kind = jarg_str(item, "artifact_kind");
        const char *origin = jarg_str(item, "artifact_origin");
        if (!item || item->type != CJ_OBJ || !source_id || !hash || !text || !kind || !origin) {
            valid = 0;
            break;
        }
        cj *selector = cj_get(item, "selector");
        cj *metadata = cj_get(item, "metadata");
        if (selector) selector_texts[i] = cj_dump(selector, -1);
        if (metadata) metadata_texts[i] = cj_dump(metadata, -1);
        artifacts[i].source_id = source_id;
        artifacts[i].artifact_kind = kind;
        artifacts[i].artifact_origin = origin;
        artifacts[i].media_type = jarg_str(item, "media_type")
                                      ? jarg_str(item, "media_type")
                                      : "text/plain; charset=utf-8";
        artifacts[i].inline_text = text;
        artifacts[i].selector_json = selector_texts[i];
        artifacts[i].language = jarg_str(item, "language");
        artifacts[i].source_content_hash = hash;
        artifacts[i].supersedes_artifact_id = jarg_str(item, "supersedes_artifact_id");
        artifacts[i].metadata_json = metadata_texts[i];
    }

    char producer_id[CHUTNI_ID_STRLEN] = "", derivation_id[CHUTNI_ID_STRLEN] = "";
    chutni_status status = CHUTNI_ERR_INVALID;
    if (valid)
        status = chutni_artifacts_put(s, &producer, operation, jarg_str(args, "recipe_hash"),
                                      parameters_json, inputs_json, artifacts, n,
                                      producer_id, derivation_id, artifact_ids);

    for (size_t i = 0; i < n; i++) {
        free(selector_texts ? selector_texts[i] : NULL);
        free(metadata_texts ? metadata_texts[i] : NULL);
    }
    free(selector_texts);
    free(metadata_texts);
    free(artifacts);
    free(details_json);
    free(parameters_json);
    free(inputs_json);

    if (status != CHUTNI_OK) {
        free(artifact_ids);
        if (!valid)
            return fail(s, CHUTNI_ERR_INVALID,
                       "each artifact requires source_id, source_content_hash, text, "
                       "artifact_kind, and artifact_origin");
        return status;
    }

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "producer_id", cj_str(producer_id));
    cj_set(result, "derivation_id", cj_str(derivation_id));
    cj_set(result, "semantic_validation", cj_str("not_performed"));
    cj *ids = cj_arr();
    for (size_t i = 0; i < n; i++) {
        cj *item = cj_obj();
        cj_set(item, "artifact_id", cj_str(artifact_ids[i]));
        cj_set(item, "artifact_kind",
               cj_str(artifact_json->items[i]->type == CJ_OBJ
                          ? jarg_str(artifact_json->items[i], "artifact_kind")
                          : ""));
        cj_push(ids, item);
    }
    cj_set(result, "artifacts", ids);
    free(artifact_ids);
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_put_memory(chutni_store *s, const cj *args,
                                         cj **out) {
    const char *memory_kind = jarg_str(args, "memory_kind");
    const char *text = jarg_str(args, "text");
    const char *operation = jarg_str(args, "operation");
    cj *producer_json = cj_get(args, "producer");
    if (!memory_kind || !*memory_kind || !text || !*text ||
        !operation || !*operation || !producer_json ||
        producer_json->type != CJ_OBJ)
        return fail(s, CHUTNI_ERR_INVALID,
                    "memory_kind, text, producer, and operation are required");

    chutni_producer producer;
    memset(&producer, 0, sizeof producer);
    producer.producer_kind = jarg_str(producer_json, "producer_kind");
    producer.name = jarg_str(producer_json, "name");
    producer.version = jarg_str(producer_json, "version");
    producer.model_id = jarg_str(producer_json, "model_id");
    producer.model_revision = jarg_str(producer_json, "model_revision");
    producer.weights_hash = jarg_str(producer_json, "weights_hash");
    producer.quantization = jarg_str(producer_json, "quantization");
    producer.runtime = jarg_str(producer_json, "runtime");
    producer.app_name = jarg_str(producer_json, "app_name");
    producer.app_version = jarg_str(producer_json, "app_version");
    cj *details = cj_get(producer_json, "details");
    char *details_json = details ? cj_dump(details, -1) : NULL;
    producer.details_json = details_json;

    cj *parameters = cj_get(args, "parameters");
    char *parameters_json = parameters ? cj_dump(parameters, -1) : strdup("{}");
    cj *inputs = cj_get(args, "inputs");
    char *inputs_json = inputs ? cj_dump(inputs, -1) : strdup("[]");
    if (!parameters_json || !inputs_json || (details && !details_json)) {
        free(details_json);
        free(parameters_json);
        free(inputs_json);
        return CHUTNI_ERR_NOMEM;
    }

    chutni_memory memory;
    memset(&memory, 0, sizeof memory);
    memory.memory_kind = memory_kind;
    memory.title = jarg_str(args, "title");
    memory.scope = jarg_str(args, "scope");
    memory.text = text;
    memory.language = jarg_str(args, "language");
    memory.producer = &producer;
    memory.operation = operation;
    memory.recipe_hash = jarg_str(args, "recipe_hash");
    memory.parameters_json = parameters_json;
    memory.input_refs_json = inputs_json;

    char source_id[CHUTNI_ID_STRLEN] = "";
    char artifact_id[CHUTNI_ID_STRLEN] = "";
    char producer_id[CHUTNI_ID_STRLEN] = "";
    char derivation_id[CHUTNI_ID_STRLEN] = "";
    chutni_status status =
        chutni_memory_put(s, &memory, source_id, artifact_id, producer_id,
                          derivation_id);
    free(details_json);
    free(parameters_json);
    free(inputs_json);
    if (status != CHUTNI_OK) return status;

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "memory_id", cj_str(source_id));
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "artifact_id", cj_str(artifact_id));
    cj_set(result, "producer_id", cj_str(producer_id));
    cj_set(result, "derivation_id", cj_str(derivation_id));
    cj_set(result, "memory_kind", cj_str(memory_kind));
    cj_set(result, "semantic_validation", cj_str("not_performed"));
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_put_representation(chutni_store *s, const cj *args, cj **out) {
    const char *artifact_id = jarg_str(args, "artifact_id");
    cj *profile_json = cj_get(args, "profile");
    cj *vector_json = cj_get(args, "vector");
    if (!artifact_id || !*artifact_id || !profile_json || profile_json->type != CJ_OBJ ||
        !vector_json || vector_json->type != CJ_ARR || vector_json->n == 0)
        return fail(s, CHUTNI_ERR_INVALID,
                    "artifact_id, profile, and a non-empty vector are required");

    size_t dims = vector_json->n;
    float *vector = malloc(dims * sizeof *vector);
    if (!vector) return CHUTNI_ERR_NOMEM;
    for (size_t i = 0; i < dims; i++) {
        cj *v = vector_json->items[i];
        if (!v || v->type != CJ_NUM) {
            free(vector);
            return fail(s, CHUTNI_ERR_INVALID,
                        "vector must contain numbers only");
        }
        vector[i] = (float)v->num;
    }

    chutni_representation_profile profile;
    memset(&profile, 0, sizeof profile);
    profile.representation_kind = jarg_str(profile_json, "representation_kind");
    profile.model_id = jarg_str(profile_json, "model_id");
    profile.model_revision = jarg_str(profile_json, "model_revision");
    profile.dimensions = jarg_int(profile_json, "dimensions", (int)dims);
    profile.dtype = jarg_str(profile_json, "dtype");
    profile.normalization = jarg_str(profile_json, "normalization");
    profile.tokenizer_hash = jarg_str(profile_json, "tokenizer_hash");
    profile.projector_hash = jarg_str(profile_json, "projector_hash");

    char representation_id[CHUTNI_ID_STRLEN];
    chutni_status status =
        chutni_representation_put(s, artifact_id, &profile, vector, dims, representation_id);
    free(vector);
    if (status != CHUTNI_OK) return status;
    cj *result = cj_obj();
    cj_set(result, "representation_id", cj_str(representation_id));
    cj_set(result, "artifact_id", cj_str(artifact_id));
    cj_set(result, "dimensions", cj_num((double)dims));
    *out = result;
    return CHUTNI_OK;
}

/* chutni-mcp's convenience wrapper for a single model-generated artifact:
   resolves a source by filesystem path, refreses it, and requires the result
   to be exactly "current" before writing — a stricter, simpler precondition
   than put_artifacts' per-artifact hash check, appropriate for a one-shot
   call that has no earlier snapshot to compare against. */
static chutni_status jcall_op_put_model_artifact(chutni_store *s, const cj *args, cj **out) {
    const char *source_path = jarg_str(args, "source_path");
    const char *text = jarg_str(args, "text");
    const char *model_id = jarg_str(args, "model_id");
    const char *model_revision = jarg_str(args, "model_revision");
    const char *app_name = jarg_str(args, "app_name");
    const char *app_version = jarg_str(args, "app_version");
    if (!source_path || !text || !model_id || !model_revision || !app_name || !app_version)
        return fail(s, CHUTNI_ERR_INVALID,
                    "source_path, text, model_id, model_revision, app_name, and "
                    "app_version are required");

    char source_id[CHUTNI_ID_STRLEN];
    chutni_status status = chutni_source_find(s, source_path, source_id);
    if (status != CHUTNI_OK)
        return fail(s, status, "the source must already be indexed in this store");
    const char *freshness = NULL;
    status = chutni_source_refresh(s, source_id, &freshness);
    if (status != CHUTNI_OK || !freshness || strcmp(freshness, "current"))
        return fail(s, CHUTNI_ERR_DENIED,
                    "the source is missing or changed; scan it before storing model output");
    char source_hash[CHUTNI_HASH_STRLEN];
    status = chutni_hash_file(source_path, source_hash);
    if (status != CHUTNI_OK) return status;

    chutni_producer producer;
    memset(&producer, 0, sizeof producer);
    producer.producer_kind = "model";
    producer.name = jarg_str(args, "producer_name") ? jarg_str(args, "producer_name") : model_id;
    producer.model_id = model_id;
    producer.model_revision = model_revision;
    producer.weights_hash = jarg_str(args, "weights_hash");
    producer.quantization = jarg_str(args, "quantization");
    producer.runtime = jarg_str(args, "runtime");
    producer.app_name = app_name;
    producer.app_version = app_version;
    char producer_id[CHUTNI_ID_STRLEN];
    status = chutni_producer_put(s, &producer, producer_id);
    if (status != CHUTNI_OK) return status;

    cj *input_array = cj_arr();
    cj *input = cj_obj();
    cj_set(input, "source_id", cj_str(source_id));
    cj_set(input, "source_content_hash", cj_str(source_hash));
    cj_push(input_array, input);
    char *input_refs = cj_dump(input_array, -1);
    cj_free(input_array);
    cj *parameters = cj_get(args, "parameters");
    char *parameters_text = parameters ? cj_dump(parameters, -1) : strdup("{}");
    if (!input_refs || !parameters_text) {
        free(input_refs);
        free(parameters_text);
        return CHUTNI_ERR_NOMEM;
    }
    char derivation_id[CHUTNI_ID_STRLEN];
    status = chutni_derivation_put(
        s, producer_id,
        jarg_str(args, "operation") ? jarg_str(args, "operation") : "generate_artifact",
        jarg_str(args, "recipe_hash"), parameters_text, input_refs, derivation_id);
    free(parameters_text);
    free(input_refs);
    if (status != CHUTNI_OK) return status;

    cj *selector = cj_get(args, "selector");
    char *selector_text = selector ? cj_dump(selector, -1) : NULL;
    chutni_artifact artifact;
    memset(&artifact, 0, sizeof artifact);
    artifact.source_id = source_id;
    artifact.artifact_kind =
        jarg_str(args, "artifact_kind") ? jarg_str(args, "artifact_kind") : "summary_short";
    artifact.artifact_origin = "model_generated";
    artifact.media_type = "text/plain; charset=utf-8";
    artifact.inline_text = text;
    artifact.selector_json = selector_text;
    artifact.source_content_hash = source_hash;
    artifact.derivation_id = derivation_id;
    artifact.supersedes_artifact_id = jarg_str(args, "supersedes_artifact_id");
    char artifact_id[CHUTNI_ID_STRLEN];
    status = chutni_artifact_put(s, &artifact, artifact_id);
    free(selector_text);
    if (status == CHUTNI_OK) status = chutni_rebuild_indexes(s);
    if (status != CHUTNI_OK) return status;

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj_set(result, "source_id", cj_str(source_id));
    cj_set(result, "artifact_id", cj_str(artifact_id));
    cj_set(result, "producer_id", cj_str(producer_id));
    cj_set(result, "derivation_id", cj_str(derivation_id));
    cj_set(result, "semantic_validation", cj_str("not_performed"));
    *out = result;
    return CHUTNI_OK;
}

static chutni_status jcall_op_source_context(chutni_store *s, const cj *args, cj **out) {
    jcall_source_snapshot source;
    chutni_status status = jcall_source_snapshot_load(s, args, &source);
    if (status != CHUTNI_OK) return status;
    chutni_artifact_info *artifacts = NULL;
    size_t artifact_count = 0;
    status = chutni_list_artifacts(s, source.source_id, &artifacts, &artifact_count);
    if (status != CHUTNI_OK) return status;

    int include_stale = jarg_bool(args, "include_stale", 0);
    int max_text_chars = jarg_int(args, "max_text_chars", 32768);
    if (max_text_chars < 0) max_text_chars = 0;
    if (max_text_chars > 262144) max_text_chars = 262144;

    cj *result = cj_obj();
    cj_set(result, "ok", cj_bool(1));
    cj *source_json = cj_obj();
    cj_set(source_json, "source_id", cj_str(source.source_id));
    cj_set(source_json, "display_path", cj_str(source.display_path));
    if (source.media_type[0]) cj_set(source_json, "media_type", cj_str(source.media_type));
    if (source.content_hash[0]) cj_set(source_json, "content_hash", cj_str(source.content_hash));
    if (source.state[0]) cj_set(source_json, "state", cj_str(source.state));
    cj_set(source_json, "size_bytes", cj_num((double)source.size_bytes));
    const char *source_freshness = NULL;
    if (chutni_check_freshness(s, source.source_id, &source_freshness) == CHUTNI_OK &&
        source_freshness)
        cj_set(source_json, "freshness", cj_str(source_freshness));
    cj_set(result, "source", source_json);

    cj *items = cj_arr();
    size_t returned = 0;
    for (size_t i = 0; i < artifact_count; i++) {
        if (!include_stale && (!artifacts[i].status || strcmp(artifacts[i].status, "active")))
            continue;
        cj *item = jcall_artifact_json(&artifacts[i]);
        if (artifacts[i].selector_json) {
            cj *sel = cj_parse(artifacts[i].selector_json, NULL);
            cj_set(item, "selector", sel ? sel : cj_str(artifacts[i].selector_json));
        }
        if (artifacts[i].metadata_json) {
            cj *meta = cj_parse(artifacts[i].metadata_json, NULL);
            cj_set(item, "metadata", meta ? meta : cj_str(artifacts[i].metadata_json));
        }
        const char *freshness = NULL;
        if (chutni_check_freshness(s, artifacts[i].artifact_id, &freshness) == CHUTNI_OK &&
            freshness)
            cj_set(item, "freshness", cj_str(freshness));
        cj_set(item, "semantic_validation", cj_str("not_performed"));

        const char *content = artifacts[i].inline_text;
        void *loaded = NULL;
        size_t content_length = content ? strlen(content) : 0;
        if (!content && artifacts[i].object_hash && artifacts[i].media_type &&
            !strncmp(artifacts[i].media_type, "text/", 5)) {
            if (chutni_object_get(s, artifacts[i].object_hash, &loaded, &content_length) == CHUTNI_OK)
                content = loaded;
        }
        if (content && max_text_chars > 0) {
            size_t shown = content_length;
            if (shown > (size_t)max_text_chars) shown = (size_t)max_text_chars;
            char *bounded = malloc(shown + 1);
            if (bounded) {
                memcpy(bounded, content, shown);
                bounded[shown] = 0;
                cj_set(item, "content", cj_str(bounded));
                cj_set(item, "content_truncated", cj_bool(shown < content_length));
                free(bounded);
            }
        }
        free(loaded);

        cj *provenance = cj_obj();
        cj *producer = cj_obj();
        if (artifacts[i].producer_id) cj_set(producer, "producer_id", cj_str(artifacts[i].producer_id));
        if (artifacts[i].producer_version)
            cj_set(producer, "version", cj_str(artifacts[i].producer_version));
        if (artifacts[i].weights_hash) cj_set(producer, "weights_hash", cj_str(artifacts[i].weights_hash));
        if (artifacts[i].quantization) cj_set(producer, "quantization", cj_str(artifacts[i].quantization));
        if (artifacts[i].runtime) cj_set(producer, "runtime", cj_str(artifacts[i].runtime));
        if (artifacts[i].app_name) cj_set(producer, "app_name", cj_str(artifacts[i].app_name));
        if (artifacts[i].app_version) cj_set(producer, "app_version", cj_str(artifacts[i].app_version));
        if (artifacts[i].producer_kind) cj_set(producer, "producer_kind", cj_str(artifacts[i].producer_kind));
        if (artifacts[i].producer_name) cj_set(producer, "name", cj_str(artifacts[i].producer_name));
        if (artifacts[i].model_id) cj_set(producer, "model_id", cj_str(artifacts[i].model_id));
        if (artifacts[i].model_revision) cj_set(producer, "model_revision", cj_str(artifacts[i].model_revision));
        if (artifacts[i].producer_details_json) {
            cj *d = cj_parse(artifacts[i].producer_details_json, NULL);
            cj_set(producer, "details", d ? d : cj_str(artifacts[i].producer_details_json));
        }
        cj_set(provenance, "producer", producer);
        cj *derivation = cj_obj();
        if (artifacts[i].derivation_id) cj_set(derivation, "derivation_id", cj_str(artifacts[i].derivation_id));
        if (artifacts[i].operation) cj_set(derivation, "operation", cj_str(artifacts[i].operation));
        if (artifacts[i].recipe_hash) cj_set(derivation, "recipe_hash", cj_str(artifacts[i].recipe_hash));
        if (artifacts[i].derivation_created_at)
            cj_set(derivation, "created_at", cj_str(artifacts[i].derivation_created_at));
        if (artifacts[i].parameters_json) {
            cj *p = cj_parse(artifacts[i].parameters_json, NULL);
            cj_set(derivation, "parameters", p ? p : cj_str(artifacts[i].parameters_json));
        }
        if (artifacts[i].input_refs_json) {
            cj *ir = cj_parse(artifacts[i].input_refs_json, NULL);
            cj_set(derivation, "inputs", ir ? ir : cj_str(artifacts[i].input_refs_json));
        }
        cj_set(provenance, "derivation", derivation);
        cj_set(item, "provenance", provenance);
        cj_push(items, item);
        returned++;
    }
    cj_set(result, "artifact_count", cj_num((double)returned));
    cj_set(result, "artifacts", items);
    chutni_artifact_info_free(artifacts, artifact_count);
    *out = result;
    return CHUTNI_OK;
}

chutni_status chutni_call(chutni_store *s, const char *operation,
                          const char *arguments_json, char **result_json) {
    if (!operation || !*operation || !result_json) return CHUTNI_ERR_INVALID;
    *result_json = NULL;

    cj *args = (!arguments_json || !*arguments_json) ? cj_obj()
                                                      : cj_parse(arguments_json, NULL);
    cj *out = NULL;
    chutni_status status = CHUTNI_OK;
    if (!args) {
        status = fail(s, CHUTNI_ERR_INVALID,
                      "arguments_json must be a valid JSON object");
        goto error;
    }
    if (args->type != CJ_OBJ) {
        cj_free(args);
        args = NULL;
        status = fail(s, CHUTNI_ERR_INVALID, "arguments_json must be a JSON object");
        goto error;
    }

    if (!strcmp(operation, "discover")) {
        status = jcall_op_discover(args, &out);
    } else if (!strcmp(operation, "capabilities")) {
        status = jcall_op_capabilities(args, &out);
    } else if (!s) {
        status = fail(NULL, CHUTNI_ERR_INVALID,
                     "operation \"%s\" requires an open store", operation);
    } else if (!strcmp(operation, "store_info")) {
        status = jcall_op_store_info(s, args, &out);
    } else if (!strcmp(operation, "add_root")) {
        status = jcall_op_add_root(s, args, &out);
    } else if (!strcmp(operation, "scan")) {
        status = jcall_op_scan(s, args, &out);
    } else if (!strcmp(operation, "children")) {
        status = jcall_op_children(s, args, &out);
    } else if (!strcmp(operation, "observe_directory")) {
        status = jcall_op_observe_directory(s, args, &out);
    } else if (!strcmp(operation, "coverage")) {
        status = jcall_op_coverage(s, args, &out);
    } else if (!strcmp(operation, "search")) {
        status = jcall_op_search(s, args, &out);
    } else if (!strcmp(operation, "search_semantic")) {
        status = jcall_op_search_semantic(s, args, &out);
    } else if (!strcmp(operation, "get_source")) {
        status = jcall_op_get_source(s, args, &out);
    } else if (!strcmp(operation, "get_artifact")) {
        status = jcall_op_get_artifact(s, args, &out);
    } else if (!strcmp(operation, "read_object")) {
        status = jcall_op_read_object(s, args, &out);
    } else if (!strcmp(operation, "check_freshness")) {
        status = jcall_op_check_freshness(s, args, &out);
    } else if (!strcmp(operation, "list_artifacts")) {
        status = jcall_op_list_artifacts(s, args, &out);
    } else if (!strcmp(operation, "source_context")) {
        status = jcall_op_source_context(s, args, &out);
    } else if (!strcmp(operation, "add_source")) {
        status = jcall_op_add_source(s, args, &out);
    } else if (!strcmp(operation, "put_artifacts")) {
        status = jcall_op_put_artifacts(s, args, &out);
    } else if (!strcmp(operation, "put_memory")) {
        status = jcall_op_put_memory(s, args, &out);
    } else if (!strcmp(operation, "put_model_artifact")) {
        status = jcall_op_put_model_artifact(s, args, &out);
    } else if (!strcmp(operation, "put_representation")) {
        status = jcall_op_put_representation(s, args, &out);
    } else if (!strcmp(operation, "mark_source_missing")) {
        status = jcall_op_mark_source_missing(s, args, &out);
    } else if (!strcmp(operation, "forget_source")) {
        status = jcall_op_forget_source(s, args, &out);
    } else if (!strcmp(operation, "rebuild_indexes")) {
        status = jcall_op_rebuild_indexes(s, args, &out);
    } else {
        status = fail(s, CHUTNI_ERR_INVALID, "unknown operation: %s", operation);
    }

    cj_free(args);

    if (status == CHUTNI_OK) {
        *result_json = cj_dump(out, -1);
        cj_free(out);
        if (!*result_json) return CHUTNI_ERR_NOMEM;
        return CHUTNI_OK;
    }

error:
    cj_free(out);
    cj *envelope = cj_obj();
    cj *error = cj_obj();
    cj_set(error, "code", cj_str(chutni_strerror(status)));
    const char *detail = chutni_last_error(s);
    cj_set(error, "message", cj_str(detail && *detail ? detail : chutni_strerror(status)));
    cj_set(envelope, "error", error);
    *result_json = cj_dump(envelope, -1);
    cj_free(envelope);
    if (!*result_json) return CHUTNI_ERR_NOMEM;
    return status;
}
