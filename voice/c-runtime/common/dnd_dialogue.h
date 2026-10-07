#ifndef VOICE_C_DND_DIALOGUE_H
#define VOICE_C_DND_DIALOGUE_H

#include "dnd_pending.h"

enum {
    DND_DIALOGUE_NONE = 0,
    DND_DIALOGUE_RECALL = 1,
    DND_DIALOGUE_CANCEL = 2,
    DND_DIALOGUE_CAST = 3,
    DND_DIALOGUE_CANCEL_AND_ASK = 4
};

/* Bounded whole commands only. Questions and reported speech are not effects. */
int dnd_dialogue_command(const char *text, int held);
int dnd_dialogue_reply(int command, const dnd_pending_view *view,
    char *out, size_t capacity);
int dnd_dialogue_prompt(const dnd_pending_view *view, int canceled, const char *prompt,
    char *out, size_t capacity);

#endif
