/* TC-UNIT-03 (header/TLV round trip), TC-UNIT-04 (TLV bounds), TC-UNIT-06 (seq arithmetic),
 * plus frame build/verify, size limits (§5.5) and the CHUNK_DATA layout (§5.4.2). */
#include <stdlib.h>

#include "protocol.h"
#include "test.h"

static uint64_t rng = 88172645463325252ull;
static uint64_t xr(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static const uint8_t ALL_TYPES[] = {
    ZT_DISCOVER, ZT_OFFER, ZT_HELLO, ZT_HELLO_ACK, ZT_REGISTER, ZT_REGISTER_ACK, ZT_REGISTER_NAK,
    ZT_KEEPALIVE, ZT_KEEPALIVE_ACK, ZT_MANIFEST_REQ, ZT_MANIFEST, ZT_CHUNK_REQ, ZT_CHUNK_DATA,
    ZT_TRANSFER_DONE, ZT_UPGRADE_STATUS, ZT_STATUS_REPORT, ZT_STATUS_ACK, ZT_CLOSE, ZT_ERROR, ZT_ACK,
};

static void test_header_roundtrip(void)
{
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 3);
    int bad = 0, macbad = 0;
    for (size_t t = 0; t < sizeof ALL_TYPES; t++) {
        for (unsigned flags = 0; flags < 16; flags++) {
            ztp_hdr_t h = {0}, d;
            h.type = ALL_TYPES[t];
            h.flags = (uint8_t)flags;
            h.key_id = (uint8_t)(1 + xr() % 3);
            h.epoch = (uint16_t)xr();
            h.session_id = (uint32_t)xr();
            h.seq = (uint32_t)xr();
            h.ack = (uint32_t)xr();
            h.sack = (uint32_t)xr();
            uint8_t payload[64], frame[ZTP_MAX_DGRAM_CAP];
            uint16_t plen = (uint16_t)(xr() % sizeof payload);
            for (int i = 0; i < plen; i++) payload[i] = (uint8_t)xr();
            int n = ztp_frame_build(frame, sizeof frame, ZTP_MAX_DGRAM_CAP, &h, payload, plen, key);
            if (n != (int)(ZTP_HDR_LEN + plen)) { bad++; continue; }
            if (ztp_hdr_decode(frame, (size_t)n, ZTP_MAX_DGRAM_CAP, 0, &d) != ZTP_OK) { bad++; continue; }
            bad += d.type != h.type || d.flags != h.flags || d.key_id != h.key_id || d.epoch != h.epoch ||
                   d.session_id != h.session_id || d.seq != h.seq || d.ack != h.ack || d.sack != h.sack ||
                   d.payload_len != plen || memcmp(d.mac, h.mac, ZTP_MAC_LEN) != 0;
            macbad += ztp_mac_verify(frame, (size_t)n, key) != 1;
        }
    }
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(macbad, 0);
    CHECK_EQ_INT(ZTP_HDR_LEN, 44);
}

static void test_header_offsets(void)
{
    /* The byte layout of §5.2, checked field by field. */
    ztp_hdr_t h = {.type = ZT_HELLO, .flags = ZF_ACK_VALID | ZF_RESUME, .key_id = ZK_DEV, .epoch = 0x0102,
                   .session_id = 0x03040506, .seq = 0x0708090A, .ack = 0x0B0C0D0E, .sack = 0x0F101112,
                   .payload_len = 0x0013};
    for (int i = 0; i < 16; i++) h.mac[i] = (uint8_t)(0xA0 + i);
    uint8_t b[44];
    ztp_hdr_encode(&h, b);
    const uint8_t want[28] = {0x5A, 0x54, 1, 0x10, 0x05, 44, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                              0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x00, 0x13, 2, 0};
    CHECK(memcmp(b, want, 28) == 0);
    CHECK(b[28] == 0xA0 && b[43] == 0xAF);
}

