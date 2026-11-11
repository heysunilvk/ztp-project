/* TC-UNIT-11 (policy parser and precedence), TC-UNIT-20 (policy tenant < model < serial NAT
 * merge, and validation of every resolved result), and the loader logic behind TC-CFG-09
 * (keys.json mode 0600, a model without a key, missing artifacts). Runs from the repo root. */
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth.h"
#include "dev.h"
#include "config.h"
#include "policy.h"
#include "test.h"

static char tmp_path[400];

static const char *tmpfile_with(const char *name, const char *text)
{
    snprintf(tmp_path, sizeof tmp_path, "%s/%s", test_tmpdir(), name);
    FILE *f = fopen(tmp_path, "w");
    if (!f) { perror(tmp_path); exit(2); }
    fputs(text, f);
    fclose(f);
    return tmp_path;
}

static int load_text(const char *text, srv_policy_t *p, char *err, size_t n)
{
    return srv_policy_load(tmpfile_with("policy.json", text), p, err, n);
}

/* Server config from conf/server.json, optionally with a profile. */
static void server_cfg(cfg_t *c, const char *profile)
{
    cfg_args_t a = {0};
    a.profile = profile;
    fflush(stderr);
    int saved = dup(2);
    FILE *null = fopen("/dev/null", "w");
    dup2(fileno(null), 2);
    int rc = cfg_load(c, CFG_SERVER, &a);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    fclose(null);
    CHECK_EQ_INT(rc, 0);
}

static const char *const MODELS =
    "\"models\": {"
    "  \"EDGE-GW-100\": {\"firmware\": \"v1.1\", \"firmware_file\": \"fw100\", \"config_version\": 3,"
    "                   \"config_file\": \"cfg3\", \"tenant\": \"cruise_ops\"},"
    "  \"EDGE-GW-050\": {\"firmware\": \"v1.0\", \"firmware_file\": \"fw050\", \"config_version\": 2,"
    "                   \"config_file\": \"cfg2\", \"tenant\": \"cruise_ops\", \"notes\": \"Legacy device\"}}";

static void test_shipped_policy(void)
{
    srv_policy_t p;
    char err[300];
    CHECK_EQ_INT(srv_policy_load("conf/policy.json", &p, err, sizeof err), 0);
    cfg_t c;
    server_cfg(&c, NULL);
    srv_assignment_t a;
    srv_policy_resolve(&p, &c, "EDGE-GW-100", "ZTP-93AF21", &a);
    CHECK(a.assigned);
    CHECK_STR(a.firmware, "v1.1");
    CHECK_EQ_INT(a.config_version, 3);
    CHECK_STR(a.tenant, "cruise_ops");
    CHECK_STR(a.config_file, "artifacts/cfg_v3.json");
    CHECK_EQ_INT(a.nat.keepalive_interval_s, 20);
    CHECK_STR(a.src_keepalive, "file:conf/server.json");   /* no nat blocks: the server's value */
    srv_policy_resolve(&p, &c, "EDGE-GW-050", "ZTP-19FE77", &a);
    CHECK(a.assigned && !strcmp(a.firmware, "v1.0") && a.config_version == 2);
    CHECK_STR(a.notes, "Legacy device");
    srv_policy_resolve(&p, &c, "EDGE-GW-200", "ZTP-000001", &a);
    CHECK(!a.assigned);                                    /* default reject -> NO_POLICY */
    CHECK_EQ_INT(srv_policy_validate(&p, &c, NULL, err, sizeof err), 0);
    cfg_free(&c);

    /* with --profile demo the profile's NAT values reach every device */
    server_cfg(&c, "demo");
    srv_policy_resolve(&p, &c, "EDGE-GW-100", "ZTP-93AF21", &a);
    CHECK_EQ_INT(a.nat.keepalive_interval_s, 2);
    CHECK_EQ_INT(a.nat.grace_s, 30);
    CHECK_STR(a.src_keepalive, "profile:demo");
    cfg_free(&c);
    srv_policy_free(&p);
}

