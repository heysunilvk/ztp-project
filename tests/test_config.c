/* TC-UNIT-20 (precedence and policy-NAT validation), plus the loader parts of TC-CFG-01..05/07:
 * defaults match Appendix B.2, file/profile/CLI layering with sources, range and cross-field
 * rules (exit 78), unknown keys strict vs lenient. */
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "json.h"
#include "test.h"

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static char dir[300], srv_path[400], dev_path[400];

static void setup(void)
{
    snprintf(dir, sizeof dir, "%s/conf", test_tmpdir());
    mkdir(dir, 0700);
    char p[400];
    snprintf(p, sizeof p, "%s/profiles", dir); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/profiles/server", dir); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/profiles/device", dir); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/profiles/server/demo.json", dir);
    write_text(p, "{\"nat\": {\"keepalive_interval_s\": 2, \"grace_s\": 30}}");
    snprintf(p, sizeof p, "%s/profiles/device/demo.json", dir);
    write_text(p, "{\"nat\": {\"keepalive_interval_s\": 2, \"keepalive_min_s\": 1}, \"run\": {\"exit_when_operational\": true}}");
    snprintf(p, sizeof p, "%s/profiles/server/typo.json", dir);
    write_text(p, "{\"nat\": {\"keepalive_intervl_s\": 2}}");
    snprintf(srv_path, sizeof srv_path, "%s/server.json", dir);
    snprintf(dev_path, sizeof dev_path, "%s/device.json", dir);
}

/* Silence the loader's stderr while exercising error paths. */
static int quiet_load(cfg_t *c, cfg_side_t side, const cfg_args_t *a)
{
    fflush(stderr);
    int saved = dup(2);
    FILE *null = fopen("/dev/null", "w");
    dup2(fileno(null), 2);
    int rc = cfg_load(c, side, a);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    fclose(null);
    return rc;
}

static void set(cfg_args_t *a, const char *k, const char *v)
{
    snprintf(a->sets[a->nsets].key, sizeof a->sets[0].key, "%s", k);
    snprintf(a->sets[a->nsets].value, sizeof a->sets[0].value, "%s", v);
    a->nsets++;
}

static void test_defaults(void)
{
    cfg_args_t a = {0};
    a.config_path = NULL;
    cfg_t c;
    /* default path conf/server.json relative to cwd may exist; point at a missing optional file
     * by using an explicit empty dir: emulate "no config file" by chdir into tmp. */
    char cwd[1024];
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    CHECK(chdir(test_tmpdir()) == 0);
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 0);
    CHECK_EQ_INT(c.c.nat.keepalive_interval_s, 20);
    CHECK_EQ_INT(c.c.nat.grace_s, 120);
    CHECK_EQ_INT(c.c.rel.tx_window, 8);
    CHECK_EQ_INT(c.c.rel.rto_initial_ms, 500);
    CHECK_EQ_INT(c.c.xfer.chunk_size, 1024);
    CHECK_EQ_INT(c.c.net.max_datagram, 1200);
    CHECK_EQ_INT(c.c.limits.max_half_open, 256);
    CHECK_STR(c.c.paths.log, "logs/server.log");
    CHECK_STR(cfg_source(&c, "nat.grace_s"), "default");
    CHECK_STR(c.file, "");
    cfg_free(&c);
    CHECK_EQ_INT(quiet_load(&c, CFG_DEVICE, &a), 0);
    CHECK_STR(c.c.paths.log, "logs/device_%s.log");
    CHECK_STR(c.c.identity.model, "EDGE-GW-100");
    CHECK(!c.c.identity.serial_set);
    CHECK(c.c.net.reconnect_min_s == 1.0 && c.c.net.reconnect_max_s == 60.0);
    CHECK_EQ_INT(c.c.discovery.attempts, 3);
    CHECK_STR(c.c.discovery.mode, "mcast");
    CHECK(cfg_source(&c, "nat.grace_s") == NULL);   /* server-only key */
    cfg_free(&c);
    CHECK(chdir(cwd) == 0);
}

