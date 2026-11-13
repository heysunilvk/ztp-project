#include "db.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "log.h"
#include "protocol.h"
#include "util.h"

static const char *const STAGES[] = {"HELLO", "REGISTERED", "CONFIG_SENT", "FW_SENT", "UPGRADING", "UPGRADED",
                                     "OPERATIONAL", "FAILED"};

const char *db_stage_name(int s) { return s >= 0 && s < 8 ? STAGES[s] : "?"; }

static int stage_parse(const char *s)
{
    for (int i = 0; i < 8; i++)
        if (!strcmp(s, STAGES[i])) return i;
    return -1;
}

/* ---- index (linear probing with backward-shift deletion) ---- */

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void index_put(srv_db_t *db, uint32_t idx);

static void index_grow(srv_db_t *db)
{
    size_t ncap = db->index_cap ? db->index_cap * 2 : 1024;
    uint32_t *old = db->index;
    size_t ocap = db->index_cap;
    db->index = calloc(ncap, sizeof *db->index);
    db->index_cap = ncap;
    for (size_t i = 0; i < ocap; i++)
        if (old[i]) index_put(db, old[i] - 1);
    free(old);
}

static void index_put(srv_db_t *db, uint32_t idx)
{
    size_t m = db->index_cap - 1, k = hash_str(db->recs[idx].serial) & m;
    while (db->index[k]) k = (k + 1) & m;
    db->index[k] = idx + 1;
}

static size_t index_find(const srv_db_t *db, const char *serial)
{
    if (!db->index_cap) return SIZE_MAX;
    size_t m = db->index_cap - 1, k = hash_str(serial) & m;
    while (db->index[k]) {
        if (!strcmp(db->recs[db->index[k] - 1].serial, serial)) return k;
        k = (k + 1) & m;
    }
    return SIZE_MAX;
}

static void index_del(srv_db_t *db, size_t k)
{
    size_t m = db->index_cap - 1;
    db->index[k] = 0;
    for (size_t j = (k + 1) & m; db->index[j]; j = (j + 1) & m) {   /* backward shift */
        uint32_t e = db->index[j];
        db->index[j] = 0;
        index_put(db, e - 1);
    }
}

db_rec_t *db_get(srv_db_t *db, const char *serial)
{
    size_t k = index_find(db, serial);
    return k == SIZE_MAX ? NULL : &db->recs[db->index[k] - 1];
}

/* ---- journal ---- */

static void iso(uint64_t ms, char out[32])
{
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&s, &tm);
    size_t k = strftime(out, 32, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + k, 32 - k, ".%03uZ", (unsigned)(ms % 1000));
}

static void jstr(char *out, size_t cap, size_t *n, const char *key, const char *v)
{
    *n += (size_t)snprintf(out + *n, cap - *n, ",\"%s\":\"", key);
    for (; *v && *n + 8 < cap; v++) {
        if (*v == '"' || *v == '\\') { out[(*n)++] = '\\'; out[(*n)++] = *v; }
        else if ((unsigned char)*v < 0x20 || (unsigned char)*v >= 0x7f)   /* control and non-ASCII bytes: the */
            *n += (size_t)snprintf(out + *n, cap - *n, "\\u%04x", (unsigned char)*v);   /* line stays valid JSON */
        else out[(*n)++] = *v;
    }
    *n += (size_t)snprintf(out + *n, cap - *n, "\"");
}

static void jint(char *out, size_t cap, size_t *n, const char *key, long long v)
{
    *n += (size_t)snprintf(out + *n, cap - *n, ",\"%s\":%lld", key, v);
}

/* Start a line: {"ts":"…","ms":…,"ev":"…" */
static size_t jbegin(char *out, size_t cap, const char *ev, uint64_t ms)
{
    char ts[32];
    iso(ms, ts);
    return (size_t)snprintf(out, cap, "{\"ts\":\"%s\",\"ms\":%llu,\"ev\":\"%s\"", ts, (unsigned long long)ms, ev);
}

/* A synced write is on disk before the caller's ACK can leave (§12.3 rule 2, §14.4); its outcome is kept in
 * db->sync_failed so the caller can refuse to ACK (db_synced). */
