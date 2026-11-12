/* Device session layer (DESIGN §7, §9, §12.3): fresh HELLO / HELLO_ACK, path probe, REGISTER,
 * resume (K_dev), periodic and health-probe keepalives, liveness, terminal errors. */
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "dev.h"
#include "log.h"
#include "logmsg.h"
#include "rand.h"

static int sess_tx(void *ctx, const uint8_t *frame, size_t len, bool to_source)
{
    device_t *d = ctx;
    return dev_send(d, frame, len, &d->server);
}

void dev_send_keepalive(device_t *d, int kind)
{
    uint8_t pl[8];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    if (kind == KA_PATH_PROBE) tlv_put_u8(&w, TAG_PATH_PROBE, 1);
    if (kind == KA_HEALTH_PROBE) tlv_put_u8(&w, TAG_HEALTH_PROBE, 1);
    /* K_boot for the path probe (half-open); K_s once REGISTER has been sent or the session resumed (§5.3) */
    /* periodic and health probes are liveness traffic, exempt from rel.max_retries (§8.3); the path probe is a
     * setup step that REGISTER waits for, so it has the HELLO's retry budget (E6a) instead of the dead interval,
     * which with production timers (60 s) would outlast the server's half-open TTL (10 s) */
    rel_msg_t m = {ZT_KEEPALIVE, 0, kind != KA_PATH_PROBE, d->reg_sent ? d->k_s : d->k_boot, d->reg_sent ? ZK_SESSION : ZK_BOOT};
    int seq = rel_send(d->rel, &m, pl, (uint16_t)w.len, d->now);
    if (seq > 0) {
        d->ka_seq = (uint32_t)seq;
        d->ka_sent = d->now;
        d->ka_is_probe = kind == KA_PATH_PROBE;
        if (kind == KA_HEALTH_PROBE) {
            d->health_seq = (uint32_t)seq;
            d->health_sent = d->now;
        }
        LOGI(T_NAT, LM_NAT_KEEPALIVE_SENT);
    }
}

static void send_register(device_t *d);

/* Clamp an advertised value into the device's bounds (§25.3), logging when it changes. */
static int clamp_keepalive(device_t *d, int adv)
{
    const ztp_config_t *c = &d->cfg.c;
    const char *src = cfg_source(&d->cfg, "nat.keepalive_interval_s");
    if (!c->nat.accept_server_keepalive || (src && !strcmp(src, "cli"))) return c->nat.keepalive_interval_s;
    int v = adv < c->nat.keepalive_min_s ? c->nat.keepalive_min_s : adv > c->nat.keepalive_max_s ? c->nat.keepalive_max_s : adv;
    if (v != adv) LOGI(T_NAT, LM_NAT_CLAMPED, (unsigned)adv, (unsigned)v);
    return v;
}

static void adopt_timers(device_t *d, const tlv_set_t *t)
{
    uint16_t ka = 0, grace = 0;
    uint8_t dead = 0;
    d->keepalive_s = tlv_get_u16(t, TAG_KEEPALIVE_S, &ka) ? clamp_keepalive(d, ka) : d->cfg.c.nat.keepalive_interval_s;
    if (tlv_get_u8(t, TAG_DEAD_INTERVALS, &dead) && d->cfg.c.nat.accept_server_keepalive) {
        int v = dead < 2 ? 2 : dead > 10 ? 10 : dead;
        if (v != dead) LOGI(T_NAT, "Advertised dead_intervals %u clamped to %d", (unsigned)dead, v);
        d->dead_intervals = v;
    } else {
        d->dead_intervals = d->cfg.c.nat.dead_intervals;
    }
    if (tlv_get_u16(t, TAG_GRACE_S, &grace)) d->grace_s = grace;
    LOGD(T_NAT, "keepalive %d s, dead after %d intervals, grace %d s", d->keepalive_s, d->dead_intervals, d->grace_s);
}

