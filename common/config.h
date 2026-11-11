/* Configuration loader (DESIGN §25): built-in defaults < config file < profile overlay < CLI.
 * Typed, range-checked, unknown keys rejected, source of every value tracked (--print-config).
 * One struct serves both binaries; the schema table says which keys exist on which side. */
#ifndef ZTP_CONFIG_H
#define ZTP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define EX_CONFIG_CODE 78   /* sysexits EX_CONFIG */
#define EX_USAGE_CODE  64   /* sysexits EX_USAGE */

typedef enum { CFG_SERVER = 1, CFG_DEVICE = 2 } cfg_side_t;

#define CFG_STR  128
#define CFG_PATH 256

typedef struct {
    int keepalive_interval_s, dead_intervals, server_idle_intervals, grace_s;
    bool allow_ip_change;
} ztp_nat_override_t;

typedef struct {
    int      schema_version;
    char     profile[CFG_STR];

    struct {
        char   bind[CFG_STR];
        int    server_port, discovery_port;
        char   mcast_group[CFG_STR];
        int    mcast_ttl;
        char   mcast_iface_addr[CFG_STR];
        int    max_datagram;
        bool   set_df, mtu_fallback;
        double reconnect_min_s, reconnect_max_s;   /* device */
        int    resume_cycles, pmtu_blackhole_s;    /* device */
    } net;

    struct {
        int  keepalive_interval_s, dead_intervals, server_idle_intervals, grace_s, half_open_ttl_s;
        bool allow_ip_change, rebind_advice;                      /* server */
        bool path_probe, accept_server_keepalive, blocked_hint;   /* device */
        int  keepalive_min_s, keepalive_max_s;                    /* device */
    } nat;

    struct {
        int tx_window, rto_initial_ms, rto_min_ms, rto_max_ms, max_retries, fast_retx_threshold,
            delayed_ack_ms;
    } rel;

    struct {
        int chunk_size, chunk_req_max, tx_queue_max;      /* server */
        int persist_every_chunks, hash_attempts;          /* device (chunk_req_max too) */
    } xfer;

    struct {
        int max_sessions, max_half_open, max_failed_records, hello_rate_per_ip, hello_burst,
            nonce_cache_total, nonce_cache_per_serial, nonce_ttl_s;
    } limits;

    struct {
        char policy[CFG_PATH], keys[CFG_PATH], artifacts[CFG_PATH], db[CFG_PATH], log[CFG_PATH],
             dump[CFG_PATH], state_dir[CFG_PATH];
    } paths;

    struct { char level[16]; bool dump_verbose; } log;

    struct {
        double   loss, reorder, dup;
        int      reorder_delay_ms, delay_ms, jitter_ms, mtu;
        char     mtu_mode[16];
        uint64_t seed;
        struct { int idle_timeout_s, reboot_at_s, change_ip_at_s; bool block_server_port; } nat;
    } sim;

    /* device only */
    struct { char model[17], hw_rev[9], serial[17], factory_fw[9]; bool serial_set; } identity;
    struct {
        char mode[16], server[CFG_STR], mcast_group[CFG_STR], mcast_iface_addr[CFG_STR],
             bcast_addr[CFG_STR];
        int  discovery_port, attempts, interval_ms;
    } discovery;
    struct { int register_attempts; } auth;
    struct { bool exit_when_operational; int operational_hold_s; } run;
    struct { int srtt_max_ms, deadline_s; } val;
} ztp_config_t;

/* One CLI --set (or shorthand flag) to apply as the last layer. */
typedef struct { char key[64]; char value[CFG_PATH]; } cfg_set_t;

#define CFG_MAX_SETS 64

typedef struct {
    const char *config_path;     /* --config (NULL = default path, optional) */
    const char *profile;         /* --profile (NULL = file's "profile", else "default") */
    bool        lenient;         /* --config-lenient */
    bool        print;           /* --print-config */
    cfg_set_t   sets[CFG_MAX_SETS];
    int         nsets;
} cfg_args_t;

/* Loaded result with provenance. */
typedef struct {
    ztp_config_t c;
    cfg_side_t   side;
    char         file[CFG_PATH];       /* the config file actually read, or "" */
    char         file_sha256[65];      /* hex sha256 of that file, or "-" */
    char         profile[CFG_STR];     /* effective profile name */
    char       **src;                  /* per-schema-entry source label (owned) */
    int          warnings;
} cfg_t;

/* Parse one argv flag at argv[*i] if it is a configuration flag (--config, --profile, --set,
 * --print-config, --config-lenient, --demo-timers, and the §19.2 shorthands such as --loss,
 * --port, --serial, -v). Advances *i past any argument.
 * Returns 1 if consumed, 0 if not a config flag, -1 on a usage error (message on stderr). */
int cfg_parse_flag(cfg_args_t *a, cfg_side_t side, int argc, char **argv, int *i);

/* Build the effective configuration. On error prints "config: ..." to stderr and returns
 * EX_CONFIG_CODE; returns 0 on success. Call cfg_free afterwards in either case. */
int  cfg_load(cfg_t *cfg, cfg_side_t side, const cfg_args_t *a);
void cfg_free(cfg_t *cfg);

/* Print the merged configuration as JSON, each leaf {"value": …, "source": "…"}. */
void cfg_print(const cfg_t *cfg, FILE *out);
/* Source label of one key ("default", "file:conf/server.json", "profile:demo", "cli"), or NULL. */
const char *cfg_source(const cfg_t *cfg, const char *key);
/* Log the one-line [CONFIG] summary (§25.9). */
void cfg_log_summary(const cfg_t *cfg);

/* Validate a per-device NAT override result (policy tenant/model/serial merge, §25.6/§25.8).
 * Returns 0, or -1 with a message in err. */
int cfg_validate_nat_override(const ztp_nat_override_t *n, const char *where, char *err, size_t errsz);

/* Number of keys in a side's schema, and the i-th key (for tests and docs). */
size_t      cfg_schema_count(cfg_side_t side);
const char *cfg_schema_key(cfg_side_t side, size_t i);

#endif
