/* Server receive pipeline (DESIGN §5.6, §5.7) and per-message handlers. M2: DISCOVER/OFFER,
 * fresh HELLO/HELLO_ACK, path-probe and periodic KEEPALIVE, ACK, unknown-session resume. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "logmsg.h"
#include "rand.h"
#include "srv.h"

/* ---- sending ---- */

int srv_send(srv_t *s, int fd, const uint8_t *frame, size_t len, const struct sockaddr_in *dst)
{
    int act = hooks_tx_action(&s->hooks, frame, len);
    if (act == HOOK_DROP) {
        LOGD(T_UDP, "test hook: dropped %s seq=%u", ztp_type_name(frame[3]), (unsigned)be32_get(frame + 12));
        return 0;
    }
    for (int i = 0; i < (act == HOOK_SEND_TWICE ? 2 : 1); i++) {
        if (netsim_sendto(&s->ns, fd, frame, len, 0, (const struct sockaddr *)dst, sizeof *dst, s->now) < 0) {
            int e = errno;
            log_limited(11, LOG_WARN, T_UDP, "sendto: %s", strerror(e));
            return -e;
        }
    }
    return 0;
}

/* Send a terminal / unsequenced frame (no session engine): K_boot, ack-class semantics. */
static int send_terminal(srv_t *s, int fd, uint8_t type, uint32_t sid, uint16_t epoch, const uint8_t *payload,
                         uint16_t plen, const struct sockaddr_in *dst, uint8_t *out, uint16_t *out_len)
{
    ztp_hdr_t h = {0};
    h.type = type;
    h.session_id = sid;
    h.epoch = epoch;
    h.seq = 1;
    h.key_id = ZK_BOOT;
    uint8_t frame[ZTP_MAX_DGRAM_CAP];
    int n = ztp_frame_build(frame, sizeof frame, ZTP_MAX_DGRAM_CAP, &h, payload, plen, s->keys.k_boot);
    if (n < 0) return n;
    if (out && (size_t)n <= ZTP_MIN_DGRAM) {
        memcpy(out, frame, (size_t)n);
        *out_len = (uint16_t)n;
    }
    return srv_send(s, fd, frame, (size_t)n, dst);
}

/* ---- rate limiting (§10.5): token bucket per source IP ---- */

bool srv_rate_ok(srv_t *s, const struct sockaddr_in *src)
{
    uint32_t ip = src->sin_addr.s_addr;
    srv_rate_t *slot = NULL, *oldest = &s->rate[0];
    for (int i = 0; i < SRV_RATE_IPS; i++) {
        if (s->rate[i].last_ms && s->rate[i].ip == ip) { slot = &s->rate[i]; break; }
        if (s->rate[i].last_ms < oldest->last_ms) oldest = &s->rate[i];
    }
    double rate = s->cfg.c.limits.hello_rate_per_ip, burst = s->cfg.c.limits.hello_burst;
    if (!slot) {
        slot = oldest;
        slot->ip = ip;
        slot->tokens = burst;
        slot->last_ms = s->now ? s->now : 1;
    }
    double add = (double)(s->now - slot->last_ms) / 1000.0 * rate;
    slot->tokens = slot->tokens + add > burst ? burst : slot->tokens + add;
    slot->last_ms = s->now ? s->now : 1;
    if (slot->tokens < 1.0) {
        s->c.rate_limited++;
        return false;
    }
    slot->tokens -= 1.0;
    return true;
}

/* ---- reply tombstones for terminal replies (§5.3) ---- */

static srv_tombstone_t *tomb_find(srv_t *s, uint32_t sid, const char *serial, uint64_t nonce)
{
    for (int i = 0; i < SRV_TOMBSTONES; i++) {
        srv_tombstone_t *t = &s->tomb[i];
        if (t->used && t->expires_ms > s->now && t->sid == sid && t->nonce == nonce && !strcmp(t->serial, serial))
            return t;
    }
    return NULL;
}

