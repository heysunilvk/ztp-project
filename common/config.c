#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "crypto.h"
#include "json.h"
#include "log.h"
#include "logmsg.h"
#include "util.h"

/* ---- schema ---- */

enum kt { KT_INT = 1, KT_DBL, KT_BOOL, KT_STR, KT_STRNULL, KT_ENUM, KT_IP, KT_HOSTPORT, KT_U64 };
#define S_ CFG_SERVER
#define D_ CFG_DEVICE
#define B_ (CFG_SERVER | CFG_DEVICE)

typedef struct {
    const char *key;
    uint8_t     type, sides;
    size_t      off, size;
    double      min, max;
    const char *def;      /* default as text ("null" for KT_STRNULL) */
    const char *def_dev;  /* device default if different, else NULL */
    const char *enums;    /* KT_ENUM: "a|b|c" */
} ent_t;

#define F(m) offsetof(ztp_config_t, m), sizeof(((ztp_config_t *)0)->m)
#define I(k, sd, m, lo, hi, d)       {k, KT_INT, sd, F(m), lo, hi, #d, NULL, NULL}
#define DB(k, sd, m, lo, hi, d)      {k, KT_DBL, sd, F(m), lo, hi, #d, NULL, NULL}
#define BO(k, sd, m, d)              {k, KT_BOOL, sd, F(m), 0, 1, #d, NULL, NULL}
#define ST(k, sd, m, d)              {k, KT_STR, sd, F(m), 0, 0, d, NULL, NULL}
#define ST2(k, sd, m, d, dd)         {k, KT_STR, sd, F(m), 0, 0, d, dd, NULL}
#define SN(k, sd, m)                 {k, KT_STRNULL, sd, F(m), 0, 0, "null", NULL, NULL}
#define EN(k, sd, m, d, e)           {k, KT_ENUM, sd, F(m), 0, 0, d, NULL, e}
#define IP(k, sd, m, d)              {k, KT_IP, sd, F(m), 0, 0, d, NULL, NULL}
#define HP(k, sd, m, d)              {k, KT_HOSTPORT, sd, F(m), 0, 0, d, NULL, NULL}

