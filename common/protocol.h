/* ZTP wire protocol v1 (DESIGN §5, normative). Header and TLV codec, MAC apply/verify. */
#ifndef ZTP_PROTOCOL_H
#define ZTP_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

/* ---- Fixed invariants (Appendix B.1) ---- */
#define ZTP_MAGIC           0x5A54u
#define ZTP_VERSION         1u
#define ZTP_HDR_LEN         44u
#define ZTP_MAC_OFF         28u     /* MAC covers hdr[0..27] || payload */
#define ZTP_MAC_LEN         16u
#define ZTP_MAX_DGRAM_CAP   1200u   /* upper bound for net.max_datagram */
#define ZTP_MIN_DGRAM       508u    /* §5.8 fallback level */
#define ZTP_CHUNK_SIZE_CAP  1024u   /* upper bound for xfer.chunk_size */
#define ZTP_CHUNK_SIZE_MIN  256u    /* fallback chunk size */
#define ZTP_CHUNK_HDR_LEN   8u      /* CHUNK_DATA fixed prefix, §5.4.2 */
#define ZTP_MAX_PAYLOAD     (ZTP_MAX_DGRAM_CAP - ZTP_HDR_LEN)
#define ZTP_RX_WINDOW       32u
#define ZTP_TX_WINDOW_MAX   16u
#define ZTP_REKEY_SEQ       (1u << 30)

_Static_assert(ZTP_HDR_LEN + ZTP_CHUNK_HDR_LEN + ZTP_CHUNK_SIZE_CAP <= ZTP_MAX_DGRAM_CAP,
               "chunk frame exceeds datagram cap");
_Static_assert(ZTP_HDR_LEN + ZTP_CHUNK_HDR_LEN + ZTP_CHUNK_SIZE_MIN <= ZTP_MIN_DGRAM,
               "fallback chunk exceeds fallback cap");

/* ---- Header flags (§5.2) ---- */
enum ztp_flags { ZF_ACK_VALID = 1u << 0, ZF_RETX = 1u << 1, ZF_RESUME = 1u << 2, ZF_FIN = 1u << 3 };
#define ZF_RESERVED_MASK 0xF0u

/* key_id hint (§5.2) — never used to select the verification key */
enum ztp_key { ZK_BOOT = 1, ZK_DEV = 2, ZK_SESSION = 3 };

/* ---- Message types (§5.3) ---- */
enum ztp_type {
    ZT_DISCOVER = 0x01, ZT_OFFER = 0x02,
    ZT_HELLO = 0x10, ZT_HELLO_ACK = 0x11,
    ZT_REGISTER = 0x20, ZT_REGISTER_ACK = 0x21, ZT_REGISTER_NAK = 0x22,
    ZT_KEEPALIVE = 0x30, ZT_KEEPALIVE_ACK = 0x31,
    ZT_MANIFEST_REQ = 0x40, ZT_MANIFEST = 0x41, ZT_CHUNK_REQ = 0x42, ZT_CHUNK_DATA = 0x43,
    ZT_TRANSFER_DONE = 0x44,
    ZT_UPGRADE_STATUS = 0x50,
    ZT_STATUS_REPORT = 0x60, ZT_STATUS_ACK = 0x61,
    ZT_CLOSE = 0x70,
    ZT_ERROR = 0x7E, ZT_ACK = 0x7F,
};

enum ztp_class { ZC_DATAGRAM = 1, ZC_RELIABLE = 2, ZC_ACKCLASS = 3, ZC_TERMINAL = 4 };
enum ztp_dir   { ZD_D2S = 1, ZD_S2D = 2, ZD_BOTH = 3 };

typedef struct {
    uint8_t     type;
    const char *name;
    uint8_t     cls;   /* enum ztp_class (ERROR is TERMINAL except BAD_REQUEST, which is reliable) */
    uint8_t     dir;   /* enum ztp_dir */
} ztp_type_info_t;

const ztp_type_info_t *ztp_type_info(uint8_t type);   /* NULL if unknown */
const char *ztp_type_name(uint8_t type);              /* "UNKNOWN" if unknown */

