#include "json.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- arena ---- */

typedef struct block { struct block *next; size_t used, cap; unsigned char data[]; } block_t;

struct json_doc {
    block_t      *blocks;
    json_value_t *root;
};

static void *arena_alloc(json_doc_t *d, size_t n)
{
    n = (n + 15u) & ~(size_t)15u;
    block_t *b = d->blocks;
    if (!b || b->cap - b->used < n) {
        size_t cap = n > 8192 ? n : 8192;
        b = malloc(sizeof *b + cap);
        if (!b) return NULL;
        b->next = d->blocks;
        b->used = 0;
        b->cap = cap;
        d->blocks = b;
    }
    void *p = b->data + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

void json_free(json_doc_t *d)
{
    if (!d) return;
    block_t *b = d->blocks;
    while (b) {
        block_t *n = b->next;
        free(b);
        b = n;
    }
    free(d);
}

json_value_t *json_root(json_doc_t *d) { return d ? d->root : NULL; }

/* ---- parser ---- */

typedef struct {
    const char   *p, *end, *line_start;
    unsigned      line;
    int           depth;
    json_doc_t   *doc;
    json_error_t *err;
    /* current path, as a stack of segment offsets into path[] */
    char          path[160];
    size_t        seg[JSON_MAX_DEPTH + 2];
    int           nseg;
    bool          failed;
} P;

static void fail(P *ps, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(P *ps, const char *fmt, ...)
{
    if (ps->failed) return;
    ps->failed = true;
    if (!ps->err) return;
    ps->err->line = ps->line;
    ps->err->col = (unsigned)(ps->p - ps->line_start) + 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->err->msg, sizeof ps->err->msg, fmt, ap);
    va_end(ap);
    snprintf(ps->err->path, sizeof ps->err->path, "%s", ps->path[0] ? ps->path : "(root)");
}

static void path_push(P *ps, const char *seg, size_t n)
{
    size_t cur = strlen(ps->path);
    ps->seg[ps->nseg++] = cur;
    size_t room = sizeof ps->path - cur - 1;
    if (cur && room) { ps->path[cur++] = '.'; room--; }
    if (n > room) n = room;
    memcpy(ps->path + cur, seg, n);
    ps->path[cur + n] = '\0';
}

static void path_push_index(P *ps, size_t i)
{
    char b[24];
    int n = snprintf(b, sizeof b, "[%zu]", i);
    size_t cur = strlen(ps->path);
    ps->seg[ps->nseg++] = cur;
    if (cur + (size_t)n < sizeof ps->path) memcpy(ps->path + cur, b, (size_t)n + 1);
}

static void path_pop(P *ps)
{
    if (ps->nseg > 0) ps->path[ps->seg[--ps->nseg]] = '\0';
}

static void skip_ws(P *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\r') ps->p++;
        else if (c == '\n') { ps->p++; ps->line++; ps->line_start = ps->p; }
        else if (c == '/') { fail(ps, "comments are not allowed"); return; }
        else break;
    }
}

static json_value_t *new_val(P *ps, json_type_t t)
{
    json_value_t *v = arena_alloc(ps->doc, sizeof *v);
    if (!v) { fail(ps, "out of memory"); return NULL; }
    v->type = t;
    v->line = ps->line;
    return v;
}

static json_value_t *parse_value(P *ps);

static int hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static size_t put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Validate one UTF-8 sequence starting at s (s < end). Returns its length or 0 if invalid. */
static size_t utf8_seq(const unsigned char *s, const unsigned char *end)
{
    unsigned char c = s[0];
    size_t n;
    uint32_t cp, min;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; min = 0x10000; }
    else return 0;
    if ((size_t)(end - s) < n) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

