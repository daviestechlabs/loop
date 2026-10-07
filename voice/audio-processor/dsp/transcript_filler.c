// transcript_filler.c — pure C Whisper filler classification (ASCII hot path).
// Replaces the pure-Go byte normalize + switch table on the publication path.

#include "transcript_filler.h"

#include <string.h>

static int is_common_whisper_filler(const char *normalized, size_t len) {
    /* Exact corpus mirror of the former Go switch (space-normalized lowercase). */
    static const char *const fillers[] = {
        "thank you",
        "thank you again",
        "thank you very much",
        "thanks",
        "thanks again",
        "thank you for watching",
        "thanks for watching",
        "thanks for listening",
        "so",
        "so lets go",
        "see you next time",
        "ill see you next time",
        "i ll see you next time",
    };
    size_t i;
    for (i = 0; i < sizeof(fillers) / sizeof(fillers[0]); ++i) {
        size_t flen = strlen(fillers[i]);
        if (flen == len && memcmp(normalized, fillers[i], len) == 0) {
            return 1;
        }
    }
    return 0;
}

uint32_t transcript_classify_filler_v1(const char *text, size_t text_len) {
    char normalized[32];
    size_t size = 0;
    int in_word = 0;
    size_t index;

    if (text == NULL && text_len != 0) {
        return 0; /* unhandled / invalid */
    }
    if (text_len == 0) {
        return TRANSCRIPT_FILLER_EMPTY_V1 | TRANSCRIPT_FILLER_HANDLED_V1;
    }

    for (index = 0; index < text_len; ++index) {
        unsigned char character = (unsigned char)text[index];
        int alphanumeric;
        if (character >= 0x80u) {
            return 0; /* not handled — Unicode fallback */
        }
        alphanumeric = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9');
        if (!alphanumeric) {
            in_word = 0;
            continue;
        }
        if (!in_word && size > 0) {
            if (size == sizeof(normalized)) {
                return TRANSCRIPT_FILLER_HANDLED_V1; /* oversize → not filler */
            }
            normalized[size++] = ' ';
        }
        in_word = 1;
        if (size == sizeof(normalized)) {
            return TRANSCRIPT_FILLER_HANDLED_V1;
        }
        if (character >= 'A' && character <= 'Z') {
            character = (unsigned char)(character + ('a' - 'A'));
        }
        normalized[size++] = (char)character;
    }

    if (size == 0) {
        return TRANSCRIPT_FILLER_EMPTY_V1 | TRANSCRIPT_FILLER_HANDLED_V1;
    }
    if (is_common_whisper_filler(normalized, size)) {
        return TRANSCRIPT_FILLER_IS_FILLER_V1 | TRANSCRIPT_FILLER_HANDLED_V1;
    }
    return TRANSCRIPT_FILLER_HANDLED_V1;
}
