/* Server internals shared by main.c, session.c and handlers.c (DESIGN §14). */
#ifndef SRV_H
#define SRV_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "artifacts.h"
#include "auth.h"
#include "config.h"
#include "db.h"
#include "hooks.h"
#include "net.h"
#include "netsim.h"
#include "policy.h"
#include "reliable.h"

typedef enum { SS_HALF_OPEN = 0, SS_REGISTERED, SS_PROVISIONING, SS_AWAIT_RESUME, SS_IDLE_GRACE,
               SS_OPERATIONAL, SS_FAILED } srv_sess_state_t;   /* §7.4 */

typedef struct srv srv_t;

typedef struct srv_session {
    srv_t   *srv;
    uint32_t session_id;
    uint16_t epoch;
    srv_sess_state_t st;
    uint8_t  db_stage;                /* SRV_STAGE (§5.4.3); 0 HELLO while half-open */
    char     serial[17], model[17], fw[9];
    struct sockaddr_in peer;
    uint64_t nd, ns;
    uint8_t  k_dev[ZTP_KEY_LEN], k_s[ZTP_KEY_LEN];
    bool     has_k_dev;
    rel_t   *rel;
    srv_assignment_t assign;
    uint64_t created_ms, last_rx_ms, half_open_deadline, grace_deadline;
    bool     established_logged;
    bool     ks_verified;             /* a frame under K_s has been verified (REGISTER) */
    bool     doomed;                  /* free after the current datagram (terminal reply sent) */
    uint32_t last_resume_ctr;
    /* transfer (§11.2): chunk requests served by a pump that reads on demand, O(window) memory */
    uint16_t max_dgram;               /* the session's datagram limit: 1200, or 508 after §5.8 */
    uint32_t chunk_size;              /* from the last MANIFEST */
    struct { uint8_t file_id; uint32_t next, end; } req[4];
    int      nreq;
    uint8_t  cached_hello_ack[ZTP_MAX_DGRAM_CAP];
    uint16_t cached_hello_ack_len;
    uint8_t  cur_key_id;              /* key of the frame being delivered (KEEPALIVE_ACK uses it) */
    const uint8_t *cur_key;
} srv_session_t;

#define SRV_TABLE_SLOTS 2048
#define SRV_NONCE_CACHE_MAX 65536          /* open addressing; load <= 0.5 at the 1024 cap (§14.2) */
#define SRV_TOMBSTONES  256
#define SRV_RATE_IPS    256

typedef struct {
    bool     used;
    uint32_t sid;                     /* 0 for HELLO-time errors */
    char     serial[17];
    uint64_t nonce;                   /* Nd or Nd' */
    uint64_t expires_ms;
    uint16_t len;
    uint8_t  bytes[ZTP_MIN_DGRAM];    /* terminal replies are small */
} srv_tombstone_t;

typedef struct {
    uint32_t ip;
    double   tokens;
    uint64_t last_ms;
} srv_rate_t;

typedef struct {
    char     serial[17];
    uint64_t nonce, ts_ms;
    bool     used;
} srv_nonce_t;

typedef struct {
    uint64_t rx_malformed, rx_unknown, rx_stale, mac_fail, replay_rejected, rate_limited, mtu_stepdowns;
    uint64_t sessions_peak;
    rel_stats_t rel;                  /* accumulated from freed sessions */
} srv_counters_t;

struct srv {
    cfg_t        cfg;
    srv_keys_t   keys;
    srv_policy_t policy;
    int          ctl_fd, disc_fd;
    netsim_t     ns;
    hooks_t      hooks;
    bool         hook_no_discovery;
    uint32_t     hook_first_sid;
    uint64_t     now;
    rel_cfg_t    relcfg;

    srv_session_t *table[SRV_TABLE_SLOTS];   /* NULL = empty, SRV_DELETED = tombstone slot */
    int            nsessions, nhalf_open;

    srv_tombstone_t tomb[SRV_TOMBSTONES];
    int             tomb_next;
    srv_rate_t      rate[SRV_RATE_IPS];
    srv_nonce_t    *nonces;           /* REGISTER nonce cache (§10.4 mechanism 3), ring */
    size_t          nonce_cap, nonce_next;
    srv_db_t        db;
    srv_artifacts_t artifacts;

    /* the datagram being processed (duplicates are re-ACKed to its source, §5.7) */
    struct sockaddr_in cur_src;
    srv_counters_t c;
};

/* session.c */
srv_session_t *srv_sess_find(srv_t *s, uint32_t sid);
srv_session_t *srv_sess_find_half_open(srv_t *s, const char *serial);
srv_session_t *srv_sess_find_registered(srv_t *s, const char *serial);
void           srv_sess_set_state(srv_t *s, srv_session_t *ss, srv_sess_state_t st);
void           srv_sess_journal(srv_t *s, srv_session_t *ss);
void           srv_sess_restore(srv_t *s);   /* journaled sessions -> IDLE_GRACE (§14.4) */
const char    *srv_state_name(srv_sess_state_t st);
srv_session_t *srv_sess_new(srv_t *s, const char *serial, const char *model);   /* NULL if full */
void           srv_sess_free(srv_t *s, srv_session_t *ss, const char *why);
uint64_t       srv_sess_next_deadline(srv_t *s);
void           srv_sess_run_timers(srv_t *s);
void           srv_sess_free_all(srv_t *s);

/* handlers.c */
void srv_sess_deliver(void *ctx, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t *p, uint16_t n);
void srv_on_ctl_datagram(srv_t *s, const uint8_t *buf, size_t len, const struct sockaddr_in *src);
void srv_on_disc_datagram(srv_t *s, const uint8_t *buf, size_t len, const struct sockaddr_in *src);
int  srv_send(srv_t *s, int fd, const uint8_t *frame, size_t len, const struct sockaddr_in *dst);
bool srv_rate_ok(srv_t *s, const struct sockaddr_in *src);
bool srv_nonce_seen(srv_t *s, const char *serial, uint64_t nonce);   /* also records it */
void srv_sess_pump(srv_session_t *ss);                                /* send requested chunks into the window */

#endif