static srv_tombstone_t *tomb_new(srv_t *s, uint32_t sid, const char *serial, uint64_t nonce)
{
    srv_tombstone_t *t = &s->tomb[s->tomb_next];
    s->tomb_next = (s->tomb_next + 1) % SRV_TOMBSTONES;
    memset(t, 0, sizeof *t);
    t->used = true;
    t->sid = sid;
    snprintf(t->serial, sizeof t->serial, "%s", serial);
    t->nonce = nonce;
    t->expires_ms = s->now + (uint64_t)s->cfg.c.nat.half_open_ttl_s * 1000u;
    return t;
}

static void terminal_error(srv_t *s, uint32_t sid, uint16_t epoch, const char *serial, uint64_t nonce,
                           uint16_t reason, const char *detail, const struct sockaddr_in *dst)
{
    uint8_t pl[128];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u16(&w, TAG_REASON, reason);
    tlv_put_str(&w, TAG_DETAIL, detail);
    tlv_put_u64(&w, TAG_NONCE_DEV, nonce);
    srv_tombstone_t *t = tomb_new(s, sid, serial, nonce);
    send_terminal(s, s->ctl_fd, ZT_ERROR, sid, epoch, pl, (uint16_t)w.len, dst, t->bytes, &t->len);
}

/* ---- HELLO (fresh), §7.1, §7.3 ---- */

static void on_fresh_hello(srv_t *s, const uint8_t *buf, size_t len, const ztp_hdr_t *h, const struct sockaddr_in *src)
{
    char a[ADDR_STR];
    addr_fmt(src, a);
    if (!ztp_mac_verify(buf, len, s->keys.k_boot)) {
        s->c.mac_fail++;
        log_limited(1, LOG_INFO, T_UDP, "HELLO from %s: MAC invalid under K_boot, dropped", a);
        return;
    }
    tlv_set_t t;
    char serial[17] = "", model[17] = "", fw[9] = "";
    uint64_t nd = 0;
    static const uint8_t need[] = {TAG_SERIAL, TAG_MODEL, TAG_FW_VER, TAG_NONCE_DEV};
    if (h->seq != 1 || tlv_parse(buf + ZTP_HDR_LEN, h->payload_len, &t) != ZTP_OK ||
        tlv_require(&t, need, sizeof need, NULL) != ZTP_OK) {
        s->c.rx_malformed++;
        return;
    }
    tlv_get_str(&t, TAG_SERIAL, serial, sizeof serial);
    tlv_get_str(&t, TAG_MODEL, model, sizeof model);
    tlv_get_str(&t, TAG_FW_VER, fw, sizeof fw);
    tlv_get_u64(&t, TAG_NONCE_DEV, &nd);
    if (!*serial || !*model) {
        s->c.rx_malformed++;
        return;
    }

    /* idempotency (§7.3): the same (serial, Nd) gets the cached reply, from a tombstone or a session */
    srv_tombstone_t *tb = tomb_find(s, 0, serial, nd);
    if (tb) {
        srv_send(s, s->ctl_fd, tb->bytes, tb->len, src);
        return;
    }
    srv_session_t *old = srv_sess_find_half_open(s, serial);
    if (old && old->nd == nd) {
        if (old->cached_hello_ack_len) srv_send(s, s->ctl_fd, old->cached_hello_ack, old->cached_hello_ack_len, src);
        LOGD(T_UDP, "HELLO retransmission from %s (%s): cached HELLO_ACK resent", serial, a);
        return;
    }
    if (!srv_rate_ok(s, src)) {
        log_limited(2, LOG_INFO, T_UDP, "HELLO from %s rate-limited", a);
        return;
    }
    const uint8_t *k_mfg = srv_keys_model(&s->keys, model);
    if (!k_mfg) {
        LOGI(T_AUTH, "HELLO from %s (%s): UNKNOWN_MODEL %s", serial, a, model);
        terminal_error(s, 0, 0, serial, nd, RC_UNKNOWN_MODEL, "no key for model", src);
        return;
    }
    if (old) srv_sess_free(s, old, "replaced by a HELLO with a new nonce");   /* half-open only (§7.3) */
    srv_session_t *ss = srv_sess_new(s, serial, model);
    if (!ss) {
        LOGW(T_UDP, "HELLO from %s (%s): session table full, SERVER_BUSY", serial, a);
        terminal_error(s, 0, 0, serial, nd, RC_SERVER_BUSY, "session table full", src);
        return;
    }
    snprintf(ss->fw, sizeof ss->fw, "%s", fw);
    ss->peer = *src;
    ss->nd = nd;
    ss->ns = rand_u64();
    ztp_derive_kdev(k_mfg, serial, ss->k_dev);
    ss->has_k_dev = true;
    ztp_derive_ks(ss->k_dev, ss->nd, ss->ns, ss->session_id, 0, ss->k_s);
    srv_policy_resolve(&s->policy, &s->cfg, model, serial, &ss->assign);
    ss->cur_key = s->keys.k_boot;
    ss->cur_key_id = ZK_BOOT;
    rel_on_rx(ss->rel, h, buf + ZTP_HDR_LEN, s->now);   /* delivered as seq 1 -> HELLO_ACK */
}

