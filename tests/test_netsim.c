/* TC-UNIT-15: netsim determinism and rates; plus the real send path over loopback UDP
 * (drop, hold-and-flush for reorder/delay, duplicate, --sim-mtu EMSGSIZE and black hole). */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <unistd.h>

#include "log.h"
#include "netsim.h"
#include "test.h"

static void test_determinism(void)
{
    netsim_cfg_t cfg = {.loss = 0.15, .reorder = 0.1, .dup = 0.05, .delay_ms = 2, .jitter_ms = 1, .seed = 42};
    netsim_t a, b;
    netsim_init(&a, &cfg);
    netsim_init(&b, &cfg);
    int diff = 0;
    for (int i = 0; i < 10000; i++) {
        netsim_decision_t x = netsim_decide(&a, 100), y = netsim_decide(&b, 100);
        diff += memcmp(&x, &y, sizeof x) != 0;
    }
    CHECK_EQ_INT(diff, 0);
    netsim_free(&a);
    netsim_free(&b);

    /* a different seed gives a different sequence */
    netsim_cfg_t c2 = cfg;
    c2.seed = 43;
    netsim_init(&a, &cfg);
    netsim_init(&b, &c2);
    int same = 0;
    for (int i = 0; i < 1000; i++) {
        netsim_decision_t x = netsim_decide(&a, 100), y = netsim_decide(&b, 100);
        same += x.drop == y.drop && x.reordered == y.reordered && x.dup == y.dup && x.delay_ms == y.delay_ms;
    }
    CHECK(same < 1000);
    netsim_free(&a);
    netsim_free(&b);
}

static void test_rates(void)
{
    netsim_cfg_t cfg = {.loss = 0.15, .reorder = 0.10, .dup = 0.05, .seed = 7};
    netsim_t ns;
    netsim_init(&ns, &cfg);
    const int N = 100000;
    int drop = 0, reo = 0, dup = 0, delayed = 0;
    for (int i = 0; i < N; i++) {
        netsim_decision_t d = netsim_decide(&ns, 500);
        drop += d.drop;
        if (!d.drop) {
            reo += d.reordered;
            dup += d.dup;
            delayed += d.delay_ms == 30;
            if (d.dup && (d.dup_delay_ms < d.delay_ms || d.dup_delay_ms > d.delay_ms + 10)) CHECK(!"dup delay out of 0..10 ms");
        }
    }
    double pl = (double)drop / N;
    CHECK(fabs(pl - 0.15) <= 0.01);   /* 15% ± 1% */
    double survivors = N - drop;
    CHECK(fabs(reo / survivors - 0.10) <= 0.01);
    CHECK(fabs(dup / survivors - 0.05) <= 0.01);
    CHECK_EQ_INT(delayed, reo);       /* reordered ones are held reorder_delay_ms (30) */
    netsim_free(&ns);

    /* jitter stays within ±J */
    netsim_cfg_t j = {.delay_ms = 5, .jitter_ms = 2, .seed = 1};
    netsim_init(&ns, &j);
    int lo = 1000, hi = -1;
    for (int i = 0; i < 10000; i++) {
        int dl = (int)netsim_decide(&ns, 10).delay_ms;
        if (dl < lo) lo = dl;
        if (dl > hi) hi = dl;
    }
    CHECK(lo == 3 && hi == 7);
    netsim_free(&ns);

    /* inactive when every knob is zero */
    netsim_cfg_t z = {0};
    netsim_init(&ns, &z);
    CHECK(!netsim_is_active(&ns));
    netsim_free(&ns);
}

