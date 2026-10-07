#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "http_min.h"
#include "http_response_header_name.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int listen_fd;
    const char *response;
    size_t response_len;
    size_t fragment_bytes;
    int request_seen;
    atomic_int *cancel_on_request;
    long hold_ns;
    int wait_for_client_close;
} server_case;

static void *serve_once(void *arg) {
    server_case *test = (server_case *)arg;
    char request[2048];
    size_t request_len = 0;
    int client = accept(test->listen_fd, NULL, NULL);
    if (client < 0) return NULL;
    while (request_len + 1 < sizeof(request)) {
        ssize_t n = recv(client, request + request_len, sizeof(request) - request_len - 1, 0);
        if (n <= 0) break;
        request_len += (size_t)n;
        request[request_len] = '\0';
        {
            char *headers_end = strstr(request, "\r\n\r\n");
            if (headers_end != NULL) {
                char *length_header = strstr(request, "Content-Length: ");
                size_t headers_len = (size_t)(headers_end + 4 - request);
                size_t body_len = 0;
                if (length_header)
                    body_len = (size_t)strtoul(
                        length_header + sizeof("Content-Length: ") - 1, NULL, 10);
                if (body_len > sizeof(request) - headers_len - 1) break;
                if (request_len >= headers_len + body_len) {
                    test->request_seen = 1;
                    if (test->cancel_on_request)
                        atomic_store_explicit(
                            test->cancel_on_request, 1, memory_order_relaxed);
                    break;
                }
            }
        }
    }
    if (test->request_seen) {
        if (test->hold_ns > 0) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = test->hold_ns};
            nanosleep(&delay, NULL);
        }
        size_t offset = 0;
        size_t fragment = test->fragment_bytes ? test->fragment_bytes : test->response_len;
        while (offset < test->response_len) {
            size_t take = test->response_len - offset;
            if (take > fragment) take = fragment;
            ssize_t n = send(client, test->response + offset, take, MSG_NOSIGNAL);
            if (n <= 0) break;
            offset += (size_t)n;
        }
        if (test->wait_for_client_close) {
            struct pollfd pfd = {.fd = client, .events = POLLIN};
            int rc;
            do {
                rc = poll(&pfd, 1, 2000);
            } while (rc < 0 && errno == EINTR);
        }
    }
    close(client);
    return NULL;
}

static int run_case_ex(
    const char *response,
    size_t fragment_bytes,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len,
    int *status,
    atomic_int *cancel,
    long hold_ns
) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char url[128];
    pthread_t thread;
    server_case test;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int rc;
    if (listen_fd < 0) return HTTP_MIN_ERR_CONNECT;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0 ||
        getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(listen_fd);
        return HTTP_MIN_ERR_CONNECT;
    }
    memset(&test, 0, sizeof(test));
    test.listen_fd = listen_fd;
    test.response = response;
    test.response_len = strlen(response);
    test.fragment_bytes = fragment_bytes;
    test.cancel_on_request = cancel;
    test.hold_ns = hold_ns;
    if (pthread_create(&thread, NULL, serve_once, &test) != 0) {
        close(listen_fd);
        return HTTP_MIN_ERR_CONNECT;
    }
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)ntohs(addr.sin_port));
    if (cancel) {
        static const uint8_t body[] = "body";
        rc = http_min_post_cancel(
            url, "text/plain", body, sizeof(body) - 1, out, out_cap,
            out_len, status, 1000, cancel);
    } else {
        rc = http_min_get(url, out, out_cap, out_len, status, 1000);
    }
    pthread_join(thread, NULL);
    close(listen_fd);
    if (!test.request_seen && rc == HTTP_MIN_OK) return HTTP_MIN_ERR_IO;
    return rc;
}

static int run_case(
    const char *response,
    size_t fragment_bytes,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len,
    int *status
) {
    return run_case_ex(
        response, fragment_bytes, out, out_cap, out_len, status, NULL, 0);
}

static int run_header_line_case(size_t line_len, size_t fragment_bytes) {
    static const char status_line[] = "HTTP/1.1 200 OK\r\n";
    static const char suffix[] = "\r\nContent-Length: 0\r\n\r\n";
    uint8_t out[1];
    char *response;
    size_t out_len;
    size_t response_cap;
    size_t used = 0;
    int status;
    int rc;
    if (line_len < 2u || line_len > SIZE_MAX - sizeof(status_line) - sizeof(suffix))
        return HTTP_MIN_ERR_ARGUMENT;
    response_cap = sizeof(status_line) - 1u + line_len + sizeof(suffix);
    response = (char *)malloc(response_cap);
    if (!response) return HTTP_MIN_ERR_IO;
    memcpy(response + used, status_line, sizeof(status_line) - 1u);
    used += sizeof(status_line) - 1u;
    memcpy(response + used, "X:", 2u);
    memset(response + used + 2u, 'a', line_len - 2u);
    used += line_len;
    memcpy(response + used, suffix, sizeof(suffix));
    rc = run_case(
        response, fragment_bytes, out, sizeof(out), &out_len, &status);
    free(response);
    return rc;
}

