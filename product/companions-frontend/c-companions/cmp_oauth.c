/* cmp_oauth.c — Authentik/OIDC authorization code flow in pure C. */
#define _POSIX_C_SOURCE 200809L

#include "cmp_oauth.h"
#include "cmp_index.h"
#include "cmp_json.h"
#include "product.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CMP_OAUTH_MAX_STATES 128u
#define CMP_OAUTH_STATE_INDEX_CAP 256u
#define CMP_OAUTH_HTTP_BODY_CAP (64u * 1024u)
#define CMP_OAUTH_HTTP_HEADER_CAP (32u * 1024u)
#define CMP_OAUTH_HTTP_REQUEST_CAP (24u * 1024u)
#define CMP_OAUTH_QUERY_CAP 8192u
#define CMP_OAUTH_CODE_CAP (PROD_MAX_OAUTH_CALLBACK_CODE + 1u)

_Static_assert((CMP_OAUTH_STATE_INDEX_CAP & (CMP_OAUTH_STATE_INDEX_CAP - 1u)) == 0u,
               "OAuth state index capacity must be a power of two");
_Static_assert(CMP_OAUTH_STATE_INDEX_CAP >= CMP_OAUTH_MAX_STATES * 2u,
               "OAuth state index load must not exceed 50 percent");

typedef struct {
    char state[CMP_OAUTH_STATE_CAP];
    char verifier[CMP_OAUTH_STATE_CAP];
    int64_t expires_at;
    int used;
} oauth_state_slot;

typedef struct {
    char provider[64];
    char client_id[512];
    char client_secret[512];
    char redirect_url[PROD_OAUTH_URL_CAP];
    char auth_url[PROD_OAUTH_URL_CAP];
    char token_url[PROD_OAUTH_URL_CAP];
    char userinfo_url[PROD_OAUTH_URL_CAP];
    char scopes[PROD_OAUTH_SCOPES_CAP];
    int enabled;
} oauth_config;

typedef struct {
    char state[CMP_OAUTH_STATE_CAP];
    char code[CMP_OAUTH_CODE_CAP];
    char error[128];
    unsigned state_count;
    unsigned code_count;
    unsigned error_count;
} oauth_callback_query;

typedef struct {
    char host[256];
    char path[PROD_OAUTH_URL_CAP];
    char port[8];
} oauth_https_url;

static pthread_mutex_t g_oauth_mu = PTHREAD_MUTEX_INITIALIZER;
static oauth_config g_oauth;
static oauth_state_slot g_states[CMP_OAUTH_MAX_STATES];
static cmp_index_entry g_state_entries[CMP_OAUTH_STATE_INDEX_CAP];
static cmp_index g_state_index;
static int g_state_index_ready;
static cmp_oauth_http_fn g_http_override;
static void *g_http_override_user;

static void oauth_configuration_clear_locked(void) {
    OPENSSL_cleanse(&g_oauth, sizeof(g_oauth));
    OPENSSL_cleanse(g_states, sizeof(g_states));
    if (g_state_index_ready)
        cmp_index_clear(&g_state_index);
    g_state_index_ready = 0;
}

static void set_reason(char *out, size_t cap, const char *reason) {
    if (out && cap)
        (void)snprintf(out, cap, "%s", reason ? reason : "provider_error");
}

static int env_copy(const char *name, char *out, size_t cap, int required) {
    const char *value = getenv(name);
    size_t len;
    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    if (!value || !value[0])
        return required ? -1 : 0;
    len = strlen(value);
    if (len >= cap || value[0] == ' ' || value[0] == '\t' ||
        value[len - 1u] == ' ' || value[len - 1u] == '\t')
        return -1;
    memcpy(out, value, len + 1u);
    return 0;
}

static int b64url_encode(const unsigned char *in, size_t in_len, char *out, size_t cap) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t input = 0;
    size_t output = 0;
    size_t encoded;
    if ((!in && in_len != 0) || !out || cap == 0 || in_len > (SIZE_MAX - 2u) / 4u * 3u)
        return -1;
    encoded = ((in_len + 2u) / 3u) * 4u;
    if (in_len % 3u != 0)
        encoded -= 3u - in_len % 3u;
    if (encoded >= cap)
        return -1;
    while (input < in_len) {
        unsigned value = (unsigned)in[input] << 16;
        if (input + 1u < in_len)
            value |= (unsigned)in[input + 1u] << 8;
        if (input + 2u < in_len)
            value |= in[input + 2u];
        out[output++] = table[(value >> 18) & 63u];
        out[output++] = table[(value >> 12) & 63u];
        if (input + 1u < in_len)
            out[output++] = table[(value >> 6) & 63u];
        if (input + 2u < in_len)
            out[output++] = table[value & 63u];
        input += 3u;
    }
    out[output] = '\0';
    return 0;
}

static int random_token(char out[CMP_OAUTH_STATE_CAP]) {
    unsigned char random_bytes[PROD_OAUTH_STATE_DECODED_LEN];
    int result = -1;
    if (RAND_bytes(random_bytes, (int)sizeof(random_bytes)) == 1 &&
        b64url_encode(random_bytes, sizeof(random_bytes), out, CMP_OAUTH_STATE_CAP) == 0 &&
        prod_oauth_pkce_ok(out))
        result = 0;
    OPENSSL_cleanse(random_bytes, sizeof(random_bytes));
    return result;
}

static int constant_time_equal(const char *left, const char *right) {
    size_t left_len;
    size_t right_len;
    if (!left || !right)
        return 0;
    left_len = strlen(left);
    right_len = strlen(right);
    return left_len == right_len && left_len != 0 &&
           CRYPTO_memcmp(left, right, left_len) == 0;
}

static int64_t monotonic_seconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0)
        return -1;
    return (int64_t)value.tv_sec;
}

