#include "dnd_dialogue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int condition, const char *name) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", name); exit(1); }
}

int main(void) {
    dnd_pending_view view = {0};
    char output[4096];
    check(dnd_dialogue_command("What spell am I holding?", 1) == DND_DIALOGUE_RECALL, "recall");
    check(dnd_dialogue_command("Never mind, cancel that spell.", 1) == DND_DIALOGUE_CANCEL, "cancel");
    check(dnd_dialogue_command("Okay, cast it now.", 1) == DND_DIALOGUE_CAST, "cast cannot use model");
    check(dnd_dialogue_command("yes", 1) == DND_DIALOGUE_CAST &&
        dnd_dialogue_command("yes", 0) == DND_DIALOGUE_NONE, "confirmation depends on held context");
    const char *cancellations[] = {"Actually, drop the idea", "Please abandon my proposal.",
        "Scrap that spell!", "Okay, please cancel this proposal.", "Drop it", "Forget the spell."};
    for (size_t i = 0; i < sizeof(cancellations) / sizeof(cancellations[0]); ++i)
        check(dnd_dialogue_command(cancellations[i], 1) == DND_DIALOGUE_CANCEL, "explicit cancellation clauses");
    const char *followups[] = {"Actually, drop the idea. What should I do instead?",
        "Cancel my spell; which alternatives do I have?", "Abandon this proposal. How can I protect Mira?",
        "Forget the spell. Could we talk our way past them instead?",
        "Forget my proposal. Can we negotiate?"};
    for (size_t i = 0; i < sizeof(followups) / sizeof(followups[0]); ++i)
        check(dnd_dialogue_command(followups[i], 1) == DND_DIALOGUE_CANCEL_AND_ASK, "cancel then answer separate question");
    check(dnd_dialogue_command("Drop it", 0) == DND_DIALOGUE_NONE, "unbound pronoun cannot cancel");
    const char *questions[] = {"Would it hit her?", "If I cast Fireball, is Mira safe?",
        "Mira said 'cast it'", "What spell am I holding? Then cast it.",
        "Cancel that spell and cast another", "Do you know how to cast Fireball?",
        "Don't drop the idea", "Do not cancel my spell", "If Mira returns, drop the idea",
        "Mira said drop the idea", "Should I abandon the proposal?", "Drop the idea only if Mira is here",
        "Cancel the spell. Only if Mira returns.", "I might scrap that proposal", "Cancel that spellbook",
        "\"Drop the idea.\" What does that mean?", "Please don't cancel my spell",
        "If I forgot that spell, could we talk our way past them instead?",
        "Don't forget the spell. Could we talk instead?", "Mira said forget the spell.",
        "Forget the spell only if Mira is here.", "Forget the spellbook."};
    for (size_t i = 0; i < sizeof(questions) / sizeof(questions[0]); ++i)
        check(dnd_dialogue_command(questions[i], 1) == DND_DIALOGUE_NONE, "questions and mixed commands stay outside shortcut");
    check(dnd_dialogue_reply(DND_DIALOGUE_RECALL, &view, output, sizeof(output)) == 0 &&
        !strcmp(output, "No spell proposal is on hold in this session."), "empty recall");
    view.held = 1;
    strcpy(view.spell, "Fireball");
    strcpy(view.question_character, "Mira");
    check(dnd_dialogue_reply(DND_DIALOGUE_RECALL, &view, output, sizeof(output)) == 0 &&
        !strcmp(output, "Your Fireball proposal is on hold."), "held recall");
    check(dnd_dialogue_reply(DND_DIALOGUE_CANCEL, &view, output, sizeof(output)) == 0 &&
        !strcmp(output, "Canceled your Fireball proposal."), "proposal cancellation is not a game undo");
    check(dnd_dialogue_reply(DND_DIALOGUE_CAST, &view, output, sizeof(output)) == 0 &&
        strstr(output, "cannot execute spells") && !strstr(output, "Casting"), "no unverified execution claim");
    check(dnd_dialogue_reply(DND_DIALOGUE_CAST, &view, output, 2) != 0 && !output[0],
        "truncated speech fails closed");
    view.scene_changed = 1;
    check(dnd_dialogue_prompt(&view, 0, "Would it hit her?", output, sizeof(output)) == 0 &&
        strstr(output, "not scene evidence or an execution receipt") &&
        strstr(output, "\"held_spell\":\"Fireball\"") &&
        strstr(output, "\"last_question_character\":\"Mira\"") &&
        strstr(output, "\"scene_context_changed\":true") &&
        strstr(output, "Current input:\nWould it hit her?"), "bounded context separates reference and authority");
    strcpy(view.question_character, "Mira\"}");
    check(dnd_dialogue_prompt(&view, 0, "Question", output, sizeof(output)) == 0 &&
        strstr(output, "Mira\\\"}"), "quoted reference remains JSON data");
    check(dnd_dialogue_prompt(&view, 0, "Question", output, 2) != 0 && !output[0], "small context buffer rejects");
    check(dnd_dialogue_prompt(&view, 1, "Drop the idea. What next?", output, sizeof(output)) == 0 &&
        strstr(output, "\"held_spell\":\"\"") && strstr(output, "\"canceled_proposal\":\"Fireball\""),
        "cancellation receipt replaces held state in model context");
    check(dnd_dialogue_prompt(&view, 2, "Question", output, sizeof(output)) != 0 && !output[0],
        "invalid cancellation receipt flag rejects");
    memset(view.spell, 'x', sizeof(view.spell));
    check(dnd_dialogue_reply(DND_DIALOGUE_CAST, &view, output, sizeof(output)) != 0 &&
        !output[0], "unterminated state rejects");
    puts("ALL PASS dialogue command and projection contract");
    return 0;
}
