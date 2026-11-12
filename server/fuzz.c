/* In-process datagram fuzzer (QA TC-SRV-07), the test hook `--self-fuzz N`: after the normal startup, N
 * generated datagrams go straight into the control and discovery handlers, on a virtual clock (1-5 ms per
 * input) so rate-limit buckets refill and half-open, idle and grace timers fire. Inputs: random bytes,
 * well-formed headers with random fields and payload, genuine K_boot HELLOs (which create half-open
 * sessions) and mutations of them, KEEPALIVE/ACK frames under K_boot for live sessions with random
 * seq/ack/sack (the reliability engine), and random or mutated DISCOVERs. Seeded, so a failure reproduces. */
#include <stdio.h>
#include <string.h>

#include "fuzz.h"
#include "log.h"
#include "protocol.h"
#include "timer.h"

static uint64_t g_rng;
static uint64_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}
static uint32_t below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

static void random_bytes(uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd();
}

/* A plausible header (magic, version, length consistent) around random content. */
static size_t random_frame(uint8_t *out, size_t cap, uint32_t sid)
{
    static const uint8_t types[] = {ZT_DISCOVER, ZT_HELLO, ZT_REGISTER, ZT_KEEPALIVE, ZT_ACK, ZT_STATUS_REPORT,
                                    ZT_MANIFEST_REQ, ZT_CHUNK_REQ, ZT_TRANSFER_DONE, ZT_UPGRADE_STATUS, 0x55, 0};
    size_t plen = below(4) ? below(200) : below((uint32_t)(cap - ZTP_HDR_LEN));
    size_t n = ZTP_HDR_LEN + plen;
    random_bytes(out, n);
    /* layout (§5.2): magic 0-1, version 2, type 3, flags 4, hdr_len 5, epoch 6-7, sid 8-11, seq 12-15,
     * ack 16-19, sack 20-23, payload_len 24-25, key_id 26, reserved 27, MAC 28-43 */
    out[0] = ZTP_MAGIC >> 8;
    out[1] = ZTP_MAGIC & 0xFF;
    out[2] = below(16) ? ZTP_VERSION : (uint8_t)rnd();
    out[3] = types[below(sizeof types)];
    out[4] = below(4) ? 0 : (uint8_t)rnd();   /* mostly valid flags */
    out[5] = below(16) ? ZTP_HDR_LEN : (uint8_t)rnd();
    if (below(2)) { out[6] = 0; out[7] = 0; }
    if (sid && below(2)) { out[8] = (uint8_t)(sid >> 24); out[9] = (uint8_t)(sid >> 16); out[10] = (uint8_t)(sid >> 8); out[11] = (uint8_t)sid; }
    if (below(8)) { out[24] = (uint8_t)(plen >> 8); out[25] = (uint8_t)plen; }
    out[26] = (uint8_t)below(4);
    if (below(8)) out[27] = 0;
    return n;
}

static int build(uint8_t *out, size_t cap, uint8_t type, uint8_t flags, uint32_t sid, uint32_t seq, uint32_t ack,
                 uint32_t sack, const uint8_t *pl, uint16_t plen, const uint8_t *key)
{
    ztp_hdr_t h = {0};
    h.type = type;
    h.flags = flags;
    h.key_id = ZK_BOOT;
    h.session_id = sid;
    h.seq = seq;
    h.ack = ack;
    h.sack = sack;
    return ztp_frame_build(out, cap, ZTP_MAX_DGRAM_CAP, &h, pl, plen, key);
}

static int genuine_hello(srv_t *s, uint8_t *out, size_t cap)
{
    char serial[17], fw[9];
    snprintf(serial, sizeof serial, "ZTP-%06X", below(0x1000000));
    snprintf(fw, sizeof fw, "v%u.%u", below(3), below(10));
    const char *model = s->policy.nmodels && below(4) ? s->policy.models[below((uint32_t)s->policy.nmodels)].name : "NOPE-1";
    uint8_t pl[128];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_str(&w, TAG_SERIAL, serial);
    tlv_put_str(&w, TAG_MODEL, model);
    tlv_put_str(&w, TAG_FW_VER, fw);
    tlv_put_u64(&w, TAG_NONCE_DEV, rnd());
    if (below(2)) tlv_put_u32(&w, TAG_CAPS, (uint32_t)rnd());
    return build(out, cap, ZT_HELLO, below(8) ? 0 : ZF_RESUME, 0, 1, 0, 0, pl, (uint16_t)w.len, s->keys.k_boot);
}

/* A live session id, if there is one. */
static uint32_t some_session(srv_t *s)
{
    if (!s->nsessions) return 0;
    for (int tries = 0; tries < 32; tries++) {
        srv_session_t *e = s->table[below(SRV_TABLE_SLOTS)];
        if (e && e->session_id && e->srv == s) return e->session_id;
    }
    return 0;
}