/* Order matters: keys sharing a prefix are contiguous (cfg_print emits nested objects). */
static const ent_t SCHEMA[] = {
    I("schema_version", B_, schema_version, 1, 1, 1),
    ST("profile", B_, profile, "default"),
    IP("net.bind", S_, net.bind, "0.0.0.0"),
    I("net.server_port", S_, net.server_port, 1, 65535, 5555),
    I("net.discovery_port", S_, net.discovery_port, 1, 65535, 5556),
    IP("net.mcast_group", S_, net.mcast_group, "239.255.90.90"),
    I("net.mcast_ttl", S_, net.mcast_ttl, 1, 255, 1),
    IP("net.mcast_iface_addr", S_, net.mcast_iface_addr, "0.0.0.0"),
    DB("net.reconnect_min_s", D_, net.reconnect_min_s, 0.1, 3600, 1),
    DB("net.reconnect_max_s", D_, net.reconnect_max_s, 0.1, 3600, 60),
    I("net.resume_cycles", D_, net.resume_cycles, 1, 100, 10),
    I("net.max_datagram", B_, net.max_datagram, 508, 1200, 1200),
    BO("net.set_df", B_, net.set_df, true),
    BO("net.mtu_fallback", B_, net.mtu_fallback, true),
    I("net.pmtu_blackhole_s", D_, net.pmtu_blackhole_s, 2, 60, 8),
    I("nat.keepalive_interval_s", B_, nat.keepalive_interval_s, 1, 300, 20),
    I("nat.dead_intervals", B_, nat.dead_intervals, 2, 10, 3),
    I("nat.server_idle_intervals", S_, nat.server_idle_intervals, 2, 10, 3),
    I("nat.grace_s", S_, nat.grace_s, 1, 3600, 120),
    I("nat.half_open_ttl_s", S_, nat.half_open_ttl_s, 1, 300, 10),
    BO("nat.allow_ip_change", S_, nat.allow_ip_change, true),
    BO("nat.rebind_advice", S_, nat.rebind_advice, true),
    BO("nat.path_probe", D_, nat.path_probe, true),
    BO("nat.accept_server_keepalive", D_, nat.accept_server_keepalive, true),
    I("nat.keepalive_min_s", D_, nat.keepalive_min_s, 1, 3600, 5),
    I("nat.keepalive_max_s", D_, nat.keepalive_max_s, 1, 3600, 300),
    BO("nat.blocked_hint", D_, nat.blocked_hint, true),
    I("rel.tx_window", B_, rel.tx_window, 1, 16, 8),
    I("rel.rto_initial_ms", B_, rel.rto_initial_ms, 100, 60000, 500),
    I("rel.rto_min_ms", B_, rel.rto_min_ms, 100, 60000, 200),
    I("rel.rto_max_ms", B_, rel.rto_max_ms, 100, 60000, 4000),
    I("rel.max_retries", B_, rel.max_retries, 1, 16, 8),
    I("rel.fast_retx_threshold", B_, rel.fast_retx_threshold, 1, 16, 3),
    I("rel.delayed_ack_ms", B_, rel.delayed_ack_ms, 0, 1000, 20),
    I("xfer.chunk_size", S_, xfer.chunk_size, 256, 1024, 1024),
    I("xfer.chunk_req_max", B_, xfer.chunk_req_max, 1, 32, 32),
    I("xfer.tx_queue_max", S_, xfer.tx_queue_max, 1, 4096, 64),
    I("xfer.persist_every_chunks", D_, xfer.persist_every_chunks, 1, 4096, 16),
    I("xfer.hash_attempts", D_, xfer.hash_attempts, 1, 10, 2),
    I("limits.max_sessions", S_, limits.max_sessions, 1, 1000000, 1024),
    I("limits.max_half_open", S_, limits.max_half_open, 1, 1000000, 256),
    I("limits.max_failed_records", S_, limits.max_failed_records, 1, 10000000, 10000),
    I("limits.hello_rate_per_ip", S_, limits.hello_rate_per_ip, 1, 1000000, 10),
    I("limits.hello_burst", S_, limits.hello_burst, 1, 1000000, 20),
    I("limits.nonce_cache_total", S_, limits.nonce_cache_total, 1, 10000000, 4096),
    I("limits.nonce_cache_per_serial", S_, limits.nonce_cache_per_serial, 1, 100000, 64),
    I("limits.nonce_ttl_s", S_, limits.nonce_ttl_s, 1, 86400, 600),
    ST("paths.policy", S_, paths.policy, "conf/policy.json"),
    ST("paths.keys", S_, paths.keys, "conf/keys.json"),
    ST("paths.artifacts", S_, paths.artifacts, "artifacts/"),
    ST("paths.db", S_, paths.db, "state/db.jsonl"),
    ST("paths.dump", S_, paths.dump, "logs/db_dump.txt"),
    ST("paths.state_dir", D_, paths.state_dir, "state/device"),
    ST2("paths.log", B_, paths.log, "logs/server.log", "logs/device_%s.log"),
    EN("log.level", B_, log.level, "info", "debug|info|warn"),
    BO("log.dump_verbose", S_, log.dump_verbose, false),
    DB("sim.loss", B_, sim.loss, 0, 1, 0),
    DB("sim.reorder", B_, sim.reorder, 0, 1, 0),
    I("sim.reorder_delay_ms", B_, sim.reorder_delay_ms, 0, 10000, 30),
    DB("sim.dup", B_, sim.dup, 0, 1, 0),
    I("sim.delay_ms", B_, sim.delay_ms, 0, 10000, 0),
    I("sim.jitter_ms", B_, sim.jitter_ms, 0, 10000, 0),
    I("sim.mtu", B_, sim.mtu, 0, 1500, 0),
    EN("sim.mtu_mode", B_, sim.mtu_mode, "emsgsize", "emsgsize|blackhole"),
    {"sim.seed", KT_U64, B_, F(sim.seed), 0, 0, "0", NULL, NULL},
    I("sim.nat.idle_timeout_s", D_, sim.nat.idle_timeout_s, 0, 86400, 0),
    I("sim.nat.reboot_at_s", D_, sim.nat.reboot_at_s, 0, 86400, 0),
    I("sim.nat.change_ip_at_s", D_, sim.nat.change_ip_at_s, 0, 86400, 0),
    BO("sim.nat.block_server_port", D_, sim.nat.block_server_port, false),
    ST("identity.model", D_, identity.model, "EDGE-GW-100"),
    ST("identity.hw_rev", D_, identity.hw_rev, "A1"),
    SN("identity.serial", D_, identity.serial),
    ST("identity.factory_fw", D_, identity.factory_fw, "v1.0"),
    EN("discovery.mode", D_, discovery.mode, "mcast", "mcast|bcast|static"),
    HP("discovery.server", D_, discovery.server, "10.10.10.5:5555"),
    IP("discovery.mcast_group", D_, discovery.mcast_group, "239.255.90.90"),
    IP("discovery.mcast_iface_addr", D_, discovery.mcast_iface_addr, "0.0.0.0"),
    I("discovery.discovery_port", D_, discovery.discovery_port, 1, 65535, 5556),
    IP("discovery.bcast_addr", D_, discovery.bcast_addr, "255.255.255.255"),
    I("discovery.attempts", D_, discovery.attempts, 1, 100, 3),
    I("discovery.interval_ms", D_, discovery.interval_ms, 100, 60000, 1000),
    I("auth.register_attempts", D_, auth.register_attempts, 1, 10, 3),
    BO("run.exit_when_operational", D_, run.exit_when_operational, false),
    I("run.operational_hold_s", D_, run.operational_hold_s, 0, 3600, 2),
    I("val.srtt_max_ms", D_, val.srtt_max_ms, 1, 60000, 1000),
    I("val.deadline_s", D_, val.deadline_s, 1, 3600, 180),
};
#define NSCHEMA (sizeof SCHEMA / sizeof SCHEMA[0])