/* Continue from the persisted stage, after REGISTER_ACK or a resume (§15.1 RESUME arrows). */
static void continue_from_stage(device_t *d)
{
    if (d->st.stage == ST_FW_INSTALLED) {
        dev_upgrade_installed(d);   /* UPGRADE_STATUS INSTALLED, then VALIDATE */
        return;
    }
    if (d->st.stage == ST_OPERATIONAL) {
        d->fsm = DS_OPERATIONAL;
        if (d->cfg.c.run.exit_when_operational && !d->hold_until)
            d->hold_until = d->now + (uint64_t)d->cfg.c.run.operational_hold_s * 1000u;
        return;
    }
    dev_provision_next(d);
}

static void on_hello_ack(device_t *d, const uint8_t *p, uint16_t n)
{
    tlv_set_t t;
    if (tlv_parse(p, n, &t) != ZTP_OK || !tlv_get_u64(&t, TAG_NONCE_SRV, &d->ns_nonce)) {
        d->c.rx_malformed++;
        return;
    }
    adopt_timers(d, &t);
    d->backoff_n = 0;
    if (d->resuming) {   /* E16: HELLO_ACK RESUMED */
        char fw[9] = "";
        uint16_t cfg = 0;
        if (tlv_get_str(&t, TAG_FW_TARGET, fw, sizeof fw) && *fw) snprintf(d->st.fw_target, sizeof d->st.fw_target, "%s", fw);
        if (tlv_get_u16(&t, TAG_CFG_VER, &cfg) && cfg) d->st.cfg_version = cfg;
        ztp_derive_ks(d->k_dev, d->nd, d->ns_nonce, d->sid, d->epoch, d->k_s);
        d->reg_sent = true;   /* every frame is K_s from here on */
        rel_set_ack_key(d->rel, d->k_s, ZK_SESSION);
        d->st.epoch = d->epoch;   /* persist epoch = proposed_epoch */
        if (dev_state_save(d->state_path, &d->st) != 0) LOGW(T_ERROR, "cannot persist state");
        d->resuming = false;
        d->rejected_once = false;
        d->resume_cycles = 0;
        LOGI(T_UDP, LM_UDP_SESSION_RESUMED " epoch=%u", (unsigned)d->sid, (unsigned)d->epoch);
        continue_from_stage(d);
        return;
    }
    ztp_derive_kdev(d->k_mfg, d->st.serial, d->k_dev);
    ztp_derive_ks(d->k_dev, d->nd, d->ns_nonce, d->sid, 0, d->k_s);
    LOGI(T_UDP, LM_UDP_SESSION_EST, (unsigned)d->sid);
    d->fsm = DS_PROBING;
    if (d->cfg.c.nat.path_probe) dev_send_keepalive(d, KA_PATH_PROBE);
    else send_register(d);
}

/* §10.2: REGISTER under K_s, after the path probe. */
static void send_register(device_t *d)
{
    const ztp_config_t *c = &d->cfg.c;
    char fw[9];
    dev_running_fw(d, fw);
    d->nonce_reg = rand_u64();
    uint8_t pl[128];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_str(&w, TAG_SERIAL, d->st.serial);
    tlv_put_str(&w, TAG_MODEL, c->identity.model);
    tlv_put_str(&w, TAG_FW_VER, fw);
    tlv_put_str(&w, TAG_HW_REV, c->identity.hw_rev);
    tlv_put_u64(&w, TAG_NONCE_REG, d->nonce_reg);
    tlv_put_u64(&w, TAG_NONCE_SRV, d->ns_nonce);
    d->reg_sent = true;
    rel_set_ack_key(d->rel, d->k_s, ZK_SESSION);   /* every frame is K_s from here on */
    rel_msg_t m = {ZT_REGISTER, 0, false, d->k_s, ZK_SESSION};
    int seq = rel_send(d->rel, &m, pl, (uint16_t)w.len, d->now);
    d->fsm = DS_REGISTER;
    LOGI(T_AUTH, LM_AUTH_REGISTER, (unsigned)d->sid, d->st.serial, c->identity.model, fw, (unsigned long long)d->nonce_reg);
    LOGD(T_AUTH, "REGISTER seq=%d TLVs: SERIAL MODEL FW_VER HW_REV(%s) NONCE_REG NONCE_SRV(%llu)", seq, c->identity.hw_rev,
         (unsigned long long)d->ns_nonce);
}

