/* Offline intervention only. Production request construction is unchanged. */
#include "dnd_dialogue.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    dnd_pending_view view = {0};
    char output[4096];
    const char *message;
    int command;
    if (argc != 4 || (strcmp(argv[1], "0") && strcmp(argv[1], "1")) ||
        (strcmp(argv[2], "full_input") && strcmp(argv[2], "remaining_question"))) return 1;
    view.held = !strcmp(argv[1], "1");
    if (view.held) {
        strcpy(view.spell, "fireball");
        strcpy(view.question_character, "Mira");
    }
    message = argv[3];
    command = dnd_dialogue_command(message, view.held);
    if (command == DND_DIALOGUE_CANCEL_AND_ASK && !strcmp(argv[2], "remaining_question")) {
        /* The admitted grammar has no delimiter in its markers, verb or object.
         * Locate that proved boundary in the original bytes, preserving names,
         * punctuation and every following clause instead of returning normalized text. */
        const char *boundary = strpbrk(message, ".;");
        if (!boundary) return 1;
        message = boundary + 1;
        while (*message == ' ' || *message == '\t' || *message == '\r' || *message == '\n') ++message;
        if (!*message) return 1;
    }
    if (dnd_dialogue_prompt(&view, command == DND_DIALOGUE_CANCEL_AND_ASK,
            message, output, sizeof(output))) return 1;
    return fputs(output, stdout) < 0 ? 1 : 0;
}