static const ent_t *find_ent(cfg_side_t side, const char *key, size_t *idx)
{
    for (size_t i = 0; i < NSCHEMA; i++)
        if ((SCHEMA[i].sides & side) && !strcmp(SCHEMA[i].key, key)) {
            if (idx) *idx = i;
            return &SCHEMA[i];
        }
    return NULL;
}

/* true if some key of this side starts with prefix + "." */
static bool is_section(cfg_side_t side, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < NSCHEMA; i++)
        if ((SCHEMA[i].sides & side) && !strncmp(SCHEMA[i].key, prefix, n) && SCHEMA[i].key[n] == '.')
            return true;
    return false;
}

size_t cfg_schema_count(cfg_side_t side)
{
    size_t n = 0;
    for (size_t i = 0; i < NSCHEMA; i++) n += (SCHEMA[i].sides & side) != 0;
    return n;
}

const char *cfg_schema_key(cfg_side_t side, size_t k)
{
    for (size_t i = 0; i < NSCHEMA; i++)
        if (SCHEMA[i].sides & side) {
            if (k == 0) return SCHEMA[i].key;
            k--;
        }
    return NULL;
}

/* ---- errors ---- */

static int cfg_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int cfg_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("config: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return EX_CONFIG_CODE;
}

static void cfg_warn(cfg_t *cfg, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cfg_warn(cfg_t *cfg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("config: warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    cfg->warnings++;
}

/* ---- value setting ---- */

static void *field(ztp_config_t *c, const ent_t *e) { return (char *)c + e->off; }

static bool enum_ok(const char *enums, const char *v)
{
    size_t n = strlen(v);
    const char *p = enums;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t k = bar ? (size_t)(bar - p) : strlen(p);
        if (k == n && !strncmp(p, v, n)) return true;
        if (!bar) break;
        p = bar + 1;
    }
    return false;
}

static bool ipv4_ok(const char *s)
{
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

static bool hostport_ok(const char *s, int *port)
{
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s) return false;
    char ip[64];
    size_t n = (size_t)(colon - s);
    if (n >= sizeof ip) return false;
    memcpy(ip, s, n);
    ip[n] = '\0';
    if (!ipv4_ok(ip)) return false;
    char *end;
    errno = 0;
    long p = strtol(colon + 1, &end, 10);
    if (errno || *end || end == colon + 1 || p < 1 || p > 65535) return false;
    if (port) *port = (int)p;
    return true;
}

/* Set a string-like field; returns NULL or an error description. */
static const char *set_str(ztp_config_t *c, const ent_t *e, const char *v)
{
    if (strlen(v) >= e->size) return "too long";
    if (e->type == KT_ENUM && !enum_ok(e->enums, v)) return "not one of the allowed values";
    if (e->type == KT_IP && !ipv4_ok(v)) return "not an IPv4 address";
    if (e->type == KT_HOSTPORT && !hostport_ok(v, NULL)) return "not IPv4:PORT";
    str_copy(field(c, e), e->size, v);
    return NULL;
}

/* Parse a textual value (default or CLI) into the field. Returns NULL or an error description. */
static const char *set_text(ztp_config_t *c, const ent_t *e, const char *v)
{
    char *end;
    switch (e->type) {
    case KT_INT: {
        errno = 0;
        long long x = strtoll(v, &end, 10);
        if (errno || *end || end == v || x < INT_MIN || x > INT_MAX) return "expected an integer";
        *(int *)field(c, e) = (int)x;
        return NULL;
    }
    case KT_U64: {
        if (*v == '-') return "expected a non-negative integer";
        errno = 0;
        unsigned long long x = strtoull(v, &end, 10);
        if (errno || *end || end == v) return "expected a non-negative integer";
        *(uint64_t *)field(c, e) = (uint64_t)x;
        return NULL;
    }
    case KT_DBL: {
        errno = 0;
        double x = strtod(v, &end);
        if (errno || *end || end == v || !isfinite(x)) return "expected a number";
        *(double *)field(c, e) = x;
        return NULL;
    }
    case KT_BOOL:
        if (!strcmp(v, "true")) *(bool *)field(c, e) = true;
        else if (!strcmp(v, "false")) *(bool *)field(c, e) = false;
        else return "expected true or false";
        return NULL;
    case KT_STRNULL:
        if (!strcmp(v, "null")) {
            ((char *)field(c, e))[0] = '\0';
            return NULL;
        }
        return set_str(c, e, v);
    default:
        return set_str(c, e, v);
    }
}

static const char *set_json(ztp_config_t *c, const ent_t *e, const json_value_t *v)
{
    switch (e->type) {
    case KT_INT:
        if (v->type != JINT) return "expected an integer";
        if (v->u.i < INT_MIN || v->u.i > INT_MAX) return "integer out of range";
        *(int *)field(c, e) = (int)v->u.i;
        return NULL;
    case KT_U64:
        if (v->type != JINT || v->u.i < 0) return "expected a non-negative integer";
        *(uint64_t *)field(c, e) = (uint64_t)v->u.i;
        return NULL;
    case KT_DBL:
        if (!json_is_number(v)) return "expected a number";
        *(double *)field(c, e) = json_as_double(v);
        return NULL;
    case KT_BOOL:
        if (v->type != JBOOL) return "expected true or false";
        *(bool *)field(c, e) = v->u.b;
        return NULL;
    case KT_STRNULL:
        if (v->type == JNULL) {
            ((char *)field(c, e))[0] = '\0';
            return NULL;
        }
        /* fallthrough */
    default:
        if (v->type != JSTRING) return e->type == KT_STRNULL ? "expected a string or null" : "expected a string";
        if (strlen(v->u.str.s) != v->u.str.len) return "string contains NUL";
        return set_str(c, e, v->u.str.s);
    }
}

static void set_src(cfg_t *cfg, size_t idx, const char *label)
{
    free(cfg->src[idx]);
    cfg->src[idx] = strdup(label);
}

/* ---- layers ---- */

typedef struct {
    cfg_t      *cfg;
    const char *label;     /* "file:conf/server.json" or "profile:demo" */
    const char *where;     /* file path, for messages */
    bool        lenient;
    bool        is_profile;
    char        profile_out[CFG_STR];
} layer_t;

static int apply_obj(layer_t *L, const json_value_t *obj, const char *prefix)
{
    for (const json_value_t *m = obj->u.obj.first; m; m = m->next) {
        if (m->key[0] == '_') continue;   /* "_comment" and friends */
        char path[192];
        if (snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "." : "", m->key) >= (int)sizeof path)
            return cfg_err("%s: key path too long near \"%s\"", L->where, m->key);

        if (!*prefix && !strcmp(m->key, "profile")) {
            if (m->type != JSTRING || m->u.str.len == 0 || m->u.str.len >= CFG_STR)
                return cfg_err("profile (%s:%u): expected a profile name", L->where, m->line);
            if (!L->is_profile) str_copy(L->profile_out, sizeof L->profile_out, m->u.str.s);
            continue;
        }
        if (m->type == JOBJECT && is_section(L->cfg->side, path)) {
            int rc = apply_obj(L, m, path);
            if (rc) return rc;
            continue;
        }
        size_t idx;
        const ent_t *e = find_ent(L->cfg->side, path, &idx);
        if (!e || is_section(L->cfg->side, path)) {
            if (L->lenient) {
                cfg_warn(L->cfg, "unknown key %s (%s:%u), ignored", path, L->where, m->line);
                continue;
            }
            return cfg_err("unknown key %s (%s:%u)", path, L->where, m->line);
        }
        const char *why = set_json(&L->cfg->c, e, m);
        if (why) return cfg_err("%s (%s:%u): %s", path, L->where, m->line, why);
        set_src(L->cfg, idx, L->label);
    }
    return 0;
}

