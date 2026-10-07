#ifndef VOICE_C_LOOP_RULES_H
#define VOICE_C_LOOP_RULES_H

#include "pb_min.h"

#define LOOP_RULES_REPLY "Which edition or verified source should I check? I have not cast anything. Verify resources and allies before deciding."

/* Recognize bounded mechanics questions in a user-established D&D topic.
 * This selects a clarification; it supplies no rules or execution authority.
 * The caller excludes scene questions and calls this only without retrieved
 * source evidence. Returns 1 for clarification, 0 otherwise, -1 for bad input. */
int loop_rules_clarification(const char *text, size_t length,
                             const session_history_response_c *history);

#endif
