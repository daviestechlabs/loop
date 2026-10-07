/* speech_display_stream.c — pure-C DisplayTextAccumulator (no heap). */

#include "speech_display_stream.h"
#include "speech_sanitize.h"

#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define DISPLAY_NOINLINE __attribute__((noinline))
#else
#define DISPLAY_NOINLINE
#endif

static int is_ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int is_ascii_speech_punct(unsigned char c) {
    return c == '.' || c == '!' || c == '?' || c == ',' || c == ';' || c == ':';
}

static int is_ascii_plain_input(unsigned char c) {
    return (c > ' ' && c < 0x7fu && c != '<') || is_ascii_space(c);
}

static void copy_suffix(
    const char *suffix,
    size_t suffix_len,
    char *out_suffix,
    size_t out_cap,
    size_t *out_len
) {
    if (out_suffix && out_cap > 0 && suffix_len > 0) {
        size_t copy = suffix_len < out_cap ? suffix_len : out_cap;
        memcpy(out_suffix, suffix, copy);
        if (out_len) *out_len = copy;
    } else if (out_len) {
        *out_len = suffix_len;
    }
}

static DISPLAY_NOINLINE int try_add_long_ascii_span(
    speech_display_acc_v1 *a,
    const char *delta,
    size_t delta_len
) {
    size_t start = 0;
    size_t end = delta_len;

    if (delta[0] == ' ') {
        start++;
    }
    if (end > start && delta[end - 1] == ' ') {
        end--;
    }
    if (start == end) {
        a->pending_space = 1;
        return 1;
    }
    if (!speech_is_canonical_ascii_v1(delta + start, end - start)) return 0;
    if ((a->pending_space || start != 0) && a->display_len > 0 &&
        !is_ascii_speech_punct((unsigned char)delta[start])) {
        a->display[a->display_len++] = ' ';
    }
    memcpy(a->display + a->display_len, delta + start, end - start);
    a->display_len += end - start;
    a->pending_space = end != delta_len;
    return 1;
}

/* Append deltas whose only whitespace is isolated ASCII space as one span. */
static int try_add_simple_ascii_span(
    speech_display_acc_v1 *a,
    const char *delta,
    size_t delta_len
) {
    size_t start = 0;
    size_t end = delta_len;
    size_t i;

    if (delta_len >= 16u) return try_add_long_ascii_span(a, delta, delta_len);
    for (i = 0; i < delta_len; i++) {
        unsigned char c = (unsigned char)delta[i];

        if (!is_ascii_plain_input(c)) {
            return 0;
        }
        if (!is_ascii_space(c)) {
            continue;
        }
        if (c != ' ' || (i > 0 && delta[i - 1] == ' ')) {
            return 0;
        }
        if (i > 0 && i + 1 < delta_len &&
            is_ascii_speech_punct((unsigned char)delta[i + 1])) {
            return 0;
        }
    }

    if (delta[0] == ' ') {
        start++;
    }
    if (end > start && delta[end - 1] == ' ') {
        end--;
    }
    if (start == end) {
        a->pending_space = 1;
        return 1;
    }
    if ((a->pending_space || start != 0) && a->display_len > 0 &&
        !is_ascii_speech_punct((unsigned char)delta[start])) {
        a->display[a->display_len++] = ' ';
    }
    memcpy(a->display + a->display_len, delta + start, end - start);
    a->display_len += end - start;
    a->pending_space = end != delta_len;
    return 1;
}

