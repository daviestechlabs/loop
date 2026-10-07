#ifndef VOICE_C_LOOP_SCENE_H
#define VOICE_C_LOOP_SCENE_H

#include "pb_min.h"

/* Resolve a bounded question focus from user messages, never a presence fact.
 * The caller supplies owner-bound history and admits only the Loop profile.
 * Return 1 for a complete reply, 0 for no match, -1 for invalid input/capacity. */
int loop_scene_reply(const char *text, size_t length,
    const session_history_response_c *history, char *out, size_t capacity);

/* One spelling name from owner-bound USER question focus. No presence claim.
 * Unsupported names or an intervening topic return 0 and clear the output. */
int loop_scene_vocabulary(const session_history_response_c *history,
    char *out, size_t capacity);

/* A bounded scene question followed by one imagined narration request.
 * Return 1 for an uncertainty preface plus borrowed narration, 2 for a name
 * clarification, 0 for no match, or -1 for invalid input/capacity.
 * History supplies spelling only. This never resolves presence or executes. */
int loop_scene_mixed_reply(const char *text, size_t length,
    const session_history_response_c *history, char *out, size_t capacity,
    const char **narration);

#endif
