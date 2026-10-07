#ifndef VOICE_C_RESPONSE_STYLE_H
#define VOICE_C_RESPONSE_STYLE_H

#include "dnd_grounding.h"

#define VOICE_RESPONSE_STYLE_ID "voice.product_response_style.default"
#define VOICE_LOOP_DIALOGUE_ID "voice.loop.assistant_default"
#define VOICE_DND_DIALOGUE_ID "voice.dnd_dialogue.default"

typedef struct {
    char direct[DND_GROUNDING_TEXT_CAP];
    char grounded[DND_GROUNDING_TEXT_CAP];
    char dnd_direct[DND_GROUNDING_TEXT_CAP];
    char dnd_grounded[DND_GROUNDING_TEXT_CAP];
    char loop_direct[DND_GROUNDING_TEXT_CAP];
    char loop_grounded[DND_GROUNDING_TEXT_CAP];
    size_t loop_direct_len;
    size_t loop_grounded_len;
    size_t direct_len;
    size_t grounded_len;
    size_t dnd_direct_len;
    size_t dnd_grounded_len;
    char loop_version[32];
    char loop_sha256[65];
    char version[32];
    char sha256[65];
    char dnd_version[32];
    char dnd_sha256[65];
} voice_response_prompts;

/* Load and compose immutable prompts before workers start. Grounding follows
 * the shared response style and keeps its separate provenance identity. */
int voice_response_prompts_load(const char *root, const dnd_grounding_prompt *grounding,
                               voice_response_prompts *out);

#endif