static void send_hello_ack(srv_session_t *ss)
{
    srv_t *s = ss->srv;
    uint8_t pl[128];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u64(&w, TAG_NONCE_DEV, ss->nd);
    tlv_put_u64(&w, TAG_NONCE_SRV, ss->ns);
    tlv_put_u16(&w, TAG_KEEPALIVE_S, (uint16_t)ss->assign.nat.keepalive_interval_s);
    tlv_put_u8(&w, TAG_DEAD_INTERVALS, (uint8_t)ss->assign.nat.dead_intervals);
    tlv_put_u16(&w, TAG_GRACE_S, (uint16_t)ss->assign.nat.grace_s);
    rel_msg_t m = {ZT_HELLO_ACK, 0, false, s->keys.k_boot, ZK_BOOT};
    rel_send(ss->rel, &m, pl, (uint16_t)w.len, s->now);
    log_sess(LOG_INFO, T_UDP, ss->session_id, ss->serial, LM_UDP_SRV_HELLO, ss->serial, ss->model, ss->fw);
}

/* ---- in-order delivery from a session's engine ---- */

void srv_sess_deliver(void *ctx, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t *p, uint16_t n)
{
    srv_session_t *ss = ctx;
    srv_t *s = ss->srv;
    tlv_set_t t;
    if (type == ZT_HELLO) {
        send_hello_ack(ss);
        return;
    }
    if (tlv_parse(p, n, &t) != ZTP_OK) {
        s->c.rx_malformed++;
        return;
    }
    switch (type) {
    case ZT_KEEPALIVE: {
        uint8_t pl[8];
        tlv_writer_t w;
        tlv_writer_init(&w, pl, sizeof pl);
        tlv_put_u8(&w, TAG_SRV_STAGE, ss->db_stage);
        rel_msg_t m = {ZT_KEEPALIVE_ACK, 0, false, ss->cur_key, ss->cur_key_id};   /* same key as the KEEPALIVE */
        rel_send_ackclass(ss->rel, &m, pl, (uint16_t)w.len, false);
        if (tlv_has(&t, TAG_PATH_PROBE)) log_sess(LOG_INFO, T_NAT, ss->session_id, ss->serial, LM_NAT_PROBE_ACKED);
        else log_sess(LOG_DEBUG, T_NAT, ss->session_id, ss->serial, "Keepalive acknowledged");
        break;
    }
    default:
        /* REGISTER and later messages arrive in M3+ */
        log_sess(LOG_DEBUG, T_UDP, ss->session_id, ss->serial, "%s seq=%u not handled yet", ztp_type_name(type),
                 (unsigned)seq);
        break;
    }
}

/* ---- RESUME HELLO for an unknown session (§5.6 step 5) ---- */

