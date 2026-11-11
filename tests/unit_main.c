#include <stdlib.h>
#include <unistd.h>

#include "test.h"

int t_checks, t_failures;
const char *t_suite = "";

void hex_of(const unsigned char *p, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[p[i] >> 4];
        out[2 * i + 1] = H[p[i] & 15];
    }
    out[2 * n] = '\0';
}

static char g_tmp[256];

const char *test_tmpdir(void)
{
    if (!g_tmp[0]) {
        const char *base = getenv("TMPDIR");
        snprintf(g_tmp, sizeof g_tmp, "%s/ztp-unit-XXXXXX", base && *base ? base : "/tmp");
        if (!mkdtemp(g_tmp)) {
            perror("mkdtemp");
            exit(2);
        }
    }
    return g_tmp;
}

static const struct { const char *name; void (*fn)(void); } SUITES[] = {
    {"crypto", suite_crypto}, {"protocol", suite_protocol}, {"json", suite_json},
    {"config", suite_config}, {"util", suite_util},         {"netsim", suite_netsim},
    {"timer", suite_timer},   {"policy", suite_policy},     {"transfer", suite_transfer},
};

int main(int argc, char **argv)
{
    for (size_t i = 0; i < sizeof SUITES / sizeof SUITES[0]; i++) {
        if (argc > 1 && strcmp(argv[1], SUITES[i].name) != 0) continue;
        int before_c = t_checks, before_f = t_failures;
        t_suite = SUITES[i].name;
        SUITES[i].fn();
        printf("%-10s %5d checks, %d failed\n", SUITES[i].name, t_checks - before_c, t_failures - before_f);
    }
    if (g_tmp[0]) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_tmp);
        if (system(cmd) != 0) fprintf(stderr, "warning: could not remove %s\n", g_tmp);
    }
    printf("unit tests: %d checks, %d failed\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
