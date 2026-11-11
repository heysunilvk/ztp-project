/* UDP socket helpers shared by both binaries (DESIGN §5.1, §6.2). IPv4 only. */
#ifndef ZTP_NET_H
#define ZTP_NET_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADDR_STR 24   /* "255.255.255.255:65535" */

/* Open a non-blocking UDP socket bound to ip:port (ip "0.0.0.0" = any, port 0 = ephemeral).
 * reuse sets SO_REUSEADDR (and SO_REUSEPORT where available). Returns fd or -1 (err filled). */
int  udp_open(const char *ip, uint16_t port, bool reuse, char *err, size_t errsz);
/* Set the don't-fragment behaviour (IP_MTU_DISCOVER=DO on Linux, IP_DONTFRAG on macOS). */
int  udp_set_df(int fd);
int  udp_join_mcast(int fd, const char *group, const char *iface_ip);
int  udp_mcast_sender(int fd, const char *iface_ip, int ttl);
int  udp_set_broadcast(int fd);

bool addr_parse(const char *ip, uint16_t port, struct sockaddr_in *out);
bool hostport_parse(const char *s, struct sockaddr_in *out);   /* "a.b.c.d:port" */
void addr_fmt(const struct sockaddr_in *a, char out[ADDR_STR]);
bool addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b);
/* The local address the kernel would use to reach dst (connect + getsockname on a scratch socket). */
bool local_addr_for(const struct sockaddr_in *dst, struct in_addr *out);

#endif
