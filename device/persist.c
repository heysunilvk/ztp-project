/* <state-dir>/device.state (DESIGN §15.3): key=value lines, written atomically with a .bak. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev.h"
#include "protocol.h"
#include "util.h"

static const char *const STAGES[] = {"NEW", "REGISTERED", "CONFIG_OK", "FW_STAGED", "FW_INSTALLED", "OPERATIONAL", "FAILED"};

const char *dev_stage_name(int stage)
{
    return stage >= 0 && stage < (int)(sizeof STAGES / sizeof STAGES[0]) ? STAGES[stage] : "?";
}

static int stage_parse(const char *s)
{
    for (int i = 0; i < (int)(sizeof STAGES / sizeof STAGES[0]); i++)
        if (!strcmp(s, STAGES[i])) return i;
    return -1;
}

static void defaults(dev_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->format = 1;
    st->active_slot = 'A';
    st->stage = ST_NEW;
    st->max_dgram = ZTP_MAX_DGRAM_CAP;
}

int dev_state_load(const char *path, dev_state_t *st)
{
    defaults(st);
    char *text = NULL;
    size_t len = 0;
    if (read_file(path, 1u << 16, true, &text, &len) != 0) return -1;
    int rc = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        unsigned long u = strtoul(v, NULL, 10);
        if (!strcmp(k, "format")) { if (u != 1) rc = -1; }
        else if (!strcmp(k, "serial")) str_copy(st->serial, sizeof st->serial, v);
        else if (!strcmp(k, "model")) str_copy(st->model, sizeof st->model, v);
        else if (!strcmp(k, "hw_rev")) str_copy(st->hw_rev, sizeof st->hw_rev, v);
        else if (!strcmp(k, "boot_count")) st->boot_count = (uint32_t)u;
        else if (!strcmp(k, "active_slot")) st->active_slot = v[0] == 'B' ? 'B' : 'A';
        else if (!strcmp(k, "pending_reboot")) st->pending_reboot = (int)u;
        else if (!strcmp(k, "stage")) { int s = stage_parse(v); if (s < 0) rc = -1; else st->stage = s; }
        else if (!strcmp(k, "session_id")) st->session_id = (uint32_t)u;
        else if (!strcmp(k, "epoch")) st->epoch = (uint16_t)u;
        else if (!strcmp(k, "proposed_epoch")) st->proposed_epoch = (uint16_t)u;
        else if (!strcmp(k, "resume_ctr")) st->resume_ctr = (uint32_t)u;
        else if (!strcmp(k, "server")) str_copy(st->server, sizeof st->server, v);
        else if (!strcmp(k, "tenant")) str_copy(st->tenant, sizeof st->tenant, v);
        else if (!strcmp(k, "max_dgram")) st->max_dgram = (uint16_t)u;
        else if (!strcmp(k, "fw_target")) str_copy(st->fw_target, sizeof st->fw_target, v);
        else if (!strcmp(k, "fw_sha256")) str_copy(st->fw_sha256, sizeof st->fw_sha256, v);
        else if (!strcmp(k, "cfg_version")) st->cfg_version = (int)u;
        else if (!strcmp(k, "cfg_sha256")) str_copy(st->cfg_sha256, sizeof st->cfg_sha256, v);
        else if (!strcmp(k, "reg_attempts")) st->reg_attempts = (int)u;
        else if (!strcmp(k, "last_fail_reason")) str_copy(st->last_fail_reason, sizeof st->last_fail_reason, v);
    }
    free(text);
    if (rc == 0 && !*st->serial) rc = -1;
    return rc;
}

int dev_state_save(const char *path, const dev_state_t *st)
{
    char buf[2048];
    int n = snprintf(buf, sizeof buf,
                     "format=1\nserial=%s\nmodel=%s\nhw_rev=%s\nboot_count=%u\nactive_slot=%c\npending_reboot=%d\n"
                     "stage=%s\nsession_id=%u\nepoch=%u\nproposed_epoch=%u\nresume_ctr=%u\nserver=%s\ntenant=%s\n"
                     "max_dgram=%u\nfw_target=%s\nfw_sha256=%s\ncfg_version=%d\ncfg_sha256=%s\nreg_attempts=%d\n"
                     "last_fail_reason=%s\n",
                     st->serial, st->model, st->hw_rev, (unsigned)st->boot_count, st->active_slot, st->pending_reboot,
                     dev_stage_name(st->stage), (unsigned)st->session_id, (unsigned)st->epoch,
                     (unsigned)st->proposed_epoch, (unsigned)st->resume_ctr, st->server, st->tenant,
                     (unsigned)st->max_dgram, st->fw_target, st->fw_sha256, st->cfg_version, st->cfg_sha256,
                     st->reg_attempts, st->last_fail_reason);
    if (n < 0 || (size_t)n >= sizeof buf) return -1;
    return atomic_write_file(path, buf, (size_t)n);
}