static void mutate(uint8_t *b, size_t *n, size_t cap)
{
    switch (below(5)) {
    case 0: b[below((uint32_t)*n)] ^= (uint8_t)(1u << below(8)); break;          /* bit flip */
    case 1: *n = below((uint32_t)*n + 1); break;                                 /* truncate */
    case 2: if (*n < cap) { size_t k = below((uint32_t)(cap - *n)) + 1; random_bytes(b + *n, k); *n += k; } break;
    case 3: b[below((uint32_t)*n)] = (uint8_t)rnd(); break;                       /* byte */
    default: for (int i = 0; i < 4; i++) b[below((uint32_t)*n)] ^= (uint8_t)rnd(); break;
    }
}

int srv_self_fuzz(srv_t *s, uint64_t n, uint64_t seed)
{
    g_rng = seed ? seed * 0x9E3779B97F4A7C15ull : 0x2545F4914F6CDD1Dull;
    uint8_t buf[ZTP_MAX_DGRAM_CAP + 64];
    uint64_t t0 = now_ms(), counts[6] = {0};
    srv_counters_t before = s->c;
    int peak = 0;
    for (uint64_t i = 0; i < n; i++) {
        s->now += 1 + below(5);
        struct sockaddr_in src = {0};
        src.sin_family = AF_INET;
        src.sin_addr.s_addr = htonl(0x7F000001u);
        src.sin_port = htons((uint16_t)(20000 + below(8)));   /* a few "devices", so peers move */
        size_t len = 0;
        int k = (int)below(100), kind;
        if (k < 20) {
            kind = 0;
            len = below(4) ? below(64) : below(ZTP_MAX_DGRAM_CAP + 32);
            random_bytes(buf, len);
        } else if (k < 40) {
            kind = 1;
            len = random_frame(buf, ZTP_MAX_DGRAM_CAP, some_session(s));
        } else if (k < 60) {
            kind = 2;
            int r = genuine_hello(s, buf, sizeof buf);
            len = r > 0 ? (size_t)r : 0;
            if (below(2)) mutate(buf, &len, ZTP_MAX_DGRAM_CAP);
        } else if (k < 85) {
            kind = 3;
            uint32_t sid = some_session(s);
            uint8_t pl[8];
            tlv_writer_t w;
            tlv_writer_init(&w, pl, sizeof pl);
            if (below(2)) tlv_put_u8(&w, TAG_PATH_PROBE, 1);
            int r = build(buf, sizeof buf, below(2) ? ZT_KEEPALIVE : ZT_ACK, below(6) ? 0 : ZF_RETX, sid, below(40),
                          below(40), (uint32_t)rnd(), pl, (uint16_t)w.len, s->keys.k_boot);
            len = r > 0 ? (size_t)r : 0;
            if (!below(4)) mutate(buf, &len, ZTP_MAX_DGRAM_CAP);
        } else {
            kind = 4;
            uint8_t pl[64];
            tlv_writer_t w;
            tlv_writer_init(&w, pl, sizeof pl);
            tlv_put_str(&w, TAG_MODEL, "EDGE-GW-100");
            tlv_put_str(&w, TAG_SERIAL, "ZTP-93AF21");
            tlv_put_u64(&w, TAG_DISC_NONCE, rnd());
            int r = build(buf, sizeof buf, ZT_DISCOVER, 0, 0, 1 + below(4), 0, 0, pl, (uint16_t)w.len, s->keys.k_boot);
            len = r > 0 ? (size_t)r : 0;
            if (below(2)) mutate(buf, &len, ZTP_MAX_DGRAM_CAP);
        }
        counts[kind]++;
        if (kind == 4) srv_on_disc_datagram(s, buf, len, &src);
        else srv_on_ctl_datagram(s, buf, len, &src);
        if (!(i & 63)) srv_sess_run_timers(s);
        if (s->nsessions > peak) peak = s->nsessions;
        if (s->nsessions > s->cfg.c.limits.max_sessions || s->nhalf_open > s->cfg.c.limits.max_half_open) {
            LOGW(T_ERROR, "[FUZZ] session caps exceeded: %d sessions, %d half-open", s->nsessions, s->nhalf_open);
            return 1;
        }
    }
    s->now += 3600u * 1000u;   /* let every timer expire */
    srv_sess_run_timers(s);
    srv_sess_run_timers(s);
    LOGI(T_UDP, "[FUZZ] %llu inputs (random %llu, header %llu, hello %llu, session %llu, discover %llu) in %llu ms; "
         "peak %d sessions, %d left after expiry; malformed +%llu, unknown +%llu, stale +%llu, mac_fail +%llu, rate_limited +%llu",
         (unsigned long long)n, (unsigned long long)counts[0], (unsigned long long)counts[1], (unsigned long long)counts[2],
         (unsigned long long)counts[3], (unsigned long long)counts[4], (unsigned long long)(now_ms() - t0), peak,
         s->nsessions, (unsigned long long)(s->c.rx_malformed - before.rx_malformed),
         (unsigned long long)(s->c.rx_unknown - before.rx_unknown), (unsigned long long)(s->c.rx_stale - before.rx_stale),
         (unsigned long long)(s->c.mac_fail - before.mac_fail), (unsigned long long)(s->c.rate_limited - before.rate_limited));
    return 0;
}
