/* Device validation checks (DESIGN §13). M1 part: the provisioned-configuration check. */
#ifndef DEV_VALIDATE_H
#define DEV_VALIDATE_H

#include <stddef.h>

#define DEV_CFG_OK        0
#define DEV_CFG_MALFORMED -1   /* not strict JSON / not an object */
#define DEV_CFG_MISSING   -2   /* a required key is absent (named in err) */
#define DEV_CFG_INVALID   -3   /* wrong type, or config_version differs from the assigned one */

/* Required keys: interfaces (object), mtu (integer), keepalive_interval (integer),
 * config_version (integer, equal to expected_version). The SHA-256 check against the stored
 * cfg_sha256 is done by the caller. */
int dev_config_check(const char *text, size_t len, int expected_version, char *err, size_t errsz);

#endif
