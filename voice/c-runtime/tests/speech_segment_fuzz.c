#include "speech_segment.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void spans(const char *text, size_t length, const speech_seg_config_v1 *cfg) {
    size_t so, sl, ro, rl;
    int rc = speech_seg_next_v1(text, length, cfg, &so, &sl, &ro, &rl);
    assert(rc == SPEECH_SEG_OK || rc == SPEECH_SEG_NO_CUT);
    assert(so <= length && sl <= length - so && ro <= length && rl == length - ro);
    if (rc == SPEECH_SEG_OK) assert(sl && so + sl <= ro);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char words[1024];
    speech_seg_config_v1 cfg = {0, 3, 80};
    size_t so, sl, ro, rl;
    if (size < 2 || size > sizeof(words)) return 0;
    cfg.max_segment_chars = 1 + data[0] % 100;
    cfg.min_segment_chars = 1 + data[1] % cfg.max_segment_chars;
    spans((const char *)data, size, &cfg);
    /* Word-only input gives an independent boundary invariant, without tags
     * or punctuation taking precedence. Whitespace is never source content. */
    for (size_t i = 0; i < size; ++i)
        words[i] = data[i] % 5u ? (char)('a' + data[i] % 26u) : ' ';
    words[0] = 'a';
    int rc = speech_seg_next_v1(words, size, &cfg, &so, &sl, &ro, &rl);
    spans(words, size, &cfg);
    if (rc == SPEECH_SEG_OK) {
        assert(so == 0 && sl <= (size_t)cfg.max_segment_chars);
        for (size_t i = sl; i < ro; ++i) assert(words[i] == ' ');
        int separator = 0;
        size_t limit = (size_t)cfg.max_segment_chars;
        for (size_t i = (size_t)cfg.min_segment_chars; i < limit && i < size; ++i)
            if (words[i] == ' ') separator = 1;
        if (separator) assert(sl < size && words[sl] == ' ');
    }
    return 0;
}
