/* Server registration DB (DESIGN §14.4, §14.5): verdict rules (§10.3), failed-row cap
 * (TC-SRV-11 logic), journal replay and compaction (TC-SRV-06 logic), dump format (TC-SRV-03). */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "protocol.h"
#include "test.h"

static char path[400];

static void reopen(srv_db_t *db, bool compact, int cap)
{
    db_close(db, compact);
    char err[200];
    CHECK_EQ_INT(db_open(db, path, cap, err, sizeof err), 0);
}

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "r");
    if (!f) return calloc(1, 1);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = calloc(1, (size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) b[0] = '\0';
    fclose(f);
    return b;
}

static void test_verdicts(void)
{
    snprintf(path, sizeof path, "%s/db1/db.jsonl", test_tmpdir());
    srv_db_t db;
    char err[200];
    CHECK_EQ_INT(db_open(&db, path, 10000, err, sizeof err), 0);
    uint64_t t0 = (uint64_t)time(NULL) * 1000u - 3600u * 1000u;   /* an hour ago: within the 24 h kept for deletions */
    db_register_ok(&db, "ZTP-93AF21", "EDGE-GW-100", "A1", "v1.0", "v1.1", 3, "cruise_ops", "", "127.0.0.1:1", t0);
    db_rec_t *r = db_get(&db, "ZTP-93AF21");
    CHECK(r && r->status == DB_SUCCESS && r->attempts == 1 && r->stage == SST_REGISTERED);
    /* a failure for a serial with a Success row never overwrites it (§10.3) */
    db_register_fail(&db, "ZTP-93AF21", "EDGE-GW-100", "Invalid HMAC", "10.0.0.9:2", t0 + 5);
    r = db_get(&db, "ZTP-93AF21");
    CHECK(r->status == DB_SUCCESS && r->failed_attempts == 1 && r->notes[0] == '\0' && r->attempts == 1);
    /* re-registration: same row, attempts + 1, first time kept, provisioning note */
    db_register_ok(&db, "ZTP-93AF21", "EDGE-GW-100", "A1", "v1.0", "v1.1", 3, "cruise_ops", "", "127.0.0.1:1", t0 + 9000);
    r = db_get(&db, "ZTP-93AF21");
    CHECK(r->attempts == 2 && r->reg_time_ms == t0 && strstr(r->prov_notes, "Re-registered"));
    /* failed device, then succeeds later: becomes Success */
    db_register_fail(&db, "ZTP-84BC19", "EDGE-GW-100", "Invalid HMAC", NULL, t0 + 1);
    db_register_fail(&db, "ZTP-84BC19", "EDGE-GW-100", "Invalid HMAC", NULL, t0 + 2);
    r = db_get(&db, "ZTP-84BC19");
    CHECK(r->status == DB_FAILED && r->failed_attempts == 2 && !strcmp(r->notes, "Invalid HMAC"));
    CHECK_EQ_INT(db_failed_only(&db), 1);
    /* stages are monotonic, except FAILED */
    db_stage(&db, "ZTP-93AF21", SST_CONFIG_SENT, t0 + 10);
    db_stage(&db, "ZTP-93AF21", SST_REGISTERED, t0 + 11);
    CHECK_EQ_INT(db_get(&db, "ZTP-93AF21")->stage, SST_CONFIG_SENT);
    db_stage(&db, "ZTP-93AF21", SST_FAILED, t0 + 12);
    CHECK_EQ_INT(db_get(&db, "ZTP-93AF21")->stage, SST_FAILED);
    /* sessions */
    db_sess_t s = {.sid = 7421, .epoch = 0, .last_resume_ctr = 1, .max_dgram = 1200, .keepalive_s = 2, .grace_s = 30};
    snprintf(s.serial, sizeof s.serial, "ZTP-93AF21");
    snprintf(s.state, sizeof s.state, "REGISTERED");
    db_session(&db, &s, t0 + 20);
    db_sess_t s2 = s;
    s2.sid = 9930;
    db_session(&db, &s2, t0 + 21);
    db_session_deleted(&db, 9930, "ZTP-93AF21", t0 + 22);
    db_sess_t s3 = s;   /* deleted more than 24 h ago: dropped at the next open (the journal does not grow forever) */
    s3.sid = 5555;
    db_session(&db, &s3, t0 - 30u * 3600u * 1000u);
    db_session_deleted(&db, 5555, "ZTP-93AF21", t0 - 30u * 3600u * 1000u + 1);

    /* replay restores everything */
    reopen(&db, false, 10000);
    r = db_get(&db, "ZTP-93AF21");
    CHECK(r && r->status == DB_SUCCESS && r->attempts == 2 && r->failed_attempts == 1 && r->reg_time_ms == t0);
    CHECK(r && r->stage == SST_FAILED);
    r = db_get(&db, "ZTP-84BC19");
    CHECK(r && r->status == DB_FAILED && r->failed_attempts == 2);
    size_t ns;
    const db_sess_t *ss = db_sessions(&db, &ns);
    CHECK_EQ_INT(ns, 2);
    int live = 0, dead = 0;
    for (size_t i = 0; i < ns; i++) {
        if (ss[i].sid == 7421 && !ss[i].deleted && ss[i].keepalive_s == 2) live++;
        if (ss[i].sid == 9930 && ss[i].deleted) dead++;
    }
    CHECK(live == 1 && dead == 1);

    /* compaction rewrites the journal and replays to the same state */
    reopen(&db, true, 10000);
    char *j = slurp(path);
    CHECK(strstr(j, "\"ev\":\"RECORD\"") && !strstr(j, "\"ev\":\"REGISTER_OK\""));
    free(j);
    r = db_get(&db, "ZTP-93AF21");
    CHECK(r && r->attempts == 2 && r->failed_attempts == 1 && strstr(r->prov_notes, "Re-registered"));
    CHECK_EQ_INT(db_failed_only(&db), 1);

    /* a torn last line (crash mid-write) is skipped */
    FILE *f = fopen(path, "a");
    fputs("{\"ts\":\"2026-11-10T14:3", f);
    fclose(f);
    reopen(&db, false, 10000);
    CHECK(db_get(&db, "ZTP-84BC19") != NULL);
    /* the next event after a torn line starts on its own line, so it survives the next replay (it would
     * otherwise be appended to the torn fragment and lost with it) */
    db_register_ok(&db, "ZTP-AAAA01", "EDGE-GW-100", "A1", "v1.0", "v1.1", 3, "t", "", NULL, t0 + 40);
    reopen(&db, false, 10000);
    CHECK(db_get(&db, "ZTP-AAAA01") != NULL);
    /* bytes outside printable ASCII are escaped, so the line still parses on replay and after compaction */
    db_register_ok(&db, "ZTP-AAAA02", "EDGE-GW-100", "\xe9\x01", "v1.0", "v1.1", 3, "t", "", NULL, t0 + 41);
    reopen(&db, false, 10000);
    CHECK(db_get(&db, "ZTP-AAAA02") != NULL);
    reopen(&db, true, 10000);
    CHECK(db_get(&db, "ZTP-AAAA02") != NULL && db_get(&db, "ZTP-AAAA01") != NULL);
    db_close(&db, false);
}

