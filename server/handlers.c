/* Server receive pipeline (DESIGN §5.6, §5.7) and per-message handlers. M2: DISCOVER/OFFER,
 * fresh HELLO/HELLO_ACK, path-probe and periodic KEEPALIVE, ACK, unknown-session resume. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "logmsg.h"
#include "rand.h"
#include "srv.h"
#include "timer.h"

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

/* ---- REGISTER nonce cache (§10.4 mechanism 3): per serial at most nonce_cache_per_serial entries,
 * TTL nonce_ttl_s, nonce_cache_total overall (a ring: the oldest entry is overwritten) ---- */

bool srv_nonce_seen(srv_t *s, const char *serial, uint64_t nonce)
{
    const ztp_config_t *c = &s->cfg.c;
    uint64_t ttl = (uint64_t)c->limits.nonce_ttl_s * 1000u;
    size_t per = 0, oldest = SIZE_MAX;
    for (size_t i = 0; i < s->nonce_cap; i++) {
        srv_nonce_t *e = &s->nonces[i];
        if (!e->used || s->now - e->ts_ms > ttl || strcmp(e->serial, serial) != 0) continue;
        if (e->nonce == nonce) return true;
        per++;
        if (oldest == SIZE_MAX || e->ts_ms < s->nonces[oldest].ts_ms) oldest = i;
    }
    srv_nonce_t *slot;
    if (per >= (size_t)c->limits.nonce_cache_per_serial && oldest != SIZE_MAX) slot = &s->nonces[oldest];
    else {
        slot = &s->nonces[s->nonce_next];
        s->nonce_next = (s->nonce_next + 1) % s->nonce_cap;
    }
    slot->used = true;
    slot->nonce = nonce;
    slot->ts_ms = s->now;
    snprintf(slot->serial, sizeof slot->serial, "%s", serial);
    return false;
}

/* REGISTER_NAK: terminal, K_boot, echoes NONCE_REG, sent to the source; tombstoned by session id so a
 * retransmitted REGISTER gets the identical bytes after the session is gone (§5.3). */
static void send_nak(srv_t *s, srv_session_t *ss, uint16_t reason, const char *detail, uint64_t nonce_reg)
{
    uint8_t pl[128];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u16(&w, TAG_REASON, reason);
    tlv_put_str(&w, TAG_DETAIL, detail);
    tlv_put_u64(&w, TAG_NONCE_REG, nonce_reg);
    srv_tombstone_t *t = tomb_new(s, ss->session_id, ss->serial, 0);
    send_terminal(s, s->ctl_fd, ZT_REGISTER_NAK, ss->session_id, ss->epoch, pl, (uint16_t)w.len, &s->cur_src, t->bytes,
                  &t->len);
    ss->doomed = true;   /* the half-open session is discarded; only the tombstone remains */
}

