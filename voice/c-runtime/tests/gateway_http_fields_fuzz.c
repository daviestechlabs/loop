#include "gateway_http_fields.h"
#include "gateway_http_header_name.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

typedef struct {
    const char *name;
    size_t name_len;
    unsigned field;
} header_name_case;

static unsigned header_name_oracle(const char *name, size_t name_len) {
    static const header_name_case cases[] = {
        {"transfer-encoding", sizeof("transfer-encoding") - 1u,
         GH_HEADER_TRANSFER_ENCODING},
        {"content-length", sizeof("content-length") - 1u,
         GH_HEADER_CONTENT_LENGTH},
        {"content-type", sizeof("content-type") - 1u,
         GH_HEADER_CONTENT_TYPE},
        {"x-voice-user", sizeof("x-voice-user") - 1u, GH_HEADER_AUTH_USER},
        {"x-voice-timestamp", sizeof("x-voice-timestamp") - 1u,
         GH_HEADER_AUTH_TIMESTAMP},
        {"x-voice-nonce", sizeof("x-voice-nonce") - 1u,
         GH_HEADER_AUTH_NONCE},
        {"x-voice-signature", sizeof("x-voice-signature") - 1u,
         GH_HEADER_AUTH_SIGNATURE},
    };
    size_t i;
    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (name_len == cases[i].name_len &&
            strncasecmp(name, cases[i].name, name_len) == 0)
            return cases[i].field;
    }
    return GH_HEADER_UNKNOWN;
}

static int header_name_token_valid(const char *name, size_t name_len) {
    size_t i;
    if (!name || name_len == 0u) return 0;
    for (i = 0u; i < name_len; ++i) {
        unsigned char byte = (unsigned char)name[i];
        if (!((byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
              (unsigned)((byte | 0x20u) - (unsigned char)'a') <= 25u ||
              byte == '!' || byte == '#' || byte == '$' || byte == '%' ||
              byte == '&' || byte == '\'' || byte == '*' || byte == '+' ||
              byte == '-' || byte == '.' || byte == '^' || byte == '_' ||
              byte == '`' || byte == '|' || byte == '~'))
            return 0;
    }
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t content_length;
    int64_t timestamp;
    if (!data || size == 0u) return 0;
    switch (data[0] % 4u) {
        case 0u:
            (void)gateway_http_content_length_parse(
                (const char *)data + 1u, size - 1u, &content_length);
            break;
        case 1u:
            (void)gateway_http_timestamp_parse(
                (const char *)data + 1u, size - 1u, &timestamp);
            break;
        case 2u:
            (void)gateway_http_content_type_is_protobuf(
                (const char *)data + 1u, size - 1u);
            break;
        default: {
            unsigned actual = gateway_http_validated_header_field(
                (const char *)data + 1u, size - 1u);
            if (header_name_token_valid(
                    (const char *)data + 1u, size - 1u) &&
                actual != header_name_oracle(
                    (const char *)data + 1u, size - 1u))
                abort();
            break;
        }
    }
    return 0;
}
