#include "pt_http.h"
#include "pt_manager.h"
#include "pt_artifact.h"
#include "pt_dnd_state.h"
#include "cmp_http_headers.h"
#include "cmp_json.h"
#include "dice.h"
#include "toolstore.h"
#include "voice_auth.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PT_HTTP_HEAD_CAP 8192u
#define PT_HTTP_BODY_CAP 16384u
#define PT_HTTP_TIMEOUT_MS 2000
#define PT_HTTP_AUTH_SKEW 30
#define PT_HTTP_NONCES 4096u

typedef struct {
    voice_auth_verifier verifier;
    voice_auth_nonce_cache nonces;
    const char *secret;
} pt_http_auth;

typedef struct {
    char user[128];
    char nonce[33];
    int64_t timestamp;
} pt_http_identity;

static int64_t mono_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_socket(int fd, short events, int64_t deadline) {
    struct pollfd socket_poll = {fd, events, 0};
    int64_t left = deadline - mono_ms();
    int result;
    if (left <= 0 || left > PT_HTTP_TIMEOUT_MS) return -1;
    do {
        result = poll(&socket_poll, 1u, (int)left);
    } while (result < 0 && errno == EINTR && (left = deadline - mono_ms()) > 0);
    return result > 0 && (socket_poll.revents & events) ? 0 : -1;
}

