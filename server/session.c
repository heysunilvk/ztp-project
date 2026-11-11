/* Server session table (DESIGN §14.2): open addressing keyed on session_id, 2048 slots. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "logmsg.h"
#include "rand.h"
#include "srv.h"

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
    }
    return d;
}

void srv_sess_run_timers(srv_t *s)
{
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++) {
        srv_session_t *e = s->table[k];
        if (!e || e == SRV_DELETED) continue;
        if (e->st == SS_HALF_OPEN && s->now >= e->half_open_deadline) {
            srv_sess_free(s, e, "half-open TTL expired");
            continue;
        }
        if (rel_on_timer(e->rel, s->now) == REL_E_FAILED) {
            /* M3+: registered sessions move to IDLE_GRACE; a half-open one is simply dropped */
            srv_sess_free(s, e, "retries exhausted");
        }
    }
}
