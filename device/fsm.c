/* Device event loop and FSM glue (DESIGN §15.1, §15.5). */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dev.h"
#include "log.h"
#include "logmsg.h"
#include "rand.h"
#include "timer.h"

int dev_send(device_t *d, const uint8_t *frame, size_t len, const struct sockaddr_in *dst)
{
    int act = hooks_tx_action(&d->hooks, frame, len);
    d->last_tx = d->now;
    if (dev_nat_before_send(d, dst)) {
        LOGD(T_SIM, "NAT: datagram to port %u blocked", (unsigned)ntohs(dst->sin_port));
        return 0;
    }
    if (act == HOOK_DROP) {
        LOGD(T_UDP, "test hook: dropped %s seq=%u", ztp_type_name(frame[3]), (unsigned)be32_get(frame + 12));
        return 0;
    }
    for (int i = 0; i < (act == HOOK_SEND_TWICE ? 2 : 1); i++)
        if (netsim_sendto(&d->ns, d->fd, frame, len, 0, (const struct sockaddr *)dst, sizeof *dst, d->now) < 0) {
            int e = errno;
            log_limited(30, LOG_INFO, T_UDP, "sendto: %s", strerror(e));
            return -e;
        }
    return 0;
}

/* Reconnect with exponential backoff and full jitter (§9.3): wait uniform(0, cap), where
 * cap = min(reconnect_max_s, reconnect_min_s * 2^n), with a 50 ms floor. */
void dev_backoff(device_t *d, const char *why)
{
    dev_session_end(d);
    double cap = d->cfg.c.net.reconnect_min_s;
    for (int i = 0; i < d->backoff_n && cap < d->cfg.c.net.reconnect_max_s; i++) cap *= 2;
    if (cap > d->cfg.c.net.reconnect_max_s) cap = d->cfg.c.net.reconnect_max_s;
    d->backoff_n++;
    uint64_t cap_ms = (uint64_t)(cap * 1000.0);
    uint64_t wait = cap_ms ? rand_u64() % (cap_ms + 1) : 0;
    if (wait < 50) wait = 50;
    d->backoff_until = d->now + wait;
    d->fsm = DS_BACKOFF;
    LOGI(T_UDP, "Reconnecting in %.1f s (%s)", (double)wait / 1000.0, why);
}

void dev_fail(device_t *d, const char *reason)
{
    if (d->in_rx) {   /* called from a deliver callback: finish after rel_on_rx returns (no use-after-free) */
        if (reason != d->pending_reason) snprintf(d->pending_reason, sizeof d->pending_reason, "%s", reason);
        d->fail_pending = true;
        return;
    }
    dev_session_end(d);
    d->st.stage = ST_FAILED;
    snprintf(d->st.last_fail_reason, sizeof d->st.last_fail_reason, "%s", reason);
    if (dev_state_save(d->state_path, &d->st) != 0) LOGW(T_ERROR, "cannot persist state: %s", strerror(errno));
    LOGI(T_STATE, LM_STATE_FAILED, reason);
    d->fsm = DS_FAILED;
    d->exit_code = 1;
}

static int g_sigpipe[2] = {-1, -1};

static void on_signal(int sig)
{
    int e = errno;
    unsigned char b = (unsigned char)sig;
    if (write(g_sigpipe[1], &b, 1) < 0) { /* a signal is already pending */ }
    errno = e;
}

static void log_counters(device_t *d)
{
    rel_stats_t z = {0};
    const rel_stats_t *r = d->rel ? rel_stats(d->rel) : &z;
    LOGI(T_UDP,
         "Counters: tx=%llu rx=%llu retx_timeout=%llu retx_fast=%llu dup=%llu ooo=%llu out_of_window=%llu "
         "rx_malformed=%llu rx_unknown=%llu rx_stale=%llu mac_fail=%llu timeouts=%llu srtt_ms=%u",
         (unsigned long long)r->tx, (unsigned long long)r->rx, (unsigned long long)r->retx_timeout,
         (unsigned long long)r->retx_fast, (unsigned long long)r->dup, (unsigned long long)r->ooo,
         (unsigned long long)r->out_of_window, (unsigned long long)d->c.rx_malformed,
         (unsigned long long)d->c.rx_unknown, (unsigned long long)d->c.rx_stale, (unsigned long long)d->c.mac_fail,
         (unsigned long long)d->c.timeouts, (unsigned)r->srtt_ms);
}

