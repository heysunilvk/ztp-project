#include "artifacts.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "crypto.h"
#include "log.h"
#include "protocol.h"
#include "util.h"

static int add(srv_artifacts_t *as, uint8_t kind, const char *path, const char *version, char *err, size_t errsz)
{
    if (srv_artifact_for(as, kind, path, version)) return 0;
    if (as->n >= 250) {
        snprintf(err, errsz, "artifacts: more than 250 distinct artifacts");
        return EX_CONFIG_CODE;
    }
    srv_artifact_t *a = &as->a[as->n];
    memset(a, 0, sizeof *a);
    a->fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (a->fd < 0 || fstat(a->fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 0x7FFFFFFF) {
        snprintf(err, errsz, "artifacts: %s: %s", path, a->fd < 0 ? strerror(errno) : "not a regular, non-empty file");
        if (a->fd >= 0) close(a->fd);
        return EX_CONFIG_CODE;
    }
    snprintf(a->path, sizeof a->path, "%s", path);
    snprintf(a->version, sizeof a->version, "%s", version);
    a->kind = kind;
    a->size = (uint32_t)st.st_size;
    a->file_id = (uint8_t)(as->n + 1);
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    for (off_t off = 0; off < st.st_size;) {
        ssize_t r = pread(a->fd, buf, sizeof buf, off);
        if (r <= 0) {
            snprintf(err, errsz, "artifacts: %s: read error", path);
            close(a->fd);
            return EX_CONFIG_CODE;
        }
        sha256_update(&c, buf, (size_t)r);
        off += r;
    }
    sha256_final(&c, a->sha256);
    char hex[65];
    hex_encode(a->sha256, 32, hex);
    LOGI(T_PROVISION, "Artifact %u: %s %s, %u bytes, sha256=%.16s…", (unsigned)a->file_id,
         kind == FK_CONFIG ? "config" : "firmware", version, (unsigned)a->size, hex);
    as->n++;
    return 0;
}

static int add_entry(srv_artifacts_t *as, const srv_pol_entry_t *e, char *err, size_t errsz)
{
    int rc;
    if (e->has_config_file) {
        char v[9];
        snprintf(v, sizeof v, "v%d", e->has_config_version ? e->config_version : 0);
        if ((rc = add(as, FK_CONFIG, e->config_file, v, err, errsz))) return rc;
    }
    if (e->has_firmware_file && (rc = add(as, FK_FIRMWARE, e->firmware_file, e->has_firmware ? e->firmware : "", err, errsz)))
        return rc;
    return 0;
}

int srv_artifacts_load(srv_artifacts_t *as, const srv_policy_t *p, char *err, size_t errsz)
{
    memset(as, 0, sizeof *as);
    as->a = calloc(250, sizeof *as->a);
    if (!as->a) { snprintf(err, errsz, "artifacts: out of memory"); return EX_CONFIG_CODE; }
    int rc = 0;
    if (p->default_assign) rc = add_entry(as, &p->dflt, err, errsz);
    for (size_t i = 0; !rc && i < p->nmodels; i++) rc = add_entry(as, &p->models[i], err, errsz);
    for (size_t i = 0; !rc && i < p->nserials; i++) rc = add_entry(as, &p->serials[i], err, errsz);
    return rc;
}

void srv_artifacts_free(srv_artifacts_t *as)
{
    for (size_t i = 0; i < as->n; i++) close(as->a[i].fd);
    free(as->a);
    memset(as, 0, sizeof *as);
}

const srv_artifact_t *srv_artifact_for(const srv_artifacts_t *as, uint8_t kind, const char *path, const char *version)
{
    for (size_t i = 0; i < as->n; i++)
        if (as->a[i].kind == kind && !strcmp(as->a[i].path, path) && !strcmp(as->a[i].version, version)) return &as->a[i];
    return NULL;
}

const srv_artifact_t *srv_artifact_by_id(const srv_artifacts_t *as, uint8_t id)
{
    return id >= 1 && id <= as->n ? &as->a[id - 1] : NULL;
}

int srv_artifact_read(const srv_artifact_t *a, uint32_t idx, uint32_t chunk_size, uint8_t *out)
{
    uint64_t off = (uint64_t)idx * chunk_size;
    if (off >= a->size) return -1;
    size_t n = a->size - off < chunk_size ? (size_t)(a->size - off) : chunk_size;
    ssize_t r = pread(a->fd, out, n, (off_t)off);
    return r == (ssize_t)n ? (int)n : -1;
}
