/* Server session table (DESIGN §14.2): open addressing keyed on session_id, 2048 slots. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "logmsg.h"
#include "rand.h"
#include "srv.h"
#include "timer.h"

static srv_session_t deleted_marker;
#define SRV_DELETED (&deleted_marker)

static size_t slot_of(uint32_t sid) { return (sid * 2654435761u) % SRV_TABLE_SLOTS; }

uint32_t srv_serial_hash(const char *serial)   /* FNV-1a */
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)serial; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

/* Find without marking the session for rescheduling (the timer heap's own lookups). */
static srv_session_t *lookup(srv_t *s, uint32_t sid, size_t *slot)
{
    if (!sid) return NULL;
    for (size_t i = 0, k = slot_of(sid); i < SRV_TABLE_SLOTS; i++, k = (k + 1) % SRV_TABLE_SLOTS) {
        srv_session_t *e = s->table[k];
        if (!e) return NULL;
        if (e != SRV_DELETED && e->session_id == sid) {
            if (slot) *slot = k;
            return e;
        }
    }
    return NULL;
}

void srv_sess_touch(srv_t *s, srv_session_t *ss)
{
    if (ss->dirty || s->ndirty >= SRV_TABLE_SLOTS) return;
    ss->dirty = true;
    s->dirty[s->ndirty++] = ss->session_id;
}

/* Every handler reaches its session through a lookup, so a found session is rescheduled after the event. */
srv_session_t *srv_sess_find(srv_t *s, uint32_t sid)
{
    srv_session_t *e = lookup(s, sid, NULL);
    if (e) srv_sess_touch(s, e);
    return e;
}

srv_session_t *srv_sess_find_half_open(srv_t *s, const char *serial)
{
    for (srv_session_t *e = s->ser_bucket[srv_serial_hash(serial) % SRV_TABLE_SLOTS]; e; e = e->ser_next)
        if (e->st == SS_HALF_OPEN && !strcmp(e->serial, serial)) {
            srv_sess_touch(s, e);
            return e;
        }
    return NULL;
}

srv_session_t *srv_sess_find_registered(srv_t *s, const char *serial)
{
    for (srv_session_t *e = s->ser_bucket[srv_serial_hash(serial) % SRV_TABLE_SLOTS]; e; e = e->ser_next)
        if (e->st != SS_HALF_OPEN && !e->doomed && !strcmp(e->serial, serial)) {
            srv_sess_touch(s, e);
            return e;
        }
    return NULL;
}

/* Too many deleted markers make lookups of unknown ids walk the whole table: rebuild it. */
static void table_rebuild(srv_t *s)
{
    srv_session_t *live[SRV_TABLE_SLOTS];
    int n = 0;
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++)
        if (s->table[k] && s->table[k] != SRV_DELETED) live[n++] = s->table[k];
    memset(s->table, 0, sizeof s->table);
    for (int i = 0; i < n; i++) {
        size_t k = slot_of(live[i]->session_id);
        while (s->table[k]) k = (k + 1) % SRV_TABLE_SLOTS;
        s->table[k] = live[i];
    }
    s->ndeleted = 0;
}

const char *srv_state_name(srv_sess_state_t st)
{
    static const char *const N[] = {"HALF_OPEN", "REGISTERED", "PROVISIONING", "AWAIT_RESUME", "IDLE_GRACE",
                                    "OPERATIONAL", "FAILED"};
    return (unsigned)st < sizeof N / sizeof N[0] ? N[st] : "?";
}

void srv_sess_set_state(srv_t *s, srv_session_t *ss, srv_sess_state_t st)
{
    srv_sess_touch(s, ss);
    if (ss->st == SS_HALF_OPEN && st != SS_HALF_OPEN) s->nhalf_open--;
    if (ss->st != SS_HALF_OPEN && st == SS_HALF_OPEN) s->nhalf_open++;
    ss->st = st;
}

/* Journal a resumable session record (no keys, §14.4). */
void srv_sess_journal(srv_t *s, srv_session_t *ss)
{
    db_sess_t r = {0};
    r.sid = ss->session_id;
    r.epoch = ss->epoch;
    r.last_resume_ctr = ss->last_resume_ctr;
    r.max_dgram = ss->max_dgram;
    r.keepalive_s = ss->assign.nat.keepalive_interval_s;
    r.grace_s = ss->assign.nat.grace_s;
    snprintf(r.serial, sizeof r.serial, "%s", ss->serial);
    snprintf(r.model, sizeof r.model, "%s", ss->model);
    snprintf(r.state, sizeof r.state, "%s", srv_state_name(ss->st));
    addr_fmt(&ss->peer, r.peer);
    r.nd_resume = ss->nd_resume;
    r.ns_resume = ss->ns_resume;
    db_session(&s->db, &r, wall_ms());
}

