/* Reliability engine (DESIGN §8), on a simulated clock and in-memory link (QA §2, no sockets):
 * TC-UNIT-07 (RTO estimator, Karn, backoff), TC-UNIT-08 (10k seeded permutations: exactly-once,
 * in-order delivery), TC-UNIT-09 (duplicate classification), TC-UNIT-10 (SACK generation and
 * fast retransmit), the PDF bonus scenario at engine level (§8.8.1, exact log lines), the
 * lost-ACK case (§8.7), window/queue back-pressure, keepalive exemption and delayed ACKs. */
#include <stdlib.h>

#include "log.h"
#include "reliable.h"
#include "test.h"

static const uint8_t KEY[32] = {7, 7, 7};

/* ---------- simulated link between two engines ---------- */

#define MAXQ 4096
typedef struct {
    uint64_t due;
    int      to;          /* 0 = deliver to A, 1 = to B */
    uint16_t len;
    uint8_t  bytes[ZTP_MAX_DGRAM_CAP];
} pkt_t;

typedef struct sim sim_t;
typedef struct {
    sim_t   *sim;
    int      id;          /* 0 = A, 1 = B */
    rel_t   *rel;
    uint32_t expect;      /* next application index expected */
    uint32_t got;
    int      errors;
} end_t;

struct sim {
    uint64_t now, rng;
    double   loss, dup;
    int      dmin, dmax;  /* one-way delay range (ms); a spread reorders frames */
    pkt_t   *q;
    int      nq;
    end_t    e[2];
    int      reply_every; /* B replies to every k-th message (piggyback traffic), 0 = never */
    uint32_t replies;
    uint64_t sent_by[2];
};

static uint64_t srand64(sim_t *s)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 7;
    s->rng ^= s->rng << 17;
    return s->rng;
}
static double srand01(sim_t *s) { return (double)(srand64(s) >> 11) / 9007199254740992.0; }

static void enqueue(sim_t *s, int to, const uint8_t *f, size_t n)
{
    if (s->nq >= MAXQ) return;
    pkt_t *p = &s->q[s->nq++];
    p->due = s->now + (uint64_t)(s->dmin + (int)(srand64(s) % (uint64_t)(s->dmax - s->dmin + 1)));
    p->to = to;
    p->len = (uint16_t)n;
    memcpy(p->bytes, f, n);
}

static int sim_tx(void *ctx, const uint8_t *f, size_t n, bool to_source)
{
    end_t *e = ctx;
    sim_t *s = e->sim;
    s->sent_by[e->id]++;
    if (srand01(s) < s->loss) return 0;
    enqueue(s, 1 - e->id, f, n);
    if (srand01(s) < s->dup) enqueue(s, 1 - e->id, f, n);
    return 0;
}

static void sim_deliver(void *ctx, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t *p, uint16_t n)
{
    end_t *e = ctx;
    if (n < 4) { e->errors++; return; }
    uint32_t idx = be32_get(p);
    if (idx != e->expect) e->errors++;
    e->expect = idx + 1;
    e->got++;
    if (e->id == 1 && e->sim->reply_every && idx % (uint32_t)e->sim->reply_every == 0) {
        /* B's own stream back to A: index = replies so far + 1 */
        rel_msg_t m = {ZT_STATUS_ACK, 0, false, KEY, ZK_SESSION};
        uint8_t pl[4];
        be32_put(pl, e->sim->replies + 1);
        if (rel_send(e->rel, &m, pl, 4, e->sim->now) >= 0) e->sim->replies++;   /* NOBUFS: skip this reply */
    }
}

static void sim_init(sim_t *s, uint64_t seed, double loss, double dup, int dmin, int dmax, rel_cfg_t *cfg)
{
    memset(s, 0, sizeof *s);
    s->rng = seed * 0x9E3779B97F4A7C15ull + 1;
    s->loss = loss;
    s->dup = dup;
    s->dmin = dmin;
    s->dmax = dmax;
    s->q = calloc(MAXQ, sizeof *s->q);
    for (int i = 0; i < 2; i++) {
        s->e[i].sim = s;
        s->e[i].id = i;
        s->e[i].expect = 1;
        s->e[i].rel = rel_new(cfg, sim_tx, sim_deliver, &s->e[i]);
        rel_set_ack_key(s->e[i].rel, KEY, ZK_SESSION);
        rel_set_session(s->e[i].rel, 42, 0);
    }
}