static void jwrite(srv_db_t *db, char *line, size_t n, bool sync)
{
    if (db->replaying || !db->journal) return;
    snprintf(line + n, 8, "}\n");
    bool ok = fputs(line, db->journal) >= 0;
    if (sync) {
        ok = fflush(db->journal) == 0 && ok;
#ifdef F_FULLFSYNC
        if (ok && fcntl(fileno(db->journal), F_FULLFSYNC) != 0) ok = fsync(fileno(db->journal)) == 0;
#else
        if (ok) ok = fsync(fileno(db->journal)) == 0;
#endif
        if (!ok && !db->sync_failed) LOGW(T_DB, "Journal %s: write failed (%s): registrations are refused until it recovers", db->path, strerror(errno));
        if (ok && db->sync_failed) LOGI(T_DB, "Journal %s: writes succeed again", db->path);
        db->sync_failed = !ok;
    } else if (!ok && !db->sync_failed) {
        LOGW(T_DB, "Journal %s: write failed (%s)", db->path, strerror(errno));
    }
}

bool db_synced(const srv_db_t *db)
{
    return !db->sync_failed;
}

/* ---- records ---- */

static db_rec_t *rec_new(srv_db_t *db, const char *serial)
{
    if (db->nrecs == db->cap_recs) {
        size_t nc = db->cap_recs ? db->cap_recs * 2 : 256;
        db_rec_t *nr = realloc(db->recs, nc * sizeof *nr);
        if (!nr) return NULL;
        db->recs = nr;
        db->cap_recs = nc;
    }
    if ((db->nrecs + 1) * 2 > db->index_cap) index_grow(db);
    db_rec_t *r = &db->recs[db->nrecs];
    memset(r, 0, sizeof *r);
    r->used = true;
    snprintf(r->serial, sizeof r->serial, "%s", serial);
    index_put(db, (uint32_t)db->nrecs);
    db->nrecs++;
    return r;
}

static void fifo_push(srv_db_t *db, uint32_t idx)
{
    if (db->fifo_len == db->fifo_cap) {
        size_t nc = db->fifo_cap ? db->fifo_cap * 2 : 256;
        uint32_t *nf = malloc(nc * sizeof *nf);
        for (size_t i = 0; i < db->fifo_len; i++) nf[i] = db->failed_fifo[(db->fifo_head + i) % db->fifo_cap];
        free(db->failed_fifo);
        db->failed_fifo = nf;
        db->fifo_cap = nc;
        db->fifo_head = 0;
    }
    db->failed_fifo[(db->fifo_head + db->fifo_len++) % db->fifo_cap] = idx;
}

/* Remove record idx by moving the last record into its place. */
static void rec_remove(srv_db_t *db, uint32_t idx)
{
    size_t k = index_find(db, db->recs[idx].serial);
    if (k != SIZE_MAX) index_del(db, k);
    uint32_t last = (uint32_t)(db->nrecs - 1);
    if (idx != last) {
        size_t kl = index_find(db, db->recs[last].serial);
        db->recs[idx] = db->recs[last];
        if (kl != SIZE_MAX) db->index[kl] = idx + 1;
        for (size_t i = 0; i < db->fifo_len; i++) {   /* keep the FIFO pointing at the moved row */
            size_t p = (db->fifo_head + i) % db->fifo_cap;
            if (db->failed_fifo[p] == last) db->failed_fifo[p] = idx;
            else if (db->failed_fifo[p] == idx) db->failed_fifo[p] = UINT32_MAX;
        }
    } else {
        for (size_t i = 0; i < db->fifo_len; i++) {
            size_t p = (db->fifo_head + i) % db->fifo_cap;
            if (db->failed_fifo[p] == idx) db->failed_fifo[p] = UINT32_MAX;
        }
    }
    db->nrecs--;
}

/* §10.3: Failed rows with no prior Success are capped; evict the oldest. */
static void enforce_cap(srv_db_t *db)
{
    while (db->max_failed > 0 && db->nfailed_only > (size_t)db->max_failed && db->fifo_len) {
        uint32_t idx = db->failed_fifo[db->fifo_head];
        db->fifo_head = (db->fifo_head + 1) % db->fifo_cap;
        db->fifo_len--;
        if (idx == UINT32_MAX || idx >= db->nrecs) continue;
        db_rec_t *r = &db->recs[idx];
        if (r->ever_success || r->status != DB_FAILED) continue;
        rec_remove(db, idx);
        db->nfailed_only--;
        db->evicted++;
    }
}

