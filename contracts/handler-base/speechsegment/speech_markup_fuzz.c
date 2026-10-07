#include "speech_markup.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size >= SPEECH_MARKUP_INPUT_CAP || memchr(data, 0, size)) return 0;
    char whole[SPEECH_MARKUP_INPUT_CAP], streamed[SPEECH_MARKUP_INPUT_CAP];
    speech_markup_v1 a, b;
    speech_markup_init_v1(&a); speech_markup_init_v1(&b);
    size_t expected = 0, used = 0;
    assert(!speech_markup_feed_v1(&a, (const char *)data, size, 1, whole, sizeof(whole), &expected));
    for (size_t i = 0; i < size;) {
        size_t chunk = 1u + data[i] % 17u, n = 0;
        if (chunk > size - i) chunk = size - i;
        assert(!speech_markup_feed_v1(&b, (const char *)data + i, chunk, 0, streamed + used, sizeof(streamed) - used, &n));
        used += n; i += chunk;
        assert(b.pending_len <= SPEECH_MARKUP_SPAN_CAP && used <= i);
    }
    size_t n = 0;
    assert(!speech_markup_feed_v1(&b, NULL, 0, 1, streamed + used, sizeof(streamed) - used, &n));
    used += n;
    assert(expected == used && used <= size && !memcmp(whole, streamed, used));
    /* Presentation filtering cannot remove or reorder a rule's letters or digits. */
    size_t cursor = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 128)) continue;
        while (cursor < used && (unsigned char)whole[cursor] != c) ++cursor;
        assert(cursor < used); ++cursor;
    }
    return 0;
}