static void test_precedence(void)
{
    /* TC-UNIT-11: serial > model > default */
    char text[2048], err[300];
    snprintf(text, sizeof text,
             "{\"schema_version\": 1,"
             " \"default\": {\"action\": \"assign\", \"firmware\": \"v0.1\", \"firmware_file\": \"fw0\","
             "               \"config_version\": 1, \"config_file\": \"cfg1\", \"tenant\": \"lab\"},"
             " \"tenants\": {\"cruise_ops\": {}, \"lab\": {}}, %s,"
             " \"serials\": {\"ZTP-55AC90\": {\"firmware\": \"v1.2\", \"firmware_file\": \"fw12\"},"
             "               \"ZTP-000AAA\": {\"tenant\": \"lab\", \"notes\": \"bench unit\"}}}",
             MODELS);
    srv_policy_t p;
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    cfg_t c;
    server_cfg(&c, NULL);
    srv_assignment_t a;
    srv_policy_resolve(&p, &c, "EDGE-GW-100", "ZTP-55AC90", &a);
    CHECK(a.assigned && !strcmp(a.firmware, "v1.2") && !strcmp(a.firmware_file, "fw12"));
    CHECK(a.config_version == 3 && !strcmp(a.config_file, "cfg3"));   /* rest from the model */
    srv_policy_resolve(&p, &c, "EDGE-GW-100", "ZTP-000AAA", &a);
    CHECK(!strcmp(a.tenant, "lab") && !strcmp(a.firmware, "v1.1") && !strcmp(a.notes, "bench unit"));
    srv_policy_resolve(&p, &c, "EDGE-GW-999", "ZTP-111111", &a);
    CHECK(a.assigned && !strcmp(a.firmware, "v0.1") && !strcmp(a.tenant, "lab"));   /* default assign */
    srv_policy_resolve(&p, &c, "EDGE-GW-999", "ZTP-55AC90", &a);
    CHECK(a.assigned && !strcmp(a.firmware, "v1.2") && a.config_version == 1);      /* serial over default */
    CHECK_EQ_INT(srv_policy_validate(&p, &c, NULL, err, sizeof err), 0);
    cfg_free(&c);
    srv_policy_free(&p);
}

