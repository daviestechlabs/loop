/* Bounded authenticated HTTP/1.1 fallback for voice turns. */
#define _POSIX_C_SOURCE 200809L
/* The HTTP owner also queues complete citation JSON. Other rings keep 64 KiB. */
#define VOICE_BYTE_RING_CAPACITY (128u * 1024u)

#include "gateway_http.h"
#include "gateway_http_fields.h"
#include "gateway_http_header_name.h"

#include "../common/base64.h"
#include "../common/byte_ring.h"
#include "../common/runtime_identity.h"
#include "../common/product_analytics.h"
#include "../common/service.h"
#include "../common/turn_response.h"
#include "../common/turn_response_json.h"
#include "../common/utf8.h"
#include "../common/voice_ascii.h"
#include "../common/voice_auth.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define GH_MAX_CONNECTIONS 40
#define GH_MAX_TURNS 32
#define GH_MAX_TURNS_PER_USER 4
#define GH_AUTH_NONCES 4096
#define GH_REQUEST_IDS 4096
#define GH_HEADER_CAP 8192u
#define GH_TOKEN_CAP 256u
#define GH_ACCEPT_TIMEOUT_MS 5000
#define GH_DEFAULT_TURN_TIMEOUT_MS 135000
#define GH_MAX_TURN_TIMEOUT_MS 300000
#define GH_REPLAY_WINDOW_SECONDS (5 * 60)
#define GH_AUTH_SKEW_SECONDS 30
#define GH_IDENTIFIER_MAX 127u
#define GH_IDLE_POLL_MS 200
#define GH_EVENT_RECEIVE_LIMIT (2u * DND_RAG_PUBLIC_JSON_CAP)
#define GH_EVENT_SUBJECT_PREFIX "ai.turn.events.cgh."
#define GH_EVENT_SUBJECT_LENGTH \
    (sizeof(GH_EVENT_SUBJECT_PREFIX) - 1u + 2u + 1u + \
     VOICE_AUTH_NONCE_HEX_LEN)

#ifndef VOICE_SOURCE_REVISION
#define VOICE_SOURCE_REVISION "unknown"
#endif

_Static_assert(
    DND_RAG_PUBLIC_JSON_CAP <= VOICE_BYTE_RING_CAPACITY,
    "response JSON capacity must fit the response ring");

typedef struct {
    int fd;
    int in_use;
    int request_done;
    int streaming;
    int terminal;
    int cancel_sent;
    char request_id[128];
    size_t request_id_len;
    char user_id[128];
    char response_subject[320];
    uint64_t deadline_ms;
    vbus_client *events;
    turn_response_state response;
    voice_analytics_turn analytics;
    uint8_t rx[GATEWAY_HTTP_RX_CAP + 1u];
    size_t rx_len;
    voice_byte_ring tx;
    size_t tx_dirty;
} gh_conn;

typedef struct {
    int listen_fd;
    int turn_timeout_ms;
    char token[GH_TOKEN_CAP];
    size_t token_len;
    vbus_client *bus;
    vbus_client *dispatching_events;
    vbus_stop_flag *stop;
    size_t conn_highwater;
    gh_conn conns[GH_MAX_CONNECTIONS];
    voice_auth_nonce_cache auth_nonces;
    voice_auth_replay_cache request_ids;
    voice_auth_nonce_pool capability_nonces;
    voice_auth_verifier auth_verifier;
    voice_runtime_identity runtime_identity;
    int runtime_identity_enabled;
    voice_analytics *analytics;
} gh_state;

static int subject_hex_value(unsigned char value) {
    return value >= (unsigned char)'0' && value <= (unsigned char)'9' ?
        (int)(value - (unsigned char)'0') :
        value >= (unsigned char)'a' && value <= (unsigned char)'f' ?
            (int)(value - (unsigned char)'a') + 10 : -1;
}

static int response_subject_write(
    char *out,
    size_t out_cap,
    size_t slot,
    const char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u]
) {
    static const char hex[] = "0123456789abcdef";
    size_t used = sizeof(GH_EVENT_SUBJECT_PREFIX) - 1u;
    if (!out || !nonce || out_cap <= GH_EVENT_SUBJECT_LENGTH ||
        slot >= (size_t)GH_MAX_CONNECTIONS)
        return -1;
    memcpy(out, GH_EVENT_SUBJECT_PREFIX, used);
    out[used++] = hex[slot >> 4u];
    out[used++] = hex[slot & 15u];
    out[used++] = '.';
    memcpy(out + used, nonce, VOICE_AUTH_NONCE_HEX_LEN + 1u);
    return 0;
}

static gh_conn *response_subject_connection(
    gh_state *state,
    const char *subject
) {
    size_t prefix_len = sizeof(GH_EVENT_SUBJECT_PREFIX) - 1u;
    int high;
    int low;
    size_t slot;
    gh_conn *conn;
    if (!state || !subject ||
        strnlen(subject, GH_EVENT_SUBJECT_LENGTH + 1u) !=
            GH_EVENT_SUBJECT_LENGTH ||
        memcmp(subject, GH_EVENT_SUBJECT_PREFIX, prefix_len) != 0 ||
        subject[prefix_len + 2u] != '.')
        return NULL;
    high = subject_hex_value((unsigned char)subject[prefix_len]);
    low = subject_hex_value((unsigned char)subject[prefix_len + 1u]);
    if (high < 0 || low < 0) return NULL;
    slot = (size_t)(unsigned int)(high * 16 + low);
    if (slot >= (size_t)GH_MAX_CONNECTIONS) return NULL;
    conn = &state->conns[slot];
    if (!conn->in_use || !conn->streaming || conn->terminal ||
        memcmp(
            conn->response_subject, subject,
            GH_EVENT_SUBJECT_LENGTH + 1u) != 0)
        return NULL;
    return conn;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)ts.tv_nsec / UINT64_C(1000000);
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t elapsed_us(uint64_t started, uint64_t ended) {
    return started != 0 && ended >= started ?
        (ended - started) / UINT64_C(1000) : 0;
}

static int64_t realtime_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000) + (int64_t)ts.tv_nsec / INT64_C(1000000);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}

static int open_listener(int port) {
    struct sockaddr_in address;
    int fd;
    int one = 1;
    if (port < 1 || port > 65535) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    const char *bind_address = svc_env("GATEWAY_HTTP_BIND_ADDRESS", "0.0.0.0");
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 64) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int safe_identifier_len(const char *value, size_t *out_len) {
    const unsigned char *p = (const unsigned char *)value;
    size_t len = 0;
    if (!p || !*p) return 0;
    while (*p) {
        if (!(voice_ascii_is_alnum(*p) || *p == '-' || *p == '_' || *p == '.' ||
              *p == ':' || *p == '@'))
            return 0;
        p++;
        len++;
        if (len > GH_IDENTIFIER_MAX) return 0;
    }
    if (out_len) *out_len = len;
    return 1;
}

