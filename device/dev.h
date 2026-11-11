/* Device internals shared by main.c, fsm.c, discovery.c, session.c, persist.c, keys.c (DESIGN §6, §7,
 * §9, §15). Named device_t because dev_t is taken by <sys/types.h>. */
#ifndef DEV_H
#define DEV_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "crypto.h"
#include "hooks.h"
#include "net.h"
#include "netsim.h"
#include "reliable.h"

/* ---- persisted state, <state-dir>/device.state (§15.3) ---- */
typedef struct {
    int      format;
    char     serial[17], model[17], hw_rev[9];
    uint32_t boot_count;
    char     active_slot;            /* 'A' or 'B' */
    int      pending_reboot;
    int      stage;                  /* enum ztp_stage */
    uint32_t session_id, resume_ctr;
    uint16_t epoch, proposed_epoch;
    char     server[ADDR_STR], tenant[33];
    uint16_t max_dgram;
    char     fw_target[9], fw_sha256[65], cfg_sha256[65];
    int      cfg_version, reg_attempts;
    char     last_fail_reason[65];
} dev_state_t;

int  dev_state_load(const char *path, dev_state_t *st);   /* 0, or -1 if absent/unreadable */
int  dev_state_save(const char *path, const dev_state_t *st);
const char *dev_stage_name(int stage);

/* ---- factory keys (keys.c) ---- */
void dev_factory_keys(const char *model, uint8_t k_fleet[ZTP_KEY_LEN], uint8_t k_mfg[ZTP_KEY_LEN]);

/* ---- device FSM (§15.1); M2 covers BOOT .. path probe ---- */
typedef enum {
    DS_DISCOVERY = 1,   /* sending DISCOVER, waiting for an OFFER */
    DS_SESSION,         /* HELLO sent, waiting for HELLO_ACK */
    DS_PROBING,         /* HELLO_ACK verified, path-probe KEEPALIVE in flight */
    DS_REGISTER,        /* probe acknowledged, REGISTER sent, waiting for REGISTER_ACK / NAK */
    DS_REGISTERED,      /* REGISTER_ACK received; CONFIG arrives in M4 (keepalives only for now) */
    DS_BACKOFF,         /* waiting (full jitter) before re-discovery */
    DS_FAILED,
    DS_EXIT,
} dev_fsm_t;

typedef struct {
    uint64_t rx_malformed, rx_unknown, rx_stale, mac_fail, timeouts;
} dev_counters_t;

typedef struct {
    cfg_t       cfg;
    dev_state_t st;
    char        state_path[CFG_PATH + 16];
    uint8_t     k_fleet[ZTP_KEY_LEN], k_boot[ZTP_KEY_LEN], k_mfg[ZTP_KEY_LEN], k_dev[ZTP_KEY_LEN], k_s[ZTP_KEY_LEN];
    int         fd;
    netsim_t    ns;
    hooks_t     hooks;
    uint64_t    now;
    dev_fsm_t   fsm;
    int         exit_code;

    struct sockaddr_in server;
    char        server_str[ADDR_STR];

    /* discovery */
    int         disc_attempt;
    uint64_t    disc_nonce, disc_deadline;
    struct sockaddr_in disc_dst;

    /* session */
    rel_t      *rel;
    rel_cfg_t   relcfg;
    uint32_t    sid;
    uint64_t    nd, ns_nonce;
    int         keepalive_s, dead_intervals, grace_s;
    uint64_t    last_tx, last_auth_rx;
    uint32_t    ka_seq;              /* seq of the keepalive in flight, 0 = none */
    bool        reg_sent;            /* REGISTER sent in this session: K_s from now on */
    uint64_t    nonce_reg;           /* NONCE_REG of the outstanding REGISTER */
    uint64_t    ka_sent;
    bool        ka_is_probe;

    /* reconnect backoff */
    int         backoff_n;
    uint64_t    backoff_until;
    uint64_t    hold_until;          /* exit after run.operational_hold_s (M2: after the probe) */

    /* NAT emulator (natsim.c, §9.8) */
    int         hook_rebind_after_s;
    struct {
        uint64_t start_ms, last_out_ms, blocked, rx_dropped;
        bool     mapped, flushed, reboot_done, ip_done, rebind_done;
        char     bind_ip[16];
    } nat;

    dev_counters_t c;
} device_t;

/* discovery.c */
void dev_discovery_start(device_t *d);
void dev_discovery_timer(device_t *d);
void dev_on_offer(device_t *d, const uint8_t *buf, size_t len, const ztp_hdr_t *h);
void dev_use_static(device_t *d);

/* session.c */
void dev_session_start(device_t *d);
void dev_session_end(device_t *d);
void dev_session_rx(device_t *d, const uint8_t *buf, size_t len, const ztp_hdr_t *h, const struct sockaddr_in *src);
void dev_session_timer(device_t *d);
uint64_t dev_session_deadline(const device_t *d);

/* natsim.c */
bool     dev_nat_active(const device_t *d);
void     dev_nat_init(device_t *d);
int      dev_rebind(device_t *d, const char *ip, const char *why);
int      dev_nat_before_send(device_t *d, const struct sockaddr_in *dst);   /* 1 = drop */
bool     dev_nat_accept_rx(device_t *d);
uint64_t dev_nat_deadline(const device_t *d);
void     dev_nat_timer(device_t *d);

/* fsm.c */
int  dev_send(device_t *d, const uint8_t *frame, size_t len, const struct sockaddr_in *dst);
void dev_backoff(device_t *d, const char *why);
void dev_fail(device_t *d, const char *reason);
int  dev_run(device_t *d);   /* the event loop; returns the exit code */

#endif