static void sim_free(sim_t *s)
{
    rel_free(s->e[0].rel);
    rel_free(s->e[1].rel);
    free(s->q);
}

/* Deliver one due packet (earliest first). */
static void deliver_pkt(sim_t *s, int i)
{
    pkt_t p = s->q[i];
    s->q[i] = s->q[--s->nq];
    end_t *e = &s->e[p.to];
    ztp_hdr_t h;
    if (ztp_hdr_decode(p.bytes, p.len, ZTP_MAX_DGRAM_CAP, 0, &h) != ZTP_OK || !ztp_mac_verify(p.bytes, p.len, KEY)) {
        e->errors++;
        return;
    }
    const ztp_type_info_t *ti = ztp_type_info(h.type);
    if (ti->cls == ZC_RELIABLE) rel_on_rx(e->rel, &h, p.bytes + ZTP_HDR_LEN, s->now);
    else rel_on_ack(e->rel, &h, s->now);
}

/* Advance the simulation by one event. Returns false when nothing remains to do. */
static bool sim_step(sim_t *s)
{
    uint64_t t = UINT64_MAX;
    int pi = -1;
    for (int i = 0; i < s->nq; i++)
        if (s->q[i].due < t) { t = s->q[i].due; pi = i; }
    uint64_t ta = rel_next_deadline(s->e[0].rel), tb = rel_next_deadline(s->e[1].rel);
    uint64_t tt = ta < tb ? ta : tb;
    if (pi < 0 && tt == UINT64_MAX) return false;
    if (pi >= 0 && t <= tt) {
        if (t > s->now) s->now = t;
        deliver_pkt(s, pi);
    } else {
        if (tt > s->now) s->now = tt;
        rel_on_timer(s->e[0].rel, s->now);
        rel_on_timer(s->e[1].rel, s->now);
    }
    return true;
}

static rel_cfg_t default_cfg(void)
{
    rel_cfg_t c = {8, 500, 200, 4000, 8, 3, 20, 16, ZTP_MAX_DGRAM_CAP};
    return c;
}

/* A sends n messages (index 1..n, random length) to B; B may reply. */
static int run_transfer(sim_t *s, uint32_t n, uint64_t seed)
{
    uint32_t next = 1;
    uint64_t guard = 0;
    while (guard++ < 5000000) {
        while (next <= n) {
            uint8_t pl[300];
            uint16_t len = (uint16_t)(4 + srand64(s) % 200);
            be32_put(pl, next);
            for (int i = 4; i < len; i++) pl[i] = (uint8_t)(seed + i);
            rel_msg_t m = {ZT_CHUNK_REQ, 0, false, KEY, ZK_SESSION};
            int rc = rel_send(s->e[0].rel, &m, pl, len, s->now);
            if (rc == REL_E_NOBUFS) break;
            if (rc < 0) return rc;
            next++;
        }
        bool done = next > n && s->e[1].got == n && rel_idle(s->e[0].rel) && rel_idle(s->e[1].rel);
        if (done) return 0;
        if (rel_failed(s->e[0].rel) || rel_failed(s->e[1].rel)) return REL_E_FAILED;
        if (!sim_step(s)) return -1;
    }
    return -2;
}