/* Reliability engine callbacks: frames go to the session peer, or to the source of the
 * datagram being processed (re-ACK of a duplicate). */
static int sess_tx(void *ctx, const uint8_t *frame, size_t len, bool to_source)
{
    srv_session_t *ss = ctx;
    srv_t *s = ss->srv;
    if (frame[3] == ZT_HELLO_ACK && !(frame[4] & ZF_RETX) && ss->cached_hello_ack_len == 0) {
        memcpy(ss->cached_hello_ack, frame, len);
        ss->cached_hello_ack_len = (uint16_t)len;
    }
    return srv_send(s, s->ctl_fd, frame, len, to_source ? &s->cur_src : &ss->peer);
}

srv_session_t *srv_sess_new(srv_t *s, const char *serial, const char *model)
{
    if (s->nsessions >= s->cfg.c.limits.max_sessions || s->nhalf_open >= s->cfg.c.limits.max_half_open) return NULL;
    /* the open-addressing table must keep free slots, or the insert probe below never ends (config caps
     * limits.max_sessions at 3/4 of the table; this guards the invariant on its own) */
    if (s->nsessions >= SRV_TABLE_SLOTS * 3 / 4) return NULL;
    if (s->nsessions + s->ndeleted >= SRV_TABLE_SLOTS * 3 / 4) table_rebuild(s);
    srv_session_t *ss = calloc(1, sizeof *ss);
    if (!ss) return NULL;
    ss->srv = s;
    uint32_t sid;
    if (s->hook_first_sid && !lookup(s, s->hook_first_sid, NULL)) {
        sid = s->hook_first_sid;
        s->hook_first_sid = 0;
    } else {
        do sid = rand_u32_nonzero(); while (lookup(s, sid, NULL));   /* §7.2: redraw on collision */
    }
    ss->session_id = sid;
    ss->st = SS_HALF_OPEN;
    snprintf(ss->serial, sizeof ss->serial, "%s", serial);
    snprintf(ss->model, sizeof ss->model, "%s", model);
    ss->rel = rel_new(&s->relcfg, sess_tx, srv_sess_deliver, ss);
    if (!ss->rel) {
        free(ss);
        return NULL;
    }
    rel_set_session(ss->rel, sid, 0);
    rel_set_ack_key(ss->rel, s->keys.k_boot, ZK_BOOT);
    rel_set_log_tag(ss->rel, sid, serial);
    ss->max_dgram = (uint16_t)s->cfg.c.net.max_datagram;
    ss->created_ms = ss->last_rx_ms = s->now;
    ss->half_open_deadline = s->now + (uint64_t)s->cfg.c.nat.half_open_ttl_s * 1000u;

    size_t k = slot_of(sid);
    while (s->table[k] && s->table[k] != SRV_DELETED) k = (k + 1) % SRV_TABLE_SLOTS;
    if (s->table[k] == SRV_DELETED) s->ndeleted--;
    s->table[k] = ss;
    size_t b = srv_serial_hash(ss->serial) % SRV_TABLE_SLOTS;
    ss->ser_next = s->ser_bucket[b];
    s->ser_bucket[b] = ss;
    s->nsessions++;
    srv_sess_touch(s, ss);
    s->nhalf_open++;
    if ((uint64_t)s->nsessions > s->c.sessions_peak) s->c.sessions_peak = (uint64_t)s->nsessions;
    return ss;
}

static void add_stats(rel_stats_t *a, const rel_stats_t *b)
{
    a->tx += b->tx; a->rx += b->rx; a->retx_timeout += b->retx_timeout; a->retx_fast += b->retx_fast;
    a->dup += b->dup; a->ooo += b->ooo; a->out_of_window += b->out_of_window; a->acks_sent += b->acks_sent;
    a->frames += b->frames;
    if (b->srtt_ms) a->srtt_ms = b->srtt_ms;
}

void srv_sess_free(srv_t *s, srv_session_t *ss, const char *why)
{
    if (ss->st != SS_HALF_OPEN && why) db_session_deleted(&s->db, ss->session_id, ss->serial, wall_ms());
    size_t k;
    if (lookup(s, ss->session_id, &k) == ss) {
        s->table[k] = SRV_DELETED;
        s->ndeleted++;
    }
    for (srv_session_t **pp = &s->ser_bucket[srv_serial_hash(ss->serial) % SRV_TABLE_SLOTS]; *pp; pp = &(*pp)->ser_next)
        if (*pp == ss) {
            *pp = ss->ser_next;
            break;
        }
    s->nsessions--;
    if (ss->st == SS_HALF_OPEN) s->nhalf_open--;
    if (why) log_sess(LOG_DEBUG, T_UDP, ss->session_id, ss->serial, "Session removed (%s)", why);
    add_stats(&s->c.rel, rel_stats(ss->rel));
    rel_free(ss->rel);
    secure_zero(ss, sizeof *ss);
    free(ss);
}