static void test_header_rejects(void)
{
    uint8_t key[32] = {1}, f[ZTP_MAX_DGRAM_CAP];
    ztp_hdr_t h = {.type = ZT_REGISTER, .seq = 2}, d;
    int n = ztp_frame_build(f, sizeof f, 1200, &h, (const uint8_t *)"abc", 3, key);
    CHECK_EQ_INT(n, 47);
    CHECK_EQ_INT(ztp_hdr_decode(f, 43, 1200, 0, &d), ZTP_E_SHORT);
    CHECK_EQ_INT(ztp_hdr_decode(f, 46, 1200, 0, &d), ZTP_E_PLEN);
    CHECK_EQ_INT(ztp_hdr_decode(f, 47, 46, 0, &d), ZTP_E_TOOBIG);
    /* direction: a REGISTER (D->S) received by the device is rejected */
    CHECK_EQ_INT(ztp_hdr_decode(f, 47, 1200, ZD_S2D, &d), ZTP_E_DIR);
    CHECK_EQ_INT(ztp_hdr_decode(f, 47, 1200, ZD_D2S, &d), ZTP_OK);
    uint8_t g[47];
    memcpy(g, f, 47); g[0] = 0x5B;
    CHECK_EQ_INT(ztp_hdr_decode(g, 47, 1200, 0, &d), ZTP_E_MAGIC);
    memcpy(g, f, 47); g[2] = 2;
    CHECK_EQ_INT(ztp_hdr_decode(g, 47, 1200, 0, &d), ZTP_E_VERSION);
    memcpy(g, f, 47); g[5] = 48;
    CHECK_EQ_INT(ztp_hdr_decode(g, 47, 1200, 0, &d), ZTP_E_HDRLEN);
    memcpy(g, f, 47); g[3] = 0x55;
    CHECK_EQ_INT(ztp_hdr_decode(g, 47, 1200, 0, &d), ZTP_E_TYPE);
    memcpy(g, f, 47); g[4] = 0x10;
    CHECK_EQ_INT(ztp_hdr_decode(g, 47, 1200, 0, &d), ZTP_E_FLAGS);
    memcpy(g, f, 47); g[27] = 1;
    CHECK_EQ_INT(ztp_hdr_decode(g, 47, 1200, 0, &d), ZTP_E_FLAGS);
    /* limit above the hard cap is clamped to 1200 */
    uint8_t big[1300] = {0};
    memcpy(big, f, 44);
    be16_put(big + 24, 1201 - 44);
    CHECK_EQ_INT(ztp_hdr_decode(big, 1201, 5000, 0, &d), ZTP_E_TOOBIG);

    /* MAC: any flipped bit in the covered bytes or the MAC itself fails; wrong key fails */
    int undetected = 0;
    for (int i = 0; i < 47; i++) {
        memcpy(g, f, 47);
        g[i] ^= 0x40;
        undetected += ztp_mac_verify(g, 47, key);
    }
    CHECK_EQ_INT(undetected, 0);
    uint8_t key2[32] = {2};
    CHECK(ztp_mac_verify(f, 47, key2) == 0);
    CHECK(ztp_mac_verify(f, 47, key) == 1);
}

static void test_size_limits(void)
{
    uint8_t key[32] = {0}, f[ZTP_MAX_DGRAM_CAP], p[ZTP_MAX_DGRAM_CAP] = {0};
    ztp_hdr_t h = {.type = ZT_CHUNK_DATA};
    CHECK_EQ_INT(ztp_frame_build(f, sizeof f, 1200, &h, p, 1156, key), 1200);
    CHECK_EQ_INT(ztp_frame_build(f, sizeof f, 1200, &h, p, 1157, key), ZTP_E_TOOBIG);
    CHECK_EQ_INT(ztp_frame_build(f, sizeof f, 508, &h, p, 464, key), 508);
    CHECK_EQ_INT(ztp_frame_build(f, sizeof f, 508, &h, p, 465, key), ZTP_E_TOOBIG);
    /* default chunk frame is 1076 B, fallback 308 B, fill frame 52 B (§5.4.2, §5.8) */
    uint8_t data[1024] = {0}, pl[1100];
    ztp_chunk_t c = {.file_id = 2, .chunk_index = 63, .data_len = 1024, .data = data};
    int cl = ztp_chunk_encode(pl, sizeof pl, &c);
    CHECK_EQ_INT(ZTP_HDR_LEN + cl, 1076);
    c.data_len = 256;
    CHECK_EQ_INT(ZTP_HDR_LEN + ztp_chunk_encode(pl, sizeof pl, &c), 308);
    c.data_len = 0;
    c.flags = CHUNK_F_MTU_REDUCED;
    CHECK_EQ_INT(ZTP_HDR_LEN + ztp_chunk_encode(pl, sizeof pl, &c), 52);
}

