/* Device provisioning (DESIGN §11): the per-file transfer state machine (§11.5), config first and
 * then firmware into the inactive slot; resume from the persisted bitmap (§11.4); whole-file SHA-256
 * (§11.3); the device half of the small-MTU fallback (§5.8). Upgrade and validation are M5. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dev.h"
#include "log.h"
#include "logmsg.h"
#include "util.h"
#include "validate.h"

static const char *kind_name(uint8_t k) { return k == FK_CONFIG ? "config" : "firmware"; }

static int send_rel(device_t *d, uint8_t type, const uint8_t *pl, uint16_t n)
{
    rel_msg_t m = {type, 0, false, d->k_s, ZK_SESSION};
    return rel_send(d->rel, &m, pl, n, d->now);
}

static void persist_map(device_t *d)
{
    dev_xfer_t *x = &d->x;
    if (x->fd < 0 || !x->map.bits) return;
    /* §11.4 durability order: chunks durable first, then the map (atomic write, fsync, rename, dir fsync) */
    if (util_full_fsync(x->fd) != 0) LOGW(T_PROVISION, "fsync %s: %s", x->part, strerror(errno));
    if (dev_xfermap_save(&x->map, x->mappath) != 0) LOGW(T_PROVISION, "cannot save %s: %s", x->mappath, strerror(errno));
    x->since_persist = 0;
}

static void discard_partial(device_t *d)
{
    dev_xfer_t *x = &d->x;
    if (x->fd >= 0) close(x->fd);
    x->fd = -1;
    dev_xfermap_free(&x->map);
    unlink(x->part);
    unlink(x->mappath);
    char bak[CFG_PATH + 40];
    snprintf(bak, sizeof bak, "%s.bak", x->mappath);
    unlink(bak);
}

static void request_manifest(device_t *d, uint8_t kind)
{
    dev_xfer_t *x = &d->x;
    x->active = true;
    x->kind = kind;
    x->req_out = x->req_acked = x->probed = x->bh_rerequested = false;
    snprintf(x->part, sizeof x->part, "%s/xfer_%u.part", d->cfg.c.paths.state_dir, (unsigned)kind);
    snprintf(x->mappath, sizeof x->mappath, "%s/xfer_%u.map", d->cfg.c.paths.state_dir, (unsigned)kind);
    uint8_t pl[16];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u8(&w, TAG_FILE_KIND, kind);
    tlv_put_u16(&w, TAG_MAX_DGRAM, d->st.max_dgram);
    send_rel(d, ZT_MANIFEST_REQ, pl, (uint16_t)w.len);
    d->fsm = kind == FK_CONFIG ? DS_CONFIG : DS_FIRMWARE;
    LOGD(T_PROVISION, "MANIFEST_REQ %s (MAX_DGRAM %u)", kind_name(kind), (unsigned)d->st.max_dgram);
}

/* Continue from the persisted stage (§15.1): config, then the firmware decision (§12.1), then the
 * upgrade (FW_STAGED) or validation (FW_INSTALLED, or firmware already current). */
void dev_provision_next(device_t *d)
{
    if (d->st.stage < ST_CONFIG_OK) {
        request_manifest(d, FK_CONFIG);
        return;
    }
    if (d->st.stage == ST_CONFIG_OK) {
        char running[9];
        int cmp = 0;
        dev_running_fw(d, running);
        if (version_cmp(running, d->st.fw_target, &cmp) == 0 && cmp >= 0) {
            LOGI(T_UPGRADE, LM_UPG_CURRENT, running);
            snprintf(d->fw_approved, sizeof d->fw_approved, "%s", running);
            dev_validate_begin(d);
            return;
        }
        LOGI(T_UPGRADE, LM_UPG_CURRENT_TARGET, running, d->st.fw_target);
        request_manifest(d, FK_FIRMWARE);
        return;
    }
    if (d->st.stage == ST_FW_STAGED) {
        dev_upgrade_begin(d);
        return;
    }
    if (d->st.stage == ST_FW_INSTALLED) {
        dev_upgrade_installed(d);
        return;
    }
    dev_validate_begin(d);
}

