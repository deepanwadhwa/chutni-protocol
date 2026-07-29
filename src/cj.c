#include "cj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ constructors */

static cj *cj_new(cj_type t) {
    cj *v = calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

cj *cj_null(void) { return cj_new(CJ_NULL); }
cj *cj_arr(void)  { return cj_new(CJ_ARR); }
cj *cj_obj(void)  { return cj_new(CJ_OBJ); }

cj *cj_bool(int b) {
    cj *v = cj_new(CJ_BOOL);
    if (v) v->bval = b ? 1 : 0;
    return v;
}

cj *cj_num(double d) {
    cj *v = cj_new(CJ_NUM);
    if (v) v->num = d;
    return v;
}

cj *cj_str(const char *s) {
    cj *v = cj_new(CJ_STR);
    if (!v) return NULL;
    v->str = strdup(s ? s : "");
    if (!v->str) { free(v); return NULL; }
    return v;
}

void cj_free(cj *v) {
    if (!v) return;
    for (size_t i = 0; i < v->n; i++) {
        cj_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v->raw);
    free(v);
}

/* ---------------------------------------------------------------- accessors */

cj *cj_get(const cj *obj, const char *key) {
    if (!obj || obj->type != CJ_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (obj->keys[i] && !strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

const char *cj_get_str(const cj *obj, const char *key) {
    cj *v = cj_get(obj, key);
    return (v && v->type == CJ_STR) ? v->str : NULL;
}

static int grow(cj *v) {
    if (v->n < v->cap) return 1;
    size_t cap = v->cap ? v->cap * 2 : 8;
    cj **items = realloc(v->items, cap * sizeof *items);
    if (!items) return 0;
    v->items = items;
    if (v->type == CJ_OBJ) {
        char **keys = realloc(v->keys, cap * sizeof *keys);
        if (!keys) return 0;
        v->keys = keys;
    }
    v->cap = cap;
    return 1;
}

int cj_push(cj *arr, cj *val) {
    if (!arr || arr->type != CJ_ARR || !val) { cj_free(val); return 0; }
    if (!grow(arr)) { cj_free(val); return 0; }
    arr->items[arr->n++] = val;
    return 1;
}

int cj_set(cj *obj, const char *key, cj *val) {
    if (!obj || obj->type != CJ_OBJ || !key || !val) { cj_free(val); return 0; }
    for (size_t i = 0; i < obj->n; i++) {
        if (obj->keys[i] && !strcmp(obj->keys[i], key)) {
            cj_free(obj->items[i]);
            obj->items[i] = val;   /* replaced in place: key order is stable */
            return 1;
        }
    }
    if (!grow(obj)) { cj_free(val); return 0; }
    char *k = strdup(key);
    if (!k) { cj_free(val); return 0; }
    obj->keys[obj->n] = k;
    obj->items[obj->n] = val;
    obj->n++;
    return 1;
}

/* ------------------------------------------------------------------ parsing */

typedef struct { const char *p; const char *err; } P;

static void skip_ws(P *s) {
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') s->p++;
}

static cj *parse_value(P *s);

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static int utf8_emit(char **out, size_t *len, size_t *cap, unsigned cp) {
    char buf[4];
    int n;
    if (cp < 0x80) { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*len + (size_t)n + 1 > *cap) {
        size_t c = *cap ? *cap * 2 : 32;
        while (*len + (size_t)n + 1 > c) c *= 2;
        char *t = realloc(*out, c);
        if (!t) return 0;
        *out = t;
        *cap = c;
    }
    memcpy(*out + *len, buf, (size_t)n);
    *len += (size_t)n;
    return 1;
}

static char *parse_string_raw(P *s) {
    if (*s->p != '"') { s->err = "expected string"; return NULL; }
    s->p++;
    char *out = NULL;
    size_t len = 0, cap = 0;
    while (*s->p && *s->p != '"') {
        unsigned cp;
        if (*s->p == '\\') {
            s->p++;
            switch (*s->p) {
            case '"':  cp = '"';  s->p++; break;
            case '\\': cp = '\\'; s->p++; break;
            case '/':  cp = '/';  s->p++; break;
            case 'b':  cp = '\b'; s->p++; break;
            case 'f':  cp = '\f'; s->p++; break;
            case 'n':  cp = '\n'; s->p++; break;
            case 'r':  cp = '\r'; s->p++; break;
            case 't':  cp = '\t'; s->p++; break;
            case 'u': {
                unsigned hi;
                if (!hex4(s->p + 1, &hi)) { s->err = "bad \\u escape"; free(out); return NULL; }
                s->p += 5;
                if (hi >= 0xD800 && hi <= 0xDBFF && s->p[0] == '\\' && s->p[1] == 'u') {
                    unsigned lo;
                    if (hex4(s->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
                        s->p += 6;
                    } else {
                        cp = 0xFFFD;   /* unpaired high surrogate */
                    }
                } else if (hi >= 0xD800 && hi <= 0xDFFF) {
                    cp = 0xFFFD;       /* lone surrogate */
                } else {
                    cp = hi;
                }
                break;
            }
            default: s->err = "bad escape"; free(out); return NULL;
            }
        } else {
            cp = (unsigned char)*s->p++;
        }
        if (!utf8_emit(&out, &len, &cap, cp)) { s->err = "out of memory"; free(out); return NULL; }
    }
    if (*s->p != '"') { s->err = "unterminated string"; free(out); return NULL; }
    s->p++;
    if (!out) { out = calloc(1, 1); if (!out) { s->err = "out of memory"; return NULL; } }
    else out[len] = 0;
    return out;
}

static cj *parse_value(P *s) {
    skip_ws(s);
    switch (*s->p) {
    case '"': {
        char *str = parse_string_raw(s);
        if (!str) return NULL;
        cj *v = cj_new(CJ_STR);
        if (!v) { free(str); s->err = "out of memory"; return NULL; }
        v->str = str;
        return v;
    }
    case '{': {
        s->p++;
        cj *o = cj_obj();
        if (!o) { s->err = "out of memory"; return NULL; }
        skip_ws(s);
        if (*s->p == '}') { s->p++; return o; }
        for (;;) {
            skip_ws(s);
            char *key = parse_string_raw(s);
            if (!key) { cj_free(o); return NULL; }
            skip_ws(s);
            if (*s->p != ':') { s->err = "expected ':'"; free(key); cj_free(o); return NULL; }
            s->p++;
            cj *val = parse_value(s);
            if (!val) { free(key); cj_free(o); return NULL; }
            if (!grow(o)) { s->err = "out of memory"; free(key); cj_free(val); cj_free(o); return NULL; }
            o->keys[o->n] = key;
            o->items[o->n] = val;
            o->n++;
            skip_ws(s);
            if (*s->p == ',') { s->p++; continue; }
            if (*s->p == '}') { s->p++; return o; }
            s->err = "expected ',' or '}'";
            cj_free(o);
            return NULL;
        }
    }
    case '[': {
        s->p++;
        cj *a = cj_arr();
        if (!a) { s->err = "out of memory"; return NULL; }
        skip_ws(s);
        if (*s->p == ']') { s->p++; return a; }
        for (;;) {
            cj *val = parse_value(s);
            if (!val) { cj_free(a); return NULL; }
            if (!cj_push(a, val)) { s->err = "out of memory"; cj_free(a); return NULL; }
            skip_ws(s);
            if (*s->p == ',') { s->p++; continue; }
            if (*s->p == ']') { s->p++; return a; }
            s->err = "expected ',' or ']'";
            cj_free(a);
            return NULL;
        }
    }
    case 't':
        if (strncmp(s->p, "true", 4)) { s->err = "bad literal"; return NULL; }
        s->p += 4;
        return cj_bool(1);
    case 'f':
        if (strncmp(s->p, "false", 5)) { s->err = "bad literal"; return NULL; }
        s->p += 5;
        return cj_bool(0);
    case 'n':
        if (strncmp(s->p, "null", 4)) { s->err = "bad literal"; return NULL; }
        s->p += 4;
        return cj_null();
    default: {
        const char *start = s->p;
        if (*s->p == '-' || *s->p == '+') s->p++;
        int digits = 0;
        while (*s->p >= '0' && *s->p <= '9') { s->p++; digits = 1; }
        if (*s->p == '.') { s->p++; while (*s->p >= '0' && *s->p <= '9') { s->p++; digits = 1; } }
        if (digits && (*s->p == 'e' || *s->p == 'E')) {
            s->p++;
            if (*s->p == '-' || *s->p == '+') s->p++;
            while (*s->p >= '0' && *s->p <= '9') s->p++;
        }
        if (!digits) { s->err = "unexpected token"; return NULL; }
        size_t len = (size_t)(s->p - start);
        cj *v = cj_new(CJ_NUM);
        if (!v) { s->err = "out of memory"; return NULL; }
        v->raw = malloc(len + 1);
        if (!v->raw) { cj_free(v); s->err = "out of memory"; return NULL; }
        memcpy(v->raw, start, len);
        v->raw[len] = 0;
        v->num = strtod(v->raw, NULL);
        return v;
    }
    }
}

cj *cj_parse(const char *text, const char **err) {
    if (err) *err = NULL;
    if (!text) { if (err) *err = "no input"; return NULL; }
    P s = { text, NULL };
    cj *v = parse_value(&s);
    if (!v) { if (err) *err = s.err ? s.err : "parse error"; return NULL; }
    skip_ws(&s);
    if (*s.p) { cj_free(v); if (err) *err = "trailing content"; return NULL; }
    return v;
}

/* -------------------------------------------------------------- serializing */

typedef struct { char *buf; size_t len, cap; int ok; } B;

static void bput(B *b, const char *s, size_t n) {
    if (!b->ok) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char *t = realloc(b->buf, cap);
        if (!t) { b->ok = 0; return; }
        b->buf = t;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

static void bputs(B *b, const char *s) { bput(b, s, strlen(s)); }

static void bput_json_string(B *b, const char *s) {
    bput(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  bputs(b, "\\\""); break;
        case '\\': bputs(b, "\\\\"); break;
        case '\b': bputs(b, "\\b"); break;
        case '\f': bputs(b, "\\f"); break;
        case '\n': bputs(b, "\\n"); break;
        case '\r': bputs(b, "\\r"); break;
        case '\t': bputs(b, "\\t"); break;
        default:
            if (*p < 0x20) {
                char esc[7];
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                bputs(b, esc);
            } else {
                bput(b, (const char *)p, 1);
            }
        }
    }
    bput(b, "\"", 1);
}

static void indent_by(B *b, int indent, int depth) {
    if (indent < 0) return;
    bput(b, "\n", 1);
    for (int i = 0; i < indent * depth; i++) bput(b, " ", 1);
}

static void dump(B *b, const cj *v, int indent, int depth) {
    if (!v) { bputs(b, "null"); return; }
    switch (v->type) {
    case CJ_NULL: bputs(b, "null"); break;
    case CJ_BOOL: bputs(b, v->bval ? "true" : "false"); break;
    case CJ_NUM:
        if (v->raw) {
            bputs(b, v->raw);
        } else {
            char tmp[40];
            if (v->num == (double)(long long)v->num)
                snprintf(tmp, sizeof tmp, "%lld", (long long)v->num);
            else
                snprintf(tmp, sizeof tmp, "%.17g", v->num);
            bputs(b, tmp);
        }
        break;
    case CJ_STR: bput_json_string(b, v->str ? v->str : ""); break;
    case CJ_ARR:
        if (v->n == 0) { bputs(b, "[]"); break; }
        bput(b, "[", 1);
        for (size_t i = 0; i < v->n; i++) {
            if (i) bput(b, ",", 1);
            indent_by(b, indent, depth + 1);
            dump(b, v->items[i], indent, depth + 1);
        }
        indent_by(b, indent, depth);
        bput(b, "]", 1);
        break;
    case CJ_OBJ:
        if (v->n == 0) { bputs(b, "{}"); break; }
        bput(b, "{", 1);
        for (size_t i = 0; i < v->n; i++) {
            if (i) bput(b, ",", 1);
            indent_by(b, indent, depth + 1);
            bput_json_string(b, v->keys[i] ? v->keys[i] : "");
            bput(b, ":", 1);
            if (indent >= 0) bput(b, " ", 1);
            dump(b, v->items[i], indent, depth + 1);
        }
        indent_by(b, indent, depth);
        bput(b, "}", 1);
        break;
    }
}

char *cj_dump(const cj *v, int indent) {
    B b = { NULL, 0, 0, 1 };
    bput(&b, "", 0);
    dump(&b, v, indent, 0);
    if (!b.ok) { free(b.buf); return NULL; }
    return b.buf;
}