static int udp_pair(int *tx, int *rx, struct sockaddr_in *to)
{
    *rx = socket(AF_INET, SOCK_DGRAM, 0);
    *tx = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(*rx, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    socklen_t l = sizeof a;
    getsockname(*rx, (struct sockaddr *)&a, &l);
    *to = a;
    fcntl(*rx, F_SETFL, O_NONBLOCK);
    return 0;
}

static int drain(int rx, uint8_t *last)
{
    int n = 0;
    uint8_t b[2000];
    usleep(20000);
    while (recv(rx, b, sizeof b, 0) > 0) {
        if (last) *last = b[0];
        n++;
    }
    return n;
}

static void test_send_path(void)
{
    int tx, rx;
    struct sockaddr_in to;
    if (udp_pair(&tx, &rx, &to) != 0) {
        CHECK(!"cannot bind loopback UDP");
        return;
    }
    uint8_t msg[1300] = {0};
    netsim_t ns;

    /* EMSGSIZE above --sim-mtu */
    netsim_cfg_t m = {.mtu = 576, .mtu_mode = NS_MTU_EMSGSIZE, .seed = 1};
    netsim_init(&ns, &m);
    errno = 0;
    CHECK(netsim_sendto(&ns, tx, msg, 1076, 0, (struct sockaddr *)&to, sizeof to, 0) == -1 && errno == EMSGSIZE);
    CHECK(netsim_sendto(&ns, tx, msg, 308, 0, (struct sockaddr *)&to, sizeof to, 0) == 308);
    CHECK_EQ_INT(drain(rx, NULL), 1);
    CHECK_EQ_INT(ns.st.emsgsize, 1);
    netsim_free(&ns);

    /* black hole: reports success, nothing arrives */
    m.mtu_mode = NS_MTU_BLACKHOLE;
    netsim_init(&ns, &m);
    CHECK(netsim_sendto(&ns, tx, msg, 1076, 0, (struct sockaddr *)&to, sizeof to, 0) == 1076);
    CHECK_EQ_INT(drain(rx, NULL), 0);
    CHECK_EQ_INT(ns.st.mtu_dropped, 1);
    netsim_free(&ns);

    /* delay: held until flushed at the due time, in due order */
    netsim_cfg_t d = {.delay_ms = 10, .seed = 1};
    netsim_init(&ns, &d);
    msg[0] = 1;
    netsim_sendto(&ns, tx, msg, 50, 0, (struct sockaddr *)&to, sizeof to, 1000);
    msg[0] = 2;
    netsim_sendto(&ns, tx, msg, 50, 0, (struct sockaddr *)&to, sizeof to, 1005);
    CHECK_EQ_INT(drain(rx, NULL), 0);
    CHECK_EQ_INT((long long)netsim_next_deadline(&ns), 1010);
    CHECK_EQ_INT(netsim_flush(&ns, 1009), 0);
    CHECK_EQ_INT(netsim_flush(&ns, 1010), 1);
    uint8_t last = 0;
    CHECK_EQ_INT(drain(rx, &last), 1);
    CHECK_EQ_INT(last, 1);
    CHECK_EQ_INT(netsim_flush(&ns, 2000), 1);
    CHECK_EQ_INT(drain(rx, &last), 1);
    CHECK_EQ_INT(last, 2);
    CHECK(netsim_next_deadline(&ns) == UINT64_MAX);
    netsim_free(&ns);

    /* 100% duplicate: two copies arrive */
    netsim_cfg_t du = {.dup = 1.0, .seed = 3};
    netsim_init(&ns, &du);
    netsim_sendto(&ns, tx, msg, 50, 0, (struct sockaddr *)&to, sizeof to, 0);
    netsim_flush(&ns, 100);
    CHECK_EQ_INT(drain(rx, NULL), 2);
    netsim_free(&ns);

    /* 100% loss: nothing arrives, totals line logged */
    netsim_cfg_t lo = {.loss = 1.0, .seed = 3};
    netsim_init(&ns, &lo);
    for (int i = 0; i < 10; i++) netsim_sendto(&ns, tx, msg, 50, 0, (struct sockaddr *)&to, sizeof to, 0);
    CHECK_EQ_INT(drain(rx, NULL), 0);
    CHECK(ns.st.tx == 10 && ns.st.dropped == 10);
    char path[300];
    snprintf(path, sizeof path, "%s/sim.log", test_tmpdir());
    FILE *f = fopen(path, "w+");
    log_set_stream(f);
    netsim_log_totals(&ns);
    log_set_stream(NULL);
    rewind(f);
    char line[256] = "";
    CHECK(fgets(line, sizeof line, f) != NULL);
    fclose(f);
    CHECK(strstr(line, "[SIM] tx=10 dropped=10 (100.0%) reordered=0 duplicated=0 rx=0") != NULL);
    netsim_free(&ns);

    /* pass-through when inactive */
    netsim_cfg_t z = {0};
    netsim_init(&ns, &z);
    CHECK(netsim_sendto(&ns, tx, msg, 1200, 0, (struct sockaddr *)&to, sizeof to, 0) == 1200);
    CHECK_EQ_INT(drain(rx, NULL), 1);
    netsim_free(&ns);
    close(tx);
    close(rx);
}

void suite_netsim(void)
{
    test_determinism();
    test_rates();
    test_send_path();
}
