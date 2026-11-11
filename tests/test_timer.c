/* Timer queue: ordering, FIFO for equal deadlines, cancel, re-arm from a callback, poll timeout. */
#include <stdlib.h>

#include "test.h"
#include "timer.h"

static uint32_t fired[64];
static int nfired;

static void cb(void *ctx, uint32_t id, uint64_t now)
{
    (void)now;
    fired[nfired++] = (uint32_t)(uintptr_t)ctx;
    (void)id;
}

static timerq_t *g_q;
static void rearm(void *ctx, uint32_t id, uint64_t now)
{
    (void)id;
    fired[nfired++] = (uint32_t)(uintptr_t)ctx;
    if ((uintptr_t)ctx < 3) timerq_add(g_q, now + 10, rearm, (void *)((uintptr_t)ctx + 1));
}

void suite_timer(void)
{
    timerq_t q;
    timerq_init(&q);
    CHECK(timerq_next(&q) == TIMER_NONE);
    CHECK_EQ_INT(timerq_poll_timeout(&q, 0, -1), -1);
    CHECK_EQ_INT(timerq_poll_timeout(&q, 0, 250), 250);

    /* random deadlines fire in sorted order */
    uint64_t dl[40];
    for (int i = 0; i < 40; i++) {
        dl[i] = (uint64_t)((i * 7919) % 97);
        timerq_add(&q, dl[i], cb, (void *)(uintptr_t)dl[i]);
    }
    nfired = 0;
    CHECK_EQ_INT(timerq_run(&q, 1000), 40);
    int sorted = 1;
    for (int i = 1; i < nfired; i++) sorted &= fired[i - 1] <= fired[i];
    CHECK(sorted);

    /* FIFO among equal deadlines; cancel removes exactly one */
    uint32_t a = timerq_add(&q, 50, cb, (void *)1);
    uint32_t b = timerq_add(&q, 50, cb, (void *)2);
    uint32_t c = timerq_add(&q, 50, cb, (void *)3);
    CHECK(a && b && c && a != b);
    CHECK(timerq_cancel(&q, b));
    CHECK(!timerq_cancel(&q, b));
    CHECK_EQ_INT(timerq_poll_timeout(&q, 20, -1), 30);
    CHECK_EQ_INT(timerq_poll_timeout(&q, 20, 10), 10);
    CHECK_EQ_INT(timerq_poll_timeout(&q, 60, -1), 0);
    nfired = 0;
    CHECK_EQ_INT(timerq_run(&q, 49), 0);
    CHECK_EQ_INT(timerq_run(&q, 50), 2);
    CHECK(fired[0] == 1 && fired[1] == 3);

    /* callbacks may schedule new timers; they fire only when due */
    g_q = &q;
    timerq_add(&q, 100, rearm, (void *)1);
    nfired = 0;
    CHECK_EQ_INT(timerq_run(&q, 100), 1);   /* re-arms at now + 10 = 110 */
    CHECK_EQ_INT(timerq_run(&q, 109), 0);
    CHECK_EQ_INT(timerq_run(&q, 110), 1);   /* re-arms at 120 */
    CHECK_EQ_INT(timerq_run(&q, 120), 1);
    CHECK(nfired == 3 && fired[2] == 3);
    CHECK(timerq_next(&q) == TIMER_NONE);

    /* monotonic clock moves forward */
    uint64_t t0 = now_ms();
    CHECK(now_ms() >= t0 && wall_ms() > 1700000000000ull);
    timerq_free(&q);
}
