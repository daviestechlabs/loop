/* Canonical public response boundary shared by the pure-C gateways. */
#ifndef VOICE_C_TURN_RESPONSE_H
#define VOICE_C_TURN_RESPONSE_H

#include <stddef.h>
#include <stdint.h>

#include "pb_min.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURN_RESPONSE_MAX_AUDIO_EVENT (16u * 1024u)

enum turn_response_action {
    TURN_RESPONSE_REJECT = -1,
    TURN_RESPONSE_DROP = 0,
    TURN_RESPONSE_FORWARD = 1,
    TURN_RESPONSE_HOLD_COMPLETED = 2,
    TURN_RESPONSE_FORWARD_TERMINAL = 3,
    TURN_RESPONSE_FORWARD_AND_COMPLETE = 4
};

typedef struct {
    int wait_for_pcm;
    int thinking_started;
    int text_completed;
    int model_completed;
    int final_pcm_seen;
    int final_pcm_ended;
    int pcm_open;
    int terminal;
    int have_pcm_sequence;
    int32_t last_pcm_sequence;
    int32_t last_pcm_segment;
} turn_response_state;

/* A validated view borrowed from one decoded event. */
typedef struct {
    const char *request_id;
    const char *type;
    uint16_t request_id_len;
    uint16_t type_len;
    uint16_t text_len;
    uint8_t text_json_raw;
    /* The shared filter sets this after public control and UTF-8 checks. */
    uint8_t text_validated;
    const char *text;
    const char *speech_text;
    const char *display_text;
    const uint8_t *audio;
    size_t audio_len;
    int type_id;
    int32_t sample_rate;
    int32_t channels;
    int32_t bit_depth;
    int32_t sequence;
    int32_t segment_index;
    int is_final;
    int32_t audio_encoding;
    const turn_stage_timestamps_c *stages;
    const turn_tool_result_c *tool_result;
    const char *provider_model;
    const turn_provider_c *provider_usage;
    const turn_retrieval_c *retrieval;
    const turn_encounter_c *encounter;
    const turn_roster_c *roster;
    const turn_initiative_c *initiative;
    /* Validated stage wire borrowed with stages until encoding completes. */
    const uint8_t *current_tts_stage_wire;
    /* Source-bound runtime hash supplied by the trusted public edge. */
    const char *runtime_identity_sha256;
    uint16_t runtime_identity_sha256_len;
} turn_response_event;

/* Public strings must be valid UTF-8 as well as structurally valid protobuf. */
int turn_retrieval_valid(const turn_retrieval_c *retrieval);
int turn_encounter_valid(const turn_encounter_c *encounter, const turn_tool_result_c *tool);
int turn_roster_valid(const turn_roster_c *roster, const turn_tool_result_c *tool);
/* Bind final state views and roll arithmetic to the same admitted tool receipt. */
int turn_state_result_valid(const turn_encounter_c *encounter, const turn_roster_c *roster,
    const turn_initiative_c *initiative, const turn_tool_result_c *tool);

/* Validate and redact one internal event before a public response stream.
 * The returned view borrows storage from event until the caller changes it. */
enum turn_response_action turn_response_filter(
    turn_response_state *state,
    const turn_event_c *event,
    turn_response_event *safe_event
);

/* Filter an event returned by a successful pb_decode_turn_event call. */
enum turn_response_action turn_response_filter_decoded(
    turn_response_state *state,
    const turn_event_c *event,
    turn_response_event *safe_event
);

/* Filter a decoded event bound to an active validated request identifier.
 * The returned view borrows request_id from the caller. */
enum turn_response_action turn_response_filter_bound(
    turn_response_state *state,
    const turn_event_c *event,
    const char *request_id,
    turn_response_event *safe_event
);

/* Filter with an already validated request identifier span. */
enum turn_response_action turn_response_filter_bound_n(
    turn_response_state *state,
    const turn_event_c *event,
    const char *request_id,
    size_t request_id_len,
    turn_response_event *safe_event
);

/* Filter an event already decoded and bound to an active request.
 * All pointers must be valid. The identifier length must be from 1 through 127. */
enum turn_response_action turn_response_filter_active_n(
    turn_response_state *state,
    const turn_event_c *event,
    const char *request_id,
    size_t request_id_len,
    turn_response_event *safe_event
);

/* Filter an event returned by pb_decode_turn_event_public_active.
 * Borrowed spans must remain valid until response encoding finishes. */
enum turn_response_action turn_response_filter_public_active_n(
    turn_response_state *state,
    const turn_event_c *event,
    const char *request_id,
    size_t request_id_len,
    turn_response_event *safe_event
);

/* Encode one filtered event for the public protobuf transport. */
size_t turn_response_protobuf_encode(
    uint8_t *out,
    size_t out_cap,
    const turn_response_event *event
);

#ifdef __cplusplus
}
#endif

#endif
