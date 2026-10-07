#define _POSIX_C_SOURCE 200809L

#include "service.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

enum {
    SVC_LOG_PREFIX_BYTES = 96,
    SVC_LOG_SERVICE_BYTES = 63,
    SVC_LOG_FIXED_BYTES = 20 + 2 + SVC_LOG_SERVICE_BYTES + 2,
    SVC_LOG_MESSAGE_BYTES = PIPE_BUF - SVC_LOG_FIXED_BYTES < 1024 ?
        PIPE_BUF - SVC_LOG_FIXED_BYTES : 1024,
    SVC_LOG_LINE_BYTES = 20 + 2 + SVC_LOG_SERVICE_BYTES + 2 +
        SVC_LOG_MESSAGE_BYTES - 1 + 1
};

_Static_assert(
    SVC_LOG_LINE_BYTES <= PIPE_BUF,
    "one service log line must fit in the host pipe atomic write");

typedef struct {
    time_t second;
    char timestamp[20];
} svc_log_time_cache;

/* Each caller keeps its own second cache. Logging needs no shared lock. */
static _Thread_local svc_log_time_cache svc_log_cached_time;

static void svc_decimal_two(char *out, unsigned value) {
    out[0] = (char)('0' + (value / 10u) % 10u);
    out[1] = (char)('0' + value % 10u);
}

static void svc_decimal_four(char *out, unsigned value) {
    out[0] = (char)('0' + (value / 1000u) % 10u);
    out[1] = (char)('0' + (value / 100u) % 10u);
    out[2] = (char)('0' + (value / 10u) % 10u);
    out[3] = (char)('0' + value % 10u);
}

static void svc_log_timestamp(char *out, time_t now) {
    struct tm tm = {0};
    size_t used = 0;
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;

    if (svc_log_cached_time.timestamp[0] != '\0' &&
        svc_log_cached_time.second == now) {
        memcpy(
            out,
            svc_log_cached_time.timestamp,
            sizeof(svc_log_cached_time.timestamp));
        return;
    }
    if (now != (time_t)-1 && gmtime_r(&now, &tm) != NULL &&
        tm.tm_year >= -1900 && tm.tm_year <= 8099 &&
        tm.tm_mon >= 0 && tm.tm_mon <= 11 &&
        tm.tm_mday >= 1 && tm.tm_mday <= 31 &&
        tm.tm_hour >= 0 && tm.tm_hour <= 23 &&
        tm.tm_min >= 0 && tm.tm_min <= 59 &&
        tm.tm_sec >= 0 && tm.tm_sec <= 60) {
        year = (unsigned)(tm.tm_year + 1900);
        month = (unsigned)(tm.tm_mon + 1);
        day = (unsigned)tm.tm_mday;
        hour = (unsigned)tm.tm_hour;
        minute = (unsigned)tm.tm_min;
        second = (unsigned)tm.tm_sec;
    }
    svc_decimal_four(svc_log_cached_time.timestamp + used, year);
    used += 4u;
    svc_log_cached_time.timestamp[used++] = '-';
    svc_decimal_two(svc_log_cached_time.timestamp + used, month);
    used += 2u;
    svc_log_cached_time.timestamp[used++] = '-';
    svc_decimal_two(svc_log_cached_time.timestamp + used, day);
    used += 2u;
    svc_log_cached_time.timestamp[used++] = 'T';
    svc_decimal_two(svc_log_cached_time.timestamp + used, hour);
    used += 2u;
    svc_log_cached_time.timestamp[used++] = ':';
    svc_decimal_two(svc_log_cached_time.timestamp + used, minute);
    used += 2u;
    svc_log_cached_time.timestamp[used++] = ':';
    svc_decimal_two(svc_log_cached_time.timestamp + used, second);
    used += 2u;
    svc_log_cached_time.timestamp[used++] = 'Z';
    svc_log_cached_time.second = now;
    memcpy(
        out,
        svc_log_cached_time.timestamp,
        sizeof(svc_log_cached_time.timestamp));
}

static size_t svc_log_prefix(char *out, const char *svc, time_t now) {
    size_t svc_len;
    size_t used = 20u;
    svc_log_timestamp(out, now);
    out[used++] = ' ';
    out[used++] = '[';
    svc_len = strnlen(svc, SVC_LOG_SERVICE_BYTES);
    memcpy(out + used, svc, svc_len);
    used += svc_len;
    out[used++] = ']';
    out[used++] = ' ';
    return used;
}

