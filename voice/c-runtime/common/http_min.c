/* http_min.c — bounded HTTP/1.1 over TCP for in-cluster plain HTTP. */

/* Darwin hides MSG_DONTWAIT when only the POSIX namespace is requested. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "http_min.h"
#include "http_response_header_name.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define HTTP_MIN_READ_BUFFER 8192u
#define HTTP_MIN_LINE_MAX 4096u
#define HTTP_MIN_HEADERS_MAX 32768u
#define HTTP_MIN_EXTRA_HEADERS_MAX 16u

typedef struct {
    int fd;
    uint8_t data[HTTP_MIN_READ_BUFFER];
    size_t pos;
    size_t len;
    const atomic_int *cancel;
    struct timespec deadline;
} http_reader;

typedef struct {
    const atomic_int *cancel;
    struct timespec deadline;
} http_wait;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
} http_request_writer;

static const char http_post_prefix[] = "POST ";
static const char http_version_host[] = " HTTP/1.1\r\nHost: ";
static const char http_common_headers[] =
    "\r\nUser-Agent: voice-c-runtime/1\r\n"
    "Accept: */*\r\nContent-Type: ";

_Static_assert(
    sizeof(((http_min_client *)0)->post_prefix) >=
        sizeof(http_post_prefix) - 1u +
        sizeof(((http_min_url *)0)->path) - 1u +
        sizeof(http_version_host) - 1u +
        sizeof(((http_min_url *)0)->host) - 1u +
        sizeof(":65535") - 1u + sizeof(http_common_headers),
    "HTTP POST prefix cache is too small");

static int request_append(
    http_request_writer *writer,
    const char *data,
    size_t length
) {
    if (!writer || !writer->data || (!data && length != 0) ||
        writer->length >= writer->capacity ||
        length >= writer->capacity - writer->length) return -1;
    if (length != 0) memcpy(writer->data + writer->length, data, length);
    writer->length += length;
    writer->data[writer->length] = '\0';
    return 0;
}

static int request_append_string(http_request_writer *writer, const char *text) {
    return text ? request_append(writer, text, strlen(text)) : -1;
}