static int load_json_file(const char *path, bool must_exist, char sha_hex[65], json_doc_t **doc,
                          bool *found)
{
    char *text = NULL;
    size_t len = 0;
    *found = false;
    if (read_file(path, JSON_MAX_INPUT, false, &text, &len) != 0) {
        if (errno == ENOENT && !must_exist) return 0;
        if (errno == E2BIG) return cfg_err("%s: larger than %u bytes", path, JSON_MAX_INPUT);
        return cfg_err("%s: cannot read: %s", path, strerror(errno));
    }
    *found = true;
    if (sha_hex) {
        uint8_t d[32];
        sha256(text, len, d);
        hex_encode(d, 32, sha_hex);
    }
    json_error_t je;
    *doc = json_parse(text, len, &je);
    free(text);
    if (!*doc) return cfg_err("%s:%u:%u: %s (at %s)", path, je.line, je.col, je.msg, je.path);
    json_value_t *root = json_root(*doc);
    if (root->type != JOBJECT) {
        json_free(*doc);
        *doc = NULL;
        return cfg_err("%s: top level must be an object", path);
    }
    json_value_t *sv = json_get(root, "schema_version");
    if (sv && !(sv->type == JINT && sv->u.i == 1)) {
        json_free(*doc);
        *doc = NULL;
        return cfg_err("schema_version (%s): must be 1", path);
    }
    return 0;
}

static bool profile_name_ok(const char *s)
{
    if (!*s || strlen(s) > 32) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return false;
    return true;
}

/* ---- validation (§25.8) ---- */

#define VFAIL(key, fmtv, val, rule) \
    return cfg_err("%s=" fmtv " (%s): %s", key, val, cfg_source(cfg, key), rule)

