/* Registration database (DESIGN §14.4) and dump/summary (§14.5).
 * In memory, one record per serial, created only at a verdict; every change is appended to an
 * append-only JSONL journal (fsync on registration and session events), replayed at startup and
 * compacted on clean shutdown. */
#ifndef SRV_DB_H
#define SRV_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { DB_NONE = 0, DB_SUCCESS = 1, DB_FAILED = 2 };

typedef struct {
    bool     used, ever_success;
    char     serial[17], model[17], hw_rev[9];
    int      status;
    uint64_t reg_time_ms;            /* first success, or first attempt if it never succeeded */
    uint64_t last_seen_ms, last_failure_ms;
    char     fw_assigned[9], fw_initial[9], tenant[33];
    int      cfg_version;
    int      stage;                  /* SRV_STAGE (§5.4.3) */
    uint32_t attempts, failed_attempts;
    char     last_src[24];
    char     notes[48];              /* default dump: outcome and policy notes */
    char     prov_notes[192];        /* verbose dump only */
} db_rec_t;

/* A journaled, resumable session (no keys, §14.4). */
typedef struct {
    uint32_t sid, last_resume_ctr;
    uint16_t epoch, max_dgram;
    char     serial[17], model[17], state[16], peer[24];
    int      keepalive_s, grace_s;
    bool     deleted;
    uint64_t ts_ms;
} db_sess_t;

typedef struct {
    db_rec_t  *recs;
    size_t     nrecs, cap_recs;
    uint32_t  *index;               /* open addressing over record indices + 1 (0 = empty) */
    size_t     index_cap;
    uint32_t  *failed_fifo;         /* indices of failed-only rows, oldest first */
    size_t     fifo_head, fifo_len, fifo_cap;
    size_t     nfailed_only;
    int        max_failed;
    db_sess_t *sess;
    size_t     nsess, cap_sess;
    FILE      *journal;
    char       path[256];
    bool       replaying;
    uint64_t   evicted;
} srv_db_t;

/* Open (and replay) the journal at path; creates its directory. 0, or -1 with err. */
int  db_open(srv_db_t *db, const char *path, int max_failed, char *err, size_t errsz);
void db_close(srv_db_t *db, bool compact);

db_rec_t *db_get(srv_db_t *db, const char *serial);
/* Verdicts (§10.3). Returns the record. */
db_rec_t *db_register_ok(srv_db_t *db, const char *serial, const char *model, const char *hw_rev,
                         const char *fw_initial, const char *fw_assigned, int cfg, const char *tenant,
                         const char *notes, const char *src, uint64_t now_ms);
db_rec_t *db_register_fail(srv_db_t *db, const char *serial, const char *model, const char *notes,
                           const char *src, uint64_t now_ms);
void db_stage(srv_db_t *db, const char *serial, int stage, uint64_t now_ms);   /* monotonic except FAILED */
void db_prov_note(srv_db_t *db, const char *serial, const char *note, uint64_t now_ms);
void db_seen(srv_db_t *db, const char *serial, const char *src, uint64_t now_ms);

/* Session journal (fsynced). */
void db_session(srv_db_t *db, const db_sess_t *s, uint64_t now_ms);
void db_session_deleted(srv_db_t *db, uint32_t sid, const char *serial, uint64_t now_ms);
const db_sess_t *db_sessions(const srv_db_t *db, size_t *n);   /* latest state per sid */

size_t db_failed_only(const srv_db_t *db);
size_t db_count(const srv_db_t *db, int status);

/* §14.5: the PDF-format table (and the verbose variant), then the summary. */
void db_dump(const srv_db_t *db, FILE *out, bool verbose);
void db_summary(const srv_db_t *db, FILE *out, const char *counters_line);

const char *db_stage_name(int stage);

#endif
