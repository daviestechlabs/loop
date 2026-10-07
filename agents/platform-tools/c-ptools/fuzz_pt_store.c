#include "pt_store.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *input, *encoded;
    pt_call before, after;
    size_t length;
    if (size >= PT_RECORD_CAP || memchr(data, '\0', size)) return 0;
    input = malloc(size + 1u);
    encoded = malloc(PT_RECORD_CAP);
    if (!input || !encoded) {
        free(input);
        free(encoded);
        return 0;
    }
    memcpy(input, data, size);
    input[size] = '\0';
    if (pt_store_record_decode(input, &before) == 0) {
        length = pt_store_record_encode(encoded, PT_RECORD_CAP, &before);
        if (!length || pt_store_record_decode(encoded, &after) != 0 ||
            memcmp(&before, &after, sizeof(before)) != 0) abort();
        if (pt_store_record_encode(encoded, length, &before) != 0) abort();
    }
    free(input);
    free(encoded);
    return 0;
}
