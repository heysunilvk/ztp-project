#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int udp_open(const char *ip, uint16_t port, bool reuse, char *err, size_t errsz)
{
    struct sockaddr_in a;
    if (!addr_parse(ip, port, &a)) {
        snprintf(err, errsz, "bad address %s", ip);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        snprintf(err, errsz, "socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    if (reuse) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        snprintf(err, errsz, "bind %s:%u: %s", ip, (unsigned)port, strerror(errno));
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int udp_set_df(int fd)
{
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
    int v = IP_PMTUDISC_DO;
    return setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &v, sizeof v);
#elif defined(IP_DONTFRAG)
    int v = 1;
    return setsockopt(fd, IPPROTO_IP, IP_DONTFRAG, &v, sizeof v);
#else
    (void)fd;
    return 0;
#endif
}

int udp_join_mcast(int fd, const char *group, const char *iface_ip)
{
    struct ip_mreq mr;
    if (inet_pton(AF_INET, group, &mr.imr_multiaddr) != 1 || inet_pton(AF_INET, iface_ip, &mr.imr_interface) != 1)
        return -1;
    return setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
}

int udp_mcast_sender(int fd, const char *iface_ip, int ttl)
{
    struct in_addr ia;
    if (inet_pton(AF_INET, iface_ip, &ia) != 1) return -1;
    unsigned char t = (unsigned char)ttl, loop = 1;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia) != 0) return -1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof t);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    return 0;
}

int udp_set_broadcast(int fd)
{
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
}

bool addr_parse(const char *ip, uint16_t port, struct sockaddr_in *out)
{
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    return inet_pton(AF_INET, ip, &out->sin_addr) == 1;
}

bool hostport_parse(const char *s, struct sockaddr_in *out)
{
    const char *c = strrchr(s, ':');
    if (!c || c == s || (size_t)(c - s) >= 16) return false;
    char ip[16];
    memcpy(ip, s, (size_t)(c - s));
    ip[c - s] = '\0';
    char *end;
    long p = strtol(c + 1, &end, 10);
    if (*end || end == c + 1 || p < 1 || p > 65535) return false;
    return addr_parse(ip, (uint16_t)p, out);
}

void addr_fmt(const struct sockaddr_in *a, char out[ADDR_STR])
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    snprintf(out, ADDR_STR, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
}

bool addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

bool local_addr_for(const struct sockaddr_in *dst, struct in_addr *out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    bool ok = false;
    if (connect(fd, (const struct sockaddr *)dst, sizeof *dst) == 0) {
        struct sockaddr_in me;
        socklen_t l = sizeof me;
        if (getsockname(fd, (struct sockaddr *)&me, &l) == 0) {
            *out = me.sin_addr;
            ok = true;
        }
    }
    close(fd);
    return ok;
}