db_rec_t *db_register_ok(srv_db_t *db, const char *serial, const char *model, const char *hw_rev,
                         const char *fw_initial, const char *fw_assigned, int cfg, const char *tenant,
                         const char *notes, const char *src, uint64_t now)
{
    db_rec_t *r = db_get(db, serial);
    bool re = r && r->ever_success;
    if (!r) r = rec_new(db, serial);
    if (!r) return NULL;
    if (r->status == DB_FAILED && !r->ever_success) db->nfailed_only--;
    if (!r->ever_success) {
        r->reg_time_ms = now;   /* the first success time is kept on re-registration */
        snprintf(r->fw_initial, sizeof r->fw_initial, "%s", fw_initial);
    }
    r->ever_success = true;
    r->status = DB_SUCCESS;
    snprintf(r->model, sizeof r->model, "%s", model);
    snprintf(r->hw_rev, sizeof r->hw_rev, "%s", hw_rev);
    snprintf(r->fw_assigned, sizeof r->fw_assigned, "%s", fw_assigned);
    snprintf(r->tenant, sizeof r->tenant, "%s", tenant);
    snprintf(r->notes, sizeof r->notes, "%s", notes ? notes : "");
    r->cfg_version = cfg;
    if (r->stage < SST_REGISTERED || r->stage == SST_FAILED) r->stage = SST_REGISTERED;
    r->attempts++;
    r->last_seen_ms = now;
    snprintf(r->last_src, sizeof r->last_src, "%s", src ? src : "");
    if (re) db_prov_note(db, serial, "Re-registered", now);
    char line[1024];
    size_t n = jbegin(line, sizeof line, "REGISTER_OK", now);
    jstr(line, sizeof line, &n, "serial", serial);
    jstr(line, sizeof line, &n, "model", model);
    jstr(line, sizeof line, &n, "hw_rev", hw_rev);
    jstr(line, sizeof line, &n, "fw_initial", fw_initial);
    jstr(line, sizeof line, &n, "fw_assigned", fw_assigned);
    jint(line, sizeof line, &n, "cfg", cfg);
    jstr(line, sizeof line, &n, "tenant", tenant);
    jstr(line, sizeof line, &n, "notes", r->notes);
    jstr(line, sizeof line, &n, "src", r->last_src);
    jwrite(db, line, n, true);
    return r;
}

db_rec_t *db_register_fail(srv_db_t *db, const char *serial, const char *model, const char *notes, const char *src,
                           uint64_t now)
{
    db_rec_t *r = db_get(db, serial);
    if (!r) {
        r = rec_new(db, serial);
        if (!r) return NULL;
        r->status = DB_FAILED;
        r->reg_time_ms = now;
        r->stage = SST_FAILED;
        snprintf(r->model, sizeof r->model, "%s", model);
        db->nfailed_only++;
        fifo_push(db, (uint32_t)(r - db->recs));
    }
    r->failed_attempts++;
    r->last_failure_ms = now;
    if (!r->ever_success) {   /* a failure never overwrites a Success row (§10.3) */
        r->status = DB_FAILED;
        snprintf(r->notes, sizeof r->notes, "%s", notes);
    }
    r->last_seen_ms = now;
    if (src) snprintf(r->last_src, sizeof r->last_src, "%s", src);
    char line[512];
    size_t n = jbegin(line, sizeof line, "REGISTER_FAIL", now);
    jstr(line, sizeof line, &n, "serial", serial);
    jstr(line, sizeof line, &n, "model", model);
    jstr(line, sizeof line, &n, "notes", notes);
    jstr(line, sizeof line, &n, "src", src ? src : "");
    jwrite(db, line, n, true);
    enforce_cap(db);
    return db_get(db, serial);
}

void db_stage(srv_db_t *db, const char *serial, int stage, uint64_t now)
{
    db_rec_t *r = db_get(db, serial);
    if (!r) return;
    if (stage != SST_FAILED && r->stage != SST_FAILED && stage <= r->stage) return;   /* monotonic */
    r->stage = stage;
    r->last_seen_ms = now;
    char line[256];
    size_t n = jbegin(line, sizeof line, "STAGE", now);
    jstr(line, sizeof line, &n, "serial", serial);
    jstr(line, sizeof line, &n, "stage", db_stage_name(stage));
    jwrite(db, line, n, false);
}

