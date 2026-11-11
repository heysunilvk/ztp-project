/* mkartifacts: deterministic mock firmware images and provisioned configs (DESIGN §11.1), and
 * demo keys files.
 *
 *   mkartifacts [--out DIR]                 artifacts/ for conf/policy.json:
 *                                            fw_EDGE-GW-100_v1.1.bin (65,536 B), fw_EDGE-GW-050_v1.0.bin
 *                                            (49,152 B), cfg_v3.json (2,310 B), cfg_v2.json
 *   mkartifacts --test-set DIR              QA §4.2 boundary files: fw_tiny (1 B), fw_1023, fw_1024, fw_1025,
 *                                            fw_64k, fw_1m (raw deterministic bytes, no header)
 *   mkartifacts --fw FILE MODEL VERSION SIZE  one firmware image with a valid header, of SIZE bytes
 *   mkartifacts --keys FILE MODEL...        keys.json with the demo-derived K_fleet and K_mfg per model
 *                                            (the same derivation as device/keys.c), mode 0600
 *
 * Firmware layout: a 64-byte header, then pseudo-random bytes seeded by model and version:
 *   magic "ZTPFW\0" (6) | fmt u8 = 1 | model[16] | version[8] | payload_len u32 (big-endian) | zero padding */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crypto.h"
#include "dev.h"
#include "protocol.h"
#include "util.h"

#define FW_HDR_LEN 64

static int write_fw_path(const char *path, const char *model, const char *version, size_t total);

static int write_fw(const char *dir, const char *model, const char *version, size_t total)
{
    char path[512];
    snprintf(path, sizeof path, "%s/fw_%s_%s.bin", dir, model, version);
    return write_fw_path(path, model, version, total);
}

static int write_fw_path(const char *path, const char *model, const char *version, size_t total)
{
    if (total < FW_HDR_LEN) { fprintf(stderr, "mkartifacts: %s: a firmware image needs at least %d bytes\n", path, FW_HDR_LEN); return -1; }
    uint8_t *b = calloc(1, total);
    if (!b) return -1;
    memcpy(b, "ZTPFW\0", 6);
    b[6] = 1;
    size_t ml = strlen(model), vl = strlen(version);
    memcpy(b + 7, model, ml < 16 ? ml : 16);     /* zero-padded, not NUL-terminated when full */
    memcpy(b + 23, version, vl < 8 ? vl : 8);
    be32_put(b + 31, (uint32_t)(total - FW_HDR_LEN));
    /* payload: SHA-256 in counter mode over "model|version", so every build is identical */
    char seed[64];
    snprintf(seed, sizeof seed, "%s|%s", model, version);
    for (size_t off = FW_HDR_LEN, ctr = 0; off < total; ctr++) {
        uint8_t blk[SHA256_DIGEST_LEN], in[96];
        int n = snprintf((char *)in, sizeof in, "%s|%zu", seed, ctr);
        sha256(in, (size_t)n, blk);
        size_t k = total - off < sizeof blk ? total - off : sizeof blk;
        memcpy(b + off, blk, k);
        off += k;
    }
    int rc = atomic_write_file(path, b, total);
    free(b);
    printf("mkartifacts: %s (%zu bytes)\n", path, total);
    return rc;
}

/* A provisioned config: a strict superset of the PDF's example (DESIGN §11.1), padded to an exact size. */
static int write_cfg(const char *dir, int version, size_t target, int acl_entries)
{
    char path[512], body[8192], acl[6144] = "", banner[1024];
    snprintf(path, sizeof path, "%s/cfg_v%d.json", dir, version);
    size_t al = 0;
    for (int i = 0; i < acl_entries; i++)
        al += (size_t)snprintf(acl + al, sizeof acl - al, "%s    \"permit udp 10.20.%d.%d/32 any eq %d\"", i ? ",\n" : "",
                               i / 250, i % 250 + 1, 5000 + i);
    const char *fmt = "{\n  \"config_version\": %d,\n  \"tenant\": \"cruise_ops\",\n"
                      "  \"interfaces\": {\n    \"wan\": \"eth0\",\n    \"lan\": \"eth1\"\n  },\n"
                      "  \"mtu\": 1500,\n  \"keepalive_interval\": 20,\n  \"ntp\": [\n    \"10.10.10.1\"\n  ],\n"
                      "  \"syslog\": \"10.10.10.2:514\",\n  \"vlans\": [\n    { \"id\": 10, \"name\": \"mgmt\" },\n"
                      "    { \"id\": 20, \"name\": \"data\" }\n  ],\n  \"banner\": \"%s\",\n  \"acl\": [\n%s\n  ]\n}\n";
    int n = snprintf(body, sizeof body, fmt, version, "", acl);
    if (n < 0 || (size_t)n > target) { fprintf(stderr, "mkartifacts: cfg v%d base is %d B > %zu\n", version, n, target); return -1; }
    static const char words[] = "Authorized access only. This edge gateway is managed by cruise_ops; all sessions are "
                                "logged and monitored. Disconnect now if you are not an authorized operator. ";
    size_t need = target - (size_t)n;
    for (size_t i = 0; i < need && i < sizeof banner - 1; i++) banner[i] = words[i % (sizeof words - 1)];
    banner[need < sizeof banner ? need : sizeof banner - 1] = '\0';
    n = snprintf(body, sizeof body, fmt, version, banner, acl);
    int rc = atomic_write_file(path, body, (size_t)n);
    printf("mkartifacts: %s (%d bytes)\n", path, n);
    return rc;
}

