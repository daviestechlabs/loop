#include "../common/dnd_source_page.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    dnd_source_page_alignment out, zero = {0};
    dnd_source_page_chunk chunks[8];
    if (size < 3 || size > 65536) return 0;
    size_t count = 1u + data[0] % 8u;
    const char *page = (const char *)data + 2;
    size_t length = size - 2u;
    for (size_t i = 0; i < count; ++i) {
        size_t begin = data[(i * 2u + 1u) % size] % length;
        size_t remaining = length - begin;
        size_t n = 1u + data[(i * 2u + 2u) % size] % remaining;
        if (data[1] & 1u) {
            begin = (i * length) / count;
            n = length - begin;
            if (n > 8191u) n = 8191u;
        }
        chunks[i] = (dnd_source_page_chunk){page + begin, n, (uint32_t)i};
    }
    if (data[1] & 2u) chunks[count - 1u].chunk += 1u;
    memset(&out, 0xa5, sizeof(out));
    int rc = dnd_source_page_align(page, length, chunks, count, &out);
    if (rc) {
        if (memcmp(&out, &zero, sizeof(out))) abort();
        if (rc != DND_SOURCE_PAGE_INVALID && rc != DND_SOURCE_PAGE_INCOMPLETE &&
            rc != DND_SOURCE_PAGE_AMBIGUOUS && rc != DND_SOURCE_PAGE_CAPACITY) abort();
        return 0;
    }
    if (out.count != count || out.begin[0]) abort();
    size_t previous_end = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t begin = out.begin[i];
        if (begin > length || chunks[i].length > length - begin ||
            memcmp(page + begin, chunks[i].text, chunks[i].length)) abort();
        size_t end = begin + chunks[i].length;
        if (i && (begin < out.begin[i - 1u] || begin > previous_end || end < previous_end)) abort();
        previous_end = end;
    }
    if (previous_end != length) abort();
    return 0;
}
