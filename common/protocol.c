/* ZTP wire protocol v1 codec (DESIGN §5). */
#include "protocol.h"

#include <string.h>

static const ztp_type_info_t TYPES[] = {
    {ZT_DISCOVER, "DISCOVER", ZC_DATAGRAM, ZD_D2S},
    {ZT_OFFER, "OFFER", ZC_DATAGRAM, ZD_S2D},
    {ZT_HELLO, "HELLO", ZC_RELIABLE, ZD_D2S},
    {ZT_HELLO_ACK, "HELLO_ACK", ZC_RELIABLE, ZD_S2D},
    {ZT_REGISTER, "REGISTER", ZC_RELIABLE, ZD_D2S},
    {ZT_REGISTER_ACK, "REGISTER_ACK", ZC_RELIABLE, ZD_S2D},
    {ZT_REGISTER_NAK, "REGISTER_NAK", ZC_TERMINAL, ZD_S2D},
    {ZT_KEEPALIVE, "KEEPALIVE", ZC_RELIABLE, ZD_D2S},
    {ZT_KEEPALIVE_ACK, "KEEPALIVE_ACK", ZC_ACKCLASS, ZD_S2D},
    {ZT_MANIFEST_REQ, "MANIFEST_REQ", ZC_RELIABLE, ZD_D2S},
    {ZT_MANIFEST, "MANIFEST", ZC_RELIABLE, ZD_S2D},
    {ZT_CHUNK_REQ, "CHUNK_REQ", ZC_RELIABLE, ZD_D2S},
    {ZT_CHUNK_DATA, "CHUNK_DATA", ZC_RELIABLE, ZD_S2D},
    {ZT_TRANSFER_DONE, "TRANSFER_DONE", ZC_RELIABLE, ZD_D2S},
    {ZT_UPGRADE_STATUS, "UPGRADE_STATUS", ZC_RELIABLE, ZD_D2S},
    {ZT_STATUS_REPORT, "STATUS_REPORT", ZC_RELIABLE, ZD_D2S},
    {ZT_STATUS_ACK, "STATUS_ACK", ZC_RELIABLE, ZD_S2D},
    {ZT_CLOSE, "CLOSE", ZC_RELIABLE, ZD_BOTH},
    {ZT_ERROR, "ERROR", ZC_TERMINAL, ZD_S2D},
    {ZT_ACK, "ACK", ZC_ACKCLASS, ZD_BOTH},
};

const ztp_type_info_t *ztp_type_info(uint8_t type)
{
    for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (TYPES[i].type == type) return &TYPES[i];
    return NULL;
}

const char *ztp_type_name(uint8_t type)
{
    const ztp_type_info_t *t = ztp_type_info(type);
    return t ? t->name : "UNKNOWN";
}