void db_prov_note(srv_db_t *db, const char *serial, const char *note, uint64_t now)
{
    db_rec_t *r = db_get(db, serial);
    if (!r || strstr(r->prov_notes, note)) return;
    size_t l = strlen(r->prov_notes);
    snprintf(r->prov_notes + l, sizeof r->prov_notes - l, "%s%s", l ? "; " : "", note);
    char line[512];
    size_t n = jbegin(line, sizeof line, "NOTE", now);
    jstr(line, sizeof line, &n, "serial", serial);
    jstr(line, sizeof line, &n, "note", note);
    jwrite(db, line, n, false);
}

void db_seen(srv_db_t *db, const char *serial, const char *src, uint64_t now)
{
    db_rec_t *r = db_get(db, serial);
    if (!r) return;
    r->last_seen_ms = now;
    if (src) snprintf(r->last_src, sizeof r->last_src, "%s", src);
}

/* ---- sessions ---- */

/* A deleted session is kept only for the 24 h in which a re-registration of its serial is noted as "after
 * session expiry" (handlers.c); older ones are dropped at open and at compaction, so the table and the
 * journal hold the live sessions plus one day of deletions, not every session ever seen. */
#define DB_DELETED_KEEP_MS (24u * 3600u * 1000u)

static void prune_sessions(srv_db_t *db, uint64_t now)
{
    size_t k = 0;
    for (size_t i = 0; i < db->nsess; i++)
        if (!db->sess[i].deleted || now - db->sess[i].ts_ms < DB_DELETED_KEEP_MS) db->sess[k++] = db->sess[i];
    db->nsess = k;
}

static db_sess_t *sess_slot(srv_db_t *db, uint32_t sid)
{
    for (size_t i = 0; i < db->nsess; i++)
        if (db->sess[i].sid == sid) return &db->sess[i];
    if (db->nsess == db->cap_sess) {
        size_t nc = db->cap_sess ? db->cap_sess * 2 : 64;
        db_sess_t *ns = realloc(db->sess, nc * sizeof *ns);
        if (!ns) return NULL;
        db->sess = ns;
        db->cap_sess = nc;
    }
    db_sess_t *s = &db->sess[db->nsess++];
    memset(s, 0, sizeof *s);
    s->sid = sid;
    return s;
}

void db_session(srv_db_t *db, const db_sess_t *in, uint64_t now)
{
    db_sess_t *s = sess_slot(db, in->sid);
    if (!s) return;
    *s = *in;
    s->ts_ms = now;
    s->deleted = false;
    char line[512];
    size_t n = jbegin(line, sizeof line, "SESSION", now);
    jint(line, sizeof line, &n, "sid", in->sid);
    jstr(line, sizeof line, &n, "serial", in->serial);
    jstr(line, sizeof line, &n, "model", in->model);
    jint(line, sizeof line, &n, "epoch", in->epoch);
    jint(line, sizeof line, &n, "last_resume_ctr", in->last_resume_ctr);
    jstr(line, sizeof line, &n, "state", in->state);
    jint(line, sizeof line, &n, "max_dgram", in->max_dgram);
    jint(line, sizeof line, &n, "keepalive_s", in->keepalive_s);
    jint(line, sizeof line, &n, "grace_s", in->grace_s);
    jstr(line, sizeof line, &n, "peer", in->peer);
    if (in->nd_resume || in->ns_resume) {
        char h[17];
        snprintf(h, sizeof h, "%016llx", (unsigned long long)in->nd_resume);
        jstr(line, sizeof line, &n, "nd_resume", h);
        snprintf(h, sizeof h, "%016llx", (unsigned long long)in->ns_resume);
        jstr(line, sizeof line, &n, "ns_resume", h);
    }
    jwrite(db, line, n, true);
}

void db_session_deleted(srv_db_t *db, uint32_t sid, const char *serial, uint64_t now)
{
    db_sess_t *s = sess_slot(db, sid);
    if (!s) return;
    s->deleted = true;
    s->ts_ms = now;
    if (serial) snprintf(s->serial, sizeof s->serial, "%s", serial);
    char line[256];
    size_t n = jbegin(line, sizeof line, "SESSION_DEL", now);
    jint(line, sizeof line, &n, "sid", sid);
    jstr(line, sizeof line, &n, "serial", s->serial);
    jwrite(db, line, n, true);
}

const db_sess_t *db_sessions(const srv_db_t *db, size_t *n)
{
    *n = db->nsess;
    return db->sess;
}

size_t db_failed_only(const srv_db_t *db) { return db->nfailed_only; }