static void test_precedence(void)
{
    /* TC-CFG-03 / TC-UNIT-20: defaults < file < profile < CLI */
    write_text(srv_path, "{\"schema_version\": 1, \"nat\": {\"keepalive_interval_s\": 5}, \"rel\": {\"tx_window\": 4}}");
    cfg_t c;
    cfg_args_t a = {0};
    a.config_path = srv_path;
    a.profile = "demo";
    set(&a, "nat.keepalive_interval_s", "3");
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 0);
    CHECK_EQ_INT(c.c.nat.keepalive_interval_s, 3);
    CHECK_STR(cfg_source(&c, "nat.keepalive_interval_s"), "cli");
    CHECK_EQ_INT(c.c.nat.grace_s, 30);
    CHECK_STR(cfg_source(&c, "nat.grace_s"), "profile:demo");
    CHECK_EQ_INT(c.c.rel.tx_window, 4);
    char want[450];
    snprintf(want, sizeof want, "file:%s", srv_path);
    CHECK_STR(cfg_source(&c, "rel.tx_window"), want);
    CHECK(c.warnings > 0);   /* keepalive < 5: "demo/test value" */
    cfg_free(&c);

    a.nsets = 0;   /* remove --set: the profile overlays the file */
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 0);
    CHECK_EQ_INT(c.c.nat.keepalive_interval_s, 2);
    CHECK_STR(cfg_source(&c, "nat.keepalive_interval_s"), "profile:demo");
    cfg_free(&c);

    a.profile = NULL;   /* remove --profile: the file */
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 0);
    CHECK_EQ_INT(c.c.nat.keepalive_interval_s, 5);
    CHECK_STR(cfg_source(&c, "nat.keepalive_interval_s"), want);
    cfg_free(&c);

    /* the file can name its default profile; --profile overrides it */
    write_text(srv_path, "{\"profile\": \"demo\", \"nat\": {\"keepalive_interval_s\": 5}}");
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 0);
    CHECK_EQ_INT(c.c.nat.keepalive_interval_s, 2);
    CHECK_STR(c.profile, "demo");
    cfg_free(&c);

    /* device: profile values reach the device side too */
    write_text(dev_path, "{\"identity\": {\"serial\": \"ZTP-93AF21\"}}");
    cfg_args_t d = {0};
    d.config_path = dev_path;
    d.profile = "demo";
    CHECK_EQ_INT(quiet_load(&c, CFG_DEVICE, &d), 0);
    CHECK(c.c.run.exit_when_operational);
    CHECK(c.c.identity.serial_set);
    CHECK_STR(c.c.identity.serial, "ZTP-93AF21");
    cfg_free(&c);
    set(&d, "identity.serial", "null");
    CHECK_EQ_INT(quiet_load(&c, CFG_DEVICE, &d), 0);
    CHECK(!c.c.identity.serial_set);
    cfg_free(&c);
}

static int load_server_text(const char *text, bool lenient)
{
    write_text(srv_path, text);
    cfg_args_t a = {0};
    a.config_path = srv_path;
    a.lenient = lenient;
    cfg_t c;
    int rc = quiet_load(&c, CFG_SERVER, &a);
    cfg_free(&c);
    return rc;
}

static int load_device_text(const char *text)
{
    write_text(dev_path, text);
    cfg_args_t a = {0};
    a.config_path = dev_path;
    cfg_t c;
    int rc = quiet_load(&c, CFG_DEVICE, &a);
    cfg_free(&c);
    return rc;
}

