/* pcapcheck: wire-level checks on a packet capture of ZTP traffic (QA TC-REL-01; later TC-PROV-04/05).
 *
 *   pcapcheck [--dump-hdr] [--max-size N] FILE.pcap PORT [PORT...]
 *   pcapcheck --selftest DIR
 *
 * For every UDP datagram to or from one of the ports:
 *   - it must decode as a valid ZTP header (magic, version, hdr_len, payload_len, known type);
 *   - IP fragments are counted (must be 0: the protocol never fragments, DESIGN §5.5);
 *   - the largest datagram is reported, and checked against --max-size if given;
 *   - for reliable frames, per stream (source, destination, session id, epoch), every seq between the
 *     lowest and highest seen appears at least once (originals or retransmissions): no gaps.
 * Link types: DLT_NULL / DLT_LOOP (macOS lo0), DLT_EN10MB (Linux lo), LINUX_SLL, LINUX_SLL2, RAW.
 * Exit status: 0 all checks pass, 1 a check failed, 2 usage or file error. */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

#define MAX_STREAMS 4096
#define MAX_SEQ     8192

typedef struct {
    uint32_t src_ip, dst_ip, sid;
    uint16_t src_port, dst_port, epoch;
    uint32_t lo, hi;
    uint8_t  seen[MAX_SEQ / 8];
    bool     overflow;
} stream_t;

static stream_t *g_streams;
static int g_nstreams;

static stream_t *stream_for(uint32_t sip, uint16_t sp, uint32_t dip, uint16_t dp, uint32_t sid, uint16_t ep)
{
    for (int i = 0; i < g_nstreams; i++) {
        stream_t *s = &g_streams[i];
        if (s->src_ip == sip && s->src_port == sp && s->dst_ip == dip && s->dst_port == dp && s->sid == sid && s->epoch == ep)
            return s;
    }
    if (g_nstreams >= MAX_STREAMS) return NULL;
    stream_t *s = &g_streams[g_nstreams++];
    memset(s, 0, sizeof *s);
    s->src_ip = sip; s->src_port = sp; s->dst_ip = dip; s->dst_port = dp; s->sid = sid; s->epoch = ep;
    s->lo = UINT32_MAX;
    return s;
}

static uint32_t rd32(const uint8_t *p, bool swap)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return swap ? __builtin_bswap32(v) : v;
}

typedef struct {
    long datagrams, ztp, invalid, fragments, other;
    size_t max_size;
} stats_t;

static void fmt_ep(uint32_t ip, uint16_t port, char *out)
{
    struct in_addr a = {ip};
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, b, sizeof b);
    snprintf(out, 32, "%s:%u", b, (unsigned)port);
}

