/* Device upgrade (DESIGN §12): A/B slots, the commit point, process restart, rollback. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "dev.h"
#include "log.h"
#include "logmsg.h"
#include "util.h"

#define FW_HDR_LEN 64

/* Read "ZTPFW\0" | fmt | model[16] | version[8] | payload_len from a firmware file (§11.1). */
static bool fw_header(const char *path, char ver[9], char model[17])
{
    uint8_t h[FW_HDR_LEN];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, h, sizeof h);
    close(fd);
    if (n != FW_HDR_LEN || memcmp(h, "ZTPFW\0", 6) != 0 || h[6] != 1) return false;
    memcpy(model, h + 7, 16);
    model[16] = '\0';
    memcpy(ver, h + 23, 8);
    ver[8] = '\0';
    return true;
}

static void slot_path(const device_t *d, char slot, char *out, size_t n)
{
    snprintf(out, n, "%s/fw_slot_%c.bin", d->cfg.c.paths.state_dir, slot);
}

bool dev_slot_version(const device_t *d, char slot, char ver[9], char model[17])
{
    char p[CFG_PATH + 32];
    slot_path(d, slot, p, sizeof p);
    return fw_header(p, ver, model);
}

/* The running version: the active slot's header, or the factory image (slot A without a file). */
void dev_running_fw(const device_t *d, char ver[9])
{
    char model[17];
    if (!dev_slot_version(d, d->st.active_slot, ver, model)) snprintf(ver, 9, "%s", d->cfg.c.identity.factory_fw);
}

static bool slot_matches(device_t *d, char slot, const char *version, const char *sha_hex)
{
    char p[CFG_PATH + 32], ver[9], model[17];
    slot_path(d, slot, p, sizeof p);
    if (!fw_header(p, ver, model) || strcmp(model, d->cfg.c.identity.model) || strcmp(ver, version)) return false;
    if (!sha_hex || !*sha_hex) return true;
    char *data = NULL;
    size_t len = 0;
    if (read_file(p, 64u << 20, false, &data, &len) != 0) return false;
    uint8_t s[32];
    char hex[65];
    sha256(data, len, s);
    free(data);
    hex_encode(s, 32, hex);
    return !strcmp(hex, sha_hex);
}

static void send_upgrade_status(device_t *d, uint8_t state, const char *fw)
{
    uint8_t pl[48];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u8(&w, TAG_UPG_STATE, state);
    tlv_put_str(&w, TAG_FW_VER, fw);
    tlv_put_str(&w, TAG_FW_TARGET, d->st.fw_target);
    rel_msg_t m = {ZT_UPGRADE_STATUS, 0, false, d->k_s, ZK_SESSION};
    int seq = rel_send(d->rel, &m, pl, (uint16_t)w.len, d->now);
    if (state == UPG_INSTALLING) d->upg_seq = seq > 0 ? (uint32_t)seq : 0;
}

/* §12.1 step 4 (also the `verified` crash point of §12.4: re-verify the staged image, never re-download). */
void dev_upgrade_begin(device_t *d)
{
    char inactive = d->st.active_slot == 'B' ? 'A' : 'B', running[9];
    if (!slot_matches(d, inactive, d->st.fw_target, d->st.fw_sha256)) {
        LOGI(T_UPGRADE, "Staged image in slot %c does not verify: downloading it again", inactive);
        d->st.stage = ST_CONFIG_OK;
        dev_state_save(d->state_path, &d->st);
        dev_provision_next(d);
        return;
    }
    dev_running_fw(d, running);
    send_upgrade_status(d, UPG_INSTALLING, running);
    d->fsm = DS_UPGRADING;
    LOGD(T_UPGRADE, "UPGRADE_STATUS INSTALLING sent; committing once it is acknowledged");
}

/* §12.1 steps 5-6: once INSTALLING is ACKed, one atomic state write is the commit; then restart. */
void dev_upgrade_timer(device_t *d)
{
    if (d->fsm != DS_UPGRADING || !d->rel || !d->upg_seq || rel_acked_through(d->rel) < d->upg_seq) return;
    char inactive = d->st.active_slot == 'B' ? 'A' : 'B';
    d->st.active_slot = inactive;
    d->st.pending_reboot = 1;
    d->st.stage = ST_FW_INSTALLED;
    d->st.session_id = d->sid;
    d->st.epoch = d->epoch;
    if (dev_state_save(d->state_path, &d->st) != 0) {
        dev_fail(d, "storage error");
        return;
    }
    dev_crash_point(d, "committed");   /* the state says slot B and pending_reboot (§12.4) */
    LOGI(T_UPGRADE, LM_UPG_COMPLETE);
    dev_restart(d);
}

/* After the restart and resume (or a fresh registration): INSTALLED, then VALIDATE. */
void dev_upgrade_installed(device_t *d)
{
    char running[9];
    dev_running_fw(d, running);
    send_upgrade_status(d, UPG_INSTALLED, running);
    dev_validate_begin(d);
}

/* §12.5: the new firmware failed validation: switch back, report. */
void dev_upgrade_rollback(device_t *d)
{
    char bad[9], old[9];
    dev_running_fw(d, bad);
    d->st.active_slot = d->st.active_slot == 'B' ? 'A' : 'B';
    d->st.pending_reboot = 0;
    dev_state_save(d->state_path, &d->st);
    dev_running_fw(d, old);
    LOGI(T_UPGRADE, LM_UPG_ROLLED_BACK, bad, old);
    send_upgrade_status(d, UPG_ROLLED_BACK, old);
    dev_report_failed(d, RC_VALIDATION_FAILED, "VALIDATION_FAILED");
}

void dev_resolve_self(device_t *d, char **argv)
{
    d->argv = argv;
    char buf[PATH_MAX], real[PATH_MAX];
#ifdef __APPLE__
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0 || !realpath(buf, real)) snprintf(real, sizeof real, "%s", argv[0]);
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        snprintf(real, sizeof real, "%s", buf);
    } else if (!realpath(argv[0], real)) {
        snprintf(real, sizeof real, "%s", argv[0]);
    }
#endif
    d->self_path = strdup(real);
}

/* §12.2: restart the process: execv(self, the same argv), or exit(75) for a supervisor. */
void dev_restart(device_t *d)
{
    if (d->hook_restart_delay_s > 0) {   /* --restart-delay: e.g. outlive the server's grace period */
        LOGI(T_UPGRADE, "Restart delayed by %d s (test hook)", d->hook_restart_delay_s);
        sleep((unsigned)d->hook_restart_delay_s);
    }
    log_close();   /* flushed; the new process reopens it in append mode */
    fflush(NULL);
    if (d->restart_exit) exit(75);
    execv(d->self_path, d->argv);
    log_open(NULL, true, true);
    LOGW(T_ERROR, "execv %s: %s; exiting with 75 for a supervisor to restart", d->self_path, strerror(errno));
    exit(75);
}