static void test_validation(void)
{
    /* TC-CFG-04 ranges and types */
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"keepalive_interval_s\": 0}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"net\": {\"max_datagram\": 1201}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"net\": {\"max_datagram\": 507}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"xfer\": {\"chunk_size\": 768}}", false), 78);        /* not a power of 2 */
    CHECK_EQ_INT(load_server_text("{\"net\": {\"max_datagram\": 600}}", false), 78);       /* 1024 > 600-52 */
    CHECK_EQ_INT(load_server_text("{\"net\": {\"max_datagram\": 600}, \"xfer\": {\"chunk_size\": 512}}", false), 0);
    CHECK_EQ_INT(load_server_text("{\"rel\": {\"tx_window\": 17}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"rel\": {\"rto_min_ms\": 600}}", false), 78);         /* min > initial */
    CHECK_EQ_INT(load_server_text("{\"rel\": {\"rto_initial_ms\": 1000, \"rto_min_ms\": 1000, \"rto_max_ms\": 60000}}", false), 0);
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"grace_s\": 100}}", false), 78);            /* < 2*20*3 */
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"server_idle_intervals\": 2, \"dead_intervals\": 3}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"limits\": {\"max_half_open\": 2000}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"net\": {\"discovery_port\": 5555}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"sim\": {\"loss\": 1.5}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"sim\": {\"mtu\": 50}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"sim\": {\"mtu\": 576, \"mtu_mode\": \"blackhole\"}}", false), 0);
    CHECK_EQ_INT(load_server_text("{\"sim\": {\"mtu_mode\": \"drop\"}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"net\": {\"bind\": \"10.10.10\"}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"keepalive_interval_s\": \"20\"}}", false), 78);  /* type */
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"keepalive_interval_s\": 20.5}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"allow_ip_change\": 1}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"schema_version\": 2}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"nat\": 5}", false), 78);                             /* section as a leaf */
    CHECK_EQ_INT(load_server_text("[1]", false), 78);
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"grace_s\": 120,}}", false), 78);           /* JSON error */
    CHECK_EQ_INT(load_server_text("{\"_comment\": \"x\", \"nat\": {\"_comment\": \"y\"}}", false), 0);

    /* TC-CFG-05 unknown keys: strict vs lenient; also device-only keys on the server */
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"keepalive_intervl_s\": 20}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"keepalive_intervl_s\": 20}}", true), 0);
    CHECK_EQ_INT(load_server_text("{\"identity\": {\"model\": \"X\"}}", false), 78);
    CHECK_EQ_INT(load_server_text("{\"nat\": {\"keepalive_min_s\": 5}}", false), 78);

    /* device rules */
    CHECK_EQ_INT(load_device_text("{\"nat\": {\"keepalive_interval_s\": 4}}"), 78);         /* < min 5 */
    CHECK_EQ_INT(load_device_text("{\"nat\": {\"keepalive_interval_s\": 4, \"keepalive_min_s\": 1}}"), 0);
    CHECK_EQ_INT(load_device_text("{\"identity\": {\"serial\": \"ZTP-93af21\"}}"), 78);
    CHECK_EQ_INT(load_device_text("{\"identity\": {\"factory_fw\": \"1.0\"}}"), 78);
    CHECK_EQ_INT(load_device_text("{\"identity\": {\"model\": \"EDGE-GW-100-TOO-LONG\"}}"), 78);
    CHECK_EQ_INT(load_device_text("{\"discovery\": {\"server\": \"10.10.10.5\"}}"), 78);
    CHECK_EQ_INT(load_device_text("{\"discovery\": {\"server\": \"127.0.0.1:5556\"}}"), 78);  /* = disc port */
    CHECK_EQ_INT(load_device_text("{\"discovery\": {\"mode\": \"anycast\"}}"), 78);
    CHECK_EQ_INT(load_device_text("{\"net\": {\"reconnect_min_s\": 10, \"reconnect_max_s\": 5}}"), 78);
    CHECK_EQ_INT(load_device_text("{\"auth\": {\"register_attempts\": 11}}"), 78);
    CHECK_EQ_INT(load_device_text("{\"net\": {\"pmtu_blackhole_s\": 1}}"), 78);
    CHECK_EQ_INT(load_device_text("{\"limits\": {\"max_sessions\": 5}}"), 78);              /* server-only */

    /* profiles: missing and invalid names, a typo inside a profile */
    cfg_args_t a = {0};
    cfg_t c;
    write_text(srv_path, "{}");
    a.config_path = srv_path;
    a.profile = "nosuch";
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 78);
    cfg_free(&c);
    a.profile = "../etc";
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 78);
    cfg_free(&c);
    a.profile = "typo";
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 78);
    cfg_free(&c);
    /* explicit --config that does not exist */
    a.profile = NULL;
    a.config_path = "/nonexistent/server.json";
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 78);
    cfg_free(&c);
    /* CLI: unknown key and bad value */
    a.config_path = srv_path;
    set(&a, "nat.nope", "1");
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 78);
    cfg_free(&c);
    a.nsets = 0;
    set(&a, "rel.tx_window", "eight");
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 78);
    cfg_free(&c);
}