/* E7: REGISTER_ACK (K_s, delivered in order). */
static void on_register_ack(device_t *d, const uint8_t *p, uint16_t n)
{
    tlv_set_t t;
    char tenant[33] = "", fw[9] = "";
    uint16_t cfg = 0, info = 0;
    if (tlv_parse(p, n, &t) != ZTP_OK || !tlv_get_str(&t, TAG_TENANT, tenant, sizeof tenant) ||
        !tlv_get_str(&t, TAG_FW_TARGET, fw, sizeof fw) || !tlv_get_u16(&t, TAG_CFG_VER, &cfg)) {
        d->c.rx_malformed++;
        return;
    }
    tlv_get_u16(&t, TAG_INFO, &info);
    LOGI(T_AUTH, LM_AUTH_REG_OK);
    LOGI(T_AUTH, LM_AUTH_TENANT, tenant);
    if (info == RC_ALREADY_REGISTERED) LOGI(T_AUTH, "Server reports this device was already registered");
    d->st.stage = d->st.stage > ST_REGISTERED && d->st.stage != ST_FAILED ? d->st.stage : ST_REGISTERED;
    d->st.session_id = d->sid;
    d->st.epoch = 0;
    d->st.proposed_epoch = 0;
    d->st.reg_attempts = 0;
    snprintf(d->st.tenant, sizeof d->st.tenant, "%s", tenant);
    snprintf(d->st.fw_target, sizeof d->st.fw_target, "%s", fw);
    d->st.cfg_version = cfg;
    snprintf(d->st.server, sizeof d->st.server, "%s", d->server_str);
    if (dev_state_save(d->state_path, &d->st) != 0) LOGW(T_ERROR, "cannot persist state");
    d->fsm = DS_REGISTERED;
    d->force_fresh = false;
    d->resume_cycles = 0;
    LOGD(T_STATE, "Registered (fw target %s, config v%u)", fw, (unsigned)cfg);
    continue_from_stage(d);   /* E7 */
}

static void sess_deliver(void *ctx, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t *p, uint16_t n)
{
    device_t *d = ctx;
    if (type == ZT_HELLO_ACK) on_hello_ack(d, p, n);
    else if (type == ZT_REGISTER_ACK) on_register_ack(d, p, n);
    else if (type == ZT_MANIFEST) dev_xfer_on_manifest(d, p, n);
    else if (type == ZT_CHUNK_DATA) dev_xfer_on_chunk(d, p, n);
    else if (type == ZT_STATUS_ACK) {
        dev_xfer_on_status_ack(d);
        dev_validate_on_status_ack(d);
    } else if (type == ZT_ERROR) {
        tlv_set_t t;
        uint16_t reason = 0;
        char detail[65] = "";
        if (tlv_parse(p, n, &t) == ZTP_OK) {
            tlv_get_u16(&t, TAG_REASON, &reason);
            tlv_get_str(&t, TAG_DETAIL, detail, sizeof detail);
        }
        LOGI(T_UDP, "Server error %s (%s)", ztp_reason_name(reason), detail);
    } else {
        LOGD(T_UDP, "%s seq=%u: no handler", ztp_type_name(type), (unsigned)seq);
    }
}

/* E8: REGISTER_NAK, terminal, K_boot; accepted only if it echoes the outstanding NONCE_REG (§10.4 #7). */
static void on_register_nak(device_t *d, const uint8_t *buf, size_t len, const ztp_hdr_t *h)
{
    if (!ztp_mac_verify(buf, len, d->k_boot)) {
        d->c.mac_fail++;
        LOGD(T_AUTH, "REGISTER_NAK with an invalid MAC ignored");
        return;
    }
    tlv_set_t t;
    uint16_t reason = 0;
    uint64_t echo = 0;
    if (tlv_parse(buf + ZTP_HDR_LEN, h->payload_len, &t) != ZTP_OK || !tlv_get_u16(&t, TAG_REASON, &reason) ||
        !tlv_get_u64(&t, TAG_NONCE_REG, &echo)) {
        d->c.rx_malformed++;
        return;
    }
    if (!d->reg_sent || echo != d->nonce_reg) {
        d->c.rx_unknown++;
        LOGD(T_AUTH, "REGISTER_NAK for another request (nonce %llu) ignored", (unsigned long long)echo);
        return;
    }
    int n = d->cfg.c.auth.register_attempts;
    if (reason == RC_NO_POLICY) {
        LOGI(T_AUTH, LM_AUTH_FAIL_NO_POLICY);
        dev_fail(d, "NO_POLICY");
        return;
    }
    d->st.reg_attempts++;
    dev_state_save(d->state_path, &d->st);
    if (reason == RC_BAD_MAC) {
        bool last = d->st.reg_attempts >= n;
        LOGI(T_AUTH, LM_AUTH_FAIL_HMAC "%s", (unsigned)d->st.reg_attempts, (unsigned)n, last ? LM_AUTH_FAIL_GIVEUP : "");
        if (last) dev_fail(d, "Invalid HMAC");
        else dev_backoff(d, "registration rejected");
    } else if (reason == RC_REPLAY) {
        LOGI(T_AUTH, "Registration rejected: REPLAY, retrying with a new nonce");
        if (d->st.reg_attempts >= n) dev_fail(d, "REPLAY");
        else dev_backoff(d, "nonce rejected");
    } else {
        LOGI(T_AUTH, "Registration rejected: %s", ztp_reason_name(reason));
        dev_backoff(d, "registration rejected");
    }
}

