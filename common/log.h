/* Tagged, millisecond-timestamped logs to stdout and a file (DESIGN §17).
 * Line format: 2026-11-10T14:32:10.123Z [TAG] message
 * Server lines about a session:   ... [TAG] [sid=<id> <serial>] message */
#ifndef ZTP_LOG_H
#define ZTP_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum log_level { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_OFF = 3 };

/* Tags (PDF tags first, then the project's own). */
#define T_BOOT       "BOOT"
#define T_DISCOVERY  "DISCOVERY"
#define T_UDP        "UDP"
#define T_NAT        "NAT"
#define T_AUTH       "AUTH"
#define T_PROVISION  "PROVISION"
#define T_UPGRADE    "UPGRADE"
#define T_VALIDATION "VALIDATION"
#define T_STATE      "STATE"
#define T_SIM        "SIM"
#define T_DB         "DB"
#define T_CONFIG     "CONFIG"
#define T_ERROR      "ERROR"
#define T_TEST       "TEST"       /* a test hook is active or fired: loud, but not an error */

/* Open the log. path may be NULL (stdout only). append=true keeps earlier content (the device
 * appends across restarts, §17.3). echo_stdout=false writes only the file. Returns 0 or -1. */
int  log_open(const char *path, bool append, bool echo_stdout);
void log_close(void);
void log_set_level(int level);
int  log_get_level(void);
int  log_level_parse(const char *s);   /* "debug"/"info"/"warn" -> level, -1 if unknown */

/* Write text as-is (no timestamp or tag) to the log file only (e.g. the DB dump, §14.5). */
void log_raw(const char *text);

/* For tests: redirect output to a memory stream (NULL restores stdout). */
void log_set_stream(FILE *f);

void log_msg(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void log_vmsg(int level, const char *tag, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));
/* Server per-session line: inserts "[sid=<sid> <serial>] " after the tag. */
void log_sess(int level, const char *tag, uint32_t sid, const char *serial, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Rate-limited variant: emits at most `burst` lines per key every `per_ms`; suppressed lines are
 * counted and reported on the next emitted line for that key (§5.6). key is any small integer. */
void log_limited(unsigned key, int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOGD(tag, ...) log_msg(LOG_DEBUG, tag, __VA_ARGS__)
#define LOGI(tag, ...) log_msg(LOG_INFO, tag, __VA_ARGS__)
#define LOGW(tag, ...) log_msg(LOG_WARN, tag, __VA_ARGS__)

#endif
