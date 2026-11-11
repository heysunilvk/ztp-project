/* Atomic file write, hex, version comparison (DESIGN §4.2). */
#ifndef ZTP_UTIL_H
#define ZTP_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Test-only fault injection points for TC-UNIT-13. */
enum atomic_fault { AF_NONE = 0, AF_AFTER_WRITE, AF_AFTER_FSYNC, AF_BEFORE_RENAME };
extern int util_atomic_fault;   /* set by tests; the write then fails at that point */

/* Write data to path atomically: write path.tmp, fsync, keep the old file as path.bak,
 * rename path.tmp -> path, fsync the directory. Returns 0 or -1 (errno set). */
int atomic_write_file(const char *path, const void *data, size_t len);

/* Read a whole file (≤ max bytes) into a malloc'd, NUL-terminated buffer. If path is missing
 * or unreadable and path.bak exists, falls back to path.bak when use_bak is true.
 * Returns 0, or -1 (errno set; E2BIG if larger than max). */
int read_file(const char *path, size_t max, bool use_bak, char **out, size_t *out_len);

/* Lowercase hex encode (out must hold 2n+1). */
void hex_encode(const uint8_t *in, size_t n, char *out);
/* Decode exactly n bytes from 2n hex chars. Returns 0, or -1 on malformed input. */
int  hex_decode(const char *in, uint8_t *out, size_t n);

/* Parse a version string "vMAJOR.MINOR[.PATCH]" (the leading 'v' is required; components are
 * decimal, at most 5 digits, no leading '+'/'-'). Returns 0 or -1. */
int version_parse(const char *s, uint32_t parts[3]);
/* Compare two versions: sets *cmp to -1/0/1 and returns 0, or -1 if either is malformed. */
int version_cmp(const char *a, const char *b, int *cmp);

/* Bounded string copy that always NUL-terminates; returns true if not truncated. */
bool str_copy(char *dst, size_t dst_sz, const char *src);

#endif
