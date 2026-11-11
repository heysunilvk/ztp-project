/* Server-side authentication (DESIGN §10). M1/M3 part: loading keys.json (§25.6):
 * {"schema_version":1, "fleet":"<hex32>", "models":{"<model>":"<hex32>", ...}}, mode 0600. */
#ifndef SRV_AUTH_H
#define SRV_AUTH_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

typedef struct {
    char    model[17];
    uint8_t k_mfg[ZTP_KEY_LEN];
} srv_model_key_t;

typedef struct {
    uint8_t          k_fleet[ZTP_KEY_LEN];
    uint8_t          k_boot[ZTP_KEY_LEN];   /* derived from k_fleet */
    srv_model_key_t *models;
    size_t           nmodels;
} srv_keys_t;

/* Load and check keys.json. Refuses a file readable or writable by group/others.
 * Returns 0, or EX_CONFIG_CODE (78) with err set. */
int  srv_keys_load(const char *path, srv_keys_t *k, char *err, size_t errsz);
void srv_keys_free(srv_keys_t *k);   /* wipes key material */
/* K_mfg for a model, or NULL (UNKNOWN_MODEL). */
const uint8_t *srv_keys_model(const srv_keys_t *k, const char *model);

#endif