static int validate(cfg_t *cfg)
{
    ztp_config_t *c = &cfg->c;
    /* per-key ranges */
    for (size_t i = 0; i < NSCHEMA; i++) {
        const ent_t *e = &SCHEMA[i];
        if (!(e->sides & cfg->side)) continue;
        if (e->type == KT_INT) {
            int v = *(int *)field(c, e);
            if (v < e->min || v > e->max)
                return cfg_err("%s=%d (%s): must be %.0f..%.0f", e->key, v, cfg->src[i], e->min, e->max);
        } else if (e->type == KT_DBL) {
            double v = *(double *)field(c, e);
            if (v < e->min || v > e->max)
                return cfg_err("%s=%g (%s): must be %g..%g", e->key, v, cfg->src[i], e->min, e->max);
        }
    }
    /* cross-field rules */
    if (c->rel.rto_min_ms > c->rel.rto_initial_ms || c->rel.rto_initial_ms > c->rel.rto_max_ms)
        VFAIL("rel.rto_initial_ms", "%d", c->rel.rto_initial_ms, "must satisfy rto_min_ms <= rto_initial_ms <= rto_max_ms");
    if (c->sim.mtu != 0 && c->sim.mtu < 68) VFAIL("sim.mtu", "%d", c->sim.mtu, "must be 0 (off) or 68..1500");
    if (c->nat.keepalive_interval_s < 5)
        cfg_warn(cfg, "nat.keepalive_interval_s=%d (%s): demo/test value", c->nat.keepalive_interval_s,
                 cfg_source(cfg, "nat.keepalive_interval_s"));

    if (cfg->side == CFG_SERVER) {
        int cs = c->xfer.chunk_size;
        if (cs & (cs - 1)) VFAIL("xfer.chunk_size", "%d", cs, "must be a power of two (256, 512 or 1024)");
        if (cs > c->net.max_datagram - 52)
            VFAIL("xfer.chunk_size", "%d", cs, "must be <= net.max_datagram - 52");
        if (c->net.server_port == c->net.discovery_port)
            VFAIL("net.discovery_port", "%d", c->net.discovery_port, "must differ from net.server_port");
        if (c->limits.max_half_open > c->limits.max_sessions)
            VFAIL("limits.max_half_open", "%d", c->limits.max_half_open, "must be <= limits.max_sessions");
        ztp_nat_override_t n = {c->nat.keepalive_interval_s, c->nat.dead_intervals,
                                c->nat.server_idle_intervals, c->nat.grace_s, c->nat.allow_ip_change};
        char err[256];
        if (cfg_validate_nat_override(&n, cfg_source(cfg, "nat.grace_s"), err, sizeof err) != 0)
            return cfg_err("%s", err);
    } else {
        if (c->nat.keepalive_min_s > c->nat.keepalive_interval_s ||
            c->nat.keepalive_interval_s > c->nat.keepalive_max_s)
            VFAIL("nat.keepalive_interval_s", "%d", c->nat.keepalive_interval_s,
                  "must satisfy keepalive_min_s <= keepalive_interval_s <= keepalive_max_s");
        if (c->net.reconnect_min_s > c->net.reconnect_max_s)
            VFAIL("net.reconnect_min_s", "%g", c->net.reconnect_min_s, "must be <= net.reconnect_max_s");
        int port = 0;
        hostport_ok(c->discovery.server, &port);
        if (port == c->discovery.discovery_port)
            VFAIL("discovery.discovery_port", "%d", c->discovery.discovery_port,
                  "must differ from the port in discovery.server");
        const char *s = c->identity.serial;
        if (*s) {
            bool ok = strlen(s) == 10 && !strncmp(s, "ZTP-", 4);
            for (int i = 4; ok && i < 10; i++)
                ok = (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F');
            if (!ok) VFAIL("identity.serial", "%s", s, "must be ZTP- followed by 6 uppercase hex digits");
        }
        uint32_t parts[3];
        if (version_parse(c->identity.factory_fw, parts) != 0)
            VFAIL("identity.factory_fw", "%s", c->identity.factory_fw, "must be a version like v1.0");
        if (!*c->identity.model) VFAIL("identity.model", "%s", "\"\"", "must not be empty");
        const char *pl = c->paths.log, *p1 = strstr(pl, "%s");
        bool log_ok = !p1 || !strstr(p1 + 2, "%s");
        for (const char *q = pl; log_ok && (q = strchr(q, '%')); q++)
            if (q != p1) log_ok = false;
            else q++;
        if (!log_ok) VFAIL("paths.log", "%s", pl, "may contain one %s (the serial) and no other '%'");
        c->identity.serial_set = *s != '\0';
    }
    return 0;
}

int cfg_validate_nat_override(const ztp_nat_override_t *n, const char *where, char *err, size_t errsz)
{
    const char *w = where ? where : "policy";
    if (n->keepalive_interval_s < 1 || n->keepalive_interval_s > 300) {
        snprintf(err, errsz, "nat.keepalive_interval_s=%d (%s): must be 1..300", n->keepalive_interval_s, w);
        return -1;
    }
    if (n->dead_intervals < 2 || n->dead_intervals > 10) {
        snprintf(err, errsz, "nat.dead_intervals=%d (%s): must be 2..10", n->dead_intervals, w);
        return -1;
    }
    if (n->server_idle_intervals < 2 || n->server_idle_intervals > 10 ||
        n->server_idle_intervals < n->dead_intervals) {
        snprintf(err, errsz, "nat.server_idle_intervals=%d (%s): must be 2..10 and >= nat.dead_intervals (%d)",
                 n->server_idle_intervals, w, n->dead_intervals);
        return -1;
    }
    long need = 2L * n->keepalive_interval_s * n->server_idle_intervals;
    if (n->grace_s < need || n->grace_s > 3600) {
        snprintf(err, errsz, "nat.grace_s=%d (%s): must be >= 2 x keepalive_interval_s x server_idle_intervals (%ld) and <= 3600",
                 n->grace_s, w, need);
        return -1;
    }
    return 0;
}

/* ---- load ---- */

int cfg_load(cfg_t *cfg, cfg_side_t side, const cfg_args_t *a)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->side = side;
    strcpy(cfg->file_sha256, "-");
    cfg->src = calloc(NSCHEMA, sizeof *cfg->src);
    if (!cfg->src) return cfg_err("out of memory");

    /* 1. built-in defaults */
    for (size_t i = 0; i < NSCHEMA; i++) {
        const ent_t *e = &SCHEMA[i];
        if (!(e->sides & side)) continue;
        const char *d = (side == CFG_DEVICE && e->def_dev) ? e->def_dev : e->def;
        const char *why = set_text(&cfg->c, e, d);
        if (why) return cfg_err("internal: bad default for %s: %s", e->key, why);
        set_src(cfg, i, "default");
    }

    /* 2. config file */
    const char *path = a->config_path ? a->config_path
                                      : (side == CFG_SERVER ? "conf/server.json" : "conf/device.json");
    json_doc_t *doc = NULL;
    bool found;
    int rc = load_json_file(path, a->config_path != NULL, cfg->file_sha256, &doc, &found);
    if (rc) return rc;
    char file_profile[CFG_STR] = "";
    if (found) {
        str_copy(cfg->file, sizeof cfg->file, path);
        char label[CFG_PATH + 8];
        snprintf(label, sizeof label, "file:%s", path);
        layer_t L = {cfg, label, path, a->lenient, false, ""};
        rc = apply_obj(&L, json_root(doc), "");
        json_free(doc);
        if (rc) return rc;
        str_copy(file_profile, sizeof file_profile, L.profile_out);
    }

    /* 3. profile overlay */
    const char *pname = a->profile ? a->profile : (*file_profile ? file_profile : "default");
    if (!profile_name_ok(pname)) return cfg_err("profile \"%s\": invalid name", pname);
    str_copy(cfg->profile, sizeof cfg->profile, pname);
    {
        char dir[CFG_PATH] = "conf";
        if (found) {
            str_copy(dir, sizeof dir, path);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
            else strcpy(dir, ".");
        }
        char ppath[CFG_PATH * 2];
        snprintf(ppath, sizeof ppath, "%s/profiles/%s/%s.json", dir,
                 side == CFG_SERVER ? "server" : "device", pname);
        bool pfound;
        doc = NULL;
        rc = load_json_file(ppath, false, NULL, &doc, &pfound);
        if (rc) return rc;
        if (!pfound && strcmp(pname, "default") != 0)
            return cfg_err("profile \"%s\": %s not found", pname, ppath);
        if (pfound) {
            char label[CFG_STR + 16];
            snprintf(label, sizeof label, "profile:%s", pname);
            layer_t L = {cfg, label, ppath, a->lenient, true, ""};
            rc = apply_obj(&L, json_root(doc), "");
            json_free(doc);
            if (rc) return rc;
        }
        size_t pidx;
        if (find_ent(side, "profile", &pidx)) {
            char label[CFG_PATH + 8] = "default";
            if (a->profile) strcpy(label, "cli");
            else if (*file_profile) snprintf(label, sizeof label, "file:%s", path);
            str_copy(cfg->c.profile, sizeof cfg->c.profile, pname);
            set_src(cfg, pidx, label);
        }
    }

    /* 4. CLI --set and shorthand flags */
    for (int i = 0; i < a->nsets; i++) {
        size_t idx;
        const ent_t *e = find_ent(side, a->sets[i].key, &idx);
        if (!e || !strcmp(e->key, "profile") || !strcmp(e->key, "schema_version"))
            return cfg_err("unknown key %s (cli)", a->sets[i].key);
        const char *why = set_text(&cfg->c, e, a->sets[i].value);
        if (why) return cfg_err("%s=%s (cli): %s", e->key, a->sets[i].value, why);
        set_src(cfg, idx, "cli");
    }

    /* 5. validation */
    return validate(cfg);
}

