#include "transfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "util.h"

#define MAP_MAGIC "ztp-xfermap 1"

static bool chunk_size_ok(uint32_t cs) { return cs >= 256 && cs <= 1024 && (cs & (cs - 1)) == 0; }
static uint32_t nbytes(uint32_t count) { return (count + 7) / 8; }

int dev_xfermap_init(dev_xfermap_t *m, const uint8_t sha256[32], uint32_t size, uint32_t chunk_size)
{
    memset(m, 0, sizeof *m);
    if (size == 0 || !chunk_size_ok(chunk_size)) return XFERMAP_EINVAL;
    memcpy(m->sha256, sha256, 32);
    m->size = size;
    m->chunk_size = chunk_size;
    m->count = (uint32_t)(((uint64_t)size + chunk_size - 1) / chunk_size);
    m->bits = calloc(nbytes(m->count), 1);
    return m->bits ? XFERMAP_OK : XFERMAP_EINVAL;
}

void dev_xfermap_free(dev_xfermap_t *m)
{
    free(m->bits);
    memset(m, 0, sizeof *m);
}

void dev_xfermap_set(dev_xfermap_t *m, uint32_t i)
{
    if (i < m->count) m->bits[i / 8] |= (uint8_t)(1u << (i % 8));
}

bool dev_xfermap_has(const dev_xfermap_t *m, uint32_t i)
{
    return i < m->count && (m->bits[i / 8] >> (i % 8)) & 1u;
}

uint32_t dev_xfermap_present(const dev_xfermap_t *m)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->count; i++) n += dev_xfermap_has(m, i);
    return n;
}

bool dev_xfermap_complete(const dev_xfermap_t *m) { return m->count && dev_xfermap_present(m) == m->count; }

bool dev_xfermap_next_missing(const dev_xfermap_t *m, uint32_t from, uint32_t max, uint32_t *start, uint32_t *num)
{
    uint32_t i = from;
    while (i < m->count && dev_xfermap_has(m, i)) i++;
    if (i >= m->count || max == 0) return false;
    uint32_t n = 0;
    while (i + n < m->count && n < max && !dev_xfermap_has(m, i + n)) n++;
    *start = i;
    *num = n;
    return true;
}

int dev_xfermap_save(const dev_xfermap_t *m, const char *path)
{
    size_t nb = nbytes(m->count);
    size_t cap = 256 + 2 * nb;
    char *buf = malloc(cap), sha[65];
    char *hex = malloc(2 * nb + 1);
    if (!buf || !hex) { free(buf); free(hex); return -1; }
    hex_encode(m->sha256, 32, sha);
    hex_encode(m->bits, nb, hex);
    int len = snprintf(buf, cap, MAP_MAGIC "\nsha256=%s\nsize=%u\nchunk_size=%u\ncount=%u\nbitmap=%s\n", sha,
                       (unsigned)m->size, (unsigned)m->chunk_size, (unsigned)m->count, hex);
    int rc = (len > 0 && (size_t)len < cap) ? atomic_write_file(path, buf, (size_t)len) : -1;
    free(buf);
    free(hex);
    return rc;
}

/* Find "key=" at the start of a line; returns the value (up to the newline, copied) or false. */
static bool field(const char *text, const char *key, char *out, size_t outsz)
{
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '\n');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= outsz) return false;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

static bool parse_u32(const char *s, uint32_t *v)
{
    char *end;
    if (!*s || *s == '-') return false;
    unsigned long x = strtoul(s, &end, 10);
    if (*end || x > 0xFFFFFFFFul) return false;
    *v = (uint32_t)x;
    return true;
}

int dev_xfermap_load(const char *path, const uint8_t expected_sha256[32], uint32_t expected_size, dev_xfermap_t *m)
{
    memset(m, 0, sizeof *m);
    char *text = NULL;
    size_t len = 0;
    if (read_file(path, 1u << 20, true, &text, &len) != 0) return XFERMAP_ABSENT;
    int rc = XFERMAP_ABSENT;
    char v[80], shahex[80];
    uint32_t size, cs, count;
    uint8_t sha[32];
    if (strncmp(text, MAP_MAGIC "\n", sizeof MAP_MAGIC) != 0) goto out;
    if (!field(text, "sha256", shahex, sizeof shahex) || hex_decode(shahex, sha, 32) != 0) goto out;
    if (!field(text, "size", v, sizeof v) || !parse_u32(v, &size)) goto out;
    if (!field(text, "chunk_size", v, sizeof v) || !parse_u32(v, &cs)) goto out;
    if (!field(text, "count", v, sizeof v) || !parse_u32(v, &count)) goto out;
    if (!ct_memeq(sha, expected_sha256, 32) || size != expected_size) { rc = XFERMAP_MISMATCH; goto out; }
    if (dev_xfermap_init(m, sha, size, cs) != XFERMAP_OK) goto out;
    if (m->count != count) goto bad;
    {
        size_t nb = nbytes(count);
        char *hex = malloc(2 * nb + 2);
        if (!hex) goto bad;
        bool ok = field(text, "bitmap", hex, 2 * nb + 2) && hex_decode(hex, m->bits, nb) == 0;
        free(hex);
        if (!ok) goto bad;
        if (count % 8 && (m->bits[nb - 1] >> (count % 8))) goto bad;   /* bits beyond count must be 0 */
    }
    rc = XFERMAP_OK;
    goto out;
bad:
    dev_xfermap_free(m);
out:
    free(text);
    return rc;
}

int dev_xfermap_convert(dev_xfermap_t *m, uint32_t new_cs)
{
    if (!chunk_size_ok(new_cs) || new_cs >= m->chunk_size || m->chunk_size % new_cs) return XFERMAP_EINVAL;
    uint32_t r = m->chunk_size / new_cs;
    if (r & (r - 1)) return XFERMAP_EINVAL;
    dev_xfermap_t n;
    if (dev_xfermap_init(&n, m->sha256, m->size, new_cs) != XFERMAP_OK) return XFERMAP_EINVAL;
    for (uint32_t i = 0; i < m->count; i++)
        if (dev_xfermap_has(m, i))
            for (uint32_t j = 0; j < r; j++) dev_xfermap_set(&n, i * r + j);   /* set() ignores >= count */
    free(m->bits);
    *m = n;
    return XFERMAP_OK;
}
