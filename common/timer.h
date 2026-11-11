/* Monotonic clock and a min-heap timer queue (DESIGN §4.4). Single-threaded. */
#ifndef ZTP_TIMER_H
#define ZTP_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TIMER_NONE UINT64_MAX

uint64_t now_ms(void);   /* CLOCK_MONOTONIC in milliseconds */
uint64_t wall_ms(void);  /* CLOCK_REALTIME in milliseconds (logs only) */

typedef void (*timer_cb)(void *ctx, uint32_t id, uint64_t now);

typedef struct {
    uint64_t deadline;
    uint32_t id;        /* unique, never 0 */
    timer_cb cb;
    void    *ctx;
} timer_ent_t;

typedef struct {
    timer_ent_t *heap;
    size_t       len, cap;
    uint32_t     next_id;
} timerq_t;

void     timerq_init(timerq_t *q);
void     timerq_free(timerq_t *q);
/* Schedule cb at deadline (absolute ms). Returns a timer id (> 0), or 0 on allocation failure. */
uint32_t timerq_add(timerq_t *q, uint64_t deadline, timer_cb cb, void *ctx);
/* Cancel by id. Returns true if it was pending. O(n). */
bool     timerq_cancel(timerq_t *q, uint32_t id);
/* Earliest deadline, or TIMER_NONE. */
uint64_t timerq_next(const timerq_t *q);
/* Fire every timer with deadline <= now, in deadline order. Callbacks may add or cancel timers.
 * Returns the number fired. */
size_t   timerq_run(timerq_t *q, uint64_t now);
/* poll() timeout in ms for the next deadline (-1 if none, capped at cap_ms if cap_ms >= 0). */
int      timerq_poll_timeout(const timerq_t *q, uint64_t now, int cap_ms);

#endif