static int run_header_budget_case(void) {
    static const char status_line[] = "HTTP/1.1 200 OK\r\n";
    static const char suffix[] = "Content-Length: 0\r\n\r\n";
    enum { HEADER_LINES = 9, HEADER_LINE_LEN = 4000 };
    uint8_t out[1];
    char *response;
    size_t out_len;
    size_t response_cap = sizeof(status_line) +
        (size_t)HEADER_LINES * ((size_t)HEADER_LINE_LEN + 2u) + sizeof(suffix);
    size_t used = 0;
    int status;
    int line;
    int rc;
    response = (char *)malloc(response_cap);
    if (!response) return HTTP_MIN_ERR_IO;
    memcpy(response + used, status_line, sizeof(status_line) - 1u);
    used += sizeof(status_line) - 1u;
    for (line = 0; line < HEADER_LINES; ++line) {
        memcpy(response + used, "X:", 2u);
        memset(response + used + 2u, 'a', HEADER_LINE_LEN - 2u);
        used += HEADER_LINE_LEN;
        memcpy(response + used, "\r\n", 2u);
        used += 2u;
    }
    memcpy(response + used, suffix, sizeof(suffix));
    rc = run_case(response, 4093u, out, sizeof(out), &out_len, &status);
    free(response);
    return rc;
}

static int run_exact_header_budget_case(void) {
    static const char status_line[] = "HTTP/1.1 200 OK\r\n";
    static const char suffix[] = "Content-Length: 0\r\n\r\n";
    enum {
        HEADER_LINES = 8,
        HEADER_LINE_LEN = 4000,
        FINAL_LINE_LEN = 712
    };
    uint8_t out[1];
    char response[32769];
    size_t out_len;
    size_t used = 0;
    int status;
    int line;
    memcpy(response + used, status_line, sizeof(status_line) - 1u);
    used += sizeof(status_line) - 1u;
    for (line = 0; line < HEADER_LINES; ++line) {
        memcpy(response + used, "X:", 2u);
        memset(response + used + 2u, 'a', HEADER_LINE_LEN - 2u);
        used += HEADER_LINE_LEN;
        memcpy(response + used, "\r\n", 2u);
        used += 2u;
    }
    memcpy(response + used, "X:", 2u);
    memset(response + used + 2u, 'a', FINAL_LINE_LEN - 2u);
    used += FINAL_LINE_LEN;
    memcpy(response + used, "\r\n", 2u);
    used += 2u;
    memcpy(response + used, suffix, sizeof(suffix));
    return run_case(response, 4093u, out, sizeof(out), &out_len, &status);
}

typedef struct {
    uint8_t data[64];
    size_t len;
    int attempts;
    int calls;
    int fail;
    atomic_int *cancel_after_body;
    int buffered_response;
} stream_capture;

static int capture_body(const uint8_t *data, size_t len, void *user) {
    stream_capture *capture = (stream_capture *)user;
    capture->attempts++;
    if (capture->fail || len > sizeof(capture->data) - capture->len) return -1;
    memcpy(capture->data + capture->len, data, len);
    capture->len += len;
    capture->calls++;
    if (capture->cancel_after_body)
        atomic_store_explicit(capture->cancel_after_body, 1, memory_order_relaxed);
    return 0;
}

static int run_stream_case_controlled(
    const char *response,
    stream_capture *capture,
    size_t max_body_len,
    size_t *out_len,
    int *status,
    atomic_int *cancel,
    int timeout_ms,
    int wait_for_client_close
) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char url[128];
    pthread_t thread;
    server_case test;
    static const uint8_t body[] = "x";
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int rc;
    if (listen_fd < 0) return HTTP_MIN_ERR_CONNECT;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0 ||
        getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(listen_fd);
        return HTTP_MIN_ERR_CONNECT;
    }
    memset(&test, 0, sizeof(test));
    test.listen_fd = listen_fd;
    test.response = response;
    test.response_len = strlen(response);
    test.fragment_bytes = capture->buffered_response ? 0u : 1u;
    test.wait_for_client_close = wait_for_client_close;
    if (pthread_create(&thread, NULL, serve_once, &test) != 0) {
        close(listen_fd);
        return HTTP_MIN_ERR_CONNECT;
    }
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/stream", (unsigned)ntohs(addr.sin_port));
    rc = http_min_post_stream_headers_cancel(
        url, "application/octet-stream", NULL, 0, body, sizeof(body) - 1,
        capture_body, capture, max_body_len, out_len, status, timeout_ms, cancel);
    pthread_join(thread, NULL);
    close(listen_fd);
    return rc;
}

static int run_stream_case(
    const char *response,
    stream_capture *capture,
    size_t max_body_len,
    size_t *out_len,
    int *status
) {
    return run_stream_case_controlled(
        response, capture, max_body_len, out_len, status, NULL, 1000, 0);
}

typedef struct {
    int listen_fd;
    int close_mode;
    int response_mode;
    int request_limit;
    int accepts;
    int requests;
    int keep_alive_requests;
    int canonical_requests;
    int post_prefix_unchanged;
    unsigned int port;
    atomic_int connection_closed;
} reuse_server;

enum {
    REUSE_HTTP11_LENGTH = 0,
    REUSE_HTTP11_CHUNKED,
    REUSE_HTTP11_PARTIAL_CONNECTION,
    REUSE_HTTP10_CLOSE,
    REUSE_HTTP10_KEEP_ALIVE
};