static int check(const char *path, const uint16_t *ports, int nports, bool dump, size_t max_size, bool quiet)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    uint8_t gh[24];
    if (fread(gh, 1, 24, f) != 24) { fprintf(stderr, "%s: short pcap header\n", path); fclose(f); return 2; }
    uint32_t magic;
    memcpy(&magic, gh, 4);
    bool swap;
    if (magic == 0xa1b2c3d4u || magic == 0xa1b23c4du) swap = false;
    else if (magic == 0xd4c3b2a1u || magic == 0x4d3cb2a1u) swap = true;
    else { fprintf(stderr, "%s: not a pcap file (magic %08x)\n", path, magic); fclose(f); return 2; }
    uint32_t linktype = rd32(gh + 20, swap) & 0x0FFFFFFF;
    size_t l2;
    switch (linktype) {
    case 0: case 108: l2 = 4; break;      /* DLT_NULL, DLT_LOOP */
    case 1: l2 = 14; break;               /* Ethernet */
    case 113: l2 = 16; break;             /* LINUX_SLL */
    case 276: l2 = 20; break;             /* LINUX_SLL2 */
    case 12: case 101: l2 = 0; break;     /* RAW */
    default: fprintf(stderr, "%s: unsupported link type %u\n", path, linktype); fclose(f); return 2;
    }
    g_streams = calloc(MAX_STREAMS, sizeof *g_streams);
    g_nstreams = 0;
    stats_t st = {0};
    static uint8_t pkt[262144];
    uint8_t rh[16];
    while (fread(rh, 1, 16, f) == 16) {
        uint32_t incl = rd32(rh + 8, swap);
        if (incl > sizeof pkt || fread(pkt, 1, incl, f) != incl) break;
        if (incl < l2 + 20) continue;
        if (linktype == 1 && (pkt[12] != 0x08 || pkt[13] != 0x00)) continue;      /* not IPv4 */
        if (linktype == 113 && (pkt[14] != 0x08 || pkt[15] != 0x00)) continue;
        if (linktype == 276 && (pkt[0] != 0x08 || pkt[1] != 0x00)) continue;
        const uint8_t *ip = pkt + l2;
        size_t iplen = incl - l2;
        if ((ip[0] >> 4) != 4 || ip[9] != 17) continue;                            /* IPv4 UDP only */
        size_t ihl = (size_t)(ip[0] & 15) * 4;
        uint16_t frag = (uint16_t)(ip[6] << 8 | ip[7]);
        uint32_t sip, dip;
        memcpy(&sip, ip + 12, 4);
        memcpy(&dip, ip + 16, 4);
        if ((frag & 0x2000) || (frag & 0x1FFF)) {   /* MF set, or a non-zero offset */
            st.fragments++;
            continue;
        }
        if (iplen < ihl + 8) continue;
        const uint8_t *udp = ip + ihl;
        uint16_t sp = (uint16_t)(udp[0] << 8 | udp[1]), dp = (uint16_t)(udp[2] << 8 | udp[3]);
        uint16_t ulen = (uint16_t)(udp[4] << 8 | udp[5]);
        bool ours = false;
        for (int i = 0; i < nports; i++) ours |= sp == ports[i] || dp == ports[i];
        if (!ours) continue;
        st.datagrams++;
        if (ulen < 8 || (size_t)ulen > iplen - ihl) { st.invalid++; continue; }
        const uint8_t *pl = udp + 8;
        size_t plen = (size_t)ulen - 8;
        if (plen > st.max_size) st.max_size = plen;
        ztp_hdr_t h;
        int rc = ztp_hdr_decode(pl, plen, ZTP_MAX_DGRAM_CAP, 0, &h);
        char a[32], b[32];
        fmt_ep(sip, sp, a);
        fmt_ep(dip, dp, b);
        if (rc != ZTP_OK) {
            st.invalid++;
            if (!quiet) fprintf(stderr, "invalid ZTP datagram %s -> %s (%zu bytes, decode %d)\n", a, b, plen, rc);
            continue;
        }
        st.ztp++;
        if (dump)
            printf("%s -> %s %-14s sid=%u epoch=%u seq=%u ack=%u sack=%08x flags=%02x len=%zu\n", a, b,
                   ztp_type_name(h.type), (unsigned)h.session_id, (unsigned)h.epoch, (unsigned)h.seq,
                   (unsigned)h.ack, (unsigned)h.sack, (unsigned)h.flags, plen);
        const ztp_type_info_t *ti = ztp_type_info(h.type);
        if (ti->cls != ZC_RELIABLE || h.session_id == 0) continue;   /* HELLO (sid 0) starts the stream at seq 1 */
        stream_t *s = stream_for(sip, sp, dip, dp, h.session_id, h.epoch);
        if (!s) continue;
        if (h.seq >= MAX_SEQ) { s->overflow = true; continue; }
        s->seen[h.seq / 8] |= (uint8_t)(1u << (h.seq % 8));
        if (h.seq < s->lo) s->lo = h.seq;
        if (h.seq > s->hi) s->hi = h.seq;
    }
    fclose(f);
    int gaps = 0;
    for (int i = 0; i < g_nstreams; i++) {
        stream_t *s = &g_streams[i];
        for (uint32_t q = s->lo; q <= s->hi; q++)
            if (!(s->seen[q / 8] & (1u << (q % 8)))) {
                char a[32], b[32];
                fmt_ep(s->src_ip, s->src_port, a);
                fmt_ep(s->dst_ip, s->dst_port, b);
                if (!quiet) fprintf(stderr, "gap: %s -> %s sid=%u epoch=%u: seq %u missing (seen %u..%u)\n", a, b,
                        (unsigned)s->sid, (unsigned)s->epoch, (unsigned)q, (unsigned)s->lo, (unsigned)s->hi);
                gaps++;
                break;
            }
    }
    bool size_ok = !max_size || st.max_size <= max_size;
    if (!quiet)
        printf("pcapcheck: %ld datagrams on the test ports, %ld valid ZTP, %ld invalid, %ld other, %ld IP fragments, "
               "largest %zu B, %d reliable streams, %d with gaps\n",
               st.datagrams, st.ztp, st.invalid, st.other, st.fragments, st.max_size, g_nstreams, gaps);
    free(g_streams);
    return (st.ztp > 0 && st.invalid == 0 && st.fragments == 0 && gaps == 0 && size_ok) ? 0 : 1;
}

/* ---- self-test: write synthetic captures and check that good ones pass and bad ones fail ---- */

