/* In-code loss / reorder / duplicate / delay / small-MTU shim (DESIGN §16.1).
 * All impairments apply on the sender's TX side only. Deterministic with a seed; uses its own
 * xorshift PRNG, never the CSPRNG. Inactive (a pure pass-through) when every knob is zero. */
#ifndef ZTP_NETSIM_H
#define ZTP_NETSIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "protocol.h"

enum netsim_mtu_mode { NS_MTU_EMSGSIZE = 0, NS_MTU_BLACKHOLE = 1 };

typedef struct {
    double   loss, reorder, dup;      /* probabilities 0..1 */
    uint32_t reorder_delay_ms;        /* hold time for a reordered datagram (default 30) */
    uint32_t delay_ms, jitter_ms;     /* base latency ± jitter */
    uint32_t mtu;                     /* 0 = off; datagrams > mtu fail or vanish */
    int      mtu_mode;                /* enum netsim_mtu_mode */
    uint64_t seed;                    /* 0 = derive from the clock (non-reproducible) */
} netsim_cfg_t;

/* Per-datagram decision (exposed for unit tests: TC-UNIT-15). */
typedef struct {
    bool     drop;          /* lost (loss or black hole) */
    bool     emsgsize;      /* fail the send with EMSGSIZE */
    bool     dup;           /* send a second copy */
    bool     reordered;     /* held for reorder_delay_ms */
    uint32_t delay_ms;      /* total hold before the first copy */
    uint32_t dup_delay_ms;  /* hold before the second copy (relative to now) */
} netsim_decision_t;

typedef struct {
    uint64_t tx, dropped, reordered, duplicated, emsgsize, mtu_dropped, rx;
} netsim_stats_t;

#define NETSIM_QUEUE_MAX 512

typedef struct {
    uint64_t due_ms;
    int      fd;
    socklen_t alen;
    struct sockaddr_storage addr;
    uint16_t len;
    uint8_t  bytes[ZTP_MAX_DGRAM_CAP];
} netsim_held_t;

typedef struct {
    netsim_cfg_t   cfg;
    bool           active;
    uint64_t       rng;
    netsim_stats_t st;
    netsim_held_t *q;        /* unsorted array, at most NETSIM_QUEUE_MAX entries */
    size_t         qlen;
} netsim_t;

int  netsim_init(netsim_t *ns, const netsim_cfg_t *cfg);   /* 0 or -1 (alloc) */
void netsim_free(netsim_t *ns);
bool netsim_is_active(const netsim_t *ns);

/* PRNG (xorshift64*). Exposed for tests. */
uint64_t netsim_rand(netsim_t *ns);
double   netsim_rand01(netsim_t *ns);

/* Decide the fate of one datagram of len bytes; consumes PRNG output; updates no counters. */
netsim_decision_t netsim_decide(netsim_t *ns, size_t len);

/* sendto() through the shim. Returns len as if sent (also for a dropped or held datagram),
 * or -1 with errno (EMSGSIZE from --sim-mtu, or a real socket error). */
ssize_t netsim_sendto(netsim_t *ns, int fd, const void *buf, size_t len, int flags,
                      const struct sockaddr *to, socklen_t tolen, uint64_t now);
/* recvfrom() through the shim (counts rx; the NAT emulator hooks in here in M3). */
ssize_t netsim_recvfrom(netsim_t *ns, int fd, void *buf, size_t cap, int flags,
                        struct sockaddr *from, socklen_t *fromlen);

/* Release held datagrams that are due. Returns the number sent. */
size_t   netsim_flush(netsim_t *ns, uint64_t now);
/* Earliest due time of a held datagram, or UINT64_MAX. */
uint64_t netsim_next_deadline(const netsim_t *ns);

/* Log the "[SIM] tx=… dropped=…" totals line (§16.1). */
void netsim_log_totals(const netsim_t *ns);

#endif
