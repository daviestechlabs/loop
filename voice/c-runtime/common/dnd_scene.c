/* Bounded presence questions and signed scene-result projection. No I/O. */
#define _POSIX_C_SOURCE 200809L
#include "dnd_tools.h"
#include "cmp_json.h"
#include "utf8.h"

#include <stdio.h>
#include <string.h>

static unsigned char lower_ascii(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}
static int prefix(const char *text, const char *word) {
    for (; *word; ++word, ++text)
        if (!*text || lower_ascii((unsigned char)*text) != (unsigned char)*word) return 0;
    return 1;
}
/* Reserved operators cannot form a label in this bounded speech grammar.
 * This is deliberately narrower than campaign storage labels. */
static int name_valid(const char *text) {
    static const char *const operators[] = {
        "not", "no", "longer", "and", "or", "is", "are", "was", "were",
        "if", "unless", "when", "while", "then", "here", "still", "wait",
        "out", "inside", "outside", "within", "near", "behind", "over", "under", "safe"
    };
    if (!dnd_campaign_label(text, 201u, 1)) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < 128u && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == ' ' || *p == '-' || *p == '\'')) return 0;
        if (p != (const unsigned char *)text && p[-1] != ' ') continue;
        for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i) {
            size_t length = strlen(operators[i]);
            if (prefix((const char *)p, operators[i]) && (!p[length] || p[length] == ' ')) return 0;
        }
    }
    return 1;
}

typedef struct {
    const char *prefix;
    int subject_first;
} presence_form;

static const presence_form *presence_start(const char **text) {
    static const presence_form forms[] = {
        {"is ", 0}, {"can you tell me whether ", 1}, {"check whether ", 1},
        {"do we know if ", 1}, {"can you check if ", 1}, {"tell me whether ", 1}
    };
    if (prefix(*text, "please ")) *text += 7u;
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); ++i) {
        if (prefix(*text, forms[i].prefix)) {
            *text += strlen(forms[i].prefix);
            return &forms[i];
        }
    }
    return NULL;
}

static size_t repair_separator(const char *text) {
    if (*text == ' ' || *text == ',' || *text == '-') return 1u;
    /* Normalize an em dash only at the repair boundary, never inside names. */
    return !strncmp(text, "\xe2\x80\x94", 3u) ? 3u : 0u;
}

static int presence_name(const char *text, char name[201]) {
    static const char *const endings[] = {
        " still in the room", " in the room", " still in this room", " in this room",
        " still in the scene", " in the scene", " still here", " here"
    };
    const presence_form *form = presence_start(&text);
    if (!form) return 0;
    /* This tool accepts an explicit character label, not a pronoun or a
     * deictic description. Preserve those questions for dialogue resolution;
     * treating the entire clause as a name loses the player's actual intent. */
    static const char *const references[] = {
        "he", "she", "it", "they", "we", "you", "someone", "anyone",
        "everyone", "this", "that", "these", "those"
    };
    for (size_t i = 0; i < sizeof(references) / sizeof(references[0]); ++i) {
        size_t length = strlen(references[i]);
        if (prefix(text, references[i]) && (!text[length] || text[length] == ' ')) return 0;
    }
    size_t n = strlen(text);
    for (size_t i = 0; i < sizeof(endings) / sizeof(endings[0]); ++i) {
        size_t ending = strlen(endings[i]);
        size_t bridge = form->subject_first ? 3u : 0u;
        if (n <= ending + bridge || n - ending - bridge > 200u ||
            !prefix(text + n - ending, endings[i]) ||
            (bridge && !prefix(text + n - ending - bridge, " is"))) continue;
        memcpy(name, text, n - ending - bridge);
        name[n - ending - bridge] = '\0';
        if (name_valid(name)) return 1;
        name[0] = '\0';
        return 0;
    }
    return 0;
}

/* Only an explicit cast proposal or a presence question can precede repair.
 * Quotes, reported speech and conditional prefixes are outside this shortcut. */