static void test_nat_override(void)
{
    /* TC-UNIT-20 / TC-CFG-06 logic: tenant < model < serial, with source labels */
    srv_policy_t p;
    char err[300];
    CHECK_EQ_INT(srv_policy_load("tests/conf/policy_override.json", &p, err, sizeof err), 0);
    cfg_t c;
    server_cfg(&c, NULL);
    srv_assignment_t a;
    srv_policy_resolve(&p, &c, "EDGE-GW-100", "ZTP-55AC90", &a);
    CHECK_EQ_INT(a.nat.keepalive_interval_s, 10);
    CHECK_STR(a.src_keepalive, "policy:serial ZTP-55AC90");
    CHECK_EQ_INT(a.nat.grace_s, 180);
    CHECK_STR(a.src_grace, "policy:tenant cruise_ops");
    srv_policy_resolve(&p, &c, "EDGE-GW-100", "ZTP-93AF21", &a);
    CHECK_EQ_INT(a.nat.keepalive_interval_s, 20);
    CHECK_STR(a.src_keepalive, "file:conf/server.json");
    CHECK_EQ_INT(a.nat.grace_s, 180);
    CHECK_EQ_INT(srv_policy_validate(&p, &c, NULL, err, sizeof err), 0);
    srv_policy_free(&p);

    /* model beats tenant, serial beats model */
    char text[2048];
    snprintf(text, sizeof text,
             "{\"default\": {\"action\": \"reject\"},"
             " \"tenants\": {\"cruise_ops\": {\"nat\": {\"keepalive_interval_s\": 15, \"grace_s\": 300}}},"
             " \"models\": {\"EDGE-GW-100\": {\"firmware\": \"v1.1\", \"firmware_file\": \"f\", \"config_version\": 3,"
             "   \"config_file\": \"c\", \"tenant\": \"cruise_ops\", \"nat\": {\"keepalive_interval_s\": 12, \"allow_ip_change\": false}}},"
             " \"serials\": {\"ZTP-55AC90\": {\"nat\": {\"keepalive_interval_s\": 10, \"dead_intervals\": 2}}}}");
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    srv_policy_resolve(&p, &c, "EDGE-GW-100", "ZTP-93AF21", &a);
    CHECK(a.nat.keepalive_interval_s == 12 && a.nat.grace_s == 300 && !a.nat.allow_ip_change);
    CHECK_STR(a.src_keepalive, "policy:model EDGE-GW-100");
    CHECK_STR(a.src_allow_ip_change, "policy:model EDGE-GW-100");
    srv_policy_resolve(&p, &c, "EDGE-GW-100", "ZTP-55AC90", &a);
    CHECK(a.nat.keepalive_interval_s == 10 && a.nat.dead_intervals == 2 && a.nat.grace_s == 300);
    CHECK_STR(a.src_dead, "policy:serial ZTP-55AC90");
    CHECK_EQ_INT(srv_policy_validate(&p, &c, NULL, err, sizeof err), 0);
    srv_policy_free(&p);

    /* a serial override of 900 s is rejected at startup, before any device sees it (exit 78) */
    snprintf(text, sizeof text,
             "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"cruise_ops\": {}}, %s,"
             " \"serials\": {\"ZTP-55AC90\": {\"nat\": {\"keepalive_interval_s\": 900}}}}", MODELS);
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    CHECK_EQ_INT(srv_policy_validate(&p, &c, NULL, err, sizeof err), EX_CONFIG_CODE);
    CHECK(strstr(err, "nat.keepalive_interval_s=900") && strstr(err, "policy:serial ZTP-55AC90"));
    srv_policy_free(&p);

    /* a merge that breaks grace >= 2 x keepalive x idle is caught too (25 s with grace 120) */
    snprintf(text, sizeof text,
             "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"cruise_ops\": {}}, %s,"
             " \"serials\": {\"ZTP-55AC90\": {\"nat\": {\"keepalive_interval_s\": 25}}}}", MODELS);
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    CHECK_EQ_INT(srv_policy_validate(&p, &c, NULL, err, sizeof err), EX_CONFIG_CODE);
    CHECK(strstr(err, "nat.grace_s") != NULL);
    srv_policy_free(&p);

    /* an undefined tenant is rejected */
    snprintf(text, sizeof text, "{\"default\": {\"action\": \"reject\"}, \"tenants\": {}, %s}", MODELS);
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    CHECK_EQ_INT(srv_policy_validate(&p, &c, NULL, err, sizeof err), EX_CONFIG_CODE);
    CHECK(strstr(err, "tenant \"cruise_ops\" is not defined") != NULL);
    srv_policy_free(&p);
    cfg_free(&c);
}