static void test_chunk(void)
{
    uint8_t data[300], buf[400];
    for (int i = 0; i < 300; i++) data[i] = (uint8_t)i;
    ztp_chunk_t c = {.file_id = 1, .flags = 0, .chunk_index = 0xDEADBEEF, .data_len = 300, .data = data}, d;
    int n = ztp_chunk_encode(buf, sizeof buf, &c);
    CHECK_EQ_INT(n, 308);
    CHECK_EQ_INT(ztp_chunk_decode(buf, (size_t)n, &d), ZTP_OK);
    CHECK(d.file_id == 1 && d.chunk_index == 0xDEADBEEF && d.data_len == 300 && !memcmp(d.data, data, 300));
    CHECK_EQ_INT(ztp_chunk_decode(buf, 7, &d), ZTP_E_SHORT);
    CHECK_EQ_INT(ztp_chunk_decode(buf, (size_t)n - 1, &d), ZTP_E_TLV);     /* data_len mismatch */
    buf[1] = 0x02;
    CHECK_EQ_INT(ztp_chunk_decode(buf, (size_t)n, &d), ZTP_E_FLAGS);       /* reserved flag bit */
    buf[1] = CHUNK_F_MTU_REDUCED;
    CHECK_EQ_INT(ztp_chunk_decode(buf, (size_t)n, &d), ZTP_E_TLV);         /* fill frame must be empty */
    c.data_len = 1025;
    CHECK_EQ_INT(ztp_chunk_encode(buf, sizeof buf, &c), ZTP_E_TOOBIG);
}

static void test_tlv_roundtrip(void)
{
    uint8_t buf[ZTP_MAX_PAYLOAD];
    tlv_writer_t w;
    tlv_writer_init(&w, buf, sizeof buf);
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) sha[i] = (uint8_t)(0xF0 ^ i);
    uint8_t ip[4] = {10, 10, 10, 5};
    /* every tag in §5.4.1 */
    for (uint8_t tag = 1; tag <= TAG_MAX_KNOWN; tag++) {
        const ztp_tag_info_t *ti = ztp_tag_info(tag);
        CHECK(ti != NULL);
        if (!ti) continue;
        switch (ti->kind) {
        case TV_STR: {
            char s[65];
            memset(s, 'a' + tag % 26, ti->max_len);
            s[ti->max_len] = '\0';
            tlv_put_str(&w, tag, s);
            break;
        }
        case TV_U8: tlv_put_u8(&w, tag, (uint8_t)(tag + 1)); break;
        case TV_U16: tlv_put_u16(&w, tag, (uint16_t)(0x1000 + tag)); break;
        case TV_U32: tlv_put_u32(&w, tag, 0x10000000u + tag); break;
        case TV_U64: tlv_put_u64(&w, tag, 0x1000000000000000ull + tag); break;
        case TV_BYTES: tlv_put_bytes(&w, tag, ti->max_len == 32 ? (const void *)sha : (const void *)ip, ti->max_len); break;
        }
    }
    CHECK_EQ_INT(w.err, ZTP_OK);
    tlv_set_t s;
    CHECK_EQ_INT(tlv_parse(buf, w.len, &s), ZTP_OK);
    int bad = 0;
    for (uint8_t tag = 1; tag <= TAG_MAX_KNOWN; tag++) {
        const ztp_tag_info_t *ti = ztp_tag_info(tag);
        uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64; char str[65]; const uint8_t *bp; uint16_t bn;
        switch (ti->kind) {
        case TV_STR: bad += !tlv_get_str(&s, tag, str, sizeof str) || strlen(str) != ti->max_len || str[0] != 'a' + tag % 26; break;
        case TV_U8: bad += !tlv_get_u8(&s, tag, &u8) || u8 != tag + 1; break;
        case TV_U16: bad += !tlv_get_u16(&s, tag, &u16) || u16 != 0x1000 + tag; break;
        case TV_U32: bad += !tlv_get_u32(&s, tag, &u32) || u32 != 0x10000000u + tag; break;
        case TV_U64: bad += !tlv_get_u64(&s, tag, &u64) || u64 != 0x1000000000000000ull + tag; break;
        case TV_BYTES: bad += !tlv_get_bytes(&s, tag, &bp, &bn) || bn != ti->max_len; break;
        }
    }
    CHECK_EQ_INT(bad, 0);

    /* resume HELLO_ACK: RESUMED + SRV_STAGE + the usual fields parse without a duplicate-tag error */
    tlv_writer_init(&w, buf, sizeof buf);
    tlv_put_u64(&w, TAG_NONCE_DEV, 1);
    tlv_put_u64(&w, TAG_NONCE_SRV, 2);
    tlv_put_u16(&w, TAG_KEEPALIVE_S, 2);
    tlv_put_u8(&w, TAG_DEAD_INTERVALS, 3);
    tlv_put_u16(&w, TAG_GRACE_S, 30);
    tlv_put_u8(&w, TAG_RESUMED, 1);
    tlv_put_u8(&w, TAG_SRV_STAGE, SST_UPGRADING);
    tlv_put_str(&w, TAG_FW_TARGET, "v1.1");
    tlv_put_u16(&w, TAG_CFG_VER, 3);
    CHECK_EQ_INT(tlv_parse(buf, w.len, &s), ZTP_OK);
    static const uint8_t need[] = {TAG_NONCE_DEV, TAG_NONCE_SRV, TAG_RESUMED, TAG_SRV_STAGE, TAG_FW_TARGET};
    CHECK_EQ_INT(tlv_require(&s, need, sizeof need, NULL), ZTP_OK);
    uint8_t missing = 0;
    static const uint8_t need2[] = {TAG_NONCE_DEV, TAG_SERIAL};
    CHECK_EQ_INT(tlv_require(&s, need2, sizeof need2, &missing), ZTP_E_MISSING);
    CHECK_EQ_INT(missing, TAG_SERIAL);

    /* enumerations of §5.4.3 have the documented values */
    CHECK(ST_NEW == 0 && ST_OPERATIONAL == 5 && ST_FAILED == 6);
    CHECK(SST_HELLO == 0 && SST_OPERATIONAL == 6 && SST_FAILED == 7);
    CHECK(UPG_INSTALLING == 1 && UPG_ROLLED_BACK == 3 && RPT_FAILED == 2 && FK_FIRMWARE == 2);
    CHECK(CAP_MCAST_DISCOVERY == 1 && CAP_RESUME == 2 && CAP_MTU_FALLBACK == 4);
    CHECK_STR(ztp_reason_name(RC_RESUME_REQUIRED), "RESUME_REQUIRED");
    CHECK_STR(ztp_type_name(ZT_KEEPALIVE_ACK), "KEEPALIVE_ACK");
}

