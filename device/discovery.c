/* Device discovery (DESIGN §6.2, §6.3): multicast or broadcast DISCOVER, authenticated OFFER,
 * fallback to the static address after discovery.attempts. */
#include <arpa/inet.h>
#include <string.h>

#include "dev.h"
#include "log.h"
#include "logmsg.h"
#include "rand.h"

static void send_discover(device_t *d)
{
    d->disc_attempt++;
    uint8_t pl[64];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_str(&w, TAG_MODEL, d->cfg.c.identity.model);
    tlv_put_str(&w, TAG_SERIAL, d->st.serial);
    tlv_put_u64(&w, TAG_DISC_NONCE, d->disc_nonce);
    ztp_hdr_t h = {0};
    h.type = ZT_DISCOVER;
    h.seq = (uint32_t)d->disc_attempt;   /* datagram class: the attempt number */
    h.key_id = ZK_BOOT;
    uint8_t frame[256];
    int n = ztp_frame_build(frame, sizeof frame, ZTP_MAX_DGRAM_CAP, &h, pl, (uint16_t)w.len, d->k_boot);
    char a[ADDR_STR];
    addr_fmt(&d->disc_dst, a);
    if (n > 0 && dev_send(d, frame, (size_t)n, &d->disc_dst) == 0)
        LOGI(T_DISCOVERY, "DISCOVER sent to %s (%s, attempt %d/%d)", a, d->cfg.c.discovery.mode, d->disc_attempt,
             d->cfg.c.discovery.attempts);
    else
        LOGI(T_DISCOVERY, "DISCOVER to %s failed (attempt %d/%d)", a, d->disc_attempt, d->cfg.c.discovery.attempts);
    d->disc_deadline = d->now + (uint64_t)d->cfg.c.discovery.interval_ms;
}

void dev_discovery_start(device_t *d)
{
    const ztp_config_t *c = &d->cfg.c;
    if (!strcmp(c->discovery.mode, "static")) {
        dev_use_static(d);
        return;
    }
    d->fsm = DS_DISCOVERY;
    d->disc_attempt = 0;
    d->disc_nonce = rand_u64();
    const char *ip = !strcmp(c->discovery.mode, "mcast") ? c->discovery.mcast_group : c->discovery.bcast_addr;
    addr_parse(ip, (uint16_t)c->discovery.discovery_port, &d->disc_dst);
    send_discover(d);
}

void dev_discovery_timer(device_t *d)
{
    if (d->now < d->disc_deadline) return;
    if (d->disc_attempt < d->cfg.c.discovery.attempts) {
        send_discover(d);
        return;
    }
    LOGI(T_DISCOVERY, "No valid OFFER after %d DISCOVER attempts: falling back to the static address",
         d->disc_attempt);
    dev_use_static(d);
}

void dev_use_static(device_t *d)
{
    if (!hostport_parse(d->cfg.c.discovery.server, &d->server)) {
        dev_fail(d, "invalid discovery.server");
        return;
    }
    addr_fmt(&d->server, d->server_str);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &d->server.sin_addr, ip, sizeof ip);
    LOGI(T_DISCOVERY, LM_DISC_STATIC, ip, (unsigned)ntohs(d->server.sin_port));
    LOGI(T_DISCOVERY, LM_DISC_SERVER_FOUND " %s (static)", d->server_str);
    dev_session_start(d);
}

void dev_on_offer(device_t *d, const uint8_t *buf, size_t len, const ztp_hdr_t *h)
{
    if (d->fsm != DS_DISCOVERY) return;   /* late or extra OFFERs: the first valid one won */
    if (!ztp_mac_verify(buf, len, d->k_boot)) {
        d->c.mac_fail++;
        log_limited(20, LOG_INFO, T_DISCOVERY, LM_DISC_OFFER_BAD_MAC);
        return;
    }
    tlv_set_t t;
    uint64_t dn;
    const uint8_t *ip;
    uint16_t iplen, port;
    if (tlv_parse(buf + ZTP_HDR_LEN, h->payload_len, &t) != ZTP_OK || !tlv_get_u64(&t, TAG_DISC_NONCE, &dn) ||
        !tlv_get_bytes(&t, TAG_SERVER_ADDR, &ip, &iplen) || !tlv_get_u16(&t, TAG_SERVER_PORT, &port) || port == 0) {
        d->c.rx_malformed++;
        return;
    }
    if (dn != d->disc_nonce) {
        log_limited(21, LOG_INFO, T_DISCOVERY, LM_DISC_OFFER_BAD_NONCE);
        return;
    }
    memset(&d->server, 0, sizeof d->server);
    d->server.sin_family = AF_INET;
    memcpy(&d->server.sin_addr.s_addr, ip, 4);
    d->server.sin_port = htons(port);
    addr_fmt(&d->server, d->server_str);
    LOGI(T_DISCOVERY, LM_DISC_SERVER_FOUND " %s (OFFER verified)", d->server_str);
    dev_session_start(d);
}