static int repair_prefix(const char *start, const char *end, char spell[201]) {
    char clause[2048], ignored[201];
    size_t n = (size_t)(end - start);
    while (n && strchr(" ,-.?", start[n - 1u])) --n;
    if (!n) return 1;
    memcpy(clause, start, n);
    clause[n] = '\0';
    if (prefix(clause, "i cast ")) {
        if (!name_valid(clause + 7u)) return 0;
        memcpy(spell, clause + 7u, strlen(clause + 7u) + 1u);
        return 1;
    }
    return presence_name(clause, ignored);
}

int dnd_scene_parse(const char *text, dnd_scene_intent *intent) {
    static const char *const repairs[] = {
        "actually, wait", "actually wait", "no wait", "no, wait", "hold on", "wait"
    };
    char question[2048];
    char spell[201] = {0};
    const char *start;
    size_t n, used = 0;
    if (!intent) return 0;
    memset(intent, 0, sizeof(*intent));
    if (!text || (n = strnlen(text, sizeof(question))) == sizeof(question)) return 0;
    if (!utf8_validate_v1((const uint8_t *)text, n)) return 0;
    /* STT punctuation and whitespace do not change the bounded question form. */
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (c < 32u || c == 127u) return 0;
        if (c == ' ' && (!used || question[used - 1u] == ' ')) continue;
        question[used++] = (char)c;
    }
    while (used && question[used - 1u] == ' ') --used;
    if (used && (question[used - 1u] == '?' || question[used - 1u] == '.')) --used;
    while (used && question[used - 1u] == ' ') --used;
    question[used] = '\0';
    start = question;
    /* A future cast plan can qualify a current-presence question. It is not
     * an interrupted declaration and must not populate proposed_spell. Keep
     * the same bounded spell-label grammar. STT can omit the clause comma. */
    if (prefix(start, "before i cast ")) {
        const char *label = start + 14u;
        const char *boundary = strchr(label, ',');
        const char *next = boundary ? boundary + 1u : NULL;
        char planned_spell[201];
        if (!boundary) {
            for (const char *p = label; *p; ++p) {
                if (*p != ' ') continue;
                const char *candidate = p + 1u;
                const char *question_start = candidate;
                if (presence_start(&question_start)) {
                    boundary = p;
                    next = candidate;
                    break;
                }
            }
        }
        if (!boundary) return 0;
        size_t length = (size_t)(boundary - label);
        while (length && label[length - 1u] == ' ') --length;
        if (!length || length >= sizeof(planned_spell)) return 0;
        memcpy(planned_spell, label, length);
        planned_spell[length] = '\0';
        if (!name_valid(planned_spell)) return 0;
        start = next;
        while (*start == ' ') ++start;
        const char *question_start = start;
        if (!presence_start(&question_start)) return 0;
    }
    /* The final repair can replace an earlier proposal or question. It never
     * authorizes that proposal. Compound typed mutations reject at admission. */
    for (const char *p = start; *p; ++p) {
        if (p != question && p[-1] != ' ' && p[-1] != ',' && p[-1] != '-') continue;
        for (size_t i = 0; i < sizeof(repairs) / sizeof(repairs[0]); ++i) {
            if (!prefix(p, repairs[i])) continue;
            const char *after = p + strlen(repairs[i]);
            size_t separator;
            if (!repair_separator(after) && *after != '.') continue;
            while ((separator = repair_separator(after)) != 0u) after += separator;
            /* STT can end the repair cue with one sentence period. Keep other
             * clause punctuation and repeated periods outside this shortcut. */
            if (*after == '.') {
                ++after;
                while (*after == ' ') ++after;
            }
            const char *question_start = after;
            if (presence_start(&question_start)) {
                if (!repair_prefix(start, p, spell)) return 0;
                start = after;
                p = after - 1;
                break;
            }
        }
    }
    if (!presence_name(start, intent->character_name)) return 0;
    memcpy(intent->proposed_spell, spell, strlen(spell) + 1u);
    return 1;
}