size_t db_count(const srv_db_t *db, int status)
{
    size_t n = 0;
    for (size_t i = 0; i < db->nrecs; i++) n += db->recs[i].status == status;
    return n;
}

/* ---- open / replay / compact ---- */

static const char *gs(const json_value_t *o, const char *k)
{
    const json_value_t *v = json_get(o, k);
    return v && v->type == JSTRING ? v->u.str.s : "";
}

static long long gi(const json_value_t *o, const char *k)
{
    const json_value_t *v = json_get(o, k);
    return v && v->type == JINT ? v->u.i : 0;
}

static void replay_line(srv_db_t *db, const json_value_t *o)
{
    const char *ev = gs(o, "ev");
    uint64_t ms = (uint64_t)gi(o, "ms");
    if (!strcmp(ev, "REGISTER_OK")) {
        db_register_ok(db, gs(o, "serial"), gs(o, "model"), gs(o, "hw_rev"), gs(o, "fw_initial"), gs(o, "fw_assigned"),
                       (int)gi(o, "cfg"), gs(o, "tenant"), gs(o, "notes"), gs(o, "src"), ms);
    } else if (!strcmp(ev, "REGISTER_FAIL")) {
        db_register_fail(db, gs(o, "serial"), gs(o, "model"), gs(o, "notes"), gs(o, "src"), ms);
    } else if (!strcmp(ev, "STAGE")) {
        int st = stage_parse(gs(o, "stage"));
        if (st >= 0) db_stage(db, gs(o, "serial"), st, ms);
    } else if (!strcmp(ev, "NOTE")) {
        db_prov_note(db, gs(o, "serial"), gs(o, "note"), ms);
    } else if (!strcmp(ev, "RECORD")) {   /* a compacted row */
        db_rec_t *r = db_get(db, gs(o, "serial"));
        if (!r) r = rec_new(db, gs(o, "serial"));
        if (!r) return;
        r->status = (int)gi(o, "status");
        r->ever_success = gi(o, "ever_success") != 0;
        r->reg_time_ms = (uint64_t)gi(o, "reg_time_ms");
        r->last_seen_ms = (uint64_t)gi(o, "last_seen_ms");
        r->last_failure_ms = (uint64_t)gi(o, "last_failure_ms");
        snprintf(r->model, sizeof r->model, "%s", gs(o, "model"));
        snprintf(r->hw_rev, sizeof r->hw_rev, "%s", gs(o, "hw_rev"));
        snprintf(r->fw_assigned, sizeof r->fw_assigned, "%s", gs(o, "fw_assigned"));
        snprintf(r->fw_initial, sizeof r->fw_initial, "%s", gs(o, "fw_initial"));
        snprintf(r->tenant, sizeof r->tenant, "%s", gs(o, "tenant"));
        snprintf(r->notes, sizeof r->notes, "%s", gs(o, "notes"));
        snprintf(r->prov_notes, sizeof r->prov_notes, "%s", gs(o, "prov_notes"));
        snprintf(r->last_src, sizeof r->last_src, "%s", gs(o, "src"));
        r->cfg_version = (int)gi(o, "cfg");
        r->stage = stage_parse(gs(o, "stage")) < 0 ? 0 : stage_parse(gs(o, "stage"));
        r->attempts = (uint32_t)gi(o, "attempts");
        r->failed_attempts = (uint32_t)gi(o, "failed_attempts");
        if (r->status == DB_FAILED && !r->ever_success) {
            db->nfailed_only++;
            fifo_push(db, (uint32_t)(r - db->recs));
        }
    } else if (!strcmp(ev, "SESSION")) {
        db_sess_t s = {0};
        s.sid = (uint32_t)gi(o, "sid");
        s.epoch = (uint16_t)gi(o, "epoch");
        s.last_resume_ctr = (uint32_t)gi(o, "last_resume_ctr");
        s.max_dgram = (uint16_t)gi(o, "max_dgram");
        s.keepalive_s = (int)gi(o, "keepalive_s");
        s.grace_s = (int)gi(o, "grace_s");
        snprintf(s.serial, sizeof s.serial, "%s", gs(o, "serial"));
        snprintf(s.model, sizeof s.model, "%s", gs(o, "model"));
        snprintf(s.state, sizeof s.state, "%s", gs(o, "state"));
        snprintf(s.peer, sizeof s.peer, "%s", gs(o, "peer"));
        s.nd_resume = strtoull(gs(o, "nd_resume"), NULL, 16);
        s.ns_resume = strtoull(gs(o, "ns_resume"), NULL, 16);
        db_session(db, &s, ms);
    } else if (!strcmp(ev, "SESSION_DEL")) {
        db_session_deleted(db, (uint32_t)gi(o, "sid"), gs(o, "serial"), ms);
    }
}

