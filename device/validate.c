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
