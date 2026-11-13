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
#include "transfer.h"

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
    DS_RESUMING,        /* resume HELLO (K_dev) sent, waiting for HELLO_ACK RESUMED (§12.3) */
    DS_PROBING,         /* HELLO_ACK verified, path-probe KEEPALIVE in flight */
    DS_REGISTER,        /* probe acknowledged, REGISTER sent, waiting for REGISTER_ACK / NAK */
    DS_REGISTERED,      /* REGISTER_ACK received */
    DS_CONFIG,          /* downloading the config (§11.5) */
    DS_FIRMWARE,        /* downloading the firmware into the inactive slot */
    DS_UPGRADING,       /* UPGRADE_STATUS INSTALLING sent; commit and restart once it is ACKed (§12.1) */
    DS_VALIDATING,      /* the three checks of §13 */
    DS_OPERATIONAL,     /* STATUS_ACK received; keepalives continue */
    DS_BACKOFF,         /* waiting (full jitter) before re-discovery */
    DS_FAILED,
    DS_EXIT,
} dev_fsm_t;

typedef struct {
    uint64_t rx_malformed, rx_unknown, rx_stale, mac_fail, timeouts;
} dev_counters_t;

/* One file transfer (§11.5): config first, then firmware. Files: xfer_<kind>.part / .map */
typedef struct {
    bool          active;
    uint8_t       kind, file_id;
    char          version[9];
    uint32_t      size, chunk_size, count;
    uint8_t       sha256[32];
    dev_xfermap_t map;
    int           fd;
    char          part[CFG_PATH + 32], mappath[CFG_PATH + 32];
    bool          req_out, req_acked;
    uint32_t      req_start, req_num, req_seq;
    uint64_t      req_acked_ms;   /* when the CHUNK_REQ was ACKed, or the last chunk arrived */
    bool          probed;         /* black-hole liveness probe sent (§5.8) */
    bool          bh_rerequested; /* one silent interval passed: chunks re-requested, the next confirms (§5.8) */
    uint32_t      since_persist, writes, dups;
    int           hash_attempt;
} dev_xfer_t;

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
    uint64_t    hold_until;          /* exit after run.operational_hold_s once OPERATIONAL */

    /* resume (§12.3) */
    uint16_t    epoch;               /* the session's epoch (the proposed one while resuming) */
    bool        resuming;
    bool        rejected_once;       /* RESUME_REJECTED once: retry with a new ctr and epoch, then fresh */
    bool        force_fresh;         /* next connect is a fresh HELLO + REGISTER */
    int         resume_cycles;       /* resume attempts without a reply (net.resume_cycles) */
    uint8_t     k_dev_ready;

    /* upgrade (§12) and validation (§13) */
    bool        upgraded;            /* this boot follows an upgrade restart (pending_reboot) */
    uint32_t    upg_seq;             /* seq of UPGRADE_STATUS INSTALLING */
    char       *self_path;           /* resolved at startup for execv (§12.2) */
    bool        restart_exit;        /* --restart-mode exit: exit(75) instead of execv */
    char      **argv;
    bool        cfg_refetched;
    bool        hook_no_refetch;     /* --no-refetch (test hook) */
    char        fw_approved[9];      /* running version the firmware decision accepted as current (§13) */
    uint32_t    rr_echo;
    rel_stats_t rel_acc;             /* stats of this process's ended sessions (the exit counters) */             /* last ECHO_SEQ a RESUME_REQUIRED was honoured for (one per seq, §5.6) */
    /* fault-injection hooks (§19.2): each fires at most once per state directory (hooks.fired) */
    char        hook_crash_at[16];   /* download, downloaded, verified, committed, post-exec, resume-sent */
    int         hook_crash_after_chunks;
    int         hook_restart_delay_s;
    bool        hook_fail_validate, hook_delete_config;
    char        hook_bad_fw_version[9];
    bool        hook_fw_override;
    int         hook_hold_probe_s;   /* --hook-hold-health-probe: delay the first health probe (TC-VAL-04) */
    uint64_t    probe_hold_until;
    uint32_t    fw_chunks_this_process;
    uint64_t    val_deadline;
    uint32_t    health_seq;
    uint64_t    health_sent;
    bool        report_pending;      /* STATUS_REPORT OPERATIONAL sent, waiting for STATUS_ACK */

    /* NAT emulator (natsim.c, §9.8) */
    int         hook_rebind_after_s;
    bool        hook_bad_key;       /* --bad-key: a wrong K_mfg (a mis-provisioned unit); K_fleet stays right */
    uint32_t    hook_rekey_at;      /* --rekey-at N: the forced re-key threshold instead of 2^30 (TC-UNIT-06) */
    struct {
        uint64_t start_ms, last_out_ms, blocked, rx_dropped;
        bool     mapped, flushed, reboot_done, ip_done, rebind_done;
        char     bind_ip[16];
    } nat;

    dev_xfer_t  x;
    uint64_t    fail_after;          /* FAILED once STATUS_ACK arrives, or at this deadline */
    bool        in_rx;               /* inside the engine's deliver callback: the engine must not be freed */
    bool        fail_pending;        /* dev_fail() requested from inside delivery */
    char        pending_reason[48];  /* its reason (a separate buffer: the caller may pass fail_reason) */
    char        fail_reason[48];

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

