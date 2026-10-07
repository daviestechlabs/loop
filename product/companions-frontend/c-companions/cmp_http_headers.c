#include "cmp_http_headers.h"

#include <string.h>

static int header_name_char(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '!' || byte == '#' ||
           byte == '$' || byte == '%' || byte == '&' || byte == '\'' ||
           byte == '*' || byte == '+' || byte == '-' || byte == '.' ||
           byte == '^' || byte == '_' || byte == '`' || byte == '|' ||
           byte == '~';
}

static unsigned char ascii_lower(unsigned char byte)
{
    if (byte >= 'A' && byte <= 'Z') return (unsigned char)(byte + ('a' - 'A'));
    return byte;
}

static uint64_t name_hash_n(const char *name, size_t name_len)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    for (index = 0; index < name_len; ++index) {
        hash ^= (uint64_t)ascii_lower((unsigned char)name[index]);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int name_equal(
    const cmp_http_header_field *field,
    const char *name,
    size_t name_len,
    uint64_t name_hash
)
{
    size_t index;
    if (!field || !name || field->name_len != name_len ||
        field->name_hash != name_hash) return 0;
    for (index = 0; index < name_len; ++index) {
        if (ascii_lower((unsigned char)field->name[index]) !=
            ascii_lower((unsigned char)name[index])) return 0;
    }
    return 1;
}

static int find_crlf(
    const char *request,
    size_t request_head_len,
    size_t start,
    size_t *line_end
)
{
    size_t index;
    if (!request || !line_end || start >= request_head_len) return 0;
    for (index = start; index < request_head_len; ++index) {
        if (request[index] == '\n') return 0;
        if (request[index] != '\r') continue;
        if (index + 1u >= request_head_len || request[index + 1u] != '\n')
            return 0;
        *line_end = index;
        return 1;
    }
    return 0;
}

size_t cmp_http_head_length(
    const char *buffer,
    size_t buffer_len,
    size_t scan_from
)
{
    size_t index;
    if (!buffer || buffer_len < 4u) return 0;
    if (scan_from > buffer_len) scan_from = 0;
    index = scan_from > 3u ? scan_from - 3u : 0u;
    for (; index <= buffer_len - 4u; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1u] == '\n' &&
            buffer[index + 2u] == '\r' && buffer[index + 3u] == '\n')
            return index + 4u;
    }
    return 0;
}

int cmp_http_headers_parse(
    const char *request,
    size_t request_head_len,
    cmp_http_headers *headers
)
{
    size_t cursor;
    size_t line_end;
    size_t index;
    if (!headers) return CMP_HTTP_HEADERS_INVALID;
    headers->field_count = 0;
    if (!request || request_head_len < 6u ||
        !find_crlf(request, request_head_len, 0, &line_end) || line_end == 0)
        return CMP_HTTP_HEADERS_INVALID;
    for (index = 0; index < line_end; ++index) {
        unsigned char byte = (unsigned char)request[index];
        if (byte < 0x20u || byte > 0x7eu) return CMP_HTTP_HEADERS_INVALID;
    }
    cursor = line_end + 2u;
    for (;;) {
        size_t colon;
        size_t value;
        cmp_http_header_field *field;
        if (!find_crlf(request, request_head_len, cursor, &line_end))
            return CMP_HTTP_HEADERS_INVALID;
        if (line_end == cursor) {
            return line_end + 2u == request_head_len
                ? CMP_HTTP_HEADERS_OK : CMP_HTTP_HEADERS_INVALID;
        }
        if (headers->field_count >= CMP_HTTP_HEADER_FIELDS_MAX)
            return CMP_HTTP_HEADERS_TOO_MANY;
        colon = cursor;
        while (colon < line_end && request[colon] != ':') ++colon;
        if (colon == cursor || colon == line_end) return CMP_HTTP_HEADERS_INVALID;
        for (index = cursor; index < colon; ++index) {
            if (!header_name_char((unsigned char)request[index]))
                return CMP_HTTP_HEADERS_INVALID;
        }
        value = colon + 1u;
        while (value < line_end &&
               (request[value] == ' ' || request[value] == '\t')) ++value;
        for (index = value; index < line_end; ++index) {
            unsigned char byte = (unsigned char)request[index];
            if (byte != '\t' && (byte < 0x20u || byte > 0x7eu))
                return CMP_HTTP_HEADERS_INVALID;
        }
        field = &headers->fields[headers->field_count++];
        field->name = request + cursor;
        field->name_len = colon - cursor;
        field->value = request + value;
        field->value_len = line_end - value;
        field->name_hash = name_hash_n(field->name, field->name_len);
        cursor = line_end + 2u;
    }
}

size_t cmp_http_headers_count(
    const cmp_http_headers *headers,
    const char *name
)
{
    size_t count = 0;
    size_t index;
    size_t name_len;
    uint64_t name_hash;
    if (!headers || !name || !name[0]) return 0;
    name_len = strlen(name);
    name_hash = name_hash_n(name, name_len);
    for (index = 0; index < headers->field_count; ++index) {
        if (name_equal(&headers->fields[index], name, name_len, name_hash))
            ++count;
    }
    return count;
}

int cmp_http_headers_get_unique(
    const cmp_http_headers *headers,
    const char *name,
    char *out,
    size_t out_cap
)
{
    const cmp_http_header_field *match = NULL;
    size_t matches = 0;
    size_t index;
    size_t name_len;
    uint64_t name_hash;
    if (!headers || !name || !name[0] || !out || out_cap == 0)
        return CMP_HTTP_HEADER_INVALID;
    out[0] = '\0';
    name_len = strlen(name);
    name_hash = name_hash_n(name, name_len);
    for (index = 0; index < headers->field_count; ++index) {
        if (!name_equal(&headers->fields[index], name, name_len, name_hash))
            continue;
        if (matches == 0) match = &headers->fields[index];
        ++matches;
    }
    if (matches == 0) return CMP_HTTP_HEADER_MISSING;
    if (matches != 1 || !match) return CMP_HTTP_HEADER_AMBIGUOUS;
    if (match->value_len >= out_cap) return CMP_HTTP_HEADER_TOO_LONG;
    memcpy(out, match->value, match->value_len);
    out[match->value_len] = '\0';
    return CMP_HTTP_HEADER_FOUND;
}
