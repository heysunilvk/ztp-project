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

srv_session_t *srv_sess_find(srv_t *s, uint32_t sid)
{
    if (!sid) return NULL;
    for (size_t i = 0, k = slot_of(sid); i < SRV_TABLE_SLOTS; i++, k = (k + 1) % SRV_TABLE_SLOTS) {
        srv_session_t *e = s->table[k];
        if (!e) return NULL;
        if (e != SRV_DELETED && e->session_id == sid) return e;
    }
    return NULL;
}

srv_session_t *srv_sess_find_half_open(srv_t *s, const char *serial)
{
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++) {
        srv_session_t *e = s->table[k];
        if (e && e != SRV_DELETED && e->st == SS_HALF_OPEN && !strcmp(e->serial, serial)) return e;
    }
    return NULL;
}

srv_session_t *srv_sess_find_registered(srv_t *s, const char *serial)
{
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++) {
        srv_session_t *e = s->table[k];
        if (e && e != SRV_DELETED && e->st != SS_HALF_OPEN && !e->doomed && !strcmp(e->serial, serial)) return e;
    }
    return NULL;
}

const char *srv_state_name(srv_sess_state_t st)
{
    static const char *const N[] = {"HALF_OPEN", "REGISTERED", "PROVISIONING", "AWAIT_RESUME", "IDLE_GRACE",
                                    "OPERATIONAL", "FAILED"};
    return (unsigned)st < sizeof N / sizeof N[0] ? N[st] : "?";
}

void srv_sess_set_state(srv_t *s, srv_session_t *ss, srv_sess_state_t st)
{
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
    r.max_dgram = s->relcfg.max_dgram;
    r.keepalive_s = ss->assign.nat.keepalive_interval_s;
    r.grace_s = ss->assign.nat.grace_s;
    snprintf(r.serial, sizeof r.serial, "%s", ss->serial);
    snprintf(r.model, sizeof r.model, "%s", ss->model);
    snprintf(r.state, sizeof r.state, "%s", srv_state_name(ss->st));
    addr_fmt(&ss->peer, r.peer);
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
    srv_session_t *ss = calloc(1, sizeof *ss);
    if (!ss) return NULL;
    ss->srv = s;
    uint32_t sid;
    if (s->hook_first_sid && !srv_sess_find(s, s->hook_first_sid)) {
        sid = s->hook_first_sid;
        s->hook_first_sid = 0;
    } else {
        do sid = rand_u32_nonzero(); while (srv_sess_find(s, sid));   /* §7.2: redraw on collision */
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
    ss->created_ms = ss->last_rx_ms = s->now;
    ss->half_open_deadline = s->now + (uint64_t)s->cfg.c.nat.half_open_ttl_s * 1000u;

    size_t k = slot_of(sid);
    while (s->table[k] && s->table[k] != SRV_DELETED) k = (k + 1) % SRV_TABLE_SLOTS;
    s->table[k] = ss;
    s->nsessions++;
    s->nhalf_open++;
    if ((uint64_t)s->nsessions > s->c.sessions_peak) s->c.sessions_peak = (uint64_t)s->nsessions;
    return ss;
}

static void add_stats(rel_stats_t *a, const rel_stats_t *b)
{
    a->tx += b->tx; a->rx += b->rx; a->retx_timeout += b->retx_timeout; a->retx_fast += b->retx_fast;
    a->dup += b->dup; a->ooo += b->ooo; a->out_of_window += b->out_of_window; a->acks_sent += b->acks_sent;
    if (b->srtt_ms) a->srtt_ms = b->srtt_ms;
}

void srv_sess_free(srv_t *s, srv_session_t *ss, const char *why)
{
    if (ss->st != SS_HALF_OPEN && why) db_session_deleted(&s->db, ss->session_id, ss->serial, wall_ms());
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++)
        if (s->table[k] == ss) {
            s->table[k] = SRV_DELETED;
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
        ss->last_resume_ctr = js[i].last_resume_ctr;
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
}

uint64_t srv_sess_next_deadline(srv_t *s)
{
    uint64_t d = UINT64_MAX;
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++) {
        srv_session_t *e = s->table[k];
        if (!e || e == SRV_DELETED) continue;
        uint64_t r = rel_next_deadline(e->rel);
        if (r < d) d = r;
        if (e->st == SS_HALF_OPEN && e->half_open_deadline < d) d = e->half_open_deadline;
        if (e->st == SS_IDLE_GRACE && e->grace_deadline < d) d = e->grace_deadline;
        if (e->st != SS_HALF_OPEN && e->st != SS_IDLE_GRACE) {
            uint64_t idle = e->last_rx_ms + (uint64_t)e->assign.nat.server_idle_intervals *
                                               (uint64_t)e->assign.nat.keepalive_interval_s * 1000u;
            if (idle < d) d = idle;
        }
    }
    return d;
}

void srv_sess_run_timers(srv_t *s)
{
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++) {
        srv_session_t *e = s->table[k];
        if (!e || e == SRV_DELETED) continue;
        if (e->doomed) {
            srv_sess_free(s, e, NULL);
            continue;
        }
        if (e->st == SS_HALF_OPEN && s->now >= e->half_open_deadline) {
            srv_sess_free(s, e, "half-open TTL expired");
            continue;
        }
        if (e->st == SS_IDLE_GRACE) {
            if (s->now >= e->grace_deadline) {
                log_sess(LOG_INFO, T_NAT, e->session_id, e->serial, "Grace period over (%d s): session deleted, DB record kept",
                         e->assign.nat.grace_s);
                srv_sess_free(s, e, "grace expired");
            }
            continue;
        }
        bool failed = rel_on_timer(e->rel, s->now) == REL_E_FAILED;
        if (e->st == SS_HALF_OPEN) {
            if (failed) srv_sess_free(s, e, "retries exhausted");
            continue;
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
}
