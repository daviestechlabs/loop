#include "loop_scene.h"
#include "dnd_tools.h"
#include "utf8.h"

#include <string.h>

static int normalized(const char *text, char out[4096]) {
    size_t n = 0;
    for (size_t i = 0; text[i]; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!n || out[n - 1u] == ' ') continue;
            c = ' ';
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (n >= 4095u) return -1;
        out[n++] = (char)c;
    }
    if (n && out[n - 1u] == ' ') --n;
    out[n] = '\0';
    return 0;
}

static int verification_question(const char *question) {
    static const char *const heads[] = {
        "what do we need to ", "what do we still need to ",
        "what else do we need to ", "what else do we still need to ",
        "what should we ", "what should i ",
        "what else should we ", "what else should i "
    };
    static const char *const verbs[] = {"check", "verify", "establish", "confirm", "know"};
    static const char *const tails[] = {
        " before deciding?", " before choosing a spell?", " before casting?"
    };
    for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); ++i) {
        size_t head_length = strlen(heads[i]);
        if (strncmp(question, heads[i], head_length)) continue;
        const char *verb = question + head_length;
        for (size_t j = 0; j < sizeof(verbs) / sizeof(verbs[0]); ++j) {
            size_t verb_length = strlen(verbs[j]);
            if (strncmp(verb, verbs[j], verb_length)) continue;
            for (size_t k = 0; k < sizeof(tails) / sizeof(tails[0]); ++k)
                if (!strcmp(verb + verb_length, tails[k])) return 1;
        }
    }
    return 0;
}

static int followup(const char *text) {
    static const char *const questions[] = {
        "what's unknown before deciding?", "what is unknown before deciding?",
        "what remains unknown before deciding?", "what's still unknown before deciding?",
        "what is still unknown before deciding?", "what else should i verify before deciding?",
        "what else should i check before deciding?"
    };
    char clean[4096];
    if (normalized(text, clean)) return 0;
    const char *question = clean;
    static const char *const prefixes[] = {"do not cast anything. ", "nothing yet. "};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t n = strlen(prefixes[i]);
        if (!strncmp(question, prefixes[i], n)) question += n;
    }
    size_t n = strlen(question);
    const char *suffix = " do not cast anything.";
    size_t suffix_length = strlen(suffix);
    if (n >= suffix_length && !strcmp(question + n - suffix_length, suffix))
        clean[(size_t)(question - clean) + n - suffix_length] = '\0';
    for (size_t i = 0; i < sizeof(questions) / sizeof(questions[0]); ++i)
        if (!strcmp(question, questions[i])) return 1;
    return verification_question(question);
}

static int append(char *out, size_t capacity, size_t *used, const char *part) {
    size_t length = strlen(part);
    if (length >= capacity - *used) { out[0] = '\0'; return -1; }
    memcpy(out + *used, part, length);
    *used += length;
    out[*used] = '\0';
    return 0;
}

static int history_valid(const session_history_response_c *history) {
    if (!history || history->count > SESSION_HISTORY_MAX) return 0;
    for (size_t i = 0; i < history->count; ++i) {
        const session_message_c *m = &history->messages[i];
        if (!memchr(m->role, '\0', sizeof(m->role)) ||
            (strcmp(m->role, "user") && strcmp(m->role, "assistant"))) return 0;
        const char *end = memchr(m->content, '\0', sizeof(m->content));
        if (!end || !utf8_validate_v1((const uint8_t *)m->content,
                (size_t)(end - m->content))) return 0;
    }
    return 1;
}

typedef struct {
    dnd_scene_intent intent;
    char alternative[201];
    int uncertain;
} loop_scene_focus;

static int mixed_scene_intent(const char *text, dnd_scene_intent *intent,
    const char **narration);
static int spelling_conflict(const char *a, const char *b);
static int name_correction(const char *text, dnd_scene_intent *intent);

static int name_equal(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + ('a' - 'A'));
        if (x != y) return 0;
    }
    return *a == *b;
}

