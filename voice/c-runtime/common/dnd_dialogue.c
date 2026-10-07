#include "dnd_dialogue.h"
#include "cmp_json.h"

#include <stdio.h>
#include <string.h>

static int view_valid(const dnd_pending_view *view) {
    return view && (view->held == 0 || view->held == 1) &&
        (view->scene_changed == 0 || view->scene_changed == 1) &&
        memchr(view->spell, '\0', sizeof(view->spell)) &&
        memchr(view->question_character, '\0', sizeof(view->question_character)) &&
        (!view->held || view->spell[0]);
}

static int cancellation(const char *text, int held) {
    static const char *const markers[] = {"actually, ", "actually ", "okay, ", "okay ", "ok, ", "please "};
    static const char *const verbs[] = {"cancel ", "drop ", "abandon ", "scrap ", "forget "};
    static const char *const objects[] = {
        "that spell", "the spell", "my spell", "this spell",
        "that proposal", "the proposal", "my proposal", "this proposal",
        "that idea", "the idea", "my idea", "this idea", "it"
    };
    for (size_t pass = 0; pass < 2u; ++pass) {
        for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
            size_t n = strlen(markers[i]);
            if (!strncmp(text, markers[i], n)) { text += n; break; }
        }
    }
    for (size_t v = 0; v < sizeof(verbs) / sizeof(verbs[0]); ++v) {
        size_t n = strlen(verbs[v]);
        if (strncmp(text, verbs[v], n)) continue;
        const char *object = text + n;
        for (size_t o = 0; o < sizeof(objects) / sizeof(objects[0]); ++o) {
            if (!held && !strcmp(objects[o], "it")) continue;
            size_t len = strlen(objects[o]);
            if (strncmp(object, objects[o], len)) continue;
            const char *tail = object + len;
            if (!*tail) return DND_DIALOGUE_CANCEL;
            /* Admit an explicit complete imperative, then a separate question.
             * Conditional, reported, negated and same-clause compound requests
             * cannot enter this state-changing shortcut. */
            if ((*tail == '.' || *tail == ';') && tail[1] == ' ') {
                tail += 2;
                if (!strncmp(tail, "what ", 5u) || !strncmp(tail, "which ", 6u) ||
                    !strncmp(tail, "how ", 4u) || !strncmp(tail, "can ", 4u) ||
                    !strncmp(tail, "could ", 6u)) return DND_DIALOGUE_CANCEL_AND_ASK;
            }
        }
    }
    return DND_DIALOGUE_NONE;
}

int dnd_dialogue_command(const char *text, int held) {
    char clean[2048];
    size_t used = 0;
    if (!text) return DND_DIALOGUE_NONE;
    for (size_t i = 0; text[i]; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (i == sizeof(clean) - 1u) return DND_DIALOGUE_NONE;
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (c < 32u || c == 127u) return DND_DIALOGUE_NONE;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c == ' ' && (!used || clean[used - 1u] == ' ')) continue;
        clean[used++] = (char)c;
    }
    while (used && clean[used - 1u] == ' ') --used;
    if (used && strchr(".?!", clean[used - 1u])) --used;
    while (used && clean[used - 1u] == ' ') --used;
    clean[used] = '\0';
    int cancel = cancellation(clean, held);
    if (cancel) return cancel;
    if (!strcmp(clean, "what spell am i holding") ||
        !strcmp(clean, "what spell is on hold") ||
        !strcmp(clean, "what is my held spell")) return DND_DIALOGUE_RECALL;
    if (!strcmp(clean, "cancel that spell") || !strcmp(clean, "cancel my spell") ||
        !strcmp(clean, "cancel the spell") || !strcmp(clean, "cancel my proposal") ||
        !strcmp(clean, "never mind, cancel that spell") ||
        !strcmp(clean, "never mind cancel that spell")) return DND_DIALOGUE_CANCEL;
    if (!strncmp(clean, "i cast ", 7u) || !strncmp(clean, "cast ", 5u) ||
        !strcmp(clean, "yes, cast it") || !strcmp(clean, "yes cast it") ||
        !strcmp(clean, "okay, cast it now") || !strcmp(clean, "okay cast it now") ||
        !strcmp(clean, "yes, cast the prepared spell at the marked point"))
        return DND_DIALOGUE_CAST;
    if (held && (!strcmp(clean, "yes") || !strcmp(clean, "okay") ||
        !strcmp(clean, "ok") || !strcmp(clean, "do it") || !strcmp(clean, "go ahead")))
        return DND_DIALOGUE_CAST;
    return DND_DIALOGUE_NONE;
}

int dnd_dialogue_reply(int command, const dnd_pending_view *view,
    char *out, size_t capacity) {
    int n;
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!view_valid(view)) return -1;
    if (command == DND_DIALOGUE_CAST) {
        n = view->held ? snprintf(out, capacity,
            "Your %s proposal is on hold. This voice path cannot execute spells.", view->spell) :
            snprintf(out, capacity, "This voice path cannot execute spells.");
    } else if (command == DND_DIALOGUE_CANCEL || command == DND_DIALOGUE_RECALL) {
        n = view->held ? snprintf(out, capacity,
            command == DND_DIALOGUE_CANCEL ? "Canceled your %s proposal." :
            "Your %s proposal is on hold.", view->spell) :
            snprintf(out, capacity, "No spell proposal is on hold in this session.");
    } else return -1;
    if (n < 0 || (size_t)n >= capacity) { out[0] = '\0'; return -1; }
    return 0;
}

int dnd_dialogue_prompt(const dnd_pending_view *view, int canceled, const char *prompt,
    char *out, size_t capacity) {
    char spell[1201], character[1201];
    int n;
    if (!prompt || !out || !capacity) return -1;
    out[0] = '\0';
    if (!view_valid(view) || (canceled != 0 && canceled != 1)) return -1;
    if (cmp_json_escape_exact(view->spell, spell, sizeof(spell)) ||
        cmp_json_escape_exact(view->question_character, character, sizeof(character))) return -1;
    if (canceled) {
        n = snprintf(out, capacity,
            "Conversation reference only; not scene evidence or an execution receipt. "
            "Spell execution is unavailable. "
            "{\"held_spell\":\"\",\"last_question_character\":\"%s\","
            "\"scene_context_changed\":%s,\"canceled_proposal\":\"%s\"}\nCurrent input:\n%s",
            character, view->scene_changed ? "true" : "false", view->held ? spell : "", prompt);
    } else n = snprintf(out, capacity,
        "Conversation reference only; not scene evidence or an execution receipt. "
        "Spell execution is unavailable. "
        "{\"held_spell\":\"%s\",\"last_question_character\":\"%s\","
        "\"scene_context_changed\":%s}\nCurrent input:\n%s", spell, character,
        view->scene_changed ? "true" : "false", prompt);
    if (n < 0 || (size_t)n >= capacity) { out[0] = '\0'; return -1; }
    return 0;
}
