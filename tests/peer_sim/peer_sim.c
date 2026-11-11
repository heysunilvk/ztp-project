/* peer_sim: a scripted ZTP protocol peer for component and negative tests (QA §3, §5).
 * It holds the test keys (from keys.json) and crafts frames directly, without the reliability
 * engine, so it can send things a well-behaved peer never would.
 *
 *   peer_sim [--keys conf/keys.json] [--server IP:PORT] [--timeout-ms N] MODE [mode options]
 *
 * Client modes (against a real ztp-server):
 *   handshakes N        N fresh HELLOs with distinct serials; session ids unique and random (TC-SESS-04)
 *   unknown-sid         (a) KEEPALIVE with a random sid: no reply; (b) RESUME HELLO with a random sid:
 *                       ERROR SESSION_UNKNOWN under K_boot echoing Nd' (TC-SESS-06)
 *   pdf-reorder         handshake, then KEEPALIVEs seq 3, seq 2 (RETX), seq 2 again (TC-REL-03)
 *   out-of-window       handshake, then a KEEPALIVE at rcv_ack + 100 (TC-REL-08)
 *   resume-expired SID SERIAL   resume HELLO (K_dev) for a deleted session: ERROR SESSION_EXPIRED
 *   spoof-source        handshake on socket A; bad-MAC frame and a replayed valid frame from socket B;
 *                       replies must still go to A (TC-NAT-06, TC-NAT-12)
 * Responder modes (against a real ztp-device):
 *   rogue-offer   --disc-port P [--group G] [--iface I] [--secs S]   OFFERs with a bad MAC (TC-DISC-07)
 *   replay-offer  --disc-port P --capture IP:PORT [...]               a captured genuine OFFER, replayed
 *                                                                     to later DISCOVERs (TC-DISC-08)
 *   fake-server   --listen IP:PORT --keepalive K --dead D [--secs S]  advertises arbitrary KEEPALIVE_S
 *                                                                     and DEAD_INTERVALS (TC-CFG-08)
 * Measurement mode:
 *   flood-discover N --disc-port P [--loss L] [--seed S]   N DISCOVERs through peer_sim's own netsim
 *                   (TX-side loss L), counting the OFFERs that come back; with the server's own --loss this
 *                   gives the per-direction and end-to-end loss rates of TC-SIM-01
 * Exit status: 0 = the expected behaviour was observed, 1 = not, 2 = usage or setup error. */
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth.h"
#include "net.h"
#include "netsim.h"
#include "log.h"
#include "protocol.h"
#include "rand.h"
#include "timer.h"