static const ztp_tag_info_t TAGS[TAG_MAX_KNOWN + 1] = {
    [TAG_SERIAL] = {TAG_SERIAL, "SERIAL", TV_STR, 16},
    [TAG_MODEL] = {TAG_MODEL, "MODEL", TV_STR, 16},
    [TAG_FW_VER] = {TAG_FW_VER, "FW_VER", TV_STR, 8},
    [TAG_HW_REV] = {TAG_HW_REV, "HW_REV", TV_STR, 8},
    [TAG_NONCE_DEV] = {TAG_NONCE_DEV, "NONCE_DEV", TV_U64, 8},
    [TAG_NONCE_SRV] = {TAG_NONCE_SRV, "NONCE_SRV", TV_U64, 8},
    [TAG_NONCE_REG] = {TAG_NONCE_REG, "NONCE_REG", TV_U64, 8},
    [TAG_TENANT] = {TAG_TENANT, "TENANT", TV_STR, 32},
    [TAG_FW_TARGET] = {TAG_FW_TARGET, "FW_TARGET", TV_STR, 8},
    [TAG_CFG_VER] = {TAG_CFG_VER, "CFG_VER", TV_U16, 2},
    [TAG_KEEPALIVE_S] = {TAG_KEEPALIVE_S, "KEEPALIVE_S", TV_U16, 2},
    [TAG_GRACE_S] = {TAG_GRACE_S, "GRACE_S", TV_U16, 2},
    [TAG_RESUME_CTR] = {TAG_RESUME_CTR, "RESUME_CTR", TV_U32, 4},
    [TAG_CAPS] = {TAG_CAPS, "CAPS", TV_U32, 4},
    [TAG_DISC_NONCE] = {TAG_DISC_NONCE, "DISC_NONCE", TV_U64, 8},
    [TAG_FILE_ID] = {TAG_FILE_ID, "FILE_ID", TV_U8, 1},
    [TAG_FILE_KIND] = {TAG_FILE_KIND, "FILE_KIND", TV_U8, 1},
    [TAG_FILE_SIZE] = {TAG_FILE_SIZE, "FILE_SIZE", TV_U32, 4},
    [TAG_CHUNK_SIZE] = {TAG_CHUNK_SIZE, "CHUNK_SIZE", TV_U16, 2},
    [TAG_CHUNK_COUNT] = {TAG_CHUNK_COUNT, "CHUNK_COUNT", TV_U32, 4},
    [TAG_SHA256] = {TAG_SHA256, "SHA256", TV_BYTES, 32},
    [TAG_CHUNK_START] = {TAG_CHUNK_START, "CHUNK_START", TV_U32, 4},
    [TAG_CHUNK_NUM] = {TAG_CHUNK_NUM, "CHUNK_NUM", TV_U16, 2},
    [TAG_VERSION] = {TAG_VERSION, "VERSION", TV_STR, 8},
    [TAG_REASON] = {TAG_REASON, "REASON", TV_U16, 2},
    [TAG_DETAIL] = {TAG_DETAIL, "DETAIL", TV_STR, 64},
    [TAG_STAGE] = {TAG_STAGE, "STAGE", TV_U8, 1},
    [TAG_SERVER_ADDR] = {TAG_SERVER_ADDR, "SERVER_ADDR", TV_BYTES, 4},
    [TAG_SERVER_PORT] = {TAG_SERVER_PORT, "SERVER_PORT", TV_U16, 2},
    [TAG_HEALTH_PROBE] = {TAG_HEALTH_PROBE, "HEALTH_PROBE", TV_U8, 1},
    [TAG_PATH_PROBE] = {TAG_PATH_PROBE, "PATH_PROBE", TV_U8, 1},
    [TAG_RESUMED] = {TAG_RESUMED, "RESUMED", TV_U8, 1},
    [TAG_SRV_STAGE] = {TAG_SRV_STAGE, "SRV_STAGE", TV_U8, 1},
    [TAG_DEAD_INTERVALS] = {TAG_DEAD_INTERVALS, "DEAD_INTERVALS", TV_U8, 1},
    [TAG_MAX_DGRAM] = {TAG_MAX_DGRAM, "MAX_DGRAM", TV_U16, 2},
    [TAG_INFO] = {TAG_INFO, "INFO", TV_U16, 2},
    [TAG_UPG_STATE] = {TAG_UPG_STATE, "UPG_STATE", TV_U8, 1},
    [TAG_REPORT_STATE] = {TAG_REPORT_STATE, "REPORT_STATE", TV_U8, 1},
    [TAG_ECHO_SEQ] = {TAG_ECHO_SEQ, "ECHO_SEQ", TV_U32, 4},
};

const ztp_tag_info_t *ztp_tag_info(uint8_t tag)
{
    if (tag > TAG_MAX_KNOWN || TAGS[tag].name == NULL) return NULL;
    return &TAGS[tag];
}

const char *ztp_reason_name(uint16_t code)
{
    static const char *const N[] = {
        "OK", "BAD_MAC", "REPLAY", "UNKNOWN_MODEL", "NO_POLICY", "RATE_LIMITED",
        "SESSION_UNKNOWN", "SESSION_EXPIRED", "RESUME_REJECTED", "BAD_REQUEST", "HASH_MISMATCH",
        "VALIDATION_FAILED", "ALREADY_REGISTERED", "SERVER_BUSY", "RESUME_REQUIRED",
    };
    return code < sizeof N / sizeof N[0] ? N[code] : "UNKNOWN";
}

/* ---- header ---- */

