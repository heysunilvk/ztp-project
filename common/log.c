#include "log.h"

#include <fcntl.h>
#include <string.h>
#include <time.h>

#include "timer.h"

static FILE *g_file;
static FILE *g_stream;   /* NULL = stdout */
static bool  g_echo = true;
static int   g_level = LOG_INFO;

int log_open(const char *path, bool append, bool echo_stdout)
{
    log_close();
    g_echo = echo_stdout;
    if (!path || !*path) return 0;
    g_file = fopen(path, append ? "a" : "w");
    if (!g_file) return -1;
    fcntl(fileno(g_file), F_SETFD, FD_CLOEXEC);
    setvbuf(g_file, NULL, _IOLBF, 0);
    return 0;
}

void log_close(void)
{
    if (g_file) fclose(g_file);
    g_file = NULL;
}

void log_raw(const char *text)
{
    if (g_file) fputs(text, g_file);
}

void log_set_level(int level) { g_level = level; }
int  log_get_level(void) { return g_level; }
void log_set_stream(FILE *f) { g_stream = f; }

int log_level_parse(const char *s)
{
    if (!strcmp(s, "debug")) return LOG_DEBUG;
    if (!strcmp(s, "info")) return LOG_INFO;
    if (!strcmp(s, "warn")) return LOG_WARN;
    return -1;
}

static void stamp(char *out, size_t n)
{
    uint64_t ms = wall_ms();
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&s, &tm);
    size_t k = strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + k, n - k, ".%03uZ", (unsigned)(ms % 1000));
}

static void emit(const char *line)
{
    FILE *out = g_stream ? g_stream : stdout;
    if (g_echo || g_stream) {
        fputs(line, out);
        fflush(out);
    }
    if (g_file) fputs(line, g_file);
}

static void vformat(int level, const char *tag, const char *sess, const char *fmt, va_list ap)
{
    if (level < g_level) return;
    char ts[32], body[1024], line[1200];
    stamp(ts, sizeof ts);
    vsnprintf(body, sizeof body, fmt, ap);
    snprintf(line, sizeof line, "%s [%s] %s%s\n", ts, tag, sess ? sess : "", body);
    emit(line);
}

void log_vmsg(int level, const char *tag, const char *fmt, va_list ap)
{
    vformat(level, tag, NULL, fmt, ap);
}

void log_msg(int level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(level, tag, NULL, fmt, ap);
    va_end(ap);
}

void log_sess(int level, const char *tag, uint32_t sid, const char *serial, const char *fmt, ...)
{
    char sess[64];
    snprintf(sess, sizeof sess, "[sid=%u %s] ", (unsigned)sid, serial && *serial ? serial : "-");
    va_list ap;
    va_start(ap, fmt);
    vformat(level, tag, sess, fmt, ap);
    va_end(ap);
}

#define LIMIT_KEYS   64
#define LIMIT_BURST  5
#define LIMIT_PER_MS 1000u

static struct { uint64_t window_start; unsigned emitted, suppressed; } g_lim[LIMIT_KEYS];

void log_limited(unsigned key, int level, const char *tag, const char *fmt, ...)
{
    if (level < g_level) return;
    key %= LIMIT_KEYS;
    uint64_t now = now_ms();
    if (now - g_lim[key].window_start >= LIMIT_PER_MS) {
        g_lim[key].window_start = now;
        g_lim[key].emitted = 0;
    }
    if (g_lim[key].emitted >= LIMIT_BURST) {
        g_lim[key].suppressed++;
        return;
    }
    g_lim[key].emitted++;
    char body[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (g_lim[key].suppressed) {
        log_msg(level, tag, "%s (%u similar suppressed)", body, g_lim[key].suppressed);
        g_lim[key].suppressed = 0;
    } else {
        log_msg(level, tag, "%s", body);
    }
}
