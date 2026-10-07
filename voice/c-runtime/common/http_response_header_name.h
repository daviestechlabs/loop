#ifndef VOICE_C_RUNTIME_HTTP_RESPONSE_HEADER_NAME_H
#define VOICE_C_RUNTIME_HTTP_RESPONSE_HEADER_NAME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    HTTP_RESPONSE_HEADER_UNKNOWN = 0,
    HTTP_RESPONSE_HEADER_CONTENT_LENGTH,
    HTTP_RESPONSE_HEADER_TRANSFER_ENCODING,
    HTTP_RESPONSE_HEADER_CONNECTION
} http_response_header_name;

static inline uint64_t http_response_header_word8(const void *value) {
    uint64_t word;
    memcpy(&word, value, sizeof(word));
    return word;
}

static inline uint16_t http_response_header_word2(const void *value) {
    uint16_t word;
    memcpy(&word, value, sizeof(word));
    return word;
}

static inline int http_response_header_eq8(
    const char *value,
    const char literal[8],
    const char ascii_case_mask[8]
) {
    return (http_response_header_word8(value) |
            http_response_header_word8(ascii_case_mask)) ==
           http_response_header_word8(literal);
}

static inline http_response_header_name http_response_header_classify(
    const char *name,
    size_t name_len
) {
    static const char content_prefix[] = "content-";
    static const char content_suffix[] = "t-length";
    static const char transfer_prefix[] = "transfer";
    static const char transfer_suffix[] = "-encodin";
    static const char connection_prefix[] = "connecti";
    static const char connection_suffix[2] = {'o', 'n'};
    static const char all_letters[8] = {
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    };
    static const char content_prefix_case[8] = {
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00,
    };
    static const char content_suffix_case[8] = {
        0x20, 0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    };
    static const char transfer_suffix_case[8] = {
        0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    };

    if (!name) return HTTP_RESPONSE_HEADER_UNKNOWN;
    switch (name_len) {
        case sizeof("Connection") - 1u:
            if (http_response_header_eq8(
                    name, connection_prefix, all_letters) &&
                (http_response_header_word2(name + 8u) |
                 UINT16_C(0x2020)) ==
                    http_response_header_word2(connection_suffix))
                return HTTP_RESPONSE_HEADER_CONNECTION;
            break;
        case sizeof("Content-Length") - 1u:
            if (http_response_header_eq8(
                    name,
                    content_prefix,
                    content_prefix_case) &&
                http_response_header_eq8(
                    name + 6u,
                    content_suffix,
                    content_suffix_case))
                return HTTP_RESPONSE_HEADER_CONTENT_LENGTH;
            break;
        case sizeof("Transfer-Encoding") - 1u:
            if (http_response_header_eq8(
                    name, transfer_prefix, all_letters) &&
                http_response_header_eq8(
                    name + 8u,
                    transfer_suffix,
                    transfer_suffix_case) &&
                (((unsigned char)name[16] | 0x20u) == (unsigned char)'g'))
                return HTTP_RESPONSE_HEADER_TRANSFER_ENCODING;
            break;
        default:
            break;
    }
    return HTTP_RESPONSE_HEADER_UNKNOWN;
}

#endif