static void test_permutations(void)
{
    /* TC-UNIT-08: 10,000 seeded runs with loss, duplication and reordering of 1..200 frames */
    rel_cfg_t cfg = default_cfg();
    cfg.max_retries = 16;   /* the property under test is ordering, not the retry budget */
    int bad = 0, failed = 0;
    uint64_t total = 0, retx = 0, ooo = 0, dups = 0;
    for (uint64_t seed = 1; seed <= 10000; seed++) {
        sim_t s;
        uint64_t r = seed * 2654435761u;
        double loss = (double)(r % 16) / 100.0;           /* 0..15 % per direction */
        double dup = (double)((r >> 8) % 6) / 100.0;      /* 0..5 % */
        int spread = (int)((r >> 16) % 40);               /* 0..39 ms of delay spread: reordering */
        sim_init(&s, seed, loss, dup, 1, 1 + spread, &cfg);
        s.reply_every = (int)(seed % 4);                  /* some runs carry reverse traffic */
        uint32_t n = 1 + (uint32_t)(seed % 200);
        int rc = run_transfer(&s, n, seed);
        if (rc == REL_E_FAILED) failed++;
        else if (rc != 0 || s.e[1].got != n || s.e[1].errors || s.e[0].errors) {
            if (bad < 3)
                fprintf(stderr, "  seed %llu: rc=%d got=%u/%u errors=%d/%d\n", (unsigned long long)seed, rc,
                        s.e[1].got, n, s.e[0].errors, s.e[1].errors);
            bad++;
        }
        total += n;
        retx += rel_stats(s.e[0].rel)->retx_timeout + rel_stats(s.e[0].rel)->retx_fast;
        ooo += rel_stats(s.e[1].rel)->ooo;
        dups += rel_stats(s.e[1].rel)->dup;
        sim_free(&s);
    }
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(failed, 0);
    CHECK(ooo > 0 && dups > 0 && retx > 0);   /* the impairments really happened */
    printf("           reliable: 10000 runs, %llu frames, %llu retransmissions, %llu out-of-order, %llu duplicates\n",
           (unsigned long long)total, (unsigned long long)retx, (unsigned long long)ooo, (unsigned long long)dups);
}

/* ---------- direct driving: capture frames from one engine ---------- */

typedef struct {
    int      n;
    uint8_t  f[64][ZTP_MAX_DGRAM_CAP];
    uint16_t len[64];
    bool     to_source[64];
    uint32_t delivered[64];
    int      ndel;
} cap_t;

static int cap_tx(void *ctx, const uint8_t *f, size_t n, bool to_source)
{
    cap_t *c = ctx;
    if (c->n < 64) {
        memcpy(c->f[c->n], f, n);
        c->len[c->n] = (uint16_t)n;
        c->to_source[c->n] = to_source;
        c->n++;
    }
    return 0;
}

static void cap_deliver(void *ctx, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t *p, uint16_t n)
{
    cap_t *c = ctx;
    if (c->ndel < 64) c->delivered[c->ndel++] = seq;
}

static ztp_hdr_t hdr_of(const cap_t *c, int i)
{
    ztp_hdr_t h = {0};
    ztp_hdr_decode(c->f[i], c->len[i], ZTP_MAX_DGRAM_CAP, 0, &h);
    return h;
}

/* Build a reliable frame header as the peer would send it. */
static ztp_hdr_t frame_hdr(uint32_t seq, uint32_t ack, uint32_t sack, bool ack_valid)
{
    ztp_hdr_t h = {0};
    h.type = ZT_CHUNK_DATA;
    h.seq = seq;
    h.ack = ack;
    h.sack = sack;
    h.flags = ack_valid ? ZF_ACK_VALID : 0;
    h.payload_len = 4;
    return h;
}

static ztp_hdr_t ack_hdr(uint32_t ack, uint32_t sack)
{
    ztp_hdr_t h = {0};
    h.type = ZT_ACK;
    h.flags = ZF_ACK_VALID;
    h.ack = ack;
    h.sack = sack;
    return h;
}

static const uint8_t PL[4] = {0, 0, 0, 1};