static int write_keys(const char *path, int nmodels, char **models)
{
    uint8_t kf[32], km[32];
    char hex[65], buf[8192];
    dev_factory_keys(nmodels ? models[0] : "", kf, km);
    hex_encode(kf, 32, hex);
    size_t n = (size_t)snprintf(buf, sizeof buf,
                                "{\n  \"schema_version\": 1,\n  \"_comment\": \"DEMO KEYS, not secret: derived as in device/keys.c "
                                "(SHA-256 of a label and the model). Generated by mkartifacts --keys.\",\n  \"fleet\": \"%s\",\n"
                                "  \"models\": {",
                                hex);
    for (int i = 0; i < nmodels; i++) {
        dev_factory_keys(models[i], kf, km);
        hex_encode(km, 32, hex);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s\n    \"%s\": \"%s\"", i ? "," : "", models[i], hex);
    }
    n += (size_t)snprintf(buf + n, sizeof buf - n, "\n  }\n}\n");
    if (atomic_write_file(path, buf, n) != 0) { perror(path); return 1; }
    chmod(path, 0600);
    printf("mkartifacts: %s (%d models)\n", path, nmodels);
    return 0;
}

static int write_raw(const char *dir, const char *name, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    uint8_t *b = malloc(n ? n : 1);
    if (!b) return -1;
    for (size_t off = 0, ctr = 0; off < n; ctr++) {
        uint8_t blk[SHA256_DIGEST_LEN], in[64];
        int k = snprintf((char *)in, sizeof in, "%s|%zu", name, ctr);
        sha256(in, (size_t)k, blk);
        size_t m = n - off < sizeof blk ? n - off : sizeof blk;
        memcpy(b + off, blk, m);
        off += m;
    }
    int rc = atomic_write_file(path, b, n);
    free(b);
    printf("mkartifacts: %s (%zu bytes)\n", path, n);
    return rc;
}

int main(int argc, char **argv)
{
    const char *out = "artifacts";
    if (argc >= 3 && !strcmp(argv[1], "--keys")) return write_keys(argv[2], argc - 3, argv + 3);
    if (argc == 6 && !strcmp(argv[1], "--fw")) return write_fw_path(argv[2], argv[3], argv[4], (size_t)atol(argv[5])) ? 1 : 0;
    if (argc == 3 && !strcmp(argv[1], "--test-set")) {
        if (mkdir(argv[2], 0755) != 0 && errno != EEXIST) { perror(argv[2]); return 1; }
        static const struct { const char *name; size_t n; } T[] = {
            {"fw_tiny", 1}, {"fw_1023", 1023}, {"fw_1024", 1024}, {"fw_1025", 1025}, {"fw_64k", 65536}, {"fw_1m", 1048576}};
        int rc = 0;
        for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) rc |= write_raw(argv[2], T[i].name, T[i].n);
        return rc ? 1 : 0;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else { fprintf(stderr, "usage: mkartifacts [--out DIR] | --keys FILE MODEL...\n"); return 2; }
    }
    if (mkdir(out, 0755) != 0 && errno != EEXIST) { perror(out); return 1; }
    int rc = 0;
    rc |= write_fw(out, "EDGE-GW-100", "v1.1", 65536);
    rc |= write_fw(out, "EDGE-GW-050", "v1.0", 49152);
    rc |= write_cfg(out, 3, 2310, 45);
    rc |= write_cfg(out, 2, 1800, 31);
    return rc ? 1 : 0;
}
