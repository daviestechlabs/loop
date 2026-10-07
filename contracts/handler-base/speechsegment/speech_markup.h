#ifndef HANDLER_BASE_SPEECH_MARKUP_H
#define HANDLER_BASE_SPEECH_MARKUP_H

#include <stddef.h>

enum {
    SPEECH_MARKUP_INPUT_CAP = 2048,
    SPEECH_MARKUP_SPAN_CAP = 256,
    SPEECH_MARKUP_OK = 0,
    SPEECH_MARKUP_ERR_ARGUMENT = 1,
    SPEECH_MARKUP_ERR_CAPACITY = 2
};

typedef struct {
    char pending[SPEECH_MARKUP_SPAN_CAP];
    size_t pending_len, source_bytes;
    unsigned char previous;
    int in_angle, finished;
} speech_markup_v1;

void speech_markup_init_v1(speech_markup_v1 *state);

/* Model presentation text only; never apply to audited tool receipts.
 * Remove bounded, paired word-boundary emphasis (*, **, ***, and underscore
 * equivalents), inline backticks, and escapes of these presentation markers.
 * Preserve words, numbers, literal operators, unmatched markers, and angle-tag
 * bytes. Tag admission remains the existing sanitizer's responsibility.
 * This is not a Markdown document renderer (lists, headings and links remain).
 * Pending delimiter decisions retain at most 256 bytes. Partitioning a stream
 * does not change its completed output. final flushes unmatched markers.
 * Inputs are complete UTF-8 deltas from the admitted model decoder. Total input
 * must remain below 2048 bytes. Output never grows relative to consumed input.
 * On error state is unchanged and out_len is zero. Buffers must not overlap.
 * No allocation, I/O, or clock access. Reset on cancellation or stream reuse. */
int speech_markup_feed_v1(speech_markup_v1 *state,
    const char *input, size_t input_len, int final,
    char *out, size_t out_cap, size_t *out_len);

#endif
