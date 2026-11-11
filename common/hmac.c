/* HMAC-SHA256 (RFC 2104), ZTP key derivation (DESIGN §10.1), constant-time helpers. */
#include "crypto.h"

#include <string.h>

void hmac_sha256_init(hmac_sha256_ctx *c, const uint8_t *key, size_t key_len)
{
    uint8_t k[SHA256_BLOCK_LEN] = {0}, pad[SHA256_BLOCK_LEN];
    if (key_len > SHA256_BLOCK_LEN) sha256(key, key_len, k);
    else if (key_len) memcpy(k, key, key_len);

    for (size_t i = 0; i < SHA256_BLOCK_LEN; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c->inner);
    sha256_update(&c->inner, pad, sizeof pad);
    for (size_t i = 0; i < SHA256_BLOCK_LEN; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c->outer);
    sha256_update(&c->outer, pad, sizeof pad);
    secure_zero(k, sizeof k);
    secure_zero(pad, sizeof pad);
}

void hmac_sha256_update(hmac_sha256_ctx *c, const void *data, size_t len)
{
    sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN])
{
    uint8_t ih[SHA256_DIGEST_LEN];
    sha256_final(&c->inner, ih);
    sha256_update(&c->outer, ih, sizeof ih);
    sha256_final(&c->outer, out);
    secure_zero(ih, sizeof ih);
}

void hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len,
                 uint8_t out[SHA256_DIGEST_LEN])
{
    hmac_sha256_ctx c;
    hmac_sha256_init(&c, key, key_len);
    hmac_sha256_update(&c, data, len);
    hmac_sha256_final(&c, out);
}

void ztp_derive_kboot(const uint8_t k_fleet[ZTP_KEY_LEN], uint8_t out[ZTP_KEY_LEN])
{
    hmac_sha256(k_fleet, ZTP_KEY_LEN, ZTP_LABEL_BOOT, strlen(ZTP_LABEL_BOOT), out);
}

void ztp_derive_kdev(const uint8_t k_mfg[ZTP_KEY_LEN], const char *serial, uint8_t out[ZTP_KEY_LEN])
{
    hmac_sha256_ctx c;
    hmac_sha256_init(&c, k_mfg, ZTP_KEY_LEN);
    hmac_sha256_update(&c, ZTP_LABEL_DEV, strlen(ZTP_LABEL_DEV));
    hmac_sha256_update(&c, serial, strlen(serial));
    hmac_sha256_final(&c, out);
}

void ztp_derive_ks(const uint8_t k_dev[ZTP_KEY_LEN], uint64_t nd, uint64_t ns,
                   uint32_t session_id, uint16_t epoch, uint8_t out[ZTP_KEY_LEN])
{
    uint8_t m[8 + 8 + 4 + 2];
    for (int i = 0; i < 8; i++) m[i] = (uint8_t)(nd >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) m[8 + i] = (uint8_t)(ns >> (56 - 8 * i));
    for (int i = 0; i < 4; i++) m[16 + i] = (uint8_t)(session_id >> (24 - 8 * i));
    m[20] = (uint8_t)(epoch >> 8);
    m[21] = (uint8_t)epoch;

    hmac_sha256_ctx c;
    hmac_sha256_init(&c, k_dev, ZTP_KEY_LEN);
    hmac_sha256_update(&c, ZTP_LABEL_SK, strlen(ZTP_LABEL_SK));
    hmac_sha256_update(&c, m, sizeof m);
    hmac_sha256_final(&c, out);
}

int ct_memeq(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    /* map 0 -> 1, non-zero -> 0 without a data-dependent branch */
    return (int)(1u & (((unsigned)d - 1u) >> 8));
}

void secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}