/* Parses a string at ps->p (which points at the opening quote). */
static bool parse_string(P *ps, char **out, size_t *out_len)
{
    ps->p++;   /* opening quote */
    /* First pass: find the end and the decoded upper bound (decoded ≤ raw length). */
    const char *s = ps->p;
    size_t cap = 0;
    while (s < ps->end && *s != '"') {
        if (*s == '\\') { s++; if (s >= ps->end) break; }
        s++;
        cap++;
    }
    if (s >= ps->end) { fail(ps, "unterminated string"); return false; }
    char *o = arena_alloc(ps->doc, (size_t)(s - ps->p) + 1);
    if (!o) { fail(ps, "out of memory"); return false; }
    size_t n = 0;
    (void)cap;
    while (*ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p;
        if (c < 0x20) { fail(ps, "control character in string"); return false; }
        if (c == '\\') {
            ps->p++;
            char e = *ps->p++;
            switch (e) {
            case '"': o[n++] = '"'; break;
            case '\\': o[n++] = '\\'; break;
            case '/': o[n++] = '/'; break;
            case 'b': o[n++] = '\b'; break;
            case 'f': o[n++] = '\f'; break;
            case 'n': o[n++] = '\n'; break;
            case 'r': o[n++] = '\r'; break;
            case 't': o[n++] = '\t'; break;
            case 'u': {
                if (ps->end - ps->p < 4) { fail(ps, "bad \\u escape"); return false; }
                int h = hex4(ps->p);
                if (h < 0) { fail(ps, "bad \\u escape"); return false; }
                ps->p += 4;
                uint32_t cp = (uint32_t)h;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') {
                        fail(ps, "unpaired surrogate");
                        return false;
                    }
                    int lo = hex4(ps->p + 2);
                    if (lo < 0xDC00 || lo > 0xDFFF) { fail(ps, "unpaired surrogate"); return false; }
                    ps->p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail(ps, "unpaired surrogate");
                    return false;
                }
                if (cp == 0) { fail(ps, "NUL character in string"); return false; }
                n += put_utf8(o + n, cp);
                break;
            }
            default: fail(ps, "invalid escape"); return false;
            }
        } else {
            size_t k = utf8_seq((const unsigned char *)ps->p, (const unsigned char *)s);
            if (!k) { fail(ps, "invalid UTF-8"); return false; }
            memcpy(o + n, ps->p, k);
            n += k;
            ps->p += k;
        }
    }
    ps->p++;   /* closing quote */
    o[n] = '\0';
    *out = o;
    *out_len = n;
    return true;
}

static json_value_t *parse_number(P *ps)
{
    const char *s = ps->p;
    bool is_int = true;
    if (*s == '-') s++;
    if (s >= ps->end) { fail(ps, "bad number"); return NULL; }
    if (*s == '0') {
        s++;
        if (s < ps->end && *s >= '0' && *s <= '9') { fail(ps, "leading zeros are not allowed"); return NULL; }
    } else if (*s >= '1' && *s <= '9') {
        while (s < ps->end && *s >= '0' && *s <= '9') s++;
    } else { fail(ps, "bad number"); return NULL; }
    if (s < ps->end && *s == '.') {
        is_int = false;
        s++;
        if (s >= ps->end || *s < '0' || *s > '9') { fail(ps, "bad number"); return NULL; }
        while (s < ps->end && *s >= '0' && *s <= '9') s++;
    }
    if (s < ps->end && (*s == 'e' || *s == 'E')) {
        is_int = false;
        s++;
        if (s < ps->end && (*s == '+' || *s == '-')) s++;
        if (s >= ps->end || *s < '0' || *s > '9') { fail(ps, "bad number"); return NULL; }
        while (s < ps->end && *s >= '0' && *s <= '9') s++;
    }
    char buf[64];
    size_t n = (size_t)(s - ps->p);
    if (n >= sizeof buf) { fail(ps, "number too long"); return NULL; }
    memcpy(buf, ps->p, n);
    buf[n] = '\0';
    json_value_t *v = new_val(ps, is_int ? JINT : JDOUBLE);
    if (!v) return NULL;
    errno = 0;
    if (is_int) {
        long long x = strtoll(buf, NULL, 10);
        if (errno == ERANGE) { fail(ps, "integer out of 64-bit range"); return NULL; }
        v->u.i = (int64_t)x;
    } else {
        double x = strtod(buf, NULL);
        if (errno == ERANGE || !isfinite(x)) { fail(ps, "number out of range"); return NULL; }
        v->u.d = x;
    }
    ps->p = s;
    return v;
}

static bool lit(P *ps, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n) != 0) return false;
    ps->p += n;
    return true;
}

