/* cmp_http_headers — bounded HTTP/1 request-header directory. */
#ifndef C_COMPANIONS_CMP_HTTP_HEADERS_H
#define C_COMPANIONS_CMP_HTTP_HEADERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMP_HTTP_HEADER_FIELDS_MAX 64u

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
    uint64_t name_hash;
} cmp_http_header_field;

typedef struct {
    cmp_http_header_field fields[CMP_HTTP_HEADER_FIELDS_MAX];
    size_t field_count;
} cmp_http_headers;

enum {
    CMP_HTTP_HEADERS_OK = 0,
    CMP_HTTP_HEADERS_INVALID = -1,
    CMP_HTTP_HEADERS_TOO_MANY = -2
};

enum {
    CMP_HTTP_HEADER_MISSING = 0,
    CMP_HTTP_HEADER_FOUND = 1,
    CMP_HTTP_HEADER_AMBIGUOUS = -1,
    CMP_HTTP_HEADER_TOO_LONG = -2,
    CMP_HTTP_HEADER_INVALID = -3
};

/* Find CRLF CRLF. scan_from is the byte length checked by the prior call. */
size_t cmp_http_head_length(
    const char *buffer,
    size_t buffer_len,
    size_t scan_from
);

/* Parse one complete request head, including its final empty CRLF line. */
int cmp_http_headers_parse(
    const char *request,
    size_t request_head_len,
    cmp_http_headers *headers
);

/* Count exact case-insensitive field-name matches. */
size_t cmp_http_headers_count(
    const cmp_http_headers *headers,
    const char *name
);

/* Copy one unique field value. Duplicate names fail closed. */
int cmp_http_headers_get_unique(
    const cmp_http_headers *headers,
    const char *name,
    char *out,
    size_t out_cap
);

#ifdef __cplusplus
}
#endif

#endif