int dnd_scene_question(const char *text, char name[201]) {
    dnd_scene_intent intent;
    if (!name) return 0;
    name[0] = '\0';
    if (!dnd_scene_parse(text, &intent)) return 0;
    memcpy(name, intent.character_name, strlen(intent.character_name) + 1u);
    return 1;
}

int dnd_scene_input(const char *prompt, const char *campaign, const char *scene, char *out, size_t capacity) {
    char name[201], escaped[1201];
    int n;
    if (!out || !capacity || !campaign || !scene ||
        !dnd_request_id_valid(campaign, strlen(campaign) + 1u) ||
        !dnd_request_id_valid(scene, strlen(scene) + 1u) ||
        !dnd_scene_question(prompt, name) || cmp_json_escape_exact(name, escaped, sizeof(escaped))) return -1;
    n = snprintf(out, capacity, "{\"operation\":\"resolve_scene_presence\",\"campaign_id\":\"%s\","
        "\"scene_id\":\"%s\",\"character_name\":\"%s\"}", campaign, scene, escaped);
    return n > 0 && (size_t)n < capacity ? n : -1;
}

int dnd_scene_output(const char *json, const char *prompt, const char *campaign, const char *scene,
                       dnd_tool_result *result) {
    cmp_json_object object;
    char name[201], returned_name[201], operation[32], call_id[80], returned_campaign[65], returned_scene[65];
    char status[16], presence[8], source[65];
    int64_t version;
    int n;
    if (!result) return -1;
    result->text[0] = '\0';
    result->scene_revision = 0;
    result->encounter_length = result->roster_length = result->initiative_length = 0;
    if (!campaign || !scene || !dnd_scene_question(prompt, name) ||
        !cmp_json_object_parse(json, &object) || object.field_count != 9u ||
        !cmp_json_object_str(&object, "operation", operation, sizeof(operation)) || strcmp(operation, "resolve_scene_presence") ||
        !cmp_json_object_str(&object, "operation_id", call_id, sizeof(call_id)) || strcmp(call_id, result->call_id) ||
        !cmp_json_object_str(&object, "campaign_id", returned_campaign, sizeof(returned_campaign)) || strcmp(campaign, returned_campaign) ||
        !cmp_json_object_str(&object, "scene_id", returned_scene, sizeof(returned_scene)) || strcmp(scene, returned_scene) ||
        !cmp_json_object_str(&object, "character_name", returned_name, sizeof(returned_name)) || strcmp(name, returned_name) ||
        !cmp_json_object_str(&object, "status", status, sizeof(status)) || (strcmp(status, "active") && strcmp(status, "archived")) ||
        !cmp_json_object_i64(&object, "version", &version) || version < 1 || version > DND_EXACT_VERSION_MAX ||
        !cmp_json_object_str(&object, "presence", presence, sizeof(presence)) ||
        (strcmp(presence, "present") && strcmp(presence, "absent") && strcmp(presence, "unknown")) ||
        !cmp_json_object_str(&object, "source_turn_id", source, sizeof(source))) return -1;
    if (!strcmp(presence, "unknown")) {
        if (source[0]) return -1;
        n = snprintf(result->text, sizeof(result->text), "I can't confirm whether %s is in this room.", name);
    } else {
        if (strcmp(status, "active") || !dnd_request_id_valid(source, sizeof(source))) return -1;
        n = snprintf(result->text, sizeof(result->text), !strcmp(presence, "present") ?
            "The current scene record places %s in this room." :
            "The current scene record says %s is not in this room.", name);
    }
    if (n <= 0 || (size_t)n >= sizeof(result->text)) { result->text[0] = '\0'; return -1; }
    result->scene_revision = (uint64_t)version;
    return 0;
}