void ztp_hdr_encode(const ztp_hdr_t *h, uint8_t out[ZTP_HDR_LEN])
{
    be16_put(out + 0, ZTP_MAGIC);
    out[2] = ZTP_VERSION;
    out[3] = h->type;
    out[4] = h->flags;
    out[5] = ZTP_HDR_LEN;
    be16_put(out + 6, h->epoch);
    be32_put(out + 8, h->session_id);
    be32_put(out + 12, h->seq);
    be32_put(out + 16, h->ack);
    be32_put(out + 20, h->sack);
    be16_put(out + 24, h->payload_len);
    out[26] = h->key_id;
    out[27] = 0;
    memcpy(out + ZTP_MAC_OFF, h->mac, ZTP_MAC_LEN);
}

int ztp_hdr_decode(const uint8_t *buf, size_t len, size_t limit, uint8_t rx_dir, ztp_hdr_t *h)
{
    if (limit > ZTP_MAX_DGRAM_CAP) limit = ZTP_MAX_DGRAM_CAP;
    if (len < ZTP_HDR_LEN) return ZTP_E_SHORT;                        /* step 1 */
    if (len > limit) return ZTP_E_TOOBIG;
    if (be16_get(buf) != ZTP_MAGIC) return ZTP_E_MAGIC;               /* step 2 */
    if (buf[2] != ZTP_VERSION) return ZTP_E_VERSION;
    if (buf[5] != ZTP_HDR_LEN) return ZTP_E_HDRLEN;
    uint16_t plen = be16_get(buf + 24);
    if ((size_t)plen != len - ZTP_HDR_LEN) return ZTP_E_PLEN;         /* step 3 */
    const ztp_type_info_t *ti = ztp_type_info(buf[3]);                 /* step 4 */
    if (!ti) return ZTP_E_TYPE;
    if (rx_dir && !(ti->dir & rx_dir)) return ZTP_E_DIR;
    if ((buf[4] & ZF_RESERVED_MASK) || buf[27] != 0) return ZTP_E_FLAGS;

    h->type = buf[3];
    h->flags = buf[4];
    h->epoch = be16_get(buf + 6);
    h->session_id = be32_get(buf + 8);
    h->seq = be32_get(buf + 12);
    h->ack = be32_get(buf + 16);
    h->sack = be32_get(buf + 20);
    h->payload_len = plen;
    h->key_id = buf[26];
    memcpy(h->mac, buf + ZTP_MAC_OFF, ZTP_MAC_LEN);
    return ZTP_OK;
}

static void compute_mac(const uint8_t *frame, size_t frame_len, const uint8_t key[ZTP_KEY_LEN],
                        uint8_t out[SHA256_DIGEST_LEN])
{
    hmac_sha256_ctx c;
    hmac_sha256_init(&c, key, ZTP_KEY_LEN);
    hmac_sha256_update(&c, frame, ZTP_MAC_OFF);
    hmac_sha256_update(&c, frame + ZTP_HDR_LEN, frame_len - ZTP_HDR_LEN);
    hmac_sha256_final(&c, out);
    secure_zero(&c, sizeof c);
}

void ztp_mac_apply(uint8_t *frame, size_t frame_len, const uint8_t key[ZTP_KEY_LEN])
{
    uint8_t full[SHA256_DIGEST_LEN];
    compute_mac(frame, frame_len, key, full);
    memcpy(frame + ZTP_MAC_OFF, full, ZTP_MAC_LEN);
}

int ztp_mac_verify(const uint8_t *frame, size_t frame_len, const uint8_t key[ZTP_KEY_LEN])
{
    if (frame_len < ZTP_HDR_LEN) return 0;
    uint8_t full[SHA256_DIGEST_LEN];
    compute_mac(frame, frame_len, key, full);
    return ct_memeq(full, frame + ZTP_MAC_OFF, ZTP_MAC_LEN);
}

