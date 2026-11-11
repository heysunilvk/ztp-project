/* Device file transfer (DESIGN §11). M1 part: the chunk bitmap persisted as xfer_<id>.map
 * (§11.4) and its conversion when the chunk size steps down (§5.8 step 4). */
#ifndef DEV_TRANSFER_H
#define DEV_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  sha256[32];   /* manifest SHA-256 the map belongs to */
    uint32_t size;         /* file size in bytes (> 0) */
    uint32_t chunk_size;   /* power of two, 256..1024 */
    uint32_t count;        /* ceil(size / chunk_size) */
    uint8_t *bits;         /* bit i (LSB first within a byte) = chunk i present */
} dev_xfermap_t;

#define XFERMAP_OK        0
#define XFERMAP_ABSENT   -1   /* no map, unreadable or malformed: start from scratch */
#define XFERMAP_MISMATCH -2   /* belongs to another manifest: discard */
#define XFERMAP_EINVAL   -3

int  dev_xfermap_init(dev_xfermap_t *m, const uint8_t sha256[32], uint32_t size, uint32_t chunk_size);
void dev_xfermap_free(dev_xfermap_t *m);
void dev_xfermap_set(dev_xfermap_t *m, uint32_t i);
bool dev_xfermap_has(const dev_xfermap_t *m, uint32_t i);
uint32_t dev_xfermap_present(const dev_xfermap_t *m);
bool dev_xfermap_complete(const dev_xfermap_t *m);
/* First contiguous run of missing chunks at or after `from`, at most `max` long.
 * Returns true and sets *start and *num, or false if nothing is missing from `from` on. */
bool dev_xfermap_next_missing(const dev_xfermap_t *m, uint32_t from, uint32_t max, uint32_t *start, uint32_t *num);

/* Atomic save (util atomic_write_file). The caller must have made the chunks durable first
 * (§11.4 durability order). Returns 0 or -1. */
int dev_xfermap_save(const dev_xfermap_t *m, const char *path);
/* Load and check against the manifest. On anything but XFERMAP_OK, *m is left empty. */
int dev_xfermap_load(const char *path, const uint8_t expected_sha256[32], uint32_t expected_size,
                     dev_xfermap_t *m);
/* Convert to a smaller chunk size (§5.8): chunk i at the old size becomes chunks i*r..i*r+r-1.
 * The ratio must be a power of two >= 2. Returns XFERMAP_OK or XFERMAP_EINVAL. */
int dev_xfermap_convert(dev_xfermap_t *m, uint32_t new_chunk_size);

#endif
