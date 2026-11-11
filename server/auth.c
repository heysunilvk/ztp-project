#include "auth.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "json.h"
#include "util.h"

static int fail(char *err, size_t n, const char *fmt, const char *a, const char *b)
{
    snprintf(err, n, fmt, a, b);
    return EX_CONFIG_CODE;
}

int srv_keys_load(const char *path, srv_keys_t *k, char *err, size_t errsz)
{
    memset(k, 0, sizeof *k);
    struct stat st;
    if (stat(path, &st) != 0) return fail(err, errsz, "keys: %s: %s", path, strerror(errno));
    if (st.st_mode & 077) {
        char mode[8];
        snprintf(mode, sizeof mode, "%04o", (unsigned)(st.st_mode & 0777));
        return fail(err, errsz, "keys: %s: must be mode 0600 (is %s)", path, mode);
    }
    char *text = NULL;
    size_t len = 0;
    if (read_file(path, JSON_MAX_INPUT, false, &text, &len) != 0)
        return fail(err, errsz, "keys: %s: cannot read: %s", path, strerror(errno));
    json_error_t je;
    json_doc_t *doc = json_parse(text, len, &je);
    secure_zero(text, len);
    free(text);
    if (!doc) {
        snprintf(err, errsz, "keys: %s:%u: %s", path, je.line, je.msg);
        return EX_CONFIG_CODE;
    }
    int rc = 0;
    json_value_t *root = json_root(doc);
    if (root->type != JOBJECT) { rc = fail(err, errsz, "keys: %s: %s", path, "top level must be an object"); goto out; }
    for (json_value_t *m = root->u.obj.first; m; m = m->next) {
        if (m->key[0] == '_') continue;
        if (!strcmp(m->key, "schema_version")) {
            if (m->type != JINT || m->u.i != 1) { rc = fail(err, errsz, "keys: %s: %s", path, "schema_version must be 1"); goto out; }
        } else if (!strcmp(m->key, "fleet")) {
            if (m->type != JSTRING || hex_decode(m->u.str.s, k->k_fleet, ZTP_KEY_LEN) != 0) {
                rc = fail(err, errsz, "keys: %s: %s", path, "fleet must be 64 hex digits");
                goto out;
            }
            ztp_derive_kboot(k->k_fleet, k->k_boot);
        } else if (!strcmp(m->key, "models")) {
            if (m->type != JOBJECT) { rc = fail(err, errsz, "keys: %s: %s", path, "models must be an object"); goto out; }
            k->models = calloc(m->u.obj.count ? m->u.obj.count : 1, sizeof *k->models);
            if (!k->models) { rc = fail(err, errsz, "keys: %s: %s", path, "out of memory"); goto out; }
            for (json_value_t *e = m->u.obj.first; e; e = e->next) {
                if (e->key[0] == '_') continue;
                srv_model_key_t *mk = &k->models[k->nmodels];
                if (strlen(e->key) == 0 || !str_copy(mk->model, sizeof mk->model, e->key)) {
                    rc = fail(err, errsz, "keys: %s: model name \"%s\" must be 1..16 characters", path, e->key);
                    goto out;
                }
                if (e->type != JSTRING || hex_decode(e->u.str.s, mk->k_mfg, ZTP_KEY_LEN) != 0) {
                    rc = fail(err, errsz, "keys: %s: models.%s must be 64 hex digits", path, e->key);
                    goto out;
                }
                k->nmodels++;
            }
        } else {
            rc = fail(err, errsz, "keys: %s: unknown key %s", path, m->key);
            goto out;
        }
    }
    if (!json_get(root, "fleet")) rc = fail(err, errsz, "keys: %s: %s", path, "missing fleet key");
out:
    json_free(doc);
    if (rc) srv_keys_free(k);
    return rc;
}

void srv_keys_free(srv_keys_t *k)
{
    if (k->models) {
        secure_zero(k->models, k->nmodels * sizeof *k->models);
        free(k->models);
    }
    secure_zero(k, sizeof *k);
}

const uint8_t *srv_keys_model(const srv_keys_t *k, const char *model)
{
    for (size_t i = 0; i < k->nmodels; i++)
        if (!strcmp(k->models[i].model, model)) return k->models[i].k_mfg;
    return NULL;
}