static int accept_reuse_client(int listen_fd) {
    struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
    int rc;
    do {
        rc = poll(&pfd, 1, 2000);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0 || (pfd.revents & POLLIN) == 0) return -1;
    return accept(listen_fd, NULL, NULL);
}

static int read_reuse_request(int fd, char *request, size_t request_cap) {
    size_t used = 0;
    while (used + 1u < request_cap) {
        ssize_t count = recv(fd, request + used, request_cap - used - 1u, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        used += (size_t)count;
        request[used] = '\0';
        {
            char *headers_end = strstr(request, "\r\n\r\n");
            char *length_header;
            size_t headers_len;
            size_t body_len;
            if (!headers_end) continue;
            length_header = strstr(request, "Content-Length: ");
            if (!length_header) return -1;
            headers_len = (size_t)(headers_end + 4 - request);
            body_len = (size_t)strtoul(
                length_header + sizeof("Content-Length: ") - 1u, NULL, 10);
            if (body_len > request_cap - headers_len - 1u) return -1;
            if (used >= headers_len + body_len) return 0;
        }
    }
    return -1;
}

static void *serve_reuse(void *arg) {
    reuse_server *server = (reuse_server *)arg;
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    int client = -1;
    int request_no;
    for (request_no = 0; request_no < server->request_limit; ++request_no) {
        char request[2048];
        const char *protocol = server->response_mode >= REUSE_HTTP10_CLOSE ?
            "HTTP/1.0" : "HTTP/1.1";
        const char *connection = "Connection: keep-alive\r\n";
        int close_after_response;
        if (client < 0) {
            client = accept_reuse_client(server->listen_fd);
            if (client < 0) break;
            server->accepts++;
            (void)setsockopt(
                client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        }
        if (read_reuse_request(client, request, sizeof(request)) != 0) break;
        server->requests++;
        if (strstr(request, "Connection: keep-alive\r\n") != NULL)
            server->keep_alive_requests++;
        if (server->port != 0u) {
            char host[64];
            int written = snprintf(
                host, sizeof(host), "Host: 127.0.0.1:%u\r\n", server->port);
            if (written > 0 && (size_t)written < sizeof(host) &&
                strncmp(
                    request,
                    "POST /reuse HTTP/1.1\r\n",
                    sizeof("POST /reuse HTTP/1.1\r\n") - 1u) == 0 &&
                strstr(request, host) != NULL &&
                strstr(request, "User-Agent: voice-c-runtime/1\r\n") != NULL &&
                strstr(request, "Accept: */*\r\n") != NULL &&
                strstr(request, "Content-Type: text/plain\r\n") != NULL &&
                strstr(request, "Content-Length: 1\r\n") != NULL)
                server->canonical_requests++;
        }
        if (server->close_mode == 4)
            connection = "Connection: upgrade, Keep-Alive, close\r\n";
        else if ((request_no == 0 && server->close_mode == 1) ||
                 server->close_mode == 3)
            connection = "Connection: close\r\n";
        else if (server->response_mode == REUSE_HTTP11_PARTIAL_CONNECTION)
            connection = "Connection: xclose, Keep-Alive\r\n";
        else if (server->response_mode == REUSE_HTTP10_CLOSE)
            connection = "";
        else if (server->response_mode == REUSE_HTTP10_KEEP_ALIVE)
            connection = "Connection: Keep-Alive\r\n";
        if (server->response_mode == REUSE_HTTP11_CHUNKED) {
            if (dprintf(
                    client,
                    "%s 200 OK\r\nTransfer-Encoding: chunked\r\n%s\r\n"
                    "2\r\nok\r\n0\r\n\r\n",
                    protocol,
                    connection) < 0)
                break;
        } else if (dprintf(
                       client,
                       "%s 200 OK\r\nContent-Length: 2\r\n%s\r\nok",
                       protocol,
                       connection) < 0) {
            break;
        }
        close_after_response =
            (request_no == 0 && server->close_mode != 0) ||
            server->close_mode == 3 ||
            server->response_mode == REUSE_HTTP10_CLOSE;
        if (close_after_response) {
            close(client);
            client = -1;
            atomic_store_explicit(
                &server->connection_closed, 1, memory_order_release);
        }
    }
    if (client >= 0) close(client);
    return NULL;
}

static int wait_for_server_close(const atomic_int *closed) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
    int attempt;
    for (attempt = 0; attempt < 2000; ++attempt) {
        if (atomic_load_explicit(closed, memory_order_acquire) != 0) return 0;
        (void)nanosleep(&pause, NULL);
    }
    return -1;
}

static int run_reuse_case(
    int close_mode,
    int response_mode,
    int stream,
    int prewarm,
    reuse_server *server
) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char url[128];
    pthread_t thread;
    http_min_client client;
    char saved_post_prefix[sizeof(client.post_prefix)];
    size_t saved_post_prefix_len;
    uint8_t out[8];
    size_t out_len = 0;
    int status = 0;
    int rc;
    int request_no;
    static const uint8_t body[] = "x";
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0 || !server) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 2) != 0 ||
        getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(listen_fd);
        return -1;
    }
    memset(server, 0, sizeof(*server));
    server->listen_fd = listen_fd;
    server->close_mode = close_mode;
    server->response_mode = response_mode;
    server->request_limit = 2;
    server->port = (unsigned int)ntohs(addr.sin_port);
    atomic_init(&server->connection_closed, 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/reuse", (unsigned)ntohs(addr.sin_port));
    if (http_min_client_init(&client, url) != HTTP_MIN_OK) {
        close(listen_fd);
        return -1;
    }
    saved_post_prefix_len = client.post_prefix_len;
    if (saved_post_prefix_len >= sizeof(saved_post_prefix)) {
        http_min_client_destroy(&client);
        close(listen_fd);
        return -1;
    }
    memcpy(
        saved_post_prefix, client.post_prefix, saved_post_prefix_len + 1u);
    if (pthread_create(&thread, NULL, serve_reuse, server) != 0) {
        http_min_client_destroy(&client);
        close(listen_fd);
        return -1;
    }
    if (prewarm &&
        (http_min_client_warm(&client, 1000) != HTTP_MIN_OK ||
         http_min_client_warm(&client, 1000) != HTTP_MIN_OK)) {
        http_min_client_destroy(&client);
        pthread_cancel(thread);
        pthread_join(thread, NULL);
        close(listen_fd);
        return -1;
    }
    for (request_no = 0; request_no < 2; ++request_no) {
        stream_capture capture;
        if (request_no == 1 && close_mode == 2 &&
            wait_for_server_close(&server->connection_closed) != 0)
            break;
        out_len = 0;
        status = 0;
        memset(&capture, 0, sizeof(capture));
        if (stream) {
            rc = http_min_client_post_stream_headers_cancel(
                &client, "text/plain", NULL, 0, body, sizeof(body) - 1u,
                capture_body, &capture, sizeof(capture.data),
                &out_len, &status, 1000, NULL);
            if (rc != HTTP_MIN_OK || status != 200 || out_len != 2u ||
                capture.len != 2u || memcmp(capture.data, "ok", 2u) != 0)
                break;
        } else {
            rc = http_min_client_post(
                &client, "text/plain", body, sizeof(body) - 1u,
                out, sizeof(out), &out_len, &status, 1000);
            if (rc != HTTP_MIN_OK || status != 200 || out_len != 2u ||
                memcmp(out, "ok", 2u) != 0)
                break;
        }
    }
    server->post_prefix_unchanged =
        client.post_prefix_len == saved_post_prefix_len &&
        memcmp(
            client.post_prefix,
            saved_post_prefix,
            saved_post_prefix_len + 1u) == 0;
    http_min_client_destroy(&client);
    pthread_join(thread, NULL);
    close(listen_fd);
    return request_no == 2 ? 0 : -1;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int run_connection_benchmark(int persistent, uint64_t *elapsed_ns, int *accepts) {
    enum { REQUESTS = 256 };
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char url[128];
    pthread_t thread;
    reuse_server server;
    http_min_client client;
    uint8_t out[8];
    uint64_t started;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int request_no;
    int rc = -1;
    static const uint8_t body[] = "x";
    if (listen_fd < 0 || !elapsed_ns || !accepts) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 8) != 0 ||
        getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(listen_fd);
        return -1;
    }
    memset(&server, 0, sizeof(server));
    server.listen_fd = listen_fd;
    server.close_mode = persistent ? 0 : 3;
    server.response_mode = REUSE_HTTP11_LENGTH;
    server.request_limit = REQUESTS;
    atomic_init(&server.connection_closed, 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/bench", (unsigned)ntohs(addr.sin_port));
    if ((persistent && http_min_client_init(&client, url) != HTTP_MIN_OK) ||
        pthread_create(&thread, NULL, serve_reuse, &server) != 0) {
        if (persistent) http_min_client_destroy(&client);
        close(listen_fd);
        return -1;
    }
    started = monotonic_ns();
    for (request_no = 0; request_no < REQUESTS; ++request_no) {
        size_t out_len = 0;
        int status = 0;
        if (persistent)
            rc = http_min_client_post(
                &client, "text/plain", body, sizeof(body) - 1u,
                out, sizeof(out), &out_len, &status, 1000);
        else
            rc = http_min_post(
                url, "text/plain", body, sizeof(body) - 1u,
                out, sizeof(out), &out_len, &status, 1000);
        if (rc != HTTP_MIN_OK || status != 200 || out_len != 2u ||
            memcmp(out, "ok", 2u) != 0)
            break;
    }
    *elapsed_ns = monotonic_ns() - started;
    if (persistent) http_min_client_destroy(&client);
    pthread_join(thread, NULL);
    close(listen_fd);
    *accepts = server.accepts;
    return request_no == REQUESTS ? 0 : -1;
}