static void test_tlv_bounds(void)
{
    tlv_set_t s;
    uint8_t b[64];
    /* truncated header */
    b[0] = TAG_SERIAL; b[1] = 0;
    CHECK_EQ_INT(tlv_parse(b, 2, &s), ZTP_E_TLV);
    /* length beyond the payload */
    b[0] = TAG_SERIAL; be16_put(b + 1, 10); memcpy(b + 3, "ZTP-93", 6);
    CHECK_EQ_INT(tlv_parse(b, 9, &s), ZTP_E_TLV);
    /* duplicate tag */
    tlv_writer_t w;
    tlv_writer_init(&w, b, sizeof b);
    tlv_put_u16(&w, TAG_CFG_VER, 1);
    tlv_put_u16(&w, TAG_CFG_VER, 2);
    CHECK_EQ_INT(tlv_parse(b, w.len, &s), ZTP_E_DUPTAG);
    /* unknown tag skipped (also a duplicated unknown tag) */
    tlv_writer_init(&w, b, sizeof b);
    tlv_put_bytes(&w, 0xEE, "xyz", 3);
    tlv_put_bytes(&w, 0xEE, "xyz", 3);
    tlv_put_u16(&w, TAG_CFG_VER, 7);
    CHECK_EQ_INT(tlv_parse(b, w.len, &s), ZTP_OK);
    CHECK_EQ_INT(s.unknown_skipped, 2);
    uint16_t v = 0;
    CHECK(tlv_get_u16(&s, TAG_CFG_VER, &v) && v == 7);
    /* zero-length string accepted, read back as "" */
    tlv_writer_init(&w, b, sizeof b);
    tlv_put_bytes(&w, TAG_TENANT, "", 0);
    CHECK_EQ_INT(tlv_parse(b, w.len, &s), ZTP_OK);
    char t[33] = "x";
    CHECK(tlv_get_str(&s, TAG_TENANT, t, sizeof t) && t[0] == '\0');
    /* oversize string rejected (SERIAL ≤ 16) — by the writer and by the parser */
    tlv_writer_init(&w, b, sizeof b);
    tlv_put_str(&w, TAG_SERIAL, "ZTP-0123456789ABC");
    CHECK_EQ_INT(w.err, ZTP_E_TOOBIG);
    b[0] = TAG_SERIAL; be16_put(b + 1, 17); memset(b + 3, 'A', 17);
    CHECK_EQ_INT(tlv_parse(b, 20, &s), ZTP_E_TLV);
    /* wrong fixed width */
    b[0] = TAG_NONCE_REG; be16_put(b + 1, 4); memset(b + 3, 0, 4);
    CHECK_EQ_INT(tlv_parse(b, 7, &s), ZTP_E_TLV);
    /* writer overflow is sticky */
    uint8_t small[8];
    tlv_writer_init(&w, small, sizeof small);
    tlv_put_u32(&w, TAG_CAPS, 1);      /* 7 bytes: fits */
    tlv_put_u8(&w, TAG_STAGE, 1);      /* 4 more: does not */
    tlv_put_u8(&w, TAG_STAGE, 1);
    CHECK_EQ_INT(w.err, ZTP_E_TOOBIG);
    CHECK_EQ_INT(w.len, 7);
    /* peek: finds a string, rejects duplicates and overruns */
    tlv_writer_init(&w, b, sizeof b);
    tlv_put_str(&w, TAG_MODEL, "EDGE-GW-100");
    tlv_put_str(&w, TAG_SERIAL, "ZTP-93AF21");
    char out[17];
    CHECK(tlv_peek_str(b, w.len, TAG_SERIAL, out, sizeof out) && !strcmp(out, "ZTP-93AF21"));
    CHECK(!tlv_peek_str(b, w.len - 1, TAG_SERIAL, out, sizeof out));
    tlv_put_str(&w, TAG_SERIAL, "ZTP-000000");
    CHECK(!tlv_peek_str(b, w.len, TAG_SERIAL, out, sizeof out));

    /* random garbage never crashes and never reports out-of-range values (run under ASan) */
    int ok = 0;
    for (int iter = 0; iter < 200000; iter++) {
        size_t n = xr() % 48;
        uint8_t *g = malloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) g[i] = (uint8_t)(xr() % 4 == 0 ? xr() % 0x28 : xr());
        if (tlv_parse(g, n, &s) == ZTP_OK) {
            ok++;
            for (uint8_t tag = 1; tag <= TAG_MAX_KNOWN; tag++)
                if (tlv_has(&s, tag) && (s.val[tag] < g || s.val[tag] + s.len[tag] > g + n)) CHECK(!"TLV value out of bounds");
        }
        (void)tlv_peek_str(g, n, TAG_SERIAL, out, sizeof out);
        free(g);
    }
    CHECK(ok > 0);
}

static void test_seq(void)
{
    CHECK(seq_lt(1, 2));
    CHECK(!seq_lt(2, 1));
    CHECK(!seq_lt(5, 5) && seq_le(5, 5));
    CHECK(seq_lt(0xFFFFFFFFu, 0));          /* wraparound 2^32-1 -> 0 */
    CHECK(seq_lt(0xFFFFFFF0u, 0x10));
    CHECK(!seq_lt(0x10, 0xFFFFFFF0u));
    CHECK(seq_lt(0x7FFFFFFFu, 0x80000000u));
    /* the forced re-key at next_seq > ZTP_REKEY_SEQ (2^30) is exercised end to end in tests/m5_upgrade.sh
     * (--rekey-at stands in for the threshold); here: the threshold is far from the wrap */
    CHECK(seq_lt(ZTP_REKEY_SEQ, ZTP_REKEY_SEQ + 0x7FFFFFFFu));
}

void suite_protocol(void)
{
    test_header_roundtrip();
    test_header_offsets();
    test_header_rejects();
    test_size_limits();
    test_chunk();
    test_tlv_roundtrip();
    test_tlv_bounds();
    test_seq();
}