/* upgrade.c (§12) */
bool     dev_slot_version(const device_t *d, char slot, char ver[9], char model[17]);   /* from the slot header */
void     dev_running_fw(const device_t *d, char ver[9]);   /* active slot's header, or the factory version */
void     dev_upgrade_begin(device_t *d);       /* FW_STAGED: re-verify, UPGRADE_STATUS INSTALLING */
void     dev_upgrade_timer(device_t *d);       /* INSTALLING ACKed -> commit -> restart */
void     dev_upgrade_rollback(device_t *d);
void     dev_upgrade_installed(device_t *d);   /* after the restart: INSTALLED, then VALIDATE */
void     dev_restart(device_t *d);             /* execv(self) or exit(75) (§12.2) */
void     dev_resolve_self(device_t *d, char **argv);

/* validation (§13), in validate.c */
void     dev_validate_begin(device_t *d);
void     dev_validate_timer(device_t *d);
uint64_t dev_validate_deadline(const device_t *d);
void     dev_validate_on_status_ack(device_t *d);

/* session.c: resume */
void     dev_resume_start(device_t *d);
bool     dev_can_resume(const device_t *d);
void     dev_report_failed(device_t *d, uint16_t reason, const char *why);

/* provision.c (§11) */
void     dev_provision_next(device_t *d);   /* after REGISTER_ACK: config, then firmware, by persisted stage */
void     dev_xfer_on_manifest(device_t *d, const uint8_t *p, uint16_t n);
void     dev_xfer_on_chunk(device_t *d, const uint8_t *p, uint16_t n);
void     dev_xfer_on_large_rx(device_t *d);
void     dev_xfer_on_status_ack(device_t *d);
void     dev_xfer_timer(device_t *d);
uint64_t dev_xfer_deadline(const device_t *d);
void     dev_xfer_suspend(device_t *d);     /* session lost: persist the map, close the part file */
enum { KA_PERIODIC = 0, KA_PATH_PROBE = 1, KA_HEALTH_PROBE = 2 };
void     dev_send_keepalive(device_t *d, int kind);

/* natsim.c */
bool     dev_nat_active(const device_t *d);
void     dev_nat_init(device_t *d);
int      dev_rebind(device_t *d, const char *ip, const char *why);
int      dev_nat_before_send(device_t *d, const struct sockaddr_in *dst);   /* 1 = drop */
bool     dev_nat_accept_rx(device_t *d);
uint64_t dev_nat_deadline(const device_t *d);
void     dev_nat_timer(device_t *d);

/* Abort at a named §12.4 crash point if --crash-at names it (once per state directory). */
void     dev_crash_point(device_t *d, const char *point);
void     dev_rel_stats_add(rel_stats_t *a, const rel_stats_t *b);
void     dev_log_counters(device_t *d);   /* "Counters: ..." for this process (at exit and before a restart) */
uint64_t dev_validate_recovery_deadline(const device_t *d);
void     dev_validate_recovery_timer(device_t *d);   /* val.deadline_s while the session is being recovered */

/* fsm.c */
int  dev_send(device_t *d, const uint8_t *frame, size_t len, const struct sockaddr_in *dst);
void dev_backoff(device_t *d, const char *why);
void dev_fail(device_t *d, const char *reason);
int  dev_run(device_t *d);   /* the event loop; returns the exit code */

#endif