static void test_cap(void)
{
    snprintf(path, sizeof path, "%s/db2/db.jsonl", test_tmpdir());
    srv_db_t db;
    char err[200];
    CHECK_EQ_INT(db_open(&db, path, 100, err, sizeof err), 0);
    db_register_ok(&db, "ZTP-000001", "EDGE-GW-100", "A1", "v1.0", "v1.1", 3, "t", "", NULL, 1);
    char serial[17];
    for (int i = 0; i < 1000; i++) {
        snprintf(serial, sizeof serial, "ZTP-F%05X", (unsigned)i);
        db_register_fail(&db, serial, "EDGE-GW-100", "Invalid HMAC", NULL, (uint64_t)(10 + i));
        if (db_failed_only(&db) > 100) CHECK(!"cap exceeded");
    }
    /* a failure against the Success row in the middle of it all */
    db_register_fail(&db, "ZTP-000001", "EDGE-GW-100", "Invalid HMAC", NULL, 5000);
    CHECK_EQ_INT(db_failed_only(&db), 100);
    CHECK(db_get(&db, "ZTP-000001") && db_get(&db, "ZTP-000001")->status == DB_SUCCESS);   /* never evicted */
    CHECK(db_get(&db, "ZTP-F00000") == NULL);                                               /* oldest evicted */
    CHECK(db_get(&db, "ZTP-F003E7") != NULL);                                               /* newest kept */
    CHECK_EQ_INT(db_count(&db, DB_FAILED), 100);
    /* every remaining row is still findable through the index */
    int lost = 0;
    for (int i = 900; i < 1000; i++) {
        snprintf(serial, sizeof serial, "ZTP-F%05X", (unsigned)i);
        lost += db_get(&db, serial) == NULL;
    }
    CHECK_EQ_INT(lost, 0);
    db_close(&db, false);
}

