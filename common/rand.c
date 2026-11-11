#include "rand.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define HAVE_ARC4RANDOM 1
#endif

int rand_bytes(void *buf, size_t n)
{
#ifdef HAVE_ARC4RANDOM
    arc4random_buf(buf, n);
    return 0;
#else
    uint8_t *p = buf;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) { close(fd); return -1; }
        p += r;
        n -= (size_t)r;
    }
    close(fd);
    return 0;
#endif
}

uint64_t rand_u64(void)
{
    uint64_t v;
    if (rand_bytes(&v, sizeof v) != 0) {
        fprintf(stderr, "fatal: CSPRNG unavailable\n");
        abort();
    }
    return v;
}

uint32_t rand_u32_nonzero(void)
{
    uint32_t v;
    do v = (uint32_t)rand_u64(); while (v == 0);
    return v;
}
