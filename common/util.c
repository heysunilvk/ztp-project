#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int util_atomic_fault = AF_NONE;

static int write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* fsync that also flushes the drive's cache on macOS, where plain fsync() doesn't (DESIGN §15.3). */
int util_full_fsync(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;   /* some filesystems don't support it: fall back */
#endif
    return fsync(fd);
}

void util_fsync_dir(const char *path)
{
    char dir[PATH_MAX];
    if (!str_copy(dir, sizeof dir, path)) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = '\0';
    else if (slash) *slash = '\0';
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) {
        (void)util_full_fsync(fd);
        close(fd);
    }
}

int atomic_write_file(const char *path, const void *data, size_t len)
{
    char tmp[PATH_MAX], bak[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp ||
        snprintf(bak, sizeof bak, "%s.bak", path) >= (int)sizeof bak) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (write_all(fd, data, len) != 0) goto fail;
    if (util_atomic_fault == AF_AFTER_WRITE) { errno = EIO; goto fail; }
    if (util_full_fsync(fd) != 0) goto fail;
    if (util_atomic_fault == AF_AFTER_FSYNC) { errno = EIO; goto fail; }
    if (close(fd) != 0) { fd = -1; goto fail; }
    fd = -1;
    if (util_atomic_fault == AF_BEFORE_RENAME) { errno = EIO; goto fail; }

    /* Keep the previous version as .bak (hard link so path never disappears). */
    struct stat st;
    if (stat(path, &st) == 0) {
        (void)unlink(bak);
        int linked = link(path, bak);   /* best effort: without hard links there is no .bak */
        (void)linked;
    }
    if (rename(tmp, path) != 0) goto fail;
    util_fsync_dir(path);
    return 0;

fail:;
    int e = errno;
    if (fd >= 0) close(fd);
    (void)unlink(tmp);
    errno = e;
    return -1;
}

static int read_one(const char *path, size_t max, char **out, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); errno = e; return -1; }
    if (!S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    if ((uintmax_t)st.st_size > max) { close(fd); errno = E2BIG; return -1; }
    size_t n = (size_t)st.st_size;
    char *buf = malloc(n + 1);
    if (!buf) { close(fd); errno = ENOMEM; return -1; }
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno; free(buf); close(fd); errno = e; return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    *out = buf;
    if (out_len) *out_len = got;
    return 0;
}

int read_file(const char *path, size_t max, bool use_bak, char **out, size_t *out_len)
{
    if (read_one(path, max, out, out_len) == 0) return 0;
    if (!use_bak || errno == E2BIG) return -1;
    int e = errno;
    char bak[PATH_MAX];
    if (snprintf(bak, sizeof bak, "%s.bak", path) >= (int)sizeof bak) { errno = e; return -1; }
    if (read_one(bak, max, out, out_len) == 0) return 0;
    errno = e;
    return -1;
}

void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hex_decode(const char *in, uint8_t *out, size_t n)
{
    if (strlen(in) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

int version_parse(const char *s, uint32_t parts[3])
{
    if (!s || s[0] != 'v') return -1;
    const char *p = s + 1;
    int n = 0;
    parts[0] = parts[1] = parts[2] = 0;
    for (;;) {
        if (n == 3) return -1;
        int digits = 0;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 5) return -1;
            v = v * 10u + (uint32_t)(*p - '0');
            p++;
        }
        if (digits == 0) return -1;
        parts[n++] = v;
        if (*p == '\0') break;
        if (*p != '.') return -1;
        p++;
    }
    return n >= 2 ? 0 : -1;
}

int version_cmp(const char *a, const char *b, int *cmp)
{
    uint32_t x[3], y[3];
    if (version_parse(a, x) != 0 || version_parse(b, y) != 0) return -1;
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) {
            *cmp = x[i] < y[i] ? -1 : 1;
            return 0;
        }
    *cmp = 0;
    return 0;
}

bool str_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst_sz) return false;
    size_t n = strlen(src);
    bool ok = n < dst_sz;
    if (!ok) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return ok;
}
