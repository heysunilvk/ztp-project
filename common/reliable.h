/* Per-peer reliability engine (DESIGN §8): selective repeat with a cumulative ACK and a 32-bit
 * SACK, RFC 6298 RTO with Karn's rule and per-frame exponential backoff, fast retransmit,
 * a 32-slot reorder buffer, duplicate detection and delayed ACKs.
 *
 * The engine owns no socket and no clock: it emits frames through a tx callback, delivers
 * in-order payloads through a deliver callback, and is driven with explicit `now` values.
 * Both binaries use it (one instance on the device, one per session on the server), and the
 * unit tests drive it over a simulated link. */
#ifndef ZTP_RELIABLE_H
#define ZTP_RELIABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "protocol.h"

#define REL_E_FAILED  -100   /* a frame exhausted rel.max_retries: SESSION_FAILED */
#define REL_E_NOBUFS  -101   /* window full and the send queue is full */

/* Send a frame. to_source: reply to the source of the datagram being processed rather than
 * the session peer (re-ACK of a duplicate, §5.7). Return 0, or -errno (e.g. -EMSGSIZE). */
typedef int (*rel_tx_fn)(void *ctx, const uint8_t *frame, size_t len, bool to_source);
/* In-order, exactly-once delivery of a reliable frame's payload. */
typedef void (*rel_deliver_fn)(void *ctx, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t *payload,
                               uint16_t len);

typedef struct {
    int      window;            /* rel.tx_window, 1..16 */
    int      rto_initial_ms, rto_min_ms, rto_max_ms;
    int      max_retries;
    int      fast_retx_threshold;
    int      delayed_ack_ms;
    int      queue_max;         /* frames queued while the window is full (0 = no queue) */
    uint16_t max_dgram;         /* current datagram limit: 1200, or 508 after §5.8 */
} rel_cfg_t;

void rel_cfg_from(rel_cfg_t *rc, const ztp_config_t *c, int queue_max);

/* What to send. The key is copied: a frame is retransmitted under the key it was first sent
 * with, even if the session has switched keys since (K_boot path probe -> K_s, §10.1). */
typedef struct {
    uint8_t        type;
    uint8_t        flags;       /* extra header flags: ZF_RESUME, ZF_FIN */
    bool           keepalive;   /* exempt from max_retries (§8.3); liveness decides instead */
    const uint8_t *key;         /* 32 bytes */
    uint8_t        key_id;
} rel_msg_t;

typedef enum {
    REL_RX_NEW = 1,     /* never-seen seq: delivered (maybe with a drain) or buffered */
    REL_RX_DUP,         /* duplicate: dropped, re-ACKed to the source */
    REL_RX_OOW,         /* beyond the receive window: dropped */
} rel_rx_t;

typedef struct {
    uint64_t tx, rx, retx_timeout, retx_fast, dup, ooo, out_of_window, acks_sent;
    uint32_t srtt_ms;
} rel_stats_t;

typedef struct rel rel_t;

rel_t *rel_new(const rel_cfg_t *cfg, rel_tx_fn tx, rel_deliver_fn deliver, void *ctx);
void   rel_free(rel_t *r);
/* Start a new epoch: both sequence spaces restart (next_seq = 1, rcv_ack = 0); the RTT
 * estimate is kept. Queued and in-flight frames are discarded. */
void   rel_reset(rel_t *r, uint16_t epoch);

void   rel_set_session(rel_t *r, uint32_t session_id, uint16_t epoch);
/* Key for standalone ACK frames (K_boot until REGISTER, then K_s; §5.3). */
void   rel_set_ack_key(rel_t *r, const uint8_t key[ZTP_KEY_LEN], uint8_t key_id);
void   rel_set_max_dgram(rel_t *r, uint16_t max_dgram);
/* Server log lines carry "[sid=<sid> <serial>]" (§17.1); the device passes sid 0. */
void   rel_set_log_tag(rel_t *r, uint32_t sid, const char *serial);

/* Send a reliable frame. Returns the seq it was sent with (> 0), 0 if it was queued because
 * the window is full, ZTP_E_TOOBIG if it exceeds the datagram limit, REL_E_NOBUFS if the queue
 * is full, or REL_E_FAILED once the engine has failed. */
int    rel_send(rel_t *r, const rel_msg_t *m, const uint8_t *payload, uint16_t len, uint64_t now);
/* Send an ack-class frame now (ACK, KEEPALIVE_ACK, unsequenced ERROR): seq = next_seq, not
 * consumed, never retransmitted; carries the current ACK/SACK. */
int    rel_send_ackclass(rel_t *r, const rel_msg_t *m, const uint8_t *payload, uint16_t len, bool to_source);

/* A MAC-verified reliable frame: process its ACK fields, classify its seq, deliver. */
rel_rx_t rel_on_rx(rel_t *r, const ztp_hdr_t *h, const uint8_t *payload, uint64_t now);
/* A MAC-verified ack-class frame: process its ACK fields only. */
void   rel_on_ack(rel_t *r, const ztp_hdr_t *h, uint64_t now);

/* Earliest pending deadline (retransmission or delayed ACK), or UINT64_MAX. */
uint64_t rel_next_deadline(const rel_t *r);
/* Run due retransmissions and delayed ACKs. Returns 0, or REL_E_FAILED. */
int    rel_on_timer(rel_t *r, uint64_t now);

uint32_t rel_in_flight(const rel_t *r);        /* sent and not cumulatively ACKed */
uint32_t rel_queued(const rel_t *r);
bool     rel_idle(const rel_t *r);             /* nothing in flight or queued */
bool     rel_failed(const rel_t *r);
uint32_t rel_rcv_ack(const rel_t *r);
uint32_t rel_next_seq(const rel_t *r);
uint32_t rel_rto_ms(const rel_t *r);
const rel_stats_t *rel_stats(const rel_t *r);

#endif