const char *svc_env(const char *key, const char *fallback) {
    const char *v = getenv(key);
    if (v && v[0] != '\0') return v;
    return fallback;
}

int svc_env_int_range(const char *key, int fallback, int min_value, int max_value) {
    const char *v = getenv(key);
    char *end;
    long parsed;
    if (!key || min_value > max_value || fallback < min_value || fallback > max_value)
        return fallback;
    if (!v || !v[0]) return fallback;
    errno = 0;
    end = NULL;
    parsed = strtol(v, &end, 10);
    if (errno == ERANGE || end == v || *end != '\0' || parsed < min_value ||
        parsed > max_value) return fallback;
    return (int)parsed;
}

int svc_env_int(const char *key, int fallback) {
    return svc_env_int_range(key, fallback, INT_MIN, INT_MAX);
}

vbus_client *svc_connect_bus(void) {
    const char *path = vbus_default_path();
    int timeout_ms = svc_env_int_range("VBUS_CONNECT_TIMEOUT_MS", 5000, 0, 60000);
    int elapsed_ms = 0;
    vbus_client *c;
    for (;;) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
        c = vbus_connect(path);
        if (c) return c;
        if (elapsed_ms >= timeout_ms) break;
        nanosleep(&delay, NULL);
        elapsed_ms += 20;
    }
    fprintf(
        stderr,
        "vbus connect failed path=%s after_ms=%d (is vbus-broker running?)\n",
        path, elapsed_ms);
    return NULL;
}

static vbus_stop_flag *g_stop_ptr;

static void on_sig(int sig) {
    (void)sig;
    if (g_stop_ptr) atomic_store_explicit(g_stop_ptr, 1, memory_order_relaxed);
}

void svc_install_signals(vbus_stop_flag *stop) {
    g_stop_ptr = stop;
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);
}

void svc_log(const char *svc, const char *fmt, ...) {
    static const char newline = '\n';
    char message[SVC_LOG_MESSAGE_BYTES];
    char prefix[SVC_LOG_PREFIX_BYTES];
    struct iovec vectors[3];
    va_list ap;
    int formatted;
    size_t message_len;
    size_t prefix_len;
    ssize_t written;

    if (!fmt) return;
    if (!svc) svc = "unknown";
    va_start(ap, fmt);
    formatted = vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    if (formatted < 0) return;
    message_len = (size_t)formatted;
    if (message_len >= sizeof(message)) message_len = sizeof(message) - 1u;

    prefix_len = svc_log_prefix(prefix, svc, time(NULL));
    vectors[0].iov_base = prefix;
    vectors[0].iov_len = prefix_len;
    vectors[1].iov_base = message;
    vectors[1].iov_len = message_len;
    vectors[2].iov_base = (void *)&newline;
    vectors[2].iov_len = 1u;
    do {
        written = writev(STDERR_FILENO, vectors, 3);
    } while (written < 0 && errno == EINTR);
}

void svc_log_join_n(
    const char *svc,
    const char *literal,
    size_t literal_len,
    const char *value,
    size_t value_len
) {
    static const char newline = '\n';
    char prefix[SVC_LOG_PREFIX_BYTES];
    struct iovec vectors[4];
    size_t prefix_len;
    size_t remaining;
    ssize_t written;

    if (!literal || (!value && value_len != 0u)) return;
    if (!value) value = "";
    if (!svc) svc = "unknown";
    if (literal_len >= SVC_LOG_MESSAGE_BYTES)
        literal_len = SVC_LOG_MESSAGE_BYTES - 1u;
    remaining = SVC_LOG_MESSAGE_BYTES - 1u - literal_len;
    if (value_len > remaining) value_len = remaining;

    prefix_len = svc_log_prefix(prefix, svc, time(NULL));
    vectors[0].iov_base = prefix;
    vectors[0].iov_len = prefix_len;
    vectors[1].iov_base = (void *)literal;
    vectors[1].iov_len = literal_len;
    vectors[2].iov_base = (void *)value;
    vectors[2].iov_len = value_len;
    vectors[3].iov_base = (void *)&newline;
    vectors[3].iov_len = 1u;
    do {
        written = writev(STDERR_FILENO, vectors, 4);
    } while (written < 0 && errno == EINTR);
}