int ztp_frame_build(uint8_t *out, size_t out_cap, size_t limit, ztp_hdr_t *h,
                    const uint8_t *payload, uint16_t payload_len, const uint8_t key[ZTP_KEY_LEN])
{
    if (limit > ZTP_MAX_DGRAM_CAP) limit = ZTP_MAX_DGRAM_CAP;
    size_t total = (size_t)ZTP_HDR_LEN + payload_len;
    if (total > limit || total > out_cap) return ZTP_E_TOOBIG;
    h->payload_len = payload_len;
    memset(h->mac, 0, ZTP_MAC_LEN);
    ztp_hdr_encode(h, out);
    if (payload_len) memmove(out + ZTP_HDR_LEN, payload, payload_len);
    ztp_mac_apply(out, total, key);
    memcpy(h->mac, out + ZTP_MAC_OFF, ZTP_MAC_LEN);
    return (int)total;
}

/* ---- TLV writer ---- */

void tlv_writer_init(tlv_writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = ZTP_OK;
}

void tlv_put_bytes(tlv_writer_t *w, uint8_t tag, const void *v, uint16_t n)
{
    if (w->err) return;
    if (w->cap - w->len < 3u + n) { w->err = ZTP_E_TOOBIG; return; }
    w->buf[w->len] = tag;
    be16_put(w->buf + w->len + 1, n);
    if (n) memcpy(w->buf + w->len + 3, v, n);
    w->len += 3u + n;
}

void tlv_put_str(tlv_writer_t *w, uint8_t tag, const char *s)
{
    size_t n = strlen(s);
    const ztp_tag_info_t *ti = ztp_tag_info(tag);
    if (n > 0xFFFF || (ti && ti->kind == TV_STR && n > ti->max_len)) { w->err = ZTP_E_TOOBIG; return; }
    tlv_put_bytes(w, tag, s, (uint16_t)n);
}

void tlv_put_u8(tlv_writer_t *w, uint8_t tag, uint8_t v) { tlv_put_bytes(w, tag, &v, 1); }
void tlv_put_u16(tlv_writer_t *w, uint8_t tag, uint16_t v)
{ uint8_t b[2]; be16_put(b, v); tlv_put_bytes(w, tag, b, 2); }
void tlv_put_u32(tlv_writer_t *w, uint8_t tag, uint32_t v)
{ uint8_t b[4]; be32_put(b, v); tlv_put_bytes(w, tag, b, 4); }
void tlv_put_u64(tlv_writer_t *w, uint8_t tag, uint64_t v)
{ uint8_t b[8]; be64_put(b, v); tlv_put_bytes(w, tag, b, 8); }

/* ---- TLV parser ---- */

static void set_present(tlv_set_t *s, uint8_t tag)
{
    if (tag < 32) s->present_lo |= 1u << tag;
    else s->present_hi |= 1u << (tag - 32);
}

bool tlv_has(const tlv_set_t *s, uint8_t tag)
{
    if (tag > TAG_MAX_KNOWN) return false;
    return tag < 32 ? (s->present_lo >> tag) & 1u : (s->present_hi >> (tag - 32)) & 1u;
}

static bool value_ok(const ztp_tag_info_t *ti, uint16_t n)
{
    switch (ti->kind) {
    case TV_STR: return n <= ti->max_len;
    case TV_U8: return n == 1;
    case TV_U16: return n == 2;
    case TV_U32: return n == 4;
    case TV_U64: return n == 8;
    case TV_BYTES: return n == ti->max_len;
    default: return false;
    }
}

int tlv_parse(const uint8_t *p, size_t n, tlv_set_t *out)
{
    memset(out, 0, sizeof *out);
    size_t off = 0;
    while (off < n) {
        if (n - off < 3) return ZTP_E_TLV;
        uint8_t tag = p[off];
        uint16_t vlen = be16_get(p + off + 1);
        off += 3;
        if (vlen > n - off) return ZTP_E_TLV;
        const ztp_tag_info_t *ti = ztp_tag_info(tag);
        if (!ti) {
            out->unknown_skipped++;
        } else {
            if (tlv_has(out, tag)) return ZTP_E_DUPTAG;
            if (!value_ok(ti, vlen)) return ZTP_E_TLV;
            set_present(out, tag);
            out->val[tag] = p + off;
            out->len[tag] = vlen;
        }
        off += vlen;
    }
    return ZTP_OK;
}

