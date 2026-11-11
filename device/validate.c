#include "validate.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

int dev_config_check(const char *text, size_t len, int expected_version, char *err, size_t errsz)
{
    json_error_t je;
    json_doc_t *doc = json_parse(text, len, &je);
    if (!doc) {
        snprintf(err, errsz, "config.json: line %u: %s", je.line, je.msg);
        return DEV_CFG_MALFORMED;
    }
    json_value_t *root = json_root(doc);
    int rc = DEV_CFG_OK;
    static const struct { const char *key; json_type_t type; } REQ[] = {
        {"interfaces", JOBJECT}, {"mtu", JINT}, {"keepalive_interval", JINT}, {"config_version", JINT},
    };
    if (root->type != JOBJECT) {
        snprintf(err, errsz, "config.json: top level must be an object");
        rc = DEV_CFG_MALFORMED;
        goto out;
    }
    for (size_t i = 0; i < sizeof REQ / sizeof REQ[0]; i++) {
        json_value_t *v = json_get(root, REQ[i].key);
        if (!v) {
            snprintf(err, errsz, "config.json: missing key %s", REQ[i].key);
            rc = DEV_CFG_MISSING;
            goto out;
        }
        if (v->type != REQ[i].type) {
            snprintf(err, errsz, "config.json: %s must be of type %s (is %s)", REQ[i].key,
                     json_type_name(REQ[i].type), json_type_name(v->type));
            rc = DEV_CFG_INVALID;
            goto out;
        }
    }
    long long ver = json_get(root, "config_version")->u.i;
    if (ver != expected_version) {
        snprintf(err, errsz, "config.json: config_version %lld, assigned %d", ver, expected_version);
        rc = DEV_CFG_INVALID;
    }
out:
    json_free(doc);
    return rc;
}

/* ---- the validation flow (§13, §13.1); device_t-dependent part ---- */

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "dev.h"
#include "log.h"
#include "logmsg.h"
#include "util.h"

static void send_operational(device_t *d)
{
    char fw[9];
    dev_running_fw(d, fw);
    uint8_t pl[48];
    tlv_writer_t w;
    tlv_writer_init(&w, pl, sizeof pl);
    tlv_put_u8(&w, TAG_REPORT_STATE, RPT_OPERATIONAL);
    tlv_put_u16(&w, TAG_REASON, RC_OK);
    tlv_put_str(&w, TAG_FW_VER, fw);
    tlv_put_u16(&w, TAG_CFG_VER, (uint16_t)d->st.cfg_version);
    rel_msg_t m = {ZT_STATUS_REPORT, 0, false, d->k_s, ZK_SESSION};
    rel_send(d->rel, &m, pl, (uint16_t)w.len, d->now);
    d->report_pending = true;
}

/* Check 1: config.json present, its sha equal to the stored one, required keys, the assigned version. */
static bool config_ok(device_t *d, char *why, size_t n)
{
    char path[CFG_PATH + 32], *text = NULL, hex[65];
    size_t len = 0;
    snprintf(path, sizeof path, "%s/config.json", d->cfg.c.paths.state_dir);
    if (read_file(path, 1u << 20, false, &text, &len) != 0) {
        snprintf(why, n, "config.json missing");
        return false;
    }
    uint8_t s[32];
    sha256(text, len, s);
    hex_encode(s, 32, hex);
    int rc = dev_config_check(text, len, d->st.cfg_version, why, n);
    free(text);
    if (*d->st.cfg_sha256 && strcmp(hex, d->st.cfg_sha256)) {
        snprintf(why, n, "config.json sha256 differs from the verified download");
        return false;
    }
    return rc == DEV_CFG_OK;
}