static void test_rto(void)
{
    /* TC-UNIT-07 */
    rel_cfg_t cfg = default_cfg();
    cap_t c = {0};
    rel_t *r = rel_new(&cfg, cap_tx, cap_deliver, &c);
    rel_msg_t m = {ZT_CHUNK_REQ, 0, false, KEY, ZK_SESSION};
    CHECK_EQ_INT(rel_rto_ms(r), 500);
    CHECK_EQ_INT(rel_send(r, &m, PL, 4, 0), 1);
    ztp_hdr_t a = ack_hdr(1, 0);
    rel_on_ack(r, &a, 100);                         /* R = 100: SRTT 100, RTTVAR 50, RTO 300 */
    CHECK_EQ_INT(rel_rto_ms(r), 300);
    CHECK_EQ_INT(rel_stats(r)->srtt_ms, 100);
    CHECK_EQ_INT(rel_send(r, &m, PL, 4, 1000), 2);
    a = ack_hdr(2, 0);
    rel_on_ack(r, &a, 1200);                        /* R = 200: RTTVAR 62.5, SRTT 112.5, RTO 362.5 */
    CHECK_EQ_INT(rel_rto_ms(r), 363);
    /* clamp to rto_min (200) */
    rel_t *r2 = rel_new(&cfg, cap_tx, cap_deliver, &c);
    rel_send(r2, &m, PL, 4, 0);
    a = ack_hdr(1, 0);
    rel_on_ack(r2, &a, 10);                         /* 10 + 4*5 = 30 -> 200 */
    CHECK_EQ_INT(rel_rto_ms(r2), 200);
    /* clamp to rto_max (4000) */
    rel_send(r2, &m, PL, 4, 0);
    a = ack_hdr(2, 0);
    rel_on_ack(r2, &a, 3900);
    CHECK_EQ_INT(rel_rto_ms(r2), 4000);
    rel_free(r2);

    /* Karn: an ACK for a retransmitted frame gives no sample */
    uint32_t before = rel_rto_ms(r);
    CHECK_EQ_INT(rel_send(r, &m, PL, 4, 2000), 3);
    CHECK_EQ_INT(rel_on_timer(r, 2000 + before), 0);   /* retransmits seq 3 */
    CHECK_EQ_INT(rel_stats(r)->retx_timeout, 1);
    a = ack_hdr(3, 0);
    rel_on_ack(r, &a, 2000 + before + 5);
    CHECK_EQ_INT(rel_rto_ms(r), before);
    CHECK(rel_idle(r));
    rel_free(r);

    /* backoff and the retry budget: 0.5, 1.5, 3.5, 7.5, 11.5 ... 23.5 s, failure at 27.5 s */
    r = rel_new(&cfg, cap_tx, cap_deliver, &c);
    c.n = 0;
    rel_send(r, &m, PL, 4, 0);
    const uint64_t want[] = {500, 1500, 3500, 7500, 11500, 15500, 19500, 23500};
    int wrong = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t d = rel_next_deadline(r);
        wrong += d != want[i];
        CHECK_EQ_INT(rel_on_timer(r, d), 0);
    }
    CHECK_EQ_INT(wrong, 0);
    CHECK_EQ_INT(c.n, 9);                              /* original + 8 retransmissions */
    ztp_hdr_t h = hdr_of(&c, 8);
    CHECK(h.seq == 1 && (h.flags & ZF_RETX));
    CHECK_EQ_INT((long long)rel_next_deadline(r), 27500);
    CHECK_EQ_INT(rel_on_timer(r, 27500), REL_E_FAILED);
    CHECK(rel_failed(r));
    CHECK_EQ_INT(rel_send(r, &m, PL, 4, 27501), REL_E_FAILED);
    rel_free(r);

    /* keepalives are exempt from the retry budget */
    cfg.max_retries = 2;
    r = rel_new(&cfg, cap_tx, cap_deliver, &c);
    rel_msg_t ka = {ZT_KEEPALIVE, 0, true, KEY, ZK_SESSION};
    rel_send(r, &ka, NULL, 0, 0);
    int rc = 0;
    for (int i = 0; i < 20 && rc == 0; i++) rc = rel_on_timer(r, rel_next_deadline(r));
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(rel_stats(r)->retx_timeout, 20);
    CHECK_EQ_INT((long long)(rel_next_deadline(r) - 0) > 0, 1);
    rel_free(r);
}