static int try_add_ascii_plain(
    speech_display_acc_v1 *a,
    const char *delta,
    size_t delta_len,
    char *out_suffix,
    size_t out_cap,
    size_t *out_len
) {
    size_t display_start = a->display_len;
    int pending_start = a->pending_space;
    size_t i;

    if (try_add_simple_ascii_span(a, delta, delta_len)) {
        copy_suffix(
            a->display + display_start,
            a->display_len - display_start,
            out_suffix,
            out_cap,
            out_len);
        if (a->display_len < SPEECH_DISPLAY_ACC_CAP) {
            a->display[a->display_len] = '\0';
        }
        return 1;
    }
    for (i = 0; i < delta_len; i++) {
        unsigned char c = (unsigned char)delta[i];
        if (!is_ascii_plain_input(c)) {
            a->display_len = display_start;
            a->pending_space = pending_start;
            return 0;
        }
        if (is_ascii_space(c)) {
            a->pending_space = 1;
            continue;
        }
        if (a->pending_space && a->display_len > 0 && !is_ascii_speech_punct(c))
            a->display[a->display_len++] = ' ';
        a->pending_space = 0;
        a->display[a->display_len++] = (char)c;
    }
    copy_suffix(
        a->display + display_start,
        a->display_len - display_start,
        out_suffix,
        out_cap,
        out_len);
    if (a->display_len < SPEECH_DISPLAY_ACC_CAP) a->display[a->display_len] = '\0';
    return 1;
}

void speech_display_acc_init_v1(speech_display_acc_v1 *a) {
    if (!a) {
        return;
    }
    a->raw_len = 0;
    a->display_len = 0;
    a->raw[0] = '\0';
    a->display[0] = '\0';
    a->ascii_plain = 1;
    a->pending_space = 0;
}

const char *speech_display_acc_text_v1(const speech_display_acc_v1 *a, size_t *out_len) {
    if (!a) {
        if (out_len) {
            *out_len = 0;
        }
        return "";
    }
    if (out_len) {
        *out_len = a->display_len;
    }
    return a->display;
}

const char *speech_display_acc_canonical_ascii_v1(
    const speech_display_acc_v1 *a,
    size_t *out_len
) {
    if (out_len) *out_len = 0;
    if (!a || !a->ascii_plain || a->display_len == 0u ||
        a->display_len >= SPEECH_DISPLAY_ACC_CAP)
        return NULL;
    if (out_len) *out_len = a->display_len;
    return a->display;
}

int speech_display_acc_add_v1(
    speech_display_acc_v1 *a,
    const char *delta,
    size_t delta_len,
    char *out_suffix,
    size_t out_cap,
    size_t *out_len
) {
    if (out_len) {
        *out_len = 0;
    }
    if (!a || (!delta && delta_len > 0)) {
        return SPEECH_DISPLAY_ERR_ARGUMENT;
    }
    if (delta_len == 0) {
        return SPEECH_DISPLAY_OK;
    }
    if (a->raw_len > SPEECH_DISPLAY_ACC_CAP ||
        delta_len > SPEECH_DISPLAY_ACC_CAP - a->raw_len) {
        return SPEECH_DISPLAY_ERR_CAPACITY;
    }

    memcpy(a->raw + a->raw_len, delta, delta_len);
    a->raw_len += delta_len;

    if (a->ascii_plain &&
        try_add_ascii_plain(a, delta, delta_len, out_suffix, out_cap, out_len))
        return SPEECH_DISPLAY_OK;

    a->ascii_plain = 0;
    {
        char next[SPEECH_DISPLAY_ACC_CAP];
        size_t next_len = 0;

        if (speech_sanitize_v1(a->raw, a->raw_len, 0, next, sizeof(next), &next_len) !=
            SPEECH_SANITIZE_OK) {
            return SPEECH_DISPLAY_ERR_ARGUMENT;
        }

        /* Fail closed if rewrite shortens/changes already emitted display prefix. */
        if (next_len < a->display_len) {
            return SPEECH_DISPLAY_OK;
        }
        if (a->display_len > 0 && memcmp(next, a->display, a->display_len) != 0) {
            return SPEECH_DISPLAY_OK;
        }

        copy_suffix(
            next + a->display_len,
            next_len - a->display_len,
            out_suffix,
            out_cap,
            out_len);

        memcpy(a->display, next, next_len);
        a->display_len = next_len;
        if (a->display_len < SPEECH_DISPLAY_ACC_CAP) {
            a->display[a->display_len] = '\0';
        }
    }
    return SPEECH_DISPLAY_OK;
}
