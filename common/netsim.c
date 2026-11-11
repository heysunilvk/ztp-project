#include "netsim.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "logmsg.h"
#include "timer.h"

int netsim_init(netsim_t *ns, const netsim_cfg_t *cfg)
{
    memset(ns, 0, sizeof *ns);
    ns->cfg = *cfg;
    if (ns->cfg.reorder_delay_ms == 0) ns->cfg.reorder_delay_ms = 30;
    ns->active = cfg->loss > 0 || cfg->reorder > 0 || cfg->dup > 0 || cfg->delay_ms > 0 ||
                 cfg->jitter_ms > 0 || cfg->mtu > 0;
    uint64_t seed = cfg->seed ? cfg->seed : (now_ms() ^ ((uint64_t)wall_ms() << 20));
    /* splitmix64 the seed so small seeds (1, 2, 3...) give unrelated streams */
    seed += 0x9E3779B97F4A7C15ull;
    seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
    seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
    seed ^= seed >> 31;
    ns->rng = seed ? seed : 0x2545F4914F6CDD1Dull;
    if (ns->active) {
        ns->q = calloc(NETSIM_QUEUE_MAX, sizeof *ns->q);
        if (!ns->q) return -1;
    }
    return 0;
}

void netsim_free(netsim_t *ns)
{
    free(ns->q);
    ns->q = NULL;
    ns->qlen = 0;
}

bool netsim_is_active(const netsim_t *ns) { return ns->active; }