static void test_malformed(void)
{
    static const char *const bad[] = {
        "{\"default\": {\"action\": \"reject\"},}",                                          /* JSON */
        "{\"tenants\": {}}",                                                                 /* no default */
        "{\"default\": {\"action\": \"drop\"}}",
        "{\"default\": {\"action\": \"assign\"}}",                                           /* incomplete */
        "{\"default\": {\"action\": \"reject\"}, \"modles\": {}}",                           /* typo */
        "{\"default\": {\"action\": \"reject\"}, \"models\": {\"M\": {\"firmware\": \"v1.1\"}}}",
        "{\"default\": {\"action\": \"reject\"}, \"models\": {\"M\": {\"firmware\": \"1.1\", \"firmware_file\": \"f\","
        " \"config_version\": 3, \"config_file\": \"c\", \"tenant\": \"t\"}}}",            /* bad version */
        "{\"default\": {\"action\": \"reject\"}, \"models\": {\"M\": {\"firmware\": \"v1.1\", \"firmware_file\": \"f\","
        " \"config_version\": \"3\", \"config_file\": \"c\", \"tenant\": \"t\"}}}",        /* type */
        "{\"default\": {\"action\": \"reject\"}, \"serials\": {\"ZTP-93af21\": {}}}",          /* serial format */
        "{\"default\": {\"action\": \"reject\"}, \"serials\": {\"ZTP-93AF21\": {\"colour\": 1}}}",
        "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"t\": {\"firmware\": \"v1\"}}}",
        "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"t\": {\"nat\": {\"keepalive\": 5}}}}",
        "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"t\": {\"nat\": {\"grace_s\": \"x\"}}}}",
        "{\"schema_version\": 2, \"default\": {\"action\": \"reject\"}}",
        "[]",
    };
    char err[300];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        srv_policy_t p;
        int rc = load_text(bad[i], &p, err, sizeof err);
        if (rc != EX_CONFIG_CODE) {
            fprintf(stderr, "  accepted bad policy #%zu: %s\n", i, bad[i]);
            CHECK(!"malformed policy accepted");
            if (rc == 0) srv_policy_free(&p);
        }
        CHECK(strncmp(err, "policy: ", 8) == 0);
    }
    srv_policy_t p;
    CHECK_EQ_INT(srv_policy_load("/nonexistent/policy.json", &p, err, sizeof err), EX_CONFIG_CODE);
}