static void on_unknown_resume(srv_t *s, const uint8_t *buf, size_t len, const ztp_hdr_t *h, const struct sockaddr_in *src)
{
    if (!srv_rate_ok(s, src)) return;
    char model[17], serial[17];
    const uint8_t *pl = buf + ZTP_HDR_LEN;
    if (!tlv_peek_str(pl, h->payload_len, TAG_MODEL, model, sizeof model) ||
        !tlv_peek_str(pl, h->payload_len, TAG_SERIAL, serial, sizeof serial)) {
        s->c.rx_unknown++;
        return;
    }
    const uint8_t *k_mfg = srv_keys_model(&s->keys, model);
    if (!k_mfg) { s->c.rx_unknown++; return; }
    uint8_t k_dev[ZTP_KEY_LEN];
    ztp_derive_kdev(k_mfg, serial, k_dev);
    int ok = ztp_mac_verify(buf, len, k_dev);
    secure_zero(k_dev, sizeof k_dev);
    if (!ok) { s->c.mac_fail++; return; }
    tlv_set_t t;
    uint64_t nd2 = 0;
    if (tlv_parse(pl, h->payload_len, &t) != ZTP_OK || !tlv_get_u64(&t, TAG_NONCE_DEV, &nd2)) {
        s->c.rx_malformed++;
        return;
    }
    srv_tombstone_t *tb = tomb_find(s, h->session_id, serial, nd2);
    if (tb) { srv_send(s, s->ctl_fd, tb->bytes, tb->len, src); return; }
    /* M5 adds expired-session tombstones (SESSION_EXPIRED); unknown is all M2 can tell */
    log_sess(LOG_INFO, T_UDP, h->session_id, serial, "Resume for an unknown session: SESSION_UNKNOWN");
    terminal_error(s, h->session_id, h->epoch, serial, nd2, RC_SESSION_UNKNOWN, "unknown session", src);
}

/* ---- control socket pipeline (§5.7) ---- */

void srv_on_ctl_datagram(srv_t *s, const uint8_t *buf, size_t len, const struct sockaddr_in *src)
{
    ztp_hdr_t h;
    s->cur_src = *src;
    int rc = ztp_hdr_decode(buf, len, ZTP_MAX_DGRAM_CAP, ZD_D2S, &h);   /* steps 1-4 */
    if (rc != ZTP_OK) {
        s->c.rx_malformed++;
        LOGD(T_UDP, "malformed datagram (%d), %zu bytes", rc, len);
        return;
    }
    LOGD(T_UDP, "rx %s sid=%u seq=%u ack=%u len=%zu", ztp_type_name(h.type), (unsigned)h.session_id, (unsigned)h.seq,
         (unsigned)h.ack, len);
    if (h.session_id == 0) {   /* pre-session path */
        if (h.type == ZT_HELLO && !(h.flags & ZF_RESUME)) on_fresh_hello(s, buf, len, &h, src);
        else s->c.rx_malformed++;
        return;
    }
    srv_session_t *ss = srv_sess_find(s, h.session_id);
    if (!ss) {
        if (h.type == ZT_HELLO && (h.flags & ZF_RESUME)) on_unknown_resume(s, buf, len, &h, src);
        else s->c.rx_unknown++;
        return;
    }
    if (h.type == ZT_HELLO && (h.flags & ZF_RESUME)) {
        log_sess(LOG_DEBUG, T_UDP, ss->session_id, ss->serial, "resume HELLO: handled from M5");
        return;
    }
    if (h.epoch != ss->epoch) {
        s->c.rx_stale++;
        return;
    }
    /* step 6: key by message type and session state, never by key_id */
    const uint8_t *key;
    uint8_t key_id;
    if (ss->st == SS_HALF_OPEN && (h.type == ZT_KEEPALIVE || h.type == ZT_ACK)) {
        key = s->keys.k_boot;
        key_id = ZK_BOOT;
    } else {
        key = ss->k_s;
        key_id = ZK_SESSION;
    }
    if (!ztp_mac_verify(buf, len, key)) {
        s->c.mac_fail++;
        log_limited(3, LOG_DEBUG, T_UDP, "sid=%u %s: MAC invalid, dropped", (unsigned)h.session_id, ztp_type_name(h.type));
        return;
    }
    uint64_t prev_rx = ss->last_rx_ms;
    ss->last_rx_ms = s->now;
    const ztp_type_info_t *ti = ztp_type_info(h.type);
    if (ti->cls == ZC_RELIABLE) {
        /* step 8: only a never-seen seq may move the peer (§9.4) */
        if (!addr_eq(src, &ss->peer) && rel_classify(ss->rel, h.seq) == REL_RX_NEW) {
            char from[ADDR_STR], to[ADDR_STR];
            addr_fmt(&ss->peer, from);
            addr_fmt(src, to);
            if (src->sin_addr.s_addr == ss->peer.sin_addr.s_addr)
                log_sess(LOG_INFO, T_NAT, ss->session_id, ss->serial, LM_NAT_PEER_CHANGED, from, to);
            else
                log_sess(LOG_INFO, T_NAT, ss->session_id, ss->serial, LM_NAT_IP_CHANGED_OK, from, to);
            ss->peer = *src;
            /* §9.7: a rebinding after an idle gap longer than the keepalive interval means the NAT's
             * idle timeout is probably shorter than the keepalive */
            uint64_t gap = s->now - prev_rx, ka = (uint64_t)ss->assign.nat.keepalive_interval_s * 1000u;
            if (s->cfg.c.nat.rebind_advice && gap > ka)
                log_sess(LOG_INFO, T_NAT, ss->session_id, ss->serial, LM_NAT_REBIND_ADVICE, (unsigned)((gap + 500) / 1000),
                         (unsigned)ss->assign.nat.keepalive_interval_s);
        }
        ss->cur_key = key;
        ss->cur_key_id = key_id;
        rel_on_rx(ss->rel, &h, buf + ZTP_HDR_LEN, s->now);
    } else {
        rel_on_ack(ss->rel, &h, s->now);
    }
    if (!ss->established_logged && rel_acked_through(ss->rel) >= 1) {   /* HELLO_ACK acknowledged */
        ss->established_logged = true;
        log_sess(LOG_INFO, T_UDP, ss->session_id, ss->serial, LM_UDP_SRV_ESTABLISHED);
    }
}

