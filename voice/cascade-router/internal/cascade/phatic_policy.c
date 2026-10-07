// phatic_policy.c — pure C normalize + match for social/glue turns.
// Replaces the pure-Go strings.Builder normalize and contains/switch table.

#include "phatic_policy.h"

#include <string.h>

enum { PHATIC_NORMALIZED_MAX_V1 = 48 };

static const char *const phatic_replies_v1[] = {
    "",
    "Hey — I'm here. What do you need?",
    "Doing well — ready when you are.",
    "You're welcome.",
    "Got it.",
    "Talk soon.",
};

static const char *const phatic_reasons_v1[] = {
    "",
    "phatic_greeting_policy",
    "phatic_checkin_policy",
    "phatic_gratitude_policy",
    "phatic_affirmation_policy",
    "phatic_goodbye_policy",
};

/* Zero skips a byte. One emits a normalized separator. */
static const unsigned char phatic_normalized_bytes_v1[256] = {
    ['\t'] = 1u, ['\n'] = 1u, [' '] = 1u, ['!'] = 1u,
    [','] = 1u, ['.'] = 1u, [';'] = 1u,
    ['\''] = '\'', ['?'] = '?',
    ['0'] = '0', ['1'] = '1', ['2'] = '2', ['3'] = '3', ['4'] = '4',
    ['5'] = '5', ['6'] = '6', ['7'] = '7', ['8'] = '8', ['9'] = '9',
    ['A'] = 'a', ['B'] = 'b', ['C'] = 'c', ['D'] = 'd', ['E'] = 'e',
    ['F'] = 'f', ['G'] = 'g', ['H'] = 'h', ['I'] = 'i', ['J'] = 'j',
    ['K'] = 'k', ['L'] = 'l', ['M'] = 'm', ['N'] = 'n', ['O'] = 'o',
    ['P'] = 'p', ['Q'] = 'q', ['R'] = 'r', ['S'] = 's', ['T'] = 't',
    ['U'] = 'u', ['V'] = 'v', ['W'] = 'w', ['X'] = 'x', ['Y'] = 'y',
    ['Z'] = 'z',
    ['a'] = 'a', ['b'] = 'b', ['c'] = 'c', ['d'] = 'd', ['e'] = 'e',
    ['f'] = 'f', ['g'] = 'g', ['h'] = 'h', ['i'] = 'i', ['j'] = 'j',
    ['k'] = 'k', ['l'] = 'l', ['m'] = 'm', ['n'] = 'n', ['o'] = 'o',
    ['p'] = 'p', ['q'] = 'q', ['r'] = 'r', ['s'] = 's', ['t'] = 't',
    ['u'] = 'u', ['v'] = 'v', ['w'] = 'w', ['x'] = 'x', ['y'] = 'y',
    ['z'] = 'z',
};

const char *phatic_policy_reply_v1(uint32_t reply_index) {
    if (reply_index > PHATIC_REPLY_GOODBYE_V1) {
        return "";
    }
    return phatic_replies_v1[reply_index];
}

const char *phatic_policy_reason_v1(uint32_t reason_index) {
    if (reason_index > PHATIC_REPLY_GOODBYE_V1) {
        return "";
    }
    return phatic_reasons_v1[reason_index];
}

/* Normalize into caller buffer; returns length, or (size_t)-1 if overflow. */
static size_t normalize_phatic(
    const char *text,
    size_t text_len,
    char *out,
    size_t out_cap
) {
    size_t size = 0;
    int last_space = 0;
    size_t index;
    for (index = 0; index < text_len; ++index) {
        unsigned char normalized =
            phatic_normalized_bytes_v1[(unsigned char)text[index]];
        if (normalized > 1u) {
            if (size + 1 > out_cap) {
                return (size_t)-1;
            }
            out[size++] = (char)normalized;
            last_space = 0;
        } else if (normalized == 1u) {
            if (!last_space && size > 0) {
                if (size + 1 > out_cap) {
                    return (size_t)-1;
                }
                out[size++] = ' ';
                last_space = 1;
            }
        }
        /* other bytes (incl. non-ASCII) act as separators — skip */
    }
    while (size > 0 && out[size - 1] == ' ') {
        --size;
    }
    return size;
}

static int normalized_phatic_fits(const char *text, size_t text_len) {
    size_t size = 0;
    int pending_space = 0;
    size_t index;

    for (index = 0; index < text_len; ++index) {
        unsigned char normalized =
            phatic_normalized_bytes_v1[(unsigned char)text[index]];
        if (normalized > 1u) {
            size += 1u + (size_t)pending_space;
            if (size > PHATIC_NORMALIZED_MAX_V1) {
                return 0;
            }
            pending_space = 0;
        } else if (normalized == 1u && size > 0) {
            pending_space = 1;
        }
    }
    return 1;
}

