/* http_min — pure-C HTTP/1.1 client (no TLS, no curl). For in-cluster plain HTTP. */
#ifndef VOICE_C_HTTP_MIN_H
#define VOICE_C_HTTP_MIN_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HTTP_MIN_OK = 0,
    HTTP_MIN_ERR_ARGUMENT = 1,
    HTTP_MIN_ERR_CONNECT = 2,
    HTTP_MIN_ERR_IO = 3,
    HTTP_MIN_ERR_PARSE = 4,
    HTTP_MIN_ERR_CAPACITY = 5,
    HTTP_MIN_ERR_STATUS = 6,
    HTTP_MIN_ERR_CANCELED = 7,
    HTTP_MIN_ERR_CALLBACK = 8
};

typedef int (*http_min_body_callback)(const uint8_t *data, size_t len, void *user);

typedef struct http_min_url {
    char host[256];
    char path[512];
    int port;
} http_min_url;

/* One serial HTTP/1.1 connection owner. A client is not thread-safe. */
typedef struct http_min_client {
    http_min_url url;
    char post_prefix[864];
    size_t post_prefix_len;
    int fd;
    int initialized;
} http_min_client;

typedef struct http_min_header {
    const char *name;
    const char *value;
} http_min_header;

/* Parse http://host[:port]/path. Returns 0 on success. */
int http_min_parse_url(const char *url, http_min_url *out);

/* Parse one fixed endpoint. Successful responses can reuse its connection. */
int http_min_client_init(http_min_client *client, const char *url);

/* Open the fixed endpoint before its first request. Failure leaves the client
 * initialized so a later request can connect normally. */
int http_min_client_warm(http_min_client *client, int timeout_ms);

/* Close the owned connection and clear the client. */
void http_min_client_destroy(http_min_client *client);

/*
 * POST body to url. Writes response body (no headers) into out_body.
 * *out_status set to HTTP status when headers parse. timeout_ms for connect/IO.
 */
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
);

/* POST with a bounded list of validated application headers. Hop-by-hop and
 * framing headers are rejected so callers cannot create ambiguous requests. */
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
);

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
);

/* Deliver a decoded HTTP response body incrementally (content-length,
 * chunked, and close-delimited are normalized). max_body_len remains a hard
 * whole-response bound; a nonzero callback return aborts the request. */
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
);

/* Client-owned variants preserve a healthy HTTP/1.1 connection between calls. */
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
);

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
);

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
);

/* POST variant for barge-in-sensitive workers. A nonzero flag aborts bounded
 * connect/send/receive waits and returns HTTP_MIN_ERR_CANCELED. */
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
);

/* GET; same output contract. */
int http_min_get_headers(
    const char *url,
    const http_min_header *headers,
    size_t header_count,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms
);

int http_min_get(
    const char *url,
    uint8_t *out_body,
    size_t out_cap,
    size_t *out_len,
    int *out_status,
    int timeout_ms
);

#ifdef __cplusplus
}
#endif

#endif
