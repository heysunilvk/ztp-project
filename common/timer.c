#include "timer.h"

#include <stdlib.h>
#include <time.h>

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void timerq_init(timerq_t *q)
{
    q->heap = NULL;
    q->len = q->cap = 0;
    q->next_id = 1;
}

void timerq_free(timerq_t *q)
{
    free(q->heap);
    timerq_init(q);
}

/* Ordering: deadline, then id (FIFO among equal deadlines since ids increase). */
static int before(const timer_ent_t *a, const timer_ent_t *b)
{
    if (a->deadline != b->deadline) return a->deadline < b->deadline;
    return (int32_t)(a->id - b->id) < 0;
}

static void swap(timer_ent_t *a, timer_ent_t *b)
{
    timer_ent_t t = *a;
    *a = *b;
    *b = t;
}

static void sift_up(timerq_t *q, size_t i)
{
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!before(&q->heap[i], &q->heap[p])) break;
        swap(&q->heap[i], &q->heap[p]);
        i = p;
    }
}

static void sift_down(timerq_t *q, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < q->len && before(&q->heap[l], &q->heap[m])) m = l;
        if (r < q->len && before(&q->heap[r], &q->heap[m])) m = r;
        if (m == i) break;
        swap(&q->heap[i], &q->heap[m]);
        i = m;
    }
}

uint32_t timerq_add(timerq_t *q, uint64_t deadline, timer_cb cb, void *ctx)
{
    if (q->len == q->cap) {
        size_t nc = q->cap ? q->cap * 2 : 16;
        timer_ent_t *nh = realloc(q->heap, nc * sizeof *nh);
        if (!nh) return 0;
        q->heap = nh;
        q->cap = nc;
    }
    uint32_t id = q->next_id++;
    if (q->next_id == 0) q->next_id = 1;
    q->heap[q->len] = (timer_ent_t){deadline, id, cb, ctx};
    sift_up(q, q->len++);
    return id;
}

static void remove_at(timerq_t *q, size_t i)
{
    q->len--;
    if (i == q->len) return;
    q->heap[i] = q->heap[q->len];
    sift_down(q, i);
    sift_up(q, i);
}

bool timerq_cancel(timerq_t *q, uint32_t id)
{
    for (size_t i = 0; i < q->len; i++)
        if (q->heap[i].id == id) {
            remove_at(q, i);
            return true;
        }
    return false;
}

uint64_t timerq_next(const timerq_t *q)
{
    return q->len ? q->heap[0].deadline : TIMER_NONE;
}

size_t timerq_run(timerq_t *q, uint64_t now)
{
    size_t fired = 0;
    while (q->len && q->heap[0].deadline <= now) {
        timer_ent_t e = q->heap[0];
        remove_at(q, 0);
        e.cb(e.ctx, e.id, now);
        fired++;
    }
    return fired;
}

int timerq_poll_timeout(const timerq_t *q, uint64_t now, int cap_ms)
{
    uint64_t d = timerq_next(q);
    if (d == TIMER_NONE) return cap_ms >= 0 ? cap_ms : -1;
    uint64_t wait = d > now ? d - now : 0;
    if (cap_ms >= 0 && wait > (uint64_t)cap_ms) wait = (uint64_t)cap_ms;
    if (wait > 0x7FFFFFFF) wait = 0x7FFFFFFF;
    return (int)wait;
}
