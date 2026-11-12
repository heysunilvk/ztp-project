/* In-process datagram fuzzer, the server test hook --self-fuzz N (QA TC-SRV-07). */
#ifndef SRV_FUZZ_H
#define SRV_FUZZ_H

#include <stdint.h>

#include "srv.h"

/* Feed n generated datagrams to the handlers on a virtual clock. Returns 0, or 1 if an invariant broke. */
int srv_self_fuzz(srv_t *s, uint64_t n, uint64_t seed);

#endif
