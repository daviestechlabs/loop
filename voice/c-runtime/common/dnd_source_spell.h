#ifndef VOICE_C_DND_SOURCE_SPELL_H
#define VOICE_C_DND_SOURCE_SPELL_H

#include <stddef.h>
#include <string.h>

/* Both compilation and admission derive the header flag from the same span.
 * Optional fact extraction reads that span; it contains no named-spell facts. */
static inline unsigned char dnd_spell_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}
static inline int dnd_spell_prefix(const char *text, size_t length, const char *key) {
    size_t n = strlen(key);
    if (n > length) return 0;
    for (size_t i = 0; i < n; ++i)
        if (dnd_spell_lower((unsigned char)text[i]) != (unsigned char)key[i]) return 0;
    return 1;
}
static inline size_t dnd_spell_marker(const char *text, size_t length, size_t begin, const char *key) {
    for (size_t i = begin; i < length; ++i) {
        if (text[i] == '.' || text[i] == '!' || text[i] == '?') return length;
        if ((i == 0 || text[i - 1u] == ' ') && dnd_spell_prefix(text + i, length - i, key)) return i;
    }
    return length;
}
static inline int dnd_source_spell_header(const char *text, size_t length) {
    static const char *const schools[] = {
        "abjuration", "conjuration", "divination", "enchantment",
        "evocation", "illusion", "necromancy", "transmutation"
    };
    static const char *const ordinals[] = {"st", "nd", "rd", "th", "th", "th", "th", "th", "th"};
    static const char *const fields[] = {"casting time: ", "range: ", "components: ", "duration: "};
    if (!text || !length || length > DND_SOURCE_PAGE_CAP) return 0;
    size_t start = 0, school_end = 0;
    int leveled = 0;
    if (text[0] >= '1' && text[0] <= '9') {
        size_t at = 1u;
        /* OCR can separate the digit and ordinal. Inspect original bytes;
         * never normalize the stored source or accept a mismatched suffix. */
        while (at < length && text[at] == ' ') ++at;
        if (length - at >= 9u &&
            dnd_spell_prefix(text + at, length - at, ordinals[(size_t)(text[0] - '1')]) &&
            dnd_spell_prefix(text + at + 2u, length - at - 2u, "-level ")) {
            start = at + 9u; leveled = 1;
        }
    }
    for (size_t i = 0; i < 8; ++i) {
        size_t n = strlen(schools[i]);
        if (start + n < length && dnd_spell_prefix(text + start, length - start, schools[i]) &&
            text[start + n] == ' ') { school_end = start + n + 1u; break; }
    }
    if (!school_end) return 0;
    if (!leveled) {
        if (!dnd_spell_prefix(text + school_end, length - school_end, "cantrip ")) return 0;
        school_end += 8u;
    }
    if (dnd_spell_prefix(text + school_end, length - school_end, "(ritual) ")) school_end += 9u;
    if (!dnd_spell_prefix(text + school_end, length - school_end, fields[0])) return 0;
    size_t at = school_end + strlen(fields[0]);
    for (size_t i = 1; i < 4; ++i) {
        size_t next = dnd_spell_marker(text, length, at, fields[i]);
        if (next == length || next <= at || (next == at + 1u && text[at] == ' ')) return 0;
        at = next + strlen(fields[i]);
    }
    return at < length && text[at] != ' ';
}

enum {
    DND_SPELL_LEVEL_UNKNOWN = 255,
    DND_SPELL_COST_UNSUPPORTED = 0,
    DND_SPELL_COST_ACTION = 1,
    DND_SPELL_COST_BONUS = 2,
    DND_SPELL_COST_REACTION = 3
};

typedef struct {
    unsigned int level, standard_cost, ritual_tag, conditional_reaction;
    size_t classification_begin, classification_end, time_begin, time_end;
} dnd_spell_header_facts;

/* The caller supplies an admitted, identity-bound span beginning after the
 * spell title. This function proves neither source admission nor a prior cast.
 * Offsets refer to original bytes. Standard cost excludes feature modifiers
 * and ritual casting. Unsupported costs must not become a guessed action.
 * Reaction cost does not establish that its trigger occurred. */
static inline int dnd_source_spell_facts(
    const char *text, size_t length, dnd_spell_header_facts *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->level = DND_SPELL_LEVEL_UNKNOWN;
    if (!text || !length || length > DND_SOURCE_PAGE_CAP ||
        memchr(text, 0, length) || !dnd_source_spell_header(text, length)) return 0;
    dnd_spell_header_facts result = {0};
    size_t casting = dnd_spell_marker(text, length, 0, "casting time: ");
    if (casting == length) return 0;
    result.time_begin = casting + sizeof("casting time: ") - 1u;
    result.time_end = dnd_spell_marker(text, length, result.time_begin, "range: ");
    if (result.time_end == length) return 0;
    while (result.time_end > result.time_begin && text[result.time_end - 1u] == ' ')
        --result.time_end;
    if (result.time_end == result.time_begin) return 0;
    if (text[0] >= '1' && text[0] <= '9') {
        result.level = (unsigned int)(text[0] - '0');
        size_t end = 1u;
        while (end < casting && !dnd_spell_prefix(text + end, length - end, "-level ")) ++end;
        if (end == casting) return 0;
        result.classification_end = end + sizeof("-level") - 1u;
    } else {
        result.classification_begin = dnd_spell_marker(text, length, 0, "cantrip ");
        if (result.classification_begin == length) return 0;
        result.classification_end = result.classification_begin + sizeof("cantrip") - 1u;
    }
    result.ritual_tag = casting >= sizeof("(ritual) ") - 1u &&
        dnd_spell_prefix(text + casting - (sizeof("(ritual) ") - 1u),
                        sizeof("(ritual) ") - 1u, "(ritual) ");
    size_t n = result.time_end - result.time_begin;
    const char *value = text + result.time_begin;
    if (n == sizeof("1 action") - 1u && dnd_spell_prefix(value, n, "1 action"))
        result.standard_cost = DND_SPELL_COST_ACTION;
    else if (n == sizeof("1 bonus action") - 1u && dnd_spell_prefix(value, n, "1 bonus action"))
        result.standard_cost = DND_SPELL_COST_BONUS;
    else if (n == sizeof("1 reaction") - 1u && dnd_spell_prefix(value, n, "1 reaction"))
        result.standard_cost = DND_SPELL_COST_REACTION;
    else if (n > sizeof("1 reaction, ") - 1u &&
             dnd_spell_prefix(value, n, "1 reaction, ") && value[sizeof("1 reaction, ") - 1u] != ' ') {
        result.standard_cost = DND_SPELL_COST_REACTION;
        result.conditional_reaction = 1;
    }
    *out = result;
    return 1;
}

#endif
