/* Strict RFC 8259-subset JSON parser (DESIGN §25.2).
 * - input ≤ 64 KiB, nesting depth ≤ 16
 * - duplicate keys, trailing commas, comments, leading zeros, NaN/Infinity, control characters
 *   in strings, invalid UTF-8 and trailing garbage are rejected
 * - numbers: 64-bit signed integers, or decimals (stored as double)
 * - errors carry line, column and the JSON path being parsed */
#ifndef ZTP_JSON_H
#define ZTP_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JSON_MAX_INPUT (64u * 1024u)
#define JSON_MAX_DEPTH 16

typedef enum { JNULL = 0, JBOOL, JINT, JDOUBLE, JSTRING, JARRAY, JOBJECT } json_type_t;

typedef struct json_value json_value_t;
struct json_value {
    json_type_t type;
    unsigned    line;       /* 1-based line where the value starts */
    union {
        bool    b;
        int64_t i;
        double  d;
        struct { char *s; size_t len; } str;
        struct { json_value_t *first; size_t count; } arr;   /* elements linked via next */
        struct { json_value_t *first; size_t count; } obj;   /* members linked via next, key set */
    } u;
    char         *key;      /* member name when inside an object, else NULL */
    json_value_t *next;
};

typedef struct {
    unsigned line, col;
    char     msg[96];
    char     path[160];     /* e.g. "nat.keepalive_interval_s" or "models.EDGE-GW-100" */
} json_error_t;

typedef struct json_doc json_doc_t;   /* owns every allocation */

/* Parse text[0..len). Returns a document (free with json_free) or NULL with *err filled. */
json_doc_t   *json_parse(const char *text, size_t len, json_error_t *err);
json_value_t *json_root(json_doc_t *doc);
void          json_free(json_doc_t *doc);

/* Lookup helpers. */
json_value_t *json_get(const json_value_t *obj, const char *key);      /* NULL if absent */
/* Dotted path lookup ("nat.grace_s"); segments must be object keys. */
json_value_t *json_get_path(const json_value_t *obj, const char *path);
bool          json_is_number(const json_value_t *v);
double        json_as_double(const json_value_t *v);
const char   *json_type_name(json_type_t t);

#endif