static int correction_matches(const loop_scene_focus *focus, const dnd_scene_intent *next) {
    return name_equal(next->character_name, focus->intent.character_name) ||
        (focus->alternative[0] && name_equal(next->character_name, focus->alternative));
}

/* Retain unresolved questions, not imagined facts. A conflicting name stays
 * ambiguous across abstention follow-ups until an explicit user correction.
 * Return 2 for that ambiguity; it must never become an STT spelling hint. */
static int history_focus(const session_history_response_c *history,
    loop_scene_focus *focus) {
    int have = 0;
    memset(focus, 0, sizeof(*focus));
    for (size_t i = 0; i < history->count; ++i) {
        const session_message_c *m = &history->messages[i];
        dnd_scene_intent next = {0};
        const char *narration = NULL;
        int mixed = 0;
        if (strcmp(m->role, "user")) continue;
        if (dnd_scene_parse(m->content, &next)) {
            focus->intent = next; focus->alternative[0] = '\0'; focus->uncertain = 0; have = 1;
        } else if ((mixed = mixed_scene_intent(m->content, &next, &narration))) {
            if (have && (spelling_conflict(next.character_name, focus->intent.character_name) ||
                (mixed == 2 && !name_equal(next.character_name, focus->intent.character_name))))
                memcpy(focus->alternative, focus->intent.character_name,
                    strlen(focus->intent.character_name) + 1u);
            else if (!have || !name_equal(next.character_name, focus->intent.character_name))
                focus->alternative[0] = '\0';
            focus->intent = next; focus->uncertain = mixed == 2; have = 1;
        } else if (have && name_correction(m->content, &next) && correction_matches(focus, &next)) {
            memcpy(focus->intent.character_name, next.character_name,
                strlen(next.character_name) + 1u);
            focus->alternative[0] = '\0';
            focus->uncertain = 0;
        } else if (!followup(m->content)) {
            memset(focus, 0, sizeof(*focus)); have = 0;
        }
    }
    return have ? (focus->alternative[0] || focus->uncertain ? 2 : 1) : 0;
}

int loop_scene_vocabulary(const session_history_response_c *history,
    char *out, size_t capacity) {
    loop_scene_focus focus = {0};
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!history_valid(history)) return -1;
    if (history_focus(history, &focus) != 1) return 0;
    size_t n = strlen(focus.intent.character_name);
    if (!n || n > 64u) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)focus.intent.character_name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) continue;
        if (i && (c == '\'' || c == '-')) continue;
        return 0;
    }
    if (n >= capacity) return -1;
    memcpy(out, focus.intent.character_name, n + 1u);
    return 1;
}

/* Translate bounded nearby forms through the existing label validator.
 * A missing subject verb returns 2: clarification only, never narration. */
static int nearby_question(const char *text, dnd_scene_intent *intent) {
    static const char *const heads[] = {
        "i need to know if ", "i need to know whether ", "do we know if ", "is "
    };
    static const char *const tails[] = {
        " is nearby before deciding", " is nearby", "'s nearby before deciding",
        "'s nearby", " nearby before deciding", " nearby"
    };
    char clean[4096], question[256];
    size_t n = strlen(text);
    if (n >= sizeof(clean)) return 0;
    for (size_t k = 0; k <= n; ++k) {
        unsigned char c = (unsigned char)text[k];
        clean[k] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    if (n && (clean[n - 1u] == '.' || clean[n - 1u] == '?')) clean[--n] = '\0';
    if (!n) return 0;
    for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); ++i) {
        size_t head = strlen(heads[i]);
        if (strncmp(clean, heads[i], head)) continue;
        for (size_t j = 0; j < sizeof(tails) / sizeof(tails[0]); ++j) {
            size_t tail = strlen(tails[j]);
            if (n <= head + tail || strcmp(clean + n - tail, tails[j])) continue;
            if (i == 3u && j < 4u) continue;
            size_t name = n - head - tail;
            if (name > 200u) return 0;
            memcpy(question, "Is ", 3u);
            memcpy(question + 3u, text + head, name);
            memcpy(question + 3u + name, " here?", 7u);
            if (!dnd_scene_parse(question, intent)) return 0;
            return i != 3u && j >= 4u ? 2 : 1;
        }
    }
    return 0;
}