/* ---- TLV tags (§5.4.1) ---- */
enum ztp_tag {
    TAG_SERIAL = 0x01, TAG_MODEL = 0x02, TAG_FW_VER = 0x03, TAG_HW_REV = 0x04,
    TAG_NONCE_DEV = 0x05, TAG_NONCE_SRV = 0x06, TAG_NONCE_REG = 0x07, TAG_TENANT = 0x08,
    TAG_FW_TARGET = 0x09, TAG_CFG_VER = 0x0A, TAG_KEEPALIVE_S = 0x0B, TAG_GRACE_S = 0x0C,
    TAG_RESUME_CTR = 0x0D, TAG_CAPS = 0x0E, TAG_DISC_NONCE = 0x0F, TAG_FILE_ID = 0x10,
    TAG_FILE_KIND = 0x11, TAG_FILE_SIZE = 0x12, TAG_CHUNK_SIZE = 0x13, TAG_CHUNK_COUNT = 0x14,
    TAG_SHA256 = 0x15, TAG_CHUNK_START = 0x16, TAG_CHUNK_NUM = 0x17, TAG_VERSION = 0x18,
    TAG_REASON = 0x19, TAG_DETAIL = 0x1A, TAG_STAGE = 0x1B, TAG_SERVER_ADDR = 0x1C,
    TAG_SERVER_PORT = 0x1D, TAG_HEALTH_PROBE = 0x1E, TAG_PATH_PROBE = 0x1F, TAG_RESUMED = 0x20,
    TAG_SRV_STAGE = 0x21, TAG_DEAD_INTERVALS = 0x22, TAG_MAX_DGRAM = 0x23, TAG_INFO = 0x24,
    TAG_UPG_STATE = 0x25, TAG_REPORT_STATE = 0x26, TAG_ECHO_SEQ = 0x27,
    TAG_MAX_KNOWN = 0x27,
};
#define TAG_RESULT TAG_REASON

/* Value kinds and their bounds; the decoder enforces them. */
enum ztp_vkind { TV_STR = 1, TV_U8, TV_U16, TV_U32, TV_U64, TV_BYTES };
typedef struct {
    uint8_t     tag;
    const char *name;
    uint8_t     kind;     /* enum ztp_vkind */
    uint16_t    max_len;  /* TV_STR: max bytes; TV_BYTES: exact length */
} ztp_tag_info_t;
const ztp_tag_info_t *ztp_tag_info(uint8_t tag);   /* NULL if unknown */

/* ---- Enumerations (§5.4.3) ---- */
enum ztp_stage { ST_NEW = 0, ST_REGISTERED, ST_CONFIG_OK, ST_FW_STAGED, ST_FW_INSTALLED,
                 ST_OPERATIONAL, ST_FAILED };
enum ztp_srv_stage { SST_HELLO = 0, SST_REGISTERED, SST_CONFIG_SENT, SST_FW_SENT, SST_UPGRADING,
                     SST_UPGRADED, SST_OPERATIONAL, SST_FAILED };
enum ztp_upg_state { UPG_INSTALLING = 1, UPG_INSTALLED = 2, UPG_ROLLED_BACK = 3 };
enum ztp_report_state { RPT_OPERATIONAL = 1, RPT_FAILED = 2 };
enum ztp_file_kind { FK_CONFIG = 1, FK_FIRMWARE = 2 };
enum ztp_caps { CAP_MCAST_DISCOVERY = 1u << 0, CAP_RESUME = 1u << 1, CAP_MTU_FALLBACK = 1u << 2 };
#define CHUNK_F_MTU_REDUCED 0x01u

/* ---- Reason / error codes (Appendix C) ---- */
enum ztp_reason {
    RC_OK = 0, RC_BAD_MAC = 1, RC_REPLAY = 2, RC_UNKNOWN_MODEL = 3, RC_NO_POLICY = 4,
    RC_RATE_LIMITED = 5, RC_SESSION_UNKNOWN = 6, RC_SESSION_EXPIRED = 7, RC_RESUME_REJECTED = 8,
    RC_BAD_REQUEST = 9, RC_HASH_MISMATCH = 10, RC_VALIDATION_FAILED = 11, RC_ALREADY_REGISTERED = 12,
    RC_SERVER_BUSY = 13, RC_RESUME_REQUIRED = 14,
};
const char *ztp_reason_name(uint16_t code);

/* ---- Header (§5.2) ---- */
typedef struct {
    uint8_t  type, flags, key_id;
    uint16_t epoch, payload_len;
    uint32_t session_id, seq, ack, sack;
    uint8_t  mac[ZTP_MAC_LEN];
} ztp_hdr_t;

/* Error codes returned by the codec (negative). */
#define ZTP_OK          0
#define ZTP_E_SHORT    -1   /* too short / truncated */
#define ZTP_E_TOOBIG   -2   /* exceeds limit (E2BIG) */
#define ZTP_E_MAGIC    -3
#define ZTP_E_VERSION  -4
#define ZTP_E_HDRLEN   -5
#define ZTP_E_PLEN     -6   /* payload_len != len - 44 */
#define ZTP_E_TYPE     -7   /* unknown type */
#define ZTP_E_DIR      -8   /* type not valid in this direction */
#define ZTP_E_FLAGS    -9   /* reserved flag bits set, or reserved byte non-zero */
#define ZTP_E_DUPTAG  -10
#define ZTP_E_TLV     -11   /* malformed TLV (overrun / bad length) */
#define ZTP_E_MISSING -12   /* required tag missing */
#define ZTP_E_MAC     -13

/* Encode a header into out[0..43]. The MAC field is written from h->mac. */
void ztp_hdr_encode(const ztp_hdr_t *h, uint8_t out[ZTP_HDR_LEN]);

