/* Policy engine (DESIGN §14.3, §25.6): firmware/config assignment and per-device NAT/session
 * overrides. Resolution: serial > model > default for assignments; serial > model > tenant >
 * the server's effective nat.* for NAT values, merged key by key. */
#ifndef SRV_POLICY_H
#define SRV_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#include "auth.h"
#include "config.h"

#define POL_NAME   33
#define POL_PATH   256

typedef struct {
    bool has_keepalive, has_dead, has_idle, has_grace, has_allow_ip_change;
    int  keepalive_interval_s, dead_intervals, server_idle_intervals, grace_s;
    bool allow_ip_change;
} srv_nat_ovr_t;

typedef struct {
    char name[POL_NAME];   /* tenant, model or serial */
    bool has_firmware, has_firmware_file, has_config_version, has_config_file, has_tenant, has_notes;
    char firmware[9], firmware_file[POL_PATH], config_file[POL_PATH], tenant[POL_NAME], notes[65];
    int  config_version;
    srv_nat_ovr_t nat;
    unsigned line;         /* where it was defined, for messages */
} srv_pol_entry_t;

typedef struct {
    char path[POL_PATH];
    bool default_assign;          /* "default": {"action": "assign", ...}; false = reject */
    srv_pol_entry_t dflt;
    srv_pol_entry_t *tenants, *models, *serials;
    size_t ntenants, nmodels, nserials;
} srv_policy_t;

typedef struct {
    bool assigned;                /* false: NO_POLICY */
    char firmware[9], firmware_file[POL_PATH], config_file[POL_PATH], tenant[POL_NAME], notes[65];
    int  config_version;
    ztp_nat_override_t nat;       /* effective per-device values */
    char src_keepalive[64], src_dead[64], src_idle[64], src_grace[64], src_allow_ip_change[64];
} srv_assignment_t;

/* Parse policy.json (strict: unknown keys are errors). Returns 0, or EX_CONFIG_CODE with err set. */
int  srv_policy_load(const char *path, srv_policy_t *p, char *err, size_t errsz);
void srv_policy_free(srv_policy_t *p);

/* Resolve one device. serial may be "" (model-level resolution). */
void srv_policy_resolve(const srv_policy_t *p, const cfg_t *cfg, const char *model, const char *serial,
                        srv_assignment_t *out);

/* Startup checks (§25.8): referenced tenants exist; every policy model has a key (if keys is
 * non-NULL); every resolved NAT block passes the range rules. 0 or EX_CONFIG_CODE. */
int srv_policy_validate(const srv_policy_t *p, const cfg_t *cfg, const srv_keys_t *keys, char *err,
                        size_t errsz);
/* Every firmware_file exists (regular, non-empty) and every config_file parses as a JSON object. */
int srv_policy_check_files(const srv_policy_t *p, char *err, size_t errsz);

#endif