uint64_t netsim_rand(netsim_t *ns)
{
    uint64_t x = ns->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    ns->rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

double netsim_rand01(netsim_t *ns)
{
    return (double)(netsim_rand(ns) >> 11) * (1.0 / 9007199254740992.0);   /* [0,1) */
}

netsim_decision_t netsim_decide(netsim_t *ns, size_t len)
{
    netsim_decision_t d = {0};
    const netsim_cfg_t *c = &ns->cfg;
    if (c->mtu && len > c->mtu) {
        if (c->mtu_mode == NS_MTU_BLACKHOLE) d.drop = true;
        else d.emsgsize = true;
        return d;
    }
    /* Draw every random value in a fixed order so a given seed yields one decision sequence
     * regardless of which knobs are enabled. */
    double r_loss = netsim_rand01(ns), r_reo = netsim_rand01(ns), r_dup = netsim_rand01(ns);
    uint64_t r_jit = netsim_rand(ns), r_dupd = netsim_rand(ns);

    if (r_loss < c->loss) {
        d.drop = true;
        return d;
    }
    uint32_t delay = c->delay_ms;
    if (c->jitter_ms) {
        int64_t j = (int64_t)(r_jit % (2u * c->jitter_ms + 1u)) - (int64_t)c->jitter_ms;
        delay = (int64_t)delay + j < 0 ? 0 : (uint32_t)((int64_t)delay + j);
    }
    if (r_reo < c->reorder) {
        d.reordered = true;
        delay += c->reorder_delay_ms;
    }
    d.delay_ms = delay;
    if (r_dup < c->dup) {
        d.dup = true;
        d.dup_delay_ms = delay + (uint32_t)(r_dupd % 11u);   /* 0–10 ms after the original */
    }
    return d;
}

static int hold(netsim_t *ns, int fd, const void *buf, size_t len, const struct sockaddr *to,
                socklen_t tolen, uint64_t due)
{
    if (ns->qlen >= NETSIM_QUEUE_MAX || len > ZTP_MAX_DGRAM_CAP || tolen > sizeof(struct sockaddr_storage))
        return -1;
    netsim_held_t *h = &ns->q[ns->qlen++];
    h->due_ms = due;
    h->fd = fd;
    h->alen = tolen;
    memset(&h->addr, 0, sizeof h->addr);
    memcpy(&h->addr, to, tolen);
    h->len = (uint16_t)len;
    memcpy(h->bytes, buf, len);
    return 0;
}

ssize_t netsim_sendto(netsim_t *ns, int fd, const void *buf, size_t len, int flags,
                      const struct sockaddr *to, socklen_t tolen, uint64_t now)
{
    if (!ns || !ns->active) return sendto(fd, buf, len, flags, to, tolen);

    ns->st.tx++;
    netsim_decision_t d = netsim_decide(ns, len);
    if (d.emsgsize) {
        ns->st.emsgsize++;
        LOGD(T_SIM, "tx len=%zu > mtu=%u: EMSGSIZE", len, ns->cfg.mtu);
        errno = EMSGSIZE;
        return -1;
    }
    if (d.drop) {
        if (ns->cfg.mtu && len > ns->cfg.mtu) {
            ns->st.mtu_dropped++;
            LOGD(T_SIM, "tx len=%zu > mtu=%u: black-holed", len, ns->cfg.mtu);
        } else {
            ns->st.dropped++;
            LOGD(T_SIM, "tx len=%zu dropped", len);
        }
        return (ssize_t)len;
    }
    if (d.reordered) {
        ns->st.reordered++;
        LOGD(T_SIM, "tx len=%zu held %u ms (reorder)", len, d.delay_ms);
    }
    if (d.dup) {
        ns->st.duplicated++;
        LOGD(T_SIM, "tx len=%zu duplicated (+%u ms)", len, d.dup_delay_ms);
        if (hold(ns, fd, buf, len, to, tolen, now + d.dup_delay_ms) != 0)
            (void)sendto(fd, buf, len, flags, to, tolen);
    }
    if (d.delay_ms == 0 || hold(ns, fd, buf, len, to, tolen, now + d.delay_ms) != 0) {
        ssize_t r = sendto(fd, buf, len, flags, to, tolen);
        return r;
    }
    return (ssize_t)len;
}

ssize_t netsim_recvfrom(netsim_t *ns, int fd, void *buf, size_t cap, int flags,
                        struct sockaddr *from, socklen_t *fromlen)
{
    ssize_t r = recvfrom(fd, buf, cap, flags, from, fromlen);
    if (ns && r >= 0) ns->st.rx++;
    return r;
}

size_t netsim_flush(netsim_t *ns, uint64_t now)
{
    size_t sent = 0;
    if (!ns || !ns->q) return 0;
    /* Release in due-time order: repeatedly pick the earliest due entry. Queue is small. */
    for (;;) {
        size_t best = ns->qlen;
        for (size_t i = 0; i < ns->qlen; i++)
            if (ns->q[i].due_ms <= now && (best == ns->qlen || ns->q[i].due_ms < ns->q[best].due_ms))
                best = i;
        if (best == ns->qlen) break;
        netsim_held_t *h = &ns->q[best];
        (void)sendto(h->fd, h->bytes, h->len, 0, (struct sockaddr *)&h->addr, h->alen);
        sent++;
        ns->q[best] = ns->q[--ns->qlen];
    }
    return sent;
}

uint64_t netsim_next_deadline(const netsim_t *ns)
{
    uint64_t d = UINT64_MAX;
    if (!ns) return d;
    for (size_t i = 0; i < ns->qlen; i++)
        if (ns->q[i].due_ms < d) d = ns->q[i].due_ms;
    return d;
}

void netsim_log_totals(const netsim_t *ns)
{
    if (!ns || !ns->active) return;
    uint64_t lost = ns->st.dropped + ns->st.mtu_dropped;
    double pct = ns->st.tx ? 100.0 * (double)lost / (double)ns->st.tx : 0.0;
    LOGI(T_SIM, LM_SIM_TOTALS, (unsigned long long)ns->st.tx, (unsigned long long)lost, pct,
         (unsigned long long)ns->st.reordered, (unsigned long long)ns->st.duplicated, (unsigned long long)ns->st.rx);
}