static void session_reset(device_t *d)
{
    dev_session_end(d);
    if (d->st.max_dgram && d->st.max_dgram < d->relcfg.max_dgram) d->relcfg.max_dgram = d->st.max_dgram;   /* §5.8 */
    d->rel = rel_new(&d->relcfg, sess_tx, sess_deliver, d);
    d->nd = rand_u64();
    d->ka_seq = 0;
    d->health_seq = 0;
    d->reg_sent = false;
    d->nonce_reg = 0;
    d->last_auth_rx = 0;
    d->report_pending = false;
}

void dev_session_start(device_t *d)
{
    session_reset(d);
    if (!d->rel) { dev_fail(d, "out of memory"); return; }
    rel_set_ack_key(d->rel, d->k_boot, ZK_BOOT);
    rel_set_session(d->rel, 0, 0);
    d->sid = 0;
    d->epoch = 0;
    d->resuming = false;
    d->fsm = DS_SESSION;
    char fw[9];
    dev_running_fw(d, fw);
    uint8_t pl[96];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_str(&w, TAG_SERIAL, d->st.serial);
    tlv_put_str(&w, TAG_MODEL, d->cfg.c.identity.model);
    tlv_put_str(&w, TAG_FW_VER, fw);
    tlv_put_u64(&w, TAG_NONCE_DEV, d->nd);
    tlv_put_u32(&w, TAG_CAPS, CAP_MCAST_DISCOVERY | CAP_RESUME | CAP_MTU_FALLBACK);
    rel_msg_t m = {ZT_HELLO, 0, false, d->k_boot, ZK_BOOT};
    int seq = rel_send(d->rel, &m, pl, (uint16_t)w.len, d->now);
    if (seq > 0) LOGI(T_UDP, LM_UDP_HELLO_SENT, (unsigned)seq);
}

bool dev_can_resume(const device_t *d)
{
    return !d->force_fresh && d->st.session_id && d->st.stage >= ST_REGISTERED && d->st.stage != ST_FAILED &&
           d->st.server[0] && d->resume_cycles < d->cfg.c.net.resume_cycles;
}

/* §12.3 / E2: persist resume_ctr + 1 and proposed_epoch = max(epoch, proposed_epoch) + 1 *before*
 * sending, so the device never proposes an epoch twice, even across a crash. */
