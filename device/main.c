/* ztp-device entry point. M1: configuration loading, --print-config and the [CONFIG] line.
 * The device state machine arrives in M2–M5. */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "log.h"

static void dev_usage(void)
{
    fputs("usage: ztp-device --state-dir DIR [--config FILE] [--profile NAME] [--set key=value]...\n"
          "                  [--print-config] [--config-lenient] [--demo-timers] [-v|-q]\n"
          "                  [flags from DESIGN §19.2]\n",
          stderr);
}

int main(int argc, char **argv)
{
    cfg_args_t args = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            dev_usage();
            return 0;
        }
        int r = cfg_parse_flag(&args, CFG_DEVICE, argc, argv, &i);
        if (r < 0) return EX_USAGE_CODE;
        if (r == 0) {
            fprintf(stderr, "ztp-device: unknown option %s\n", argv[i]);
            dev_usage();
            return EX_USAGE_CODE;
        }
    }

    cfg_t cfg;
    int rc = cfg_load(&cfg, CFG_DEVICE, &args);
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
    fputs("ztp-device: state machine not built yet (milestone M2+)\n", stderr);
    cfg_free(&cfg);
    return 0;
}
