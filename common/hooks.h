/* Test hooks shared by both binaries (DESIGN §19.2). Every hook is refused unless --test-hooks
 * is also given, and logs "[ERROR] TEST HOOK ACTIVE: <name>" at startup. M2 part:
 *   --drop-seq tx:N[,N...]  drop the first transmission of each listed reliable TX seq
 *   --dup-seq tx:N          send one extra copy of the (first) retransmission of seq N */
#ifndef ZTP_HOOKS_H
#define ZTP_HOOKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOOK_MAX_SEQ 16

typedef struct {
    bool     enabled;                 /* --test-hooks given */
    uint32_t drop_seq[HOOK_MAX_SEQ];
    int      ndrop;
    uint32_t dup_seq;                 /* 0 = none */
    bool     dup_done;
    uint32_t dropped_mask;            /* bit i: drop_seq[i] already dropped */
} hooks_t;

/* Parse "tx:N[,N...]" into the list. Returns 0 or -1. */
int  hooks_parse_seq_list(const char *arg, uint32_t *out, int max, int *n);

enum hook_action { HOOK_SEND = 0, HOOK_DROP = 1, HOOK_SEND_TWICE = 2 };
/* Decide what to do with an outgoing frame (inspects its header). Reliable frames only. */
int  hooks_tx_action(hooks_t *h, const uint8_t *frame, size_t len);

#endif