/* ---- discovery socket (§6.2) ---- */

void srv_on_disc_datagram(srv_t *s, const uint8_t *buf, size_t len, const struct sockaddr_in *src)
{
    ztp_hdr_t h;
    if (ztp_hdr_decode(buf, len, ZTP_MAX_DGRAM_CAP, ZD_D2S, &h) != ZTP_OK || h.type != ZT_DISCOVER || h.session_id) {
        s->c.rx_malformed++;
        return;
    }
    if (!ztp_mac_verify(buf, len, s->keys.k_boot)) {
        s->c.mac_fail++;
        return;
    }
    if (!srv_rate_ok(s, src)) return;
    tlv_set_t t;
    uint64_t dn = 0;
    char serial[17] = "";
    if (tlv_parse(buf + ZTP_HDR_LEN, h.payload_len, &t) != ZTP_OK || !tlv_get_u64(&t, TAG_DISC_NONCE, &dn)) {
        s->c.rx_malformed++;
        return;
    }
    tlv_get_str(&t, TAG_SERIAL, serial, sizeof serial);
    struct in_addr me;
    struct sockaddr_in bind_a;
    addr_parse(s->cfg.c.net.bind, 0, &bind_a);
    if (bind_a.sin_addr.s_addr != htonl(INADDR_ANY)) me = bind_a.sin_addr;
    else if (!local_addr_for(src, &me)) return;

    uint8_t pl[64];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u64(&w, TAG_DISC_NONCE, dn);
    tlv_put_bytes(&w, TAG_SERVER_ADDR, &me.s_addr, 4);   /* network byte order */
    tlv_put_u16(&w, TAG_SERVER_PORT, (uint16_t)s->cfg.c.net.server_port);
    ztp_hdr_t o = {0};
    o.type = ZT_OFFER;
    o.seq = h.seq;   /* datagram class: echo the attempt number */
    o.key_id = ZK_BOOT;
    uint8_t frame[256];
    int n = ztp_frame_build(frame, sizeof frame, ZTP_MAX_DGRAM_CAP, &o, pl, (uint16_t)w.len, s->keys.k_boot);
    if (n > 0) srv_send(s, s->disc_fd, frame, (size_t)n, src);
    char a[ADDR_STR], sa[ADDR_STR];
    struct sockaddr_in me_a = {0};
    me_a.sin_family = AF_INET;
    me_a.sin_addr = me;
    me_a.sin_port = htons((uint16_t)s->cfg.c.net.server_port);
    addr_fmt(src, a);
    addr_fmt(&me_a, sa);
    LOGI(T_DISCOVERY, "DISCOVER from %s (%s), attempt %u: OFFER sent (server %s)", *serial ? serial : "?", a,
         (unsigned)h.seq, sa);
}
