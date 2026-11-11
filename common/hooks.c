#include "hooks.h"

#include <stdlib.h>
#include <string.h>

#include "protocol.h"

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
                return HOOK_DROP;
            }
    if (h->dup_seq && seq == h->dup_seq && (flags & ZF_RETX) && !h->dup_done) {
        h->dup_done = true;
        return HOOK_SEND_TWICE;
    }
    return HOOK_SEND;
}