static int safe_identifier(const char *value) {
    return safe_identifier_len(value, NULL);
}

static int safe_identifier_n(const char *value, size_t value_len) {
    size_t i;
    if (!value || value_len == 0u || value_len > GH_IDENTIFIER_MAX ||
        value[value_len] != '\0')
        return 0;
    for (i = 0u; i < value_len; ++i) {
        unsigned char byte = (unsigned char)value[i];
        if (!(voice_ascii_is_alnum(byte) || byte == (unsigned char)'-' ||
              byte == (unsigned char)'_' || byte == (unsigned char)'.' ||
              byte == (unsigned char)':' || byte == (unsigned char)'@'))
            return 0;
    }
    return 1;
}

static int tx_append(gh_conn *conn, const void *data, size_t len) {
    size_t dirty;
    if (!conn) return -1;
    dirty = voice_byte_ring_dirty_after_write(&conn->tx, conn->tx_dirty, len);
    if (!voice_byte_ring_write(&conn->tx, data, len)) return -1;
    conn->tx_dirty = dirty;
    return 0;
}

static void publish_cancel(gh_state *state, gh_conn *conn, const char *reason) {
    uint8_t wire[512];
    size_t wire_len;
    if (!state || !conn || !conn->streaming || conn->cancel_sent || !conn->request_id[0]) return;
    wire_len = pb_encode_turn_cancel(
        wire, sizeof(wire), conn->request_id, conn->user_id, reason ? reason : "disconnect");
    if (wire_len != 0)
        (void)vbus_publish(state->bus, SUBJ_TURN_CANCEL, wire, wire_len);
    conn->cancel_sent = 1;
}

static void conn_close(gh_state *state, gh_conn *conn, int cancel) {
    size_t rx_dirty;
    size_t slot;
    if (!conn || !conn->in_use) return;
    slot = (size_t)(conn - state->conns);
    rx_dirty = conn->rx_len < sizeof(conn->rx) ?
        conn->rx_len + 1u : sizeof(conn->rx);
    if (cancel) publish_cancel(state, conn, "client_disconnect");
    voice_analytics_finish(state->analytics, &conn->analytics,
        cancel && conn->analytics.client_interrupt && conn->cancel_sent ? VOICE_ANALYTICS_CANCELED :
        cancel ? VOICE_ANALYTICS_DISCONNECTED : VOICE_ANALYTICS_FAILED, monotonic_ns());
    close(conn->fd);
    /* A callback can reject and close its HTTP connection. Its event reader
     * stays alive until vbus_poll_one returns to the owner. */
    if (conn->events && conn->events != state->dispatching_events)
        vbus_close(conn->events);
    voice_byte_ring_scrub(&conn->tx, conn->tx_dirty);
    memset(conn, 0, offsetof(gh_conn, rx) + rx_dirty);
    conn->rx_len = 0u;
    conn->tx_dirty = 0u;
    conn->fd = -1;
    if (slot + 1u == state->conn_highwater)
        while (state->conn_highwater != 0u &&
               !state->conns[state->conn_highwater - 1u].in_use)
            state->conn_highwater--;
}

static int response_start(gh_conn *conn, int status, const char *reason, const char *type,
                          size_t content_length, int stream) {
    char header[512];
    int written;
    written = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "%sConnection: close\r\nCache-Control: no-store\r\n"
        "X-Service: c-voice-session-gateway\r\n\r\n",
        status, reason, type,
        stream ? "" : "Content-Length: ");
    if (written <= 0 || (size_t)written >= sizeof(header)) return -1;
    if (!stream) {
        char complete[512];
        written = snprintf(
            complete, sizeof(complete),
            "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Connection: close\r\nCache-Control: no-store\r\n"
            "X-Service: c-voice-session-gateway\r\n\r\n",
            status, reason, type, content_length);
        if (written <= 0 || (size_t)written >= sizeof(complete)) return -1;
        return tx_append(conn, complete, (size_t)written);
    }
    (void)content_length;
    return tx_append(conn, header, (size_t)written);
}

static void respond_json(gh_conn *conn, int status, const char *reason, const char *body) {
    size_t body_len = body ? strlen(body) : 0;
    conn->request_done = 1;
    conn->terminal = 1;
    if (response_start(conn, status, reason, "application/json", body_len, 0) != 0 ||
        tx_append(conn, body, body_len) != 0) {
        voice_byte_ring_reset(&conn->tx);
        conn->terminal = 1;
    }
}

static void respond_runtime_identity(gh_state *state, gh_conn *conn) {
    char header[512];
    int written;
    if (!state || !conn || !state->runtime_identity_enabled) {
        respond_json(conn, 404, "Not Found", "{\"error\":\"not_found\"}");
        return;
    }
    conn->request_done = 1;
    conn->terminal = 1;
    written = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nX-Service: c-voice-session-gateway\r\n"
        VOICE_RUNTIME_IDENTITY_HEADER ": %s\r\n\r\n",
        state->runtime_identity.body_len,
        state->runtime_identity.sha256);
    if (written <= 0 || (size_t)written >= sizeof(header) ||
        tx_append(conn, header, (size_t)written) != 0 ||
        tx_append(
            conn,
            state->runtime_identity.body,
            state->runtime_identity.body_len) != 0) {
        voice_byte_ring_reset(&conn->tx);
        conn->terminal = 1;
    }
}

enum {
    GH_HEADERS_INVALID = -1,
    GH_HEADERS_COMPLETE = 0,
    GH_HEADERS_INCOMPLETE = 1,
    GH_HEADER_AUTH_ALL = GH_HEADER_AUTH_USER | GH_HEADER_AUTH_TIMESTAMP |
        GH_HEADER_AUTH_NONCE | GH_HEADER_AUTH_SIGNATURE,
};

typedef struct {
    size_t content_length;
    int64_t auth_timestamp;
    char auth_user[128];
    char auth_nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    char auth_signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    unsigned present;
    unsigned rejected;
} gh_request_headers;

