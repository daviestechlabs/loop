#ifndef VOICE_C_RUNTIME_GATEWAY_HTTP_FIELDS_H
#define VOICE_C_RUNTIME_GATEWAY_HTTP_FIELDS_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

/* Parse bounded gateway header fields without temporary value copies. */
static inline int gateway_http_content_length_parse(
    const char *text,
    size_t text_len,
    size_t *out
) {
    size_t value = 0u;
    size_t i;
    if (!text || text_len == 0u || text_len >= 32u || !out) return -1;
    for (i = 0u; i < text_len; ++i) {
        unsigned digit;
        unsigned char byte = (unsigned char)text[i];
        if (byte < (unsigned char)'0' || byte > (unsigned char)'9') return -1;
        digit = (unsigned)(byte - (unsigned char)'0');
        if (value > (SIZE_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    *out = value;
    return 0;
}

static inline int gateway_http_timestamp_parse(
    const char *text,
    size_t text_len,
    int64_t *out
) {
    uint64_t value = 0u;
    uint64_t limit = (uint64_t)INT64_MAX;
    size_t i = 0u;
    int negative = 0;
    if (!text || text_len == 0u || text_len >= 32u || !out) return -1;
    if (text[i] == '+' || text[i] == '-') {
        negative = text[i] == '-';
        i++;
        if (i == text_len) return -1;
        if (negative) limit++;
    }
    for (; i < text_len; ++i) {
        unsigned digit;
        unsigned char byte = (unsigned char)text[i];
        if (byte < (unsigned char)'0' || byte > (unsigned char)'9') return -1;
        digit = (unsigned)(byte - (unsigned char)'0');
        if (value > (limit - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    if (!negative) {
        *out = (int64_t)value;
    } else if (value == (uint64_t)INT64_MAX + 1u) {
        *out = INT64_MIN;
    } else {
        *out = -(int64_t)value;
    }
    return 0;
}

static inline int gateway_http_content_type_is_protobuf(
    const char *text,
    size_t text_len
) {
    static const char expected[] = "application/x-protobuf";
    return text && text_len == sizeof(expected) - 1u &&
        strncasecmp(text, expected, text_len) == 0;
}

#endif