/* One ASCII edit suggests ambiguity. It does not identify either character. */
static int spelling_conflict(const char *a, const char *b) {
    char x[4096], y[4096];
    if (normalized(a, x) || normalized(b, y)) return 0;
    size_t nx = strlen(x), ny = strlen(y), i = 0, j = 0;
    unsigned edits = 0;
    if (nx < 3u || ny < 3u || nx > 64u || ny > 64u ||
        nx + 1u < ny || ny + 1u < nx || !strcmp(x, y)) return 0;
    for (size_t k = 0; k < nx; ++k) if (x[k] < 'a' || x[k] > 'z') return 0;
    for (size_t k = 0; k < ny; ++k) if (y[k] < 'a' || y[k] > 'z') return 0;
    while (i < nx && j < ny) {
        if (x[i] == y[j]) { ++i; ++j; continue; }
        if (++edits > 1u) return 0;
        if (nx >= ny) ++i;
        if (ny >= nx) ++j;
    }
    return edits + (unsigned)(i < nx || j < ny) == 1u;
}

static int name_correction(const char *text, dnd_scene_intent *intent) {
    static const char *const heads[] = {"i mean ", "i meant "};
    char clean[4096], question[256];
    size_t n = strlen(text);
    if (n >= sizeof(clean)) return 0;
    for (size_t k = 0; k <= n; ++k) {
        unsigned char c = (unsigned char)text[k];
        clean[k] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    if (n && clean[n - 1u] == '.') clean[--n] = '\0';
    for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); ++i) {
        size_t head = strlen(heads[i]);
        if (n <= head || n - head > 200u || strncmp(clean, heads[i], head)) continue;
        memcpy(question, "Is ", 3u);
        memcpy(question + 3u, text + head, n - head);
        memcpy(question + 3u + n - head, " here?", 7u);
        return dnd_scene_parse(question, intent);
    }
    return 0;
}

