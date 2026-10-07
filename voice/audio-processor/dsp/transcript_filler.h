#ifndef TRANSCRIPT_FILLER_H
#define TRANSCRIPT_FILLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Classify a transcript for Whisper-filler publication policy.
 * packed bits:
 *   bit0 = empty (no alphanumerics)
 *   bit1 = filler (known Whisper outro/thanks corpus)
 *   bit2 = handled (ASCII path fully decided; 0 means caller must use Unicode)
 *
 * Span-shaped: one call over the full UTF-8 byte span. No allocation.
 * Non-ASCII bytes (top bit set) set handled=0 so Go can run Unicode fallback.
 */
uint32_t transcript_classify_filler_v1(const char *text, size_t text_len);

enum {
    TRANSCRIPT_FILLER_EMPTY_V1 = 1u << 0,
    TRANSCRIPT_FILLER_IS_FILLER_V1 = 1u << 1,
    TRANSCRIPT_FILLER_HANDLED_V1 = 1u << 2
};

#ifdef __cplusplus
}
#endif

#endif
