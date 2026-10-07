#ifndef VOICE_C_DND_CLAIM_EVIDENCE_H
#define VOICE_C_DND_CLAIM_EVIDENCE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "utf8.h"

#define DND_CLAIM_TEXT_CAP 16384u
#define DND_CLAIM_CONTEXT_CAP 4096u
#define DND_CLAIM_QUOTE_CAP 2048u

typedef enum {
    DND_CLAIM_INVALID = 0,
    DND_CLAIM_BOUND = 1,
    DND_CLAIM_CONTEXT_MISSING = -1,
    DND_CLAIM_CONTEXT_AMBIGUOUS = -2,
    DND_CLAIM_QUOTE_MISSING = -3,
    DND_CLAIM_QUOTE_AMBIGUOUS = -4
} dnd_claim_evidence_status;

typedef struct {
    size_t context_begin;
    size_t context_end;
    size_t quote_begin;
    size_t quote_end;
} dnd_claim_evidence_span;

/* Count overlapping matches. Stop after the second match, never choose one. */
static inline unsigned dnd_claim_unique(const unsigned char *text, size_t length,
                                        const unsigned char *quote, size_t quote_length,
                                        size_t *begin) {
    unsigned count = 0;
    if (!quote_length || quote_length > length) return 0;
    for (size_t i = 0; i <= length - quote_length; ++i) {
        if (memcmp(text + i, quote, quote_length) != 0) continue;
        *begin = i;
        if (++count == 2u) return 2u;
    }
    return count;
}

/* Explicit context scopes a quote. Empty context means the complete utterance.
 * This kernel proves byte membership, not entailment, correction relevance,
 * actor authority, or permission. Callers must bind an immutable utterance.
 * The output storage must not alias the input spans. */
static inline dnd_claim_evidence_status dnd_claim_evidence_resolve(
        const unsigned char *text, size_t length,
        const unsigned char *context, size_t context_length,
        const unsigned char *quote, size_t quote_length,
        dnd_claim_evidence_span *out) {
    if (!out) return DND_CLAIM_INVALID;
    memset(out, 0, sizeof(*out));
    if (!text || !length || length > DND_CLAIM_TEXT_CAP ||
        !quote || !quote_length || quote_length > DND_CLAIM_QUOTE_CAP ||
        context_length > DND_CLAIM_CONTEXT_CAP || (!context && context_length) ||
        !utf8_validate_v1(text, length) || !utf8_validate_v1(quote, quote_length) ||
        !utf8_validate_v1(context, context_length) ||
        memchr(text, 0, length) || memchr(quote, 0, quote_length) ||
        (context_length && memchr(context, 0, context_length))) return DND_CLAIM_INVALID;

    size_t context_begin = 0;
    size_t context_end = length;
    if (context_length) {
        unsigned matches = dnd_claim_unique(text, length, context, context_length, &context_begin);
        if (matches == 0u) return DND_CLAIM_CONTEXT_MISSING;
        if (matches > 1u) return DND_CLAIM_CONTEXT_AMBIGUOUS;
        context_end = context_begin + context_length;
    }
    size_t local_begin = 0;
    unsigned matches = dnd_claim_unique(text + context_begin, context_end - context_begin,
                                        quote, quote_length, &local_begin);
    if (matches == 0u) return DND_CLAIM_QUOTE_MISSING;
    if (matches > 1u) return DND_CLAIM_QUOTE_AMBIGUOUS;
    out->context_begin = context_begin;
    out->context_end = context_end;
    out->quote_begin = context_begin + local_begin;
    out->quote_end = out->quote_begin + quote_length;
    return DND_CLAIM_BOUND;
}

#endif
