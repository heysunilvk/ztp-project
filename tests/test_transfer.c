/* TC-UNIT-14 (chunk bitmap persistence), TC-UNIT-19 (bitmap conversion on chunk-size change),
 * TC-UNIT-17 (provisioned-config key validation against cfg_v3 / cfg_missing_key / cfg_malformed). */
#include <stdlib.h>
#include <unistd.h>

#include "test.h"
#include "transfer.h"
#include "util.h"
#include "validate.h"

static uint64_t rs = 0xC0FFEE1234567ull;
static uint64_t xr(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static void test_map_basics(void)
{
    uint8_t sha[32] = {1};
    dev_xfermap_t m;
    CHECK_EQ_INT(dev_xfermap_init(&m, sha, 2310, 1024), XFERMAP_OK);
    CHECK_EQ_INT(m.count, 3);
    CHECK(!dev_xfermap_complete(&m));
    uint32_t s, n;
    CHECK(dev_xfermap_next_missing(&m, 0, 32, &s, &n) && s == 0 && n == 3);
    dev_xfermap_set(&m, 1);
    CHECK(dev_xfermap_next_missing(&m, 0, 32, &s, &n) && s == 0 && n == 1);
    CHECK(dev_xfermap_next_missing(&m, 1, 32, &s, &n) && s == 2 && n == 1);
    dev_xfermap_set(&m, 0);
    dev_xfermap_set(&m, 2);
    dev_xfermap_set(&m, 3);   /* beyond count: ignored */
    CHECK(dev_xfermap_complete(&m) && dev_xfermap_present(&m) == 3);
    CHECK(!dev_xfermap_next_missing(&m, 0, 32, &s, &n));
    dev_xfermap_free(&m);
    /* 64 chunks: runs are capped at max (xfer.chunk_req_max) */
    CHECK_EQ_INT(dev_xfermap_init(&m, sha, 65536, 1024), XFERMAP_OK);
    CHECK(dev_xfermap_next_missing(&m, 0, 32, &s, &n) && s == 0 && n == 32);
    CHECK(dev_xfermap_next_missing(&m, 32, 32, &s, &n) && s == 32 && n == 32);
    dev_xfermap_free(&m);
    CHECK_EQ_INT(dev_xfermap_init(&m, sha, 0, 1024), XFERMAP_EINVAL);
    CHECK_EQ_INT(dev_xfermap_init(&m, sha, 10, 768), XFERMAP_EINVAL);
    CHECK_EQ_INT(dev_xfermap_init(&m, sha, 10, 2048), XFERMAP_EINVAL);
}

static void test_persist(void)
{
    char path[400];
    snprintf(path, sizeof path, "%s/xfer_2.map", test_tmpdir());
    uint8_t sha[32], other[32];
    for (int i = 0; i < 32; i++) { sha[i] = (uint8_t)(i * 7); other[i] = sha[i]; }
    other[31] ^= 1;
    int bad = 0;
    for (int round = 0; round < 200; round++) {
        uint32_t size = 1 + (uint32_t)(xr() % 200000);
        uint32_t cs = 256u << (xr() % 3);
        dev_xfermap_t m, l;
        CHECK_EQ_INT(dev_xfermap_init(&m, sha, size, cs), XFERMAP_OK);
        for (uint32_t i = 0; i < m.count; i++)
            if (xr() & 1) dev_xfermap_set(&m, i);
        CHECK_EQ_INT(dev_xfermap_save(&m, path), 0);
        if (dev_xfermap_load(path, sha, size, &l) != XFERMAP_OK) { bad++; dev_xfermap_free(&m); continue; }
        bad += l.count != m.count || l.chunk_size != cs || l.size != size ||
               memcmp(l.bits, m.bits, (m.count + 7) / 8) != 0;
        dev_xfermap_free(&l);
        dev_xfermap_free(&m);
    }
    CHECK_EQ_INT(bad, 0);

    /* a map for another manifest (sha or size) is discarded */
    dev_xfermap_t m, l;
    dev_xfermap_init(&m, sha, 65536, 1024);
    dev_xfermap_set(&m, 5);
    CHECK_EQ_INT(dev_xfermap_save(&m, path), 0);
    CHECK_EQ_INT(dev_xfermap_load(path, other, 65536, &l), XFERMAP_MISMATCH);
    CHECK(l.bits == NULL);
    CHECK_EQ_INT(dev_xfermap_load(path, sha, 65535, &l), XFERMAP_MISMATCH);
    dev_xfermap_free(&m);

    /* corrupt or absent maps start from scratch */
    char bogus[400];
    snprintf(bogus, sizeof bogus, "%s/nope.map", test_tmpdir());
    CHECK_EQ_INT(dev_xfermap_load(bogus, sha, 65536, &l), XFERMAP_ABSENT);
    char hex[65];
    hex_encode(sha, 32, hex);
    const char *const corrupt[] = {
        "garbage\n",
        "ztp-xfermap 1\nsha256=%s\nsize=65536\nchunk_size=1024\ncount=64\nbitmap=00\n",                  /* short */
        "ztp-xfermap 1\nsha256=%s\nsize=65536\nchunk_size=1024\ncount=63\nbitmap=0000000000000000\n",    /* count */
        "ztp-xfermap 1\nsha256=%s\nsize=65536\nchunk_size=1000\ncount=64\nbitmap=0000000000000000\n",    /* cs */
        "ztp-xfermap 1\nsha256=%s\nsize=-5\nchunk_size=1024\ncount=64\nbitmap=0000000000000000\n",
        "ztp-xfermap 1\nsha256=%s\nsize=2310\nchunk_size=1024\ncount=3\nbitmap=f8\n",                     /* bits > count */
        "ztp-xfermap 1\nsha256=%s\nsize=65536\nchunk_size=1024\ncount=64\nbitmap=zz00000000000000\n",
    };
    for (size_t i = 0; i < sizeof corrupt / sizeof corrupt[0]; i++) {
        char text[512];
        snprintf(text, sizeof text, corrupt[i], hex);
        unlink(path);
        char bak[420];
        snprintf(bak, sizeof bak, "%s.bak", path);
        unlink(bak);
        FILE *f = fopen(path, "w");
        fputs(text, f);
        fclose(f);
        uint32_t sz = strstr(text, "size=2310") ? 2310 : 65536;
        int rc = dev_xfermap_load(path, sha, sz, &l);
        if (rc == XFERMAP_OK) {
            fprintf(stderr, "  accepted corrupt map #%zu\n", i);
            CHECK(!"corrupt map accepted");
            dev_xfermap_free(&l);
        }
    }
}

static void test_convert(void)
{
    uint8_t sha[32] = {9};
    int bad = 0;
    for (int round = 0; round < 500; round++) {
        uint32_t size = 1 + (uint32_t)(xr() % 300000);
        uint32_t from = (round % 3 == 2) ? 512 : 1024, to = (from == 512 || xr() % 2) ? 256 : 512;
        dev_xfermap_t m, o;
        dev_xfermap_init(&m, sha, size, from);
        for (uint32_t i = 0; i < m.count; i++)
            if (xr() % 3) dev_xfermap_set(&m, i);
        dev_xfermap_init(&o, sha, size, from);
        memcpy(o.bits, m.bits, (m.count + 7) / 8);
        if (dev_xfermap_convert(&m, to) != XFERMAP_OK) { bad++; continue; }
        uint32_t r = from / to;
        bad += m.chunk_size != to || m.count != (size + to - 1) / to;
        /* chunk i set at the old size <=> chunks i*r..i*r+r-1 (those that exist) set at the new size */
        for (uint32_t i = 0; i < o.count; i++)
            for (uint32_t j = 0; j < r && i * r + j < m.count; j++)
                bad += dev_xfermap_has(&o, i) != dev_xfermap_has(&m, i * r + j);
        /* byte coverage is identical */
        uint64_t bytes_o = 0, bytes_m = 0;
        for (uint32_t i = 0; i < o.count; i++)
            if (dev_xfermap_has(&o, i)) bytes_o += (i == o.count - 1) ? size - (uint64_t)i * from : from;
        for (uint32_t i = 0; i < m.count; i++)
            if (dev_xfermap_has(&m, i)) bytes_m += (i == m.count - 1) ? size - (uint64_t)i * to : to;
        bad += bytes_o != bytes_m;
        dev_xfermap_free(&m);
        dev_xfermap_free(&o);
    }
    CHECK_EQ_INT(bad, 0);

    /* the §5.8 example: the 3-chunk config becomes 10 chunks of 256 */
    dev_xfermap_t m;
    dev_xfermap_init(&m, sha, 2310, 1024);
    dev_xfermap_set(&m, 0);
    dev_xfermap_set(&m, 2);
    CHECK_EQ_INT(dev_xfermap_convert(&m, 256), XFERMAP_OK);
    CHECK_EQ_INT(m.count, 10);
    CHECK(dev_xfermap_has(&m, 3) && !dev_xfermap_has(&m, 4) && !dev_xfermap_has(&m, 7) && dev_xfermap_has(&m, 9));
    CHECK_EQ_INT(dev_xfermap_present(&m), 6);
    /* rejected: not smaller, not a valid size, non-power-of-two ratio */
    CHECK_EQ_INT(dev_xfermap_convert(&m, 256), XFERMAP_EINVAL);
    CHECK_EQ_INT(dev_xfermap_convert(&m, 512), XFERMAP_EINVAL);
    dev_xfermap_free(&m);
    dev_xfermap_init(&m, sha, 2310, 1024);
    CHECK_EQ_INT(dev_xfermap_convert(&m, 768), XFERMAP_EINVAL);
    CHECK_EQ_INT(dev_xfermap_convert(&m, 128), XFERMAP_EINVAL);
    CHECK_EQ_INT(m.chunk_size, 1024);   /* unchanged after a rejected conversion */
    dev_xfermap_free(&m);
}

static int check_file(const char *path, int expected, char *err, size_t n)
{
    char *text = NULL;
    size_t len = 0;
    if (read_file(path, 1 << 16, false, &text, &len) != 0) return 99;
    int rc = dev_config_check(text, len, expected, err, n);
    free(text);
    return rc;
}

static void test_config_check(void)
{
    char err[200];
    CHECK_EQ_INT(check_file("tests/conf/cfg_v3.json", 3, err, sizeof err), DEV_CFG_OK);
    CHECK_EQ_INT(check_file("tests/conf/cfg_missing_key.json", 3, err, sizeof err), DEV_CFG_MISSING);
    CHECK_STR(err, "config.json: missing key keepalive_interval");
    CHECK_EQ_INT(check_file("tests/conf/cfg_malformed.json", 3, err, sizeof err), DEV_CFG_MALFORMED);
    CHECK_EQ_INT(check_file("tests/conf/cfg_v3.json", 2, err, sizeof err), DEV_CFG_INVALID);
    CHECK(strstr(err, "config_version 3, assigned 2") != NULL);
    const char *t = "{\"interfaces\": {}, \"mtu\": \"1500\", \"keepalive_interval\": 20, \"config_version\": 3}";
    CHECK_EQ_INT(dev_config_check(t, strlen(t), 3, err, sizeof err), DEV_CFG_INVALID);
    t = "[1, 2]";
    CHECK_EQ_INT(dev_config_check(t, strlen(t), 3, err, sizeof err), DEV_CFG_MALFORMED);
    /* cfg_v3 is the §11.2 example: 2310 bytes, 3 chunks, and contains the PDF's keys verbatim */
    char *text = NULL;
    size_t len = 0;
    CHECK(read_file("tests/conf/cfg_v3.json", 1 << 16, false, &text, &len) == 0 && len == 2310);
    if (text) {
        CHECK(strstr(text, "\"wan\": \"eth0\"") && strstr(text, "\"lan\": \"eth1\"") && strstr(text, "\"mtu\": 1500") &&
              strstr(text, "\"keepalive_interval\": 20"));
        free(text);
    }
}

void suite_transfer(void)
{
    test_map_basics();
    test_persist();
    test_convert();
    test_config_check();
}
