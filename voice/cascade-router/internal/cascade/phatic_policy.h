#ifndef PHATIC_POLICY_H
#define PHATIC_POLICY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Match pure social/glue turns. packed:
 *   bits 0..7  = reply index (1..5) or 0 if no match
 *   bits 8..15 = reason index (1..5) matching reply family
 * Reply/reason string tables are exported for Go thin wrappers.
 *
 * Span-shaped, allocation-free. Tokenization is ASCII-oriented (STT English);
 * non-ASCII letters are treated as separators (same conservative spirit as Go).
 */
uint32_t phatic_policy_match_v1(const char *text, size_t text_len);

/* Whole-utterance admission for canned dialogue replies. Unknown content abstains. */
uint32_t phatic_policy_match_v2(const char *text, size_t text_len);

const char *phatic_policy_reply_v1(uint32_t reply_index);
const char *phatic_policy_reason_v1(uint32_t reason_index);

enum {
    PHATIC_REPLY_NONE_V1 = 0,
    PHATIC_REPLY_GREETING_V1 = 1,
    PHATIC_REPLY_CHECKIN_V1 = 2,
    PHATIC_REPLY_GRATITUDE_V1 = 3,
    PHATIC_REPLY_AFFIRMATION_V1 = 4,
    PHATIC_REPLY_GOODBYE_V1 = 5
};

#ifdef __cplusplus
}
#endif

#endif