static int failures;

static void expect(const char *name, int condition) {
    if (condition) printf("PASS %s\n", name);
    else {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static http_response_header_name response_header_name_oracle(
    const char *name,
    size_t name_len
) {
    if (!name) return HTTP_RESPONSE_HEADER_UNKNOWN;
    if (name_len == sizeof("Content-Length") - 1u &&
        strncasecmp(name, "Content-Length", name_len) == 0)
        return HTTP_RESPONSE_HEADER_CONTENT_LENGTH;
    if (name_len == sizeof("Transfer-Encoding") - 1u &&
        strncasecmp(name, "Transfer-Encoding", name_len) == 0)
        return HTTP_RESPONSE_HEADER_TRANSFER_ENCODING;
    if (name_len == sizeof("Connection") - 1u &&
        strncasecmp(name, "Connection", name_len) == 0)
        return HTTP_RESPONSE_HEADER_CONNECTION;
    return HTTP_RESPONSE_HEADER_UNKNOWN;
}

static int response_header_name_differential(void) {
    static const char *const names[] = {
        "Content-Length",
        "Transfer-Encoding",
        "Connection",
    };
    char candidate[sizeof("Transfer-Encoding")];
    size_t name_no;

    if (http_response_header_classify(NULL, 0u) !=
            HTTP_RESPONSE_HEADER_UNKNOWN ||
        http_response_header_classify("", 0u) !=
            HTTP_RESPONSE_HEADER_UNKNOWN ||
        http_response_header_classify("Content_Length", 14u) !=
            HTTP_RESPONSE_HEADER_UNKNOWN)
        return 0;
    for (name_no = 0; name_no < sizeof(names) / sizeof(names[0]); name_no++) {
        const size_t name_len = strlen(names[name_no]);
        size_t byte_no;
        memcpy(candidate, names[name_no], name_len + 1u);
        if (http_response_header_classify(candidate, name_len) !=
            response_header_name_oracle(candidate, name_len))
            return 0;
        for (byte_no = 0; byte_no < name_len; byte_no++) {
            unsigned value;
            const char saved = candidate[byte_no];
            for (value = 0; value <= 255u; value++) {
                candidate[byte_no] = (char)(unsigned char)value;
                if (http_response_header_classify(candidate, name_len) !=
                    response_header_name_oracle(candidate, name_len))
                    return 0;
            }
            candidate[byte_no] = saved;
        }
    }
    return 1;
}

static void response_header_name_benchmark(
    int current,
    uint64_t *elapsed_ns,
    unsigned *checksum
) {
    typedef http_response_header_name (*classifier_fn)(const char *, size_t);
    char names[4][sizeof("Transfer-Encoding")] = {
        "Content-Length",
        "Transfer-Encoding",
        "Connection",
        "Server",
    };
    static const size_t name_lengths[4] = {
        sizeof("Content-Length") - 1u,
        sizeof("Transfer-Encoding") - 1u,
        sizeof("Connection") - 1u,
        sizeof("Server") - 1u,
    };
    classifier_fn classify = current ?
        http_response_header_classify : response_header_name_oracle;
    uint64_t started = monotonic_ns();
    unsigned sum = 0u;
    size_t iteration;

    for (iteration = 0u; iteration < 5000000u; iteration++) {
        size_t index = iteration & 3u;
        names[index][0] = (char)((unsigned char)names[index][0] ^ 0x20u);
        sum += (unsigned)classify(names[index], name_lengths[index]);
    }
    *elapsed_ns = monotonic_ns() - started;
    *checksum = sum;
}

int main(int argc, char **argv) {
    uint8_t out[64];
    size_t out_len;
    int status;
    int rc;

    if (argc == 2 &&
        (strcmp(argv[1], "--bench-header-names-current") == 0 ||
         strcmp(argv[1], "--bench-header-names-libc") == 0)) {
        uint64_t elapsed_ns = 0u;
        unsigned checksum = 0u;
        int current = strcmp(argv[1], "--bench-header-names-current") == 0;
        response_header_name_benchmark(current, &elapsed_ns, &checksum);
        printf(
            "BenchmarkHTTPResponseHeaderNames\tmode=%s\tselections=5000000\t"
            "checksum=%u\ttotal_ns=%llu\tns_per_selection=%.3f\n",
            current ? "current" : "libc",
            checksum,
            (unsigned long long)elapsed_ns,
            (double)elapsed_ns / 5000000.0);
        return checksum == 7500000u ? 0 : 1;
    }

    if (argc == 2 &&
        (strcmp(argv[1], "--bench-persistent") == 0 ||
         strcmp(argv[1], "--bench-close") == 0)) {
        uint64_t elapsed_ns = 0;
        int accepts = 0;
        int persistent = strcmp(argv[1], "--bench-persistent") == 0;
        rc = run_connection_benchmark(persistent, &elapsed_ns, &accepts);
        if (rc != 0) return 1;
        printf(
            "BenchmarkHTTPConnections\tmode=%s\trequests=256\taccepts=%d\t"
            "total_ns=%llu\tns_per_request=%llu\n",
            persistent ? "persistent" : "close",
            accepts,
            (unsigned long long)elapsed_ns,
            (unsigned long long)(elapsed_ns / UINT64_C(256)));
        return 0;
    }

    expect(
        "response header name classifier exhaustive differential",
        response_header_name_differential());

    rc = run_case(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
        1, out, sizeof(out), &out_len, &status);
    expect("fragmented content-length", rc == HTTP_MIN_OK && status == 200 &&
        out_len == 5 && memcmp(out, "hello", 5) == 0);

    rc = run_case(
        "HTTP/1.1 200 OK\r\ntRaNsFeR-EnCoDiNg: chunked\r\n\r\n"
        "2;ext=yes\r\nhe\r\n3\r\nllo\r\n0\r\nX-Test: ok\r\n\r\n",
        2, out, sizeof(out), &out_len, &status);
    expect("mixed-case fragmented chunked", rc == HTTP_MIN_OK && out_len == 5 &&
        memcmp(out, "hello", 5) == 0);

    {
        static const char headers[] =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        static const char *const truncated[] = {
            "", "1", "3\r\nx", "1\r\nx", "1\r\nx\r", "1\r\nx\r\n", "0\r\nX: v"
        };
        static const char *const malformed[] = {"Q\r\n", "1\r\nxXX"};
        char response[4200];
        char name[96];
        size_t i;
        for (i = 0; i < sizeof(truncated) / sizeof(truncated[0]); ++i) {
            stream_capture capture = {0};
            snprintf(response, sizeof(response), "%s%s", headers, truncated[i]);
            rc = run_case(response, 1u, out, sizeof(out), &out_len, &status);
            snprintf(name, sizeof(name), "buffered chunk truncation %zu preserves IO", i);
            expect(name, rc == HTTP_MIN_ERR_IO && status == 200 && out_len == 0u);
            rc = run_stream_case(response, &capture, sizeof(capture.data), &out_len, &status);
            snprintf(name, sizeof(name), "streamed chunk truncation %zu preserves IO", i);
            expect(name, rc == HTTP_MIN_ERR_IO && status == 200 && out_len == 0u);
        }
        for (i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
            stream_capture capture = {0};
            snprintf(response, sizeof(response), "%s%s", headers, malformed[i]);
            rc = run_stream_case(response, &capture, sizeof(capture.data), &out_len, &status);
            snprintf(name, sizeof(name), "malformed chunk framing %zu remains PARSE", i);
            expect(name, rc == HTTP_MIN_ERR_PARSE && status == 200 && out_len == 0u);
        }
        memcpy(response, headers, sizeof(headers) - 1u);
        memset(response + sizeof(headers) - 1u, '0', 4095u);
        memcpy(response + sizeof(headers) - 1u + 4095u, "\r\n", 3u);
        rc = run_case(response, 257u, out, sizeof(out), &out_len, &status);
        expect("oversized chunk size line preserves capacity", rc == HTTP_MIN_ERR_CAPACITY &&
            status == 200 && out_len == 0u);

        {
            atomic_int cancel;
            stream_capture capture = {0};
            atomic_init(&cancel, 0);
            capture.cancel_after_body = &cancel;
            capture.buffered_response = 1;
            rc = run_stream_case_controlled(
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                "1\r\nx\r\n1\r\ny\r\n0\r\n\r\n",
                &capture, sizeof(capture.data), &out_len, &status, &cancel, 1000, 0);
            expect("cancellation suppresses already-buffered body and completion",
                rc == HTTP_MIN_ERR_CANCELED && capture.calls == 1 &&
                capture.len == 1u && capture.data[0] == 'x' && out_len == 0u);
        }

        /* The body callback cancels only after HTTP headers and one byte arrive.
         * An open server socket distinguishes deadline expiry from EOF. */
        for (i = 0; i < 2u; ++i) {
            atomic_int cancel;
            stream_capture capture = {0};
            atomic_init(&cancel, 0);
            capture.cancel_after_body = &cancel;
            snprintf(response, sizeof(response), "%s1\r\nx%s", headers, i ? "\r\n" : "");
            rc = run_stream_case_controlled(response, &capture, sizeof(capture.data),
                &out_len, &status, &cancel, 1000, 1);
            snprintf(name, sizeof(name), "chunk %s read preserves cancellation", i ? "size" : "terminator");
            expect(name, rc == HTTP_MIN_ERR_CANCELED && status == 200 && out_len == 0u &&
                capture.len == 1u && capture.data[0] == 'x' && capture.calls == 1);
            memset(&capture, 0, sizeof(capture));
            rc = run_stream_case_controlled(response, &capture, sizeof(capture.data),
                &out_len, &status, NULL, 1000, 1);
            snprintf(name, sizeof(name), "chunk %s read preserves deadline IO", i ? "size" : "terminator");
            expect(name, rc == HTTP_MIN_ERR_IO && status == 200 && out_len == 0u &&
                capture.len == 1u && capture.data[0] == 'x' && capture.calls == 1);
        }
    }

    rc = run_case(
        "HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh: 5\r\n"
        "cOnNeCtIoN: close\r\n\r\nhello",
        1, out, sizeof(out), &out_len, &status);
    expect("mixed-case response header names", rc == HTTP_MIN_OK &&
        status == 200 && out_len == 5 && memcmp(out, "hello", 5) == 0);

    rc = run_case(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n",
        0, out, sizeof(out), &out_len, &status);
    expect("reject unsupported transfer coding", rc == HTTP_MIN_ERR_PARSE);

    rc = run_case(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        0, out, sizeof(out), &out_len, &status);
    expect("reject duplicate transfer encoding", rc == HTTP_MIN_ERR_PARSE);

    rc = run_case(
        "HTTP/1.9 200garbage\r\nContent-Length: 0\r\n\r\n",
        0, out, sizeof(out), &out_len, &status);
    expect("reject malformed status line", rc == HTTP_MIN_ERR_PARSE);

    expect(
        "accept maximum response header line",
        run_header_line_case(4094u, 257u) == HTTP_MIN_OK);
    expect(
        "reject oversized response header line",
        run_header_line_case(4095u, 257u) == HTTP_MIN_ERR_CAPACITY);
    expect(
        "reject oversized response header block",
        run_header_budget_case() == HTTP_MIN_ERR_CAPACITY);
    expect(
        "accept exact response header budget",
        run_exact_header_budget_case() == HTTP_MIN_OK);

    rc = run_case(
        "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\n123456",
        0, out, 5, &out_len, &status);
    expect("content-length capacity", rc == HTTP_MIN_ERR_CAPACITY);

    rc = run_case(
        "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n123456",
        0, out, 5, &out_len, &status);
    expect("close-delimited capacity", rc == HTTP_MIN_ERR_CAPACITY);

    rc = run_case(
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nok",
        0, out, sizeof(out), &out_len, &status);
    expect("reject conflicting lengths", rc == HTTP_MIN_ERR_PARSE);

    rc = run_case(
        "HTTP/1.1 503 Nope\r\nContent-Length: 4\r\n\r\nnope",
        0, out, sizeof(out), &out_len, &status);
    expect("status with body", rc == HTTP_MIN_ERR_STATUS && status == 503 &&
        out_len == 4 && memcmp(out, "nope", 4) == 0);

    expect("reject port overflow",
        http_min_parse_url("http://host:999999999999/x", &(http_min_url){0}) == HTTP_MIN_ERR_PARSE);
    expect("reject request injection",
        http_min_parse_url("http://host/x\r\nInjected: yes", &(http_min_url){0}) == HTTP_MIN_ERR_PARSE);
    expect("reject URL fragment",
        http_min_parse_url("http://host/path#secret", &(http_min_url){0}) == HTTP_MIN_ERR_PARSE);

    {
        static const uint8_t body[] = "body";
        http_min_header injected = {"X-Language", "en\r\nInjected: yes"};
        http_min_header reserved = {"Content-Length", "99"};
        expect(
            "reject extra header injection",
            http_min_post_headers(
                "http://127.0.0.1:1/test", "application/octet-stream",
                &injected, 1, body, sizeof(body) - 1, out, sizeof(out),
                &out_len, &status, 100) == HTTP_MIN_ERR_ARGUMENT);
        expect(
            "reject framing header override",
            http_min_post_headers(
                "http://127.0.0.1:1/test", "application/octet-stream",
                &reserved, 1, body, sizeof(body) - 1, out, sizeof(out),
                &out_len, &status, 100) == HTTP_MIN_ERR_ARGUMENT);
        expect("reject GET header injection before connection",
            http_min_get_headers("http://127.0.0.1:1/test", &injected, 1u,
                out, sizeof(out), &out_len, &status, 100) == HTTP_MIN_ERR_ARGUMENT);
        expect("reject GET framing override before connection",
            http_min_get_headers("http://127.0.0.1:1/test", &reserved, 1u,
                out, sizeof(out), &out_len, &status, 100) == HTTP_MIN_ERR_ARGUMENT);
        expect("reject missing GET header storage",
            http_min_get_headers("http://127.0.0.1:1/test", NULL, 1u,
                out, sizeof(out), &out_len, &status, 100) == HTTP_MIN_ERR_ARGUMENT);
        expect("reject excessive GET headers before connection",
            http_min_get_headers("http://127.0.0.1:1/test", &injected,
                SIZE_MAX,
                out, sizeof(out), &out_len, &status, 100) == HTTP_MIN_ERR_ARGUMENT);
    }

    {
        atomic_int cancel;
        atomic_init(&cancel, 0);
        rc = run_case_ex(
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
            0, out, sizeof(out), &out_len, &status, &cancel, 100000000L);
        expect("cancel blocked response", rc == HTTP_MIN_ERR_CANCELED);
    }

    {
        stream_capture capture;
        memset(&capture, 0, sizeof(capture));
        rc = run_stream_case(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n",
            &capture, sizeof(capture.data), &out_len, &status);
        expect(
            "stream decoded body",
            rc == HTTP_MIN_OK && status == 200 && out_len == 5 &&
            capture.len == 5 && capture.calls > 0 &&
            capture.attempts == capture.calls &&
            memcmp(capture.data, "hello", 5) == 0);

        memset(&capture, 0, sizeof(capture));
        rc = run_stream_case(
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
            &capture, 4, &out_len, &status);
        expect("stream whole-body bound", rc == HTTP_MIN_ERR_CAPACITY && capture.len == 0);

        memset(&capture, 0, sizeof(capture));
        capture.fail = 1;
        rc = run_stream_case(
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
            &capture, sizeof(capture.data), &out_len, &status);
        expect(
            "stream callback abort is terminal",
            rc == HTTP_MIN_ERR_CALLBACK && capture.attempts == 1 &&
            capture.calls == 0 && capture.len == 0 && out_len == 0);
    }

    {
        reuse_server server;
        http_min_client client = {0};
        memset(&client, 0xa5, sizeof(client));
        rc = http_min_client_init(&client, "https://127.0.0.1/test");
        expect(
            "client rejects unsupported URL",
            rc == HTTP_MIN_ERR_PARSE);
        expect(
            "failed client initialization scrubs cached POST prefix",
            rc == HTTP_MIN_ERR_PARSE && !client.initialized && client.fd == -1 &&
            client.post_prefix_len == 0u && client.post_prefix[0] == '\0');
        expect(
            "uninitialized client rejects request",
            http_min_client_post(
                &client, "text/plain", (const uint8_t *)"x", 1,
                out, sizeof(out), &out_len, &status, 100) ==
                HTTP_MIN_ERR_ARGUMENT);
        expect(
            "uninitialized client rejects preconnect",
            http_min_client_warm(&client, 100) == HTTP_MIN_ERR_ARGUMENT);
        http_min_client_destroy(&client);

        expect(
            "initialize client for failed preconnect",
            http_min_client_init(&client, "http://127.0.0.1:1/test") ==
                HTTP_MIN_OK);
        expect(
            "failed preconnect preserves initialized client",
            http_min_client_warm(&client, 100) == HTTP_MIN_ERR_CONNECT &&
            client.initialized && client.fd == -1);
        http_min_client_destroy(&client);

        {
            char url[800];
            size_t used = 0u;
            size_t expected_prefix_len =
                sizeof("POST ") - 1u + 511u +
                sizeof(" HTTP/1.1\r\nHost: ") - 1u + 255u +
                sizeof(":65535") - 1u +
                sizeof(
                    "\r\nUser-Agent: voice-c-runtime/1\r\n"
                    "Accept: */*\r\nContent-Type: ") - 1u;
            memcpy(url + used, "http://", sizeof("http://") - 1u);
            used += sizeof("http://") - 1u;
            memset(url + used, 'h', 255u);
            used += 255u;
            memcpy(url + used, ":65535/", sizeof(":65535/") - 1u);
            used += sizeof(":65535/") - 1u;
            memset(url + used, 'p', 510u);
            used += 510u;
            url[used] = '\0';
            expect(
                "client caches maximum fixed POST prefix",
                http_min_client_init(&client, url) == HTTP_MIN_OK &&
                client.initialized && client.fd == -1 &&
                client.post_prefix_len == expected_prefix_len &&
                expected_prefix_len < sizeof(client.post_prefix) &&
                client.post_prefix[expected_prefix_len] == '\0');
            http_min_client_destroy(&client);
            expect(
                "client destroy scrubs cached POST prefix",
                !client.initialized && client.fd == -1 &&
                client.post_prefix_len == 0u && client.post_prefix[0] == '\0');
        }

        rc = run_reuse_case(0, REUSE_HTTP11_LENGTH, 0, 0, &server);
        expect(
            "client reuses HTTP/1.1 connection",
            rc == 0 && server.accepts == 1 && server.requests == 2 &&
            server.keep_alive_requests == 2 && server.canonical_requests == 2);
        expect(
            "persistent requests preserve cached POST prefix",
            rc == 0 && server.post_prefix_unchanged);

        rc = run_reuse_case(0, REUSE_HTTP11_CHUNKED, 1, 0, &server);
        expect(
            "stream client reuses chunked HTTP/1.1 connection",
            rc == 0 && server.accepts == 1 && server.requests == 2 &&
            server.keep_alive_requests == 2);

        rc = run_reuse_case(0, REUSE_HTTP11_PARTIAL_CONNECTION, 0, 0, &server);
        expect(
            "client rejects partial connection tokens",
            rc == 0 && server.accepts == 1 && server.requests == 2 &&
            server.keep_alive_requests == 2);

        rc = run_reuse_case(1, REUSE_HTTP11_LENGTH, 0, 0, &server);
        expect(
            "client honors connection close",
            rc == 0 && server.accepts == 2 && server.requests == 2 &&
            server.keep_alive_requests == 2);

        rc = run_reuse_case(4, REUSE_HTTP11_LENGTH, 0, 0, &server);
        expect(
            "client honors comma-delimited connection close",
            rc == 0 && server.accepts == 2 && server.requests == 2 &&
            server.keep_alive_requests == 2);

        rc = run_reuse_case(2, REUSE_HTTP11_LENGTH, 0, 0, &server);
        expect(
            "client replaces stale idle connection",
            rc == 0 && server.accepts == 2 && server.requests == 2 &&
            server.keep_alive_requests == 2);

        rc = run_reuse_case(0, REUSE_HTTP10_CLOSE, 0, 0, &server);
        expect(
            "client treats HTTP/1.0 close as default",
            rc == 0 && server.accepts == 2 && server.requests == 2);

        rc = run_reuse_case(0, REUSE_HTTP10_KEEP_ALIVE, 0, 0, &server);
        expect(
            "client honors HTTP/1.0 keep-alive",
            rc == 0 && server.accepts == 1 && server.requests == 2);

        rc = run_reuse_case(0, REUSE_HTTP11_LENGTH, 0, 1, &server);
        expect(
            "client preconnects and reuses HTTP/1.1 connection",
            rc == 0 && server.accepts == 1 && server.requests == 2 &&
            server.keep_alive_requests == 2);
    }

    if (failures) return 1;
    printf("ALL PASS http_min\n");
    return 0;
}