static void request_next(device_t *d)
{
    dev_xfer_t *x = &d->x;
    uint32_t s, n;
    if (!dev_xfermap_next_missing(&x->map, 0, (uint32_t)d->cfg.c.xfer.chunk_req_max, &s, &n)) return;
    uint8_t pl[24];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u8(&w, TAG_FILE_ID, x->file_id);
    tlv_put_u32(&w, TAG_CHUNK_START, s);
    tlv_put_u16(&w, TAG_CHUNK_NUM, (uint16_t)n);
    int seq = send_rel(d, ZT_CHUNK_REQ, pl, (uint16_t)w.len);
    x->req_out = seq > 0;
    x->req_acked = false;
    x->probed = false;
    x->req_start = s;
    x->req_num = n;
    x->req_seq = seq > 0 ? (uint32_t)seq : 0;
}

static void step_down(device_t *d, const char *why)
{
    if (d->st.max_dgram <= ZTP_MIN_DGRAM) return;
    if (!d->cfg.c.net.mtu_fallback) {
        LOGW(T_UDP, "%s, but net.mtu_fallback is off: the transfer cannot continue", why);
        return;
    }
    d->st.max_dgram = ZTP_MIN_DGRAM;
    dev_state_save(d->state_path, &d->st);   /* survives a restart and a resume */
    rel_set_max_dgram(d->rel, ZTP_MIN_DGRAM);
    persist_map(d);
    request_manifest(d, d->x.kind);           /* re-manifest at 256 B chunks; the partial is kept */
}