static void test_receiver(void)
{
    /* TC-UNIT-09 duplicates; TC-UNIT-10 SACK generation; delayed vs immediate ACK */
    rel_cfg_t cfg = default_cfg();
    cap_t c = {0};
    rel_t *r = rel_new(&cfg, cap_tx, cap_deliver, &c);
    rel_set_ack_key(r, KEY, ZK_SESSION);
    ztp_hdr_t h = frame_hdr(1, 0, 0, false);
    CHECK_EQ_INT(rel_on_rx(r, &h, PL, 0), REL_RX_NEW);
    CHECK_EQ_INT(c.n, 0);                              /* in-order: ACK delayed */
    CHECK_EQ_INT((long long)rel_next_deadline(r), 20);
    CHECK_EQ_INT(rel_on_timer(r, 20), 0);
    CHECK_EQ_INT(c.n, 1);
    ztp_hdr_t a = hdr_of(&c, 0);
    CHECK(a.type == ZT_ACK && a.ack == 1 && a.sack == 0 && (a.flags & ZF_ACK_VALID) && a.seq == 1);

    /* seq <= rcv_ack: duplicate, dropped, immediate re-ACK to the source */
    h = frame_hdr(1, 0, 0, false);
    CHECK_EQ_INT(rel_on_rx(r, &h, PL, 30), REL_RX_DUP);
    CHECK(c.n == 2 && c.to_source[1]);
    CHECK_EQ_INT(rel_stats(r)->dup, 1);

    /* 3, 5, 6 arrive (2 and 4 missing): buffered, SACK = bits for 3, 5, 6 = 0b1101 */
    uint32_t seqs[] = {3, 5, 6};
    for (int i = 0; i < 3; i++) {
        h = frame_hdr(seqs[i], 0, 0, false);
        CHECK_EQ_INT(rel_on_rx(r, &h, PL, 40), REL_RX_NEW);
    }
    a = hdr_of(&c, c.n - 1);
    CHECK(a.ack == 1 && a.sack == 0xD);                /* immediate ACK on each gap */
    CHECK(!c.to_source[c.n - 1]);
    /* an already-buffered seq is a duplicate too */
    h = frame_hdr(5, 0, 0, false);
    CHECK_EQ_INT(rel_on_rx(r, &h, PL, 41), REL_RX_DUP);
    CHECK_EQ_INT(rel_stats(r)->dup, 2);
    /* out of window: rcv_ack + 34 */
    h = frame_hdr(1 + 34, 0, 0, false);
    CHECK_EQ_INT(rel_on_rx(r, &h, PL, 42), REL_RX_OOW);
    h = frame_hdr(1 + 33, 0, 0, false);                /* the last in-window seq is accepted */
    CHECK_EQ_INT(rel_on_rx(r, &h, PL, 42), REL_RX_NEW);
    /* 2 arrives: delivers 2 and drains 3; 4 arrives: drains 5, 6 */
    c.ndel = 0;
    h = frame_hdr(2, 0, 0, false);
    rel_on_rx(r, &h, PL, 50);
    h = frame_hdr(4, 0, 0, false);
    rel_on_rx(r, &h, PL, 51);
    CHECK_EQ_INT(c.ndel, 5);
    int order_ok = 1;
    for (int i = 0; i < c.ndel; i++) order_ok &= c.delivered[i] == (uint32_t)(2 + i);
    CHECK(order_ok);
    CHECK_EQ_INT(rel_rcv_ack(r), 6);
    rel_on_timer(r, 100);
    a = hdr_of(&c, c.n - 1);
    CHECK(a.ack == 6 && a.sack == (1u << (34 - 6 - 2)));   /* seq 34 still buffered */
    rel_free(r);
}

