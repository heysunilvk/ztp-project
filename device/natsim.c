/* Device-side NAT emulator (DESIGN §9.8), configured by --nat-sim / sim.nat.*, plus the
 * --rebind-after test hook. It acts on the device's own socket, the way a NAT in front of it
 * would:
 *   idle_timeout_s     no outbound datagram for this long: the mapping is gone, inbound datagrams
 *                      are dropped, and the next send gets a new source port (socket re-bound)
 *   reboot_at_s        at T: the mapping is flushed at once (same effect as above)
 *   change_ip_at_s     at T: flushed, and re-bound to another source IP (127.0.0.2; on macOS this
 *                      needs an lo0 alias, otherwise only the port changes)
 *   block_server_port  every datagram to the server's port is dropped (a firewall rule)
 *   --rebind-after S   (hook) at T: re-open the socket with a new port, keeping the mapping */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dev.h"
#include "log.h"
#include "timer.h"

bool dev_nat_active(const device_t *d)
{
    const ztp_config_t *c = &d->cfg.c;
    return c->sim.nat.idle_timeout_s > 0 || c->sim.nat.reboot_at_s > 0 || c->sim.nat.change_ip_at_s > 0 ||
           c->sim.nat.block_server_port || d->hook_rebind_after_s > 0;
}

void dev_nat_init(device_t *d)
{
    d->nat.start_ms = d->nat.last_out_ms = now_ms();
    d->nat.mapped = false;   /* no mapping until the first outbound datagram (§3.7 T-12) */
    snprintf(d->nat.bind_ip, sizeof d->nat.bind_ip, "0.0.0.0");
}

int dev_rebind(device_t *d, const char *ip, const char *why)
{
    char err[128];
    int fd = udp_open(ip, 0, false, err, sizeof err);
    if (fd < 0) {
        LOGW(T_SIM, "NAT: cannot bind %s (%s); keeping the current address", ip, err);
        return -1;
    }
    const ztp_config_t *c = &d->cfg.c;
    if (c->net.set_df) udp_set_df(fd);
    if (!strcmp(c->discovery.mode, "bcast")) udp_set_broadcast(fd);
    if (!strcmp(c->discovery.mode, "mcast") && strcmp(c->discovery.mcast_iface_addr, "0.0.0.0") != 0)
        udp_mcast_sender(fd, c->discovery.mcast_iface_addr, 1);
    close(d->fd);
    d->fd = fd;
    struct sockaddr_in me;
    socklen_t l = sizeof me;
    getsockname(fd, (struct sockaddr *)&me, &l);
    char a[ADDR_STR];
    addr_fmt(&me, a);
    LOGI(T_SIM, "NAT: %s: new source address %s", why, a);
    return 0;
}

static bool expired(const device_t *d)
{
    double idle = d->cfg.c.sim.nat.idle_timeout_s;
    return idle > 0 && (double)(d->now - d->nat.last_out_ms) > idle * 1000.0;
}

int dev_nat_before_send(device_t *d, const struct sockaddr_in *dst)
{
    if (!dev_nat_active(d)) return 0;
    const ztp_config_t *c = &d->cfg.c;
    if (c->sim.nat.block_server_port && ntohs(dst->sin_port) == ntohs(d->server.sin_port) && d->server.sin_port) {
        d->nat.blocked++;
        return 1;
    }
    if (d->nat.mapped && (expired(d) || d->nat.flushed)) {
        /* the old mapping is gone: a new outbound datagram creates a new one, on a new port */
        dev_rebind(d, d->nat.bind_ip, d->nat.flushed ? "mapping flushed" : "mapping expired after idle");
        d->nat.flushed = false;
    }
    d->nat.mapped = true;
    d->nat.last_out_ms = d->now;
    return 0;
}

bool dev_nat_accept_rx(device_t *d)
{
    if (!dev_nat_active(d)) return true;
    if (!d->nat.mapped || d->nat.flushed || expired(d)) {
        d->nat.rx_dropped++;
        return false;
    }
    return true;
}

uint64_t dev_nat_deadline(const device_t *d)
{
    const ztp_config_t *c = &d->cfg.c;
    uint64_t t = UINT64_MAX;
    if (c->sim.nat.reboot_at_s > 0 && !d->nat.reboot_done) {
        uint64_t x = d->nat.start_ms + (uint64_t)c->sim.nat.reboot_at_s * 1000u;
        if (x < t) t = x;
    }
    if (c->sim.nat.change_ip_at_s > 0 && !d->nat.ip_done) {
        uint64_t x = d->nat.start_ms + (uint64_t)c->sim.nat.change_ip_at_s * 1000u;
        if (x < t) t = x;
    }
    if (d->hook_rebind_after_s > 0 && !d->nat.rebind_done) {
        uint64_t x = d->nat.start_ms + (uint64_t)d->hook_rebind_after_s * 1000u;
        if (x < t) t = x;
    }
    return t;
}

void dev_nat_timer(device_t *d)
{
    const ztp_config_t *c = &d->cfg.c;
    uint64_t el = d->now - d->nat.start_ms;
    if (c->sim.nat.reboot_at_s > 0 && !d->nat.reboot_done && el >= (uint64_t)c->sim.nat.reboot_at_s * 1000u) {
        d->nat.reboot_done = true;
        d->nat.flushed = true;
        LOGI(T_SIM, "NAT: reboot, all mappings flushed");
    }
    if (c->sim.nat.change_ip_at_s > 0 && !d->nat.ip_done && el >= (uint64_t)c->sim.nat.change_ip_at_s * 1000u) {
        d->nat.ip_done = true;
        d->nat.flushed = true;
        snprintf(d->nat.bind_ip, sizeof d->nat.bind_ip, "127.0.0.2");
        LOGI(T_SIM, "NAT: public IP change, next datagram from %s", d->nat.bind_ip);
    }
    if (d->hook_rebind_after_s > 0 && !d->nat.rebind_done && el >= (uint64_t)d->hook_rebind_after_s * 1000u) {
        d->nat.rebind_done = true;
        dev_rebind(d, d->nat.bind_ip, "--rebind-after");
    }
}