int db_open(srv_db_t *db, const char *path, int max_failed, char *err, size_t errsz)
{
    memset(db, 0, sizeof *db);
    db->max_failed = max_failed;
    snprintf(db->path, sizeof db->path, "%s", path);
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = '\0'; mkdir(dir, 0755); }
    FILE *in = fopen(path, "r");
    bool torn = false;   /* the file ends without a newline: a write cut short by a crash */
    if (in) {
        db->replaying = true;
        char line[4096];
        unsigned ln = 0, bad = 0;
        while (fgets(line, sizeof line, in)) {
            ln++;
            size_t l = strlen(line);
            torn = l == 0 || line[l - 1] != '\n';
            json_error_t je;
            json_doc_t *d = json_parse(line, strlen(line), &je);
            if (!d) { bad++; continue; }   /* a torn last line after a crash is skipped */
            if (json_root(d)->type == JOBJECT) replay_line(db, json_root(d));
            json_free(d);
        }
        fclose(in);
        db->replaying = false;
        prune_sessions(db, (uint64_t)time(NULL) * 1000u);
        LOGI(T_DB, "Journal %s replayed: %u lines (%u skipped), %zu records, %zu sessions", path, ln, bad, db->nrecs,
             db->nsess);
    }
    db->journal = fopen(path, "a");
    if (!db->journal) {
        snprintf(err, errsz, "db: cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (torn) {   /* end the torn line, or the next event would be appended to it and lost with it on replay */
        fputs("\n", db->journal);
        fflush(db->journal);
    }
    return 0;
}

void db_close(srv_db_t *db, bool compact)
{
    if (compact && db->journal) {
        /* rewrite as one RECORD per device plus the live sessions, atomically */
        fclose(db->journal);
        db->journal = NULL;
        size_t cap = 4096 + db->nrecs * 1024 + db->nsess * 512, n = 0;
        char *buf = malloc(cap);
        uint64_t now = (uint64_t)time(NULL) * 1000u;
        prune_sessions(db, now);
        for (size_t i = 0; buf && i < db->nrecs; i++) {
            const db_rec_t *r = &db->recs[i];
            char line[1536];
            size_t k = jbegin(line, sizeof line, "RECORD", now);
            jstr(line, sizeof line, &k, "serial", r->serial);
            jint(line, sizeof line, &k, "status", r->status);
            jint(line, sizeof line, &k, "ever_success", r->ever_success);
            jint(line, sizeof line, &k, "reg_time_ms", (long long)r->reg_time_ms);
            jint(line, sizeof line, &k, "last_seen_ms", (long long)r->last_seen_ms);
            jint(line, sizeof line, &k, "last_failure_ms", (long long)r->last_failure_ms);
            jstr(line, sizeof line, &k, "model", r->model);
            jstr(line, sizeof line, &k, "hw_rev", r->hw_rev);
            jstr(line, sizeof line, &k, "fw_assigned", r->fw_assigned);
            jstr(line, sizeof line, &k, "fw_initial", r->fw_initial);
            jstr(line, sizeof line, &k, "tenant", r->tenant);
            jint(line, sizeof line, &k, "cfg", r->cfg_version);
            jstr(line, sizeof line, &k, "stage", db_stage_name(r->stage));
            jint(line, sizeof line, &k, "attempts", r->attempts);
            jint(line, sizeof line, &k, "failed_attempts", r->failed_attempts);
            jstr(line, sizeof line, &k, "notes", r->notes);
            jstr(line, sizeof line, &k, "prov_notes", r->prov_notes);
            jstr(line, sizeof line, &k, "src", r->last_src);
            k += (size_t)snprintf(line + k, sizeof line - k, "}\n");
            if (n + k < cap) { memcpy(buf + n, line, k); n += k; }
        }
        for (size_t i = 0; buf && i < db->nsess; i++) {
            const db_sess_t *s = &db->sess[i];
            char line[512];
            size_t k = jbegin(line, sizeof line, s->deleted ? "SESSION_DEL" : "SESSION", s->ts_ms);
            jint(line, sizeof line, &k, "sid", s->sid);
            jstr(line, sizeof line, &k, "serial", s->serial);
            if (!s->deleted) {
                jstr(line, sizeof line, &k, "model", s->model);
                jint(line, sizeof line, &k, "epoch", s->epoch);
                jint(line, sizeof line, &k, "last_resume_ctr", s->last_resume_ctr);
                jstr(line, sizeof line, &k, "state", s->state);
                jint(line, sizeof line, &k, "max_dgram", s->max_dgram);
                jint(line, sizeof line, &k, "keepalive_s", s->keepalive_s);
                jint(line, sizeof line, &k, "grace_s", s->grace_s);
                jstr(line, sizeof line, &k, "peer", s->peer);
                if (s->nd_resume || s->ns_resume) {
                    char h[17];
                    snprintf(h, sizeof h, "%016llx", (unsigned long long)s->nd_resume);
                    jstr(line, sizeof line, &k, "nd_resume", h);
                    snprintf(h, sizeof h, "%016llx", (unsigned long long)s->ns_resume);
                    jstr(line, sizeof line, &k, "ns_resume", h);
                }
            }
            k += (size_t)snprintf(line + k, sizeof line - k, "}\n");
            if (n + k < cap) { memcpy(buf + n, line, k); n += k; }
        }
        if (buf && atomic_write_file(db->path, buf, n) != 0) LOGW(T_DB, "journal compaction failed: %s", strerror(errno));
        free(buf);
    }
    if (db->journal) fclose(db->journal);
    free(db->recs);
    free(db->index);
    free(db->failed_fifo);
    free(db->sess);
    memset(db, 0, sizeof *db);
}

