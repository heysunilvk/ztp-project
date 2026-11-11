#include "hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "logmsg.h"
#include "protocol.h"
#include "util.h"

void hooks_set_marker(hooks_t *h, const char *path)
{
    snprintf(h->marker, sizeof h->marker, "%s", path);
}

static bool fired(const hooks_t *h, const char *name)
{
    char *text = NULL;
    size_t len = 0;
    if (!*h->marker || read_file(h->marker, 1u << 16, false, &text, &len) != 0) return false;
    bool found = false;
    for (char *line = strtok(text, "\n"); line && !found; line = strtok(NULL, "\n")) found = !strcmp(line, name);
    free(text);
    return found;
}

bool hooks_fire_once(hooks_t *h, const char *name)
{
    if (fired(h, name)) return false;
    if (*h->marker) {   /* written before the hook acts, so a crash hook can't fire twice */
        char *text = NULL, *buf;
        size_t len = 0;
        if (read_file(h->marker, 1u << 16, false, &text, &len) != 0) len = 0;
        buf = malloc(len + strlen(name) + 2);
        if (buf) {
            if (len) memcpy(buf, text, len);
            snprintf(buf + len, strlen(name) + 2, "%s\n", name);
            atomic_write_file(h->marker, buf, len + strlen(name) + 1);
            free(buf);
        }
        free(text);
    }
    LOGI(T_ERROR, "TEST HOOK FIRED: %s", name);
    return true;
}

int hooks_parse_seq_list(const char *arg, uint32_t *out, int max, int *n)
{
    if (strncmp(arg, "tx:", 3) != 0) return -1;
    const char *p = arg + 3;
    *n = 0;
    while (*p) {
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p || v == 0 || v > 0xFFFFFFFFul || *n >= max) return -1;
        out[(*n)++] = (uint32_t)v;
        if (*end == ',') p = end + 1;
        else if (*end == '\0') p = end;
        else return -1;
    }
    return *n ? 0 : -1;
}

int hooks_tx_action(hooks_t *h, const uint8_t *frame, size_t len)
{
    if (!h || !h->enabled || len < ZTP_HDR_LEN) return HOOK_SEND;
    uint8_t type = frame[3], flags = frame[4];
    const ztp_type_info_t *ti = ztp_type_info(type);
    if (!ti || ti->cls != ZC_RELIABLE) return HOOK_SEND;
    uint32_t seq = be32_get(frame + 12);
    if (!(flags & ZF_RETX))
        for (int i = 0; i < h->ndrop; i++)
            if (h->drop_seq[i] == seq && !(h->dropped_mask & (1u << i))) {
                h->dropped_mask |= 1u << i;
                char name[48];
                snprintf(name, sizeof name, "drop-seq tx:%u", (unsigned)seq);
                return hooks_fire_once(h, name) ? HOOK_DROP : HOOK_SEND;
            }
    if (h->dup_seq && seq == h->dup_seq && (flags & ZF_RETX) && !h->dup_done) {
        h->dup_done = true;
        char name[48];
        snprintf(name, sizeof name, "dup-seq tx:%u", (unsigned)seq);
        return hooks_fire_once(h, name) ? HOOK_SEND_TWICE : HOOK_SEND;
    }
    return HOOK_SEND;
}
