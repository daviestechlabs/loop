#include "http_response_header_name.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

static http_response_header_name response_header_name_oracle(
    const char *name,
    size_t name_len
) {
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const char *name = (const char *)data;
    if (http_response_header_classify(name, size) !=
        response_header_name_oracle(name, size))
        abort();
    return 0;
}