static void test_fast_retx_and_window(void)
{
    /* TC-UNIT-10 sender side; window and queue back-pressure */
    rel_cfg_t cfg = default_cfg();
    cfg.queue_max = 4;
    cap_t c = {0};
    rel_t *r = rel_new(&cfg, cap_tx, cap_deliver, &c);
    rel_msg_t m = {ZT_CHUNK_DATA, 0, false, KEY, ZK_SESSION};
    for (int i = 1; i <= 8; i++) CHECK_EQ_INT(rel_send(r, &m, PL, 4, 0), i);
    for (int i = 0; i < 4; i++) CHECK_EQ_INT(rel_send(r, &m, PL, 4, 0), 0);   /* queued */
    CHECK_EQ_INT(rel_send(r, &m, PL, 4, 0), REL_E_NOBUFS);
    CHECK_EQ_INT(rel_in_flight(r), 8);
    CHECK_EQ_INT(rel_queued(r), 4);
    CHECK_EQ_INT(c.n, 8);

    /* ack=1, seq 2 missing, 3 and 4 SACKed: below the threshold, no fast retransmit */
    ztp_hdr_t a = ack_hdr(1, 0x3);
    rel_on_ack(r, &a, 50);
    CHECK_EQ_INT(rel_stats(r)->retx_fast, 0);
    CHECK_EQ_INT(rel_in_flight(r), 8);                 /* 1 acked, queue drained one */
    CHECK_EQ_INT(rel_queued(r), 3);
    /* seq 5 SACKed too: three later frames, so seq 2 is fast-retransmitted, once */
    a = ack_hdr(1, 0x7);
    rel_on_ack(r, &a, 60);
    CHECK_EQ_INT(rel_stats(r)->retx_fast, 1);
    ztp_hdr_t last = hdr_of(&c, c.n - 1);
    CHECK(last.seq == 2 && (last.flags & ZF_RETX));
    a = ack_hdr(1, 0xF);
    rel_on_ack(r, &a, 70);
    CHECK_EQ_INT(rel_stats(r)->retx_fast, 1);          /* at most once per RTO period */
    /* SACKed frames have no deadline. The earliest is seq 9: it left the queue at t=50, after the
     * first sample (R = 50) had set RTO = 200, so 250; seq 2 was re-armed at 60 + 500. */
    CHECK_EQ_INT((long long)rel_next_deadline(r), 250);
    /* stale or bogus ACKs are ignored */
    a = ack_hdr(1000, 0);
    rel_on_ack(r, &a, 80);
    CHECK_EQ_INT(rel_in_flight(r), 8);
    /* cumulative ACK of everything sent drains the queue */
    a = ack_hdr(9, 0);
    rel_on_ack(r, &a, 90);
    CHECK_EQ_INT(rel_queued(r), 0);
    CHECK_EQ_INT(rel_in_flight(r), 3);
    a = ack_hdr(12, 0);
    rel_on_ack(r, &a, 95);
    CHECK(rel_idle(r));

    /* datagram limit */
    uint8_t big[ZTP_MAX_PAYLOAD] = {0};
    CHECK_EQ_INT(rel_send(r, &m, big, ZTP_MAX_PAYLOAD, 100), 13);
    rel_set_max_dgram(r, ZTP_MIN_DGRAM);
    CHECK_EQ_INT(rel_send(r, &m, big, 465, 100), ZTP_E_TOOBIG);
    CHECK(rel_send(r, &m, big, 464, 100) > 0);

    /* a new epoch restarts both sequence spaces */
    rel_reset(r, 3);
    CHECK_EQ_INT(rel_next_seq(r), 1);
    CHECK_EQ_INT(rel_rcv_ack(r), 0);
    CHECK(rel_idle(r));
    c.n = 0;
    rel_set_session(r, 7421, 3);
    rel_send(r, &m, PL, 4, 200);
    ztp_hdr_t h = hdr_of(&c, 0);
    CHECK(h.seq == 1 && h.epoch == 3 && h.session_id == 7421 && !(h.flags & ZF_ACK_VALID));
    rel_free(r);
}

static void test_sacked_head_stall(void)
{
    /* Regression (found by TC-UNIT-08): seq 3 is SACKed under ack=1, then ack=2 makes it the
     * oldest unacknowledged frame; if the ACK covering it is lost, its timer must still fire. */
    rel_cfg_t cfg = default_cfg();
    cap_t c = {0};
    rel_t *r = rel_new(&cfg, cap_tx, cap_deliver, &c);
    rel_msg_t m = {ZT_CHUNK_DATA, 0, false, KEY, ZK_SESSION};
    for (int i = 0; i < 3; i++) rel_send(r, &m, PL, 4, 0);
    ztp_hdr_t a = ack_hdr(1, 0x1);   /* 1 acked, 3 SACKed */
    rel_on_ack(r, &a, 10);
    a = ack_hdr(2, 0);               /* 2 acked: 3 (SACKed) is now the head */
    rel_on_ack(r, &a, 20);
    CHECK_EQ_INT(rel_in_flight(r), 1);
    uint64_t d = rel_next_deadline(r);
    CHECK(d != UINT64_MAX);
    int before = c.n;
    CHECK_EQ_INT(rel_on_timer(r, d), 0);
    CHECK_EQ_INT(c.n, before + 1);
    ztp_hdr_t h = hdr_of(&c, c.n - 1);
    CHECK(h.seq == 3 && (h.flags & ZF_RETX));
    rel_free(r);
}