static srv_keys_t K;
static struct sockaddr_in g_server;
static int g_timeout_ms = 1000;
static const char *MODEL = "EDGE-GW-100";

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("peer_sim: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static int open_sock(void)
{
    char err[128];
    int fd = udp_open("0.0.0.0", 0, false, err, sizeof err);
    if (fd < 0) { say("socket: %s", err); exit(2); }
    return fd;
}

typedef struct {
    uint8_t type, flags, key_id;
    uint32_t sid, seq, ack, sack;
    uint16_t epoch;
} fr_t;

static int build(uint8_t *out, const fr_t *f, const uint8_t *pl, uint16_t n, const uint8_t *key)
{
    ztp_hdr_t h = {0};
    h.type = f->type;
    h.flags = f->flags;
    h.key_id = f->key_id;
    h.session_id = f->sid;
    h.seq = f->seq;
    h.ack = f->ack;
    h.sack = f->sack;
    h.epoch = f->epoch;
    return ztp_frame_build(out, ZTP_MAX_DGRAM_CAP, ZTP_MAX_DGRAM_CAP, &h, pl, n, key);
}

static void send_to(int fd, const uint8_t *b, int n, const struct sockaddr_in *to)
{
    if (n > 0) sendto(fd, b, (size_t)n, 0, (const struct sockaddr *)to, sizeof *to);
}

/* Receive one datagram within ms; decode the header (no MAC check). Returns length or -1. */
static int recv_one(int fd, int ms, uint8_t *buf, ztp_hdr_t *h, struct sockaddr_in *src)
{
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return -1;
    struct sockaddr_in s;
    socklen_t sl = sizeof s;
    ssize_t n = recvfrom(fd, buf, ZTP_MAX_DGRAM_CAP + 64, 0, (struct sockaddr *)&s, &sl);
    if (n < 0) return -1;
    if (src) *src = s;
    if (ztp_hdr_decode(buf, (size_t)n, ZTP_MAX_DGRAM_CAP, 0, h) != ZTP_OK) return -2;
    return (int)n;
}

/* ---------- client side ---------- */

typedef struct {
    int      fd;
    uint32_t sid;
    uint64_t nd, ns;
    char     serial[17];
    uint32_t srv_next;   /* server's next seq (for our ACK fields) */
} hs_t;

static void put_hello(tlv_writer_t *w, const char *serial, uint64_t nd)
{
    tlv_put_str(w, TAG_SERIAL, serial);
    tlv_put_str(w, TAG_MODEL, MODEL);
    tlv_put_str(w, TAG_FW_VER, "v1.0");
    tlv_put_u64(w, TAG_NONCE_DEV, nd);
}

/* Fresh HELLO -> HELLO_ACK (verified under K_boot, Nd echo). Returns 0 on success. */
static int handshake(hs_t *hs, const char *serial)
{
    snprintf(hs->serial, sizeof hs->serial, "%s", serial);
    hs->nd = rand_u64();
    uint8_t pl[96], b[ZTP_MAX_DGRAM_CAP + 64];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    put_hello(&w, serial, hs->nd);
    fr_t f = {ZT_HELLO, 0, ZK_BOOT, 0, 1, 0, 0, 0};
    int n = build(b, &f, pl, (uint16_t)w.len, K.k_boot);
    for (int attempt = 0; attempt < 5; attempt++) {
        send_to(hs->fd, b, n, &g_server);
        uint64_t until = now_ms() + (uint64_t)g_timeout_ms;
        while (now_ms() < until) {
            ztp_hdr_t h;
            uint8_t r[ZTP_MAX_DGRAM_CAP + 64];
            int m = recv_one(hs->fd, (int)(until - now_ms()), r, &h, NULL);
            if (m < 0) continue;
            if (h.type != ZT_HELLO_ACK || !ztp_mac_verify(r, (size_t)m, K.k_boot)) continue;
            tlv_set_t t;
            uint64_t echo = 0;
            if (tlv_parse(r + ZTP_HDR_LEN, h.payload_len, &t) != ZTP_OK || !tlv_get_u64(&t, TAG_NONCE_DEV, &echo) ||
                echo != hs->nd)
                continue;
            tlv_get_u64(&t, TAG_NONCE_SRV, &hs->ns);
            hs->sid = h.session_id;
            hs->srv_next = 2;
            return 0;
        }
    }
    return -1;
}

/* KEEPALIVE (path probe) with a chosen seq, ACKing the server's HELLO_ACK (seq 1). */
static int keepalive_frame(uint8_t *b, const hs_t *hs, uint32_t seq, uint8_t flags, const uint8_t *key)
{
    uint8_t pl[8];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u8(&w, TAG_PATH_PROBE, 1);
    fr_t f = {ZT_KEEPALIVE, (uint8_t)(ZF_ACK_VALID | flags), ZK_BOOT, hs->sid, seq, 1, 0, 0};
    return build(b, &f, pl, (uint16_t)w.len, key);
}

/* Collect replies for ms; returns the highest ack seen in KEEPALIVE_ACK/ACK frames (or -1). */
static long collect_acks(int fd, int ms, int *n_ka_ack, int *n_ack)
{
    long best = -1;
    uint64_t until = now_ms() + (uint64_t)ms;
    while (now_ms() < until) {
        ztp_hdr_t h;
        uint8_t r[ZTP_MAX_DGRAM_CAP + 64];
        int m = recv_one(fd, (int)(until - now_ms()), r, &h, NULL);
        if (m < 0 || !ztp_mac_verify(r, (size_t)m, K.k_boot)) continue;
        if (h.type == ZT_KEEPALIVE_ACK && n_ka_ack) (*n_ka_ack)++;
        if (h.type == ZT_ACK && n_ack) (*n_ack)++;
        if ((h.flags & ZF_ACK_VALID) && (long)h.ack > best) best = (long)h.ack;
    }
    return best;
}

static int mode_handshakes(int count)
{
    uint32_t *sids = calloc((size_t)count, sizeof *sids);
    hs_t hs = {.fd = open_sock()};
    for (int i = 0; i < count; i++) {
        char serial[17];
        snprintf(serial, sizeof serial, "ZTP-%06X", (unsigned)(0xA00000 + i));
        if (handshake(&hs, serial) != 0) { say("handshake %d failed", i); return 1; }
        sids[i] = hs.sid;
    }
    int dups = 0, zero = 0, seqish = 0;
    for (int i = 0; i < count; i++) {
        zero += sids[i] == 0;
        if (i && (sids[i] == sids[i - 1] + 1 || sids[i] + 1 == sids[i - 1])) seqish++;
        for (int j = 0; j < i; j++) dups += sids[i] == sids[j];
    }
    /* Wald-Wolfowitz runs test around the median (values above/below 2^31) */
    int above = 0, runs = 1;
    for (int i = 0; i < count; i++) {
        bool a = sids[i] >= 0x80000000u;
        above += a;
        if (i && a != (sids[i - 1] >= 0x80000000u)) runs++;
    }
    double n1 = above, n2 = count - above, nn = count;
    double mu = 2 * n1 * n2 / nn + 1, var = 2 * n1 * n2 * (2 * n1 * n2 - nn) / (nn * nn * (nn - 1));
    double z = var > 0 ? (runs - mu) / sqrt(var) : 0;
    say("%d handshakes: %d duplicate, %d zero, %d sequential neighbours, runs=%d (expected %.0f, z=%.2f)", count, dups,
        zero, seqish, runs, mu, z);
    free(sids);
    return (dups == 0 && zero == 0 && seqish <= count / 100 && z > -4 && z < 4) ? 0 : 1;
}

static int mode_unknown_sid(void)
{
    int fd = open_sock();
    uint8_t b[ZTP_MAX_DGRAM_CAP + 64];
    hs_t fake = {.fd = fd, .sid = rand_u32_nonzero()};
    /* (a) non-HELLO with a random sid: dropped, no reply */
    int n = keepalive_frame(b, &fake, 2, 0, K.k_boot);
    send_to(fd, b, n, &g_server);
    ztp_hdr_t h;
    int got = recv_one(fd, g_timeout_ms / 2, b, &h, NULL);
    if (got >= 0) { say("(a) unexpected reply %s", ztp_type_name(h.type)); return 1; }
    say("(a) no reply to a KEEPALIVE for unknown sid %u", (unsigned)fake.sid);
    /* (b) RESUME HELLO with a random sid, under K_dev: terminal ERROR SESSION_UNKNOWN under K_boot */
    const uint8_t *k_mfg = srv_keys_model(&K, MODEL);
    if (!k_mfg) { say("no key for %s", MODEL); return 2; }
    uint8_t k_dev[32];
    const char *serial = "ZTP-5E5510";
    ztp_derive_kdev(k_mfg, serial, k_dev);
    uint64_t nd2 = rand_u64();
    uint8_t pl[128];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    put_hello(&w, serial, nd2);
    tlv_put_u32(&w, TAG_RESUME_CTR, 1);
    tlv_put_u8(&w, TAG_STAGE, ST_REGISTERED);
    uint32_t sid = rand_u32_nonzero();
    fr_t f = {ZT_HELLO, ZF_RESUME, ZK_DEV, sid, 1, 0, 0, 1};
    n = build(b, &f, pl, (uint16_t)w.len, k_dev);
    send_to(fd, b, n, &g_server);
    uint8_t r[ZTP_MAX_DGRAM_CAP + 64];
    got = recv_one(fd, g_timeout_ms, r, &h, NULL);
    if (got < 0) { say("(b) no reply to the resume HELLO"); return 1; }
    if (h.type != ZT_ERROR || !ztp_mac_verify(r, (size_t)got, K.k_boot)) {
        say("(b) reply is %s, K_boot MAC %s", ztp_type_name(h.type), ztp_mac_verify(r, (size_t)got, K.k_boot) ? "ok" : "bad");
        return 1;
    }
    tlv_set_t t;
    uint16_t reason = 0;
    uint64_t echo = 0;
    if (tlv_parse(r + ZTP_HDR_LEN, h.payload_len, &t) != ZTP_OK || !tlv_get_u16(&t, TAG_REASON, &reason) ||
        !tlv_get_u64(&t, TAG_NONCE_DEV, &echo)) {
        say("(b) malformed ERROR");
        return 1;
    }
    say("(b) ERROR %s, Nd' echo %s", ztp_reason_name(reason), echo == nd2 ? "matches" : "WRONG");
    /* (b') the same resume HELLO with a wrong K_dev is dropped silently (no reflection) */
    uint8_t wrong[32] = {0};
    n = build(b, &f, pl, (uint16_t)w.len, wrong);
    send_to(fd, b, n, &g_server);
    got = recv_one(fd, g_timeout_ms / 2, r, &h, NULL);
    if (got >= 0) { say("(b') unexpected reply to a bad-MAC resume HELLO"); return 1; }
    say("(b') no reply to a resume HELLO with a bad K_dev MAC");
    return (reason == RC_SESSION_UNKNOWN && echo == nd2) ? 0 : 1;
}

/* A resume HELLO (K_dev) for a given session id: expect the terminal ERROR named by want. */
static int mode_resume_probe(uint32_t sid, const char *serial, uint16_t want)
{
    int fd = open_sock();
    const uint8_t *k_mfg = srv_keys_model(&K, MODEL);
    if (!k_mfg) { say("no key for %s", MODEL); return 2; }
    uint8_t k_dev[32], pl[128], b[ZTP_MAX_DGRAM_CAP + 64];
    ztp_derive_kdev(k_mfg, serial, k_dev);
    uint64_t nd2 = rand_u64();
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    put_hello(&w, serial, nd2);
    tlv_put_u32(&w, TAG_RESUME_CTR, 99);
    tlv_put_u8(&w, TAG_STAGE, ST_REGISTERED);
    fr_t f = {ZT_HELLO, ZF_RESUME, ZK_DEV, sid, 1, 0, 0, 5};
    send_to(fd, b, build(b, &f, pl, (uint16_t)w.len, k_dev), &g_server);
    ztp_hdr_t h;
    int got = recv_one(fd, g_timeout_ms, b, &h, NULL);
    tlv_set_t t;
    uint16_t reason = 0;
    uint64_t echo = 0;
    if (got < 0 || h.type != ZT_ERROR || !ztp_mac_verify(b, (size_t)got, K.k_boot) ||
        tlv_parse(b + ZTP_HDR_LEN, h.payload_len, &t) != ZTP_OK || !tlv_get_u16(&t, TAG_REASON, &reason) ||
        !tlv_get_u64(&t, TAG_NONCE_DEV, &echo)) {
        say("resume of sid %u: no valid ERROR reply", (unsigned)sid);
        return 1;
    }
    say("resume of sid %u: ERROR %s, Nd' echo %s", (unsigned)sid, ztp_reason_name(reason), echo == nd2 ? "matches" : "WRONG");
    return reason == want && echo == nd2 ? 0 : 1;
}

static int mode_pdf_reorder(void)
{
    hs_t hs = {.fd = open_sock()};
    if (handshake(&hs, "ZTP-0D0F03") != 0) { say("handshake failed"); return 1; }
    say("session %u: sending KEEPALIVE seq 3, then seq 2 (RETX), then seq 2 again", (unsigned)hs.sid);
    uint8_t b[ZTP_MAX_DGRAM_CAP];
    int ka = 0, ac = 0;
    send_to(hs.fd, b, keepalive_frame(b, &hs, 3, 0, K.k_boot), &g_server);
    long a1 = collect_acks(hs.fd, 300, &ka, &ac);
    send_to(hs.fd, b, keepalive_frame(b, &hs, 2, ZF_RETX, K.k_boot), &g_server);
    long a2 = collect_acks(hs.fd, 300, &ka, &ac);
    send_to(hs.fd, b, keepalive_frame(b, &hs, 2, ZF_RETX, K.k_boot), &g_server);
    long a3 = collect_acks(hs.fd, 300, &ka, &ac);
    say("acks after each step: %ld, %ld, %ld; KEEPALIVE_ACKs %d, ACKs %d", a1, a2, a3, ka, ac);
    /* after seq 3 alone: ack 1 (gap); after seq 2: ack 3 (drained); the duplicate: re-ACK 3 */
    return (a1 == 1 && a2 == 3 && a3 == 3 && ka == 2) ? 0 : 1;
}

static int mode_out_of_window(void)
{
    hs_t hs = {.fd = open_sock()};
    if (handshake(&hs, "ZTP-00B00B") != 0) { say("handshake failed"); return 1; }
    uint8_t b[ZTP_MAX_DGRAM_CAP];
    int ka = 0, ac = 0;
    send_to(hs.fd, b, keepalive_frame(b, &hs, 1 + 100, 0, K.k_boot), &g_server);
    long a = collect_acks(hs.fd, 400, &ka, &ac);
    say("seq 101 (rcv_ack + 100): replies %d, highest ack %ld", ka + ac, a);
    int replies_oow = ka + ac;
    ka = ac = 0;
    send_to(hs.fd, b, keepalive_frame(b, &hs, 2, 0, K.k_boot), &g_server);
    long a2 = collect_acks(hs.fd, 400, &ka, &ac);
    say("seq 2 afterwards: ack %ld (%d KEEPALIVE_ACK)", a2, ka);
    /* out of window: dropped without any reply, not buffered; seq 2 is then delivered normally */
    return (replies_oow == 0 && a == -1 && a2 == 2 && ka == 1) ? 0 : 1;
}

static int mode_spoof_source(void)
{
    hs_t hs = {.fd = open_sock()};
    if (handshake(&hs, "ZTP-5900F0") != 0) { say("handshake failed"); return 1; }
    int other = open_sock();
    uint8_t b[ZTP_MAX_DGRAM_CAP];
    /* TC-NAT-06: bad MAC from another port */
    uint8_t wrong[32] = {9};
    send_to(other, b, keepalive_frame(b, &hs, 2, 0, wrong), &g_server);
    int ka_b = 0, ac_b = 0;
    collect_acks(other, 300, &ka_b, &ac_b);
    /* the real peer (socket A) continues: its KEEPALIVE_ACK must come to A */
    int n2 = keepalive_frame(b, &hs, 2, 0, K.k_boot);
    uint8_t valid2[ZTP_MAX_DGRAM_CAP];
    memcpy(valid2, b, (size_t)n2);
    send_to(hs.fd, b, n2, &g_server);
    int ka_a = 0, ac_a = 0;
    long a = collect_acks(hs.fd, 300, &ka_a, &ac_a);
    say("bad MAC from another port: %d replies there; real peer got ack %ld (%d KEEPALIVE_ACK)", ka_b + ac_b, a, ka_a);
    if (ka_b + ac_b != 0 || a != 2 || ka_a != 1) return 1;
    /* TC-NAT-12: replay the captured valid seq 2 from the other port: a duplicate, re-ACKed only there */
    send_to(other, valid2, n2, &g_server);
    int ka_r = 0, ac_r = 0, ka_a2 = 0, ac_a2 = 0;
    collect_acks(other, 300, &ka_r, &ac_r);
    send_to(hs.fd, b, keepalive_frame(b, &hs, 3, 0, K.k_boot), &g_server);
    long a3 = collect_acks(hs.fd, 300, &ka_a2, &ac_a2);
    say("replayed valid frame from another port: %d ACK there; real peer then got ack %ld", ac_r, a3);
    return (ac_r == 1 && ka_r == 0 && a3 == 3 && ka_a2 == 1) ? 0 : 1;
}

static int mode_flood_discover(int count, int disc_port, double loss, uint64_t seed)
{
    struct sockaddr_in dst = g_server;
    dst.sin_port = htons((uint16_t)disc_port);
    int fd = open_sock();
    netsim_t ns;
    netsim_cfg_t nc = {.loss = loss, .seed = seed};
    netsim_init(&ns, &nc);
    long received = 0;
    uint8_t pl[64], b[ZTP_MAX_DGRAM_CAP + 64];
    for (int i = 0; i < count; i++) {
        tlv_writer_t w;
        tlv_writer_init(&w, pl, sizeof pl);
        tlv_put_str(&w, TAG_MODEL, MODEL);
        tlv_put_str(&w, TAG_SERIAL, "ZTP-F100D0");
        tlv_put_u64(&w, TAG_DISC_NONCE, (uint64_t)i + 1);
        fr_t f = {ZT_DISCOVER, 0, ZK_BOOT, 0, 1, 0, 0, 0};
        int n = build(b, &f, pl, (uint16_t)w.len, K.k_boot);
        netsim_sendto(&ns, fd, b, (size_t)n, 0, (struct sockaddr *)&dst, sizeof dst, now_ms());
        if (i % 50 == 49 || i == count - 1) {   /* pace: drain replies so no socket buffer overflows */
            ztp_hdr_t h;
            while (recv_one(fd, i == count - 1 ? 500 : 20, b, &h, NULL) > 0)
                if (h.type == ZT_OFFER) received++;
        }
    }
    say("flood: sent=%d tx_dropped=%llu offers_received=%ld", count,
        (unsigned long long)(ns.st.dropped), received);
    netsim_free(&ns);
    return 0;
}

/* ---------- responder side ---------- */

static int open_disc_listener(int port, const char *group, const char *iface)
{
    char err[128];
    int fd = udp_open("0.0.0.0", (uint16_t)port, true, err, sizeof err);
    if (fd < 0) { say("discovery socket: %s", err); exit(2); }
    if (udp_join_mcast(fd, group, iface) != 0) say("warning: could not join %s on %s", group, iface);
    return fd;
}

static int mode_offer(int disc_port, const char *group, const char *iface, int secs, const char *capture)
{
    uint8_t genuine[ZTP_MAX_DGRAM_CAP];
    int glen = 0;
    if (capture) {   /* get a genuine OFFER from the real server: DISCOVER under K_boot, unicast */
        struct sockaddr_in cap;
        if (!hostport_parse(capture, &cap)) { say("bad --capture"); return 2; }
        int fd = open_sock();
        uint8_t pl[64], b[ZTP_MAX_DGRAM_CAP + 64];
        tlv_writer_t w;
        tlv_writer_init(&w, pl, sizeof pl);
        tlv_put_str(&w, TAG_MODEL, MODEL);
        tlv_put_str(&w, TAG_SERIAL, "ZTP-CAFE00");
        tlv_put_u64(&w, TAG_DISC_NONCE, rand_u64());
        fr_t f = {ZT_DISCOVER, 0, ZK_BOOT, 0, 1, 0, 0, 0};
        send_to(fd, b, build(b, &f, pl, (uint16_t)w.len, K.k_boot), &cap);
        ztp_hdr_t h;
        glen = recv_one(fd, g_timeout_ms, b, &h, NULL);
        if (glen <= 0 || h.type != ZT_OFFER) { say("could not capture an OFFER from %s", capture); return 2; }
        memcpy(genuine, b, (size_t)glen);
        close(fd);
        say("captured a genuine OFFER (%d bytes) from %s", glen, capture);
    }
    int fd = open_disc_listener(disc_port, group, iface);
    uint64_t until = now_ms() + (uint64_t)secs * 1000u;
    int answered = 0;
    while (now_ms() < until) {
        ztp_hdr_t h;
        uint8_t b[ZTP_MAX_DGRAM_CAP + 64];
        struct sockaddr_in src;
        int n = recv_one(fd, (int)(until - now_ms()), b, &h, &src);
        if (n < 0 || h.type != ZT_DISCOVER) continue;
        if (capture) {
            send_to(fd, genuine, glen, &src);
        } else {
            tlv_set_t t;
            uint64_t dn = 0;
            if (tlv_parse(b + ZTP_HDR_LEN, h.payload_len, &t) != ZTP_OK || !tlv_get_u64(&t, TAG_DISC_NONCE, &dn)) continue;
            uint8_t pl[64], o[256];
            tlv_writer_t w;
            tlv_writer_init(&w, pl, sizeof pl);
            tlv_put_u64(&w, TAG_DISC_NONCE, dn);   /* echoes the nonce, but the MAC is wrong */
            uint8_t evil_ip[4] = {127, 0, 0, 1};
            tlv_put_bytes(&w, TAG_SERVER_ADDR, evil_ip, 4);
            tlv_put_u16(&w, TAG_SERVER_PORT, 9);
            uint8_t rogue_key[32] = {0x66};
            fr_t f = {ZT_OFFER, 0, ZK_BOOT, 0, h.seq, 0, 0, 0};
            send_to(fd, o, build(o, &f, pl, (uint16_t)w.len, rogue_key), &src);
        }
        answered++;
    }
    say("answered %d DISCOVERs with a %s OFFER", answered, capture ? "replayed" : "forged");
    return answered > 0 ? 0 : 1;
}

static int mode_fake_server(const char *listen, int keepalive, int dead, int secs)
{
    struct sockaddr_in la;
    if (!hostport_parse(listen, &la)) { say("bad --listen"); return 2; }
    char ip[INET_ADDRSTRLEN], err[128];
    inet_ntop(AF_INET, &la.sin_addr, ip, sizeof ip);
    int fd = udp_open(ip, ntohs(la.sin_port), true, err, sizeof err);
    if (fd < 0) { say("listen: %s", err); return 2; }
    uint64_t until = now_ms() + (uint64_t)secs * 1000u;
    uint32_t sid = 7421, rcv = 0;
    int hellos = 0;
    while (now_ms() < until) {
        ztp_hdr_t h;
        uint8_t b[ZTP_MAX_DGRAM_CAP + 64], o[ZTP_MAX_DGRAM_CAP];
        struct sockaddr_in src;
        int n = recv_one(fd, (int)(until - now_ms()), b, &h, &src);
        if (n < 0 || !ztp_mac_verify(b, (size_t)n, K.k_boot)) continue;
        if (h.type == ZT_HELLO && !(h.flags & ZF_RESUME)) {
            tlv_set_t t;
            uint64_t nd = 0;
            if (tlv_parse(b + ZTP_HDR_LEN, h.payload_len, &t) != ZTP_OK || !tlv_get_u64(&t, TAG_NONCE_DEV, &nd)) continue;
            uint8_t pl[96];
            tlv_writer_t w;
            tlv_writer_init(&w, pl, sizeof pl);
            tlv_put_u64(&w, TAG_NONCE_DEV, nd);
            tlv_put_u64(&w, TAG_NONCE_SRV, rand_u64());
            tlv_put_u16(&w, TAG_KEEPALIVE_S, (uint16_t)keepalive);
            tlv_put_u8(&w, TAG_DEAD_INTERVALS, (uint8_t)dead);
            tlv_put_u16(&w, TAG_GRACE_S, 30);
            rcv = 1;
            fr_t f = {ZT_HELLO_ACK, ZF_ACK_VALID, ZK_BOOT, sid, 1, rcv, 0, 0};
            send_to(fd, o, build(o, &f, pl, (uint16_t)w.len, K.k_boot), &src);
            hellos++;
        } else if (h.type == ZT_KEEPALIVE && h.session_id == sid) {
            if (h.seq == rcv + 1) rcv = h.seq;
            uint8_t pl[8];
            tlv_writer_t w;
            tlv_writer_init(&w, pl, sizeof pl);
            tlv_put_u8(&w, TAG_SRV_STAGE, 0);
            fr_t f = {ZT_KEEPALIVE_ACK, ZF_ACK_VALID, ZK_BOOT, sid, 2, rcv, 0, 0};
            send_to(fd, o, build(o, &f, pl, (uint16_t)w.len, K.k_boot), &src);
        }
    }
    say("fake server: answered %d HELLOs advertising keepalive %d s, dead %d", hellos, keepalive, dead);
    return hellos > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *keys = "conf/keys.json", *server = "127.0.0.1:5555", *mode = NULL;
    const char *group = "239.255.90.90", *iface = "127.0.0.1", *capture = NULL, *listen = NULL;
    int count = 1000, disc_port = 5556, secs = 5, keepalive = 20, dead = 3;
    double loss = 0;
    const char *arg1 = NULL, *arg2 = NULL;
    uint64_t seed = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--keys") && v) { keys = v; i++; }
        else if (!strcmp(a, "--server") && v) { server = v; i++; }
        else if (!strcmp(a, "--timeout-ms") && v) { g_timeout_ms = atoi(v); i++; }
        else if (!strcmp(a, "--disc-port") && v) { disc_port = atoi(v); i++; }
        else if (!strcmp(a, "--group") && v) { group = v; i++; }
        else if (!strcmp(a, "--iface") && v) { iface = v; i++; }
        else if (!strcmp(a, "--secs") && v) { secs = atoi(v); i++; }
        else if (!strcmp(a, "--capture") && v) { capture = v; i++; }
        else if (!strcmp(a, "--listen") && v) { listen = v; i++; }
        else if (!strcmp(a, "--keepalive") && v) { keepalive = atoi(v); i++; }
        else if (!strcmp(a, "--dead") && v) { dead = atoi(v); i++; }
        else if (!strcmp(a, "--loss") && v) { loss = atof(v); i++; }
        else if (!strcmp(a, "--seed") && v) { seed = strtoull(v, NULL, 10); i++; }
        else if (!mode) mode = a;
        else if (!strcmp(mode, "handshakes") || !strcmp(mode, "flood-discover")) count = atoi(a);
        else if (!strcmp(mode, "resume-expired") && !arg1) arg1 = a;
        else if (!strcmp(mode, "resume-expired") && !arg2) arg2 = a;
        else { fprintf(stderr, "peer_sim: unexpected argument %s\n", a); return 2; }
    }
    if (!mode) { fprintf(stderr, "usage: see the comment at the top of tests/peer_sim/peer_sim.c\n"); return 2; }
    char err[256];
    log_set_level(LOG_OFF);
    if (srv_keys_load(keys, &K, err, sizeof err) != 0) { fprintf(stderr, "peer_sim: %s\n", err); return 2; }
    if (!hostport_parse(server, &g_server)) { fprintf(stderr, "peer_sim: bad --server\n"); return 2; }
    int rc;
    if (!strcmp(mode, "handshakes")) rc = mode_handshakes(count);
    else if (!strcmp(mode, "unknown-sid")) rc = mode_unknown_sid();
    else if (!strcmp(mode, "pdf-reorder")) rc = mode_pdf_reorder();
    else if (!strcmp(mode, "resume-expired"))
        rc = arg1 && arg2 ? mode_resume_probe((uint32_t)strtoul(arg1, NULL, 10), arg2, RC_SESSION_EXPIRED) : 2;
    else if (!strcmp(mode, "out-of-window")) rc = mode_out_of_window();
    else if (!strcmp(mode, "spoof-source")) rc = mode_spoof_source();
    else if (!strcmp(mode, "rogue-offer")) rc = mode_offer(disc_port, group, iface, secs, NULL);
    else if (!strcmp(mode, "replay-offer")) rc = mode_offer(disc_port, group, iface, secs, capture);
    else if (!strcmp(mode, "flood-discover")) rc = mode_flood_discover(count, disc_port, loss, seed);
    else if (!strcmp(mode, "fake-server")) rc = listen ? mode_fake_server(listen, keepalive, dead, secs) : 2;
    else { fprintf(stderr, "peer_sim: unknown mode %s\n", mode); rc = 2; }
    srv_keys_free(&K);
    return rc;
}
