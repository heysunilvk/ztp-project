/* Minimal unit-test harness. Each suite is a function registered in unit_main.c. */
#ifndef ZTP_TEST_H
#define ZTP_TEST_H

#include <stdio.h>
#include <string.h>

extern int t_checks, t_failures;
extern const char *t_suite;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        t_checks++;                                                                          \
        if (!(cond)) {                                                                       \
            t_failures++;                                                                    \
            fprintf(stderr, "  FAIL [%s] %s:%d: %s\n", t_suite, __FILE__, __LINE__, #cond); \
        }                                                                                    \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                     \
    do {                                                                                       \
        long long _a = (long long)(a), _b = (long long)(b);                                    \
        t_checks++;                                                                            \
        if (_a != _b) {                                                                        \
            t_failures++;                                                                      \
            fprintf(stderr, "  FAIL [%s] %s:%d: %s == %s (%lld vs %lld)\n", t_suite, __FILE__, \
                    __LINE__, #a, #b, _a, _b);                                                 \
        }                                                                                      \
    } while (0)

#define CHECK_STR(a, b)                                                                          \
    do {                                                                                         \
        const char *_a = (a), *_b = (b);                                                         \
        t_checks++;                                                                              \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                                 \
            t_failures++;                                                                        \
            fprintf(stderr, "  FAIL [%s] %s:%d: %s == \"%s\" (got \"%s\")\n", t_suite, __FILE__, \
                    __LINE__, #a, _b ? _b : "(null)", _a ? _a : "(null)");                       \
        }                                                                                        \
    } while (0)

/* Helpers shared by suites. */
void hex_of(const unsigned char *p, size_t n, char *out);   /* lowercase, NUL-terminated */
const char *test_tmpdir(void);                              /* per-run scratch directory */

void suite_crypto(void);
void suite_protocol(void);
void suite_json(void);
void suite_config(void);
void suite_util(void);
void suite_netsim(void);
void suite_timer(void);
void suite_policy(void);
void suite_transfer(void);
void suite_reliable(void);
void suite_db(void);

#endif
