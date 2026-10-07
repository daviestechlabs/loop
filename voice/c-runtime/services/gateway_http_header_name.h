#ifndef VOICE_C_RUNTIME_GATEWAY_HTTP_HEADER_NAME_H
#define VOICE_C_RUNTIME_GATEWAY_HTTP_HEADER_NAME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum gateway_http_header_field {
    GH_HEADER_UNKNOWN = 0u,
    GH_HEADER_TRANSFER_ENCODING = 1u << 0,
    GH_HEADER_CONTENT_LENGTH = 1u << 1,
    GH_HEADER_CONTENT_TYPE = 1u << 2,
    GH_HEADER_AUTH_USER = 1u << 3,
    GH_HEADER_AUTH_TIMESTAMP = 1u << 4,
    GH_HEADER_AUTH_NONCE = 1u << 5,
    GH_HEADER_AUTH_SIGNATURE = 1u << 6,
};

static inline int gateway_http_header_name_equal_lower(
    const char *name,
    const char *lower_name,
    size_t name_len
) {
    static const uint64_t lowercase_mask = UINT64_C(0x2020202020202020);
    while (name_len >= sizeof(uint64_t)) {
        uint64_t actual;
        uint64_t expected;
        memcpy(&actual, name, sizeof(actual));
        memcpy(&expected, lower_name, sizeof(expected));
        if ((actual | lowercase_mask) != expected) return 0;
        name += sizeof(uint64_t);
        lower_name += sizeof(uint64_t);
        name_len -= sizeof(uint64_t);
    }
    while (name_len != 0u) {
        if (((unsigned char)*name | 0x20u) != (unsigned char)*lower_name)
            return 0;
        name++;
        lower_name++;
        name_len--;
    }
    return 1;
}

/* The caller validates every byte as an HTTP field-name token first. */
static inline unsigned gateway_http_validated_header_field(
    const char *name,
    size_t name_len
) {
    unsigned char first;
    if (!name || name_len == 0u) return GH_HEADER_UNKNOWN;
    first = (unsigned char)name[0] | 0x20u;
    switch (name_len) {
    case sizeof("content-type") - 1u:
        if (first == (unsigned char)'c' &&
            gateway_http_header_name_equal_lower(
                name, "content-type", name_len))
            return GH_HEADER_CONTENT_TYPE;
        if (first == (unsigned char)'x' &&
            gateway_http_header_name_equal_lower(
                name, "x-voice-user", name_len))
            return GH_HEADER_AUTH_USER;
        break;
    case sizeof("x-voice-nonce") - 1u:
        if (first == (unsigned char)'x' &&
            gateway_http_header_name_equal_lower(
                name, "x-voice-nonce", name_len))
            return GH_HEADER_AUTH_NONCE;
        break;
    case sizeof("content-length") - 1u:
        if (first == (unsigned char)'c' &&
            gateway_http_header_name_equal_lower(
                name, "content-length", name_len))
            return GH_HEADER_CONTENT_LENGTH;
        break;
    case sizeof("transfer-encoding") - 1u:
        if (first == (unsigned char)'t' &&
            gateway_http_header_name_equal_lower(
                name, "transfer-encoding", name_len))
            return GH_HEADER_TRANSFER_ENCODING;
        if (first == (unsigned char)'x' &&
            (((unsigned char)name[8] | 0x20u) == (unsigned char)'t') &&
            gateway_http_header_name_equal_lower(
                name, "x-voice-timestamp", name_len))
            return GH_HEADER_AUTH_TIMESTAMP;
        if (first == (unsigned char)'x' &&
            (((unsigned char)name[8] | 0x20u) == (unsigned char)'s') &&
            gateway_http_header_name_equal_lower(
                name, "x-voice-signature", name_len))
            return GH_HEADER_AUTH_SIGNATURE;
        break;
    default:
        break;
    }
    return GH_HEADER_UNKNOWN;
}

#endif
