/* ztp-server: CLI, startup checks, sockets, the single-threaded poll() loop (DESIGN §14.1). */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "logmsg.h"
#include "srv.h"
#include "timer.h"
#include "util.h"

static int g_sigpipe[2] = {-1, -1};

static void on_signal(int sig)
{
    int e = errno;
    unsigned char b = (unsigned char)sig;
    if (write(g_sigpipe[1], &b, 1) < 0) { /* pipe full: a signal is already pending */ }
    errno = e;
}

static void srv_usage(void)
{
    fputs("usage: ztp-server [--config FILE] [--profile NAME] [--set key=value]... [--print-config]\n"
          "                  [--config-lenient] [--demo-timers] [-v|-q] [--run-for SECS]\n"
          "                  [--test-hooks [--first-session-id N] [--drop-seq tx:N[,N]] [--dup-seq tx:N]\n"
          "                                [--no-discovery]]\n"
          "                  [flags from DESIGN §19.2]\n",
          stderr);
}

static void mkdir_parent(const char *path)
{
    char dir[CFG_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = '\0';
    mkdir(dir, 0755);
}

static void srv_log_counters(srv_t *s)
{
    rel_stats_t r = s->c.rel;
    LOGI(T_UDP,
         "Counters: tx=%llu rx=%llu retx_timeout=%llu retx_fast=%llu dup=%llu ooo=%llu out_of_window=%llu "
         "rx_malformed=%llu rx_unknown=%llu rx_stale=%llu mac_fail=%llu replay_rejected=%llu rate_limited=%llu "
         "mtu_stepdowns=%llu sessions_peak=%llu srtt_ms=%u",
         (unsigned long long)r.tx, (unsigned long long)r.rx, (unsigned long long)r.retx_timeout,
         (unsigned long long)r.retx_fast, (unsigned long long)r.dup, (unsigned long long)r.ooo,
         (unsigned long long)r.out_of_window, (unsigned long long)s->c.rx_malformed,
         (unsigned long long)s->c.rx_unknown, (unsigned long long)s->c.rx_stale, (unsigned long long)s->c.mac_fail,
         (unsigned long long)s->c.replay_rejected, (unsigned long long)s->c.rate_limited,
         (unsigned long long)s->c.mtu_stepdowns, (unsigned long long)s->c.sessions_peak, (unsigned)r.srtt_ms);
}

static void counters_str(srv_t *s, char *out, size_t n)
{
    rel_stats_t r = s->c.rel;
    for (size_t k = 0; k < SRV_TABLE_SLOTS; k++) {   /* live sessions too */
        srv_session_t *e = s->table[k];
        if (!e || e == (srv_session_t *)NULL) continue;
        if (!srv_sess_find(s, e->session_id)) continue;
        const rel_stats_t *x = rel_stats(e->rel);
        r.rx += x->rx; r.tx += x->tx; r.retx_timeout += x->retx_timeout; r.retx_fast += x->retx_fast;
        r.dup += x->dup; r.ooo += x->ooo; r.out_of_window += x->out_of_window;
    }
    snprintf(out, n, "rx=%llu tx=%llu retx_timeout=%llu retx_fast=%llu dup=%llu ooo=%llu out_of_window=%llu mac_fail=%llu "
             "replay_rejected=%llu rate_limited=%llu",
             (unsigned long long)r.rx, (unsigned long long)r.tx, (unsigned long long)r.retx_timeout,
             (unsigned long long)r.retx_fast, (unsigned long long)r.dup, (unsigned long long)r.ooo,
             (unsigned long long)r.out_of_window, (unsigned long long)s->c.mac_fail,
             (unsigned long long)s->c.replay_rejected, (unsigned long long)s->c.rate_limited);
}

/* §14.5: the dump (and summary) to stdout, the server log and paths.dump; verbose variant optional. */
static void srv_dump(srv_t *s, bool summary)
{
    const ztp_config_t *c = &s->cfg.c;
    char counters[512];
    counters_str(s, counters, sizeof counters);
    char *text = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&text, &len);
    if (!m) return;
    db_dump(&s->db, m, false);
    if (summary) db_summary(&s->db, m, counters);
    fclose(m);
    fputs(text, stdout);
    fflush(stdout);
    log_raw(text);
    mkdir_parent(c->paths.dump);
    if (atomic_write_file(c->paths.dump, text, len) != 0) LOGW(T_DB, "cannot write %s: %s", c->paths.dump, strerror(errno));
    free(text);
    if (c->log.dump_verbose) {
        char vpath[CFG_PATH + 32];
        snprintf(vpath, sizeof vpath, "%s", c->paths.dump);
        char *slash = strrchr(vpath, '/');
        snprintf(slash ? slash + 1 : vpath, sizeof vpath - (size_t)(slash ? slash + 1 - vpath : 0), "db_dump_verbose.txt");
        m = open_memstream(&text, &len);
        if (!m) return;
        db_dump(&s->db, m, true);
        if (summary) db_summary(&s->db, m, counters);
        fclose(m);
        if (atomic_write_file(vpath, text, len) != 0) LOGW(T_DB, "cannot write %s: %s", vpath, strerror(errno));
        free(text);
    }
}

