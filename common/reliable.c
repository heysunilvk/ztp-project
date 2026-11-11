#include "reliable.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "logmsg.h"

#define RX_SLOTS ZTP_RX_WINDOW   /* 32 = SACK width */

typedef struct {
    bool     in_use, sacked, retransmitted, keepalive, fast_sent;
    uint8_t  type, flags, key_id, retries;
    uint32_t seq;
    uint16_t len;
    uint32_t rto_ms;                       /* this frame's backed-off RTO */
    uint64_t first_sent, last_sent, deadline, last_fast;
    uint8_t  key[ZTP_KEY_LEN];
    uint8_t  payload[ZTP_MAX_PAYLOAD];
} tx_slot_t;

typedef struct {
    uint8_t  type, flags, key_id;
    bool     keepalive;
    uint16_t len;
    uint8_t  key[ZTP_KEY_LEN];
    uint8_t  payload[ZTP_MAX_PAYLOAD];
} queued_t;

typedef struct {
    uint8_t  type, flags;
    uint16_t len;
    uint8_t  payload[ZTP_MAX_PAYLOAD];
} rx_slot_t;

struct rel {
    rel_cfg_t      cfg;
    rel_tx_fn      tx;
    rel_deliver_fn deliver;
    void          *ctx;

    uint32_t session_id;
    uint16_t epoch;
    uint8_t  ack_key[ZTP_KEY_LEN], ack_key_id;
    bool     failed;

    /* sender */
    uint32_t  next_seq, snd_una;
    tx_slot_t tx_slots[ZTP_TX_WINDOW_MAX];
    queued_t *q;
    int       qhead, qlen;
    double    srtt, rttvar;   /* ms; srtt < 0 = no sample yet */
    uint32_t  rto;            /* ms, from the estimator (or rto_initial) */

    /* receiver */
    uint32_t  rcv_ack, rx_bitmap;
    bool      rx_seen;        /* received at least one reliable frame (ACK_VALID meaningful) */
    rx_slot_t rx[RX_SLOTS];
    bool      ack_pending;
    uint64_t  ack_deadline;

    int      last_tx_err;
    uint32_t max_in_flight;

    /* logging */
    uint32_t log_sid;
    char     log_serial[17];

    rel_stats_t st;
};

/* ---- setup ---- */

void rel_cfg_from(rel_cfg_t *rc, const ztp_config_t *c, int queue_max)
{
    rc->window = c->rel.tx_window;
    rc->rto_initial_ms = c->rel.rto_initial_ms;
    rc->rto_min_ms = c->rel.rto_min_ms;
    rc->rto_max_ms = c->rel.rto_max_ms;
    rc->max_retries = c->rel.max_retries;
    rc->fast_retx_threshold = c->rel.fast_retx_threshold;
    rc->delayed_ack_ms = c->rel.delayed_ack_ms;
    rc->queue_max = queue_max;
    rc->max_dgram = (uint16_t)c->net.max_datagram;
}

rel_t *rel_new(const rel_cfg_t *cfg, rel_tx_fn tx, rel_deliver_fn deliver, void *ctx)
{
    rel_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->cfg = *cfg;
    if (r->cfg.window < 1) r->cfg.window = 1;
    if (r->cfg.window > (int)ZTP_TX_WINDOW_MAX) r->cfg.window = ZTP_TX_WINDOW_MAX;
    if (r->cfg.max_dgram == 0 || r->cfg.max_dgram > ZTP_MAX_DGRAM_CAP) r->cfg.max_dgram = ZTP_MAX_DGRAM_CAP;
    if (r->cfg.queue_max > 0) {
        r->q = calloc((size_t)r->cfg.queue_max, sizeof *r->q);
        if (!r->q) { free(r); return NULL; }
    }
    r->tx = tx;
    r->deliver = deliver;
    r->ctx = ctx;
    r->srtt = -1;
    r->rto = (uint32_t)cfg->rto_initial_ms;
    rel_reset(r, 0);
    return r;
}

void rel_free(rel_t *r)
{
    if (!r) return;
    free(r->q);
    secure_zero(r, sizeof *r);
    free(r);
}

void rel_reset(rel_t *r, uint16_t epoch)
{
    r->epoch = epoch;
    r->failed = false;
    r->next_seq = r->snd_una = 1;
    memset(r->tx_slots, 0, sizeof r->tx_slots);
    r->qhead = r->qlen = 0;
    r->rcv_ack = 0;
    r->rx_bitmap = 0;
    r->rx_seen = false;
    r->ack_pending = false;
}

