/* ztp-device: CLI, configuration, state directory, boot and serial origin (DESIGN §6.1, §15.5). */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dev.h"
#include "log.h"
#include "logmsg.h"
#include "rand.h"
#include "timer.h"

static void dev_usage(void)
{
    fputs("usage: ztp-device --state-dir DIR [--config FILE] [--profile NAME] [--set key=value]...\n"
          "                  [--print-config] [--config-lenient] [--demo-timers] [-v|-q] [--reset-state]\n"
          "                  [--test-hooks [--drop-seq tx:N[,N]] [--dup-seq tx:N]]\n"
          "                  [flags from DESIGN §19.2]\n",
          stderr);
}

static int mkdirs(const char *path)
{
    char p[CFG_PATH];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = '\0';
            if (mkdir(p, 0755) != 0 && errno != EEXIST) return -1;
            *s = '/';
        }
    return (mkdir(p, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

static void reset_state(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char p[CFG_PATH + 512];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(dp);
}

int main(int argc, char **argv)
{
    static device_t D;
    device_t *d = &D;
    cfg_args_t args = {0};
    bool want_hooks = false, reset = false;
    const char *hook_names[4];
    int nhooks = 0;

    for (int i = 1; i < argc; i++) {
        const char *f = argv[i];
        if (!strcmp(f, "-h") || !strcmp(f, "--help")) { dev_usage(); return 0; }
        int r = cfg_parse_flag(&args, CFG_DEVICE, argc, argv, &i);
        if (r < 0) return EX_USAGE_CODE;
        if (r == 1) continue;
        if (!strcmp(f, "--reset-state")) {
            reset = true;
        } else if (!strcmp(f, "--test-hooks")) {
            want_hooks = true;
        } else if (!strcmp(f, "--drop-seq") && i + 1 < argc) {
            if (hooks_parse_seq_list(argv[++i], d->hooks.drop_seq, HOOK_MAX_SEQ, &d->hooks.ndrop) != 0) {
                fprintf(stderr, "--drop-seq: expected tx:N[,N...]\n");
                return EX_USAGE_CODE;
            }
            hook_names[nhooks++] = "--drop-seq";
        } else if (!strcmp(f, "--dup-seq") && i + 1 < argc) {
            int n1 = 0;
            if (hooks_parse_seq_list(argv[++i], &d->hooks.dup_seq, 1, &n1) != 0) {
                fprintf(stderr, "--dup-seq: expected tx:N\n");
                return EX_USAGE_CODE;
            }
            hook_names[nhooks++] = "--dup-seq";
        } else {
            fprintf(stderr, "ztp-device: unknown option %s\n", f);
            dev_usage();
            return EX_USAGE_CODE;
        }
    }
    if (nhooks && !want_hooks) {
        fprintf(stderr, "ztp-device: %s is a test hook and needs --test-hooks\n", hook_names[0]);
        return EX_USAGE_CODE;
    }
    d->hooks.enabled = want_hooks;

    int rc = cfg_load(&d->cfg, CFG_DEVICE, &args);
    if (rc) { cfg_free(&d->cfg); return rc; }
    if (args.print) { cfg_print(&d->cfg, stdout); cfg_free(&d->cfg); return 0; }
    const ztp_config_t *c = &d->cfg.c;

    /* state directory and identity (§6.1) */
    if (mkdirs(c->paths.state_dir) != 0) {
        fprintf(stderr, "ztp-device: cannot create %s: %s\n", c->paths.state_dir, strerror(errno));
        return 1;
    }
    if (reset) reset_state(c->paths.state_dir);
    snprintf(d->state_path, sizeof d->state_path, "%s/device.state", c->paths.state_dir);
    bool had_state = dev_state_load(d->state_path, &d->st) == 0;
    const char *origin;
    if (had_state) {
        if (c->identity.serial_set && strcmp(c->identity.serial, d->st.serial) != 0) {
            fprintf(stderr, "config: identity.serial=%s (%s): this device is already %s; refusing to change identity "
                            "(use --reset-state)\n", c->identity.serial, cfg_source(&d->cfg, "identity.serial"), d->st.serial);
            return EX_CONFIG_CODE;
        }
        origin = c->identity.serial_set ? LM_BOOT_SERIAL_ASSIGNED : LM_BOOT_SERIAL_LOADED;
    } else if (c->identity.serial_set) {
        snprintf(d->st.serial, sizeof d->st.serial, "%s", c->identity.serial);
        origin = LM_BOOT_SERIAL_ASSIGNED;
    } else {
        uint8_t b[3];
        rand_bytes(b, sizeof b);
        snprintf(d->st.serial, sizeof d->st.serial, "ZTP-%02X%02X%02X", b[0], b[1], b[2]);
        origin = LM_BOOT_SERIAL_GENERATED;
    }
    snprintf(d->st.model, sizeof d->st.model, "%s", c->identity.model);
    snprintf(d->st.hw_rev, sizeof d->st.hw_rev, "%s", c->identity.hw_rev);
    d->st.boot_count++;
    if (dev_state_save(d->state_path, &d->st) != 0) {
        fprintf(stderr, "ztp-device: cannot write %s: %s\n", d->state_path, strerror(errno));
        return 1;
    }

    /* log file: paths.log with %s = serial; appended across restarts (§17.3) */
    char logpath[2 * CFG_PATH + 32];
    {
        /* replace the single "%s" (validated: at most one, no other '%') with the serial */
        const char *pl = c->paths.log, *pct = strstr(pl, "%s");
        size_t pre = pct ? (size_t)(pct - pl) : strlen(pl), sl = pct ? strlen(d->st.serial) : 0;
        const char *rest = pct ? pct + 2 : "";
        size_t rl = strlen(rest);
        if (pre + sl + rl + 1 > sizeof logpath) {
            fprintf(stderr, "ztp-device: log path too long\n");
            return EX_CONFIG_CODE;
        }
        memcpy(logpath, pl, pre);
        memcpy(logpath + pre, d->st.serial, sl);
        memcpy(logpath + pre + sl, rest, rl + 1);
    }
    char dir[2 * CFG_PATH + 32];
    snprintf(dir, sizeof dir, "%s", logpath);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = '\0'; mkdirs(dir); }
    log_set_level(log_level_parse(c->log.level));
    if (log_open(logpath, true, true) != 0)
        fprintf(stderr, "ztp-device: cannot open log %s: %s (stdout only)\n", logpath, strerror(errno));
    for (int i = 0; i < nhooks; i++) LOGW(T_ERROR, LM_ERROR_TEST_HOOK, hook_names[i]);

    LOGI(T_BOOT, LM_BOOT_STARTING_N, (unsigned)d->st.boot_count);
    LOGI(T_BOOT, "%s", origin);
    LOGI(T_BOOT, LM_BOOT_SOFT_SERIAL, d->st.serial);
    LOGI(T_BOOT, LM_BOOT_FW_VERSION, c->identity.factory_fw);   /* M5: read from the active slot header */
    cfg_log_summary(&d->cfg);
    if (d->st.stage == ST_FAILED) {
        LOGI(T_STATE, LM_STATE_FAILED, d->st.last_fail_reason[0] ? d->st.last_fail_reason : "unknown");
        LOGI(T_STATE, "Stage FAILED is persisted; run with --reset-state to start over");
        return 1;
    }

    dev_factory_keys(c->identity.model, d->k_fleet, d->k_mfg);
    ztp_derive_kboot(d->k_fleet, d->k_boot);

    char err[256];
    d->fd = udp_open("0.0.0.0", 0, false, err, sizeof err);
    if (d->fd < 0) { LOGW(T_ERROR, "socket: %s", err); return 1; }
    if (c->net.set_df) udp_set_df(d->fd);
    if (!strcmp(c->discovery.mode, "bcast")) udp_set_broadcast(d->fd);
    if (!strcmp(c->discovery.mode, "mcast") && strcmp(c->discovery.mcast_iface_addr, "0.0.0.0") != 0 &&
        udp_mcast_sender(d->fd, c->discovery.mcast_iface_addr, 1) != 0)
        LOGW(T_DISCOVERY, "multicast interface %s: %s", c->discovery.mcast_iface_addr, strerror(errno));

    netsim_cfg_t nc = {c->sim.loss, c->sim.reorder, c->sim.dup, (uint32_t)c->sim.reorder_delay_ms,
                       (uint32_t)c->sim.delay_ms, (uint32_t)c->sim.jitter_ms, (uint32_t)c->sim.mtu,
                       !strcmp(c->sim.mtu_mode, "blackhole") ? NS_MTU_BLACKHOLE : NS_MTU_EMSGSIZE, c->sim.seed};
    netsim_init(&d->ns, &nc);
    rel_cfg_from(&d->relcfg, c, 8);
    d->keepalive_s = c->nat.keepalive_interval_s;
    d->dead_intervals = c->nat.dead_intervals;

    rc = dev_run(d);

    netsim_free(&d->ns);
    close(d->fd);
    secure_zero(d->k_fleet, sizeof d->k_fleet);
    secure_zero(d->k_mfg, sizeof d->k_mfg);
    cfg_free(&d->cfg);
    log_close();
    return rc;
}