static int contains_span(const char *hay, size_t hay_len, const char *needle) {
    size_t needle_len = strlen(needle);
    size_t start;
    if (needle_len == 0 || needle_len > hay_len) {
        return 0;
    }
    for (start = 0; start + needle_len <= hay_len; ++start) {
        if (memcmp(hay + start, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int eq_span(const char *hay, size_t hay_len, const char *lit) {
    size_t lit_len = strlen(lit);
    return hay_len == lit_len && memcmp(hay, lit, lit_len) == 0;
}

static uint32_t pack_match(uint32_t reply_index) {
    return reply_index | (reply_index << 8);
}

uint32_t phatic_policy_match_v2(const char *text, size_t text_len) {
    static const struct { const char *text; uint32_t reply; } phrases[] = {
        {"hey", 1u}, {"hi", 1u}, {"hello", 1u}, {"yo", 1u}, {"sup", 1u},
        {"hiya", 1u}, {"howdy", 1u}, {"whats up", 1u}, {"what's up", 1u},
        {"what is up", 1u}, {"good morning", 1u}, {"good afternoon", 1u}, {"good evening", 1u},
        {"how are you", 2u}, {"hows it going", 2u}, {"how's it going", 2u}, {"how is it going", 2u},
        {"thanks", 3u}, {"thank you", 3u}, {"thank you very much", 3u}, {"thanks a lot", 3u},
        {"thanks that was helpful", 3u}, {"thank you that was helpful", 3u}, {"ty", 3u}, {"appreciate it", 3u},
        {"ok", 4u}, {"okay", 4u}, {"got it", 4u}, {"cool", 4u}, {"alright", 4u},
        {"yes", 4u}, {"yeah", 4u}, {"yep", 4u}, {"sounds good", 4u},
        {"bye", 5u}, {"goodbye", 5u}, {"see you", 5u}, {"see you later", 5u}, {"talk to you later", 5u},
    };
    char normalized[PHATIC_NORMALIZED_MAX_V1 + 1u];
    size_t length;
    if (!text || text_len == 0u || text_len > 256u) return 0u;
    /* Do not erase quotes, controls, or unfamiliar bytes to manufacture a match. */
    for (size_t i = 0u; i < text_len; ++i)
        if (phatic_normalized_bytes_v1[(unsigned char)text[i]] == 0u) return 0u;
    length = normalize_phatic(text, text_len, normalized, sizeof(normalized));
    if (length == (size_t)-1 || length > PHATIC_NORMALIZED_MAX_V1) return 0u;
    for (size_t i = 0u; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
        if (eq_span(normalized, length, phrases[i].text)) return pack_match(phrases[i].reply);
        /* Only complete social check-ins accept a trailing question mark. */
        if (phrases[i].reply == PHATIC_REPLY_CHECKIN_V1 && length > 1u &&
            normalized[length - 1u] == '?' && eq_span(normalized, length - 1u, phrases[i].text))
            return pack_match(phrases[i].reply);
    }
    return 0u;
}

uint32_t phatic_policy_match_v1(const char *text, size_t text_len) {
    /* Keep one byte for a trailing separator that normalization removes. */
    char normalized[PHATIC_NORMALIZED_MAX_V1 + 1u];
    size_t nlen;
    int has_q;
    int short_checkin_q;

    if (text == NULL && text_len != 0) {
        return 0;
    }
    if (text_len > PHATIC_NORMALIZED_MAX_V1 &&
        !normalized_phatic_fits(text, text_len)) {
        return 0;
    }
    nlen = normalize_phatic(text, text_len, normalized, sizeof(normalized));
    if (nlen == (size_t)-1 || nlen < 2 || nlen > PHATIC_NORMALIZED_MAX_V1) {
        return 0;
    }

    has_q = contains_span(normalized, nlen, "?");
    short_checkin_q =
        contains_span(normalized, nlen, "how are you") ||
        contains_span(normalized, nlen, "hows it going") ||
        contains_span(normalized, nlen, "how is it going");
    if (has_q && !short_checkin_q) {
        return 0;
    }

    if (eq_span(normalized, nlen, "hey") || eq_span(normalized, nlen, "hi") ||
        eq_span(normalized, nlen, "hello") || eq_span(normalized, nlen, "yo") ||
        eq_span(normalized, nlen, "sup") || eq_span(normalized, nlen, "hiya") ||
        eq_span(normalized, nlen, "howdy") ||
        contains_span(normalized, nlen, "whats up") ||
        contains_span(normalized, nlen, "what is up") ||
        eq_span(normalized, nlen, "good morning") ||
        eq_span(normalized, nlen, "good afternoon") ||
        eq_span(normalized, nlen, "good evening")) {
        return pack_match(PHATIC_REPLY_GREETING_V1);
    }
    if (short_checkin_q) {
        return pack_match(PHATIC_REPLY_CHECKIN_V1);
    }
    if (contains_span(normalized, nlen, "thank") ||
        eq_span(normalized, nlen, "thanks") ||
        eq_span(normalized, nlen, "ty") ||
        eq_span(normalized, nlen, "appreciate it")) {
        return pack_match(PHATIC_REPLY_GRATITUDE_V1);
    }
    if (eq_span(normalized, nlen, "ok") || eq_span(normalized, nlen, "okay") ||
        eq_span(normalized, nlen, "got it") || eq_span(normalized, nlen, "cool") ||
        eq_span(normalized, nlen, "alright") || eq_span(normalized, nlen, "yes") ||
        eq_span(normalized, nlen, "yeah") || eq_span(normalized, nlen, "yep") ||
        contains_span(normalized, nlen, "sounds good")) {
        return pack_match(PHATIC_REPLY_AFFIRMATION_V1);
    }
    if (eq_span(normalized, nlen, "bye") ||
        contains_span(normalized, nlen, "goodbye") ||
        contains_span(normalized, nlen, "see you") ||
        contains_span(normalized, nlen, "talk to you later")) {
        return pack_match(PHATIC_REPLY_GOODBYE_V1);
    }
    return 0;
}
