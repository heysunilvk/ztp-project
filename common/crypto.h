/* SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104), ZTP key derivation (DESIGN §10.1),
 * and constant-time helpers. No external dependencies. */
#ifndef ZTP_CRYPTO_H
#define ZTP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32u
#define SHA256_BLOCK_LEN  64u
#define ZTP_KEY_LEN       32u

typedef struct {
    uint32_t h[8];
    uint64_t total_len;          /* bytes hashed so far */
    uint8_t  buf[SHA256_BLOCK_LEN];
    size_t   buf_len;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

typedef struct {
    sha256_ctx inner, outer;
} hmac_sha256_ctx;

void hmac_sha256_init(hmac_sha256_ctx *c, const uint8_t *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_ctx *c, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]);
void hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len,
                 uint8_t out[SHA256_DIGEST_LEN]);

/* Key derivation labels (fixed protocol invariants, Appendix B.1). */
#define ZTP_LABEL_BOOT "ZTP-BOOT-v1"
#define ZTP_LABEL_DEV  "ZTP-DEV-v1"
#define ZTP_LABEL_SK   "ZTP-SK-v1"

/* K_boot = HMAC(K_fleet, "ZTP-BOOT-v1") */
void ztp_derive_kboot(const uint8_t k_fleet[ZTP_KEY_LEN], uint8_t out[ZTP_KEY_LEN]);
/* K_dev = HMAC(K_mfg, "ZTP-DEV-v1" || serial) */
void ztp_derive_kdev(const uint8_t k_mfg[ZTP_KEY_LEN], const char *serial, uint8_t out[ZTP_KEY_LEN]);
/* K_s = HMAC(K_dev, "ZTP-SK-v1" || be64(Nd) || be64(Ns) || be32(session_id) || be16(epoch)) */
void ztp_derive_ks(const uint8_t k_dev[ZTP_KEY_LEN], uint64_t nd, uint64_t ns,
                   uint32_t session_id, uint16_t epoch, uint8_t out[ZTP_KEY_LEN]);

/* Constant-time equality: returns 1 if equal, 0 otherwise. No early exit. */
int ct_memeq(const void *a, const void *b, size_t n);
/* Wipe memory in a way the compiler may not elide. */
void secure_zero(void *p, size_t n);

#endif