static void test_piggyback(void)
{
    /* a reply sent from inside deliver() carries the ACK, so no standalone ACK follows */
    rel_cfg_t cfg = default_cfg();
    sim_t s;
    sim_init(&s, 99, 0, 0, 5, 5, &cfg);
    s.reply_every = 1;
    CHECK_EQ_INT(run_transfer(&s, 10, 1), 0);
    CHECK_EQ_INT(s.e[1].got, 10);
    CHECK_EQ_INT(s.e[0].got, 10);
    /* B piggybacked on every reply: its standalone ACKs are far fewer than the frames it received */
    CHECK(rel_stats(s.e[1].rel)->acks_sent < 3);
    sim_free(&s);

    /* per-slot keys: a frame keeps the key it was first sent with */
    cap_t c = {0};
    rel_t *r = rel_new(&cfg, cap_tx, cap_deliver, &c);
    uint8_t k1[32] = {1}, k2[32] = {2};
    rel_msg_t m1 = {ZT_KEEPALIVE, 0, true, k1, ZK_BOOT}, m2 = {ZT_REGISTER, 0, false, k2, ZK_SESSION};
    rel_send(r, &m1, NULL, 0, 0);
    rel_send(r, &m2, PL, 4, 0);
    rel_on_timer(r, 500);
    CHECK_EQ_INT(c.n, 4);
    CHECK(ztp_mac_verify(c.f[2], c.len[2], k1) && hdr_of(&c, 2).key_id == ZK_BOOT);
    CHECK(ztp_mac_verify(c.f[3], c.len[3], k2) && hdr_of(&c, 3).key_id == ZK_SESSION);
    rel_free(r);
}

static char *read_all(FILE *f)
{
    fflush(f);
    long n = ftell(f);
    rewind(f);
    char *b = calloc(1, (size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) b[0] = '\0';
    return b;
}

static void test_pdf_scenario(void)
{
    /* §8.8.1 / TC-REL-03 at engine level: seq 2 lost, seq 3 buffered, timeout retransmission,
     * drain, then a duplicate of seq 2. Exact PDF log lines. */
    char path[300];
    snprintf(path, sizeof path, "%s/pdf.log", test_tmpdir());
    FILE *lf = fopen(path, "w+");
    log_set_stream(lf);
    log_set_level(LOG_INFO);
    rel_cfg_t cfg = default_cfg();
    cap_t cd = {0}, cs = {0};
    rel_t *dev = rel_new(&cfg, cap_tx, cap_deliver, &cd);
    rel_t *srv = rel_new(&cfg, cap_tx, cap_deliver, &cs);
    rel_set_ack_key(srv, KEY, ZK_SESSION);
    rel_msg_t m = {ZT_CHUNK_REQ, 0, false, KEY, ZK_SESSION};
    for (int i = 0; i < 3; i++) rel_send(dev, &m, PL, 4, 0);
    CHECK_EQ_INT(cd.n, 3);
    ztp_hdr_t h;
    h = hdr_of(&cd, 0); rel_on_rx(srv, &h, PL, 5);           /* seq 1 delivered */
    /* seq 2 lost */
    h = hdr_of(&cd, 2); rel_on_rx(srv, &h, PL, 6);           /* seq 3: out of order, buffered */
    ztp_hdr_t ack = hdr_of(&cs, cs.n - 1);
    CHECK(ack.type == ZT_ACK && ack.ack == 1 && ack.sack == 0x1);   /* the diagram's ACK ack=1 sack=0x1 */
    rel_on_ack(dev, &ack, 10);
    CHECK_EQ_INT(rel_on_timer(dev, rel_next_deadline(dev)), 0);     /* only seq 2 is due */
    CHECK_EQ_INT(cd.n, 4);
    h = hdr_of(&cd, 3);
    CHECK(h.seq == 2 && (h.flags & ZF_RETX));
    rel_on_rx(srv, &h, PL, 520);                                     /* deliver 2, drain 3 */
    CHECK_EQ_INT(rel_rcv_ack(srv), 3);
    rel_on_timer(srv, 540);
    ack = hdr_of(&cs, cs.n - 1);
    CHECK(ack.ack == 3 && ack.sack == 0);
    h = hdr_of(&cd, 1);
    CHECK_EQ_INT(rel_on_rx(srv, &h, PL, 600), REL_RX_DUP);          /* the delayed original of seq 2 */
    log_set_stream(NULL);
    char *out = read_all(lf);
    fclose(lf);
    const char *lines[] = {
        "[UDP] Received packet seq=3 (out of order, expected=2)\n",
        "[UDP] Buffered packet seq=3\n",
        "[UDP] Retransmitting seq=2 (timeout)\n",
        "[UDP] Delivered seq=2..3 (drained 1 buffered)\n",
        "[UDP] Duplicate packet seq=2 ignored\n",
    };
    const char *p = out;
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        const char *hit = strstr(p, lines[i]);
        if (!hit) {
            fprintf(stderr, "  missing or out of order: %s", lines[i]);
            CHECK(!"PDF log line");
        } else {
            p = hit + strlen(lines[i]);
        }
    }
    free(out);

    /* server-side lines carry the session tag (§17.1) */
    lf = fopen(path, "w+");
    log_set_stream(lf);
    rel_set_log_tag(srv, 7421, "ZTP-93AF21");
    h = hdr_of(&cd, 1);
    rel_on_rx(srv, &h, PL, 700);
    log_set_stream(NULL);
    out = read_all(lf);
    fclose(lf);
    CHECK(strstr(out, "[UDP] [sid=7421 ZTP-93AF21] Duplicate packet seq=2 ignored") != NULL);
    free(out);
    log_set_level(LOG_OFF);
    rel_free(dev);
    rel_free(srv);
}

