/* Server artifacts (DESIGN §11.1, §11.2, §14.3): every config and firmware file the policy refers
 * to is opened, sized and hashed at startup, then read on demand (pread) one chunk at a time. */
#ifndef SRV_ARTIFACTS_H
#define SRV_ARTIFACTS_H

#include <stddef.h>
#include <stdint.h>

#include "policy.h"

typedef struct {
    char     path[POL_PATH];
    uint8_t  file_id;        /* 1.. (FILE_ID TLV) */
    uint8_t  kind;           /* FK_CONFIG or FK_FIRMWARE */
    char     version[9];     /* firmware version, or "v<config_version>" */
    uint32_t size;
    uint8_t  sha256[32];
    int      fd;
} srv_artifact_t;

typedef struct {
    srv_artifact_t *a;
    size_t          n;
} srv_artifacts_t;

/* Load every artifact referenced by the policy. 0, or EX_CONFIG_CODE with err. */
int  srv_artifacts_load(srv_artifacts_t *as, const srv_policy_t *p, char *err, size_t errsz);
void srv_artifacts_free(srv_artifacts_t *as);
const srv_artifact_t *srv_artifact_for(const srv_artifacts_t *as, uint8_t kind, const char *path, const char *version);
const srv_artifact_t *srv_artifact_by_id(const srv_artifacts_t *as, uint8_t file_id);
/* Read chunk idx at chunk_size into out; returns the byte count, or -1. */
int  srv_artifact_read(const srv_artifact_t *a, uint32_t idx, uint32_t chunk_size, uint8_t *out);

#endif
