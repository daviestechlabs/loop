#include "speech_markup.h"
#include <stdint.h>
#include <string.h>

static int space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c >= 128;
}

static int marker(unsigned char c) { return c == '*' || c == '_' || c == '`'; }
static int escaped(unsigned char c) { return marker(c); }

static size_t run(const char *s, size_t n, size_t at) {
    size_t end = at + 1;
    while (end < n && s[end] == s[at]) ++end;
    return end - at;
}

/* Reserve one byte of the bound for right-boundary lookahead. A delimiter at
 * the end of a live delta is not yet a proved closing delimiter. */
static size_t closing(const char *s, size_t n, size_t at, size_t width, int final) {
    size_t limit = n < at + SPEECH_MARKUP_SPAN_CAP ? n : at + SPEECH_MARKUP_SPAN_CAP - 1u;
    int angle = 0;
    for (size_t j = at + width; j < limit;) {
        unsigned char c = (unsigned char)s[j];
        if (c == '<') angle = 1;
        if (angle) { if (c == '>') angle = 0; ++j; continue; }
        if (s[at] != '`' && c == '\\' && j + 1 < limit) { j += 2; continue; }
        if (s[at] != '`' && c == '`') {
            size_t ticks = run(s, n, j);
            size_t end = ticks <= 2 ? closing(s, limit, j, ticks, final && limit == n) : SIZE_MAX;
            if (end != SIZE_MAX) { j = end + ticks; continue; }
            if (ticks <= 2 && !final && n - at < SPEECH_MARKUP_SPAN_CAP) return SIZE_MAX;
        }
        if (s[j] != s[at]) { ++j; continue; }
        size_t count = run(s, n, j), end = j + count;
        if (count == width && j > at + width && end - at < SPEECH_MARKUP_SPAN_CAP &&
            !space((unsigned char)s[j - 1]) &&
            ((end < n && !word((unsigned char)s[end])) || (end == n && final))) return j;
        j = end;
    }
    return SIZE_MAX;
}

static int copy(const char *s, size_t n, char *out, size_t cap, size_t *used) {
    if (n > cap - *used) return SPEECH_MARKUP_ERR_CAPACITY;
    if (n) memcpy(out + *used, s, n);
    *used += n;
    return SPEECH_MARKUP_OK;
}

static int render(const char *s, size_t n, int final, unsigned depth,
    unsigned char *previous, int *angle, char *out, size_t cap, size_t *used, size_t *consumed) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (*angle || c == '<') {
            *angle = c != '>';
            if (copy(s + i, 1, out, cap, used)) return SPEECH_MARKUP_ERR_CAPACITY;
            *previous = c; ++i; continue;
        }
        if (c == '\\') {
            if (i + 1 == n && !final) break;
            if (i + 1 < n && escaped((unsigned char)s[i + 1])) {
                if (copy(s + i + 1, 1, out, cap, used)) return SPEECH_MARKUP_ERR_CAPACITY;
                *previous = (unsigned char)s[i + 1]; i += 2; continue;
            }
        }
        if (marker(c) && !word(*previous) && *previous != c && depth < 8) {
            size_t width = run(s, n, i);
            size_t max_width = c == '`' ? 2u : 3u;
            if (width <= max_width && i + width == n && !final) break;
            if (width <= max_width && i + width < n && !space((unsigned char)s[i + width])) {
                size_t end = closing(s, n, i, width, final);
                if (end != SIZE_MAX) {
                    if (c == '`') {
                        if (copy(s + i + width, end - i - width, out, cap, used)) return SPEECH_MARKUP_ERR_CAPACITY;
                    } else {
                        unsigned char inner_previous = 0;
                        int inner_angle = 0;
                        size_t inner_consumed = 0;
                        int rc = render(s + i + width, end - i - width, 1, depth + 1,
                            &inner_previous, &inner_angle, out, cap, used, &inner_consumed);
                        if (rc) return rc;
                    }
                    i = end + width; *previous = c; continue;
                }
                if (!final && n - i < SPEECH_MARKUP_SPAN_CAP) break;
            }
            if (copy(s + i, width, out, cap, used)) return SPEECH_MARKUP_ERR_CAPACITY;
            *previous = c; i += width; continue;
        }
        if (copy(s + i, 1, out, cap, used)) return SPEECH_MARKUP_ERR_CAPACITY;
        *previous = c; ++i;
    }
    *consumed = i;
    return SPEECH_MARKUP_OK;
}

void speech_markup_init_v1(speech_markup_v1 *state) {
    if (state) memset(state, 0, sizeof(*state));
}

int speech_markup_feed_v1(speech_markup_v1 *state,
    const char *input, size_t input_len, int final,
    char *out, size_t out_cap, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!state || (!input && input_len) || !out || !out_len || state->finished ||
        state->pending_len > SPEECH_MARKUP_SPAN_CAP || state->pending_len > state->source_bytes ||
        state->source_bytes >= SPEECH_MARKUP_INPUT_CAP ||
        input_len >= SPEECH_MARKUP_INPUT_CAP - state->source_bytes ||
        (input_len && memchr(input, 0, input_len))) return SPEECH_MARKUP_ERR_ARGUMENT;
    speech_markup_v1 next = *state;
    char joined[SPEECH_MARKUP_INPUT_CAP];
    size_t length = next.pending_len + input_len, used = 0, consumed = 0;
    if (next.pending_len) memcpy(joined, next.pending, next.pending_len);
    if (input_len) memcpy(joined + next.pending_len, input, input_len);
    int rc = render(joined, length, final != 0, 0, &next.previous, &next.in_angle, out, out_cap, &used, &consumed);
    if (rc) return rc;
    next.pending_len = length - consumed;
    if (next.pending_len > sizeof(next.pending)) return SPEECH_MARKUP_ERR_ARGUMENT;
    if (next.pending_len) memcpy(next.pending, joined + consumed, next.pending_len);
    next.source_bytes += input_len;
    next.finished = final != 0;
    *state = next;
    *out_len = used;
    return SPEECH_MARKUP_OK;
}
