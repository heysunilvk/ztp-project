/* TC-UNIT-12 (version comparison), TC-UNIT-13 (atomic file write with injected faults), hex. */
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "test.h"
#include "util.h"

static void test_versions(void)
{
    int c = 99;
    CHECK(version_cmp("v0.9", "v1.0", &c) == 0 && c == -1);
    CHECK(version_cmp("v1.0", "v1.1", &c) == 0 && c == -1);
    CHECK(version_cmp("v1.1", "v1.10", &c) == 0 && c == -1);   /* numeric, not lexical */
    CHECK(version_cmp("v1.10", "v1.9", &c) == 0 && c == 1);
    CHECK(version_cmp("v1.1", "v1.1", &c) == 0 && c == 0);
    CHECK(version_cmp("v1.1", "v1.1.0", &c) == 0 && c == 0);
    CHECK(version_cmp("v1.1.1", "v1.1", &c) == 0 && c == 1);
    static const char *const bad[] = {"1.0", "v", "v1", "v1.", "v.1", "v1..2", "v1.2.3.4", "v-1.0",
                                      "v1.0a", "V1.0", "v123456.0", "", "v1.0 "};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint32_t p[3];
        if (version_parse(bad[i], p) == 0) {
            fprintf(stderr, "  accepted bad version \"%s\"\n", bad[i]);
            CHECK(!"malformed version accepted");
        }
    }
    CHECK(version_cmp("v1.0", "garbage", &c) == -1);
}

static void read_back(const char *path, char *out, size_t n)
{
    char *buf = NULL;
    size_t len = 0;
    out[0] = '\0';
    if (read_file(path, 1 << 20, false, &buf, &len) == 0) {
        snprintf(out, n, "%s", buf);
        free(buf);
    }
}

static void test_atomic(void)
{
    char path[400], tmp[420], got[64];
    snprintf(path, sizeof path, "%s/state.txt", test_tmpdir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    CHECK(atomic_write_file(path, "old", 3) == 0);
    read_back(path, got, sizeof got);
    CHECK_STR(got, "old");

    const int faults[] = {AF_AFTER_WRITE, AF_AFTER_FSYNC, AF_BEFORE_RENAME};
    for (size_t i = 0; i < 3; i++) {
        util_atomic_fault = faults[i];
        CHECK(atomic_write_file(path, "new-value", 9) == -1);
        util_atomic_fault = AF_NONE;
        read_back(path, got, sizeof got);
        CHECK_STR(got, "old");                 /* never torn: still the old version */
        CHECK(access(tmp, F_OK) != 0);         /* temp file cleaned up */
    }
    CHECK(atomic_write_file(path, "new-value", 9) == 0);
    read_back(path, got, sizeof got);
    CHECK_STR(got, "new-value");

    /* .bak keeps the previous version; read_file falls back to it if the main file is gone */
    char bak[420];
    snprintf(bak, sizeof bak, "%s.bak", path);
    read_back(bak, got, sizeof got);
    CHECK_STR(got, "old");
    CHECK(unlink(path) == 0);
    char *buf = NULL;
    size_t len;
    CHECK(read_file(path, 1024, true, &buf, &len) == 0 && len == 3 && !memcmp(buf, "old", 3));
    free(buf);
    CHECK(read_file(path, 1024, false, &buf, &len) == -1 && errno == ENOENT);

    /* size cap */
    CHECK(atomic_write_file(path, "0123456789", 10) == 0);
    CHECK(read_file(path, 5, true, &buf, &len) == -1 && errno == E2BIG);
}

static void test_hex(void)
{
    uint8_t in[4] = {0x00, 0xAB, 0x10, 0xFF}, out[4];
    char h[9];
    hex_encode(in, 4, h);
    CHECK_STR(h, "00ab10ff");
    CHECK(hex_decode("00AB10ff", out, 4) == 0 && !memcmp(in, out, 4));
    CHECK(hex_decode("00ab10f", out, 4) == -1);
    CHECK(hex_decode("00ab10fg", out, 4) == -1);
    char s[4];
    CHECK(str_copy(s, sizeof s, "abc") && !strcmp(s, "abc"));
    CHECK(!str_copy(s, sizeof s, "abcd") && !strcmp(s, "abc"));
}

void suite_util(void)
{
    test_versions();
    test_atomic();
    test_hex();
}