/* ---- dump (§14.5) ---- */

static size_t dwidth(const char *s)   /* display columns of UTF-8 text */
{
    size_t w = 0;
    for (; *s; s++) w += ((unsigned char)*s & 0xC0) != 0x80;
    return w;
}

static void cell(FILE *f, const char *s, size_t w)
{
    fprintf(f, " %s", s);
    for (size_t i = dwidth(s); i < w; i++) fputc(' ', f);
    fputs(" |", f);
}

static void rule(FILE *f, const size_t *w, int ncol)
{
    fputc('+', f);
    for (int c = 0; c < ncol; c++) {
        for (size_t i = 0; i < w[c] + 2; i++) fputc('-', f);
        fputc('+', f);
    }
    fputc('\n', f);
}

static void tm_str(uint64_t ms, char out[24], bool with_t)
{
    if (!ms) { snprintf(out, 24, "—"); return; }
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&s, &tm);
    strftime(out, 24, with_t ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%d %H:%M:%S", &tm);
}

static int by_time(const void *a, const void *b)
{
    const db_rec_t *x = *(const db_rec_t *const *)a, *y = *(const db_rec_t *const *)b;
    if (x->reg_time_ms != y->reg_time_ms) return x->reg_time_ms < y->reg_time_ms ? -1 : 1;
    return strcmp(x->serial, y->serial);
}

#define NCOL_MAX 15