/* After a restart, every journaled live session comes back in IDLE_GRACE with a new grace timer,
 * so a device mid-provisioning resumes instead of registering again (§14.4; resume itself is M5). */
void srv_sess_restore(srv_t *s)
{
    size_t n;
    const db_sess_t *js = db_sessions(&s->db, &n);
    int restored = 0;
    for (size_t i = 0; i < n; i++) {
        if (js[i].deleted || srv_sess_find(s, js[i].sid) || s->nsessions >= s->cfg.c.limits.max_sessions) continue;
        uint32_t keep = s->hook_first_sid;
        s->hook_first_sid = js[i].sid;    /* reuse the allocation path with the journaled id */
        srv_session_t *ss = srv_sess_new(s, js[i].serial, js[i].model);
        s->hook_first_sid = keep;
        if (!ss) continue;
        ss->epoch = js[i].epoch;
        if (js[i].max_dgram) ss->max_dgram = js[i].max_dgram;
        ss->last_resume_ctr = js[i].last_resume_ctr;
        ss->nd_resume = js[i].nd_resume;
        ss->ns_resume = js[i].ns_resume;
        db_rec_t *rec = db_get(&s->db, ss->serial);
        ss->db_stage = rec ? (uint8_t)rec->stage : SST_REGISTERED;
        srv_policy_resolve(&s->policy, &s->cfg, ss->model, ss->serial, &ss->assign);
        rel_set_session(ss->rel, ss->session_id, ss->epoch);
        hostport_parse(js[i].peer, &ss->peer);
        srv_sess_set_state(s, ss, SS_IDLE_GRACE);
        ss->grace_deadline = s->now + (uint64_t)ss->assign.nat.grace_s * 1000u;
        restored++;
    }
    if (restored) LOGI(T_DB, "Restored %d journaled session(s) into IDLE_GRACE", restored);
}

void srv_sess_free_all(srv_t *s)
{
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++)
        if (s->table[k] && s->table[k] != SRV_DELETED) srv_sess_free(s, s->table[k], NULL);
    memset(s->table, 0, sizeof s->table);
    memset(s->ser_bucket, 0, sizeof s->ser_bucket);
    s->ndeleted = 0;
    s->ndirty = 0;
    s->hlen = 0;
}

void srv_timers_free(srv_t *s)
{
    free(s->heap);
    s->heap = NULL;
    s->hlen = s->hcap = 0;
}

/* §14.2 timer heap. A session's next deadline (the earliest of its engine, half-open TTL, grace and idle
 * timers) is pushed after every event that touched it; an entry whose generation is older, or whose
 * session is gone, is stale and skipped when it surfaces. */
static uint64_t sess_deadline(const srv_session_t *e)
{
    if (e->doomed) return 0;
    bool waiting = e->st == SS_IDLE_GRACE || e->st == SS_AWAIT_RESUME;
    if (rel_failed(e->rel) && !waiting) return 0;   /* retries exhausted: handled at once */
    uint64_t d = rel_next_deadline(e->rel);
    if (e->st == SS_HALF_OPEN && e->half_open_deadline < d) d = e->half_open_deadline;
    if (waiting && e->grace_deadline < d) d = e->grace_deadline;
    if (e->st != SS_HALF_OPEN && !waiting) {
        uint64_t idle = e->last_rx_ms + (uint64_t)e->assign.nat.server_idle_intervals *
                                           (uint64_t)e->assign.nat.keepalive_interval_s * 1000u;
        if (idle < d) d = idle;
    }
    return d;
}

static void heap_swap(srv_timer_t *h, size_t i, size_t j) { srv_timer_t x = h[i]; h[i] = h[j]; h[j] = x; }

static void heap_push(srv_t *s, srv_timer_t t)
{
    if (s->hlen == s->hcap) {
        size_t cap = s->hcap ? s->hcap * 2 : 256;
        srv_timer_t *h = realloc(s->heap, cap * sizeof *h);
        if (!h) return;   /* the session is still rescheduled on its next event */
        s->heap = h;
        s->hcap = cap;
    }
    size_t i = s->hlen++;
    s->heap[i] = t;
    while (i && s->heap[(i - 1) / 2].deadline > s->heap[i].deadline) {
        heap_swap(s->heap, i, (i - 1) / 2);
        i = (i - 1) / 2;
    }
}