/* §5.6 steps 1–4: length vs limit, magic/version/hdr_len, payload_len, type and direction.
 * rx_dir is the direction this endpoint receives (ZD_D2S on the server, ZD_S2D on the device),
 * or 0 to skip the direction check. Decodes into *h on success. */
int ztp_hdr_decode(const uint8_t *buf, size_t len, size_t limit, uint8_t rx_dir, ztp_hdr_t *h);

/* Compute and store the truncated MAC over frame[0..27] || frame[44..]. frame_len >= 44. */
void ztp_mac_apply(uint8_t *frame, size_t frame_len, const uint8_t key[ZTP_KEY_LEN]);
/* Constant-time MAC verification: 1 if valid, 0 otherwise. */
int ztp_mac_verify(const uint8_t *frame, size_t frame_len, const uint8_t key[ZTP_KEY_LEN]);

/* Build a full frame: header + payload, then MAC. Returns the frame length, or ZTP_E_TOOBIG if
 * 44 + payload_len exceeds limit (the session's current datagram limit, ≤ 1200). The header's
 * payload_len field is set from payload_len. */
int ztp_frame_build(uint8_t *out, size_t out_cap, size_t limit, ztp_hdr_t *h,
                    const uint8_t *payload, uint16_t payload_len, const uint8_t key[ZTP_KEY_LEN]);

/* ---- TLV writer ---- */
typedef struct {
    uint8_t *buf;
    size_t   cap, len;
    int      err;   /* sticky: ZTP_E_TOOBIG once any put overflows */
} tlv_writer_t;

void tlv_writer_init(tlv_writer_t *w, uint8_t *buf, size_t cap);
void tlv_put_bytes(tlv_writer_t *w, uint8_t tag, const void *v, uint16_t n);
void tlv_put_str(tlv_writer_t *w, uint8_t tag, const char *s);
void tlv_put_u8(tlv_writer_t *w, uint8_t tag, uint8_t v);
void tlv_put_u16(tlv_writer_t *w, uint8_t tag, uint16_t v);
void tlv_put_u32(tlv_writer_t *w, uint8_t tag, uint32_t v);
void tlv_put_u64(tlv_writer_t *w, uint8_t tag, uint64_t v);

/* ---- TLV parser ----
 * Parses a whole payload in one pass with bounds checks. Unknown tags are skipped; a duplicate
 * known tag, an overrun, or a value violating its tag's kind/length rejects the payload. */
typedef struct {
    uint32_t present_lo, present_hi;   /* bitmap over tags 0..63 */
    const uint8_t *val[TAG_MAX_KNOWN + 1];
    uint16_t       len[TAG_MAX_KNOWN + 1];
    uint16_t       unknown_skipped;
} tlv_set_t;

int  tlv_parse(const uint8_t *p, size_t n, tlv_set_t *out);
bool tlv_has(const tlv_set_t *s, uint8_t tag);
/* Getters return false if absent. Strings are copied and always NUL-terminated (dst_sz ≥ 1). */
bool tlv_get_str(const tlv_set_t *s, uint8_t tag, char *dst, size_t dst_sz);
bool tlv_get_u8(const tlv_set_t *s, uint8_t tag, uint8_t *v);
bool tlv_get_u16(const tlv_set_t *s, uint8_t tag, uint16_t *v);
bool tlv_get_u32(const tlv_set_t *s, uint8_t tag, uint32_t *v);
bool tlv_get_u64(const tlv_set_t *s, uint8_t tag, uint64_t *v);
bool tlv_get_bytes(const tlv_set_t *s, uint8_t tag, const uint8_t **v, uint16_t *n);
/* Returns ZTP_OK if every tag in tags[0..n-1] is present, else ZTP_E_MISSING and *missing. */
int  tlv_require(const tlv_set_t *s, const uint8_t *tags, size_t n, uint8_t *missing);

/* Bounded, read-only peek for one string tag without building a tlv_set (used on the resume
 * path before the MAC check, §5.6 step 5). Returns false on absence or any malformation. */
bool tlv_peek_str(const uint8_t *p, size_t n, uint8_t tag, char *dst, size_t dst_sz);

/* ---- CHUNK_DATA fixed layout (§5.4.2) ---- */
typedef struct {
    uint8_t  file_id, flags;
    uint32_t chunk_index;
    uint16_t data_len;
    const uint8_t *data;
} ztp_chunk_t;

int ztp_chunk_encode(uint8_t *out, size_t cap, const ztp_chunk_t *c);   /* returns length */
int ztp_chunk_decode(const uint8_t *p, size_t n, ztp_chunk_t *c);

/* Serial-number arithmetic on 32-bit sequence numbers (RFC 1982 style). */
static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

/* Big-endian helpers. */
static inline void be16_put(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void be32_put(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline uint16_t be16_get(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32_get(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void be64_put(uint8_t *p, uint64_t v) { be32_put(p, (uint32_t)(v >> 32)); be32_put(p + 4, (uint32_t)v); }
static inline uint64_t be64_get(const uint8_t *p) { return (uint64_t)be32_get(p) << 32 | be32_get(p + 4); }

#endif