static void db_row_log(srv_t *s, const char *serial)
{
    db_rec_t *r = db_get(&s->db, serial);
    if (!r) return;
    LOGI(T_DB, LM_DB_ROW, r->serial, r->status == DB_SUCCESS ? "Success" : "Failed",
         r->status == DB_SUCCESS ? r->fw_assigned : "-", (unsigned)(r->status == DB_SUCCESS ? r->cfg_version : 0));
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
        db_register_fail(&s->db, serial, model, "Unknown model", a, wall_ms());   /* identity unverified: capped */
        db_row_log(s, serial);
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

static void send_bad_request(srv_session_t *ss, uint32_t seq, const char *detail)
{
    srv_t *s = ss->srv;
    uint8_t pl[96];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u16(&w, TAG_REASON, RC_BAD_REQUEST);
    tlv_put_str(&w, TAG_DETAIL, detail);
    tlv_put_u32(&w, TAG_ECHO_SEQ, seq);
    rel_msg_t m = {ZT_ERROR, 0, false, ss->k_s, ZK_SESSION};   /* BAD_REQUEST is the one reliable ERROR */
    rel_send(ss->rel, &m, pl, (uint16_t)w.len, s->now);
    log_sess(LOG_INFO, T_UDP, ss->session_id, ss->serial, "BAD_REQUEST for seq %u: %s", (unsigned)seq, detail);
}

static void send_register_ack(srv_session_t *ss, bool already)
{
    srv_t *s = ss->srv;
    uint8_t pl[96];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_str(&w, TAG_TENANT, ss->assign.tenant);
    tlv_put_str(&w, TAG_FW_TARGET, ss->assign.firmware);
    tlv_put_u16(&w, TAG_CFG_VER, (uint16_t)ss->assign.config_version);
    if (already) tlv_put_u16(&w, TAG_INFO, RC_ALREADY_REGISTERED);
    rel_msg_t m = {ZT_REGISTER_ACK, 0, false, ss->k_s, ZK_SESSION};
    rel_send(ss->rel, &m, pl, (uint16_t)w.len, s->now);
}

/* ---- transfer (§11.2) and the small-MTU fallback (§5.8) ---- */

/* A CHUNK_DATA that no longer fits becomes a 52 B fill frame at the same seq (§5.8 step 2). */
static uint16_t fill_frame(void *ctx, uint8_t type, const uint8_t *p, uint16_t len, uint8_t *out)
{
    if (type != ZT_CHUNK_DATA || len < ZTP_CHUNK_HDR_LEN) {
        memcpy(out, p, len);
        return len;
    }
    ztp_chunk_t c = {.file_id = p[0], .flags = CHUNK_F_MTU_REDUCED, .chunk_index = be32_get(p + 2), .data_len = 0};
    return (uint16_t)ztp_chunk_encode(out, ZTP_CHUNK_HDR_LEN, &c);
}

static void step_down(srv_session_t *ss, bool emsgsize)
{
    srv_t *s = ss->srv;
    if (ss->max_dgram <= ZTP_MIN_DGRAM) return;
    if (!s->cfg.c.net.mtu_fallback) {
        log_sess(LOG_WARN, T_UDP, ss->session_id, ss->serial,
                 "Datagram too big for the path (%s) and net.mtu_fallback is off: transfer cannot continue",
                 emsgsize ? "EMSGSIZE" : "device request");
        return;
    }
    ss->max_dgram = ZTP_MIN_DGRAM;
    int dropped = rel_step_down(ss->rel, ZTP_MIN_DGRAM, fill_frame, ss, s->now);
    ss->nreq = 0;   /* requests at the old chunk size: the device re-requests after the new MANIFEST */
    s->c.mtu_stepdowns++;
    if (emsgsize) log_sess(LOG_INFO, T_UDP, ss->session_id, ss->serial, LM_UDP_MTU_EMSGSIZE);
    else log_sess(LOG_INFO, T_UDP, ss->session_id, ss->serial, "Device reports a small path MTU: datagram limit now 508");
    log_sess(LOG_DEBUG, T_UDP, ss->session_id, ss->serial, "%d queued frames dropped at the step-down", dropped);
    db_prov_note(&s->db, ss->serial, "Datagram limit reduced to 508", wall_ms());
    srv_sess_journal(s, ss);
}

void srv_sess_pump(srv_session_t *ss)
{
    srv_t *s = ss->srv;
    while (ss->nreq && !ss->doomed && rel_in_flight(ss->rel) + rel_queued(ss->rel) < (uint32_t)s->relcfg.window) {
        const srv_artifact_t *a = srv_artifact_by_id(&s->artifacts, ss->req[0].file_id);
        if (!a || (size_t)ZTP_HDR_LEN + ZTP_CHUNK_HDR_LEN + ss->chunk_size > ss->max_dgram) {
            ss->nreq = 0;   /* stale request at an old chunk size (§5.8) */
            break;
        }
        uint8_t data[ZTP_CHUNK_SIZE_CAP], pl[ZTP_MAX_PAYLOAD];
        uint32_t idx = ss->req[0].next++;
        int n = srv_artifact_read(a, idx, ss->chunk_size, data);
        if (ss->req[0].next >= ss->req[0].end) {
            memmove(&ss->req[0], &ss->req[1], (size_t)(ss->nreq - 1) * sizeof ss->req[0]);
            ss->nreq--;
        }
        if (n < 0) continue;
        ztp_chunk_t c = {.file_id = a->file_id, .chunk_index = idx, .data_len = (uint16_t)n, .data = data};
        int pl_len = ztp_chunk_encode(pl, sizeof pl, &c);
        rel_msg_t m = {ZT_CHUNK_DATA, 0, false, ss->k_s, ZK_SESSION};
        rel_send(ss->rel, &m, pl, (uint16_t)pl_len, s->now);
        if (rel_take_tx_error(ss->rel) == -EMSGSIZE) step_down(ss, true);
    }
}

static const srv_artifact_t *assigned(srv_session_t *ss, uint8_t kind)
{
    char v[9];
    if (kind == FK_CONFIG) {
        snprintf(v, sizeof v, "v%d", ss->assign.config_version);
        return srv_artifact_for(&ss->srv->artifacts, FK_CONFIG, ss->assign.config_file, v);
    }
    return srv_artifact_for(&ss->srv->artifacts, FK_FIRMWARE, ss->assign.firmware_file, ss->assign.firmware);
}

static void on_manifest_req(srv_session_t *ss, uint32_t seq, const tlv_set_t *t)
{
    srv_t *s = ss->srv;
    uint8_t kind = 0;
    uint16_t want = 0;
    if (!tlv_get_u8(t, TAG_FILE_KIND, &kind) || !tlv_get_u16(t, TAG_MAX_DGRAM, &want) ||
        (kind != FK_CONFIG && kind != FK_FIRMWARE)) {
        send_bad_request(ss, seq, "MANIFEST_REQ needs FILE_KIND (1 or 2) and MAX_DGRAM");
        return;
    }
    if (want < ss->max_dgram && want >= ZTP_MIN_DGRAM) step_down(ss, false);   /* black hole seen by the device */
    const srv_artifact_t *a = assigned(ss, kind);
    if (!a) {
        send_bad_request(ss, seq, "no artifact of this kind is assigned");
        return;
    }
    uint32_t lim = want < ss->max_dgram ? want : ss->max_dgram, cs = (uint32_t)s->cfg.c.xfer.chunk_size;
    while (cs > ZTP_CHUNK_SIZE_MIN && ZTP_HDR_LEN + ZTP_CHUNK_HDR_LEN + cs > lim) cs /= 2;
    ss->chunk_size = cs;
    ss->nreq = 0;
    uint32_t count = (a->size + cs - 1) / cs;
    uint8_t pl[128];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u8(&w, TAG_FILE_ID, a->file_id);
    tlv_put_u8(&w, TAG_FILE_KIND, kind);
    tlv_put_str(&w, TAG_VERSION, a->version);
    tlv_put_u32(&w, TAG_FILE_SIZE, a->size);
    tlv_put_u16(&w, TAG_CHUNK_SIZE, (uint16_t)cs);
    tlv_put_u32(&w, TAG_CHUNK_COUNT, count);
    tlv_put_bytes(&w, TAG_SHA256, a->sha256, 32);
    rel_msg_t m = {ZT_MANIFEST, 0, false, ss->k_s, ZK_SESSION};
    rel_send(ss->rel, &m, pl, (uint16_t)w.len, s->now);
    if (ss->st == SS_REGISTERED) {
        srv_sess_set_state(s, ss, SS_PROVISIONING);
        srv_sess_journal(s, ss);
    }
    if (kind == FK_CONFIG)
        log_sess(LOG_INFO, T_PROVISION, ss->session_id, ss->serial, LM_PROV_SEND_CONFIG, (unsigned)ss->assign.config_version,
                 (unsigned)count);
    else
        log_sess(LOG_INFO, T_PROVISION, ss->session_id, ss->serial, LM_PROV_SEND_FW, a->version, (unsigned)count);
}

static void on_chunk_req(srv_session_t *ss, uint32_t seq, const tlv_set_t *t)
{
    srv_t *s = ss->srv;
    uint8_t id = 0;
    uint32_t start = 0;
    uint16_t num = 0;
    const srv_artifact_t *a;
    if (!tlv_get_u8(t, TAG_FILE_ID, &id) || !tlv_get_u32(t, TAG_CHUNK_START, &start) ||
        !tlv_get_u16(t, TAG_CHUNK_NUM, &num) || !(a = srv_artifact_by_id(&s->artifacts, id)) ||
        (a != assigned(ss, a->kind)) || !ss->chunk_size) {
        send_bad_request(ss, seq, "CHUNK_REQ for a file that is not this session's");
        return;
    }
    uint32_t count = (a->size + ss->chunk_size - 1) / ss->chunk_size;
    if (num == 0 || num > s->cfg.c.xfer.chunk_req_max || start >= count || num > count - start) {
        send_bad_request(ss, seq, "CHUNK_REQ range out of bounds");
        return;
    }
    if (ss->nreq >= 4) {   /* the device pipelines at most a couple of runs; drop the oldest */
        memmove(&ss->req[0], &ss->req[1], 3 * sizeof ss->req[0]);
        ss->nreq = 3;
    }
    ss->req[ss->nreq].file_id = id;
    ss->req[ss->nreq].next = start;
    ss->req[ss->nreq].end = start + num;
    ss->nreq++;
    log_sess(LOG_DEBUG, T_PROVISION, ss->session_id, ss->serial, "CHUNK_REQ file %u chunks %u..%u", (unsigned)id,
             (unsigned)start, (unsigned)(start + num - 1));
}

static void on_transfer_done(srv_session_t *ss, const tlv_set_t *t)
{
    srv_t *s = ss->srv;
    uint8_t id = 0;
    uint16_t result = 0;
    tlv_get_u8(t, TAG_FILE_ID, &id);
    tlv_get_u16(t, TAG_RESULT, &result);
    const srv_artifact_t *a = srv_artifact_by_id(&s->artifacts, id);
    if (!a) return;
    if (result == RC_OK) {
        db_stage(&s->db, ss->serial, a->kind == FK_CONFIG ? SST_CONFIG_SENT : SST_FW_SENT, wall_ms());
        ss->db_stage = a->kind == FK_CONFIG ? SST_CONFIG_SENT : SST_FW_SENT;
        log_sess(LOG_INFO, T_PROVISION, ss->session_id, ss->serial, "%s %s delivered and verified by the device (max in flight %u)",
                 a->kind == FK_CONFIG ? "Config" : "Firmware", a->version, (unsigned)rel_max_in_flight(ss->rel));
    } else {
        char note[64];
        snprintf(note, sizeof note, "Transfer of %s failed: %s", a->version, ztp_reason_name(result));
        db_prov_note(&s->db, ss->serial, note, wall_ms());
        log_sess(LOG_INFO, T_PROVISION, ss->session_id, ss->serial, "%s", note);
    }
}

static void on_status_report(srv_session_t *ss, const tlv_set_t *t)
{
    srv_t *s = ss->srv;
    uint8_t st = 0;
    uint16_t reason = 0;
    tlv_get_u8(t, TAG_REPORT_STATE, &st);
    tlv_get_u16(t, TAG_REASON, &reason);
    if (st == RPT_FAILED) {
        db_stage(&s->db, ss->serial, SST_FAILED, wall_ms());
        ss->db_stage = SST_FAILED;
        char note[64];
        snprintf(note, sizeof note, "Device failed: %s", ztp_reason_name(reason));
        db_prov_note(&s->db, ss->serial, note, wall_ms());
        srv_sess_set_state(s, ss, SS_FAILED);
        srv_sess_journal(s, ss);
        log_sess(LOG_INFO, T_STATE, ss->session_id, ss->serial, "FAILED (%s)", ztp_reason_name(reason));
    } else if (st == RPT_OPERATIONAL) {
        db_stage(&s->db, ss->serial, SST_OPERATIONAL, wall_ms());
        ss->db_stage = SST_OPERATIONAL;
        srv_sess_set_state(s, ss, SS_OPERATIONAL);
        srv_sess_journal(s, ss);
        log_sess(LOG_INFO, T_STATE, ss->session_id, ss->serial, LM_STATE_SRV_OPERATIONAL);
    }
    rel_msg_t m = {ZT_STATUS_ACK, 0, false, ss->k_s, ZK_SESSION};
    rel_send(ss->rel, &m, NULL, 0, s->now);
}

/* §10.2, §10.3: a REGISTER that passed the K_s MAC check, delivered in order. */
static void on_register(srv_session_t *ss, uint32_t seq, const tlv_set_t *t)
{
    srv_t *s = ss->srv;
    static const uint8_t need[] = {TAG_SERIAL, TAG_MODEL, TAG_FW_VER, TAG_HW_REV, TAG_NONCE_REG, TAG_NONCE_SRV};
    uint8_t missing = 0;
    if (tlv_require(t, need, sizeof need, &missing) != ZTP_OK) {
        char d[48];
        snprintf(d, sizeof d, "missing %s", ztp_tag_info(missing) ? ztp_tag_info(missing)->name : "tag");
        send_bad_request(ss, seq, d);
        return;
    }
    char serial[17], model[17], fw[9], hw[9];
    uint64_t nreg = 0, ns = 0;
    tlv_get_str(t, TAG_SERIAL, serial, sizeof serial);
    tlv_get_str(t, TAG_MODEL, model, sizeof model);
    tlv_get_str(t, TAG_FW_VER, fw, sizeof fw);
    tlv_get_str(t, TAG_HW_REV, hw, sizeof hw);
    tlv_get_u64(t, TAG_NONCE_REG, &nreg);
    tlv_get_u64(t, TAG_NONCE_SRV, &ns);
    if (strcmp(serial, ss->serial) || strcmp(model, ss->model) || ns != ss->ns) {
        send_bad_request(ss, seq, "SERIAL, MODEL or NONCE_SRV differs from the HELLO");
        return;
    }
    if (ss->st != SS_HALF_OPEN) {   /* a second REGISTER in a registered session: same answer, no new attempt */
        send_register_ack(ss, true);
        return;
    }
    char src[ADDR_STR];
    addr_fmt(&s->cur_src, src);
    uint64_t wall = wall_ms();
    if (srv_nonce_seen(s, serial, nreg)) {
        s->c.replay_rejected++;
        log_sess(LOG_INFO, T_AUTH, ss->session_id, serial, "Registration rejected: REPLAY (nonce %llu already seen)",
                 (unsigned long long)nreg);
        db_register_fail(&s->db, serial, model, "Replayed nonce", src, wall);
        send_nak(s, ss, RC_REPLAY, "nonce reused", nreg);
        return;
    }
    if (!ss->assign.assigned) {
        log_sess(LOG_INFO, T_AUTH, ss->session_id, serial, LM_AUTH_FAIL_NO_POLICY);
        db_register_fail(&s->db, serial, model, "No policy", src, wall);
        db_row_log(s, serial);
        send_nak(s, ss, RC_NO_POLICY, "no assignment for this model", nreg);
        return;
    }
    /* success: one registered session per serial; a verified REGISTER supersedes an older one (§14.2) */
    srv_session_t *old = srv_sess_find_registered(s, serial);
    if (old && old != ss) {
        log_sess(LOG_INFO, T_UDP, ss->session_id, serial, LM_UDP_SUPERSEDED, (unsigned)old->session_id,
                 (unsigned)ss->session_id);
        srv_sess_free(s, old, "superseded");
    }
    db_rec_t *prev = db_get(&s->db, serial);
    bool already = prev && prev->ever_success;
    srv_sess_set_state(s, ss, SS_REGISTERED);
    ss->ks_verified = true;
    ss->db_stage = SST_REGISTERED;
    rel_set_ack_key(ss->rel, ss->k_s, ZK_SESSION);
    db_register_ok(&s->db, serial, model, hw, fw, ss->assign.firmware, ss->assign.config_version, ss->assign.tenant,
                   ss->assign.notes, src, wall);
    srv_sess_journal(s, ss);
    send_register_ack(ss, already);
    log_sess(LOG_INFO, T_AUTH, ss->session_id, serial, LM_AUTH_REG_OK);
    log_sess(LOG_INFO, T_AUTH, ss->session_id, serial, LM_AUTH_SRV_TENANT, ss->assign.tenant, ss->assign.firmware,
             (unsigned)ss->assign.config_version);
    log_sess(LOG_INFO, T_PROVISION, ss->session_id, serial, LM_PROV_POLICY, model, ss->assign.firmware,
             (unsigned)ss->assign.config_version, (unsigned)ss->assign.nat.keepalive_interval_s,
             (unsigned)ss->assign.nat.grace_s, ss->assign.src_keepalive);
    db_row_log(s, serial);
}

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
    case ZT_REGISTER:
        on_register(ss, seq, &t);
        break;
    case ZT_MANIFEST_REQ:
        if (ss->st != SS_HALF_OPEN) on_manifest_req(ss, seq, &t);
        break;
    case ZT_CHUNK_REQ:
        if (ss->st != SS_HALF_OPEN) on_chunk_req(ss, seq, &t);
        break;
    case ZT_TRANSFER_DONE:
        if (ss->st != SS_HALF_OPEN) on_transfer_done(ss, &t);
        break;
    case ZT_STATUS_REPORT:
        if (ss->st != SS_HALF_OPEN) on_status_report(ss, &t);
        break;
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
    /* a session deleted after its grace period leaves a 24-hour tombstone: SESSION_EXPIRED (§14.4) */
    size_t nj;
    const db_sess_t *js = db_sessions(&s->db, &nj);
    bool expired = false;
    for (size_t i = 0; i < nj; i++)
        if (js[i].sid == h->session_id && js[i].deleted && wall_ms() - js[i].ts_ms < 24u * 3600u * 1000u) expired = true;
    log_sess(LOG_INFO, T_UDP, h->session_id, serial, "Resume for an unknown session: %s",
             expired ? "SESSION_EXPIRED" : "SESSION_UNKNOWN");
    terminal_error(s, h->session_id, h->epoch, serial, nd2, expired ? RC_SESSION_EXPIRED : RC_SESSION_UNKNOWN,
                   expired ? "session expired" : "unknown session", src);
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
        if (h.type == ZT_HELLO && (h.flags & ZF_RESUME)) {
            on_unknown_resume(s, buf, len, &h, src);
            return;
        }
        if (h.type == ZT_REGISTER)   /* a retransmitted REGISTER after a NAK: resend the identical NAK */
            for (int i = 0; i < SRV_TOMBSTONES; i++) {
                srv_tombstone_t *t = &s->tomb[i];
                if (t->used && t->sid == h.session_id && t->expires_ms > s->now && t->len) {
                    srv_send(s, s->ctl_fd, t->bytes, t->len, src);
                    return;
                }
            }
        s->c.rx_unknown++;
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
    if (ss->st == SS_IDLE_GRACE) {   /* only a resume HELLO revives it (M5) */
        s->c.rx_stale++;
        return;
    }
    if (ss->st == SS_HALF_OPEN && (h.type == ZT_KEEPALIVE || h.type == ZT_ACK)) {
        /* §5.3: until the session's first K_s frame is verified, an ACK or KEEPALIVE may come under either
         * key: the device switches to K_s as soon as it sends REGISTER, which may not have arrived yet */
        key = s->keys.k_boot;
        key_id = ZK_BOOT;
        if (!ztp_mac_verify(buf, len, key) && ztp_mac_verify(buf, len, ss->k_s)) {
            key = ss->k_s;
            key_id = ZK_SESSION;
        }
    } else {
        key = ss->k_s;
        key_id = ZK_SESSION;
    }
    if (!ztp_mac_verify(buf, len, key)) {
        s->c.mac_fail++;
        if (h.type == ZT_REGISTER && ss->st == SS_HALF_OPEN && rel_classify(ss->rel, h.seq) == REL_RX_NEW) {
            /* §10.3: a REGISTER failing K_s in a half-open session is a verified credential failure for
             * the claimed serial: record it, NAK under K_boot echoing NONCE_REG, discard the session */
            tlv_set_t t;
            uint64_t nreg = 0;
            if (tlv_parse(buf + ZTP_HDR_LEN, h.payload_len, &t) == ZTP_OK) tlv_get_u64(&t, TAG_NONCE_REG, &nreg);
            char a[ADDR_STR];
            addr_fmt(src, a);
            log_sess(LOG_INFO, T_AUTH, ss->session_id, ss->serial, "Registration failed: Invalid HMAC");
            db_register_fail(&s->db, ss->serial, ss->model, "Invalid HMAC", a, wall_ms());
            db_row_log(s, ss->serial);
            send_nak(s, ss, RC_BAD_MAC, "Invalid HMAC", nreg);
            srv_sess_free(s, ss, NULL);
            return;
        }
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
    if (ss->st != SS_HALF_OPEN) db_seen(&s->db, ss->serial, NULL, wall_ms());
    if (!ss->doomed) srv_sess_pump(ss);
    if (ss->doomed) srv_sess_free(s, ss, NULL);   /* a terminal reply was sent from inside delivery */
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