void dev_resume_start(device_t *d)
{
    if (!hostport_parse(d->st.server, &d->server)) {
        d->force_fresh = true;
        dev_discovery_start(d);
        return;
    }
    addr_fmt(&d->server, d->server_str);
    d->st.resume_ctr++;
    d->st.proposed_epoch = (uint16_t)((d->st.epoch > d->st.proposed_epoch ? d->st.epoch : d->st.proposed_epoch) + 1);
    if (dev_state_save(d->state_path, &d->st) != 0) {
        dev_fail(d, "storage error");
        return;
    }
    session_reset(d);
    if (!d->rel) { dev_fail(d, "out of memory"); return; }
    d->sid = d->st.session_id;
    d->epoch = d->st.proposed_epoch;
    d->resuming = true;
    d->resume_cycles++;
    ztp_derive_kdev(d->k_mfg, d->st.serial, d->k_dev);
    rel_set_session(d->rel, d->sid, d->epoch);
    rel_set_ack_key(d->rel, d->k_dev, ZK_DEV);
    d->fsm = DS_RESUMING;
    char fw[9];
    dev_running_fw(d, fw);
    uint8_t pl[128];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_str(&w, TAG_SERIAL, d->st.serial);
    tlv_put_str(&w, TAG_MODEL, d->cfg.c.identity.model);
    tlv_put_u32(&w, TAG_RESUME_CTR, d->st.resume_ctr);
    tlv_put_u64(&w, TAG_NONCE_DEV, d->nd);
    tlv_put_str(&w, TAG_FW_VER, fw);
    tlv_put_u8(&w, TAG_STAGE, (uint8_t)d->st.stage);
    rel_msg_t m = {ZT_HELLO, ZF_RESUME, false, d->k_dev, ZK_DEV};
    rel_send(d->rel, &m, pl, (uint16_t)w.len, d->now);
    netsim_flush(&d->ns, d->now);
    dev_crash_point(d, "resume-sent");   /* the server may or may not accept it (TC-UPG-11 (a)) */
    LOGI(T_UDP, "Resuming session %u (ctr=%u, proposed epoch %u, stage %s)", (unsigned)d->sid, (unsigned)d->st.resume_ctr,
         (unsigned)d->epoch, dev_stage_name(d->st.stage));
}

void dev_session_end(device_t *d)
{
    dev_xfer_suspend(d);
    rel_free(d->rel);
    d->rel = NULL;
    d->sid = 0;
    d->ka_seq = 0;
    secure_zero(d->k_s, sizeof d->k_s);
}

/* Fall back to a fresh HELLO + REGISTER with the known server (§12.3 fallbacks). */
static void go_fresh(device_t *d, const char *why)
{
    LOGI(T_UDP, "%s: registering again", why);
    d->force_fresh = true;
    d->resuming = false;
    dev_session_start(d);
}

static void on_error(device_t *d, const uint8_t *buf, size_t len, const ztp_hdr_t *h)
{
    if (!ztp_mac_verify(buf, len, d->k_boot)) { d->c.mac_fail++; return; }
    tlv_set_t t;
    uint16_t reason = 0;
    uint64_t echo = 0;
    char detail[65] = "";
    if (tlv_parse(buf + ZTP_HDR_LEN, h->payload_len, &t) != ZTP_OK || !tlv_get_u16(&t, TAG_REASON, &reason)) {
        d->c.rx_malformed++;
        return;
    }
    if (!tlv_get_u64(&t, TAG_NONCE_DEV, &echo) || echo != d->nd) {   /* must answer our pending HELLO */
        d->c.rx_unknown++;
        return;
    }
    tlv_get_str(&t, TAG_DETAIL, detail, sizeof detail);
    if (reason == RC_UNKNOWN_MODEL) {
        LOGI(T_AUTH, LM_AUTH_FAIL_UNKNOWN_MODEL);
        dev_fail(d, "UNKNOWN_MODEL");
    } else if (reason == RC_SERVER_BUSY) {
        dev_backoff(d, "server busy");
    } else if (d->resuming && reason == RC_RESUME_REJECTED) {
        if (!d->rejected_once) {   /* E17: once: a new ctr and epoch */
            d->rejected_once = true;
            LOGI(T_UDP, "Resume rejected: retrying with a new counter and epoch");
            dev_resume_start(d);
        } else {
            go_fresh(d, "Resume rejected twice");   /* E17a */
        }
    } else if (d->resuming && (reason == RC_SESSION_UNKNOWN || reason == RC_SESSION_EXPIRED)) {
        go_fresh(d, reason == RC_SESSION_EXPIRED ? "Session expired on the server" : "Session unknown to the server");
    } else {
        LOGI(T_UDP, "Server error %s (%s)", ztp_reason_name(reason), detail);
    }
}

/* E25 (§9.7 strict IP-change mode): accepted only if ECHO_SEQ names a seq we sent in this epoch that is not
 * older than snd_una - 32, at most once per echoed seq, and never while a resume is in progress. */