static int write_all(int fd, const char *data, size_t length, int64_t deadline) {
    size_t used = 0;
    while (used < length) {
        ssize_t n;
        if (wait_socket(fd, POLLOUT, deadline) != 0) return -1;
        n = send(fd, data + used, length - used, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}

static void http_reply(int fd, int code, const char *body) {
    char header[256];
    size_t length = strlen(body);
    int64_t deadline = mono_ms() + PT_HTTP_TIMEOUT_MS;
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d Response\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", code, length);
    if (n > 0 && (size_t)n < sizeof(header) &&
        write_all(fd, header, (size_t)n, deadline) == 0)
        (void)write_all(fd, body, length, deadline);
}

static int decimal(const char *text, uint64_t maximum, uint64_t *value) {
    uint64_t number = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (!p || !*p || (p[0] == '0' && p[1])) return -1;
    while (*p) {
        uint64_t digit;
        if (*p < '0' || *p > '9') return -1;
        digit = (uint64_t)(*p++ - '0');
        if (digit > maximum || number > (maximum - digit) / 10u) return -1;
        number = number * 10u + digit;
    }
    *value = number;
    return 0;
}

static int read_request(int fd, char *buffer, size_t capacity,
                        cmp_http_headers *headers, char *method, char *path,
                        char **body, size_t *body_length) {
    size_t used = 0, head_length = 0, wanted = 0;
    int64_t deadline = mono_ms() + PT_HTTP_TIMEOUT_MS;
    for (;;) {
        ssize_t n;
        if (used == capacity - 1u || wait_socket(fd, POLLIN, deadline) != 0) return -1;
        n = recv(fd, buffer + used, capacity - 1u - used, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
        buffer[used] = '\0';
        if (!head_length) {
            char length[32], canonical[600];
            uint64_t length_value = 0;
            int consumed = 0, canonical_length, found;
            head_length = cmp_http_head_length(buffer, used, 0u);
            if (!head_length) {
                if (used >= PT_HTTP_HEAD_CAP) return -1;
                continue;
            }
            if (head_length > PT_HTTP_HEAD_CAP ||
                cmp_http_headers_parse(buffer, head_length, headers) != CMP_HTTP_HEADERS_OK ||
                cmp_http_headers_count(headers, "Transfer-Encoding") != 0u ||
                cmp_http_headers_count(headers, "Host") != 1u ||
                sscanf(buffer, "%15s %511s HTTP/1.1%n", method, path, &consumed) != 2 ||
                consumed <= 0 || (size_t)consumed + 2u > head_length ||
                memcmp(buffer + consumed, "\r\n", 2u) != 0) return -1;
            canonical_length = snprintf(canonical, sizeof(canonical), "%s %s HTTP/1.1", method, path);
            if (canonical_length != consumed || memcmp(buffer, canonical, (size_t)consumed) != 0)
                return -1;
            found = cmp_http_headers_get_unique(headers, "Content-Length", length, sizeof(length));
            if (found < 0 || (strcmp(method, "POST") == 0 && found != CMP_HTTP_HEADER_FOUND) ||
                (found == CMP_HTTP_HEADER_FOUND &&
                 decimal(length, PT_HTTP_BODY_CAP, &length_value) != 0)) return -1;
            wanted = head_length + (size_t)length_value;
            *body = buffer + head_length;
            *body_length = (size_t)length_value;
        }
        if (used > wanted) return -1;
        if (used == wanted) return memchr(*body, '\0', *body_length) ? -1 : 0;
    }
}

static int authenticate(pt_http_auth *auth, const cmp_http_headers *headers,
                        const char *method, const char *path, const char *body,
                        size_t body_length, pt_http_identity *identity) {
    char timestamp[32], signature[65];
    char *user = identity->user;
    char *nonce = identity->nonce;
    uint64_t seconds;
    int64_t now = (int64_t)time(NULL);
    if (now <= PT_HTTP_AUTH_SKEW ||
        cmp_http_headers_get_unique(headers, "X-Tool-User", user, 128u) != CMP_HTTP_HEADER_FOUND ||
        !ts_valid_id(user) ||
        cmp_http_headers_get_unique(headers, "X-Tool-Timestamp", timestamp, sizeof(timestamp)) != CMP_HTTP_HEADER_FOUND ||
        decimal(timestamp, INT64_MAX, &seconds) != 0 ||
        cmp_http_headers_get_unique(headers, "X-Tool-Nonce", nonce, sizeof(identity->nonce)) != CMP_HTTP_HEADER_FOUND ||
        cmp_http_headers_get_unique(headers, "X-Tool-Signature", signature, sizeof(signature)) != CMP_HTTP_HEADER_FOUND ||
        (int64_t)seconds < now - PT_HTTP_AUTH_SKEW ||
        (int64_t)seconds > now + PT_HTTP_AUTH_SKEW ||
        voice_auth_verifier_verify(&auth->verifier, method, path, user, (int64_t)seconds,
                                   nonce, (const uint8_t *)body, body_length, signature) != VOICE_AUTH_OK ||
        voice_auth_nonce_index_accept(&auth->nonces, nonce, now, 2 * PT_HTTP_AUTH_SKEW + 1) != VOICE_AUTH_OK)
        return -1;
    identity->timestamp = (int64_t)seconds;
    return 0;
}

static int parse_tool_request(const char *json, const char *user, pt_start_req *request) {
    static const char *const names[] = {
        "tool_call_id", "idempotency_key", "session_id", "parent_turn_id", "agent_id",
        "tool_id", "user_id", "input_json", "deadline_unix_ms"
    };
    cmp_json_object object, input;
    char expression[DICE_EXPR], error[DICE_MSG];
    dice_spec spec;
    int64_t now = pt_now_ms();
    size_t i, j;
    if (!cmp_json_object_parse(json, &object) || now <= 0) return -1;
    memset(request, 0, sizeof(*request));
    for (i = 0; i < object.field_count; ++i) {
        const cmp_json_field *field = &object.fields[i];
        int known = 0;
        if (field->key_escaped) return -1;
        for (j = 0; j < sizeof(names) / sizeof(names[0]); ++j)
            if (field->key_len == strlen(names[j]) &&
                memcmp(field->key, names[j], field->key_len) == 0) known = 1;
        if (!known) return -1;
    }
#define READ_ID(name) \
    if (!cmp_json_object_str(&object, #name, request->name, sizeof(request->name)) || \
        !ts_valid_id(request->name)) return -1
    READ_ID(tool_call_id);
    READ_ID(idempotency_key);
    READ_ID(session_id);
    READ_ID(parent_turn_id);
    READ_ID(agent_id);
    READ_ID(tool_id);
#undef READ_ID
    if (strcmp(request->agent_id, "dnd-agent") != 0 ||
        (strcmp(request->tool_id, "dnd-dice-roll") != 0 && !pt_dnd_state_tool(request->tool_id))) return -1;
    if (cmp_json_object_key_count(&object, "user_id") != 0 &&
        (!cmp_json_object_str(&object, "user_id", request->user_id, sizeof(request->user_id)) ||
         strcmp(request->user_id, user) != 0)) return -1;
    memcpy(request->user_id, user, strlen(user) + 1u);
    if (!cmp_json_object_str(&object, "input_json", request->input_json, sizeof(request->input_json))) return -1;
    if (pt_dnd_state_tool(request->tool_id)) {
        if (!pt_dnd_command_valid(request->tool_id, request->input_json)) return -1;
    } else {
        if (!cmp_json_object_parse(request->input_json, &input) || input.field_count != 1u ||
            !cmp_json_object_str(&input, "expression", expression, sizeof(expression)) ||
            dice_parse(expression, &spec, error, sizeof(error)) != DICE_OK) return -1;
        /* Canonical dice syntax makes equivalent whitespace retries identical. */
        snprintf(request->input_json, sizeof(request->input_json), "{\"expression\":\"%s\"}", spec.expression);
    }
    request->deadline_unix_ms = now + 5000;
    if (cmp_json_object_key_count(&object, "deadline_unix_ms") != 0 &&
        (!cmp_json_object_i64(&object, "deadline_unix_ms", &request->deadline_unix_ms) ||
         request->deadline_unix_ms <= now || request->deadline_unix_ms > now + 60000)) return -1;
    return 0;
}

static void reply_call(pt_manager *manager, pt_http_auth *auth, int fd, const char *id,
                        const char *path, const pt_http_identity *identity) {
    pt_call call;
    char *output;
    char signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    size_t length;
    static const char prefix[] = "{\"accepted\":true,\"call\":";
    if (pt_read_owned_call(manager, id, identity->user, &call) != 0) {
        http_reply(fd, 404, "{\"accepted\":false,\"error\":\"not_found\"}");
        return;
    }
    if (call.state == PT_ST_COMPLETED && strcmp(call.tool_id, "dnd-dice-roll") == 0 &&
        pt_artifact_verify(manager->worker.artifact_dir, &call) != 0) {
        http_reply(fd, 503, "{\"accepted\":false,\"error\":\"artifact_unavailable\"}");
        return;
    }
    output = malloc(PT_RECORD_CAP + sizeof(prefix) + sizeof(signature) + 32u);
    if (!output) {
        http_reply(fd, 503, "{\"accepted\":false,\"error\":\"capacity\"}");
        return;
    }
    memcpy(output, prefix, sizeof(prefix) - 1u);
    length = pt_store_record_encode(output + sizeof(prefix) - 1u, PT_RECORD_CAP, &call);
    if (length && voice_auth_sign(auth->secret, strlen(auth->secret), "RESULT", path,
            identity->user, identity->timestamp, identity->nonce,
            (const uint8_t *)output + sizeof(prefix) - 1u, length, signature) == VOICE_AUTH_OK) {
        length += sizeof(prefix) - 1u;
        (void)snprintf(output + length, sizeof(signature) + 32u,
                       ",\"signature\":\"%s\"}", signature);
        http_reply(fd, 200, output);
    } else http_reply(fd, 500, "{\"accepted\":false,\"error\":\"record_encoding\"}");
    free(output);
}

static void handle_conn(pt_manager *manager, pt_http_auth *auth, int fd) {
    char buffer[PT_HTTP_HEAD_CAP + PT_HTTP_BODY_CAP + 1u];
    char method[16], path[512], content_type[64];
    pt_http_identity identity = {0};
    char *body = NULL;
    size_t body_length = 0;
    cmp_http_headers headers;
    if (read_request(fd, buffer, sizeof(buffer), &headers, method, path, &body, &body_length) != 0) {
        http_reply(fd, 400, "{\"accepted\":false,\"error\":\"invalid_http\"}");
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/healthz") == 0 && body_length == 0u) {
        if (pt_manager_ready(manager))
            http_reply(fd, 200, "{\"status\":\"ok\",\"service\":\"c-ptools\",\"auth\":\"configured\"}");
        else http_reply(fd, 503, "{\"status\":\"unavailable\",\"service\":\"c-ptools\"}");
        return;
    }
    if (authenticate(auth, &headers, method, path, body, body_length, &identity) != 0) {
        http_reply(fd, 401, "{\"accepted\":false,\"error\":\"unauthorized\"}");
        return;
    }
    if (strcmp(method, "POST") == 0 &&
        (cmp_http_headers_get_unique(&headers, "Content-Type", content_type, sizeof(content_type)) != CMP_HTTP_HEADER_FOUND ||
         strcmp(content_type, "application/json") != 0)) {
        http_reply(fd, 400, "{\"accepted\":false,\"error\":\"invalid_content_type\"}");
        return;
    }
    if (strcmp(method, "POST") == 0 &&
        (strcmp(path, "/v1/tools/execute") == 0 || strcmp(path, "/v1/tools/calls") == 0)) {
        pt_start_req request;
        pt_dispatch_resp response;
        if (parse_tool_request(body, identity.user, &request) != 0) {
            http_reply(fd, 400, "{\"accepted\":false,\"error\":\"invalid_tool_request\"}");
            return;
        }
        if (pt_execute_core(manager, &request, &response) != 0 || !response.accepted) {
            http_reply(fd, 409, "{\"accepted\":false,\"error\":\"tool_rejected\"}");
            return;
        }
        reply_call(manager, auth, fd, response.tool_call_id, path, &identity);
        return;
    }
    if (strncmp(path, "/v1/tools/calls/", sizeof("/v1/tools/calls/") - 1u) == 0) {
        const char *start = path + sizeof("/v1/tools/calls/") - 1u;
        const char *slash = strchr(start, '/');
        size_t length = slash ? (size_t)(slash - start) : strlen(start);
        char id[PT_ID];
        if (length >= sizeof(id)) {
            http_reply(fd, 400, "{\"accepted\":false,\"error\":\"invalid_id\"}");
            return;
        }
        memcpy(id, start, length);
        id[length] = '\0';
        if (ts_valid_id(id) && !slash && strcmp(method, "GET") == 0 && body_length == 0u) {
            reply_call(manager, auth, fd, id, path, &identity);
            return;
        }
        if (ts_valid_id(id) && slash && strcmp(slash, "/cancel") == 0 &&
            strcmp(method, "POST") == 0) {
            cmp_json_object object;
            pt_cancel_req cancel = {0};
            pt_start_resp response;
            if (!cmp_json_object_parse(body, &object) || object.field_count > 1u ||
                (object.field_count == 1u &&
                 !cmp_json_object_str(&object, "reason", cancel.reason, sizeof(cancel.reason)))) {
                http_reply(fd, 400, "{\"accepted\":false,\"error\":\"invalid_cancel\"}");
                return;
            }
            memcpy(cancel.tool_call_id, id, length + 1u);
            memcpy(cancel.user_id, identity.user, strlen(identity.user) + 1u);
            if (pt_cancel_core(manager, &cancel, &response) != 0 || !response.accepted) {
                http_reply(fd, 404, "{\"accepted\":false,\"error\":\"not_found\"}");
                return;
            }
            reply_call(manager, auth, fd, id, path, &identity);
            return;
        }
    }
    http_reply(fd, 404, "{\"accepted\":false,\"error\":\"not_found\"}");
}

int pt_http_serve(pt_manager *manager, const char *host, int port,
                  volatile sig_atomic_t *stop, const char *secret) {
    pt_http_auth auth = {0};
    struct sockaddr_in address = {0};
    int fd = -1, result = -1, one = 1;
    if (!manager || !host || !stop || port < 1 || port > 65535 || !secret ||
        strlen(secret) < 32u || strlen(secret) > 255u) return -1;
    auth.secret = secret;
    if (voice_auth_verifier_init(&auth.verifier, secret, strlen(secret)) != VOICE_AUTH_OK ||
        voice_auth_nonce_index_init(&auth.nonces, PT_HTTP_NONCES) != VOICE_AUTH_OK) goto done;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) goto done;
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) goto done;
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 32) != 0) goto done;
    fprintf(stderr, "c-ptools authenticated HTTP listening on %s:%d\n", host, port);
    result = 0;
    while (!*stop) {
        struct pollfd listener = {fd, POLLIN, 0};
        int ready = poll(&listener, 1u, 200);
        int client;
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { result = -1; break; }
        if (!ready) continue;
        client = accept(fd, NULL, NULL);
        if (client < 0 && errno == EINTR) continue;
        if (client < 0) { result = -1; break; }
        handle_conn(manager, &auth, client);
        close(client);
    }
done:
    if (fd >= 0) close(fd);
    voice_auth_verifier_destroy(&auth.verifier);
    voice_auth_nonce_index_destroy(&auth.nonces);
    return result;
}
