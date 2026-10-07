#ifndef VOICE_C_DND_TOOLS_H
#define VOICE_C_DND_TOOLS_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "pb_min.h"

typedef struct {
    char tool_id[32];
    char call_id[80];
    char output_sha256[65];
    char text[2048];
    int64_t elapsed_ms;
    uint64_t scene_revision;
    union {
        uint8_t roster_wire[DND_TURN_ROSTER_MAX];
        struct {
            uint8_t encounter_wire[DND_TURN_ENCOUNTER_MAX];
            uint8_t initiative_wire[DND_TURN_INITIATIVE_MAX];
        };
    };
    size_t encounter_length;
    size_t roster_length;
    size_t initiative_length;
} dnd_tool_result;

enum {
    DND_TOOL_OK = 0,
    DND_TOOL_INVALID = -1,
    DND_TOOL_UNAVAILABLE = -2,
    DND_TOOL_CANCELED = -3,
    DND_TOOL_DEADLINE = -4
};

/* 1: bounded dice command; 0: no roll command; -1: unsupported roll command. */
int dnd_dice_intent(const char *text, char expression[64]);
int dnd_tools_config_valid(const char *url, const char *secret);
int dnd_encounter_intent(const char *text);
int dnd_roster_intent(const char *text);
int dnd_initiative_intent(const char *text);
int dnd_campaign_intent(const char *text);
int dnd_action_intent(const char *text);
/* Direct current-presence questions, including a final self-correction. */
typedef struct {
    char character_name[201];
    char proposed_spell[201];
} dnd_scene_intent;
/* Retain an explicit repaired proposal as data, never execution authority. */
int dnd_scene_parse(const char *text, dnd_scene_intent *intent);
int dnd_scene_question(const char *text, char name[201]);
int dnd_scene_input(const char *prompt, const char *campaign, const char *scene, char *out, size_t capacity);
int dnd_scene_output(const char *json, const char *prompt, const char *campaign, const char *scene, dnd_tool_result *result);
int dnd_scene_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *scene_id, int64_t deadline_ms, int timeout_ms,
    const atomic_int *canceled, dnd_tool_result *result);
int dnd_action_input(const dnd_encounter_action_c *request, const char *campaign_id, const char *encounter_id,
    char *out, size_t capacity);
int dnd_action_output(const char *json, const char *campaign_id, const char *encounter_id,
    const dnd_encounter_action_c *request, dnd_tool_result *result);
int dnd_action_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *encounter_id, const dnd_encounter_action_c *request,
    int64_t deadline_ms, int timeout_ms, const atomic_int *canceled, dnd_tool_result *result);
int dnd_campaign_input(const dnd_campaign_request_c *request, const char *campaign_id,
    const char *user_id, char *out, size_t capacity);
int dnd_campaign_output(const char *json, const char *campaign_id, const char *user_id,
    const dnd_campaign_request_c *request, dnd_tool_result *result);
int dnd_campaign_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const dnd_campaign_request_c *request,
    int64_t deadline_ms, int timeout_ms, const atomic_int *canceled, dnd_tool_result *result);
int dnd_encounter_output(const char *json, const char *campaign_id, const char *encounter_id,
                         dnd_tool_result *result);
int dnd_roster_output(const char *json, const char *campaign_id, dnd_tool_result *result);
int dnd_initiative_output(const char *json, const char *campaign_id, const char *encounter_id,
    const dnd_initiative_request_c *request, dnd_tool_result *result);
int dnd_encounter_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *encounter_id, int64_t deadline_ms, int timeout_ms,
    const atomic_int *canceled, dnd_tool_result *result);

int dnd_roster_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, int64_t deadline_ms, int timeout_ms,
    const atomic_int *canceled, dnd_tool_result *result);
int dnd_initiative_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *encounter_id, const dnd_initiative_request_c *initiative,
    int64_t deadline_ms, int timeout_ms, const atomic_int *canceled, dnd_tool_result *result);

/* Validate typed arithmetic and format speech without model-authored numbers. */
int dnd_dice_output_text(const char *json, const char *expression,
                         const char *call_id, char *text, size_t capacity);

int dnd_tools_execute(const char *url, const char *secret,
                      const char *request_id, const char *user_id,
                      const char *session_id, const char *prompt,
                      int64_t deadline_ms, int timeout_ms,
                      const atomic_int *canceled, dnd_tool_result *result);

#endif
