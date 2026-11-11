/* ztp-server entry point. M1: configuration loading, --print-config and the [CONFIG] line.
 * The event loop, sessions and handlers arrive in M2–M5. */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "log.h"

static void srv_usage(void)
{
    fputs("usage: ztp-server [--config FILE] [--profile NAME] [--set key=value]... [--print-config]\n"
          "                  [--config-lenient] [--demo-timers] [-v|-q] [flags from DESIGN §19.2]\n",
          stderr);
}

int main(int argc, char **argv)
{
    cfg_args_t args = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            srv_usage();
            return 0;
        }
        int r = cfg_parse_flag(&args, CFG_SERVER, argc, argv, &i);
        if (r < 0) return EX_USAGE_CODE;
        if (r == 0) {
            fprintf(stderr, "ztp-server: unknown option %s\n", argv[i]);
            srv_usage();
            return EX_USAGE_CODE;
        }
    }

    cfg_t cfg;
    int rc = cfg_load(&cfg, CFG_SERVER, &args);
    if (rc) {
        cfg_free(&cfg);
        return rc;
    }
    if (args.print) {
        cfg_print(&cfg, stdout);
        cfg_free(&cfg);
        return 0;
    }
    log_set_level(log_level_parse(cfg.c.log.level));
    cfg_log_summary(&cfg);
    fputs("ztp-server: protocol engine not built yet (milestone M2+)\n", stderr);
    cfg_free(&cfg);
    return 0;
}
