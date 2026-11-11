/* M1 platform spike (PRD R-03): can a datagram sent to a multicast group or a broadcast address
 * be received by another socket on the same host? Exercises exactly the socket options the
 * discovery code will use (§6.2).
 *   netspike mcast [GROUP] [IFACE_ADDR]    default 239.255.90.90 via 127.0.0.1
 *   netspike bcast ADDR                     e.g. the interface's directed broadcast address
 * Exit 0 if at least one datagram arrives within 2 s, 1 otherwise. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 5599

static int die(const char *what)
{
    fprintf(stderr, "netspike: %s: %s\n", what, strerror(errno));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "mcast") && strcmp(argv[1], "bcast"))) {
        fprintf(stderr, "usage: netspike mcast [GROUP] [IFACE_ADDR] | bcast ADDR\n");
        return 2;
    }
    int mcast = !strcmp(argv[1], "mcast");
    const char *dst = mcast ? (argc > 2 ? argv[2] : "239.255.90.90") : (argc > 2 ? argv[2] : "255.255.255.255");
    const char *ifa = mcast && argc > 3 ? argv[3] : "127.0.0.1";

    int rx = socket(AF_INET, SOCK_DGRAM, 0), tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0 || tx < 0) return die("socket");
    int one = 1;
    setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(rx, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    any.sin_port = htons(PORT);
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(rx, (struct sockaddr *)&any, sizeof any) != 0) return die("bind");

    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(PORT);
    if (inet_pton(AF_INET, dst, &to.sin_addr) != 1) { fprintf(stderr, "bad address %s\n", dst); return 2; }

    if (mcast) {
        struct ip_mreq mr;
        inet_pton(AF_INET, dst, &mr.imr_multiaddr);
        inet_pton(AF_INET, ifa, &mr.imr_interface);
        if (setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) != 0) return die("IP_ADD_MEMBERSHIP");
        struct in_addr ia;
        inet_pton(AF_INET, ifa, &ia);
        if (setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia) != 0) return die("IP_MULTICAST_IF");
        unsigned char ttl = 1, loop = 1;
        setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        setsockopt(tx, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    } else if (setsockopt(tx, SOL_SOCKET, SO_BROADCAST, &one, sizeof one) != 0) {
        return die("SO_BROADCAST");
    }

    int got = 0;
    for (int i = 0; i < 5 && !got; i++) {
        char msg[32];
        int n = snprintf(msg, sizeof msg, "ztp-spike %d", i);
        if (sendto(tx, msg, (size_t)n, 0, (struct sockaddr *)&to, sizeof to) < 0) return die("sendto");
        struct pollfd p = {rx, POLLIN, 0};
        if (poll(&p, 1, 400) > 0) {
            char buf[64];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t r = recvfrom(rx, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
            if (r > 0) {
                buf[r] = '\0';
                char fa[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, fa, sizeof fa);
                printf("netspike %s %s: received \"%s\" from %s:%u\n", argv[1], dst, buf, fa, ntohs(from.sin_port));
                got = 1;
            }
        }
    }
    if (!got) printf("netspike %s %s: nothing received\n", argv[1], dst);
    close(rx);
    close(tx);
    return got ? 0 : 1;
}