static void on_datagram(device_t *d, const uint8_t *buf, size_t len, const struct sockaddr_in *src)
{
    if (!dev_nat_accept_rx(d)) {
        LOGD(T_SIM, "NAT: inbound datagram dropped (no mapping)");
        return;
    }
    ztp_hdr_t h;
    if (ztp_hdr_decode(buf, len, d->st.max_dgram ? d->st.max_dgram : ZTP_MAX_DGRAM_CAP, ZD_S2D, &h) != ZTP_OK) {
        d->c.rx_malformed++;
        return;
    }
    LOGD(T_UDP, "rx %s sid=%u seq=%u ack=%u len=%zu", ztp_type_name(h.type), (unsigned)h.session_id, (unsigned)h.seq,
         (unsigned)h.ack, len);
    if (h.type == ZT_OFFER) dev_on_offer(d, buf, len, &h);
    else dev_session_rx(d, buf, len, &h, src);
}

int dev_run(device_t *d)
{
    if (pipe(g_sigpipe) == 0) {
        fcntl(g_sigpipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_sigpipe[1], F_SETFL, O_NONBLOCK);
    }
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    d->now = now_ms();
    dev_discovery_start(d);
    while (d->fsm != DS_EXIT && d->fsm != DS_FAILED) {
        d->now = now_ms();
        uint64_t t = UINT64_MAX;
        if (d->fsm == DS_DISCOVERY) t = d->disc_deadline;
        else if (d->fsm == DS_BACKOFF) t = d->backoff_until;
        else t = dev_session_deadline(d);
        uint64_t nd = netsim_next_deadline(&d->ns);
        if (nd < t) t = nd;
        if ((nd = dev_nat_deadline(d)) < t) t = nd;
        int timeout = t == UINT64_MAX ? 1000 : (t <= d->now ? 0 : (int)(t - d->now > 1000 ? 1000 : t - d->now));
        struct pollfd p[2] = {{d->fd, POLLIN, 0}, {g_sigpipe[0], POLLIN, 0}};
        if (poll(p, 2, timeout) < 0 && errno != EINTR) break;
        d->now = now_ms();
        if (p[1].revents & POLLIN) {
            unsigned char b;
            bool stop = false;
            while (read(g_sigpipe[0], &b, 1) == 1) stop |= (b == SIGINT || b == SIGTERM);
            if (stop) {
                LOGI(T_BOOT, "Stopping on signal");
                d->exit_code = 0;
                break;
            }
        }
        if (p[0].revents & POLLIN)
            for (int i = 0; i < 64; i++) {
                uint8_t buf[2048];
                struct sockaddr_in src;
                socklen_t sl = sizeof src;
                ssize_t n = netsim_recvfrom(&d->ns, d->fd, buf, sizeof buf, 0, (struct sockaddr *)&src, &sl);
                if (n < 0) break;
                d->now = now_ms();
                on_datagram(d, buf, (size_t)n, &src);
            }
        d->now = now_ms();
        dev_nat_timer(d);
        if (d->fsm == DS_DISCOVERY) dev_discovery_timer(d);
        else if (d->fsm == DS_BACKOFF) { if (d->now >= d->backoff_until) dev_discovery_start(d); }
        else if (d->fsm != DS_EXIT && d->fsm != DS_FAILED) dev_session_timer(d);
        netsim_flush(&d->ns, d->now);
    }
    log_counters(d);
    netsim_log_totals(&d->ns);
    if (dev_nat_active(d))
        LOGI(T_SIM, "NAT: blocked=%llu inbound_dropped=%llu", (unsigned long long)d->nat.blocked,
             (unsigned long long)d->nat.rx_dropped);
    dev_session_end(d);
    return d->exit_code;
}