void rel_set_session(rel_t *r, uint32_t session_id, uint16_t epoch)
{
    r->session_id = session_id;
    r->epoch = epoch;
}

void rel_set_ack_key(rel_t *r, const uint8_t key[ZTP_KEY_LEN], uint8_t key_id)
{
    memcpy(r->ack_key, key, ZTP_KEY_LEN);
    r->ack_key_id = key_id;
}

void rel_set_max_dgram(rel_t *r, uint16_t max_dgram) { r->cfg.max_dgram = max_dgram; }

void rel_set_log_tag(rel_t *r, uint32_t sid, const char *serial)
{
    r->log_sid = sid;
    snprintf(r->log_serial, sizeof r->log_serial, "%s", serial ? serial : "");
}

/* ---- logging ---- */

static void rlog(const rel_t *r, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void rlog(const rel_t *r, int level, const char *fmt, ...)
{
    if (level < log_get_level()) return;
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (r->log_sid) log_sess(level, T_UDP, r->log_sid, r->log_serial, "%s", body);
    else log_msg(level, T_UDP, "%s", body);
}

/* ---- frame building ---- */

static int emit(rel_t *r, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t *key, uint8_t key_id,
                const uint8_t *payload, uint16_t len, bool to_source)
{
    ztp_hdr_t h = {0};
    h.type = type;
    h.flags = flags;
    h.epoch = r->epoch;
    h.session_id = r->session_id;
    h.seq = seq;
    h.key_id = key_id;
    if (r->rx_seen) {
        h.flags |= ZF_ACK_VALID;
        h.ack = r->rcv_ack;
        h.sack = r->rx_bitmap;
        r->ack_pending = false;   /* piggybacked */
    }
    uint8_t frame[ZTP_MAX_DGRAM_CAP];
    int n = ztp_frame_build(frame, sizeof frame, r->cfg.max_dgram, &h, payload, len, key);
    if (n < 0) return n;
    r->st.tx++;
    LOGD(T_UDP, "tx %s seq=%u ack=%u sack=%08x len=%d%s", ztp_type_name(type), (unsigned)seq, (unsigned)h.ack,
         (unsigned)h.sack, n, (flags & ZF_RETX) ? " RETX" : "");
    int rc = r->tx(r->ctx, frame, (size_t)n, to_source);
    if (rc < 0) r->last_tx_err = rc;
    return rc;
}

static void send_ack(rel_t *r, bool to_source)
{
    if (!r->rx_seen) return;
    emit(r, ZT_ACK, 0, r->next_seq, r->ack_key, r->ack_key_id, NULL, 0, to_source);
    r->st.acks_sent++;
}

static tx_slot_t *slot_of(rel_t *r, uint32_t seq) { return &r->tx_slots[seq % ZTP_TX_WINDOW_MAX]; }

uint32_t rel_in_flight(const rel_t *r) { return r->next_seq - r->snd_una; }
uint32_t rel_max_in_flight(const rel_t *r) { return r->max_in_flight; }

int rel_take_tx_error(rel_t *r)
{
    int e = r->last_tx_err;
    r->last_tx_err = 0;
    return e;
}
uint32_t rel_queued(const rel_t *r) { return (uint32_t)r->qlen; }
bool     rel_idle(const rel_t *r) { return rel_in_flight(r) == 0 && r->qlen == 0; }
bool     rel_failed(const rel_t *r) { return r->failed; }
uint32_t rel_rcv_ack(const rel_t *r) { return r->rcv_ack; }
uint32_t rel_next_seq(const rel_t *r) { return r->next_seq; }
uint32_t rel_acked_through(const rel_t *r) { return r->snd_una - 1; }
uint32_t rel_rto_ms(const rel_t *r) { return r->rto; }
const rel_stats_t *rel_stats(const rel_t *r) { return &r->st; }

/* Put a message into the window with the next seq and transmit it. */
static int transmit_new(rel_t *r, uint8_t type, uint8_t flags, bool keepalive, const uint8_t *key, uint8_t key_id,
                        const uint8_t *payload, uint16_t len, uint64_t now)
{
    uint32_t seq = r->next_seq++;
    tx_slot_t *s = slot_of(r, seq);
    memset(s, 0, offsetof(tx_slot_t, payload));
    s->in_use = true;
    s->seq = seq;
    s->type = type;
    s->flags = flags;
    s->keepalive = keepalive;
    s->key_id = key_id;
    memcpy(s->key, key, ZTP_KEY_LEN);
    s->len = len;
    if (len) memcpy(s->payload, payload, len);
    s->rto_ms = r->rto;
    s->first_sent = s->last_sent = now;
    s->deadline = now + s->rto_ms;
    if (rel_in_flight(r) > r->max_in_flight) r->max_in_flight = rel_in_flight(r);
    emit(r, type, flags, seq, key, key_id, payload, len, false);
    return (int)seq;
}

static void drain_queue(rel_t *r, uint64_t now)
{
    while (r->qlen && rel_in_flight(r) < (uint32_t)r->cfg.window) {
        queued_t *q = &r->q[r->qhead];
        transmit_new(r, q->type, q->flags, q->keepalive, q->key, q->key_id, q->payload, q->len, now);
        r->qhead = (r->qhead + 1) % r->cfg.queue_max;
        r->qlen--;
    }
}

int rel_send(rel_t *r, const rel_msg_t *m, const uint8_t *payload, uint16_t len, uint64_t now)
{
    if (r->failed) return REL_E_FAILED;
    if ((size_t)ZTP_HDR_LEN + len > r->cfg.max_dgram) return ZTP_E_TOOBIG;
    if (rel_in_flight(r) < (uint32_t)r->cfg.window && r->qlen == 0)
        return transmit_new(r, m->type, m->flags, m->keepalive, m->key, m->key_id, payload, len, now);
    if (r->qlen >= r->cfg.queue_max) return REL_E_NOBUFS;
    queued_t *q = &r->q[(r->qhead + r->qlen) % r->cfg.queue_max];
    q->type = m->type;
    q->flags = m->flags;
    q->keepalive = m->keepalive;
    q->key_id = m->key_id;
    memcpy(q->key, m->key, ZTP_KEY_LEN);
    q->len = len;
    if (len) memcpy(q->payload, payload, len);
    r->qlen++;
    return 0;
}

int rel_send_ackclass(rel_t *r, const rel_msg_t *m, const uint8_t *payload, uint16_t len, bool to_source)
{
    return emit(r, m->type, m->flags, r->next_seq, m->key, m->key_id, payload, len, to_source);
}

int rel_step_down(rel_t *r, uint16_t max_dgram, rel_fill_fn fill, void *ctx, uint64_t now)
{
    r->cfg.max_dgram = max_dgram;
    /* in flight: holes are filled in order, without renumbering (§5.8 step 2) */
    for (uint32_t seq = r->snd_una; seq_lt(seq, r->next_seq); seq++) {
        tx_slot_t *s = slot_of(r, seq);
        if (!s->in_use || s->sacked || (size_t)ZTP_HDR_LEN + s->len <= max_dgram) continue;
        uint8_t out[ZTP_MAX_PAYLOAD];
        s->len = fill(ctx, s->type, s->payload, s->len, out);
        memcpy(s->payload, out, s->len);
        s->retries++;
        s->retransmitted = true;
        s->last_sent = now;
        s->deadline = now + s->rto_ms;
        emit(r, s->type, (uint8_t)(s->flags | ZF_RETX), s->seq, s->key, s->key_id, s->payload, s->len, false);
    }
    /* queued, not yet sequenced: dropped; the peer re-requests them (§5.8 step 2) */
    int dropped = 0, kept = 0;
    for (int i = 0; i < r->qlen; i++) {
        queued_t *q = &r->q[(r->qhead + i) % r->cfg.queue_max];
        if ((size_t)ZTP_HDR_LEN + q->len > max_dgram) { dropped++; continue; }
        if (kept != i) r->q[(r->qhead + kept) % r->cfg.queue_max] = *q;
        kept++;
    }
    r->qlen = kept;
    return dropped;
}

/* ---- RTT / RTO (RFC 6298) ---- */

static void rtt_sample(rel_t *r, uint64_t rtt_ms)
{
    double R = (double)rtt_ms;
    if (r->srtt < 0) {
        r->srtt = R;
        r->rttvar = R / 2;
    } else {
        double d = r->srtt - R;
        r->rttvar = 0.75 * r->rttvar + 0.25 * (d < 0 ? -d : d);
        r->srtt = 0.875 * r->srtt + 0.125 * R;
    }
    double k = 4 * r->rttvar;
    double rto = r->srtt + (k > 1.0 ? k : 1.0);   /* G = 1 ms clock granularity */
    if (rto < r->cfg.rto_min_ms) rto = r->cfg.rto_min_ms;
    if (rto > r->cfg.rto_max_ms) rto = r->cfg.rto_max_ms;
    r->rto = (uint32_t)(rto + 0.5);
    r->st.srtt_ms = (uint32_t)(r->srtt + 0.5);
}

static void retransmit(rel_t *r, tx_slot_t *s, uint64_t now, bool fast)
{
    s->retries++;
    s->retransmitted = true;
    s->last_sent = now;
    if (fast) {
        s->last_fast = now;
        s->fast_sent = true;
        r->st.retx_fast++;
        rlog(r, LOG_INFO, LM_UDP_RETX_FAST, (unsigned)s->seq);
    } else {
        uint64_t nr = (uint64_t)s->rto_ms * 2;
        s->rto_ms = (uint32_t)(nr > (uint64_t)r->cfg.rto_max_ms ? (uint64_t)r->cfg.rto_max_ms : nr);
        r->st.retx_timeout++;
        rlog(r, LOG_INFO, LM_UDP_RETX_TIMEOUT, (unsigned)s->seq);
    }
    s->deadline = now + s->rto_ms;
    emit(r, s->type, (uint8_t)(s->flags | ZF_RETX), s->seq, s->key, s->key_id, s->payload, s->len, false);
}

/* ---- sender: ACK processing ---- */

void rel_on_ack(rel_t *r, const ztp_hdr_t *h, uint64_t now)
{
    if (!(h->flags & ZF_ACK_VALID) || r->failed) return;
    uint32_t A = h->ack;
    /* only acknowledgements of frames we actually sent; older (reordered) ACKs are ignored */
    if (!seq_lt(A, r->next_seq) || seq_lt(A, r->snd_una - 1)) return;

    uint64_t sample = 0;
    bool have_sample = false;
    uint64_t newest_sent = 0;
    /* cumulative part */
    while (seq_le(r->snd_una, A)) {
        tx_slot_t *s = slot_of(r, r->snd_una);
        if (s->in_use && s->seq == r->snd_una) {
            if (!s->retransmitted && !s->sacked && s->first_sent >= newest_sent) {   /* Karn */
                newest_sent = s->first_sent;
                sample = now - s->first_sent;
                have_sample = true;
            }
            s->in_use = false;
        }
        r->snd_una++;
    }
    /* selective part: bit i => seq A+2+i */
    for (uint32_t i = 0; i < 32; i++) {
        if (!((h->sack >> i) & 1u)) continue;
        uint32_t seq = A + 2 + i;
        if (!seq_lt(seq, r->next_seq)) break;
        tx_slot_t *s = slot_of(r, seq);
        if (s->in_use && s->seq == seq && !s->sacked) {
            s->sacked = true;
            if (!s->retransmitted && s->first_sent >= newest_sent) {
                newest_sent = s->first_sent;
                sample = now - s->first_sent;
                have_sample = true;
            }
        }
    }
    if (have_sample) rtt_sample(r, sample);

    /* fast retransmit: an un-SACKed frame with >= threshold later frames SACKed */
    for (uint32_t seq = r->snd_una; seq_lt(seq, r->next_seq); seq++) {
        tx_slot_t *s = slot_of(r, seq);
        if (!s->in_use || s->sacked) continue;
        int later = 0;
        for (uint32_t t = seq + 1; seq_lt(t, r->next_seq); t++) {
            tx_slot_t *u = slot_of(r, t);
            later += u->in_use && u->sacked;
        }
        if (later < r->cfg.fast_retx_threshold) continue;
        if (s->fast_sent && now - s->last_fast < s->rto_ms) continue;   /* once per RTO period */
        if (s->retries >= r->cfg.max_retries && !s->keepalive) continue;  /* the timer will fail it */
        retransmit(r, s, now, true);
    }
    drain_queue(r, now);
}

/* ---- receiver ---- */

/* Arm the delayed ACK before handing the frame up: a reply sent from inside the deliver
 * callback piggybacks the ACK and cancels it. */
static void deliver_one(rel_t *r, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t *p, uint16_t n,
                        uint64_t now)
{
    if (!r->ack_pending) {
        r->ack_pending = true;
        r->ack_deadline = now + (uint64_t)r->cfg.delayed_ack_ms;
    }
    if (r->deliver) r->deliver(r->ctx, type, flags, seq, p, n);
}

rel_rx_t rel_classify(const rel_t *r, uint32_t s)
{
    if (seq_le(s, r->rcv_ack)) return REL_RX_DUP;
    uint32_t d = s - r->rcv_ack;
    if (d > 1 + RX_SLOTS) return REL_RX_OOW;
    if (d > 1 && ((r->rx_bitmap >> (d - 2)) & 1u)) return REL_RX_DUP;
    return REL_RX_NEW;
}

rel_rx_t rel_on_rx(rel_t *r, const ztp_hdr_t *h, const uint8_t *payload, uint64_t now)
{
    r->st.rx++;
    rel_on_ack(r, h, now);
    uint32_t s = h->seq;
    r->rx_seen = true;

    if (seq_le(s, r->rcv_ack)) goto duplicate;
    uint32_t d = s - r->rcv_ack;
    if (d > 1 + RX_SLOTS) {
        r->st.out_of_window++;
        log_limited(7, LOG_WARN, T_UDP, "Dropped seq=%u outside the receive window (expected=%u)", (unsigned)s,
                    (unsigned)(r->rcv_ack + 1));
        return REL_RX_OOW;
    }
    if (d == 1) {
        uint8_t fl = h->flags & (uint8_t)~(ZF_ACK_VALID | ZF_RETX);
        r->rcv_ack = s;
        uint32_t carry = r->rx_bitmap & 1u;
        r->rx_bitmap >>= 1;
        deliver_one(r, h->type, fl, s, payload, h->payload_len, now);
        uint32_t drained = 0;
        while (carry) {   /* the next seq was buffered: deliver it and keep draining */
            uint32_t ns = r->rcv_ack + 1;
            rx_slot_t *b = &r->rx[ns % RX_SLOTS];
            r->rcv_ack = ns;
            carry = r->rx_bitmap & 1u;
            r->rx_bitmap >>= 1;
            drained++;
            deliver_one(r, b->type, b->flags, ns, b->payload, b->len, now);
        }
        if (drained) rlog(r, LOG_INFO, LM_UDP_DELIVERED, (unsigned)s, (unsigned)r->rcv_ack, (unsigned)drained);
        if (r->cfg.delayed_ack_ms == 0 && r->ack_pending) send_ack(r, false);
        return REL_RX_NEW;
    }
    {
        uint32_t bit = d - 2;
        if ((r->rx_bitmap >> bit) & 1u) goto duplicate;
        rx_slot_t *b = &r->rx[s % RX_SLOTS];
        b->type = h->type;
        b->flags = h->flags & (uint8_t)~(ZF_ACK_VALID | ZF_RETX);
        b->len = h->payload_len;
        if (h->payload_len) memcpy(b->payload, payload, h->payload_len);
        r->rx_bitmap |= 1u << bit;
        r->st.ooo++;
        rlog(r, LOG_INFO, LM_UDP_OOO, (unsigned)s, (unsigned)(r->rcv_ack + 1));
        rlog(r, LOG_INFO, LM_UDP_BUFFERED, (unsigned)s);
        send_ack(r, false);   /* immediate: tells the sender about the gap */
        return REL_RX_NEW;
    }

duplicate:
    r->st.dup++;
    rlog(r, LOG_INFO, LM_UDP_DUP_IGNORED, (unsigned)s);
    send_ack(r, true);        /* immediate re-ACK to the datagram's source (§5.7, §8.7) */
    return REL_RX_DUP;
}

/* ---- timers ---- */

/* SACKed frames are held by the receiver and need no retransmission, except the oldest
 * unacknowledged one: if the cumulative ACK that would cover it is lost, nothing else would
 * ever prompt the peer to ACK again. Its retransmission arrives as a duplicate, and the
 * immediate re-ACK carries the cumulative ACK. */
static bool timed(const rel_t *r, const tx_slot_t *s) { return !s->sacked || s->seq == r->snd_una; }

uint64_t rel_next_deadline(const rel_t *r)
{
    uint64_t d = UINT64_MAX;
    if (r->failed) return d;
    if (r->ack_pending && r->ack_deadline < d) d = r->ack_deadline;
    for (uint32_t seq = r->snd_una; seq_lt(seq, r->next_seq); seq++) {
        const tx_slot_t *s = &r->tx_slots[seq % ZTP_TX_WINDOW_MAX];
        if (s->in_use && timed(r, s) && s->deadline < d) d = s->deadline;
    }
    return d;
}

int rel_on_timer(rel_t *r, uint64_t now)
{
    if (r->failed) return REL_E_FAILED;
    for (uint32_t seq = r->snd_una; seq_lt(seq, r->next_seq); seq++) {
        tx_slot_t *s = slot_of(r, seq);
        if (!s->in_use || !timed(r, s) || s->deadline > now) continue;
        if (s->retries >= r->cfg.max_retries && !s->keepalive) {
            r->failed = true;
            rlog(r, LOG_WARN, "%s seq=%u unacknowledged after %u retransmissions: session failed",
                 ztp_type_name(s->type), (unsigned)s->seq, (unsigned)s->retries);
            return REL_E_FAILED;
        }
        retransmit(r, s, now, false);
    }
    if (r->ack_pending && r->ack_deadline <= now) send_ack(r, false);
    return 0;
}