void cfg_free(cfg_t *cfg)
{
    if (cfg->src) {
        for (size_t i = 0; i < NSCHEMA; i++) free(cfg->src[i]);
        free(cfg->src);
    }
    cfg->src = NULL;
}

const char *cfg_source(const cfg_t *cfg, const char *key)
{
    size_t idx;
    if (!cfg->src || !find_ent(cfg->side, key, &idx)) return NULL;
    return cfg->src[idx] ? cfg->src[idx] : "default";
}

/* ---- print ---- */

static void json_str_out(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

static void value_out(FILE *f, const ztp_config_t *c, const ent_t *e)
{
    const void *p = (const char *)c + e->off;
    switch (e->type) {
    case KT_INT: fprintf(f, "%d", *(const int *)p); break;
    case KT_U64: fprintf(f, "%llu", (unsigned long long)*(const uint64_t *)p); break;
    case KT_DBL: fprintf(f, "%g", *(const double *)p); break;
    case KT_BOOL: fputs(*(const bool *)p ? "true" : "false", f); break;
    case KT_STRNULL:
        if (!*(const char *)p) { fputs("null", f); break; }
        /* fallthrough */
    default: json_str_out(f, (const char *)p); break;
    }
}

/* Split "a.b.c" into segments; returns count. */
static int split_key(const char *key, char seg[4][48])
{
    int n = 0;
    const char *p = key;
    while (*p && n < 4) {
        const char *dot = strchr(p, '.');
        size_t k = dot ? (size_t)(dot - p) : strlen(p);
        if (k >= 48) k = 47;
        memcpy(seg[n], p, k);
        seg[n][k] = '\0';
        n++;
        if (!dot) break;
        p = dot + 1;
    }
    return n;
}

void cfg_print(const cfg_t *cfg, FILE *f)
{
    char open[4][48];
    int nopen = 0;
    bool first[5] = {true, true, true, true, true};
    fputs("{", f);
    for (size_t i = 0; i < NSCHEMA; i++) {
        const ent_t *e = &SCHEMA[i];
        if (!(e->sides & cfg->side)) continue;
        char seg[4][48];
        int n = split_key(e->key, seg);
        int common = 0;
        while (common < nopen && common < n - 1 && !strcmp(open[common], seg[common])) common++;
        while (nopen > common) {
            nopen--;
            fprintf(f, "\n%*s}", 2 * (nopen + 1), "");
            first[nopen + 1] = true;
        }
        for (int d = nopen; d < n - 1; d++) {
            fprintf(f, "%s\n%*s", first[d] ? "" : ",", 2 * (d + 1), "");
            json_str_out(f, seg[d]);
            fputs(": {", f);
            first[d] = false;
            strcpy(open[d], seg[d]);
            nopen = d + 1;
            first[nopen] = true;
        }
        fprintf(f, "%s\n%*s", first[nopen] ? "" : ",", 2 * (nopen + 1), "");
        first[nopen] = false;
        json_str_out(f, seg[n - 1]);
        fputs(": {\"value\": ", f);
        value_out(f, &cfg->c, e);
        fputs(", \"source\": ", f);
        json_str_out(f, cfg->src[i] ? cfg->src[i] : "default");
        fputc('}', f);
    }
    while (nopen > 0) {
        nopen--;
        fprintf(f, "\n%*s}", 2 * (nopen + 1), "");
    }
    fputs("\n}\n", f);
}

void cfg_log_summary(const cfg_t *cfg)
{
    char grace[16];
    if (cfg->side == CFG_SERVER) snprintf(grace, sizeof grace, "%ds", cfg->c.nat.grace_s);
    else strcpy(grace, "-");
    LOGI(T_CONFIG, LM_CONFIG_SUMMARY, cfg->profile, *cfg->file ? cfg->file : "(none)", cfg->file_sha256,
         (unsigned)cfg->c.nat.keepalive_interval_s, grace, (unsigned)cfg->c.rel.tx_window,
         (unsigned)cfg->c.net.max_datagram);
}

/* ---- CLI ---- */

typedef struct {
    const char *flag;
    uint8_t     sides;
    const char *key;
    const char *fixed;   /* value for argument-less flags, NULL = takes an argument */
} flag_t;

static const flag_t FLAGS[] = {
    {"--loss", B_, "sim.loss", NULL},
    {"--reorder", B_, "sim.reorder", NULL},
    {"--reorder-delay", B_, "sim.reorder_delay_ms", NULL},
    {"--dup", B_, "sim.dup", NULL},
    {"--delay", B_, "sim.delay_ms", NULL},
    {"--jitter", B_, "sim.jitter_ms", NULL},
    {"--seed", B_, "sim.seed", NULL},
    {"--log", B_, "paths.log", NULL},
    {"-v", B_, "log.level", "debug"},
    {"-q", B_, "log.level", "warn"},
    {"--bind", S_, "net.bind", NULL},
    {"--port", S_, "net.server_port", NULL},
    {"--disc-port", S_, "net.discovery_port", NULL},
    {"--mcast", S_, "net.mcast_group", NULL},
    {"--mcast-iface", S_, "net.mcast_iface_addr", NULL},
    {"--policy", S_, "paths.policy", NULL},
    {"--keys", S_, "paths.keys", NULL},
    {"--db", S_, "paths.db", NULL},
    {"--dump-verbose", S_, "log.dump_verbose", "true"},
    {"--state-dir", D_, "paths.state_dir", NULL},
    {"--model", D_, "identity.model", NULL},
    {"--fw", D_, "identity.factory_fw", NULL},
    {"--serial", D_, "identity.serial", NULL},
    {"--discover", D_, "discovery.mode", NULL},
    {"--server", D_, "discovery.server", NULL},
    {"--bcast-addr", D_, "discovery.bcast_addr", NULL},
    {"--mcast-iface", D_, "discovery.mcast_iface_addr", NULL},
    {"--exit-when-operational", D_, "run.exit_when_operational", "true"},
};

static int add_set(cfg_args_t *a, const char *key, const char *value)
{
    if (a->nsets >= CFG_MAX_SETS) {
        fprintf(stderr, "too many configuration overrides\n");
        return -1;
    }
    if (!str_copy(a->sets[a->nsets].key, sizeof a->sets[a->nsets].key, key) ||
        !str_copy(a->sets[a->nsets].value, sizeof a->sets[a->nsets].value, value)) {
        fprintf(stderr, "override too long: %s\n", key);
        return -1;
    }
    a->nsets++;
    return 0;
}

static const char *need_arg(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s: missing argument\n", argv[*i]);
        return NULL;
    }
    return argv[++*i];
}