void db_dump(const srv_db_t *db, FILE *f, bool verbose)
{
    static const char *const H[NCOL_MAX] = {"Device ID", "Registration Status", "Firmware Assigned", "Config Version",
                                            "Registration Time", "Notes", "Model", "FW Initial", "Tenant", "Stage",
                                            "Attempts", "Failed Attempts", "Last Seen", "Last Source",
                                            "Provisioning Notes"};
    int ncol = verbose ? 15 : 6;
    const db_rec_t **rows = malloc((db->nrecs ? db->nrecs : 1) * sizeof *rows);
    size_t nrows = 0;
    for (size_t i = 0; i < db->nrecs; i++) rows[nrows++] = &db->recs[i];
    qsort(rows, nrows, sizeof *rows, by_time);
    /* minimum widths as in the PDF's example table */
    size_t w[NCOL_MAX] = {10, 19, 17, 14, 19, 16, 11, 10, 10, 11, 8, 15, 19, 21, 20};
    char (*cells)[NCOL_MAX][200] = malloc((nrows ? nrows : 1) * sizeof *cells);
    for (size_t r = 0; r < nrows; r++) {
        const db_rec_t *x = rows[r];
        bool ok = x->status == DB_SUCCESS;
        char t[24], ls[24];
        tm_str(x->reg_time_ms, t, false);
        tm_str(x->last_seen_ms, ls, false);
        snprintf(cells[r][0], 200, "%s", x->serial);
        snprintf(cells[r][1], 200, "%s", ok ? "Success" : "Failed");
        snprintf(cells[r][2], 200, "%s", ok && *x->fw_assigned ? x->fw_assigned : "—");
        if (ok && x->cfg_version) snprintf(cells[r][3], 200, "v%d", x->cfg_version);
        else snprintf(cells[r][3], 200, "—");
        snprintf(cells[r][4], 200, "%s", t);
        snprintf(cells[r][5], 200, "%s", *x->notes ? x->notes : "—");
        snprintf(cells[r][6], 200, "%s", *x->model ? x->model : "—");
        snprintf(cells[r][7], 200, "%s", *x->fw_initial ? x->fw_initial : "—");
        snprintf(cells[r][8], 200, "%s", *x->tenant ? x->tenant : "—");
        snprintf(cells[r][9], 200, "%s", db_stage_name(x->stage));
        snprintf(cells[r][10], 200, "%u", (unsigned)x->attempts);
        snprintf(cells[r][11], 200, "%u", (unsigned)x->failed_attempts);
        snprintf(cells[r][12], 200, "%s", ls);
        snprintf(cells[r][13], 200, "%s", *x->last_src ? x->last_src : "—");
        snprintf(cells[r][14], 200, "%s", *x->prov_notes ? x->prov_notes : "—");
        for (int c = 0; c < ncol; c++)
            if (dwidth(cells[r][c]) > w[c]) w[c] = dwidth(cells[r][c]);
    }
    for (int c = 0; c < ncol; c++)
        if (dwidth(H[c]) > w[c]) w[c] = dwidth(H[c]);
    fprintf(f, "Device Registration Details\n");
    rule(f, w, ncol);
    fputc('|', f);
    for (int c = 0; c < ncol; c++) cell(f, H[c], w[c]);
    fputc('\n', f);
    rule(f, w, ncol);
    for (size_t r = 0; r < nrows; r++) {
        fputc('|', f);
        for (int c = 0; c < ncol; c++) cell(f, cells[r][c], w[c]);
        fputc('\n', f);
    }
    rule(f, w, ncol);
    free(cells);
    free(rows);
}

void db_summary(const srv_db_t *db, FILE *f, const char *counters)
{
    const db_rec_t **rows = malloc((db->nrecs ? db->nrecs : 1) * sizeof *rows);
    size_t n = 0, ok = 0, failed = 0;
    for (size_t i = 0; i < db->nrecs; i++) rows[n++] = &db->recs[i];
    qsort(rows, n, sizeof *rows, by_time);
    fprintf(f, "Summary\n  Registered successfully : %zu  ", db_count(db, DB_SUCCESS));
    for (size_t i = 0; i < n; i++)
        if (rows[i]->status == DB_SUCCESS) fprintf(f, "%s%s", ok++ ? ", " : " ", rows[i]->serial);
    fprintf(f, "\n  Failed registration     : %zu  ", db_count(db, DB_FAILED));
    for (size_t i = 0; i < n && failed < 50; i++)
        if (rows[i]->status == DB_FAILED)
            fprintf(f, "%s%s (%s, %u attempt%s)", failed++ ? ", " : " ", rows[i]->serial, rows[i]->notes,
                    (unsigned)rows[i]->failed_attempts, rows[i]->failed_attempts == 1 ? "" : "s");
    if (db_count(db, DB_FAILED) > 50) fprintf(f, ", …");
    fprintf(f, "\n  Firmware / config per device:\n");
    for (size_t i = 0; i < n; i++) {
        const db_rec_t *x = rows[i];
        if (x->status != DB_SUCCESS) continue;
        int cmp = 0;
        bool same = version_cmp(x->fw_initial, x->fw_assigned, &cmp) == 0 && cmp >= 0;
        char fw[40];
        if (same) snprintf(fw, sizeof fw, "%s (current)", x->fw_initial);
        else snprintf(fw, sizeof fw, "%s -> %s", x->fw_initial, x->fw_assigned);
        fprintf(f, "    %-10s  %-11s  %-16s cfg v%d  %s%s%s\n", x->serial, x->model, fw, x->cfg_version, x->tenant,
                *x->notes ? "  " : "", x->notes);
    }
    if (counters) fprintf(f, "  Protocol counters: %s\n", counters);
    free(rows);
}
