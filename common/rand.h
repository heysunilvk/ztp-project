/* CSPRNG wrapper: arc4random_buf on macOS/BSD, getrandom()//dev/urandom on Linux.
 * Used for nonces and session ids only; netsim uses its own seeded PRNG (DESIGN §16.1). */
#ifndef ZTP_RAND_H
#define ZTP_RAND_H

#include <stddef.h>
#include <stdint.h>

int      rand_bytes(void *buf, size_t n);   /* 0 on success, -1 on failure */
uint64_t rand_u64(void);                    /* aborts if the CSPRNG is unavailable */
uint32_t rand_u32_nonzero(void);

#endif
