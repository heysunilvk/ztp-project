#include "policy.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "json.h"
#include "util.h"

typedef struct {
    const char *path;
    char       *err;
    size_t      errsz;
} ctx_t;

static int perr(ctx_t *c, unsigned line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int perr(ctx_t *c, unsigned line, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(c->err, c->errsz, "policy: %s:%u: %s", c->path, line, msg);
    return EX_CONFIG_CODE;
}

static int get_str(ctx_t *c, const json_value_t *v, const char *where, char *dst, size_t dsz)
{
    if (v->type != JSTRING) return perr(c, v->line, "%s: expected a string", where);
    if (strlen(v->u.str.s) != v->u.str.len || !str_copy(dst, dsz, v->u.str.s))
        return perr(c, v->line, "%s: longer than %zu characters", where, dsz - 1);
    return 0;
}

static int get_int(ctx_t *c, const json_value_t *v, const char *where, int lo, int hi, int *out)
{
    if (v->type != JINT) return perr(c, v->line, "%s: expected an integer", where);
    if (v->u.i < lo || v->u.i > hi) return perr(c, v->line, "%s=%lld: must be %d..%d", where, (long long)v->u.i, lo, hi);
    *out = (int)v->u.i;
    return 0;
}

static int parse_nat(ctx_t *c, const json_value_t *obj, const char *where, srv_nat_ovr_t *n)
{
    if (obj->type != JOBJECT) return perr(c, obj->line, "%s.nat: expected an object", where);
    char w[160];
    for (const json_value_t *m = obj->u.obj.first; m; m = m->next) {
        if (m->key[0] == '_') continue;
        snprintf(w, sizeof w, "%s.nat.%s", where, m->key);
        int rc;
        /* coarse type/range here; the §25.8 rules run on every resolved result */
        if (!strcmp(m->key, "keepalive_interval_s")) {
            if ((rc = get_int(c, m, w, 1, 3600, &n->keepalive_interval_s))) return rc;
            n->has_keepalive = true;
        } else if (!strcmp(m->key, "dead_intervals")) {
            if ((rc = get_int(c, m, w, 1, 100, &n->dead_intervals))) return rc;
            n->has_dead = true;
        } else if (!strcmp(m->key, "server_idle_intervals")) {
            if ((rc = get_int(c, m, w, 1, 100, &n->server_idle_intervals))) return rc;
            n->has_idle = true;
        } else if (!strcmp(m->key, "grace_s")) {
            if ((rc = get_int(c, m, w, 1, 86400, &n->grace_s))) return rc;
            n->has_grace = true;
        } else if (!strcmp(m->key, "allow_ip_change")) {
            if (m->type != JBOOL) return perr(c, m->line, "%s: expected true or false", w);
            n->allow_ip_change = m->u.b;
            n->has_allow_ip_change = true;
        } else {
            return perr(c, m->line, "unknown key %s", w);
        }
    }
    return 0;
}

/* kind: 't' tenant (nat only), 'm' model, 's' serial, 'd' default */
static int parse_entry(ctx_t *c, const json_value_t *obj, char kind, const char *where, srv_pol_entry_t *e,
                       bool *assign)
{
    if (obj->type != JOBJECT) return perr(c, obj->line, "%s: expected an object", where);
    e->line = obj->line;
    char w[160];
    int rc;
    for (const json_value_t *m = obj->u.obj.first; m; m = m->next) {
        if (m->key[0] == '_') continue;
        snprintf(w, sizeof w, "%s.%s", where, m->key);
        if (!strcmp(m->key, "nat") && kind != 'd') {
            if ((rc = parse_nat(c, m, where, &e->nat))) return rc;
            continue;
        }
        if (kind == 'd' && !strcmp(m->key, "action")) {
            if (m->type != JSTRING || (strcmp(m->u.str.s, "reject") && strcmp(m->u.str.s, "assign")))
                return perr(c, m->line, "%s: must be \"reject\" or \"assign\"", w);
            *assign = !strcmp(m->u.str.s, "assign");
            continue;
        }
        if (kind == 't') return perr(c, m->line, "unknown key %s (a tenant entry holds only nat)", w);
        if (!strcmp(m->key, "firmware")) {
            uint32_t parts[3];
            if ((rc = get_str(c, m, w, e->firmware, sizeof e->firmware))) return rc;
            if (version_parse(e->firmware, parts) != 0) return perr(c, m->line, "%s: not a version like v1.1", w);
            e->has_firmware = true;
        } else if (!strcmp(m->key, "firmware_file")) {
            if ((rc = get_str(c, m, w, e->firmware_file, sizeof e->firmware_file))) return rc;
            e->has_firmware_file = true;
        } else if (!strcmp(m->key, "config_version")) {
            if ((rc = get_int(c, m, w, 1, 65535, &e->config_version))) return rc;
            e->has_config_version = true;
        } else if (!strcmp(m->key, "config_file")) {
            if ((rc = get_str(c, m, w, e->config_file, sizeof e->config_file))) return rc;
            e->has_config_file = true;
        } else if (!strcmp(m->key, "tenant")) {
            if ((rc = get_str(c, m, w, e->tenant, sizeof e->tenant))) return rc;
            if (!e->tenant[0]) return perr(c, m->line, "%s: must not be empty", w);
            e->has_tenant = true;
        } else if (!strcmp(m->key, "notes")) {
            if ((rc = get_str(c, m, w, e->notes, sizeof e->notes))) return rc;
            e->has_notes = true;
        } else {
            return perr(c, m->line, "unknown key %s", w);
        }
    }
    return 0;
}

static bool complete(const srv_pol_entry_t *e)
{
    return e->has_firmware && e->has_firmware_file && e->has_config_version && e->has_config_file && e->has_tenant;
}

static bool serial_ok(const char *s)
{
    if (strlen(s) != 10 || strncmp(s, "ZTP-", 4)) return false;
    for (int i = 4; i < 10; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}

static int parse_map(ctx_t *c, const json_value_t *obj, char kind, const char *sect, srv_pol_entry_t **out,
                     size_t *n)
{
    if (obj->type != JOBJECT) return perr(c, obj->line, "%s: expected an object", sect);
    *out = calloc(obj->u.obj.count ? obj->u.obj.count : 1, sizeof **out);
    if (!*out) return perr(c, obj->line, "out of memory");
    for (const json_value_t *m = obj->u.obj.first; m; m = m->next) {
        if (m->key[0] == '_') continue;
        srv_pol_entry_t *e = &(*out)[*n];
        size_t maxlen = kind == 'm' ? 16 : 32;
        if (!*m->key || strlen(m->key) > maxlen) return perr(c, m->line, "%s: name \"%s\" must be 1..%zu characters", sect, m->key, maxlen);
        if (kind == 's' && !serial_ok(m->key)) return perr(c, m->line, "%s: \"%s\" is not a serial like ZTP-93AF21", sect, m->key);
        str_copy(e->name, sizeof e->name, m->key);
        char where[96];
        snprintf(where, sizeof where, "%s.%s", sect, m->key);
        bool dummy = false;
        int rc = parse_entry(c, m, kind, where, e, &dummy);
        if (rc) return rc;
        if (kind == 'm' && !complete(e))
            return perr(c, m->line, "%s: needs firmware, firmware_file, config_version, config_file and tenant", where);
        (*n)++;
    }
    return 0;
}

int srv_policy_load(const char *path, srv_policy_t *p, char *err, size_t errsz)
{
    memset(p, 0, sizeof *p);
    str_copy(p->path, sizeof p->path, path);
    ctx_t c = {path, err, errsz};
    char *text = NULL;
    size_t len = 0;
    if (read_file(path, JSON_MAX_INPUT, false, &text, &len) != 0) {
        snprintf(err, errsz, "policy: %s: cannot read: %s", path, strerror(errno));
        return EX_CONFIG_CODE;
    }
    json_error_t je;
    json_doc_t *doc = json_parse(text, len, &je);
    free(text);
    if (!doc) return perr(&c, je.line, "%s (at %s)", je.msg, je.path);
    json_value_t *root = json_root(doc);
    int rc = 0;
    if (root->type != JOBJECT) { rc = perr(&c, root->line, "top level must be an object"); goto out; }
    bool have_default = false;
    for (const json_value_t *m = root->u.obj.first; m && !rc; m = m->next) {
        if (m->key[0] == '_') continue;
        if (!strcmp(m->key, "schema_version")) {
            if (m->type != JINT || m->u.i != 1) rc = perr(&c, m->line, "schema_version must be 1");
        } else if (!strcmp(m->key, "default")) {
            have_default = true;
            rc = parse_entry(&c, m, 'd', "default", &p->dflt, &p->default_assign);
            if (!rc && p->default_assign && !complete(&p->dflt))
                rc = perr(&c, m->line, "default: action \"assign\" needs firmware, firmware_file, config_version, config_file and tenant");
        } else if (!strcmp(m->key, "tenants")) {
            rc = parse_map(&c, m, 't', "tenants", &p->tenants, &p->ntenants);
        } else if (!strcmp(m->key, "models")) {
            rc = parse_map(&c, m, 'm', "models", &p->models, &p->nmodels);
        } else if (!strcmp(m->key, "serials")) {
            rc = parse_map(&c, m, 's', "serials", &p->serials, &p->nserials);
        } else {
            rc = perr(&c, m->line, "unknown key %s", m->key);
        }
    }
    if (!rc && !have_default) rc = perr(&c, root->line, "missing \"default\" (use {\"action\": \"reject\"})");
out:
    json_free(doc);
    if (rc) srv_policy_free(p);
    return rc;
}

void srv_policy_free(srv_policy_t *p)
{
    free(p->tenants);
    free(p->models);
    free(p->serials);
    p->tenants = p->models = p->serials = NULL;
    p->ntenants = p->nmodels = p->nserials = 0;
}

static const srv_pol_entry_t *find(const srv_pol_entry_t *v, size_t n, const char *name)
{
    if (!name || !*name) return NULL;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(v[i].name, name)) return &v[i];
    return NULL;
}

static void take_assign(srv_assignment_t *o, const srv_pol_entry_t *e, bool have[5])
{
    if (!e) return;
    if (e->has_firmware && !have[0]) { str_copy(o->firmware, sizeof o->firmware, e->firmware); have[0] = true; }
    if (e->has_firmware_file && !have[1]) { str_copy(o->firmware_file, sizeof o->firmware_file, e->firmware_file); have[1] = true; }
    if (e->has_config_version && !have[2]) { o->config_version = e->config_version; have[2] = true; }
    if (e->has_config_file && !have[3]) { str_copy(o->config_file, sizeof o->config_file, e->config_file); have[3] = true; }
    if (e->has_tenant && !have[4]) { str_copy(o->tenant, sizeof o->tenant, e->tenant); have[4] = true; }
    if (e->has_notes && !o->notes[0]) str_copy(o->notes, sizeof o->notes, e->notes);
}

static void apply_nat(srv_assignment_t *o, const srv_pol_entry_t *e, const char *kind)
{
    if (!e) return;
    char label[64];
    snprintf(label, sizeof label, "policy:%s %s", kind, e->name);
    const srv_nat_ovr_t *n = &e->nat;
    if (n->has_keepalive) { o->nat.keepalive_interval_s = n->keepalive_interval_s; str_copy(o->src_keepalive, 64, label); }
    if (n->has_dead) { o->nat.dead_intervals = n->dead_intervals; str_copy(o->src_dead, 64, label); }
    if (n->has_idle) { o->nat.server_idle_intervals = n->server_idle_intervals; str_copy(o->src_idle, 64, label); }
    if (n->has_grace) { o->nat.grace_s = n->grace_s; str_copy(o->src_grace, 64, label); }
    if (n->has_allow_ip_change) { o->nat.allow_ip_change = n->allow_ip_change; str_copy(o->src_allow_ip_change, 64, label); }
}

void srv_policy_resolve(const srv_policy_t *p, const cfg_t *cfg, const char *model, const char *serial,
                        srv_assignment_t *o)
{
    memset(o, 0, sizeof *o);
    const srv_pol_entry_t *se = find(p->serials, p->nserials, serial);
    const srv_pol_entry_t *me = find(p->models, p->nmodels, model);

    /* assignment: serial > model > default (key by key) */
    bool have[5] = {false};
    take_assign(o, se, have);
    take_assign(o, me, have);
    if (p->default_assign) take_assign(o, &p->dflt, have);
    o->assigned = (se || me || p->default_assign) && have[0] && have[1] && have[2] && have[3] && have[4];

    /* NAT: server effective < tenant < model < serial */
    o->nat.keepalive_interval_s = cfg->c.nat.keepalive_interval_s;
    o->nat.dead_intervals = cfg->c.nat.dead_intervals;
    o->nat.server_idle_intervals = cfg->c.nat.server_idle_intervals;
    o->nat.grace_s = cfg->c.nat.grace_s;
    o->nat.allow_ip_change = cfg->c.nat.allow_ip_change;
    const char *s;
    str_copy(o->src_keepalive, 64, (s = cfg_source(cfg, "nat.keepalive_interval_s")) ? s : "default");
    str_copy(o->src_dead, 64, (s = cfg_source(cfg, "nat.dead_intervals")) ? s : "default");
    str_copy(o->src_idle, 64, (s = cfg_source(cfg, "nat.server_idle_intervals")) ? s : "default");
    str_copy(o->src_grace, 64, (s = cfg_source(cfg, "nat.grace_s")) ? s : "default");
    str_copy(o->src_allow_ip_change, 64, (s = cfg_source(cfg, "nat.allow_ip_change")) ? s : "default");
    apply_nat(o, find(p->tenants, p->ntenants, o->tenant), "tenant");
    apply_nat(o, me, "model");
    apply_nat(o, se, "serial");
}

static int check_nat(const srv_assignment_t *a, const char *where, char *err, size_t errsz)
{
    char msg[256];
    if (cfg_validate_nat_override(&a->nat, where, msg, sizeof msg) != 0) {
        snprintf(err, errsz, "policy: %s", msg);
        return EX_CONFIG_CODE;
    }
    return 0;
}

static int check_tenant(const srv_policy_t *p, const srv_pol_entry_t *e, const char *kind, char *err, size_t errsz)
{
    if (e->has_tenant && !find(p->tenants, p->ntenants, e->tenant)) {
        snprintf(err, errsz, "policy: %s:%u: %s %s: tenant \"%s\" is not defined under \"tenants\"", p->path, e->line,
                 kind, e->name[0] ? e->name : "default", e->tenant);
        return EX_CONFIG_CODE;
    }
    return 0;
}

int srv_policy_validate(const srv_policy_t *p, const cfg_t *cfg, const srv_keys_t *keys, char *err, size_t errsz)
{
    int rc;
    srv_assignment_t a;
    char where[96];
    if (p->default_assign && (rc = check_tenant(p, &p->dflt, "default", err, errsz))) return rc;
    for (size_t i = 0; i < p->nmodels; i++)
        if ((rc = check_tenant(p, &p->models[i], "model", err, errsz))) return rc;
    for (size_t i = 0; i < p->nserials; i++)
        if ((rc = check_tenant(p, &p->serials[i], "serial", err, errsz))) return rc;

    /* every model has a K_mfg */
    if (keys)
        for (size_t i = 0; i < p->nmodels; i++)
            if (!srv_keys_model(keys, p->models[i].name)) {
                snprintf(err, errsz, "policy: %s:%u: model %s has no key in keys.json", p->path, p->models[i].line,
                         p->models[i].name);
                return EX_CONFIG_CODE;
            }

    /* every resolved NAT block passes §25.8: tenant alone, each model, each serial with each model */
    for (size_t t = 0; t < p->ntenants; t++) {
        memset(&a, 0, sizeof a);
        a.nat = (ztp_nat_override_t){cfg->c.nat.keepalive_interval_s, cfg->c.nat.dead_intervals,
                                     cfg->c.nat.server_idle_intervals, cfg->c.nat.grace_s, cfg->c.nat.allow_ip_change};
        apply_nat(&a, &p->tenants[t], "tenant");
        snprintf(where, sizeof where, "policy:tenant %s", p->tenants[t].name);
        if ((rc = check_nat(&a, where, err, errsz))) return rc;
    }
    for (size_t m = 0; m < p->nmodels; m++) {
        srv_policy_resolve(p, cfg, p->models[m].name, "", &a);
        snprintf(where, sizeof where, "policy:model %s", p->models[m].name);
        if ((rc = check_nat(&a, where, err, errsz))) return rc;
        for (size_t s = 0; s < p->nserials; s++) {
            srv_policy_resolve(p, cfg, p->models[m].name, p->serials[s].name, &a);
            snprintf(where, sizeof where, "policy:serial %s", p->serials[s].name);
            if ((rc = check_nat(&a, where, err, errsz))) return rc;
        }
    }
    for (size_t s = 0; s < p->nserials; s++) {   /* serials resolved with no model entry */
        srv_policy_resolve(p, cfg, "", p->serials[s].name, &a);
        snprintf(where, sizeof where, "policy:serial %s", p->serials[s].name);
        if ((rc = check_nat(&a, where, err, errsz))) return rc;
    }
    return 0;
}

static int check_one_file(const srv_policy_t *p, const srv_pol_entry_t *e, bool is_cfg, char *err, size_t errsz)
{
    const char *f = is_cfg ? e->config_file : e->firmware_file;
    if (!(is_cfg ? e->has_config_file : e->has_firmware_file)) return 0;
    struct stat st;
    if (stat(f, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
        snprintf(err, errsz, "policy: %s:%u: %s %s: %s \"%s\" is missing or empty", p->path, e->line,
                 e->name[0] ? e->name : "default", is_cfg ? "config_file" : "firmware_file", is_cfg ? "config" : "firmware", f);
        return EX_CONFIG_CODE;
    }
    if (is_cfg) {
        char *text = NULL;
        size_t len = 0;
        json_error_t je;
        json_doc_t *d = NULL;
        if (read_file(f, JSON_MAX_INPUT, false, &text, &len) == 0) d = json_parse(text, len, &je);
        free(text);
        bool ok = d && json_root(d)->type == JOBJECT;
        json_free(d);
        if (!ok) {
            snprintf(err, errsz, "policy: %s:%u: %s: config_file \"%s\" is not a valid JSON object", p->path, e->line,
                     e->name[0] ? e->name : "default", f);
            return EX_CONFIG_CODE;
        }
    }
    return 0;
}

int srv_policy_check_files(const srv_policy_t *p, char *err, size_t errsz)
{
    int rc;
    const srv_pol_entry_t *sets[] = {p->models, p->serials};
    const size_t counts[] = {p->nmodels, p->nserials};
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; i < counts[k]; i++)
            if ((rc = check_one_file(p, &sets[k][i], false, err, errsz)) || (rc = check_one_file(p, &sets[k][i], true, err, errsz)))
                return rc;
    if (p->default_assign &&
        ((rc = check_one_file(p, &p->dflt, false, err, errsz)) || (rc = check_one_file(p, &p->dflt, true, err, errsz))))
        return rc;
    return 0;
}