static int mixed_scene_intent(const char *text, dnd_scene_intent *intent,
    const char **narration) {
    char question[2048], clean[4096];
    *narration = NULL;
    size_t length = strlen(text);
    if (length >= sizeof(clean)) return 0;
    for (size_t i = 0; i <= length; ++i) {
        unsigned char c = (unsigned char)text[i];
        clean[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    const char *end = strpbrk(text, ".?");
    const char *command = strstr(clean, " describe ");
    const char *imagine = strstr(clean, " imagine ");
    if (imagine && (!command || imagine < command)) command = imagine;
    size_t q = end ? (size_t)(end - text) + 1u : length;
    const char *rest = end ? end + 1u : NULL;
    /* ASR can omit a sentence boundary. Only split before an explicit
     * imagined-narration verb; validate the complete question below. */
    if (command && (!end || (size_t)(command - clean) < (size_t)(end - text))) {
        q = (size_t)(command - clean);
        rest = text + q + 1u;
    }
    if (!rest || q >= sizeof(question)) return 0;
    while (*rest == ' ') ++rest;
    if (!*rest || normalized(rest, clean)) return 0;
    size_t n = strlen(clean);
    if (n && clean[n - 1u] == '.') clean[--n] = '\0';
    if (strpbrk(clean, ".?;\"\n") ||
        (strncmp(clean, "describe ", 9u) && strncmp(clean, "imagine ", 8u)) ||
        (!strstr(clean, " imagined ") && !strstr(clean, " fictional "))) return 0;
    memcpy(question, text, q); question[q] = '\0';
    int kind = dnd_scene_parse(question, intent) ? 1 : nearby_question(question, intent);
    if (!kind) return 0;
    *narration = rest;
    return kind;
}

static int conflict_reply(const char *name, const char *previous,
    char *out, size_t capacity) {
    size_t used = 0;
    if (append(out, capacity, &used, "I heard ") ||
        append(out, capacity, &used, name)) return -1;
    if (!previous[0]) return append(out, capacity, &used,
        ". Which character do you mean? Presence is unknown. ");
    return append(out, capacity, &used, ". Do you mean ") ||
        append(out, capacity, &used, previous) ||
        append(out, capacity, &used, " or a different character? Presence is unknown. ") ? -1 : 0;
}

int loop_scene_mixed_reply(const char *text, size_t length,
    const session_history_response_c *history, char *out, size_t capacity,
    const char **narration) {
    dnd_scene_intent intent = {0};
    loop_scene_focus focus = {0};
    size_t used = 0;
    if (!out || !capacity || !narration) return -1;
    out[0] = '\0'; *narration = NULL;
    if (!text || !length || length >= 4096u || text[length] ||
        memchr(text, '\0', length) || !utf8_validate_v1((const uint8_t *)text, length) ||
        !history_valid(history)) return -1;
    const char *rest = NULL;
    int mixed = mixed_scene_intent(text, &intent, &rest);
    if (!mixed) return 0;
    int prior = history_focus(history, &focus);
    const char *previous = focus.intent.character_name;
    int conflict = mixed == 2 || (prior && spelling_conflict(intent.character_name, previous));
    if (!conflict && prior == 2 && name_equal(intent.character_name, previous)) {
        previous = focus.alternative; conflict = 1;
    }
    if (conflict) {
        if (conflict_reply(intent.character_name, previous, out, capacity)) return -1;
        used = strlen(out);
    } else {
        if (append(out, capacity, &used, intent.character_name) ||
            append(out, capacity, &used, "'s presence is unknown. ")) return -1;
    }
    if (intent.proposed_spell[0] &&
        (append(out, capacity, &used, intent.proposed_spell) ||
         append(out, capacity, &used, " remains a proposal. "))) return -1;
    if (!conflict) *narration = rest;
    return conflict ? 2 : 1;
}

int loop_scene_reply(const char *text, size_t length,
    const session_history_response_c *history, char *out, size_t capacity) {
    dnd_scene_intent intent = {0};
    loop_scene_focus focus = {0};
    size_t used = 0;
    int continued = 0;
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!text || !length || length >= 4096u || text[length] ||
        memchr(text, '\0', length) || !utf8_validate_v1((const uint8_t *)text, length) ||
        !history_valid(history)) return -1;
    if (!dnd_scene_parse(text, &intent)) {
        int prior = history_focus(history, &focus);
        dnd_scene_intent correction = {0};
        if (!followup(text) && (!prior || !name_correction(text, &correction) ||
            !correction_matches(&focus, &correction))) return 0;
        if (!prior) return 0;
        if (correction.character_name[0]) {
            memcpy(focus.intent.character_name, correction.character_name,
                strlen(correction.character_name) + 1u);
            focus.alternative[0] = '\0';
            focus.uncertain = 0;
        } else if (prior == 2) {
            if (conflict_reply(focus.intent.character_name, focus.alternative, out, capacity)) return -1;
            return 1;
        }
        continued = 1;
        intent = focus.intent;
    }
    if (continued) {
        if (append(out, capacity, &used, intent.character_name) ||
            append(out, capacity, &used, "'s presence is still unknown. Verify your resources and nearby allies before deciding.")) return -1;
    } else {
        if (append(out, capacity, &used, "I cannot confirm whether ") ||
            append(out, capacity, &used, intent.character_name) ||
            append(out, capacity, &used, " is still here. ")) return -1;
        if (intent.proposed_spell[0] &&
            (append(out, capacity, &used, intent.proposed_spell) ||
             append(out, capacity, &used, " remains a proposal. "))) return -1;
        if (append(out, capacity, &used, "What can you observe?")) return -1;
    }
    return 1;
}