bool tlv_get_str(const tlv_set_t *s, uint8_t tag, char *dst, size_t dst_sz)
{
    if (!dst_sz) return false;
    dst[0] = '\0';
    if (!tlv_has(s, tag)) return false;
    size_t n = s->len[tag];
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, s->val[tag], n);
    dst[n] = '\0';
    return true;
}

bool tlv_get_u8(const tlv_set_t *s, uint8_t tag, uint8_t *v)
{
    if (!tlv_has(s, tag) || s->len[tag] != 1) return false;
    *v = s->val[tag][0];
    return true;
}

bool tlv_get_u16(const tlv_set_t *s, uint8_t tag, uint16_t *v)
{
    if (!tlv_has(s, tag) || s->len[tag] != 2) return false;
    *v = be16_get(s->val[tag]);
    return true;
}

bool tlv_get_u32(const tlv_set_t *s, uint8_t tag, uint32_t *v)
{
    if (!tlv_has(s, tag) || s->len[tag] != 4) return false;
    *v = be32_get(s->val[tag]);
    return true;
}

bool tlv_get_u64(const tlv_set_t *s, uint8_t tag, uint64_t *v)
{
    if (!tlv_has(s, tag) || s->len[tag] != 8) return false;
    *v = be64_get(s->val[tag]);
    return true;
}

bool tlv_get_bytes(const tlv_set_t *s, uint8_t tag, const uint8_t **v, uint16_t *n)
{
    if (!tlv_has(s, tag)) return false;
    *v = s->val[tag];
    *n = s->len[tag];
    return true;
}

int tlv_require(const tlv_set_t *s, const uint8_t *tags, size_t n, uint8_t *missing)
{
    for (size_t i = 0; i < n; i++)
        if (!tlv_has(s, tags[i])) {
            if (missing) *missing = tags[i];
            return ZTP_E_MISSING;
        }
    return ZTP_OK;
}

bool tlv_peek_str(const uint8_t *p, size_t n, uint8_t tag, char *dst, size_t dst_sz)
{
    if (!dst_sz) return false;
    dst[0] = '\0';
    const ztp_tag_info_t *ti = ztp_tag_info(tag);
    if (!ti || ti->kind != TV_STR) return false;
    size_t off = 0;
    bool found = false;
    while (off < n) {
        if (n - off < 3) return false;
        uint8_t t = p[off];
        uint16_t vlen = be16_get(p + off + 1);
        off += 3;
        if (vlen > n - off) return false;
        if (t == tag) {
            if (found || vlen > ti->max_len || vlen >= dst_sz) return false;
            memcpy(dst, p + off, vlen);
            dst[vlen] = '\0';
            found = true;
        }
        off += vlen;
    }
    if (!found) dst[0] = '\0';
    return found;
}

/* ---- CHUNK_DATA ---- */

int ztp_chunk_encode(uint8_t *out, size_t cap, const ztp_chunk_t *c)
{
    size_t total = (size_t)ZTP_CHUNK_HDR_LEN + c->data_len;
    if (c->data_len > ZTP_CHUNK_SIZE_CAP || total > cap) return ZTP_E_TOOBIG;
    out[0] = c->file_id;
    out[1] = c->flags;
    be32_put(out + 2, c->chunk_index);
    be16_put(out + 6, c->data_len);
    if (c->data_len) memmove(out + ZTP_CHUNK_HDR_LEN, c->data, c->data_len);
    return (int)total;
}

int ztp_chunk_decode(const uint8_t *p, size_t n, ztp_chunk_t *c)
{
    if (n < ZTP_CHUNK_HDR_LEN) return ZTP_E_SHORT;
    c->file_id = p[0];
    c->flags = p[1];
    if (c->flags & ~CHUNK_F_MTU_REDUCED) return ZTP_E_FLAGS;
    c->chunk_index = be32_get(p + 2);
    c->data_len = be16_get(p + 6);
    if (c->data_len > ZTP_CHUNK_SIZE_CAP || (size_t)c->data_len != n - ZTP_CHUNK_HDR_LEN) return ZTP_E_TLV;
    if ((c->flags & CHUNK_F_MTU_REDUCED) && c->data_len != 0) return ZTP_E_TLV;
    c->data = p + ZTP_CHUNK_HDR_LEN;
    return ZTP_OK;
}
