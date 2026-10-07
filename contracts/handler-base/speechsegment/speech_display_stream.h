/* speech_display_stream.h — pure-C stable display deltas from spoken stream.
 *
 * Re-evaluates the bounded accumulated spoken response so a tag split across
 * token deltas is never exposed as a partial literal (fail-closed).
 * No heap. Fixed capacity.
 */
#ifndef HANDLER_BASE_SPEECH_DISPLAY_STREAM_H
#define HANDLER_BASE_SPEECH_DISPLAY_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SPEECH_DISPLAY_ACC_CAP = 8192,
    SPEECH_DISPLAY_OK = 0,
    SPEECH_DISPLAY_ERR_ARGUMENT = 1,
    SPEECH_DISPLAY_ERR_CAPACITY = 2
};

typedef struct speech_display_acc_v1 {
    char raw[SPEECH_DISPLAY_ACC_CAP];
    size_t raw_len;
    char display[SPEECH_DISPLAY_ACC_CAP];
    size_t display_len;
    int ascii_plain;
    int pending_space;
} speech_display_acc_v1;

void speech_display_acc_init_v1(speech_display_acc_v1 *a);

/*
 * Append spoken-channel delta. On success writes only the newly stable,
 * tag-free display suffix to out_suffix (not necessarily NUL-terminated).
 * If malformed markup would rewrite an already emitted prefix, returns OK
 * with *out_len=0 (fail closed, wait for completion).
 */
int speech_display_acc_add_v1(
    speech_display_acc_v1 *a,
    const char *delta,
    size_t delta_len,
    char *out_suffix,
    size_t out_cap,
    size_t *out_len
);

/* Stable display text so far (not necessarily NUL-terminated). */
const char *speech_display_acc_text_v1(const speech_display_acc_v1 *a, size_t *out_len);

/* Return NUL-terminated stable text only when it proves canonical ASCII. */
const char *speech_display_acc_canonical_ascii_v1(
    const speech_display_acc_v1 *a,
    size_t *out_len
);

#ifdef __cplusplus
}
#endif

#endif /* HANDLER_BASE_SPEECH_DISPLAY_STREAM_H */
