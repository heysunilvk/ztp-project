/* TC-UNIT-18: strict JSON parser — accept/reject corpus, limits, error positions, and a
 * random/mutation fuzz (FUZZ_ITERS, default 100000; CI ASan run uses 1000000). */
#include <stdlib.h>

#include "json.h"
#include "test.h"

static int parses(const char *s)
{
    json_error_t e;
    json_doc_t *d = json_parse(s, strlen(s), &e);
    json_free(d);
    return d != NULL;
}

static void test_accept(void)
{
    static const char *const ok[] = {
        "{}", "[]", "0", "-0", "1", "-1", "1.5", "-1.5e10", "1E+2", "1e-2", "0.0", "true", "false", "null",
        "\"\"", "\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"", "\"\\u00e9\"", "\"\\ud83d\\ude00\"", "\"\xc3\xa9\"",
        "\"\xf0\x9f\x98\x80\"", " { \"a\" : [ 1 , 2 , { \"b\" : null } ] } ", "[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]",
        "9223372036854775807", "-9223372036854775808", "{\"a\":1,\"b\":{\"a\":2}}", "\t\r\n[1]\n",
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (!parses(ok[i])) {
            fprintf(stderr, "  should accept: %s\n", ok[i]);
            CHECK(!"valid JSON rejected");
        }
}

static void test_reject(void)
{
    static const char *const bad[] = {
        "", " ", "{", "}", "[", "[1,]", "{\"a\":1,}", "[,1]", "{,}", "{\"a\"}", "{\"a\":}", "{a:1}", "{'a':1}",
        "{\"a\":1 \"b\":2}", "[1 2]", "01", "-01", "1.", ".5", "-", "+1", "1e", "1e+", "0x10", "NaN",
        "Infinity", "-Infinity", "tru", "nul", "True", "\"abc", "\"\\x\"", "\"\\u12\"", "\"\\uZZZZ\"",
        "\"\\ud83d\"", "\"\\ude00\"", "\"\\u0000\"", "\"a\nb\"", "\"\t\"", "\"\xc3\"", "\"\xff\"",
        "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "[1]x", "{} {}", "// c\n{}", "{/* c */}", "{\"a\":1}#",
        "{\"a\":1,\"a\":2}", "{\"x\":{\"k\":1,\"k\":1}}", "9223372036854775808", "1e400",
        "[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]", "\xef\xbb\xbf{}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (parses(bad[i])) {
            fprintf(stderr, "  should reject: %s\n", bad[i]);
            CHECK(!"invalid JSON accepted");
        }
    /* embedded NUL byte inside the input buffer */
    json_error_t e;
    CHECK(json_parse("{\"a\":1}\0", 8, &e) == NULL);
}

static void test_limits_and_errors(void)
{
    json_error_t e;
    /* depth 16 ok, 17 rejected (with message) */
    char deep[64];
    memset(deep, '[', 16);
    memset(deep + 16, ']', 16);
    deep[32] = '\0';
    CHECK(parses(deep));
    memset(deep, '[', 17);
    memset(deep + 17, ']', 17);
    deep[34] = '\0';
    CHECK(json_parse(deep, strlen(deep), &e) == NULL && strstr(e.msg, "nesting"));

    /* size limit */
    size_t big = JSON_MAX_INPUT + 1;
    char *s = malloc(big + 1);
    memset(s, ' ', big);
    s[0] = '1';
    s[big] = '\0';
    CHECK(json_parse(s, big, &e) == NULL && strstr(e.msg, "larger"));
    json_doc_t *ok = json_parse(s, JSON_MAX_INPUT, &e);   /* exactly 64 KiB: allowed */
    CHECK(ok != NULL);
    json_free(ok);
    free(s);

    /* error line, column and path */
    const char *t = "{\n  \"nat\": {\n    \"grace_s\": 120,\n    \"grace_s\": 5\n  }\n}";
    CHECK(json_parse(t, strlen(t), &e) == NULL);
    CHECK_EQ_INT(e.line, 4);
    CHECK_STR(e.path, "nat.grace_s");
    CHECK(strstr(e.msg, "duplicate") != NULL);
    t = "{\"a\": [1, 2, ]}";
    CHECK(json_parse(t, strlen(t), &e) == NULL && strstr(e.msg, "trailing comma"));
    CHECK_STR(e.path, "a");
    t = "{\"a\": [1, {\"b\": tru}]}";
    CHECK(json_parse(t, strlen(t), &e) == NULL);
    CHECK_STR(e.path, "a[1].b");
    t = "{\n\"a\": 1, // note\n}";
    CHECK(json_parse(t, strlen(t), &e) == NULL && strstr(e.msg, "comment") && e.line == 2);
}

static void test_values(void)
{
    const char *t = "{\"i\": -42, \"d\": 2.5, \"b\": true, \"n\": null, \"s\": \"h\\u00e9\", "
                    "\"o\": {\"x\": {\"y\": 7}}, \"a\": [1, \"two\", [3]]}";
    json_error_t e;
    json_doc_t *d = json_parse(t, strlen(t), &e);
    CHECK(d != NULL);
    if (!d) return;
    json_value_t *r = json_root(d);
    CHECK(json_get(r, "i")->type == JINT && json_get(r, "i")->u.i == -42);
    CHECK(json_get(r, "d")->type == JDOUBLE && json_get(r, "d")->u.d == 2.5);
    CHECK(json_get(r, "b")->u.b == true && json_get(r, "n")->type == JNULL);
    CHECK_STR(json_get(r, "s")->u.str.s, "h\xc3\xa9");
    CHECK(json_get_path(r, "o.x.y")->u.i == 7);
    CHECK(json_get_path(r, "o.x.z") == NULL && json_get_path(r, "i.x") == NULL);
    json_value_t *a = json_get(r, "a");
    CHECK(a->type == JARRAY && a->u.arr.count == 3);
    CHECK(a->u.arr.first->next->type == JSTRING);
    CHECK(r->u.obj.count == 7);
    json_free(d);
}

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t xr(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static void test_fuzz(void)
{
    const char *seeds[] = {
        "{\"schema_version\":1,\"nat\":{\"keepalive_interval_s\":20,\"grace_s\":120},\"x\":[1,2.5,true,null,\"s\"]}",
        "[{\"a\":\"\\u00e9\\n\"},[[]],{}]", "{\"models\":{\"EDGE-GW-100\":{\"firmware\":\"v1.1\"}}}",
    };
    const char alphabet[] = "{}[]:,\"\\ -0123456789.eE+truefalsnul\x80\xc3\xa9\n\t/u";
    const char *env = getenv("FUZZ_ITERS");
    long iters = env ? atol(env) : 100000;
    long accepted = 0;
    char buf[512];
    for (long it = 0; it < iters; it++) {
        size_t n;
        if (it % 4 == 0) {   /* purely random from the JSON alphabet */
            n = xr() % 64;
            for (size_t i = 0; i < n; i++) buf[i] = alphabet[xr() % (sizeof alphabet - 1)];
        } else {             /* mutate a seed */
            const char *sd = seeds[xr() % 3];
            n = strlen(sd);
            memcpy(buf, sd, n);
            int muts = 1 + (int)(xr() % 4);
            for (int m = 0; m < muts; m++) {
                size_t pos = n ? xr() % n : 0;
                switch (xr() % 4) {
                case 0: buf[pos] = alphabet[xr() % (sizeof alphabet - 1)]; break;
                case 1: if (n) { memmove(buf + pos, buf + pos + 1, n - pos - 1); n--; } break;
                case 2: if (n + 1 < sizeof buf) { memmove(buf + pos + 1, buf + pos, n - pos); buf[pos] = alphabet[xr() % (sizeof alphabet - 1)]; n++; } break;
                case 3: buf[pos] = (char)xr(); break;
                }
            }
        }
        /* exact-size heap copy so ASan catches any read past the end */
        char *heap = malloc(n ? n : 1);
        memcpy(heap, buf, n);
        json_error_t e;
        json_doc_t *d = json_parse(heap, n, &e);
        if (d) accepted++;
        json_free(d);
        free(heap);
    }
    CHECK(accepted > 0);
    printf("           json fuzz: %ld inputs, %ld accepted\n", iters, accepted);
}

void suite_json(void)
{
    test_accept();
    test_reject();
    test_limits_and_errors();
    test_values();
    test_fuzz();
}