static void state_clear_slot(size_t slot) {
    if (slot >= CMP_OAUTH_MAX_STATES || !g_states[slot].used)
        return;
    (void)cmp_index_remove(&g_state_index, g_states[slot].state, NULL);
    OPENSSL_cleanse(&g_states[slot], sizeof(g_states[slot]));
}

static void state_expire_locked(int64_t now) {
    size_t i;
    for (i = 0; i < CMP_OAUTH_MAX_STATES; ++i) {
        if (g_states[i].used && g_states[i].expires_at <= now)
            state_clear_slot(i);
    }
}

static int state_store(const char *state, const char *verifier) {
    int64_t now = monotonic_seconds();
    size_t i;
    int result = -1;
    if (now <= 0 || !state || !verifier)
        return -1;
    pthread_mutex_lock(&g_oauth_mu);
    state_expire_locked(now);
    for (i = 0; i < CMP_OAUTH_MAX_STATES; ++i) {
        if (!g_states[i].used) {
            (void)snprintf(g_states[i].state, sizeof(g_states[i].state), "%s", state);
            (void)snprintf(g_states[i].verifier, sizeof(g_states[i].verifier), "%s", verifier);
            g_states[i].expires_at = now + CMP_OAUTH_STATE_TTL_SEC;
            g_states[i].used = 1;
            if (cmp_index_insert(&g_state_index, g_states[i].state, (uint32_t)i, NULL) ==
                CMP_INDEX_OK) {
                result = 0;
            } else {
                OPENSSL_cleanse(&g_states[i], sizeof(g_states[i]));
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_oauth_mu);
    return result;
}

static int state_consume(const char *state, char *verifier, size_t verifier_cap) {
    uint32_t slot = 0;
    int64_t now = monotonic_seconds();
    int result = -1;
    if (!state || !verifier || verifier_cap == 0 || now <= 0)
        return -1;
    verifier[0] = '\0';
    pthread_mutex_lock(&g_oauth_mu);
    state_expire_locked(now);
    if (cmp_index_find(&g_state_index, state, &slot, NULL) == CMP_INDEX_OK &&
        slot < CMP_OAUTH_MAX_STATES && g_states[slot].used &&
        constant_time_equal(g_states[slot].state, state) &&
        strlen(g_states[slot].verifier) < verifier_cap) {
        memcpy(verifier, g_states[slot].verifier, strlen(g_states[slot].verifier) + 1u);
        state_clear_slot(slot);
        result = 0;
    }
    pthread_mutex_unlock(&g_oauth_mu);
    return result;
}

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static int query_decode(const char *input, size_t input_len, char *out, size_t out_cap) {
    size_t input_pos = 0;
    size_t output_pos = 0;
    if (!input || !out || out_cap == 0)
        return -1;
    while (input_pos < input_len) {
        unsigned char value = (unsigned char)input[input_pos++];
        if (value == '%') {
            int high;
            int low;
            if (input_len - input_pos < 2u)
                return -1;
            high = hex_value((unsigned char)input[input_pos]);
            low = hex_value((unsigned char)input[input_pos + 1u]);
            if (high < 0 || low < 0)
                return -1;
            value = (unsigned char)((unsigned)high * 16u + (unsigned)low);
            input_pos += 2u;
        } else if (value == '+') {
            value = ' ';
        }
        if (value == 0 || value == '\r' || value == '\n' || output_pos + 1u >= out_cap)
            return -1;
        out[output_pos++] = (char)value;
    }
    out[output_pos] = '\0';
    return 0;
}

static int parse_callback_query(const char *request_path, size_t request_path_len,
                                oauth_callback_query *out) {
    const char *query;
    const char *end;
    const char *cursor;
    if (!request_path || !out || request_path_len == 0 || request_path_len >= CMP_OAUTH_QUERY_CAP ||
        memchr(request_path, '\0', request_path_len) != NULL ||
        memchr(request_path, '#', request_path_len) != NULL)
        return -1;
    query = memchr(request_path, '?', request_path_len);
    if (!query || (size_t)(query - request_path) != sizeof("/api/oauth/callback") - 1u ||
        memcmp(request_path, "/api/oauth/callback", sizeof("/api/oauth/callback") - 1u) != 0)
        return -1;
    query++;
    end = request_path + request_path_len;
    if (query == end)
        return -1;
    memset(out, 0, sizeof(*out));
    cursor = query;
    while (cursor < end) {
        const char *amp = memchr(cursor, '&', (size_t)(end - cursor));
        const char *field_end = amp ? amp : end;
        const char *equals = memchr(cursor, '=', (size_t)(field_end - cursor));
        char key[64];
        char value[CMP_OAUTH_CODE_CAP];
        if (!equals || equals == cursor || equals + 1 > field_end ||
            query_decode(cursor, (size_t)(equals - cursor), key, sizeof(key)) != 0 ||
            query_decode(equals + 1, (size_t)(field_end - equals - 1), value,
                         sizeof(value)) != 0)
            return -1;
        if (strcmp(key, "state") == 0) {
            if (++out->state_count != 1u || strlen(value) >= sizeof(out->state))
                return -1;
            memcpy(out->state, value, strlen(value) + 1u);
        } else if (strcmp(key, "code") == 0) {
            if (++out->code_count != 1u)
                return -1;
            memcpy(out->code, value, strlen(value) + 1u);
        } else if (strcmp(key, "error") == 0) {
            if (++out->error_count != 1u || strlen(value) >= sizeof(out->error))
                return -1;
            memcpy(out->error, value, strlen(value) + 1u);
        }
        if (!amp)
            break;
        cursor = amp + 1;
        if (cursor == end)
            return -1;
    }
    return 0;
}

int cmp_oauth_callback_query_valid(const char *request_path, size_t request_path_len) {
    oauth_callback_query query;
    return parse_callback_query(request_path, request_path_len, &query) == 0 ? 1 : 0;
}

static int query_value(const char *request_path, const char *wanted, char *out, size_t cap) {
    const char *query;
    const char *cursor;
    unsigned found = 0;
    if (!request_path || !wanted || !out || cap == 0 || strchr(request_path, '#'))
        return -1;
    out[0] = '\0';
    query = strchr(request_path, '?');
    if (!query)
        return 0;
    cursor = query + 1;
    while (*cursor) {
        const char *amp = strchr(cursor, '&');
        const char *end = amp ? amp : cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        char key[64];
        char value[512];
        if (!equals || query_decode(cursor, (size_t)(equals - cursor), key, sizeof(key)) != 0 ||
            query_decode(equals + 1, (size_t)(end - equals - 1), value, sizeof(value)) != 0)
            return -1;
        if (strcmp(key, wanted) == 0) {
            if (++found != 1u || strlen(value) >= cap)
                return -1;
            memcpy(out, value, strlen(value) + 1u);
        }
        if (!amp)
            break;
        cursor = amp + 1;
        if (!*cursor)
            return -1;
    }
    return found == 1u ? 1 : 0;
}

static int url_encode(const char *input, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t input_pos;
    size_t output_pos = 0;
    if (!input || !out || cap == 0)
        return -1;
    for (input_pos = 0; input[input_pos]; ++input_pos) {
        unsigned char value = (unsigned char)input[input_pos];
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
            value == '~') {
            if (output_pos + 1u >= cap)
                return -1;
            out[output_pos++] = (char)value;
        } else {
            if (output_pos + 3u >= cap)
                return -1;
            out[output_pos++] = '%';
            out[output_pos++] = hex[value >> 4];
            out[output_pos++] = hex[value & 15u];
        }
    }
    out[output_pos] = '\0';
    return 0;
}

static int configured_url_safe(const char *url) {
    const unsigned char *cursor = (const unsigned char *)url;
    if (!cursor || !cursor[0])
        return 0;
    while (*cursor) {
        if (*cursor <= 0x20u || *cursor >= 0x7fu)
            return 0;
        ++cursor;
    }
    return 1;
}

static int bearer_token_safe(const char *token) {
    const unsigned char *cursor = (const unsigned char *)token;
    if (!cursor || !cursor[0])
        return 0;
    while (*cursor) {
        if (*cursor < 0x21u || *cursor > 0x7eu)
            return 0;
        ++cursor;
    }
    return 1;
}

static int parse_https_url(const char *url, oauth_https_url *out) {
    const char *authority;
    const char *path;
    const char *host_start;
    const char *host_end;
    const char *port_start = NULL;
    size_t host_len;
    size_t path_len;
    if (!url || !out || strncmp(url, "https://", 8) != 0)
        return -1;
    memset(out, 0, sizeof(*out));
    authority = url + 8;
    path = strchr(authority, '/');
    if (!path || path == authority)
        return -1;
    host_start = authority;
    host_end = path;
    if (*host_start == '[') {
        const char *right = memchr(host_start, ']', (size_t)(path - host_start));
        if (!right || right == host_start + 1)
            return -1;
        host_start++;
        host_end = right;
        if (right + 1 < path) {
            if (right[1] != ':')
                return -1;
            port_start = right + 2;
        }
    } else {
        const char *colon = memchr(host_start, ':', (size_t)(path - host_start));
        if (colon) {
            host_end = colon;
            port_start = colon + 1;
        }
    }
    host_len = (size_t)(host_end - host_start);
    path_len = strlen(path);
    if (host_len == 0 || host_len >= sizeof(out->host) || path_len >= sizeof(out->path))
        return -1;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';
    memcpy(out->path, path, path_len + 1u);
    if (port_start) {
        size_t port_len = (size_t)(path - port_start);
        size_t i;
        if (port_len == 0 || port_len >= sizeof(out->port))
            return -1;
        for (i = 0; i < port_len; ++i)
            if (!isdigit((unsigned char)port_start[i]))
                return -1;
        memcpy(out->port, port_start, port_len);
        out->port[port_len] = '\0';
    } else {
        memcpy(out->port, "443", sizeof("443"));
    }
    return 0;
}

static int connect_https_host(const oauth_https_url *url) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(url->host, url->port, &hints, &addresses) != 0)
        return -1;
    for (address = addresses; address; address = address->ai_next) {
        struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
        int flags;
        int connected;
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        connected = connect(fd, address->ai_addr, (socklen_t)address->ai_addrlen);
        if (connected != 0 && errno == EINPROGRESS) {
            struct pollfd descriptor = {.fd = fd, .events = POLLOUT, .revents = 0};
            int polled;
            do {
                polled = poll(&descriptor, 1, 15000);
            } while (polled < 0 && errno == EINTR);
            if (polled > 0 && (descriptor.revents & POLLOUT) != 0) {
                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);
                connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 &&
                                    socket_error == 0
                                ? 0
                                : -1;
            } else {
                connected = -1;
            }
        }
        if (connected == 0 && fcntl(fd, F_SETFL, flags) == 0) {
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int parse_decimal(const char *value, size_t value_len, size_t *out) {
    size_t result = 0;
    size_t i = 0;
    while (i < value_len && (value[i] == ' ' || value[i] == '\t'))
        ++i;
    if (i == value_len)
        return -1;
    for (; i < value_len && isdigit((unsigned char)value[i]); ++i) {
        unsigned digit = (unsigned)(value[i] - '0');
        if (result > (SIZE_MAX - digit) / 10u)
            return -1;
        result = result * 10u + digit;
    }
    while (i < value_len && (value[i] == ' ' || value[i] == '\t'))
        ++i;
    if (i != value_len)
        return -1;
    *out = result;
    return 0;
}

static int chunked_decode(const uint8_t *input, size_t input_len, uint8_t *out, size_t out_cap,
                          size_t *out_len) {
    size_t input_pos = 0;
    size_t output_pos = 0;
    while (input_pos < input_len) {
        const uint8_t *line_end = NULL;
        size_t line_len;
        size_t chunk_size = 0;
        size_t i;
        for (i = input_pos; i + 1u < input_len; ++i) {
            if (input[i] == '\r' && input[i + 1u] == '\n') {
                line_end = input + i;
                break;
            }
        }
        if (!line_end)
            return -1;
        line_len = (size_t)(line_end - (input + input_pos));
        if (line_len == 0)
            return -1;
        for (i = 0; i < line_len && input[input_pos + i] != ';'; ++i) {
            int digit = hex_value(input[input_pos + i]);
            if (digit < 0 || chunk_size > (SIZE_MAX - (unsigned)digit) / 16u)
                return -1;
            chunk_size = chunk_size * 16u + (unsigned)digit;
        }
        input_pos += line_len + 2u;
        if (chunk_size == 0) {
            if (input_len - input_pos == 2u && input[input_pos] == '\r' &&
                input[input_pos + 1u] == '\n') {
                *out_len = output_pos;
                return 0;
            }
            return -1;
        }
        if (chunk_size > input_len - input_pos || chunk_size > out_cap - output_pos ||
            input_len - input_pos - chunk_size < 2u || input[input_pos + chunk_size] != '\r' ||
            input[input_pos + chunk_size + 1u] != '\n')
            return -1;
        memcpy(out + output_pos, input + input_pos, chunk_size);
        output_pos += chunk_size;
        input_pos += chunk_size + 2u;
    }
    return -1;
}

static int decode_http_response(const uint8_t *raw, size_t raw_len, uint8_t *out, size_t out_cap,
                                size_t *out_len) {
    const uint8_t *header_end = NULL;
    const uint8_t *cursor;
    const uint8_t *end = raw + raw_len;
    size_t header_len;
    size_t content_length = 0;
    unsigned content_length_count = 0;
    unsigned transfer_encoding_count = 0;
    int chunked = 0;
    size_t i;
    if (!raw || !out || !out_len)
        return -1;
    for (i = 0; i + 3u < raw_len && i < CMP_OAUTH_HTTP_HEADER_CAP; ++i) {
        if (raw[i] == '\r' && raw[i + 1u] == '\n' && raw[i + 2u] == '\r' &&
            raw[i + 3u] == '\n') {
            header_end = raw + i + 4u;
            break;
        }
    }
    if (!header_end)
        return -1;
    header_len = (size_t)(header_end - raw);
    if (header_len < 16u || memcmp(raw, "HTTP/1.", 7u) != 0 ||
        (raw[7] != '0' && raw[7] != '1') || raw[8] != ' ' || raw[9] != '2' ||
        raw[10] != '0' || raw[11] != '0' || (raw[12] != ' ' && raw[12] != '\r'))
        return -1;
    cursor = memchr(raw, '\n', header_len);
    if (!cursor || cursor == raw || cursor[-1] != '\r')
        return -1;
    cursor++;
    while (cursor < header_end - 2u) {
        const uint8_t *line_end = NULL;
        const uint8_t *colon;
        const uint8_t *value;
        size_t name_len;
        size_t value_len;
        for (i = 0; cursor + i + 1u < header_end; ++i) {
            if (cursor[i] == '\r' && cursor[i + 1u] == '\n') {
                line_end = cursor + i;
                break;
            }
        }
        if (!line_end || line_end == cursor || cursor[0] == ' ' || cursor[0] == '\t')
            return -1;
        colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (!colon || colon == cursor)
            return -1;
        name_len = (size_t)(colon - cursor);
        value = colon + 1;
        while (value < line_end && (*value == ' ' || *value == '\t'))
            value++;
        value_len = (size_t)(line_end - value);
        while (value_len > 0 && (value[value_len - 1u] == ' ' || value[value_len - 1u] == '\t'))
            value_len--;
        if (name_len == sizeof("Content-Length") - 1u &&
            strncasecmp((const char *)cursor, "Content-Length", name_len) == 0) {
            if (++content_length_count != 1u ||
                parse_decimal((const char *)value, value_len, &content_length) != 0)
                return -1;
        } else if (name_len == sizeof("Transfer-Encoding") - 1u &&
                   strncasecmp((const char *)cursor, "Transfer-Encoding", name_len) == 0) {
            if (++transfer_encoding_count != 1u || value_len != sizeof("chunked") - 1u ||
                strncasecmp((const char *)value, "chunked", value_len) != 0)
                return -1;
            chunked = 1;
        }
        cursor = line_end + 2u;
    }
    if (content_length_count && transfer_encoding_count)
        return -1;
    if (chunked)
        return chunked_decode(header_end, (size_t)(end - header_end), out, out_cap, out_len);
    if (content_length_count) {
        if (content_length != (size_t)(end - header_end) || content_length > out_cap)
            return -1;
        memcpy(out, header_end, content_length);
        *out_len = content_length;
        return 0;
    }
    if ((size_t)(end - header_end) > out_cap)
        return -1;
    memcpy(out, header_end, (size_t)(end - header_end));
    *out_len = (size_t)(end - header_end);
    return 0;
}

static int session_https_request(const char *method, const char *url, const char *content_type,
                              const char *authorization, const uint8_t *body, size_t body_len,
                              uint8_t *out, size_t out_cap, size_t *out_len, const char *cookie,
                              const char *identity_token) {
    oauth_https_url parsed;
    SSL_CTX *context = NULL;
    SSL *ssl = NULL;
    int fd = -1;
    char *request = NULL;
    uint8_t *raw = NULL;
    size_t request_len;
    size_t sent = 0;
    size_t raw_len = 0;
    size_t raw_cap;
    int written;
    int result = -1;
    const char *ca_file = getenv("COMPANIONS_OAUTH_CA_FILE");
    if (!method || !url || !out || !out_len || out_cap > ((cookie || identity_token) ? 1024u*1024u : CMP_OAUTH_HTTP_BODY_CAP) ||
        (body_len != 0 && !body) || parse_https_url(url, &parsed) != 0)
        return -1;
    request = malloc(CMP_OAUTH_HTTP_REQUEST_CAP);
    raw_cap = out_cap + CMP_OAUTH_HTTP_HEADER_CAP + 1u;
    raw = malloc(raw_cap);
    if (!request || !raw)
        goto done;
    written = snprintf(request, CMP_OAUTH_HTTP_REQUEST_CAP,
                       "%s %s HTTP/1.1\r\nHost: %s\r\nAccept: application/json\r\n"
                       "Connection: close\r\nUser-Agent: c-companions/1\r\n",
                       method, parsed.path, parsed.host);
    if (written <= 0 || (size_t)written >= CMP_OAUTH_HTTP_REQUEST_CAP)
        goto done;
    request_len = (size_t)written;
    if (cookie) {
        if (!cookie[0] || strlen(cookie)>8192) goto done;
        for (const unsigned char *p=(const unsigned char *)cookie;*p;p++)
            if (*p<32 || *p>=127) goto done;
        written=snprintf(request+request_len,CMP_OAUTH_HTTP_REQUEST_CAP-request_len,"Cookie: %s\r\n",cookie);
        if (written<=0 || (size_t)written>=CMP_OAUTH_HTTP_REQUEST_CAP-request_len) goto done;
        request_len+=(size_t)written;
    }
    if (identity_token) {
        size_t token_len=strlen(identity_token),segment=0;unsigned dots=0;
        if (!token_len || token_len>16384 || cookie) goto done;
        for (const unsigned char *p=(const unsigned char *)identity_token;*p;p++) {
            if (*p=='.') { if (!segment || ++dots>2) goto done;segment=0; }
            else if ((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
                     (*p>='0' && *p<='9') || *p=='_' || *p=='-') segment++;
            else goto done;
        }
        if (dots!=2 || !segment) goto done;
        written=snprintf(request+request_len,CMP_OAUTH_HTTP_REQUEST_CAP-request_len,
                         "X-Envoy-Oidc-Id-Token: %s\r\n",identity_token);
        if (written<=0 || (size_t)written>=CMP_OAUTH_HTTP_REQUEST_CAP-request_len) goto done;
        request_len+=(size_t)written;
    }
    if (authorization && authorization[0]) {
        written = snprintf(request + request_len, CMP_OAUTH_HTTP_REQUEST_CAP - request_len,
                           "Authorization: %s\r\n", authorization);
        if (written <= 0 || (size_t)written >= CMP_OAUTH_HTTP_REQUEST_CAP - request_len)
            goto done;
        request_len += (size_t)written;
    }
    if (body_len != 0) {
        written = snprintf(request + request_len, CMP_OAUTH_HTTP_REQUEST_CAP - request_len,
                           "Content-Type: %s\r\nContent-Length: %zu\r\n",
                           content_type ? content_type : "application/octet-stream", body_len);
        if (written <= 0 || (size_t)written >= CMP_OAUTH_HTTP_REQUEST_CAP - request_len)
            goto done;
        request_len += (size_t)written;
    }
    if (request_len + 2u + body_len > CMP_OAUTH_HTTP_REQUEST_CAP)
        goto done;
    memcpy(request + request_len, "\r\n", 2u);
    request_len += 2u;
    if (body_len) {
        memcpy(request + request_len, body, body_len);
        request_len += body_len;
    }
    context = SSL_CTX_new(TLS_client_method());
    if (!context || SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1)
        goto done;
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
    (void)SSL_CTX_set_options(context, SSL_OP_IGNORE_UNEXPECTED_EOF);
    if ((ca_file && ca_file[0] && SSL_CTX_load_verify_locations(context, ca_file, NULL) != 1) ||
        ((!ca_file || !ca_file[0]) && SSL_CTX_set_default_verify_paths(context) != 1))
        goto done;
    fd = connect_https_host(&parsed);
    if (fd < 0)
        goto done;
    ssl = SSL_new(context);
    if (!ssl || SSL_set_fd(ssl, fd) != 1 || SSL_set_tlsext_host_name(ssl, parsed.host) != 1 ||
        SSL_set1_host(ssl, parsed.host) != 1 || SSL_connect(ssl) != 1 ||
        SSL_get_verify_result(ssl) != X509_V_OK)
        goto done;
    while (sent < request_len) {
        size_t chunk = request_len - sent;
        int count;
        if (chunk > (size_t)INT_MAX)
            chunk = (size_t)INT_MAX;
        count = SSL_write(ssl, request + sent, (int)chunk);
        if (count <= 0)
            goto done;
        sent += (size_t)count;
    }
    for (;;) {
        int count;
        if (raw_len == raw_cap - 1u)
            goto done;
        count = SSL_read(ssl, raw + raw_len, (int)(raw_cap - 1u - raw_len));
        if (count > 0) {
            raw_len += (size_t)count;
            continue;
        }
        if (SSL_get_error(ssl, count) == SSL_ERROR_ZERO_RETURN)
            break;
        goto done;
    }
    if (decode_http_response(raw, raw_len, out, out_cap, out_len) != 0)
        goto done;
    result = 0;
done:
    if (ssl) {
        (void)SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (fd >= 0)
        close(fd);
    SSL_CTX_free(context);
    if (request)
        OPENSSL_clear_free(request, CMP_OAUTH_HTTP_REQUEST_CAP);
    if (raw)
        OPENSSL_clear_free(raw, raw_cap);
    ERR_clear_error();
    return result;
}

static int real_https_request(const char *method, const char *url, const char *content_type,
                              const char *authorization, const uint8_t *body, size_t body_len,
                              uint8_t *out, size_t cap, size_t *length, void *user) {
    (void)user;
    return session_https_request(method,url,content_type,authorization,body,body_len,out,cap,length,NULL,NULL);
}

int cmp_oauth_session_get(const char *url,const char *cookie,uint8_t *out,size_t cap,size_t *length) {
    if (!cookie || !cookie[0]) return -1;
    return session_https_request("GET",url,NULL,NULL,NULL,0,out,cap,length,cookie,NULL);
}

int cmp_oauth_identity_get(const char *url,const char *token,uint8_t *out,size_t cap,size_t *length) {
    if (!token || !token[0]) return -1;
    return session_https_request("GET",url,NULL,NULL,NULL,0,out,cap,length,NULL,token);
}

static int oauth_http(const char *method, const char *url, const char *content_type,
                      const char *authorization, const uint8_t *body, size_t body_len,
                      uint8_t *out, size_t out_cap, size_t *out_len) {
    cmp_oauth_http_fn function;
    void *user;
    pthread_mutex_lock(&g_oauth_mu);
    function = g_http_override ? g_http_override : real_https_request;
    user = g_http_override ? g_http_override_user : NULL;
    pthread_mutex_unlock(&g_oauth_mu);
    return function(method, url, content_type, authorization, body, body_len, out, out_cap,
                    out_len, user);
}

int cmp_oauth_public_keys_get(const char *url, uint8_t *out, size_t cap, size_t *length) {
    if (!url || strncmp(url,"https://",8)) return -1;
    return oauth_http("GET",url,NULL,NULL,NULL,0,out,cap,length);
}

void cmp_oauth_set_http_for_tests(cmp_oauth_http_fn function, void *user) {
    pthread_mutex_lock(&g_oauth_mu);
    g_http_override = function;
    g_http_override_user = function ? user : NULL;
    pthread_mutex_unlock(&g_oauth_mu);
}

int cmp_oauth_init(void) {
    oauth_config config;
    char table_auth[PROD_OAUTH_URL_CAP];
    char table_token[PROD_OAUTH_URL_CAP];
    char table_userinfo[PROD_OAUTH_URL_CAP];
    char scopes_csv[PROD_OAUTH_SCOPES_CAP];
    const char *client_id = getenv("OAUTH_CLIENT_ID");
    const char *client_secret = getenv("OAUTH_CLIENT_SECRET");
    size_t i;
    memset(&config, 0, sizeof(config));
    if ((!client_id || !client_id[0]) && (!client_secret || !client_secret[0])) {
        pthread_mutex_lock(&g_oauth_mu);
        oauth_configuration_clear_locked();
        pthread_mutex_unlock(&g_oauth_mu);
        return CMP_OAUTH_OK;
    }
    if (env_copy("OAUTH_CLIENT_ID", config.client_id, sizeof(config.client_id), 1) != 0 ||
        env_copy("OAUTH_CLIENT_SECRET", config.client_secret, sizeof(config.client_secret), 1) !=
            0 ||
        env_copy("OAUTH_PROVIDER", config.provider, sizeof(config.provider), 1) != 0 ||
        env_copy("OAUTH_REDIRECT_URL", config.redirect_url, sizeof(config.redirect_url), 1) != 0 ||
        !prod_oauth_provider_ok(config.provider) || !configured_url_safe(config.redirect_url) ||
        prod_oauth_provider_config(config.provider, table_auth, sizeof(table_auth), table_token,
                                   sizeof(table_token), table_userinfo, sizeof(table_userinfo),
                                   scopes_csv, sizeof(scopes_csv)) != PROD_OK ||
        prod_oauth_endpoint_url(config.redirect_url, 1, config.redirect_url,
                                sizeof(config.redirect_url)) != PROD_OK)
        goto invalid;
    if (strcmp(config.provider, "authentik") == 0 || strcmp(config.provider, "oidc") == 0) {
        if (env_copy("OAUTH_AUTH_URL", config.auth_url, sizeof(config.auth_url), 1) != 0 ||
            env_copy("OAUTH_TOKEN_URL", config.token_url, sizeof(config.token_url), 1) != 0 ||
            env_copy("OAUTH_USERINFO_URL", config.userinfo_url, sizeof(config.userinfo_url), 1) !=
                0)
            goto invalid;
    } else {
        (void)snprintf(config.auth_url, sizeof(config.auth_url), "%s", table_auth);
        (void)snprintf(config.token_url, sizeof(config.token_url), "%s", table_token);
        (void)snprintf(config.userinfo_url, sizeof(config.userinfo_url), "%s", table_userinfo);
    }
    if (!configured_url_safe(config.auth_url) || !configured_url_safe(config.token_url) ||
        !configured_url_safe(config.userinfo_url) ||
        prod_oauth_endpoint_url(config.auth_url, 0, config.auth_url, sizeof(config.auth_url)) !=
            PROD_OK ||
        prod_oauth_endpoint_url(config.token_url, 0, config.token_url, sizeof(config.token_url)) !=
            PROD_OK ||
        prod_oauth_endpoint_url(config.userinfo_url, 0, config.userinfo_url,
                                sizeof(config.userinfo_url)) != PROD_OK ||
        strncmp(config.token_url, "https://", 8) != 0 ||
        strncmp(config.userinfo_url, "https://", 8) != 0)
        goto invalid;
    for (i = 0; scopes_csv[i]; ++i)
        config.scopes[i] = scopes_csv[i] == ',' ? ' ' : scopes_csv[i];
    config.scopes[i] = '\0';
    config.enabled = 1;
    pthread_mutex_lock(&g_oauth_mu);
    oauth_configuration_clear_locked();
    g_oauth = config;
    if (cmp_index_init(&g_state_index, g_state_entries, CMP_OAUTH_STATE_INDEX_CAP) != CMP_INDEX_OK) {
        OPENSSL_cleanse(&g_oauth, sizeof(g_oauth));
        pthread_mutex_unlock(&g_oauth_mu);
        OPENSSL_cleanse(&config, sizeof(config));
        return CMP_OAUTH_ERR;
    }
    g_state_index_ready = 1;
    pthread_mutex_unlock(&g_oauth_mu);
    OPENSSL_cleanse(&config, sizeof(config));
    return CMP_OAUTH_OK;
invalid:
    OPENSSL_cleanse(&config, sizeof(config));
    pthread_mutex_lock(&g_oauth_mu);
    oauth_configuration_clear_locked();
    pthread_mutex_unlock(&g_oauth_mu);
    return CMP_OAUTH_ERR;
}

void cmp_oauth_cleanup(void) {
    pthread_mutex_lock(&g_oauth_mu);
    oauth_configuration_clear_locked();
    g_http_override = NULL;
    g_http_override_user = NULL;
    pthread_mutex_unlock(&g_oauth_mu);
}

int cmp_oauth_enabled(void) {
    int enabled;
    pthread_mutex_lock(&g_oauth_mu);
    enabled = g_oauth.enabled;
    pthread_mutex_unlock(&g_oauth_mu);
    return enabled;
}

int cmp_oauth_begin(const char *request_path, char *state, size_t state_cap, char *location,
                    size_t location_cap, char *reason, size_t reason_cap) {
    oauth_config config;
    char requested_provider[64];
    char verifier[CMP_OAUTH_STATE_CAP];
    char challenge[CMP_OAUTH_STATE_CAP];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char client_id[1536];
    char redirect[1536];
    char scopes[768];
    int provider_result;
    int written;
    if (!request_path || !state || state_cap < CMP_OAUTH_STATE_CAP || !location ||
        location_cap == 0)
        return CMP_OAUTH_ERR;
    state[0] = location[0] = '\0';
    set_reason(reason, reason_cap, "provider_error");
    pthread_mutex_lock(&g_oauth_mu);
    config = g_oauth;
    pthread_mutex_unlock(&g_oauth_mu);
    if (!config.enabled || strncmp(request_path, "/api/oauth/login", 16u) != 0)
        goto failure;
    provider_result = query_value(request_path, "provider", requested_provider,
                                  sizeof(requested_provider));
    if (provider_result < 0 ||
        (provider_result == 1 && strcmp(requested_provider, config.provider) != 0))
        goto failure;
    if (random_token(state) != 0 || random_token(verifier) != 0 ||
        !SHA256((const unsigned char *)verifier, strlen(verifier), digest) ||
        b64url_encode(digest, sizeof(digest), challenge, sizeof(challenge)) != 0 ||
        url_encode(config.client_id, client_id, sizeof(client_id)) != 0 ||
        url_encode(config.redirect_url, redirect, sizeof(redirect)) != 0 ||
        url_encode(config.scopes, scopes, sizeof(scopes)) != 0 || state_store(state, verifier) != 0)
        goto failure;
    written = snprintf(location, location_cap,
                       "%s?response_type=code&client_id=%s&redirect_uri=%s&scope=%s&state=%s&"
                       "code_challenge=%s&code_challenge_method=S256",
                       config.auth_url, client_id, redirect, scopes, state, challenge);
    OPENSSL_cleanse(verifier, sizeof(verifier));
    OPENSSL_cleanse(&config, sizeof(config));
    if (written <= 0 || (size_t)written >= location_cap) {
        char discarded[CMP_OAUTH_STATE_CAP];
        (void)state_consume(state, discarded, sizeof(discarded));
        OPENSSL_cleanse(discarded, sizeof(discarded));
        state[0] = location[0] = '\0';
        return CMP_OAUTH_ERR;
    }
    if (reason && reason_cap)
        reason[0] = '\0';
    return CMP_OAUTH_OK;
failure:
    OPENSSL_cleanse(verifier, sizeof(verifier));
    OPENSSL_cleanse(&config, sizeof(config));
    state[0] = location[0] = '\0';
    return CMP_OAUTH_ERR;
}

static int form_append(char *form, size_t cap, size_t *used, const char *name, const char *value) {
    char encoded[6144];
    int written;
    if (!form || !used || !name || !value || url_encode(value, encoded, sizeof(encoded)) != 0)
        return -1;
    written = snprintf(form + *used, cap - *used, "%s%s=%s", *used ? "&" : "", name, encoded);
    OPENSSL_cleanse(encoded, sizeof(encoded));
    if (written <= 0 || (size_t)written >= cap - *used)
        return -1;
    *used += (size_t)written;
    return 0;
}

static int userinfo_shape_valid(const char *json) {
    static const char *const fields[] = {
        "id", "login", "sub", "preferred_username", "email", "email_verified", "name"
    };
    cmp_json_object object;
    size_t i;
    if (!cmp_json_object_parse(json, &object))
        return 0;
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        int count = cmp_json_object_key_count(&object, fields[i]);
        if (count < 0 || count > 1)
            return 0;
    }
    return 1;
}

int cmp_oauth_finish(const char *request_path, const char *state_cookie,
                     cmp_oauth_identity *identity, char *reason, size_t reason_cap) {
    oauth_callback_query query;
    oauth_config config;
    char verifier[CMP_OAUTH_STATE_CAP];
    char form[16384];
    size_t form_len = 0;
    uint8_t response[CMP_OAUTH_HTTP_BODY_CAP + 1u];
    size_t response_len = 0;
    char access_token[8192];
    char token_type[32];
    char authorization[8256];
    char parse_error[PROD_OAUTH_USERINFO_ERR_CAP];
    cmp_json_object token_object;
    int token_type_count;
    int written;
    if (identity)
        memset(identity, 0, sizeof(*identity));
    verifier[0] = '\0';
    access_token[0] = token_type[0] = '\0';
    set_reason(reason, reason_cap, "provider_error");
    if (!request_path || !state_cookie || !identity ||
        parse_callback_query(request_path, strlen(request_path), &query) != 0)
        return CMP_OAUTH_ERR;
    if (query.error_count == 1u && query.error[0]) {
        char normalized[CMP_OAUTH_REASON_CAP];
        (void)prod_oauth_provider_error_norm(query.error, normalized, sizeof(normalized));
        set_reason(reason, reason_cap, normalized);
        return CMP_OAUTH_ERR;
    }
    if (query.state_count != 1u || !constant_time_equal(query.state, state_cookie)) {
        set_reason(reason, reason_cap, "state_mismatch");
        return CMP_OAUTH_ERR;
    }
    if (state_consume(query.state, verifier, sizeof(verifier)) != 0) {
        set_reason(reason, reason_cap, "invalid_state");
        return CMP_OAUTH_ERR;
    }
    if (query.code_count != 1u || !prod_oauth_callback_code_ok(query.code)) {
        set_reason(reason, reason_cap, "no_code");
        goto failure;
    }
    pthread_mutex_lock(&g_oauth_mu);
    config = g_oauth;
    pthread_mutex_unlock(&g_oauth_mu);
    if (!config.enabled || form_append(form, sizeof(form), &form_len, "grant_type",
                                       "authorization_code") != 0 ||
        form_append(form, sizeof(form), &form_len, "client_id", config.client_id) != 0 ||
        form_append(form, sizeof(form), &form_len, "client_secret", config.client_secret) != 0 ||
        form_append(form, sizeof(form), &form_len, "code", query.code) != 0 ||
        form_append(form, sizeof(form), &form_len, "redirect_uri", config.redirect_url) != 0 ||
        form_append(form, sizeof(form), &form_len, "code_verifier", verifier) != 0 ||
        oauth_http("POST", config.token_url, "application/x-www-form-urlencoded", NULL,
                   (const uint8_t *)form, form_len, response, CMP_OAUTH_HTTP_BODY_CAP,
                   &response_len) != 0 || response_len == 0 ||
        response_len > CMP_OAUTH_HTTP_BODY_CAP)
        goto token_failure;
    response[response_len] = '\0';
    if (!cmp_json_object_parse((const char *)response, &token_object) ||
        cmp_json_object_key_count(&token_object, "access_token") != 1 ||
        !cmp_json_object_str(&token_object, "access_token", access_token, sizeof(access_token)))
        goto token_failure;
    token_type_count = cmp_json_object_key_count(&token_object, "token_type");
    if (token_type_count < 0 || token_type_count > 1 ||
        (token_type_count == 1 &&
         (!cmp_json_object_str(&token_object, "token_type", token_type, sizeof(token_type)) ||
          strcasecmp(token_type, "Bearer") != 0)))
        goto token_failure;
    if (!bearer_token_safe(access_token))
        goto token_failure;
    written = snprintf(authorization, sizeof(authorization), "Bearer %s", access_token);
    if (written <= 0 || (size_t)written >= sizeof(authorization))
        goto token_failure;
    OPENSSL_cleanse(response, sizeof(response));
    response_len = 0;
    if (oauth_http("GET", config.userinfo_url, NULL, authorization, NULL, 0, response,
                   CMP_OAUTH_HTTP_BODY_CAP, &response_len) != 0 || response_len == 0 ||
        response_len > CMP_OAUTH_HTTP_BODY_CAP)
        goto userinfo_failure;
    response[response_len] = '\0';
    if (!userinfo_shape_valid((const char *)response) ||
        prod_oauth_parse_userinfo(config.provider, (const char *)response, identity->subject,
                                  sizeof(identity->subject), identity->username,
                                  sizeof(identity->username), identity->email,
                                  sizeof(identity->email), identity->name, sizeof(identity->name),
                                  parse_error, sizeof(parse_error)) != PROD_OK)
        goto userinfo_failure;
    (void)snprintf(identity->provider, sizeof(identity->provider), "%s", config.provider);
    if (reason && reason_cap)
        reason[0] = '\0';
    OPENSSL_cleanse(verifier, sizeof(verifier));
    OPENSSL_cleanse(form, sizeof(form));
    OPENSSL_cleanse(response, sizeof(response));
    OPENSSL_cleanse(access_token, sizeof(access_token));
    OPENSSL_cleanse(authorization, sizeof(authorization));
    OPENSSL_cleanse(&config, sizeof(config));
    return CMP_OAUTH_OK;
userinfo_failure:
    set_reason(reason, reason_cap, "userinfo_failed");
    goto failure_with_config;
token_failure:
    set_reason(reason, reason_cap, "token_exchange_failed");
failure_with_config:
    OPENSSL_cleanse(&config, sizeof(config));
failure:
    OPENSSL_cleanse(verifier, sizeof(verifier));
    OPENSSL_cleanse(form, sizeof(form));
    OPENSSL_cleanse(response, sizeof(response));
    OPENSSL_cleanse(access_token, sizeof(access_token));
    OPENSSL_cleanse(authorization, sizeof(authorization));
    memset(identity, 0, sizeof(*identity));
    return CMP_OAUTH_ERR;
}