static void on_resume_required(device_t *d, const uint8_t *p, uint16_t n)
{
    tlv_set_t t;
    uint16_t reason = 0;
    uint32_t echo = 0;
    if (tlv_parse(p, n, &t) != ZTP_OK || !tlv_get_u16(&t, TAG_REASON, &reason) || reason != RC_RESUME_REQUIRED ||
        !tlv_get_u32(&t, TAG_ECHO_SEQ, &echo)) {
        d->c.rx_unknown++;
        return;
    }
    uint32_t una = rel_acked_through(d->rel) + 1;
    if (d->resuming || echo == 0 || echo >= rel_next_seq(d->rel) || echo + 32 < una || echo == d->rr_echo) {
        d->c.rx_unknown++;
        return;
    }
    d->rr_echo = echo;
    LOGI(T_UDP, LM_UDP_RESUME_REQUIRED);
    dev_resume_start(d);
}

void dev_session_rx(device_t *d, const uint8_t *buf, size_t len, const ztp_hdr_t *h, const struct sockaddr_in *src)
{
    if (!d->rel || d->fsm < DS_SESSION || d->fsm > DS_OPERATIONAL) {
        d->c.rx_unknown++;
        return;
    }
    if (h->type == ZT_ERROR && !(d->reg_sent && h->session_id == d->sid && h->key_id == ZK_SESSION)) {
        on_error(d, buf, len, h);   /* terminal ERRORs (K_boot); BAD_REQUEST is reliable under K_s */
        return;
    }
    if (h->type == ZT_REGISTER_NAK) { on_register_nak(d, buf, len, h); return; }
    if (h->type == ZT_HELLO_ACK && (d->sid == 0 || d->resuming)) {
        /* bind only to a HELLO_ACK that echoes our nonce: under K_boot (fresh, §7.1) or K_dev (resume, §12.3) */
        tlv_set_t t;
        uint64_t echo = 0;
        if (!ztp_mac_verify(buf, len, d->resuming ? d->k_dev : d->k_boot)) { d->c.mac_fail++; return; }
        if (h->session_id == 0 || (d->resuming && (h->session_id != d->sid || h->epoch != d->epoch)) ||
            tlv_parse(buf + ZTP_HDR_LEN, h->payload_len, &t) != ZTP_OK || !tlv_get_u64(&t, TAG_NONCE_DEV, &echo) ||
            echo != d->nd) {
            d->c.rx_unknown++;
            return;
        }
        if (!d->resuming) {
            d->sid = h->session_id;
            rel_set_session(d->rel, d->sid, 0);
        }
    }
    if (d->sid == 0 || h->session_id != d->sid) { d->c.rx_unknown++; return; }
    if (h->epoch != d->epoch) { d->c.rx_stale++; return; }   /* dropped before the MAC check */
    /* key by type and state (§5.3): HELLO_ACK K_boot (fresh) or K_dev (resume); after REGISTER or a
     * resume every reliable frame is K_s; ack-class frames may still arrive under K_boot at the switch */
    const ztp_type_info_t *tinfo = ztp_type_info(h->type);
    bool ok;
    if (h->type == ZT_HELLO_ACK) ok = ztp_mac_verify(buf, len, d->resuming ? d->k_dev : d->k_boot);
    else if (!d->reg_sent) ok = ztp_mac_verify(buf, len, d->k_boot);
    else if (tinfo->cls == ZC_ACKCLASS) ok = ztp_mac_verify(buf, len, d->k_s) || ztp_mac_verify(buf, len, d->k_boot);
    else ok = ztp_mac_verify(buf, len, d->k_s);
    if (!ok) {
        d->c.mac_fail++;
        return;
    }
    if (h->type == ZT_ERROR && tinfo->cls != ZC_RELIABLE) {   /* E25: RESUME_REQUIRED (K_s, unsequenced) */
        on_resume_required(d, buf + ZTP_HDR_LEN, h->payload_len);
        return;
    }
    d->last_auth_rx = d->now;
    if (tinfo->cls == ZC_RELIABLE) {
        d->in_rx = true;
        rel_on_rx(d->rel, h, buf + ZTP_HDR_LEN, d->now);
        d->in_rx = false;
        if (d->fail_pending) {
            d->fail_pending = false;
            dev_fail(d, d->pending_reason);
            return;
        }
    } else {
        rel_on_ack(d->rel, h, d->now);
    }
    if (!d->rel) return;   /* a handler ended the session */

    if (d->ka_seq && rel_acked_through(d->rel) >= d->ka_seq) {
        LOGI(T_NAT, LM_NAT_KEEPALIVE_ACKED " (rtt=%u ms)", (unsigned)(d->now - d->ka_sent));
        d->ka_seq = 0;
        if (d->ka_is_probe && d->fsm == DS_PROBING) send_register(d);   /* E6 */
    }
    dev_upgrade_timer(d);
    dev_validate_timer(d);
}