int cfg_parse_flag(cfg_args_t *a, cfg_side_t side, int argc, char **argv, int *i)
{
    const char *f = argv[*i], *v;
    if (!strcmp(f, "--config")) {
        if (!(v = need_arg(argc, argv, i))) return -1;
        a->config_path = v;
        return 1;
    }
    if (!strcmp(f, "--profile")) {
        if (!(v = need_arg(argc, argv, i))) return -1;
        a->profile = v;
        return 1;
    }
    if (!strcmp(f, "--demo-timers")) { a->profile = "demo"; return 1; }
    if (!strcmp(f, "--print-config")) { a->print = true; return 1; }
    if (!strcmp(f, "--config-lenient")) { a->lenient = true; return 1; }
    if (!strcmp(f, "--set")) {
        if (!(v = need_arg(argc, argv, i))) return -1;
        const char *eq = strchr(v, '=');
        if (!eq || eq == v) {
            fprintf(stderr, "--set: expected key=value, got \"%s\"\n", v);
            return -1;
        }
        char key[64];
        size_t n = (size_t)(eq - v);
        if (n >= sizeof key) { fprintf(stderr, "--set: key too long\n"); return -1; }
        memcpy(key, v, n);
        key[n] = '\0';
        return add_set(a, key, eq + 1) ? -1 : 1;
    }
    if (!strcmp(f, "--sim-mtu")) {
        if (!(v = need_arg(argc, argv, i))) return -1;
        char num[16];
        const char *colon = strchr(v, ':');
        size_t n = colon ? (size_t)(colon - v) : strlen(v);
        if (n == 0 || n >= sizeof num) { fprintf(stderr, "--sim-mtu: expected N[:emsgsize|:blackhole]\n"); return -1; }
        memcpy(num, v, n);
        num[n] = '\0';
        if (add_set(a, "sim.mtu", num)) return -1;
        if (colon && add_set(a, "sim.mtu_mode", colon + 1)) return -1;
        return 1;
    }
    if (side == CFG_DEVICE && !strcmp(f, "--nat-sim")) {
        if (!(v = need_arg(argc, argv, i))) return -1;
        char buf[CFG_PATH];
        if (!str_copy(buf, sizeof buf, v)) { fprintf(stderr, "--nat-sim: too long\n"); return -1; }
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            char key[64];
            char *eq = strchr(tok, '=');
            if (eq) *eq = '\0';
            snprintf(key, sizeof key, "sim.nat.%s", tok);
            if (add_set(a, key, eq ? eq + 1 : "true")) return -1;
        }
        return 1;
    }
    for (size_t k = 0; k < sizeof FLAGS / sizeof FLAGS[0]; k++) {
        if (!(FLAGS[k].sides & side) || strcmp(FLAGS[k].flag, f) != 0) continue;
        if (FLAGS[k].fixed) v = FLAGS[k].fixed;
        else if (!(v = need_arg(argc, argv, i))) return -1;
        return add_set(a, FLAGS[k].key, v) ? -1 : 1;
    }
    return 0;
}