static void test_files_and_keys(void)
{
    char err[300], fw[400], cfgp[400], keys[400], text[2048];
    snprintf(fw, sizeof fw, "%s", tmpfile_with("fw.bin", "ZTPFW firmware bytes"));
    snprintf(cfgp, sizeof cfgp, "%s", tmpfile_with("cfg.json", "{\"config_version\": 3}"));
    snprintf(text, sizeof text,
             "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"cruise_ops\": {}}, \"models\": {"
             " \"EDGE-GW-100\": {\"firmware\": \"v1.1\", \"firmware_file\": \"%s\", \"config_version\": 3,"
             " \"config_file\": \"%s\", \"tenant\": \"cruise_ops\"}}}", fw, cfgp);
    srv_policy_t p;
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    CHECK_EQ_INT(srv_policy_check_files(&p, err, sizeof err), 0);
    srv_policy_free(&p);
    /* missing firmware_file */
    snprintf(text, sizeof text,
             "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"cruise_ops\": {}}, \"models\": {"
             " \"EDGE-GW-100\": {\"firmware\": \"v1.1\", \"firmware_file\": \"/nonexistent/fw.bin\", \"config_version\": 3,"
             " \"config_file\": \"%s\", \"tenant\": \"cruise_ops\"}}}", cfgp);
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    CHECK_EQ_INT(srv_policy_check_files(&p, err, sizeof err), EX_CONFIG_CODE);
    CHECK(strstr(err, "firmware_file") && strstr(err, "/nonexistent/fw.bin"));
    srv_policy_free(&p);
    /* config_file that is not JSON */
    tmpfile_with("cfg.json", "not json");
    snprintf(text, sizeof text,
             "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"cruise_ops\": {}}, \"models\": {"
             " \"EDGE-GW-100\": {\"firmware\": \"v1.1\", \"firmware_file\": \"%s\", \"config_version\": 3,"
             " \"config_file\": \"%s\", \"tenant\": \"cruise_ops\"}}}", fw, cfgp);
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    CHECK_EQ_INT(srv_policy_check_files(&p, err, sizeof err), EX_CONFIG_CODE);
    CHECK(strstr(err, "not a valid JSON object") != NULL);
    srv_policy_free(&p);

    /* keys.json: 0600 loads; 0644 (keys_world_readable) refused; lookup; model without key */
    char *ktext = NULL;
    FILE *f = fopen("tests/conf/keys_test.json", "r");
    CHECK(f != NULL);
    if (!f) return;
    char kbuf[4096];
    size_t kn = fread(kbuf, 1, sizeof kbuf - 1, f);
    fclose(f);
    kbuf[kn] = '\0';
    ktext = kbuf;
    snprintf(keys, sizeof keys, "%s", tmpfile_with("keys.json", ktext));
    chmod(keys, 0600);
    srv_keys_t k;
    CHECK_EQ_INT(srv_keys_load(keys, &k, err, sizeof err), 0);
    CHECK_EQ_INT(k.nmodels, 2);
    const uint8_t *km = srv_keys_model(&k, "EDGE-GW-100");
    CHECK(km && km[0] == 0x20 && km[31] == 0x3f);
    CHECK(srv_keys_model(&k, "EDGE-GW-999") == NULL);   /* UNKNOWN_MODEL */
    uint8_t kb[32];
    ztp_derive_kboot(k.k_fleet, kb);
    CHECK(memcmp(kb, k.k_boot, 32) == 0);

    cfg_t c;
    server_cfg(&c, NULL);
    CHECK_EQ_INT(srv_policy_load("conf/policy.json", &p, err, sizeof err), 0);
    CHECK_EQ_INT(srv_policy_validate(&p, &c, &k, err, sizeof err), 0);
    srv_policy_free(&p);
    snprintf(text, sizeof text,
             "{\"default\": {\"action\": \"reject\"}, \"tenants\": {\"cruise_ops\": {}}, \"models\": {"
             " \"EDGE-GW-200\": {\"firmware\": \"v2.0\", \"firmware_file\": \"f\", \"config_version\": 1,"
             " \"config_file\": \"c\", \"tenant\": \"cruise_ops\"}}}");
    CHECK_EQ_INT(load_text(text, &p, err, sizeof err), 0);
    CHECK_EQ_INT(srv_policy_validate(&p, &c, &k, err, sizeof err), EX_CONFIG_CODE);
    CHECK(strstr(err, "model EDGE-GW-200 has no key") != NULL);
    srv_policy_free(&p);
    cfg_free(&c);
    srv_keys_free(&k);

    chmod(keys, 0644);
    CHECK_EQ_INT(srv_keys_load(keys, &k, err, sizeof err), EX_CONFIG_CODE);
    CHECK(strstr(err, "must be mode 0600 (is 0644)") != NULL);
    chmod(keys, 0600);
    tmpfile_with("keys.json", "{\"fleet\": \"00\", \"models\": {}}");
    CHECK_EQ_INT(srv_keys_load(keys, &k, err, sizeof err), EX_CONFIG_CODE);
    tmpfile_with("keys.json", "{\"models\": {}}");
    CHECK_EQ_INT(srv_keys_load(keys, &k, err, sizeof err), EX_CONFIG_CODE);
    CHECK(strstr(err, "missing fleet") != NULL);
    tmpfile_with("keys.json", "{\"fleet\": \"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\", \"extra\": 1}");
    CHECK_EQ_INT(srv_keys_load(keys, &k, err, sizeof err), EX_CONFIG_CODE);
    CHECK_EQ_INT(srv_keys_load("/nonexistent/keys.json", &k, err, sizeof err), EX_CONFIG_CODE);
}

static void test_demo_keys_match(void)
{
    /* conf/keys.json must hold exactly the keys the device derives "in its firmware" (device/keys.c) */
    char err[300];
    srv_keys_t k;
    chmod("conf/keys.json", 0600);
    CHECK_EQ_INT(srv_keys_load("conf/keys.json", &k, err, sizeof err), 0);
    const char *models[] = {"EDGE-GW-100", "EDGE-GW-050"};
    for (int i = 0; i < 2; i++) {
        uint8_t kf[32], km[32];
        dev_factory_keys(models[i], kf, km);
        CHECK(memcmp(kf, k.k_fleet, 32) == 0);
        const uint8_t *s = srv_keys_model(&k, models[i]);
        CHECK(s && memcmp(km, s, 32) == 0);
    }
    srv_keys_free(&k);
}

void suite_policy(void)
{
    test_demo_keys_match();
    test_shipped_policy();
    test_precedence();
    test_nat_override();
    test_malformed();
    test_files_and_keys();
}