static int request_headers_parse(
    const char *headers,
    size_t headers_len,
    gh_request_headers *out,
    size_t *request_line_len,
    size_t *parsed_headers_len
) {
    const char *line = headers;
    const char *limit;
    int have_request_line = 0;
    if (!headers || !out || !request_line_len || !parsed_headers_len)
        return GH_HEADERS_INVALID;
    /* Callers read a value only after its presence bit passes without rejection. */
    out->present = 0u;
    out->rejected = 0u;
    *request_line_len = 0u;
    *parsed_headers_len = 0u;
    limit = headers + headers_len;
    while (line < limit) {
        const char *eol;
        const char *colon;
        const char *scan;
        const char *value;
        char *destination = NULL;
        size_t destination_cap = 0;
        size_t line_len;
        size_t name_len;
        size_t value_len;
        unsigned field = 0;
        eol = memchr(line, '\r', (size_t)(limit - line));
        if (!eol || eol + 1 == limit) return GH_HEADERS_INCOMPLETE;
        if (eol[1] != '\n') return GH_HEADERS_INVALID;
        line_len = (size_t)(eol - line);
        if (!voice_ascii_http_line_valid(line, line_len))
            return GH_HEADERS_INVALID;
        if (!have_request_line) {
            if (line_len == 0u) return GH_HEADERS_INVALID;
            *request_line_len = line_len;
            line = eol + 2;
            have_request_line = 1;
            continue;
        }
        if (line_len == 0u) {
            *parsed_headers_len = (size_t)(eol + 2 - headers);
            return GH_HEADERS_COMPLETE;
        }
        colon = memchr(line, ':', line_len);
        if (!colon || colon == line || colon == eol)
            return GH_HEADERS_INVALID;
        for (scan = line; scan < colon; ++scan) {
            unsigned char byte = (unsigned char)*scan;
            if (!(voice_ascii_is_alnum(byte) || byte == '!' || byte == '#' || byte == '$' ||
                  byte == '%' || byte == '&' || byte == '\'' || byte == '*' ||
                  byte == '+' || byte == '-' || byte == '.' || byte == '^' ||
                  byte == '_' || byte == '`' || byte == '|' || byte == '~'))
                return GH_HEADERS_INVALID;
        }
        name_len = (size_t)(colon - line);
        field = gateway_http_validated_header_field(line, name_len);
        if (field == GH_HEADER_AUTH_USER) {
            destination = out->auth_user;
            destination_cap = sizeof(out->auth_user);
        } else if (field == GH_HEADER_AUTH_NONCE) {
            destination = out->auth_nonce;
            destination_cap = sizeof(out->auth_nonce);
        } else if (field == GH_HEADER_AUTH_SIGNATURE) {
            destination = out->auth_signature;
            destination_cap = sizeof(out->auth_signature);
        }
        if (field == 0) {
            line = eol + 2;
            continue;
        }
        if ((out->present & field) != 0) {
            out->rejected |= field;
            line = eol + 2;
            continue;
        }
        out->present |= field;
        if (field == GH_HEADER_TRANSFER_ENCODING) {
            line = eol + 2;
            continue;
        }
        value = colon + 1;
        while (value < eol && (*value == ' ' || *value == '\t')) value++;
        value_len = (size_t)(eol - value);
        while (value_len > 0 &&
               (value[value_len - 1u] == ' ' || value[value_len - 1u] == '\t'))
            value_len--;
        if (field == GH_HEADER_CONTENT_LENGTH) {
            if (gateway_http_content_length_parse(
                    value, value_len, &out->content_length) != 0)
                out->rejected |= field;
            line = eol + 2;
            continue;
        }
        if (field == GH_HEADER_CONTENT_TYPE) {
            if (!gateway_http_content_type_is_protobuf(value, value_len))
                out->rejected |= field;
            line = eol + 2;
            continue;
        }
        if (field == GH_HEADER_AUTH_TIMESTAMP) {
            if (gateway_http_timestamp_parse(
                    value, value_len, &out->auth_timestamp) != 0)
                out->rejected |= field;
            line = eol + 2;
            continue;
        }
        if (!destination || value_len >= destination_cap) {
            out->rejected |= field;
            line = eol + 2;
            continue;
        }
        memcpy(destination, value, value_len);
        destination[value_len] = '\0';
        line = eol + 2;
    }
    return GH_HEADERS_INCOMPLETE;
}

static int request_headers_have_exact(
    const gh_request_headers *headers,
    unsigned fields
) {
    return headers && (headers->present & fields) == fields &&
        (headers->rejected & fields) == 0;
}

typedef enum {
    GH_ROUTE_OTHER = 0,
    GH_ROUTE_HEALTH,
    GH_ROUTE_READY,
    GH_ROUTE_RUNTIME_IDENTITY,
    GH_ROUTE_TURN_START,
    GH_ROUTE_TURN_CANCEL,
} gh_request_route;

static int request_line_route(
    const char *line,
    size_t line_len,
    gh_request_route *route
) {
    const char *method_end;
    const char *path;
    const char *path_end;
    const char *version;
    size_t method_len;
    size_t path_len;
    size_t version_len;
    size_t i;
    if (!line || !route) return -1;
    method_end = memchr(line, ' ', line_len);
    if (!method_end || method_end == line) return -1;
    path = method_end + 1;
    path_end = memchr(path, ' ', line_len - (size_t)(path - line));
    if (!path_end || path_end == path) return -1;
    version = path_end + 1;
    method_len = (size_t)(method_end - line);
    path_len = (size_t)(path_end - path);
    version_len = line_len - (size_t)(version - line);
    if (method_len >= 16u || path_len >= 256u ||
        version_len != sizeof("HTTP/1.1") - 1u ||
        (memcmp(version, "HTTP/1.1", version_len) != 0 &&
         memcmp(version, "HTTP/1.0", version_len) != 0))
        return -1;
    for (i = 0; i < method_len; ++i) {
        unsigned char byte = (unsigned char)line[i];
        if (!(voice_ascii_is_alnum(byte) || byte == '!' || byte == '#' || byte == '$' ||
              byte == '%' || byte == '&' || byte == '\'' || byte == '*' ||
              byte == '+' || byte == '-' || byte == '.' || byte == '^' ||
              byte == '_' || byte == '`' || byte == '|' || byte == '~'))
            return -1;
    }
    for (i = 0; i < path_len; ++i) {
        unsigned char byte = (unsigned char)path[i];
        if (byte <= 0x20u || byte > 0x7eu) return -1;
    }
    *route = GH_ROUTE_OTHER;
    if (method_len == sizeof("GET") - 1u &&
        memcmp(line, "GET", sizeof("GET") - 1u) == 0) {
        if (path_len == sizeof("/healthz") - 1u &&
            memcmp(path, "/healthz", sizeof("/healthz") - 1u) == 0)
            *route = GH_ROUTE_HEALTH;
        else if (path_len == sizeof("/readyz") - 1u &&
                 memcmp(path, "/readyz", sizeof("/readyz") - 1u) == 0)
            *route = GH_ROUTE_READY;
        else if (path_len == sizeof(VOICE_RUNTIME_IDENTITY_PATH) - 1u &&
                 memcmp(
                     path,
                     VOICE_RUNTIME_IDENTITY_PATH,
                     sizeof(VOICE_RUNTIME_IDENTITY_PATH) - 1u) == 0)
            *route = GH_ROUTE_RUNTIME_IDENTITY;
    } else if (method_len == sizeof("POST") - 1u &&
               memcmp(line, "POST", sizeof("POST") - 1u) == 0) {
        if (path_len == sizeof("/v1/voice/turns") - 1u &&
            memcmp(path, "/v1/voice/turns", sizeof("/v1/voice/turns") - 1u) == 0)
            *route = GH_ROUTE_TURN_START;
        else if (path_len == sizeof("/v1/voice/turns/cancel") - 1u &&
                 memcmp(
                     path, "/v1/voice/turns/cancel",
                     sizeof("/v1/voice/turns/cancel") - 1u) == 0)
            *route = GH_ROUTE_TURN_CANCEL;
    }
    return 0;
}