static json_value_t *parse_container(P *ps, bool is_obj)
{
    if (++ps->depth > JSON_MAX_DEPTH) { fail(ps, "nesting deeper than %d", JSON_MAX_DEPTH); return NULL; }
    json_value_t *v = new_val(ps, is_obj ? JOBJECT : JARRAY);
    if (!v) return NULL;
    ps->p++;   /* { or [ */
    json_value_t **tail = is_obj ? &v->u.obj.first : &v->u.arr.first;
    size_t count = 0;
    char close = is_obj ? '}' : ']';
    skip_ws(ps);
    if (ps->failed) return NULL;
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;) {
        skip_ws(ps);
        if (ps->failed) return NULL;
        if (ps->p >= ps->end) { fail(ps, "unexpected end of input"); return NULL; }
        char *key = NULL;
        size_t klen = 0;
        if (is_obj) {
            if (*ps->p == close) { fail(ps, "trailing comma"); return NULL; }
            if (*ps->p != '"') { fail(ps, "expected a string key"); return NULL; }
            if (!parse_string(ps, &key, &klen)) return NULL;
            if (strlen(key) != klen) { fail(ps, "NUL in key"); return NULL; }
            for (json_value_t *m = v->u.obj.first; m; m = m->next)
                if (!strcmp(m->key, key)) {
                    path_push(ps, key, klen);
                    fail(ps, "duplicate key \"%s\"", key);
                    return NULL;
                }
            skip_ws(ps);
            if (ps->failed) return NULL;
            if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "expected ':'"); return NULL; }
            ps->p++;
            path_push(ps, key, klen);
        } else {
            if (*ps->p == close) { fail(ps, "trailing comma"); return NULL; }
            path_push_index(ps, count);
        }
        json_value_t *item = parse_value(ps);
        if (!item) return NULL;
        path_pop(ps);
        item->key = key;
        *tail = item;
        tail = &item->next;
        count++;
        skip_ws(ps);
        if (ps->failed) return NULL;
        if (ps->p >= ps->end) { fail(ps, "unexpected end of input"); return NULL; }
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; break; }
        fail(ps, "expected ',' or '%c'", close);
        return NULL;
    }
    if (is_obj) v->u.obj.count = count;
    else v->u.arr.count = count;
    ps->depth--;
    return v;
}

static json_value_t *parse_value(P *ps)
{
    skip_ws(ps);
    if (ps->failed) return NULL;
    if (ps->p >= ps->end) { fail(ps, "unexpected end of input"); return NULL; }
    char c = *ps->p;
    json_value_t *v;
    switch (c) {
    case '{': return parse_container(ps, true);
    case '[': return parse_container(ps, false);
    case '"':
        v = new_val(ps, JSTRING);
        if (!v || !parse_string(ps, &v->u.str.s, &v->u.str.len)) return NULL;
        return v;
    case 't':
        if (!lit(ps, "true")) break;
        v = new_val(ps, JBOOL);
        if (v) v->u.b = true;
        return v;
    case 'f':
        if (!lit(ps, "false")) break;
        v = new_val(ps, JBOOL);
        if (v) v->u.b = false;
        return v;
    case 'n':
        if (!lit(ps, "null")) break;
        return new_val(ps, JNULL);
    default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
        break;
    }
    fail(ps, "unexpected character '%c'", c >= 0x20 && c < 0x7F ? c : '?');
    return NULL;
}

json_doc_t *json_parse(const char *text, size_t len, json_error_t *err)
{
    if (err) memset(err, 0, sizeof *err);
    P ps = {0};
    ps.err = err;
    ps.line = 1;
    ps.p = ps.line_start = text;
    ps.end = text + len;
    if (len > JSON_MAX_INPUT) {
        fail(&ps, "input larger than %u bytes", JSON_MAX_INPUT);
        return NULL;
    }
    /* Reject a UTF-8 BOM explicitly (RFC 8259 §8.1: must not be added). */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        fail(&ps, "byte order mark is not allowed");
        return NULL;
    }
    ps.doc = calloc(1, sizeof *ps.doc);
    if (!ps.doc) { fail(&ps, "out of memory"); return NULL; }
    json_value_t *root = parse_value(&ps);
    if (root) {
        skip_ws(&ps);
        if (!ps.failed && ps.p != ps.end) fail(&ps, "trailing data after the value");
    }
    if (ps.failed || !root) {
        json_free(ps.doc);
        return NULL;
    }
    ps.doc->root = root;
    return ps.doc;
}

json_value_t *json_get(const json_value_t *obj, const char *key)
{
    if (!obj || obj->type != JOBJECT) return NULL;
    for (json_value_t *m = obj->u.obj.first; m; m = m->next)
        if (!strcmp(m->key, key)) return m;
    return NULL;
}

json_value_t *json_get_path(const json_value_t *obj, const char *path)
{
    char seg[128];
    const json_value_t *cur = obj;
    while (cur && *path) {
        const char *dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        if (n >= sizeof seg) return NULL;
        memcpy(seg, path, n);
        seg[n] = '\0';
        cur = json_get(cur, seg);
        path += n + (dot ? 1 : 0);
    }
    return (json_value_t *)cur;
}

bool json_is_number(const json_value_t *v) { return v && (v->type == JINT || v->type == JDOUBLE); }

double json_as_double(const json_value_t *v)
{
    return v->type == JINT ? (double)v->u.i : v->u.d;
}

const char *json_type_name(json_type_t t)
{
    static const char *const N[] = {"null", "boolean", "integer", "number", "string", "array", "object"};
    return (unsigned)t < sizeof N / sizeof N[0] ? N[t] : "?";
}