static int request_append_size(http_request_writer *writer, size_t value) {
    char digits[sizeof(size_t) * 3u];
    size_t start = sizeof(digits);
    do {
        digits[--start] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    return request_append(writer, digits + start, sizeof(digits) - start);
}

static int wait_stopped(const http_wait *wait) {
    struct timespec now;
    if (wait && wait->cancel &&
        atomic_load_explicit(wait->cancel, memory_order_relaxed) != 0) return 2;
    if (!wait || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 1;
    return now.tv_sec > wait->deadline.tv_sec ||
           (now.tv_sec == wait->deadline.tv_sec && now.tv_nsec >= wait->deadline.tv_nsec);
}

static int wait_remaining_ms(const http_wait *wait) {
    struct timespec now;
    int64_t ms;
    if (!wait || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    ms = (int64_t)(wait->deadline.tv_sec - now.tv_sec) * 1000 +
         (int64_t)(wait->deadline.tv_nsec - now.tv_nsec + 999999L) / 1000000;
    if (ms <= 0) return 0;
    if (ms > 25) return 25;
    return (int)ms;
}

static int has_request_control(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (!p) return 1;
    while (*p) {
        if (*p <= 0x20u || *p == 0x7fu) return 1;
        p++;
    }
    return 0;
}

static int extra_header_valid(const http_min_header *header) {
    const unsigned char *p;
    static const char *const reserved[] = {
        "host", "content-length", "content-type", "connection", "transfer-encoding"
    };
    size_t i;
    if (!header || !header->name || !header->name[0] || !header->value) return 0;
    p = (const unsigned char *)header->name;
    while (*p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-')) return 0;
        p++;
    }
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        if (strcasecmp(header->name, reserved[i]) == 0) return 0;
    }
    p = (const unsigned char *)header->value;
    while (*p) {
        if (*p < 0x20u || *p == 0x7fu) return 0;
        p++;
    }
    return 1;
}

int http_min_parse_url(const char *url, http_min_url *out) {
    const char *p;
    const char *host_start;
    size_t host_len;
    if (!url || !out) return HTTP_MIN_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->port = 80;
    if (strncmp(url, "http://", 7) != 0) return HTTP_MIN_ERR_PARSE;

    p = url + 7;
    host_start = p;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
    host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return HTTP_MIN_ERR_PARSE;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';
    if (has_request_control(out->host)) return HTTP_MIN_ERR_PARSE;

    if (*p == ':') {
        unsigned port = 0;
        int digits = 0;
        p++;
        while (*p >= '0' && *p <= '9') {
            unsigned digit = (unsigned)(*p - '0');
            if (port > (65535u - digit) / 10u) return HTTP_MIN_ERR_PARSE;
            port = port * 10u + digit;
            digits++;
            p++;
        }
        if (digits == 0 || port == 0 || (*p && *p != '/' && *p != '?'))
            return HTTP_MIN_ERR_PARSE;
        out->port = (int)port;
    }
    if (*p == '#') return HTTP_MIN_ERR_PARSE;
    if (strchr(p, '#') != NULL) return HTTP_MIN_ERR_PARSE;
    if (*p == '?') {
        size_t query_len = strlen(p);
        if (query_len + 1 >= sizeof(out->path)) return HTTP_MIN_ERR_PARSE;
        out->path[0] = '/';
        memcpy(out->path + 1, p, query_len + 1);
    } else if (*p == '\0') {
        memcpy(out->path, "/", 2);
    } else {
        size_t path_len = strlen(p);
        if (*p != '/' || path_len >= sizeof(out->path)) return HTTP_MIN_ERR_PARSE;
        memcpy(out->path, p, path_len + 1);
    }
    if (has_request_control(out->path)) return HTTP_MIN_ERR_PARSE;
    return HTTP_MIN_OK;
}

static int prepare_client_post_prefix(http_min_client *client) {
    int written;
    if (!client) return -1;
    if (client->url.port == 80) {
        written = snprintf(
            client->post_prefix,
            sizeof(client->post_prefix),
            "%s%s%s%s%s",
            http_post_prefix,
            client->url.path,
            http_version_host,
            client->url.host,
            http_common_headers);
    } else {
        written = snprintf(
            client->post_prefix,
            sizeof(client->post_prefix),
            "%s%s%s%s:%d%s",
            http_post_prefix,
            client->url.path,
            http_version_host,
            client->url.host,
            client->url.port,
            http_common_headers);
    }
    if (written <= 0 || (size_t)written >= sizeof(client->post_prefix))
        return -1;
    client->post_prefix_len = (size_t)written;
    return 0;
}

int http_min_client_init(http_min_client *client, const char *url) {
    int rc;
    if (!client) return HTTP_MIN_ERR_ARGUMENT;
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    rc = http_min_parse_url(url, &client->url);
    if (rc != HTTP_MIN_OK) return rc;
    if (prepare_client_post_prefix(client) != 0) {
        memset(client, 0, sizeof(*client));
        client->fd = -1;
        return HTTP_MIN_ERR_CAPACITY;
    }
    client->initialized = 1;
    return HTTP_MIN_OK;
}

void http_min_client_destroy(http_min_client *client) {
    if (!client) return;
    if (client->initialized && client->fd >= 0) close(client->fd);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static int set_timeouts(int fd, int timeout_ms) {
    struct timeval tv;
    if (timeout_ms <= 0 || timeout_ms > 25) timeout_ms = 25;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
    return 0;
}

static int connect_with_timeout(
    int fd,
    const struct sockaddr *addr,
    socklen_t len,
    const http_wait *wait
) {
    int flags;
    int rc;
    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    struct pollfd pfd;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    rc = connect(fd, addr, len);
    if (rc != 0 && errno != EINPROGRESS) return -1;
    if (rc != 0) {
        pfd.fd = fd;
        pfd.events = POLLOUT;
        for (;;) {
            int wait_ms;
            int stopped = wait_stopped(wait);
            if (stopped) return stopped == 2 ? -2 : -1;
            wait_ms = wait_remaining_ms(wait);
            if (wait_ms <= 0) return -1;
            rc = poll(&pfd, 1, wait_ms);
            if (rc < 0 && errno == EINTR) continue;
            if (rc == 0) continue;
            break;
        }
        if (rc <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 ||
            socket_error != 0) return -1;
    }
    if (fcntl(fd, F_SETFL, flags) != 0) return -1;
    return 0;
}

static int connect_host(const http_min_url *u, const http_wait *wait) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    char portbuf[16];
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    snprintf(portbuf, sizeof(portbuf), "%d", u->port);
    if (getaddrinfo(u->host, portbuf, &hints, &res) != 0) return -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (connect_with_timeout(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen, wait) == 0 &&
            set_timeouts(fd, 25) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int connected_socket_is_idle(int fd) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    uint8_t byte;
    ssize_t count;
    int rc;
    do {
        rc = poll(&pfd, 1, 0);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) return 1;
    if (rc < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return 0;
    if ((pfd.revents & POLLIN) == 0) return 1;
    count = recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
    return 0;
}

int http_min_client_warm(http_min_client *client, int timeout_ms) {
    http_wait wait;
    int fd;
    if (!client || !client->initialized) return HTTP_MIN_ERR_ARGUMENT;
    if (client->fd >= 0) {
        if (connected_socket_is_idle(client->fd)) return HTTP_MIN_OK;
        close(client->fd);
        client->fd = -1;
    }
    if (timeout_ms <= 0) timeout_ms = 5000;
    if (clock_gettime(CLOCK_MONOTONIC, &wait.deadline) != 0)
        return HTTP_MIN_ERR_CONNECT;
    wait.cancel = NULL;
    wait.deadline.tv_sec += timeout_ms / 1000;
    wait.deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (wait.deadline.tv_nsec >= 1000000000L) {
        wait.deadline.tv_sec++;
        wait.deadline.tv_nsec -= 1000000000L;
    }
    fd = connect_host(&client->url, &wait);
    if (fd < 0) return HTTP_MIN_ERR_CONNECT;
    client->fd = fd;
    return HTTP_MIN_OK;
}

static int send_request(
    int fd,
    const void *header,
    size_t header_len,
    const void *body,
    size_t body_len,
    const http_wait *wait
) {
    struct iovec parts[2];
    size_t part_count = body_len == 0 ? 1u : 2u;
    size_t part = 0;
    parts[0].iov_base = (void *)header;
    parts[0].iov_len = header_len;
    parts[1].iov_base = (void *)body;
    parts[1].iov_len = body_len;
    while (part < part_count) {
        struct msghdr message;
        size_t consumed;
        ssize_t count;
        int stopped = wait_stopped(wait);
        if (stopped) return stopped == 2 ? -2 : -1;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &parts[part];
#if defined(__APPLE__)
        /* This request has at most two vectors. */
        message.msg_iovlen = (int)(part_count - part);
#else
        message.msg_iovlen = part_count - part;
#endif
        count = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        if (count == 0) return -1;
        consumed = (size_t)count;
        while (part < part_count && consumed >= parts[part].iov_len) {
            consumed -= parts[part].iov_len;
            part++;
        }
        if (part < part_count && consumed != 0) {
            parts[part].iov_base = (uint8_t *)parts[part].iov_base + consumed;
            parts[part].iov_len -= consumed;
        }
    }
    return 0;
}

static int reader_refill(http_reader *reader) {
    ssize_t n;
    reader->pos = 0;
    reader->len = 0;
    for (;;) {
        http_wait wait = {.cancel = reader->cancel, .deadline = reader->deadline};
        int stopped = wait_stopped(&wait);
        if (stopped) return stopped == 2 ? -2 : -1;
        n = recv(reader->fd, reader->data, sizeof(reader->data), 0);
        /* Cancellation can arrive while recv blocks. Do not expose the bytes
         * that wake it, even when the peer delivers a complete response. */
        if (reader->cancel &&
            atomic_load_explicit(reader->cancel, memory_order_relaxed) != 0) return -2;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        break;
    }
    if (n < 0) return -1;
    if (n == 0) return 0;
    reader->len = (size_t)n;
    return 1;
}

static inline int reader_fill(http_reader *reader) {
    /* A prior body callback can cancel with more chunks in this same buffer. */
    if (reader->cancel &&
        atomic_load_explicit(reader->cancel, memory_order_relaxed) != 0) return -2;
    return reader->pos < reader->len ? 1 : reader_refill(reader);
}

static int reader_line(
    http_reader *reader,
    char *scratch,
    size_t scratch_cap,
    const char **out,
    size_t *out_len,
    size_t *header_bytes
) {
    size_t used = 0;
    if (!reader || !scratch || scratch_cap == 0 || !out || !out_len ||
        !header_bytes) return HTTP_MIN_ERR_ARGUMENT;
    for (;;) {
        const uint8_t *start;
        const uint8_t *newline;
        size_t available;
        size_t span_len;
        int fill = reader_fill(reader);
        if (fill <= 0) return fill == -2 ? HTTP_MIN_ERR_CANCELED : HTTP_MIN_ERR_IO;
        available = reader->len - reader->pos;
        start = reader->data + reader->pos;
        newline = (const uint8_t *)memchr(start, '\n', available);
        span_len = newline ? (size_t)(newline - start) : available;
        if (*header_bytes > HTTP_MIN_HEADERS_MAX ||
            span_len > HTTP_MIN_HEADERS_MAX - *header_bytes ||
            used >= scratch_cap || span_len >= scratch_cap - used)
            return HTTP_MIN_ERR_CAPACITY;
        reader->pos += span_len;
        *header_bytes += span_len;
        if (newline && used == 0u) {
            if (*header_bytes == HTTP_MIN_HEADERS_MAX)
                return HTTP_MIN_ERR_CAPACITY;
            reader->pos++;
            (*header_bytes)++;
            if (span_len == 0u || start[span_len - 1u] != '\r')
                return HTTP_MIN_ERR_PARSE;
            *out = (const char *)start;
            *out_len = span_len - 1u;
            return HTTP_MIN_OK;
        }
        memcpy(scratch + used, start, span_len);
        used += span_len;
        if (newline) {
            if (*header_bytes == HTTP_MIN_HEADERS_MAX)
                return HTTP_MIN_ERR_CAPACITY;
            reader->pos++;
            (*header_bytes)++;
            if (used == 0 || scratch[used - 1] != '\r')
                return HTTP_MIN_ERR_PARSE;
            used--;
            scratch[used] = '\0';
            *out = scratch;
            *out_len = used;
            return HTTP_MIN_OK;
        }
    }
}

static int reader_exact(http_reader *reader, uint8_t *out, size_t len) {
    size_t copied = 0;
    while (copied < len) {
        size_t available;
        size_t take;
        int fill = reader_fill(reader);
        if (fill <= 0) return fill == -2 ? HTTP_MIN_ERR_CANCELED : HTTP_MIN_ERR_IO;
        available = reader->len - reader->pos;
        take = len - copied < available ? len - copied : available;
        memcpy(out + copied, reader->data + reader->pos, take);
        reader->pos += take;
        copied += take;
    }
    return HTTP_MIN_OK;
}

typedef struct {
    uint8_t *out;
    size_t out_cap;
    size_t total;
    size_t max_total;
    http_min_body_callback callback;
    void *callback_user;
} http_body_sink;

static int sink_write(http_body_sink *sink, const uint8_t *data, size_t len) {
    if (!sink || (len != 0 && !data) || len > sink->max_total - sink->total)
        return HTTP_MIN_ERR_CAPACITY;
    if (len != 0) {
        if (sink->callback) {
            if (sink->callback(data, len, sink->callback_user) != 0)
                return HTTP_MIN_ERR_CALLBACK;
        } else {
            if (!sink->out || len > sink->out_cap - sink->total)
                return HTTP_MIN_ERR_CAPACITY;
            memcpy(sink->out + sink->total, data, len);
        }
    }
    sink->total += len;
    return HTTP_MIN_OK;
}

static int reader_to_sink(http_reader *reader, http_body_sink *sink, size_t len) {
    size_t consumed = 0;
    while (consumed < len) {
        size_t available;
        size_t take;
        int fill = reader_fill(reader);
        int rc;
        if (fill <= 0) return fill == -2 ? HTTP_MIN_ERR_CANCELED : HTTP_MIN_ERR_IO;
        available = reader->len - reader->pos;
        take = len - consumed < available ? len - consumed : available;
        rc = sink_write(sink, reader->data + reader->pos, take);
        if (rc != HTTP_MIN_OK) return rc;
        reader->pos += take;
        consumed += take;
    }
    return HTTP_MIN_OK;
}

static int parse_decimal_size(const char *value, size_t value_len, size_t *out) {
    size_t parsed = 0;
    size_t i = 0;
    while (i < value_len && (value[i] == ' ' || value[i] == '\t')) i++;
    if (i == value_len) return -1;
    for (; i < value_len && value[i] >= '0' && value[i] <= '9'; ++i) {
        unsigned digit = (unsigned)(value[i] - '0');
        if (parsed > (SIZE_MAX - digit) / 10u) return -1;
        parsed = parsed * 10u + digit;
    }
    while (i < value_len && (value[i] == ' ' || value[i] == '\t')) i++;
    if (i != value_len) return -1;
    *out = parsed;
    return 0;
}

static int transfer_encoding_is_chunked(const char *value, size_t value_len) {
    size_t start = 0;
    size_t end = value_len;
    while (start < end && (value[start] == ' ' || value[start] == '\t')) start++;
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
    return end - start == sizeof("chunked") - 1 &&
           strncasecmp(value + start, "chunked", sizeof("chunked") - 1) == 0;
}

static int parse_chunk_size(const char *line, size_t line_len, size_t *out) {
    size_t value = 0;
    size_t digits = 0;
    size_t i = 0;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
    while (i < line_len) {
        unsigned digit;
        unsigned char c = (unsigned char)line[i];
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10u;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10u;
        else break;
        if (value > (SIZE_MAX - digit) / 16u) return -1;
        value = value * 16u + digit;
        digits++;
        i++;
    }
    while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (digits == 0 || (i < line_len && line[i] != ';')) return -1;
    *out = value;
    return 0;
}

static int parse_status_line(
    const char *line,
    size_t line_len,
    int *out_status,
    int *out_minor
) {
    int status;
    if (!line || !out_status || !out_minor || line_len < 12 ||
        memcmp(line, "HTTP/1.", sizeof("HTTP/1.") - 1) != 0 ||
        (line[7] != '0' && line[7] != '1') || line[8] != ' ' ||
        line[9] < '0' || line[9] > '9' ||
        line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' ||
        (line_len > 12 && line[12] != ' ')) return -1;
    status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (status < 100) return -1;
    *out_status = status;
    *out_minor = line[7] - '0';
    return 0;
}

enum {
    CONNECTION_TOKEN_CLOSE = 1u << 0,
    CONNECTION_TOKEN_KEEP_ALIVE = 1u << 1
};

static unsigned connection_header_tokens(const char *value, size_t value_len) {
    static const char canonical_keep_alive[] = " keep-alive";
    size_t start = 0;
    unsigned tokens = 0;
    /* Persistent model backends normally return this exact field value. */
    if (value_len == sizeof(canonical_keep_alive) - 1u &&
        memcmp(
            value,
            canonical_keep_alive,
            sizeof(canonical_keep_alive) - 1u) == 0)
        return CONNECTION_TOKEN_KEEP_ALIVE;
    while (start < value_len) {
        size_t end;
        while (start < value_len &&
               (value[start] == ' ' || value[start] == '\t' || value[start] == ','))
            start++;
        end = start;
        while (end < value_len && value[end] != ',') end++;
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
        if (end - start == sizeof("close") - 1u &&
            strncasecmp(value + start, "close", sizeof("close") - 1u) == 0)
            tokens |= CONNECTION_TOKEN_CLOSE;
        else if (end - start == sizeof("keep-alive") - 1u &&
                 strncasecmp(
                     value + start, "keep-alive", sizeof("keep-alive") - 1u) == 0)
            tokens |= CONNECTION_TOKEN_KEEP_ALIVE;
        start = end < value_len ? end + 1u : end;
    }
    return tokens;
}

static int read_response(
    int fd,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    const http_wait *wait,
    http_min_body_callback callback,
    void *callback_user,
    size_t max_body_len,
    int *out_reusable
) {
    http_reader reader;
    char line_scratch[HTTP_MIN_LINE_MAX];
    const char *line;
    size_t line_len = 0;
    size_t header_bytes = 0;
    size_t content_len = 0;
    http_body_sink sink = {
        .out = out_body,
        .out_cap = out_cap,
        .total = 0,
        .max_total = callback ? max_body_len : out_cap,
        .callback = callback,
        .callback_user = callback_user
    };
    int have_content_len = 0;
    int have_transfer_encoding = 0;
    int chunked = 0;
    int status = 0;
    int http_minor = 0;
    int connection_close = 0;
    int connection_keep_alive = 0;
    int rc;

    /* recv() fills reader.data before pos and len expose any byte. Avoid
     * clearing the unused 8 KiB buffer for every persistent request. */
    reader.fd = fd;
    reader.pos = 0;
    reader.len = 0;
    reader.cancel = wait ? wait->cancel : NULL;
    reader.deadline = wait ? wait->deadline : (struct timespec){0};

    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;
    if (out_reusable) *out_reusable = 0;
    if (!callback) out_body[0] = 0;

    rc = reader_line(
        &reader, line_scratch, sizeof(line_scratch), &line,
        &line_len, &header_bytes);
    if (rc != HTTP_MIN_OK) return rc;
    if (parse_status_line(line, line_len, &status, &http_minor) != 0)
        return HTTP_MIN_ERR_PARSE;
    if (out_status) *out_status = status;

    for (;;) {
        const char *colon;
        size_t name_len;
        const char *value;
        size_t value_len;
        rc = reader_line(
            &reader, line_scratch, sizeof(line_scratch), &line,
            &line_len, &header_bytes);
        if (rc != HTTP_MIN_OK) return rc;
        if (line_len == 0) break;
        colon = memchr(line, ':', line_len);
        if (!colon) return HTTP_MIN_ERR_PARSE;
        name_len = (size_t)(colon - line);
        while (name_len > 0 && (line[name_len - 1] == ' ' || line[name_len - 1] == '\t')) name_len--;
        value = colon + 1;
        value_len = line_len - (size_t)(value - line);
        switch (http_response_header_classify(line, name_len)) {
            case HTTP_RESPONSE_HEADER_CONTENT_LENGTH: {
                size_t parsed;
                if (parse_decimal_size(value, value_len, &parsed) != 0 ||
                    (have_content_len && parsed != content_len))
                    return HTTP_MIN_ERR_PARSE;
                content_len = parsed;
                have_content_len = 1;
                break;
            }
            case HTTP_RESPONSE_HEADER_TRANSFER_ENCODING:
                if (have_transfer_encoding) return HTTP_MIN_ERR_PARSE;
                have_transfer_encoding = 1;
                if (transfer_encoding_is_chunked(value, value_len)) chunked = 1;
                break;
            case HTTP_RESPONSE_HEADER_CONNECTION: {
                unsigned tokens = connection_header_tokens(value, value_len);
                if ((tokens & CONNECTION_TOKEN_CLOSE) != 0) connection_close = 1;
                if ((tokens & CONNECTION_TOKEN_KEEP_ALIVE) != 0)
                    connection_keep_alive = 1;
                break;
            }
            case HTTP_RESPONSE_HEADER_UNKNOWN:
                break;
        }
    }
    if ((have_transfer_encoding && !chunked) || (chunked && have_content_len))
        return HTTP_MIN_ERR_PARSE;
    if (callback && (status < 200 || status >= 300)) return HTTP_MIN_ERR_STATUS;

    if (chunked) {
        for (;;) {
            size_t chunk_size;
            uint8_t crlf[2];
            rc = reader_line(
                &reader, line_scratch, sizeof(line_scratch), &line,
                &line_len, &header_bytes);
            if (rc != HTTP_MIN_OK) return rc;
            if (parse_chunk_size(line, line_len, &chunk_size) != 0)
                return HTTP_MIN_ERR_PARSE;
            if (chunk_size == 0) {
                do {
                    rc = reader_line(
                        &reader, line_scratch, sizeof(line_scratch), &line,
                        &line_len, &header_bytes);
                    if (rc != HTTP_MIN_OK) return rc;
                } while (line_len != 0);
                break;
            }
            if (chunk_size > sink.max_total - sink.total) return HTTP_MIN_ERR_CAPACITY;
            rc = reader_to_sink(&reader, &sink, chunk_size);
            if (rc != HTTP_MIN_OK) return rc;
            rc = reader_exact(&reader, crlf, sizeof(crlf));
            if (rc != HTTP_MIN_OK) return rc;
            if (crlf[0] != '\r' || crlf[1] != '\n')
                return HTTP_MIN_ERR_PARSE;
        }
    } else if (have_content_len) {
        if (content_len > sink.max_total) return HTTP_MIN_ERR_CAPACITY;
        rc = reader_to_sink(&reader, &sink, content_len);
        if (rc != HTTP_MIN_OK) return rc;
    } else {
        connection_close = 1;
        for (;;) {
            size_t available;
            int sink_rc;
            int fill = reader_fill(&reader);
            if (fill < 0) return fill == -2 ? HTTP_MIN_ERR_CANCELED : HTTP_MIN_ERR_IO;
            if (fill == 0) break;
            available = reader.len - reader.pos;
            sink_rc = sink_write(&sink, reader.data + reader.pos, available);
            if (sink_rc != HTTP_MIN_OK) return sink_rc;
            reader.pos += available;
        }
    }

    if (out_len) *out_len = sink.total;
    if (!callback && sink.total < out_cap) out_body[sink.total] = 0;
    if (out_reusable)
        *out_reusable = !connection_close &&
            (http_minor == 1 || connection_keep_alive);
    if (status < 200 || status >= 300) return HTTP_MIN_ERR_STATUS;
    return HTTP_MIN_OK;
}

static int do_request(
    const char *method,
    const char *url,
    http_min_client *client,
    const char *content_type,
    const http_min_header *headers,
    size_t header_count,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms,
    const atomic_int *cancel,
    http_min_body_callback callback,
    void *callback_user,
    size_t max_body_len
) {
    http_min_url parsed;
    const http_min_url *target;
    char request[4096];
    http_request_writer writer;
    size_t request_len;
    int fd;
    int rc;
    int reusable = 0;
    int send_rc;
    http_wait wait;
    if (!method || (!url && !client) || (client && !client->initialized) ||
        (!callback && (!out_body || out_cap == 0)) ||
        (callback && max_body_len == 0) || (body_len != 0 && !body))
        return HTTP_MIN_ERR_ARGUMENT;
    if (header_count > HTTP_MIN_EXTRA_HEADERS_MAX || (header_count != 0 && !headers))
        return HTTP_MIN_ERR_ARGUMENT;
    if (content_type && (strchr(content_type, '\r') || strchr(content_type, '\n')))
        return HTTP_MIN_ERR_ARGUMENT;
    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;
    if (timeout_ms <= 0) timeout_ms = 5000;
    if (clock_gettime(CLOCK_MONOTONIC, &wait.deadline) != 0) return HTTP_MIN_ERR_IO;
    wait.cancel = cancel;
    wait.deadline.tv_sec += timeout_ms / 1000;
    wait.deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (wait.deadline.tv_nsec >= 1000000000L) {
        wait.deadline.tv_sec++;
        wait.deadline.tv_nsec -= 1000000000L;
    }
    if (wait_stopped(&wait) == 2) return HTTP_MIN_ERR_CANCELED;
    if (client) target = &client->url;
    else {
        rc = http_min_parse_url(url, &parsed);
        if (rc != HTTP_MIN_OK) return rc;
        target = &parsed;
    }
    writer.data = request;
    writer.capacity = sizeof(request);
    writer.length = 0;
    request[0] = '\0';
    if (strcmp(method, "POST") == 0) {
        static const char content_length[] = "\r\nContent-Length: ";
        static const char header_end[] = "\r\nConnection: ";
        const char *body_type = content_type ?
            content_type : "application/octet-stream";
        if (client) {
            if (request_append(
                    &writer,
                    client->post_prefix,
                    client->post_prefix_len) != 0)
                return HTTP_MIN_ERR_CAPACITY;
        } else if (
            request_append(
                &writer, http_post_prefix, sizeof(http_post_prefix) - 1u) != 0 ||
            request_append_string(&writer, target->path) != 0 ||
            request_append(
                &writer, http_version_host, sizeof(http_version_host) - 1u) != 0 ||
            request_append_string(&writer, target->host) != 0 ||
            (target->port != 80 &&
             (request_append(&writer, ":", 1u) != 0 ||
              request_append_size(&writer, (size_t)target->port) != 0)) ||
            request_append(
                &writer,
                http_common_headers,
                sizeof(http_common_headers) - 1u) != 0)
            return HTTP_MIN_ERR_CAPACITY;
        if (request_append_string(&writer, body_type) != 0 ||
            request_append(
                &writer, content_length, sizeof(content_length) - 1u) != 0 ||
            request_append_size(&writer, body_len) != 0)
            return HTTP_MIN_ERR_CAPACITY;
        {
            size_t i;
            for (i = 0; i < header_count; ++i) {
                if (!extra_header_valid(&headers[i])) return HTTP_MIN_ERR_ARGUMENT;
                if (request_append(&writer, "\r\n", 2u) != 0 ||
                    request_append_string(&writer, headers[i].name) != 0 ||
                    request_append(&writer, ": ", 2u) != 0 ||
                    request_append_string(&writer, headers[i].value) != 0)
                    return HTTP_MIN_ERR_CAPACITY;
            }
            if (request_append(&writer, header_end, sizeof(header_end) - 1u) != 0 ||
                request_append_string(
                    &writer, client ? "keep-alive" : "close") != 0 ||
                request_append(&writer, "\r\n\r\n", 4u) != 0)
                return HTTP_MIN_ERR_CAPACITY;
        }
    } else if (strcmp(method, "GET") == 0) {
        static const char get_prefix[] = "GET ";
        static const char version_host[] = " HTTP/1.1\r\nHost: ";
        static const char get_suffix[] =
            "User-Agent: voice-c-runtime/1\r\n"
            "Accept: */*";
        size_t i;
        if (request_append(&writer, get_prefix, sizeof(get_prefix) - 1u) != 0 ||
            request_append_string(&writer, target->path) != 0 ||
            request_append(&writer, version_host, sizeof(version_host) - 1u) != 0 ||
            request_append_string(&writer, target->host) != 0 ||
            (target->port != 80 &&
             (request_append(&writer, ":", 1u) != 0 ||
              request_append_size(&writer, (size_t)target->port) != 0)) ||
            request_append(&writer, "\r\n", 2u) != 0 ||
            request_append(&writer, get_suffix, sizeof(get_suffix) - 1u) != 0)
            return HTTP_MIN_ERR_CAPACITY;
        for (i = 0; i < header_count; ++i) {
            if (!extra_header_valid(&headers[i])) return HTTP_MIN_ERR_ARGUMENT;
            if (request_append(&writer, "\r\n", 2u) != 0 ||
                request_append_string(&writer, headers[i].name) != 0 ||
                request_append(&writer, ": ", 2u) != 0 ||
                request_append_string(&writer, headers[i].value) != 0)
                return HTTP_MIN_ERR_CAPACITY;
        }
        if (request_append_string(&writer, "\r\nConnection: close\r\n\r\n") != 0)
            return HTTP_MIN_ERR_CAPACITY;
    } else {
        return HTTP_MIN_ERR_ARGUMENT;
    }
    request_len = writer.length;
    if (request_len == 0) return HTTP_MIN_ERR_CAPACITY;

    if (client && client->fd >= 0 && !connected_socket_is_idle(client->fd)) {
        close(client->fd);
        client->fd = -1;
    }
    fd = client ? client->fd : -1;
    if (fd < 0) fd = connect_host(target, &wait);
    if (fd < 0)
        return wait_stopped(&wait) == 2 ? HTTP_MIN_ERR_CANCELED : HTTP_MIN_ERR_CONNECT;
    if (client) client->fd = fd;
    send_rc = send_request(
        fd, request, request_len, body, body_len, &wait);
    if (send_rc != 0) {
        close(fd);
        if (client) client->fd = -1;
        return send_rc == -2 ? HTTP_MIN_ERR_CANCELED : HTTP_MIN_ERR_IO;
    }
    rc = read_response(
        fd, out_body, out_cap, out_len, out_status, &wait,
        callback, callback_user, max_body_len, &reusable);
    if (!client || !reusable) {
        close(fd);
        if (client) client->fd = -1;
    }
    return rc;
}

int http_min_post(
    const char *url,
    const char *content_type,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms
) {
    return do_request(
        "POST", url, NULL, content_type, NULL, 0, body, body_len,
        out_body, out_cap, out_len,
        out_status, timeout_ms, NULL, NULL, NULL, 0);
}

int http_min_post_headers(
    const char *url,
    const char *content_type,
    const http_min_header *headers,
    size_t header_count,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms
) {
    return do_request(
        "POST", url, NULL, content_type, headers, header_count, body, body_len,
        out_body, out_cap, out_len, out_status, timeout_ms, NULL, NULL, NULL, 0);
}

int http_min_post_headers_cancel(
    const char *url,
    const char *content_type,
    const http_min_header *headers,
    size_t header_count,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms,
    const atomic_int *cancel
) {
    return do_request(
        "POST", url, NULL, content_type, headers, header_count, body, body_len,
        out_body, out_cap, out_len, out_status, timeout_ms, cancel, NULL, NULL, 0);
}

int http_min_post_stream_headers_cancel(
    const char *url,
    const char *content_type,
    const http_min_header *headers,
    size_t header_count,
    const uint8_t *body,
    size_t body_len,
    http_min_body_callback callback,
    void *callback_user,
    size_t max_body_len,
    size_t *out_len,
    int *out_status,
    int timeout_ms,
    const atomic_int *cancel
) {
    if (!callback) return HTTP_MIN_ERR_ARGUMENT;
    return do_request(
        "POST", url, NULL, content_type, headers, header_count, body, body_len,
        NULL, 0, out_len, out_status, timeout_ms, cancel,
        callback, callback_user, max_body_len);
}

int http_min_client_post(
    http_min_client *client,
    const char *content_type,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms
) {
    return do_request(
        "POST", NULL, client, content_type, NULL, 0, body, body_len,
        out_body, out_cap, out_len, out_status, timeout_ms, NULL, NULL, NULL, 0);
}

int http_min_client_post_headers_cancel(
    http_min_client *client,
    const char *content_type,
    const http_min_header *headers,
    size_t header_count,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms,
    const atomic_int *cancel
) {
    return do_request(
        "POST", NULL, client, content_type, headers, header_count, body, body_len,
        out_body, out_cap, out_len, out_status, timeout_ms, cancel, NULL, NULL, 0);
}

int http_min_client_post_stream_headers_cancel(
    http_min_client *client,
    const char *content_type,
    const http_min_header *headers,
    size_t header_count,
    const uint8_t *body,
    size_t body_len,
    http_min_body_callback callback,
    void *callback_user,
    size_t max_body_len,
    size_t *out_len,
    int *out_status,
    int timeout_ms,
    const atomic_int *cancel
) {
    if (!callback) return HTTP_MIN_ERR_ARGUMENT;
    return do_request(
        "POST", NULL, client, content_type, headers, header_count, body, body_len,
        NULL, 0, out_len, out_status, timeout_ms, cancel,
        callback, callback_user, max_body_len);
}

int http_min_post_cancel(
    const char *url,
    const char *content_type,
    const uint8_t *body,
    size_t body_len,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms,
    const atomic_int *cancel
) {
    return do_request(
        "POST", url, NULL, content_type, NULL, 0, body, body_len,
        out_body, out_cap, out_len,
        out_status, timeout_ms, cancel, NULL, NULL, 0);
}

int http_min_get_headers(
    const char *url,
    const http_min_header *headers,
    size_t header_count,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms
) {
    return do_request(
        "GET", url, NULL, NULL, headers, header_count, NULL, 0,
        out_body, out_cap, out_len, out_status,
        timeout_ms, NULL, NULL, NULL, 0);
}

int http_min_get(
    const char *url, uint8_t *out_body, size_t out_cap,
    size_t *out_len, int *out_status, int timeout_ms
) {
    return http_min_get_headers(url, NULL, 0u, out_body, out_cap,
                                out_len, out_status, timeout_ms);
}