static int append_event_buffered(
    gh_conn *conn,
    const turn_response_event *event,
    int64_t timestamp
) {
    char frame[TURN_RESPONSE_JSON_CAPACITY];
    size_t frame_len = turn_response_json_encode(
        frame, sizeof(frame), event, timestamp);
    if (frame_len == 0u) return -1;
    return tx_append(conn, frame, frame_len);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int append_extended_event(gh_conn *conn, const turn_response_event *event, int64_t timestamp) {
    char frame[DND_RAG_PUBLIC_JSON_CAP];
    size_t length = turn_response_json_encode(frame, sizeof(frame), event, timestamp);
    return length ? tx_append(conn, frame, length) : -1;
}

static int append_event(gh_conn *conn, const turn_response_event *event) {
    uint8_t *write_span;
    size_t write_span_len;
    size_t write_offset;
    size_t dirty_end;
    size_t frame_len;
    int64_t timestamp;
    if (!conn) return -1;
    timestamp = realtime_ms();
    if (event->retrieval || event->roster || event->initiative)
        return append_extended_event(conn, event, timestamp);
    if (voice_byte_ring_write_span(
            &conn->tx, &write_span, &write_span_len) &&
        write_span_len >= TURN_RESPONSE_JSON_CAPACITY) {
        write_offset = (size_t)(write_span - conn->tx.data);
        frame_len = turn_response_json_encode(
            (char *)write_span, TURN_RESPONSE_JSON_CAPACITY, event, timestamp);
        if (frame_len == 0u ||
            !voice_byte_ring_write_commit(&conn->tx, frame_len)) {
            dirty_end = write_offset + TURN_RESPONSE_JSON_CAPACITY;
            if (dirty_end > conn->tx_dirty) conn->tx_dirty = dirty_end;
            return -1;
        }
        dirty_end = write_offset + frame_len;
        if (dirty_end > conn->tx_dirty) conn->tx_dirty = dirty_end;
        return 0;
    }
    return append_event_buffered(conn, event, timestamp);
}

static void fail_stream(gh_state *state, gh_conn *conn, const char *reason) {
    turn_response_event failed;
    if (!state || !conn || conn->terminal) return;
    voice_analytics_finish(state->analytics, &conn->analytics, VOICE_ANALYTICS_FAILED, monotonic_ns());
    publish_cancel(state, conn, reason);
    memset(&failed, 0, sizeof(failed));
    failed.request_id = conn->request_id;
    failed.type = "failed";
    failed.request_id_len = (uint16_t)conn->request_id_len;
    failed.type_len = sizeof("failed") - 1u;
    failed.text = "upstream_failed";
    failed.speech_text = "";
    failed.display_text = "";
    failed.type_id = 12;
    if (state->runtime_identity_enabled) {
        failed.runtime_identity_sha256 = state->runtime_identity.sha256;
        failed.runtime_identity_sha256_len = VOICE_RUNTIME_IDENTITY_HASH_LEN;
    }
    conn->response.terminal = 1;
    if (append_event(conn, &failed) != 0) {
        conn_close(state, conn, 0);
        return;
    }
    conn->terminal = 1;
}

static void on_turn_event(const char *subject, const char *reply, const uint8_t *data,
                          size_t data_len, void *user) {
    gh_state *state = (gh_state *)user;
    gh_conn *conn;
    turn_event_c event;
    turn_response_event safe_event;
    enum turn_response_action action;
    (void)reply;
    if (!state || !subject) return;
    conn = response_subject_connection(state, subject);
    if (!conn) return;
    if (pb_decode_turn_event_public_active(
            data, data_len, conn->request_id,
            conn->request_id_len, &event) != 0) {
        fail_stream(state, conn, "invalid_turn_event");
        return;
    }
    action = turn_response_filter_public_active_n(
        &conn->response, &event, conn->request_id,
        conn->request_id_len, &safe_event);
    if (action == TURN_RESPONSE_DROP || action == TURN_RESPONSE_HOLD_COMPLETED) return;
    if (action == TURN_RESPONSE_REJECT) {
        fail_stream(state, conn, "invalid_turn_event");
        return;
    }
    if (state->runtime_identity_enabled) {
        safe_event.runtime_identity_sha256 = state->runtime_identity.sha256;
        safe_event.runtime_identity_sha256_len =
            VOICE_RUNTIME_IDENTITY_HASH_LEN;
    }
    if (append_event(conn, &safe_event) != 0) {
        publish_cancel(state, conn, "edge_backpressure");
        conn_close(state, conn, 0);
        return;
    }
    if (safe_event.audio_len && conn->analytics.admitted_ns && !conn->analytics.first_audio_ns)
        voice_analytics_audio(&conn->analytics, monotonic_ns());
    if (action == TURN_RESPONSE_FORWARD_AND_COMPLETE) {
        turn_response_event completed = safe_event;
        completed.type_id = 10;
        completed.type = "completed";
        completed.type_len = sizeof("completed") - 1u;
        completed.audio = NULL;
        completed.audio_len = 0;
        if (append_event(conn, &completed) != 0) {
            publish_cancel(state, conn, "edge_backpressure");
            conn_close(state, conn, 0);
            return;
        }
        conn->terminal = 1;
        voice_analytics_finish(state->analytics, &conn->analytics, VOICE_ANALYTICS_COMPLETED, monotonic_ns());
    } else if (action == TURN_RESPONSE_FORWARD_TERMINAL) {
        conn->terminal = 1;
        voice_analytics_finish(state->analytics, &conn->analytics,
            safe_event.type_id == 10 ? VOICE_ANALYTICS_COMPLETED :
            safe_event.type_id == 11 ? VOICE_ANALYTICS_CANCELED : VOICE_ANALYTICS_FAILED,
            monotonic_ns());
    }
}

static int remember_auth_nonce(gh_state *state, const char *nonce, int64_t now) {
    if (!state) return -1;
    return voice_auth_nonce_index_accept(
               &state->auth_nonces, nonce, now,
               GH_AUTH_SKEW_SECONDS * 2) == VOICE_AUTH_OK ? 0 : -1;
}

static int auth_user(
    gh_state *state,
    const gh_request_headers *headers,
    const char *method,
    const char *path,
    const uint8_t *body,
    size_t body_len
) {
    int64_t now = (int64_t)time(NULL);
    if (!state || now <= 0 ||
        !request_headers_have_exact(headers, GH_HEADER_AUTH_ALL) ||
        !safe_identifier(headers->auth_user))
        return -1;
    if (headers->auth_timestamp < now - GH_AUTH_SKEW_SECONDS ||
        headers->auth_timestamp > now + GH_AUTH_SKEW_SECONDS ||
        voice_auth_verifier_verify(
            &state->auth_verifier, method, path, headers->auth_user,
            headers->auth_timestamp,
            headers->auth_nonce, body, body_len,
            headers->auth_signature) != VOICE_AUTH_OK ||
        remember_auth_nonce(state, headers->auth_nonce, now) != 0) return -1;
    return 0;
}

static gh_conn *find_active(gh_state *state, const char *request_id, const char *user_id) {
    size_t i;
    for (i = 0; i < state->conn_highwater; ++i) {
        gh_conn *conn = &state->conns[i];
        if (conn->in_use && conn->streaming && !conn->terminal &&
            strcmp(conn->request_id, request_id) == 0 &&
            (!user_id || strcmp(conn->user_id, user_id) == 0)) return conn;
    }
    return NULL;
}

static int request_id_accept(
    gh_state *state,
    const char *request_id,
    size_t request_id_len
) {
    int64_t now = (int64_t)time(NULL);
    if (!state || !request_id || request_id_len == 0u ||
        request_id_len > GH_IDENTIFIER_MAX || now <= 0) return VOICE_AUTH_ERR_ARGUMENT;
    return voice_auth_replay_index_accept(
        &state->request_ids, request_id, request_id_len,
        now, GH_REPLAY_WINDOW_SECONDS);
}

static int active_turn_count(const gh_state *state) {
    size_t i;
    int count = 0;
    for (i = 0; i < state->conn_highwater; ++i)
        if (state->conns[i].in_use && state->conns[i].streaming && !state->conns[i].terminal)
            count++;
    return count;
}

static int active_user_turn_count(const gh_state *state, const char *user_id) {
    size_t i;
    int count = 0;
    if (!state || !user_id) return 0;
    for (i = 0; i < state->conn_highwater; ++i)
        if (state->conns[i].in_use && state->conns[i].streaming &&
            !state->conns[i].terminal &&
            strcmp(state->conns[i].user_id, user_id) == 0) count++;
    return count;
}

static void handle_turn_start(
    gh_state *state,
    gh_conn *conn,
    const gh_request_headers *headers,
    const uint8_t *body,
    size_t body_len
) {
    turn_start_c turn;
    turn_acceptance_metrics acceptance_metrics;
    uint8_t wire[DND_TURN_START_WIRE_MAX];
    char accepted[TURN_ACCEPTANCE_JSON_CAPACITY];
    char event_nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    size_t accepted_len;
    size_t request_id_len;
    size_t wire_len;
    int64_t accepted_at;
    uint64_t prepare_started = monotonic_ns();
    uint64_t auth_started = monotonic_ns();
    uint64_t auth_ended;
    uint64_t admission_ended;
    uint64_t capability_ended;
    uint64_t encode_started;
    uint64_t encode_ended;
    uint64_t publish_started;
    uint64_t publish_ended;
    int replay_status;
    size_t connection_slot;
    if (auth_user(
            state, headers, "POST", "/v1/voice/turns",
            body, body_len) != 0) {
        respond_json(conn, 401, "Unauthorized", "{\"error\":\"unauthorized\"}");
        return;
    }
    auth_ended = monotonic_ns();
    if (pb_decode_turn_start(body, body_len, &turn) != 0 ||
        !safe_identifier_n(turn.request_id, turn.request_id_len) ||
        !safe_identifier_n(turn.user_id, turn.user_id_len) ||
        strcmp(headers->auth_user, turn.user_id) != 0 ||
        turn.text_len == 0u ||
        !utf8_validate_v1((const uint8_t *)turn.text, turn.text_len) ||
        (turn.session_id_len != 0u &&
         !safe_identifier_n(turn.session_id, turn.session_id_len))) {
        respond_json(conn, 400, "Bad Request", "{\"error\":\"invalid_turn\"}");
        return;
    }
    request_id_len = turn.request_id_len;
    if (find_active(state, turn.request_id, NULL)) {
        respond_json(conn, 409, "Conflict", "{\"error\":\"request_id_reused\"}");
        return;
    }
    if (active_turn_count(state) >= GH_MAX_TURNS) {
        respond_json(conn, 503, "Service Unavailable", "{\"error\":\"turn_capacity\"}");
        return;
    }
    if (active_user_turn_count(state, headers->auth_user) >= GH_MAX_TURNS_PER_USER) {
        respond_json(conn, 429, "Too Many Requests", "{\"error\":\"user_turn_capacity\"}");
        return;
    }
    replay_status = request_id_accept(state, turn.request_id, request_id_len);
    if (replay_status == VOICE_AUTH_ERR_REPLAY) {
        respond_json(conn, 409, "Conflict", "{\"error\":\"request_id_reused\"}");
        return;
    }
    if (replay_status != VOICE_AUTH_OK) {
        respond_json(conn, 503, "Service Unavailable", "{\"error\":\"request_id_capacity\"}");
        return;
    }
    admission_ended = monotonic_ns();
    if (voice_auth_nonce_pool_next(&state->capability_nonces, event_nonce) != VOICE_AUTH_OK) {
        respond_json(conn, 503, "Service Unavailable", "{\"error\":\"entropy_unavailable\"}");
        return;
    }
    connection_slot = (size_t)(conn - state->conns);
    if (response_subject_write(
            turn.response_subject, sizeof(turn.response_subject),
            connection_slot, event_nonce) != 0) {
        respond_json(conn, 400, "Bad Request", "{\"error\":\"invalid_turn\"}");
        return;
    }
    turn.response_subject_len = GH_EVENT_SUBJECT_LENGTH;
    capability_ended = monotonic_ns();
    encode_started = monotonic_ns();
    wire_len = pb_encode_turn_start_prepared(wire, sizeof(wire), &turn);
    encode_ended = monotonic_ns();
    /* Start and cancel share this VBus connection. FIFO ordering prevents a
     * fast disconnect from overtaking its turn start on another publisher. */
    publish_started = monotonic_ns();
    conn->events = wire_len ?
        vbus_connect_bounded(vbus_default_path(), GH_EVENT_RECEIVE_LIMIT) : NULL;
    if (!conn->events || vbus_subscribe_flow(
            conn->events, turn.response_subject, on_turn_event, state) != 0 ||
        vbus_publish(state->bus, SUBJ_TURN_START, wire, wire_len) != 0) {
        vbus_close(conn->events);
        conn->events = NULL;
        respond_json(conn, 503, "Service Unavailable", "{\"error\":\"bus_unavailable\"}");
        return;
    }
    publish_ended = monotonic_ns();
    if (state->analytics)
        voice_analytics_begin(&conn->analytics, &turn, 0, 0, publish_ended);
    conn->request_done = 1;
    conn->streaming = 1;
    conn->response.wait_for_pcm = turn.enable_tts ? 1 : 0;
    conn->deadline_ms = monotonic_ms() + (uint64_t)state->turn_timeout_ms;
    conn->request_id_len = request_id_len;
    memcpy(conn->request_id, turn.request_id, conn->request_id_len + 1u);
    memcpy(conn->user_id, turn.user_id, turn.user_id_len + 1u);
    memcpy(
        conn->response_subject, turn.response_subject,
        GH_EVENT_SUBJECT_LENGTH + 1u);
    if (response_start(conn, 200, "OK", "application/x-ndjson", 0, 1) != 0) {
        publish_cancel(state, conn, "edge_capacity");
        conn_close(state, conn, 0);
        return;
    }
    accepted_at = realtime_ms();
    if (accepted_at <= 0) {
        publish_cancel(state, conn, "edge_clock");
        conn_close(state, conn, 0);
        return;
    }
    acceptance_metrics.auth_us = elapsed_us(auth_started, auth_ended);
    acceptance_metrics.vbus_publish_us = elapsed_us(
        publish_started, publish_ended);
    acceptance_metrics.prepare_us = elapsed_us(prepare_started, publish_ended);
    acceptance_metrics.admission_us = elapsed_us(auth_ended, admission_ended);
    acceptance_metrics.capability_us = elapsed_us(
        admission_ended, capability_ended);
    acceptance_metrics.encode_us = elapsed_us(encode_started, encode_ended);
    accepted_len = turn_acceptance_json_encode(
        accepted, sizeof(accepted), turn.request_id, conn->request_id_len,
        accepted_at, &acceptance_metrics);
    if (accepted_len == 0u || tx_append(conn, accepted, accepted_len) != 0) {
        publish_cancel(state, conn, "edge_capacity");
        conn_close(state, conn, 0);
    }
}

static void handle_turn_cancel(
    gh_state *state,
    gh_conn *conn,
    const gh_request_headers *headers,
    const uint8_t *body,
    size_t body_len
) {
    turn_cancel_c cancel;
    gh_conn *active;
    uint8_t wire[512];
    size_t wire_len;
    char response[320];
    int written;
    if (auth_user(
            state, headers, "POST", "/v1/voice/turns/cancel",
            body, body_len) != 0) {
        respond_json(conn, 401, "Unauthorized", "{\"error\":\"unauthorized\"}");
        return;
    }
    if (pb_decode_turn_cancel(body, body_len, &cancel) != 0 ||
        !safe_identifier(cancel.request_id) || !safe_identifier(cancel.user_id) ||
        strcmp(headers->auth_user, cancel.user_id) != 0) {
        respond_json(conn, 400, "Bad Request", "{\"error\":\"invalid_cancel\"}");
        return;
    }
    active = find_active(state, cancel.request_id, headers->auth_user);
    if (active) {
        wire_len = pb_encode_turn_cancel(
            wire, sizeof(wire), cancel.request_id, headers->auth_user,
            cancel.reason[0] ? cancel.reason : "client_cancel");
        if (wire_len == 0 || vbus_publish(state->bus, SUBJ_TURN_CANCEL, wire, wire_len) != 0) {
            respond_json(conn, 503, "Service Unavailable", "{\"error\":\"bus_unavailable\"}");
            return;
        }
        active->cancel_sent = 1;
        active->analytics.client_interrupt = 1;
    }
    written = snprintf(
        response, sizeof(response),
        "{\"ok\":true,\"request_id\":\"%s\",\"cancelled\":%s}",
        cancel.request_id, active ? "true" : "false");
    if (written <= 0 || (size_t)written >= sizeof(response)) {
        respond_json(conn, 500, "Internal Server Error", "{\"error\":\"encode_failed\"}");
        return;
    }
    respond_json(conn, 200, "OK", response);
}

static void process_request(gh_state *state, gh_conn *conn) {
    gh_request_headers headers;
    gh_request_route route;
    size_t headers_len;
    size_t request_line_len;
    size_t body_len = 0;
    size_t have;
    int header_result = request_headers_parse(
        (const char *)conn->rx,
        conn->rx_len,
        &headers,
        &request_line_len,
        &headers_len);
    if (header_result == GH_HEADERS_INCOMPLETE) {
        if (conn->rx_len > GH_HEADER_CAP)
            respond_json(conn, 431, "Request Header Fields Too Large", "{\"error\":\"headers_too_large\"}");
        return;
    }
    if (header_result != GH_HEADERS_COMPLETE || headers_len > GH_HEADER_CAP) {
        respond_json(conn, 400, "Bad Request", "{\"error\":\"invalid_request\"}");
        return;
    }
    if (request_line_route(
            (const char *)conn->rx, request_line_len, &route) != 0) {
        respond_json(conn, 400, "Bad Request", "{\"error\":\"invalid_request\"}");
        return;
    }
    if ((headers.present & GH_HEADER_TRANSFER_ENCODING) != 0) {
        respond_json(conn, 400, "Bad Request", "{\"error\":\"unsupported_framing\"}");
        return;
    }
    if (route == GH_ROUTE_HEALTH) {
        char health[320];
        voice_analytics_counts counts = voice_analytics_get_counts(state->analytics);
        (void)snprintf(health, sizeof(health),
            "{\"live\":true,\"service\":\"c-voice-session-gateway\","
            "\"analytics\":{\"enabled\":%s,\"queued\":%u,\"accepted\":%u,\"failed\":%u,\"dropped\":%u}}",
            state->analytics ? "true" : "false", counts.queued, counts.accepted, counts.failed, counts.dropped);
        respond_json(conn, 200, "OK", health);
        return;
    }
    if (route == GH_ROUTE_READY) {
        respond_json(conn, 200, "OK", "{\"ready\":true,\"auth\":\"configured\",\"bus\":\"connected\"}");
        return;
    }
    if (route == GH_ROUTE_RUNTIME_IDENTITY) {
        respond_runtime_identity(state, conn);
        return;
    }
    if (route != GH_ROUTE_TURN_START && route != GH_ROUTE_TURN_CANCEL) {
        respond_json(conn, 404, "Not Found", "{\"error\":\"not_found\"}");
        return;
    }
    if (!request_headers_have_exact(&headers, GH_HEADER_CONTENT_LENGTH) ||
        headers.content_length == 0u ||
        headers.content_length > GATEWAY_HTTP_RX_CAP - headers_len) {
        respond_json(conn, 400, "Bad Request", "{\"error\":\"invalid_content_length\"}");
        return;
    }
    body_len = headers.content_length;
    if (!request_headers_have_exact(&headers, GH_HEADER_CONTENT_TYPE)) {
        respond_json(conn, 415, "Unsupported Media Type", "{\"error\":\"protobuf_required\"}");
        return;
    }
    have = conn->rx_len - headers_len;
    if (have < body_len) return;
    if (have != body_len) {
        respond_json(conn, 400, "Bad Request", "{\"error\":\"trailing_request_bytes\"}");
        return;
    }
    if (route == GH_ROUTE_TURN_START)
        handle_turn_start(
            state, conn, &headers, conn->rx + headers_len, body_len);
    else
        handle_turn_cancel(
            state, conn, &headers, conn->rx + headers_len, body_len);
}

static void read_connection(gh_state *state, gh_conn *conn);

static void accept_connections(gh_state *state) {
    for (;;) {
        int fd = accept(state->listen_fd, NULL, NULL);
        int i;
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (set_nonblocking(fd) != 0) {
            close(fd);
            continue;
        }
        for (i = 0; i < (int)state->conn_highwater; ++i)
            if (!state->conns[i].in_use) break;
        if (i == (int)state->conn_highwater &&
            state->conn_highwater < (size_t)GH_MAX_CONNECTIONS)
            state->conn_highwater++;
        else if (i == GH_MAX_CONNECTIONS) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 16\r\n"
                "Connection: close\r\n\r\n{\"error\":\"busy\"}";
            (void)send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        /* calloc and conn_close reset metadata and erase each written prefix. */
        state->conns[i].fd = fd;
        state->conns[i].in_use = 1;
        state->conns[i].deadline_ms = monotonic_ms() + GH_ACCEPT_TIMEOUT_MS;
        read_connection(state, &state->conns[i]);
    }
}

static void read_connection(gh_state *state, gh_conn *conn) {
    for (;;) {
        ssize_t count;
        if (conn->request_done) {
            uint8_t byte;
            count = recv(conn->fd, &byte, 1, MSG_PEEK);
        } else {
            if (conn->rx_len == GATEWAY_HTTP_RX_CAP) {
                respond_json(conn, 413, "Payload Too Large", "{\"error\":\"request_too_large\"}");
                return;
            }
            count = recv(
                conn->fd, conn->rx + conn->rx_len,
                GATEWAY_HTTP_RX_CAP - conn->rx_len, 0);
        }
        if (count > 0) {
            if (conn->request_done) {
                conn_close(state, conn, conn->streaming && !conn->terminal);
                return;
            }
            conn->rx_len += (size_t)count;
            conn->rx[conn->rx_len] = '\0';
            process_request(state, conn);
            if (conn->request_done || !conn->in_use) return;
            continue;
        }
        if (count == 0) {
            conn_close(state, conn, conn->streaming && !conn->terminal);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        conn_close(state, conn, conn->streaming && !conn->terminal);
        return;
    }
}

static void write_connection(gh_state *state, gh_conn *conn) {
    const uint8_t *data;
    size_t data_len;
    while (voice_byte_ring_peek(&conn->tx, &data, &data_len)) {
        ssize_t count = send(conn->fd, data, data_len, MSG_NOSIGNAL);
        if (count > 0) {
            if (!voice_byte_ring_consume(&conn->tx, (size_t)count)) {
                conn_close(state, conn, conn->streaming && !conn->terminal);
                return;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        conn_close(state, conn, conn->streaming && !conn->terminal);
        return;
    }
    if (conn->terminal) conn_close(state, conn, 0);
}

static int read_turn_event(gh_state *state, gh_conn *conn) {
    vbus_client *events = conn->events;
    int result;
    if (!events || conn->terminal || conn->tx.size != 0u) return 0;
    /* Consume one frame only when the prior output has drained. A stalled
     * HTTP socket pauses its own subscriber; other turns keep their readers.
     * VBus retains its existing per-client and global pending-byte bounds. */
    state->dispatching_events = events;
    result = vbus_poll_one(events, 0);
    state->dispatching_events = NULL;
    if (conn->events != events) {
        vbus_close(events);
        return result > 0;
    }
    if (result < 0) fail_stream(state, conn, "event_bus_unavailable");
    if (conn->in_use && conn->terminal && conn->events) {
        vbus_close(conn->events);
        conn->events = NULL;
    }
    return result != 0;
}

static int load_token(gh_state *state) {
    const char *token = getenv("VOICE_GATEWAY_TOKEN");
    size_t len = token ? strlen(token) : 0;
    if (len < 32u || len >= sizeof(state->token)) return -1;
    memcpy(state->token, token, len + 1u);
    state->token_len = len;
    return 0;
}

static void destroy_replay_indexes(gh_state *state) {
    if (!state) return;
    voice_auth_nonce_pool_destroy(&state->capability_nonces);
    voice_auth_replay_index_destroy(&state->request_ids);
    voice_auth_nonce_index_destroy(&state->auth_nonces);
}

int gateway_http_run(int port, vbus_stop_flag *stop) {
    gh_state *state;
    int bus_fd;
    int identity_status;
    int result = 0;
    int i;
    state = (gh_state *)calloc(1, sizeof(*state));
    if (!state) return -1;
    state->listen_fd = -1;
    state->stop = stop;
    identity_status = voice_runtime_identity_load(
        &state->runtime_identity,
        VOICE_SOURCE_REVISION);
    if (identity_status < 0) {
        svc_log(
            "voice-session-gateway",
            "runtime identity must contain complete source and Pod provenance");
        free(state);
        return -1;
    }
    state->runtime_identity_enabled = identity_status;
    state->turn_timeout_ms = svc_env_int_range(
        "GATEWAY_TURN_TIMEOUT_MS", GH_DEFAULT_TURN_TIMEOUT_MS, 1000, GH_MAX_TURN_TIMEOUT_MS);
    for (i = 0; i < GH_MAX_CONNECTIONS; ++i) state->conns[i].fd = -1;
    if (voice_auth_nonce_index_init(&state->auth_nonces, GH_AUTH_NONCES) != VOICE_AUTH_OK) {
        svc_log("voice-session-gateway", "cannot allocate bounded replay index");
        free(state);
        return -1;
    }
    if (voice_auth_replay_index_init(
            &state->request_ids, GH_REQUEST_IDS,
            GH_IDENTIFIER_MAX) != VOICE_AUTH_OK) {
        svc_log("voice-session-gateway", "cannot allocate request ID replay index");
        destroy_replay_indexes(state);
        free(state);
        return -1;
    }
    if (load_token(state) != 0) {
        svc_log("voice-session-gateway", "VOICE_GATEWAY_TOKEN must contain 32-255 bytes");
        destroy_replay_indexes(state);
        free(state);
        return -1;
    }
    if (voice_auth_nonce_pool_init(&state->capability_nonces) != VOICE_AUTH_OK) {
        svc_log("voice-session-gateway", "CSPRNG initialization failed");
        memset(state->token, 0, sizeof(state->token));
        destroy_replay_indexes(state);
        free(state);
        return -1;
    }
    state->bus = svc_connect_bus();
    if (!state->bus) {
        memset(state->token, 0, sizeof(state->token));
        destroy_replay_indexes(state);
        free(state);
        return -1;
    }
    bus_fd = vbus_poll_fd(state->bus);
    if (bus_fd < 0) {
        vbus_close(state->bus);
        memset(state->token, 0, sizeof(state->token));
        destroy_replay_indexes(state);
        free(state);
        return -1;
    }
    state->listen_fd = open_listener(port);
    if (state->listen_fd < 0) {
        vbus_close(state->bus);
        memset(state->token, 0, sizeof(state->token));
        destroy_replay_indexes(state);
        free(state);
        return -1;
    }
    if (voice_auth_verifier_init(
            &state->auth_verifier,
            state->token,
            state->token_len) != VOICE_AUTH_OK) {
        svc_log("voice-session-gateway", "cannot initialize request verifier");
        close(state->listen_fd);
        vbus_close(state->bus);
        memset(state->token, 0, sizeof(state->token));
        destroy_replay_indexes(state);
        free(state);
        return -1;
    }
    state->analytics = voice_analytics_start(getenv("VOICE_ANALYTICS_OTLP_URL"), "c-voice-session-gateway");
    svc_log("voice-session-gateway", "authenticated HTTP fallback listening port=%d", port);
    while (!stop || atomic_load_explicit(stop, memory_order_relaxed) == 0) {
        struct pollfd pollfds[2 * GH_MAX_CONNECTIONS + 2];
        int indexes[2 * GH_MAX_CONNECTIONS + 2];
        nfds_t count = 2;
        uint64_t now;
        int poll_result;
        int dispatched = 0;
        for (i = 0; i < (int)state->conn_highwater; ++i) {
            gh_conn *conn = &state->conns[i];
            if (!conn->in_use) continue;
            dispatched |= read_turn_event(state, conn);
            if (conn->in_use && conn->tx.size != 0u) write_connection(state, conn);
        }
        pollfds[0].fd = state->listen_fd;
        pollfds[0].events = POLLIN;
        indexes[0] = -1;
        pollfds[1].fd = bus_fd;
        pollfds[1].events = POLLIN;
        indexes[1] = -2;
        for (i = 0; i < (int)state->conn_highwater; ++i) {
            if (!state->conns[i].in_use) continue;
            pollfds[count].fd = state->conns[i].fd;
            pollfds[count].events = POLLIN;
            if (state->conns[i].tx.size != 0u) pollfds[count].events |= POLLOUT;
            indexes[count] = i;
            count++;
            if (state->conns[i].events && !state->conns[i].terminal &&
                state->conns[i].tx.size == 0u) {
                pollfds[count].fd = vbus_poll_fd(state->conns[i].events);
                pollfds[count].events = POLLIN;
                indexes[count] = -2;
                count++;
            }
        }
        /* Buffered VBus frames need another fair pass even when their socket
         * has no unread bytes. Idle readers still use the ordinary poll wait. */
        poll_result = poll(pollfds, count, dispatched ? 0 : GH_IDLE_POLL_MS);
        if (poll_result < 0 && errno != EINTR) {
            result = -1;
            break;
        }
        if (poll_result > 0 && (pollfds[0].revents & POLLIN)) accept_connections(state);
        for (i = 2; i < (int)count; ++i) {
            gh_conn *conn;
            if (indexes[i] < 0) continue;
            conn = &state->conns[indexes[i]];
            if (!conn->in_use) continue;
            if (pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                conn_close(state, conn, conn->streaming && !conn->terminal);
                continue;
            }
            if (pollfds[i].revents & POLLIN) read_connection(state, conn);
            if (conn->in_use && (pollfds[i].revents & POLLOUT)) write_connection(state, conn);
        }
        if (vbus_poll(state->bus, 0) != 0) {
            result = -1;
            break;
        }
        now = monotonic_ms();
        for (i = 0; i < (int)state->conn_highwater; ++i) {
            gh_conn *conn = &state->conns[i];
            if (!conn->in_use) continue;
            if (conn->deadline_ms != 0 && now >= conn->deadline_ms) {
                publish_cancel(state, conn, conn->streaming ? "turn_deadline" : "request_deadline");
                conn_close(state, conn, 0);
            } else if (conn->tx.size != 0u) {
                write_connection(state, conn);
            }
        }
    }
    for (i = 0; i < (int)state->conn_highwater; ++i)
        if (state->conns[i].in_use) conn_close(state, &state->conns[i], 1);
    close(state->listen_fd);
    vbus_close(state->bus);
    voice_auth_verifier_destroy(&state->auth_verifier);
    voice_analytics_stop(state->analytics);
    memset(state->token, 0, sizeof(state->token));
    destroy_replay_indexes(state);
    free(state);
    return result;
}
