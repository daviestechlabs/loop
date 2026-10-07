/* speech_sanitize.h — pure-C Orpheus emotive-tag sanitize / display strip.
 *
 * No heap. Output length is always <= input length.
 * keep_allowed=1: SanitizeSpeechForTTS (allowlist + budget 2).
 * keep_allowed=0: StripTagsForDisplay (drop all tags, collapse spaces).
 */
#ifndef HANDLER_BASE_SPEECH_SANITIZE_H
#define HANDLER_BASE_SPEECH_SANITIZE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SPEECH_SANITIZE_OK = 0,
    SPEECH_SANITIZE_ERR_ARGUMENT = 1,
    SPEECH_SANITIZE_ERR_CAPACITY = 2
};

enum {
    SPEECH_SANITIZE_MAX_EMOTIVE = 2,
    SPEECH_SANITIZE_MAX_TAG_SPAN = 24
};

/* Production Orpheus allowlist identity — must stay bound to admission report.
 * Allowlist names live in speech_sanitize.c k_allowed[] (laugh…gasp). */
#define ORPHEUS_EMOTIVE_ALLOWLIST_VERSION "orpheus-live-objective-v2"
#define ORPHEUS_EMOTIVE_ADMISSION_REPORT_SHA256 \
    "59d5dca832ede24ffb6f9169faf8184dee16f42939260024ddd2884487a46c59"

/* Return true when both sanitizer policies preserve every input byte. */
int speech_is_canonical_ascii_v1(const char *in, size_t in_len);

/*
 * Rewrite angle-bracket tag spans in [in, in+in_len).
 * On success writes to out (not necessarily NUL-terminated) and sets *out_len.
 * out_cap must be >= in_len (output never grows).
 */
int speech_sanitize_v1(
    const char *in,
    size_t in_len,
    int keep_allowed,
    char *out,
    size_t out_cap,
    size_t *out_len
);

/*
 * Apply the same policy and report a fail-closed JSON-plain proof.
 * json_plain is true only when out needs no JSON string escaping.
 */
int speech_sanitize_json_plain_v1(
    const char *in,
    size_t in_len,
    int keep_allowed,
    char *out,
    size_t out_cap,
    size_t *out_len,
    int *json_plain
);

/*
 * Strip all tag-like markup without collapsing whitespace (streaming deltas).
 * Output length always <= input; out_cap must be >= in_len.
 */
int speech_strip_tags_preserve_space_v1(
    const char *in,
    size_t in_len,
    char *out,
    size_t out_cap,
    size_t *out_len
);

#ifdef __cplusplus
}
#endif

#endif /* HANDLER_BASE_SPEECH_SANITIZE_H */