void dev_validate_begin(device_t *d)
{
    d->fsm = DS_VALIDATING;
    d->report_pending = false;
    d->health_seq = 0;
    if (!d->val_deadline) d->val_deadline = d->now + (uint64_t)d->cfg.c.val.deadline_s * 1000u;
    if (d->hooks.enabled && d->hook_delete_config && hooks_fire_once(&d->hooks, "hook-delete-config")) {
        char p[CFG_PATH + 32];
        snprintf(p, sizeof p, "%s/config.json", d->cfg.c.paths.state_dir);
        unlink(p);
    }
    if (d->hooks.enabled && *d->hook_bad_fw_version && hooks_fire_once(&d->hooks, "hook-bad-fw-version")) {
        char p[CFG_PATH + 32], v[8] = {0};
        snprintf(p, sizeof p, "%s/fw_slot_%c.bin", d->cfg.c.paths.state_dir, d->st.active_slot);
        memcpy(v, d->hook_bad_fw_version, strlen(d->hook_bad_fw_version) < 8 ? strlen(d->hook_bad_fw_version) : 8);
        int fd = open(p, O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {   /* the version field of the active slot's header (§11.1) */
            if (pwrite(fd, v, 8, 23) != 8) LOGW(T_ERROR, "hook-bad-fw-version: write failed");
            close(fd);
        } else {
            d->hook_fw_override = true;   /* factory image (no slot file): report V as the running version */
        }
    }
    char why[160];
    if (!config_ok(d, why, sizeof why)) {
        if (!d->cfg_refetched && !d->hook_no_refetch) {   /* one re-fetch of the config (§13) */
            d->cfg_refetched = true;
            LOGI(T_VALIDATION, "Configuration check failed (%s): fetching it again", why);
            d->st.stage = ST_REGISTERED;
            dev_state_save(d->state_path, &d->st);
            dev_provision_next(d);
            return;
        }
        LOGI(T_VALIDATION, "Configuration check failed (%s)", why);
        dev_report_failed(d, RC_VALIDATION_FAILED, "config missing or invalid");
        return;
    }
    LOGI(T_VALIDATION, LM_VAL_CONFIG_PRESENT " (v%d, sha ok)", d->st.cfg_version);

    /* Check 2: the active slot's version is the target (and, after an upgrade, its hash is the verified one) */
    char running[9], model[17];
    int cmp = -1;
    dev_running_fw(d, running);
    if (d->hook_fw_override) snprintf(running, sizeof running, "%s", d->hook_bad_fw_version);
    /* after an upgrade: exactly the target; otherwise still the version the firmware decision approved */
    bool ok = version_cmp(running, d->st.fw_target, &cmp) == 0 &&
              (d->upgraded ? cmp == 0 : cmp >= 0 && (!*d->fw_approved || !strcmp(running, d->fw_approved)));
    if (ok && d->upgraded) {
        char p[CFG_PATH + 32], *data = NULL, hex[65];
        size_t len = 0;
        uint8_t s[32];
        snprintf(p, sizeof p, "%s/fw_slot_%c.bin", d->cfg.c.paths.state_dir, d->st.active_slot);
        ok = dev_slot_version(d, d->st.active_slot, running, model) && read_file(p, 64u << 20, false, &data, &len) == 0;
        if (ok) {
            sha256(data, len, s);
            hex_encode(s, 32, hex);
            ok = !*d->st.fw_sha256 || !strcmp(hex, d->st.fw_sha256);
        }
        free(data);
    }
    if (ok && d->hooks.enabled && d->hook_fail_validate && d->upgraded && hooks_fire_once(&d->hooks, "fail-validate")) ok = false;
    if (!ok) {
        LOGI(T_VALIDATION, "Firmware check failed: running %s, target %s", running, d->st.fw_target);
        if (d->upgraded) dev_upgrade_rollback(d);   /* §12.5 */
        else dev_report_failed(d, RC_VALIDATION_FAILED, "firmware version mismatch");
        return;
    }
    LOGI(T_VALIDATION, LM_VAL_FW_VERIFIED, running);

    /* Check 3: a fresh round trip (HEALTH_PROBE), retransmitted on the RTO schedule until the deadline */
    dev_send_keepalive(d, KA_HEALTH_PROBE);
}

void dev_validate_timer(device_t *d)
{
    if (d->fsm != DS_VALIDATING || !d->rel || d->report_pending) return;
    if (d->health_seq && rel_acked_through(d->rel) >= d->health_seq) {
        uint32_t rtt = (uint32_t)(d->now - d->health_sent), srtt = rel_stats(d->rel)->srtt_ms;
        d->health_seq = 0;
        if (rtt <= 2 * rel_rto_ms(d->rel) && srtt < (uint32_t)d->cfg.c.val.srtt_max_ms) {
            LOGI(T_VALIDATION, LM_VAL_SESSION_HEALTHY, (unsigned)rtt);
            send_operational(d);
            return;
        }
        LOGI(T_VALIDATION, "Session not healthy yet (rtt=%u ms, srtt=%u ms): probing again", (unsigned)rtt, (unsigned)srtt);
        dev_send_keepalive(d, KA_HEALTH_PROBE);
    }
    if (d->now >= d->val_deadline) {
        LOGI(T_VALIDATION, "Session not healthy within val.deadline_s (%d s)", d->cfg.c.val.deadline_s);
        dev_report_failed(d, RC_VALIDATION_FAILED, "session unhealthy");
    }
}

uint64_t dev_validate_deadline(const device_t *d)
{
    return d->fsm == DS_VALIDATING && !d->report_pending ? d->val_deadline : UINT64_MAX;
}

/* E19: STATUS_ACK for OPERATIONAL. */
void dev_validate_on_status_ack(device_t *d)
{
    if (!d->report_pending || d->fail_after) return;
    d->report_pending = false;
    d->st.stage = ST_OPERATIONAL;
    d->st.pending_reboot = 0;
    if (dev_state_save(d->state_path, &d->st) != 0) LOGW(T_ERROR, "cannot persist state");
    d->fsm = DS_OPERATIONAL;
    d->val_deadline = 0;
    LOGI(T_STATE, LM_STATE_OPERATIONAL);
    if (d->cfg.c.run.exit_when_operational)
        d->hold_until = d->now + (uint64_t)d->cfg.c.run.operational_hold_s * 1000u;
}