void dev_xfer_on_manifest(device_t *d, const uint8_t *p, uint16_t n)
{
    dev_xfer_t *x = &d->x;
    tlv_set_t t;
    uint8_t id = 0, kind = 0;
    uint16_t cs = 0;
    uint32_t size = 0, count = 0;
    const uint8_t *sha = NULL;
    uint16_t shalen = 0;
    char ver[9] = "";
    if (!x->active || tlv_parse(p, n, &t) != ZTP_OK || !tlv_get_u8(&t, TAG_FILE_ID, &id) ||
        !tlv_get_u8(&t, TAG_FILE_KIND, &kind) || kind != x->kind || !tlv_get_u32(&t, TAG_FILE_SIZE, &size) ||
        !tlv_get_u16(&t, TAG_CHUNK_SIZE, &cs) || !tlv_get_u32(&t, TAG_CHUNK_COUNT, &count) ||
        !tlv_get_bytes(&t, TAG_SHA256, &sha, &shalen) || size == 0 || cs == 0 || count != (size + cs - 1) / cs) {
        d->c.rx_malformed++;
        return;
    }
    tlv_get_str(&t, TAG_VERSION, ver, sizeof ver);
    if (x->fd >= 0) close(x->fd);
    x->fd = -1;
    dev_xfermap_free(&x->map);
    x->file_id = id;
    x->size = size;
    x->chunk_size = cs;
    x->count = count;
    memcpy(x->sha256, sha, 32);
    snprintf(x->version, sizeof x->version, "%s", ver);
    char hex[65];
    hex_encode(x->sha256, 32, hex);
    if (kind == FK_CONFIG) LOGI(T_PROVISION, LM_PROV_CFG_MANIFEST, ver, (unsigned)size, (unsigned)count, hex);
    else LOGI(T_PROVISION, LM_PROV_FW_MANIFEST, ver, (unsigned)size, (unsigned)count);

    /* CHECK_PARTIAL (§11.5) */
    int rc = dev_xfermap_load(x->mappath, x->sha256, size, &x->map);
    if (rc == XFERMAP_MISMATCH) {
        LOGI(T_PROVISION, "The %s on the server changed since the partial download: discarding it", kind_name(kind));
        discard_partial(d);
    } else if (rc == XFERMAP_OK && x->map.chunk_size != cs) {
        if (cs < x->map.chunk_size && dev_xfermap_convert(&x->map, cs) == XFERMAP_OK) {
            LOGI(T_PROVISION, "Chunk size now %u B: the partial %s is kept (bitmap converted)", (unsigned)cs, kind_name(kind));
        } else {
            discard_partial(d);
            rc = XFERMAP_ABSENT;
        }
    }
    if (rc != XFERMAP_OK || !x->map.bits) {
        if (dev_xfermap_init(&x->map, x->sha256, size, cs) != XFERMAP_OK) {
            d->c.rx_malformed++;
            return;
        }
        unlink(x->part);
    }
    x->fd = open(x->part, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (x->fd < 0 || ftruncate(x->fd, (off_t)size) != 0) {
        LOGW(T_PROVISION, "cannot prepare %s: %s", x->part, strerror(errno));
        dev_fail(d, "storage error");
        return;
    }
    uint32_t have = dev_xfermap_present(&x->map);
    if (have && have < count) {
        if (kind == FK_FIRMWARE) LOGI(T_PROVISION, LM_PROV_RESUMING_FW, (unsigned)have, (unsigned)count, (unsigned)(count - have));
        else LOGI(T_PROVISION, "Resuming config: %u/%u chunks present, requesting %u", (unsigned)have, (unsigned)count,
                  (unsigned)(count - have));
    }
    x->since_persist = x->writes = x->dups = 0;
    if (dev_xfermap_complete(&x->map)) {
        x->req_out = false;
        dev_xfer_on_chunk(d, NULL, 0);   /* VERIFYING */
        return;
    }
    request_next(d);
}

static bool file_sha_ok(device_t *d)
{
    dev_xfer_t *x = &d->x;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[16384], out[32];
    for (uint32_t off = 0; off < x->size;) {
        size_t want = x->size - off < sizeof buf ? x->size - off : sizeof buf;
        ssize_t r = pread(x->fd, buf, want, off);
        if (r <= 0) return false;
        sha256_update(&c, buf, (size_t)r);
        off += (uint32_t)r;
    }
    sha256_final(&c, out);
    return ct_memeq(out, x->sha256, 32);
}

static void transfer_done(device_t *d, uint16_t result)
{
    uint8_t pl[16];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u8(&w, TAG_FILE_ID, d->x.file_id);
    tlv_put_u16(&w, TAG_RESULT, result);
    send_rel(d, ZT_TRANSFER_DONE, pl, (uint16_t)w.len);
}

/* Report FAILED and go FAILED once the server has acknowledged it (or after 3 s). */
void dev_report_failed(device_t *d, uint16_t reason, const char *why)
{
    uint8_t pl[48];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u8(&w, TAG_REPORT_STATE, RPT_FAILED);
    tlv_put_u16(&w, TAG_REASON, reason);
    char fw[9];
    dev_running_fw(d, fw);
    tlv_put_str(&w, TAG_FW_VER, fw);
    tlv_put_u16(&w, TAG_CFG_VER, (uint16_t)d->st.cfg_version);
    send_rel(d, ZT_STATUS_REPORT, pl, (uint16_t)w.len);
    snprintf(d->fail_reason, sizeof d->fail_reason, "%s", why);
    d->fail_after = d->now + 3000;
    d->x.active = false;
}

void dev_xfer_on_status_ack(device_t *d)
{
    if (d->fail_after) {
        d->fail_after = 0;
        dev_fail(d, d->fail_reason);
    }
}

static void verify(device_t *d)
{
    dev_xfer_t *x = &d->x;
    int n = d->cfg.c.xfer.hash_attempts;
    LOGI(T_PROVISION, "%s: %u chunks written, %u duplicates ignored", x->kind == FK_CONFIG ? "Config" : "Firmware",
         (unsigned)x->writes, (unsigned)x->dups);
    if (x->kind == FK_FIRMWARE) {
        persist_map(d);
        dev_crash_point(d, "downloaded");   /* complete .part, stage still CONFIG_OK (§12.4) */
    }
    if (!file_sha_ok(d)) {
        x->hash_attempt++;
        LOGI(T_PROVISION, LM_PROV_HASH_MISMATCH, (unsigned)x->hash_attempt, (unsigned)n);
        transfer_done(d, RC_HASH_MISMATCH);
        discard_partial(d);
        if (x->hash_attempt >= n) dev_report_failed(d, RC_HASH_MISMATCH, "HASH_MISMATCH");
        else request_manifest(d, x->kind);   /* a full download again */
        return;
    }
    char hex[65];
    hex_encode(x->sha256, 32, hex);
    if (x->kind == FK_CONFIG) {
        char *text = NULL;
        size_t len = 0, cap = x->size + 1;
        text = malloc(cap);
        ssize_t r = text ? pread(x->fd, text, x->size, 0) : -1;
        char err[160] = "";
        int vrc = r == (ssize_t)x->size ? dev_config_check(text, x->size, d->st.cfg_version, err, sizeof err) : -1;
        (void)len;
        free(text);
        if (vrc != DEV_CFG_OK) {
            LOGI(T_PROVISION, "Config rejected: %s", err);
            transfer_done(d, RC_VALIDATION_FAILED);
            dev_report_failed(d, RC_VALIDATION_FAILED, "config invalid");
            return;
        }
        char dst[CFG_PATH + 32];
        snprintf(dst, sizeof dst, "%s/config.json", d->cfg.c.paths.state_dir);
        close(x->fd);
        x->fd = -1;
        if (rename(x->part, dst) != 0) { dev_fail(d, "storage error"); return; }
        util_fsync_dir(dst);
        LOGI(T_PROVISION, LM_PROV_CONFIG_VERIFIED);
        snprintf(d->st.cfg_sha256, sizeof d->st.cfg_sha256, "%s", hex);
        d->st.stage = ST_CONFIG_OK;
    } else {
        LOGI(T_PROVISION, LM_PROV_FW_VERIFIED);
        /* §12.1 step 3: the header must name this model and the target version */
        uint8_t h[64];
        bool hdr_ok = pread(x->fd, h, sizeof h, 0) == (ssize_t)sizeof h && !memcmp(h, "ZTPFW\0", 6) &&
                      !strncmp((const char *)h + 7, d->cfg.c.identity.model, 16) &&
                      !strncmp((const char *)h + 23, d->st.fw_target, 8);
        if (!hdr_ok) {
            x->hash_attempt++;
            LOGI(T_PROVISION, "Firmware header does not name %s %s, discarding (attempt %u/%u)", d->cfg.c.identity.model,
                 d->st.fw_target, (unsigned)x->hash_attempt, (unsigned)n);
            transfer_done(d, RC_HASH_MISMATCH);
            discard_partial(d);
            if (x->hash_attempt >= n) dev_report_failed(d, RC_HASH_MISMATCH, "bad firmware header");   /* §12.6 */
            else request_manifest(d, x->kind);
            return;
        }
        char dst[CFG_PATH + 32], slot = d->st.active_slot == 'B' ? 'A' : 'B';
        snprintf(dst, sizeof dst, "%s/fw_slot_%c.bin", d->cfg.c.paths.state_dir, slot);
        close(x->fd);
        x->fd = -1;
        if (rename(x->part, dst) != 0) { dev_fail(d, "storage error"); return; }
        util_fsync_dir(dst);
        LOGI(T_UPGRADE, LM_UPG_STAGED, x->version, slot);
        snprintf(d->st.fw_sha256, sizeof d->st.fw_sha256, "%s", hex);
        d->st.stage = ST_FW_STAGED;
    }
    dev_xfermap_free(&x->map);
    unlink(x->mappath);
    char bak[CFG_PATH + 40];
    snprintf(bak, sizeof bak, "%s.bak", x->mappath);
    unlink(bak);
    if (dev_state_save(d->state_path, &d->st) != 0) LOGW(T_ERROR, "cannot persist state");
    if (x->kind == FK_FIRMWARE) dev_crash_point(d, "verified");   /* slot written, FW_STAGED, still slot A */
    transfer_done(d, RC_OK);
    x->active = false;
    x->hash_attempt = 0;
    dev_provision_next(d);
}

void dev_xfer_on_chunk(device_t *d, const uint8_t *p, uint16_t n)
{
    dev_xfer_t *x = &d->x;
    if (p) {
        ztp_chunk_t c;
        if (!x->active || x->fd < 0 || ztp_chunk_decode(p, n, &c) != ZTP_OK || c.file_id != x->file_id) {
            d->c.rx_malformed += !x->active ? 0 : 1;
            return;
        }
        if (c.flags & CHUNK_F_MTU_REDUCED) {   /* a fill frame: the server stepped down (E27) */
            if (d->st.max_dgram > ZTP_MIN_DGRAM) {
                LOGI(T_UDP, "Server reduced the datagram limit to 508 (MTU_REDUCED)");
                step_down(d, "MTU_REDUCED");
            }
            return;
        }
        uint32_t i = c.chunk_index;
        uint32_t want = i + 1 == x->count ? x->size - i * x->chunk_size : x->chunk_size;
        if (i >= x->count || c.data_len != want) {
            d->c.rx_malformed++;
            return;
        }
        x->req_acked_ms = d->now;   /* data is flowing: no black hole */
        x->probed = x->bh_rerequested = false;
        if (dev_xfermap_has(&x->map, i)) {
            x->dups++;              /* idempotent: a chunk already present is ignored, not rewritten */
            return;
        }
        if (pwrite(x->fd, c.data, c.data_len, (off_t)i * x->chunk_size) != (ssize_t)c.data_len) {
            LOGW(T_PROVISION, "write %s: %s", x->part, strerror(errno));
            return;
        }
        dev_xfermap_set(&x->map, i);
        x->writes++;
        if (x->kind == FK_FIRMWARE) {
            d->fw_chunks_this_process++;
            if (d->fw_chunks_this_process == 20) dev_crash_point(d, "download");
            if (d->hooks.enabled && d->hook_crash_after_chunks &&
                d->fw_chunks_this_process == (uint32_t)d->hook_crash_after_chunks && hooks_fire_once(&d->hooks, "crash-after-chunks")) {
                log_close();
                abort();   /* chunks already on disk from an earlier run don't count (§19.2) */
            }
        }
        uint32_t have = dev_xfermap_present(&x->map);
        if (x->kind == FK_CONFIG) LOGI(T_PROVISION, LM_PROV_RECV_CONFIG, (unsigned)have, (unsigned)x->count);
        else if (have % 8 == 0 || have == x->count)
            LOGI(T_PROVISION, LM_PROV_FW_PROGRESS, (unsigned)have, (unsigned)x->count, (unsigned)(have * 100u / x->count));
        if (++x->since_persist >= (uint32_t)d->cfg.c.xfer.persist_every_chunks) persist_map(d);
        if (!dev_xfermap_complete(&x->map)) {
            bool run_done = true;
            for (uint32_t k = x->req_start; k < x->req_start + x->req_num; k++) run_done &= dev_xfermap_has(&x->map, k);
            if (run_done) request_next(d);
            return;
        }
    }
    if (dev_xfermap_complete(&x->map)) verify(d);
}

void dev_xfer_suspend(device_t *d)
{
    dev_xfer_t *x = &d->x;
    if (!x->active) return;
    persist_map(d);
    if (x->fd >= 0) close(x->fd);
    x->fd = -1;
    dev_xfermap_free(&x->map);
    x->active = false;
}

/* §5.8 black-hole detection: the CHUNK_REQ was ACKed but no CHUNK_DATA arrives. After half of
 * net.pmtu_blackhole_s, a small liveness KEEPALIVE; if it is ACKed and still nothing has arrived by
 * the full interval, the device re-requests the missing chunks (the server sends them again at once, as new
 * frames, instead of waiting out its retransmission backoff) and runs a second interval. Only if that one is
 * silent too are the large frames being dropped: step down to 508. One interval alone is not enough at the
 * tail of a transfer, where a single frame is in flight and the backoff fits only about four tries into it
 * (a false step-down at 15% loss in matrix M3, seed 3). */
uint64_t dev_xfer_deadline(const device_t *d)
{
    const dev_xfer_t *x = &d->x;
    uint64_t t = d->fail_after ? d->fail_after : UINT64_MAX;
    if (!x->active || !x->req_out || !x->req_acked || d->st.max_dgram <= ZTP_MIN_DGRAM) return t;
    uint64_t full = (uint64_t)d->cfg.c.net.pmtu_blackhole_s * 1000u, at = x->req_acked_ms + (x->probed ? full : full / 2);
    return at < t ? at : t;
}

void dev_xfer_timer(device_t *d)
{
    dev_xfer_t *x = &d->x;
    if (d->fail_after && d->now >= d->fail_after) {
        d->fail_after = 0;
        dev_fail(d, d->fail_reason);
        return;
    }
    if (!x->active || !x->req_out || d->st.max_dgram <= ZTP_MIN_DGRAM || !d->rel) return;
    if (!x->req_acked) {
        if (rel_acked_through(d->rel) >= x->req_seq) {
            x->req_acked = true;
            x->req_acked_ms = d->now;
        }
        return;
    }
    uint64_t full = (uint64_t)d->cfg.c.net.pmtu_blackhole_s * 1000u, quiet = d->now - x->req_acked_ms;
    if (!x->probed && quiet >= full / 2) {
        dev_send_keepalive(d, KA_PERIODIC);
        x->probed = true;
    } else if (x->probed && quiet >= full && d->ka_seq == 0) {   /* the probe was ACKed: small frames pass */
        if (!x->bh_rerequested) {
            LOGI(T_UDP, LM_UDP_MTU_REREQUEST, (unsigned)(quiet / 1000u));
            request_next(d);          /* resets the probe; the interval restarts once this CHUNK_REQ is ACKed */
            x->bh_rerequested = true;
            return;
        }
        x->bh_rerequested = false;
        LOGI(T_UDP, LM_UDP_MTU_BLACKHOLE);
        step_down(d, "PMTU black hole");
    }
}