static void wr32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static void write_pkt(FILE *f, uint32_t link, const uint8_t *z, size_t zl, uint16_t sp, uint16_t dp, bool frag)
{
    uint8_t buf[2048];
    size_t l2 = link == 0 ? 4 : 14;
    memset(buf, 0, sizeof buf);
    if (link == 0) { uint32_t af = 2; memcpy(buf, &af, 4); }
    else { buf[12] = 0x08; buf[13] = 0x00; }
    uint8_t *ip = buf + l2;
    size_t tot = 20 + 8 + zl;
    ip[0] = 0x45; ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot; ip[8] = 64; ip[9] = 17;
    if (frag) ip[6] = 0x20;   /* MF */
    ip[12] = 127; ip[15] = 1; ip[16] = 127; ip[19] = 1;
    uint8_t *u = ip + 20;
    u[0] = (uint8_t)(sp >> 8); u[1] = (uint8_t)sp; u[2] = (uint8_t)(dp >> 8); u[3] = (uint8_t)dp;
    u[4] = (uint8_t)((8 + zl) >> 8); u[5] = (uint8_t)(8 + zl);
    memcpy(u + 8, z, zl);
    uint32_t n = (uint32_t)(l2 + tot);
    wr32(f, 0); wr32(f, 0); wr32(f, n); wr32(f, n);
    fwrite(buf, 1, n, f);
}

static void write_capture(const char *path, uint32_t link, bool gap, bool frag, bool garbage)
{
    FILE *f = fopen(path, "wb");
    wr32(f, 0xa1b2c3d4u);
    uint16_t v[2] = {2, 4};
    fwrite(v, 2, 2, f);
    wr32(f, 0); wr32(f, 0); wr32(f, 65535); wr32(f, link);
    uint8_t key[32] = {1}, z[ZTP_MAX_DGRAM_CAP];
    for (uint32_t seq = 1; seq <= 6; seq++) {
        if (gap && seq == 4) continue;
        ztp_hdr_t h = {0};
        h.type = seq == 1 ? ZT_HELLO : ZT_KEEPALIVE;
        h.session_id = seq == 1 ? 0 : 7421;
        h.seq = seq;
        int n = ztp_frame_build(z, sizeof z, ZTP_MAX_DGRAM_CAP, &h, NULL, 0, key);
        write_pkt(f, link, z, (size_t)n, 40000, 5555, false);
        if (seq == 3) write_pkt(f, link, z, (size_t)n, 40000, 5555, false);   /* a duplicate is fine */
        ztp_hdr_t a = {0};
        a.type = ZT_ACK;
        a.session_id = 7421;
        a.seq = 2;
        a.flags = ZF_ACK_VALID;
        a.ack = seq;
        n = ztp_frame_build(z, sizeof z, ZTP_MAX_DGRAM_CAP, &a, NULL, 0, key);
        write_pkt(f, link, z, (size_t)n, 5555, 40000, frag && seq == 5);
    }
    uint8_t other[16] = {0};
    write_pkt(f, link, other, sizeof other, 40000, 9999, false);   /* other port: ignored */
    if (garbage) write_pkt(f, link, other, sizeof other, 40000, 5555, false);
    fclose(f);
}

static int selftest(const char *dir)
{
    char p[512];
    const uint16_t port = 5555;
    int bad = 0;
    struct { uint32_t link; bool gap, frag, garbage; int want; const char *name; } cases[] = {
        {0, false, false, false, 0, "lo0 (DLT_NULL), clean"},
        {1, false, false, false, 0, "lo (Ethernet), clean"},
        {1, true, false, false, 1, "gap in a reliable stream"},
        {0, false, true, false, 1, "an IP fragment"},
        {1, false, false, true, 1, "a non-ZTP datagram on the port"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(p, sizeof p, "%s/selftest%zu.pcap", dir, i);
        write_capture(p, cases[i].link, cases[i].gap, cases[i].frag, cases[i].garbage);
        int rc = check(p, &port, 1, false, 0, true);
        printf("pcapcheck selftest: %-34s -> %s\n", cases[i].name, rc == cases[i].want ? "ok" : "WRONG");
        bad += rc != cases[i].want;
    }
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    bool dump = false;
    size_t max_size = 0;
    int i = 1;
    if (argc == 3 && !strcmp(argv[1], "--selftest")) return selftest(argv[2]);
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--dump-hdr")) dump = true;
        else if (!strcmp(argv[i], "--max-size") && i + 1 < argc) max_size = (size_t)atol(argv[++i]);
        else { fprintf(stderr, "pcapcheck: unknown option %s\n", argv[i]); return 2; }
    }
    if (argc - i < 2) {
        fprintf(stderr, "usage: pcapcheck [--dump-hdr] [--max-size N] FILE.pcap PORT [PORT...] | --selftest DIR\n");
        return 2;
    }
    const char *path = argv[i++];
    uint16_t ports[16];
    int n = 0;
    for (; i < argc && n < 16; i++) ports[n++] = (uint16_t)atoi(argv[i]);
    return check(path, ports, n, dump, max_size, false);
}