static void drain(srv_t *s, int fd, bool disc)
{
    for (int i = 0; i < 64; i++) {   /* at most 64 datagrams per wakeup, for fairness (§14.1) */
        uint8_t buf[2048];
        struct sockaddr_in src;
        socklen_t sl = sizeof src;
        ssize_t n = netsim_recvfrom(&s->ns, fd, buf, sizeof buf, 0, (struct sockaddr *)&src, &sl);
        if (n < 0) return;   /* EAGAIN or error */
        s->now = now_ms();
        if (disc) srv_on_disc_datagram(s, buf, (size_t)n, &src);
        else srv_on_ctl_datagram(s, buf, (size_t)n, &src);
    }
}

int main(int argc, char **argv)
{
    static srv_t S;
    srv_t *s = &S;
    cfg_args_t args = {0};
    long run_for = 0;
    bool want_hooks = false, dump_on_exit = false;
    const char *hook_names[8];
    int nhooks = 0;

    for (int i = 1; i < argc; i++) {
        const char *f = argv[i];
        if (!strcmp(f, "-h") || !strcmp(f, "--help")) { srv_usage(); return 0; }
        int r = cfg_parse_flag(&args, CFG_SERVER, argc, argv, &i);
        if (r < 0) return EX_USAGE_CODE;
        if (r == 1) continue;
        if (!strcmp(f, "--run-for") && i + 1 < argc) {
            run_for = atol(argv[++i]);
            if (run_for <= 0) { fprintf(stderr, "--run-for: expected seconds > 0\n"); return EX_USAGE_CODE; }
        } else if (!strcmp(f, "--dump-on-exit")) {
            dump_on_exit = true;
        } else if (!strcmp(f, "--test-hooks")) {
            want_hooks = true;
        } else if (!strcmp(f, "--first-session-id") && i + 1 < argc) {
            long v = atol(argv[++i]);
            if (v <= 0 || v > 0xFFFFFFFFL) { fprintf(stderr, "--first-session-id: expected 1..4294967295\n"); return EX_USAGE_CODE; }
            s->hook_first_sid = (uint32_t)v;
            hook_names[nhooks++] = "--first-session-id";
        } else if (!strcmp(f, "--drop-seq") && i + 1 < argc) {
            if (hooks_parse_seq_list(argv[++i], s->hooks.drop_seq, HOOK_MAX_SEQ, &s->hooks.ndrop) != 0) {
                fprintf(stderr, "--drop-seq: expected tx:N[,N...]\n");
                return EX_USAGE_CODE;
            }
            hook_names[nhooks++] = "--drop-seq";
        } else if (!strcmp(f, "--dup-seq") && i + 1 < argc) {
            int n1 = 0;
            if (hooks_parse_seq_list(argv[++i], &s->hooks.dup_seq, 1, &n1) != 0) {
                fprintf(stderr, "--dup-seq: expected tx:N\n");
                return EX_USAGE_CODE;
            }
            hook_names[nhooks++] = "--dup-seq";
        } else if (!strcmp(f, "--no-discovery")) {
            s->hook_no_discovery = true;
            hook_names[nhooks++] = "--no-discovery";
        } else {
            fprintf(stderr, "ztp-server: unknown option %s\n", f);
            srv_usage();
            return EX_USAGE_CODE;
        }
    }
    if (nhooks && !want_hooks) {
        fprintf(stderr, "ztp-server: %s is a test hook and needs --test-hooks\n", hook_names[0]);
        return EX_USAGE_CODE;
    }
    s->hooks.enabled = want_hooks;

    int rc = cfg_load(&s->cfg, CFG_SERVER, &args);
    if (rc) { cfg_free(&s->cfg); return rc; }
    if (args.print) { cfg_print(&s->cfg, stdout); cfg_free(&s->cfg); return 0; }
    const ztp_config_t *c = &s->cfg.c;

    log_set_level(log_level_parse(c->log.level));
    mkdir_parent(c->paths.log);
    if (log_open(c->paths.log, false, true) != 0)
        fprintf(stderr, "ztp-server: cannot open log %s: %s (stdout only)\n", c->paths.log, strerror(errno));
    for (int i = 0; i < nhooks; i++) LOGW(T_ERROR, LM_ERROR_TEST_HOOK, hook_names[i]);
    cfg_log_summary(&s->cfg);

    char err[512];
    if ((rc = srv_keys_load(c->paths.keys, &s->keys, err, sizeof err)) ||
        (rc = srv_policy_load(c->paths.policy, &s->policy, err, sizeof err)) ||
        (rc = srv_policy_validate(&s->policy, &s->cfg, &s->keys, err, sizeof err)) ||
        (rc = srv_policy_check_files(&s->policy, err, sizeof err))) {
        fprintf(stderr, "%s\n", err);
        LOGW(T_ERROR, "%s", err);
        return rc;
    }

    if ((rc = srv_artifacts_load(&s->artifacts, &s->policy, err, sizeof err))) {
        fprintf(stderr, "%s\n", err);
        LOGW(T_ERROR, "%s", err);
        return rc;
    }
    if (db_open(&s->db, c->paths.db, c->limits.max_failed_records, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        LOGW(T_ERROR, "%s", err);
        return 1;
    }
    {   /* one-shot hooks fire at most once per server state (§19.2): state/server.hooks.fired */
        char mk[CFG_PATH + 32];
        snprintf(mk, sizeof mk, "%s", c->paths.db);
        char *slash = strrchr(mk, '/');
        snprintf(slash ? slash + 1 : mk, sizeof mk - (size_t)(slash ? slash + 1 - mk : 0), "server.hooks.fired");
        hooks_set_marker(&s->hooks, mk);
    }
    s->nonce_cap = (size_t)c->limits.nonce_cache_total;
    s->nonces = calloc(s->nonce_cap, sizeof *s->nonces);

    netsim_cfg_t nc = {c->sim.loss, c->sim.reorder, c->sim.dup, (uint32_t)c->sim.reorder_delay_ms,
                       (uint32_t)c->sim.delay_ms, (uint32_t)c->sim.jitter_ms, (uint32_t)c->sim.mtu,
                       !strcmp(c->sim.mtu_mode, "blackhole") ? NS_MTU_BLACKHOLE : NS_MTU_EMSGSIZE, c->sim.seed};
    netsim_init(&s->ns, &nc);
    rel_cfg_from(&s->relcfg, c, c->xfer.tx_queue_max);

    s->ctl_fd = udp_open(c->net.bind, (uint16_t)c->net.server_port, false, err, sizeof err);
    if (s->ctl_fd < 0) { LOGW(T_ERROR, "control socket: %s", err); fprintf(stderr, "%s\n", err); return 1; }
    if (c->net.set_df) udp_set_df(s->ctl_fd);
    s->disc_fd = -1;
    if (!s->hook_no_discovery) {
        s->disc_fd = udp_open("0.0.0.0", (uint16_t)c->net.discovery_port, true, err, sizeof err);
        if (s->disc_fd < 0) { LOGW(T_ERROR, "discovery socket: %s", err); fprintf(stderr, "%s\n", err); return 1; }
        const char *iface = c->net.mcast_iface_addr;
        if (!strcmp(iface, "0.0.0.0")) iface = c->net.bind;
        if (udp_join_mcast(s->disc_fd, c->net.mcast_group, iface) != 0)
            LOGW(T_DISCOVERY, "could not join %s on %s: %s (multicast discovery unavailable)", c->net.mcast_group,
                 iface, strerror(errno));
    }
    LOGI(T_BOOT, "Server listening on %s:%d (discovery %s:%d, group %s)", c->net.bind, c->net.server_port,
         s->disc_fd >= 0 ? "0.0.0.0" : "off", c->net.discovery_port, c->net.mcast_group);

    if (pipe(g_sigpipe) == 0) {
        fcntl(g_sigpipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_sigpipe[1], F_SETFL, O_NONBLOCK);
    }
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    s->now = now_ms();
    srv_sess_restore(s);

    uint64_t start = now_ms(), stop_at = run_for ? start + (uint64_t)run_for * 1000u : UINT64_MAX;
    bool running = true, by_signal = false;
    while (running) {
        s->now = now_ms();
        uint64_t d = srv_sess_next_deadline(s);
        uint64_t nd = netsim_next_deadline(&s->ns);
        if (nd < d) d = nd;
        if (stop_at < d) d = stop_at;
        int timeout = d == UINT64_MAX ? 1000 : (d <= s->now ? 0 : (int)(d - s->now > 1000 ? 1000 : d - s->now));
        struct pollfd p[3] = {{s->ctl_fd, POLLIN, 0}, {g_sigpipe[0], POLLIN, 0}, {s->disc_fd, POLLIN, 0}};
        int np = s->disc_fd >= 0 ? 3 : 2;
        if (poll(p, (nfds_t)np, timeout) < 0 && errno != EINTR) break;
        s->now = now_ms();
        if (p[1].revents & POLLIN) {
            unsigned char b;
            while (read(g_sigpipe[0], &b, 1) == 1) {
                if (b == SIGINT || b == SIGTERM) { running = false; by_signal = true; }
                if (b == SIGUSR1) srv_dump(s, false);
            }
        }
        if (p[0].revents & POLLIN) drain(s, s->ctl_fd, false);
        if (np == 3 && (p[2].revents & POLLIN)) drain(s, s->disc_fd, true);
        s->now = now_ms();
        srv_sess_run_timers(s);
        netsim_flush(&s->ns, s->now);
        if (s->now >= stop_at) running = false;
    }

    LOGI(T_BOOT, "Server stopping");
    if (by_signal || dump_on_exit) srv_dump(s, true);
    srv_sess_free_all(s);
    srv_log_counters(s);
    netsim_log_totals(&s->ns);
    netsim_free(&s->ns);
    db_close(&s->db, true);
    srv_artifacts_free(&s->artifacts);
    free(s->nonces);
    srv_policy_free(&s->policy);
    srv_keys_free(&s->keys);
    cfg_free(&s->cfg);
    close(s->ctl_fd);
    if (s->disc_fd >= 0) close(s->disc_fd);
    log_close();
    return 0;
}