static void heap_pop(srv_t *s)
{
    s->heap[0] = s->heap[--s->hlen];
    for (size_t i = 0;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < s->hlen && s->heap[l].deadline < s->heap[m].deadline) m = l;
        if (r < s->hlen && s->heap[r].deadline < s->heap[m].deadline) m = r;
        if (m == i) break;
        heap_swap(s->heap, i, m);
        i = m;
    }
}

static void reschedule(srv_t *s, srv_session_t *e)
{
    e->dirty = false;
    e->tgen++;
    uint64_t d = sess_deadline(e);
    if (d != UINT64_MAX) heap_push(s, (srv_timer_t){d, e->session_id, e->tgen});
}

/* Push the touched sessions; when stale entries dominate, rebuild the heap from the live sessions. */
static void flush_dirty(srv_t *s)
{
    for (int i = 0; i < s->ndirty; i++) {
        srv_session_t *e = lookup(s, s->dirty[i], NULL);
        if (e && e->dirty) reschedule(s, e);
    }
    s->ndirty = 0;
    if (s->hlen > 64 && s->hlen > 4 * (size_t)s->nsessions) {
        s->hlen = 0;
        for (size_t k = 0; k < SRV_TABLE_SLOTS; k++)
            if (s->table[k] && s->table[k] != SRV_DELETED) reschedule(s, s->table[k]);
    }
}

static bool stale(srv_t *s, const srv_timer_t *t, srv_session_t **out)
{
    srv_session_t *e = lookup(s, t->sid, NULL);
    *out = e;
    return !e || e->tgen != t->gen;
}

uint64_t srv_sess_next_deadline(srv_t *s)
{
    flush_dirty(s);
    srv_session_t *e;
    while (s->hlen && stale(s, &s->heap[0], &e)) heap_pop(s);
    return s->hlen ? s->heap[0].deadline : UINT64_MAX;
}

/* One session's timers (the per-session part of the §9.3 / §11 / §14.4 timer rules). */
static void sess_timer(srv_t *s, srv_session_t *e)
{
    if (e->doomed) {
        srv_sess_free(s, e, NULL);
        return;
    }
    if (e->st == SS_HALF_OPEN && s->now >= e->half_open_deadline) {
        srv_sess_free(s, e, "half-open TTL expired");
        return;
    }
    if (e->st == SS_IDLE_GRACE || e->st == SS_AWAIT_RESUME) {
        if (s->now >= e->grace_deadline) {
            log_sess(LOG_INFO, T_NAT, e->session_id, e->serial, "Grace period over (%d s): session deleted, DB record kept",
                     e->assign.nat.grace_s);
            srv_sess_free(s, e, "grace expired");
            return;
        }
        /* AWAIT_RESUME still ACKs and retransmits until the device restarts; IDLE_GRACE has no TX state */
        if (e->st == SS_AWAIT_RESUME) rel_on_timer(e->rel, s->now);
        return;
    }
    bool failed = rel_on_timer(e->rel, s->now) == REL_E_FAILED;
    if (!failed) srv_sess_pump(e);
    if (e->st == SS_HALF_OPEN) {
        if (failed) srv_sess_free(s, e, "retries exhausted");
        return;
    }
    /* registered: idle for server_idle_intervals x keepalive, or retries exhausted -> IDLE_GRACE (§9.3) */
    uint64_t idle_ms = (uint64_t)e->assign.nat.server_idle_intervals * (uint64_t)e->assign.nat.keepalive_interval_s * 1000u;
    if (failed || s->now - e->last_rx_ms >= idle_ms) {
        log_sess(LOG_INFO, T_NAT, e->session_id, e->serial, LM_NAT_IDLE_GRACE);
        rel_reset(e->rel, e->epoch);   /* reliable TX state is discarded */
        srv_sess_set_state(s, e, SS_IDLE_GRACE);
        e->grace_deadline = s->now + (uint64_t)e->assign.nat.grace_s * 1000u;
        srv_sess_journal(s, e);
    }
}

void srv_sess_run_timers(srv_t *s)
{
    flush_dirty(s);
    while (s->hlen && s->heap[0].deadline <= s->now) {
        srv_timer_t t = s->heap[0];
        heap_pop(s);
        srv_session_t *e;
        if (stale(s, &t, &e)) continue;
        e->tgen++;   /* consumed: sess_timer's effects reschedule it below */
        srv_sess_touch(s, e);
        sess_timer(s, e);
    }
    flush_dirty(s);
}
