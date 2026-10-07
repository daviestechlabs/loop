#include "loop_rules.h"
#include "utf8.h"

#include <string.h>

static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int letter(unsigned char c) {
    c = lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 128u;
}

static int phrase(const char *text, size_t length, const char *wanted) {
    size_t n = strlen(wanted);
    if (n > length) return 0;
    for (size_t i = 0; i <= length - n; ++i) {
        if ((i && letter((unsigned char)text[i - 1u])) ||
            (i + n < length && letter((unsigned char)text[i + n]))) continue;
        size_t j = 0;
        while (j < n && lower((unsigned char)text[i + j]) == (unsigned char)wanted[j]) ++j;
        if (j == n) return 1;
    }
    return 0;
}

static int game(const char *text, size_t length) {
    return phrase(text, length, "d&d") || phrase(text, length, "d and d") ||
        phrase(text, length, "dnd") || phrase(text, length, "dungeons and dragons") ||
        phrase(text, length, "dungeons & dragons");
}

static int starts(const char *text, size_t length, const char *word) {
    size_t offset = 0, n = strlen(word);
    while (offset < length && (text[offset] == ' ' || text[offset] == '\t' ||
           text[offset] == '\n')) ++offset;
    if (n > length - offset ||
        (offset + n < length && letter((unsigned char)text[offset + n]))) return 0;
    for (size_t i = 0; i < n; ++i)
        if (lower((unsigned char)text[offset + i]) != (unsigned char)word[i]) return 0;
    return 1;
}

int loop_rules_clarification(const char *text, size_t length,
                             const session_history_response_c *history) {
    static const char *const mechanics[] = {
        "spell", "spells", "cantrip", "cantrips", "damage", "attack roll",
        "saving throw", "component", "components", "material", "materials",
        "mechanic", "mechanics", "rule", "rules", "range", "area of effect",
        "spell slot", "spell slots", "concentration"
    };
    static const char *const questions[] = {
        "what", "how", "which", "look up",
        "check", "verify", "explain", "tell"
    };
    int topic, mechanical = 0, question = 0;
    if (!text || !history || !length || length >= 4096u ||
        text[length] || memchr(text, '\0', length) ||
        !utf8_validate_v1((const uint8_t *)text, length) || history->count > SESSION_HISTORY_MAX)
        return -1;
    topic = game(text, length);
    for (size_t i = 0; i < history->count; ++i) {
        const session_message_c *m = &history->messages[i];
        if (!memchr(m->role, '\0', sizeof(m->role))) return -1;
        if (strcmp(m->role, "user")) continue;
        size_t n = 0;
        while (n < sizeof(m->content) && m->content[n]) ++n;
        if (n == sizeof(m->content) || !utf8_validate_v1((const uint8_t *)m->content, n))
            return -1;
        if (game(m->content, n)) topic = 1;
    }
    if (!topic) return 0;
    for (size_t i = 0; i < sizeof(mechanics) / sizeof(mechanics[0]); ++i)
        if (phrase(text, length, mechanics[i])) mechanical = 1;
    if (!mechanical) return 0;
    question = memchr(text, '?', length) != NULL || starts(text, length, "can") ||
        starts(text, length, "does") || starts(text, length, "is") || starts(text, length, "are");
    for (size_t i = 0; i < sizeof(questions) / sizeof(questions[0]); ++i)
        if (phrase(text, length, questions[i])) question = 1;
    return question;
}