static void test_nat_override(void)
{
    char err[256];
    ztp_nat_override_t n = {20, 3, 3, 120, true};
    CHECK_EQ_INT(cfg_validate_nat_override(&n, "policy:serial ZTP-55AC90", err, sizeof err), 0);
    n.keepalive_interval_s = 900;   /* TC-UNIT-20: rejected before any device sees it */
    CHECK_EQ_INT(cfg_validate_nat_override(&n, "policy:serial ZTP-55AC90", err, sizeof err), -1);
    CHECK(strstr(err, "nat.keepalive_interval_s=900") && strstr(err, "policy:serial ZTP-55AC90"));
    n.keepalive_interval_s = 10;    /* the shipped override example: 10 s, grace 120 >= 60 */
    CHECK_EQ_INT(cfg_validate_nat_override(&n, NULL, err, sizeof err), 0);
    n.keepalive_interval_s = 25;    /* grace 120 < 2*25*3 = 150 */
    CHECK_EQ_INT(cfg_validate_nat_override(&n, NULL, err, sizeof err), -1);
    n.grace_s = 200; n.server_idle_intervals = 4; n.dead_intervals = 4;   /* metered profile */
    CHECK_EQ_INT(cfg_validate_nat_override(&n, NULL, err, sizeof err), 0);
}

static void test_cli_flags(void)
{
    cfg_args_t a = {0};
    char *argv[] = {"x", "--loss", "0.15", "--sim-mtu", "576:blackhole", "-v", "--demo-timers", "--set",
                    "rel.tx_window=4", "--serial", "ZTP-93AF21", "--nat-sim", "idle_timeout_s=5,block_server_port",
                    "--test-hooks", "--set", "novalue"};
    int argc = (int)(sizeof argv / sizeof argv[0]);
    int i = 1, r;
    r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == 1 && i == 2);
    i++; r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == 1 && i == 4);
    i++; r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == 1);
    i++; r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == 1 && a.profile && !strcmp(a.profile, "demo"));
    i++; r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == 1 && i == 8);
    i++; r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == 1 && i == 10);
    i++; r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == 1 && i == 12);
    i++; r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == 0);   /* --test-hooks: not config */
    i++; r = cfg_parse_flag(&a, CFG_DEVICE, argc, argv, &i); CHECK(r == -1);  /* --set without '=' */
    CHECK_EQ_INT(a.nsets, 8);
    CHECK_STR(a.sets[0].key, "sim.loss");
    CHECK_STR(a.sets[2].value, "blackhole");
    CHECK_STR(a.sets[3].value, "debug");
    CHECK_STR(a.sets[6].key, "sim.nat.idle_timeout_s");
    CHECK_STR(a.sets[7].key, "sim.nat.block_server_port");
    CHECK_STR(a.sets[7].value, "true");
    /* server-only shorthand is not a device flag */
    char *argv2[] = {"x", "--port", "5555"};
    i = 1;
    CHECK_EQ_INT(cfg_parse_flag(&a, CFG_DEVICE, 3, argv2, &i), 0);
    i = 1;
    CHECK_EQ_INT(cfg_parse_flag(&a, CFG_SERVER, 3, argv2, &i), 1);
    /* missing argument */
    char *argv3[] = {"x", "--loss"};
    i = 1;
    CHECK_EQ_INT(cfg_parse_flag(&a, CFG_SERVER, 2, argv3, &i), -1);
}

static void test_print(void)
{
    cfg_args_t a = {0};
    a.config_path = srv_path;
    write_text(srv_path, "{\"nat\": {\"grace_s\": 150}}");
    cfg_t c;
    CHECK_EQ_INT(quiet_load(&c, CFG_SERVER, &a), 0);
    char out[300];
    snprintf(out, sizeof out, "%s/print.json", test_tmpdir());
    FILE *f = fopen(out, "w");
    cfg_print(&c, f);
    fclose(f);
    cfg_free(&c);
    /* the printed document is itself valid strict JSON (re-parse with our parser) */
    f = fopen(out, "r");
    char buf[16384];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    buf[n] = '\0';
    CHECK(strstr(buf, "\"grace_s\": {\"value\": 150, \"source\": \"file:") != NULL);
    CHECK(strstr(buf, "\"loss\": {\"value\": 0, \"source\": \"default\"}") != NULL);
    json_error_t err;
    json_doc_t *doc = json_parse(buf, n, &err);
    CHECK(doc != NULL);
    json_free(doc);
    CHECK(cfg_schema_count(CFG_SERVER) > 50 && cfg_schema_count(CFG_DEVICE) > 50);
}

void suite_config(void)
{
    setup();
    test_defaults();
    test_precedence();
    test_validation();
    test_nat_override();
    test_cli_flags();
    test_print();
}