static void test_dump(void)
{
    snprintf(path, sizeof path, "%s/db3/db.jsonl", test_tmpdir());
    srv_db_t db;
    char err[200];
    CHECK_EQ_INT(db_open(&db, path, 10000, err, sizeof err), 0);
    uint64_t t = 1762785130000ull;   /* 2025-11-10 14:32:10 UTC */
    db_register_ok(&db, "ZTP-93AF21", "EDGE-GW-100", "A1", "v1.0", "v1.1", 3, "cruise_ops", "", NULL, t + 100);
    db_register_fail(&db, "ZTP-84BC19", "EDGE-GW-100", "Invalid HMAC", NULL, t + 200);
    db_register_ok(&db, "ZTP-19FE77", "EDGE-GW-050", "A1", "v0.9", "v1.0", 2, "cruise_ops", "Legacy device", NULL, t + 1300);
    char out[300];
    snprintf(out, sizeof out, "%s/dump.txt", test_tmpdir());
    FILE *f = fopen(out, "w");
    db_dump(&db, f, false);
    db_summary(&db, f, "rx=1 tx=2");
    fclose(f);
    char *d = slurp(out);
    const char *head = "Device Registration Details\n+------------+---------------------+";
    CHECK(strncmp(d, head, strlen(head)) == 0);
    CHECK(strstr(d, "| Device ID  | Registration Status | Firmware Assigned | Config Version | Registration Time   | Notes            |"));
    CHECK(strstr(d, "| ZTP-93AF21 | Success             | v1.1              | v3             | 2025-11-10 14:32:10 | —                |"));
    CHECK(strstr(d, "| ZTP-84BC19 | Failed              | —                 | —              | 2025-11-10 14:32:10 | Invalid HMAC     |"));
    CHECK(strstr(d, "| ZTP-19FE77 | Success             | v1.0              | v2             | 2025-11-10 14:32:11 | Legacy device    |"));
    /* sorted by time: 93AF21 (…10.100) before 84BC19 (…10.200) before 19FE77 */
    CHECK(strstr(d, "ZTP-93AF21 | Success") < strstr(d, "ZTP-84BC19 | Failed"));
    CHECK(strstr(d, "Registered successfully : 2") && strstr(d, "Failed registration     : 1"));
    CHECK(strstr(d, "ZTP-84BC19 (Invalid HMAC, 1 attempt)") && strstr(d, "v0.9 -> v1.0"));
    CHECK(strstr(d, "Protocol counters: rx=1 tx=2"));
    free(d);
    f = fopen(out, "w");
    db_dump(&db, f, true);
    fclose(f);
    d = slurp(out);
    CHECK(strstr(d, "| Model ") && strstr(d, "| Provisioning Notes ") && strstr(d, "EDGE-GW-050"));
    free(d);
    db_close(&db, false);
}

void suite_db(void)
{
    test_verdicts();
    test_cap();
    test_dump();
}