static uint64_t dead_deadline(const device_t *d)
{
    if (!d->sid || !d->last_auth_rx || d->resuming) return UINT64_MAX;
    return d->last_auth_rx + (uint64_t)d->dead_intervals * (uint64_t)d->keepalive_s * 1000u;
}

static uint64_t keepalive_due(const device_t *d)
{
    if (!d->sid || d->ka_seq || d->fsm < DS_REGISTER || d->fsm > DS_OPERATIONAL) return UINT64_MAX;
    return d->last_tx + (uint64_t)d->keepalive_s * 1000u;
}

uint64_t dev_session_deadline(const device_t *d)
{
    if (!d->rel) return UINT64_MAX;
    uint64_t t = rel_next_deadline(d->rel), x;
    if ((x = dead_deadline(d)) < t) t = x;
    if ((x = keepalive_due(d)) < t) t = x;
    if (d->hold_until && d->hold_until < t) t = d->hold_until;
    if ((x = dev_xfer_deadline(d)) < t) t = x;
    if ((x = dev_validate_deadline(d)) < t) t = x;
    return t;
}

/* Lost the session: a registered device resumes (§12.3), otherwise a fresh HELLO; with backoff (§9.3). */
static void session_lost(device_t *d, const char *why)
{
    dev_backoff(d, why);
}

void dev_session_timer(device_t *d)
{
    if (!d->rel) return;
    if (d->hold_until && d->now >= d->hold_until) {
        LOGI(T_STATE, LM_STATE_EXITING, (unsigned)d->cfg.c.run.operational_hold_s);
        d->fsm = DS_EXIT;
        d->exit_code = 0;
        return;
    }
    dev_xfer_timer(d);
    if (!d->rel || d->fsm == DS_FAILED) return;
    dev_upgrade_timer(d);
    if (!d->rel || d->fsm == DS_FAILED || d->fsm == DS_EXIT) return;
    dev_validate_timer(d);
    if (!d->rel || d->fsm == DS_FAILED) return;
    if (rel_on_timer(d->rel, d->now) == REL_E_FAILED) {
        if (d->resuming) {          /* E17b: no reply to the resume HELLO */
            LOGI(T_UDP, "No reply to the resume HELLO (cycle %d/%d)", d->resume_cycles, d->cfg.c.net.resume_cycles);
            session_lost(d, "resume unanswered");
        } else if (d->sid == 0) {   /* E6a: HELLO budget exhausted */
            if (d->cfg.c.nat.blocked_hint) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &d->server.sin_addr, ip, sizeof ip);
                unsigned port = ntohs(d->server.sin_port);
                LOGW(T_NAT, LM_NAT_BLOCKED_HINT, ip, port, 1u + (unsigned)d->cfg.c.rel.max_retries, port);
            }
            session_lost(d, "no HELLO_ACK");
        } else {
            d->c.timeouts++;
            LOGI(T_NAT, LM_NAT_TIMEOUT, (unsigned)((d->now - d->last_auth_rx) / 1000u));
            session_lost(d, "retries exhausted");
        }
        return;
    }
    if (d->now >= dead_deadline(d)) {   /* E22 */
        d->c.timeouts++;
        LOGI(T_NAT, LM_NAT_TIMEOUT, (unsigned)(d->dead_intervals * d->keepalive_s));
        session_lost(d, "session timeout");
        return;
    }
    if (d->now >= keepalive_due(d)) dev_send_keepalive(d, KA_PERIODIC);   /* E21 */
}