static void test_lost_ack(void)
{
    /* §8.7: REGISTER delivered once; its ACK (piggybacked on REGISTER_ACK) is lost; the device's
     * retransmission is a duplicate, re-ACKed at once, and not re-processed. */
    rel_cfg_t cfg = default_cfg();
    cap_t cd = {0}, cs = {0};
    rel_t *dev = rel_new(&cfg, cap_tx, cap_deliver, &cd);
    rel_t *srv = rel_new(&cfg, cap_tx, cap_deliver, &cs);
    rel_set_ack_key(srv, KEY, ZK_SESSION);
    rel_msg_t m = {ZT_REGISTER, 0, false, KEY, ZK_SESSION};
    rel_send(dev, &m, PL, 4, 0);
    ztp_hdr_t h = hdr_of(&cd, 0);
    rel_on_rx(srv, &h, PL, 5);
    CHECK_EQ_INT(cs.ndel, 1);
    rel_msg_t ra = {ZT_REGISTER_ACK, 0, false, KEY, ZK_SESSION};
    rel_send(srv, &ra, PL, 4, 6);                      /* carries ack=1; lost */
    CHECK_EQ_INT(rel_on_timer(dev, 500), 0);            /* device retransmits REGISTER */
    h = hdr_of(&cd, 1);
    CHECK_EQ_INT(rel_on_rx(srv, &h, PL, 505), REL_RX_DUP);
    CHECK_EQ_INT(cs.ndel, 1);                           /* not re-processed */
    ztp_hdr_t a = hdr_of(&cs, cs.n - 1);
    CHECK(a.type == ZT_ACK && a.ack == 1 && cs.to_source[cs.n - 1]);
    rel_on_ack(dev, &a, 510);
    CHECK(rel_idle(dev));
    /* the REGISTER_ACK is repaired by its own timer */
    CHECK_EQ_INT(rel_on_timer(srv, rel_next_deadline(srv)), 0);
    h = hdr_of(&cs, cs.n - 1);
    CHECK(h.type == ZT_REGISTER_ACK && (h.flags & ZF_RETX));
    rel_free(dev);
    rel_free(srv);
}

void suite_reliable(void)
{
    int level = log_get_level();
    log_set_level(LOG_OFF);
    test_rto();
    test_receiver();
    test_fast_retx_and_window();
    test_sacked_head_stall();
    test_piggyback();
    test_pdf_scenario();
    test_lost_ack();
    test_permutations();
    log_set_level(level);
}
