/* TC-UNIT-01 (SHA-256 KAT), TC-UNIT-02 (HMAC-SHA256 RFC 4231), TC-UNIT-05 (ct_memeq),
 * TC-UNIT-16 (key-derivation KAT; reference computed with Python hmac and openssl dgst). */
#include <stdlib.h>

#include "crypto.h"
#include "test.h"

static void sha_hex(const void *m, size_t n, char out[65])
{
    uint8_t d[32];
    sha256(m, n, d);
    hex_of(d, 32, out);
}

static void test_sha256(void)
{
    char h[65];
    sha_hex("", 0, h);
    CHECK_STR(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha_hex("abc", 3, h);
    CHECK_STR(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_hex(m448, strlen(m448), h);
    CHECK_STR(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* 1,000,000 x 'a', streamed in uneven pieces across block boundaries */
    sha256_ctx c;
    sha256_init(&c);
    char buf[1000];
    memset(buf, 'a', sizeof buf);
    size_t left = 1000000, step = 1;
    while (left) {
        size_t n = step < left ? step : left;
        if (n > sizeof buf) n = sizeof buf;
        sha256_update(&c, buf, n);
        left -= n;
        step = step * 7 % 997 + 1;
    }
    uint8_t d[32];
    sha256_final(&c, d);
    hex_of(d, 32, h);
    CHECK_STR(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    /* streaming == one-shot for every split point of a 200-byte message */
    uint8_t msg[200], one[32], two[32];
    for (int i = 0; i < 200; i++) msg[i] = (uint8_t)(i * 31 + 7);
    sha256(msg, sizeof msg, one);
    int mismatches = 0;
    for (size_t split = 0; split <= sizeof msg; split++) {
        sha256_init(&c);
        sha256_update(&c, msg, split);
        sha256_update(&c, msg + split, sizeof msg - split);
        sha256_final(&c, two);
        mismatches += memcmp(one, two, 32) != 0;
    }
    CHECK_EQ_INT(mismatches, 0);
}

static void test_hmac(void)
{
    struct { uint8_t key[131]; size_t klen; const char *msg; uint8_t mfill; size_t mlen; const char *mac; } v[] = {
        {{0}, 20, "Hi There", 0, 0, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {"Jefe", 4, "what do ya want for nothing?", 0, 0, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        {{0}, 20, NULL, 0xdd, 50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
        {{0}, 25, NULL, 0xcd, 50, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
        {{0}, 20, "Test With Truncation", 0, 0, "a3b6167473100ee06e0c796c2955552b"},
        {{0}, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 0, 0,
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
        {{0}, 131,
         "This is a test using a larger than block-size key and a larger than block-size data. The key needs "
         "to be hashed before being used by the HMAC algorithm.",
         0, 0, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"},
    };
    memset(v[0].key, 0x0b, 20);
    memset(v[2].key, 0xaa, 20);
    for (int i = 0; i < 25; i++) v[3].key[i] = (uint8_t)(i + 1);
    memset(v[4].key, 0x0c, 20);
    memset(v[5].key, 0xaa, 131);
    memset(v[6].key, 0xaa, 131);
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t m[64], out[32];
        const void *msg = v[i].msg;
        size_t mlen = v[i].msg ? strlen(v[i].msg) : v[i].mlen;
        if (!v[i].msg) {
            memset(m, v[i].mfill, v[i].mlen);
            msg = m;
        }
        hmac_sha256(v[i].key, v[i].klen, msg, mlen, out);
        char h[65];
        size_t want = strlen(v[i].mac) / 2;   /* case 5: truncated to 128 bits */
        hex_of(out, want, h);
        CHECK_STR(h, v[i].mac);
    }
}

static void test_ct_memeq(void)
{
    uint8_t a[64], b[64];
    for (int i = 0; i < 64; i++) a[i] = b[i] = (uint8_t)i;
    CHECK(ct_memeq(a, b, 64) == 1);
    CHECK(ct_memeq(a, b, 0) == 1);
    for (int pos = 0; pos < 64; pos++)
        for (int bit = 0; bit < 8; bit++) {
            b[pos] ^= (uint8_t)(1u << bit);
            if (ct_memeq(a, b, 64) != 0) CHECK(!"single-bit difference not detected");
            b[pos] ^= (uint8_t)(1u << bit);
        }
    CHECK(ct_memeq(a, b, 64) == 1);
}

static void test_kdf(void)
{
    uint8_t kf[32], km[32], kb[32], kd[32], ks[32];
    for (int i = 0; i < 32; i++) { kf[i] = (uint8_t)i; km[i] = (uint8_t)(32 + i); }
    char h[65];
    ztp_derive_kboot(kf, kb);
    hex_of(kb, 32, h);
    CHECK_STR(h, "4ef8c05a73b86027e34f32495516fab2ef8f60dd3e1d9d665cc067bdd6c39951");
    ztp_derive_kdev(km, "ZTP-93AF21", kd);
    hex_of(kd, 32, h);
    CHECK_STR(h, "98d86c8bba15df5b312b24760ba82932571020a797bdc301c0eb2794299bdba1");
    ztp_derive_ks(kd, 0x0102030405060708ull, 0x1112131415161718ull, 7421, 3, ks);
    hex_of(ks, 32, h);
    CHECK_STR(h, "036d9a72318feb0c9c2f37376aa5466cd8d35552671a8eb8d8064688f3f73f20");
    /* domain separation: different epoch or serial -> different key */
    uint8_t ks2[32], kd2[32];
    ztp_derive_ks(kd, 0x0102030405060708ull, 0x1112131415161718ull, 7421, 4, ks2);
    CHECK(memcmp(ks, ks2, 32) != 0);
    ztp_derive_kdev(km, "ZTP-93AF22", kd2);
    CHECK(memcmp(kd, kd2, 32) != 0);
}

static void test_secure_zero(void)
{
    uint8_t k[32];
    memset(k, 0x5a, sizeof k);
    secure_zero(k, sizeof k);
    int nz = 0;
    for (size_t i = 0; i < sizeof k; i++) nz |= k[i];
    CHECK(nz == 0);
}

void suite_crypto(void)
{
    test_sha256();
    test_hmac();
    test_ct_memeq();
    test_kdf();
    test_secure_zero();
}
