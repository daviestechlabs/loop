#ifndef VOICE_C_DND_GROUNDING_H
#define VOICE_C_DND_GROUNDING_H

#include "dnd_retrieval_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    dnd_grounding_identity identity;
    char text[DND_GROUNDING_TEXT_CAP];
    size_t text_len;
} dnd_grounding_prompt;

/* Load the canonical system prompt once, before model workers start.
 * NULL or empty root uses the prompt library's monorepo discovery.
 * Failure clears the output and never substitutes an ad hoc prompt. */
int dnd_grounding_load(const char *root, dnd_grounding_prompt *out);

#ifdef __cplusplus
}
#endif

#endif
