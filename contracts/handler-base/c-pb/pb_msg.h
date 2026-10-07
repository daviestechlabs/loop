/* pb_msg.h — pure-C messages.proto product codecs (subset structs + wire). */
#ifndef HANDLER_BASE_C_PB_MSG_H
#define HANDLER_BASE_C_PB_MSG_H

#include "pb_wire.h"
#include "pb_dnd_request.h"
#include "pb_dnd_campaign.h"
#include "pb_dnd_action.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sizing for product path (bounded, no malloc in codecs). */
#define PB_ID 160
#define PB_STR 512
#define PB_TEXT 4096
#define PB_SUBJECT 320
#define PB_INPUT 8192
#define PB_OUT 65536
#define PB_META_PAIRS 16
#define PB_TURN_META_PAIRS 32

/* ── enums (match messages.proto) ── */
enum {
    PB_TURN_STATE_CREATED = 1,
    PB_TURN_STATE_AWAITING_FIRST_TOKEN = 2,
    PB_TURN_STATE_STREAMING_TEXT = 3,
    PB_TURN_STATE_STREAMING_AUDIO = 4,
    PB_TURN_STATE_DRAINING_AUDIO = 5,
    PB_TURN_STATE_COMPLETED = 6,
    PB_TURN_STATE_CANCELED = 7,
    PB_TURN_STATE_FAILED = 8
};

enum {
    PB_TURN_EVT_STARTED = 1,
    PB_TURN_EVT_THINKING_STARTED = 2,
    PB_TURN_EVT_THINKING_ENDED = 3,
    PB_TURN_EVT_TEXT_DELTA = 4,
    PB_TURN_EVT_TEXT_COMPLETED = 5,
    PB_TURN_EVT_TTS_SEGMENT = 6,
    PB_TURN_EVT_PCM_STARTED = 7,
    PB_TURN_EVT_PCM_CHUNK = 8,
    PB_TURN_EVT_PCM_ENDED = 9,
    PB_TURN_EVT_COMPLETED = 10,
    PB_TURN_EVT_CANCELED = 11,
    PB_TURN_EVT_FAILED = 12
};

enum {
    PB_AGENT_ST_CREATED = 1,
    PB_AGENT_ST_QUEUED = 2,
    PB_AGENT_ST_RUNNING = 3,
    PB_AGENT_ST_COMPLETED = 4,
    PB_AGENT_ST_FAILED = 5,
    PB_AGENT_ST_CANCELED = 6
};

enum {
    PB_TOOL_ST_REQUESTED = 1,
    PB_TOOL_ST_AWAITING_APPROVAL = 2,
    PB_TOOL_ST_APPROVED = 3,
    PB_TOOL_ST_REJECTED = 4,
    PB_TOOL_ST_QUEUED = 5,
    PB_TOOL_ST_RUNNING = 6,
    PB_TOOL_ST_COMPLETED = 7,
    PB_TOOL_ST_FAILED = 8,
    PB_TOOL_ST_CANCELED = 9
};

typedef struct {
    char key[64];
    char val[256];
} pb_meta_pair;

typedef struct {
    char request_id[PB_ID];
    char user_id[PB_ID];
    char session_id[PB_ID];
    char username[PB_ID];
    char text[PB_TEXT];
    int premium;
    int enable_rag;
    int enable_tts;
    char system_prompt[PB_TEXT];
    char voice_id[PB_ID];
    char response_subject[PB_SUBJECT];
    pb_meta_pair meta[PB_TURN_META_PAIRS];
    int n_meta;
    dnd_initiative_request_c dnd_initiative;
    dnd_campaign_request_c dnd_campaign;
    dnd_encounter_action_c dnd_encounter_action;
} pb_turn_start_req;

typedef struct {
    char request_id[PB_ID];
    int accepted;
    int state;
    int64_t accepted_at;
    char event_subject[PB_SUBJECT];
} pb_turn_start_resp;

typedef struct {
    char request_id[PB_ID];
    char user_id[PB_ID];
    char reason[PB_STR];
} pb_turn_cancel_req;

typedef struct {
    char request_id[PB_ID];
    char user_id[PB_ID];
    int type;
    int state;
    char text[PB_TEXT];
    char speech_text[PB_TEXT];
    char display_text[PB_TEXT];
    char error[PB_STR];
    int32_t sequence;
    int32_t segment_index;
    int is_final;
    int64_t timestamp;
    int32_t sample_rate;
    int32_t channels;
    int32_t bit_depth;
    pb_meta_pair meta[PB_META_PAIRS];
    int n_meta;
} pb_turn_event;

typedef struct {
    char request_id[PB_ID];
    char user_id[PB_ID];
    char session_id[PB_ID];
    char message[PB_TEXT];
    char response[PB_TEXT];
    char response_text[PB_TEXT];
} pb_chat_req;

typedef struct {
    char request_id[PB_ID];
    char user_id[PB_ID];
    char response[PB_TEXT];
    char response_text[PB_TEXT];
    char error[PB_STR];
} pb_chat_resp;

typedef struct {
    char task_id[PB_ID];
    char idempotency_key[PB_ID];
    char parent_turn_id[PB_ID];
    char user_id[PB_ID];
    char session_id[PB_ID];
    char profile[PB_ID];
    char agent_id[PB_ID];
    char input_json[PB_INPUT];
    int64_t deadline_unix_ms;
} pb_agent_start_req;

typedef struct {
    char task_id[PB_ID];
    int accepted;
    int state;
    char event_subject[PB_SUBJECT];
    int64_t accepted_at;
    char error[PB_STR];
} pb_agent_start_resp;

typedef struct {
    char task_id[PB_ID];
    char user_id[PB_ID];
    char reason[PB_STR];
} pb_agent_cancel_req;

typedef struct {
    char task_id[PB_ID];
    char agent_id[PB_ID];
    int state;
    int type;
    int32_t sequence;
    char text[PB_STR];
    char error[PB_STR];
    double progress;
    int64_t timestamp;
} pb_agent_event;

typedef struct {
    char task_id[PB_ID];
    char agent_id[PB_ID];
    char input_json[PB_INPUT];
    int64_t deadline_unix_ms;
    int attempt;
} pb_agent_dispatch_req;

typedef struct {
    char tool_call_id[PB_ID];
    char idempotency_key[PB_ID];
    char parent_task_id[PB_ID];
    char parent_turn_id[PB_ID];
    char user_id[PB_ID];
    char session_id[PB_ID];
    char agent_id[PB_ID];
    char tool_id[PB_ID];
    char input_json[PB_INPUT];
    int64_t deadline_unix_ms;
} pb_tool_start_req;

typedef struct {
    char tool_call_id[PB_ID];
    int accepted;
    int state;
    char event_subject[PB_SUBJECT];
    int64_t accepted_at;
    char error[PB_STR];
} pb_tool_start_resp;

typedef struct {
    char tool_call_id[PB_ID];
    char user_id[PB_ID];
    char reason[PB_STR];
} pb_tool_cancel_req;

typedef struct {
    char tool_call_id[PB_ID];
    int approved;
    char approver_id[PB_ID];
    char reason[PB_STR];
    int64_t decided_at;
    char approval_id[PB_ID]; /* metadata */
} pb_tool_approval_req;

typedef struct {
    char tool_call_id[PB_ID];
    int accepted;
    char output_json[PB_OUT];
    char summary[PB_STR];
    char error[PB_STR];
} pb_tool_dispatch_resp;

typedef struct {
    char tool_call_id[PB_ID];
    char parent_task_id[PB_ID];
    char parent_turn_id[PB_ID];
    char user_id[PB_ID];
    char session_id[PB_ID];
    char agent_id[PB_ID];
    char tool_id[PB_ID];
    int state;
    int type;
    int32_t sequence;
    char text[PB_STR];
    char error[PB_STR];
    int64_t timestamp;
} pb_tool_event;

typedef struct {
    char type[32];
    const uint8_t *audio;
    size_t audio_len;
    int32_t sample_rate;
    int32_t channels;
    int32_t bit_depth;
    char utterance_id[PB_ID];
    char speaker_id[64];
    char state[32];
} pb_stt_stream;

typedef struct {
    char session_id[PB_ID];
    char utterance_id[PB_ID];
    char type[32];
    int type_id;
    int64_t timestamp_ms;
} pb_stt_lifecycle;

/* Encode: returns bytes written, 0 on failure. Decode: 0 ok, -1 err. */

size_t pb_enc_turn_start_req(uint8_t *out, size_t cap, const pb_turn_start_req *m);
int pb_dec_turn_start_req(const uint8_t *in, size_t n, pb_turn_start_req *m);

size_t pb_enc_turn_start_resp(uint8_t *out, size_t cap, const pb_turn_start_resp *m);
int pb_dec_turn_start_resp(const uint8_t *in, size_t n, pb_turn_start_resp *m);

size_t pb_enc_turn_cancel_req(uint8_t *out, size_t cap, const pb_turn_cancel_req *m);
int pb_dec_turn_cancel_req(const uint8_t *in, size_t n, pb_turn_cancel_req *m);

size_t pb_enc_turn_event(uint8_t *out, size_t cap, const pb_turn_event *m);
int pb_dec_turn_event(const uint8_t *in, size_t n, pb_turn_event *m);

size_t pb_enc_chat_req(uint8_t *out, size_t cap, const pb_chat_req *m);
int pb_dec_chat_req(const uint8_t *in, size_t n, pb_chat_req *m);
size_t pb_enc_chat_resp(uint8_t *out, size_t cap, const pb_chat_resp *m);
int pb_dec_chat_resp(const uint8_t *in, size_t n, pb_chat_resp *m);

/* Login / greeting / stream chunk (messages.proto field numbers). */
typedef struct {
    char user_id[PB_ID];
    char username[PB_ID];
    char nickname[PB_ID];
    int premium;
    int64_t timestamp;
} pb_login_event;

typedef struct {
    char user_id[PB_ID];
    char username[PB_ID];
    char nickname[PB_ID];
    int premium;
} pb_greeting_req;

typedef struct {
    char user_id[PB_ID];
    char greeting[PB_TEXT];
    char error[PB_STR];
} pb_greeting_resp;

typedef struct {
    char request_id[PB_ID];
    char type[32];
    char content[PB_TEXT];
    int done;
    int64_t timestamp;
    char error[PB_STR];
} pb_chat_stream_chunk;

size_t pb_enc_login_event(uint8_t *out, size_t cap, const pb_login_event *m);
int pb_dec_login_event(const uint8_t *in, size_t n, pb_login_event *m);
size_t pb_enc_greeting_req(uint8_t *out, size_t cap, const pb_greeting_req *m);
int pb_dec_greeting_req(const uint8_t *in, size_t n, pb_greeting_req *m);
size_t pb_enc_greeting_resp(uint8_t *out, size_t cap, const pb_greeting_resp *m);
int pb_dec_greeting_resp(const uint8_t *in, size_t n, pb_greeting_resp *m);
size_t pb_enc_chat_stream_chunk(uint8_t *out, size_t cap, const pb_chat_stream_chunk *m);
int pb_dec_chat_stream_chunk(const uint8_t *in, size_t n, pb_chat_stream_chunk *m);

size_t pb_enc_agent_start_req(uint8_t *out, size_t cap, const pb_agent_start_req *m);
int pb_dec_agent_start_req(const uint8_t *in, size_t n, pb_agent_start_req *m);
size_t pb_enc_agent_start_resp(uint8_t *out, size_t cap, const pb_agent_start_resp *m);
int pb_dec_agent_start_resp(const uint8_t *in, size_t n, pb_agent_start_resp *m);
size_t pb_enc_agent_cancel_req(uint8_t *out, size_t cap, const pb_agent_cancel_req *m);
int pb_dec_agent_cancel_req(const uint8_t *in, size_t n, pb_agent_cancel_req *m);
size_t pb_enc_agent_event(uint8_t *out, size_t cap, const pb_agent_event *m);
int pb_dec_agent_event(const uint8_t *in, size_t n, pb_agent_event *m);
size_t pb_enc_agent_dispatch_req(uint8_t *out, size_t cap, const pb_agent_dispatch_req *m);

size_t pb_enc_tool_start_req(uint8_t *out, size_t cap, const pb_tool_start_req *m);
int pb_dec_tool_start_req(const uint8_t *in, size_t n, pb_tool_start_req *m);
size_t pb_enc_tool_start_resp(uint8_t *out, size_t cap, const pb_tool_start_resp *m);
int pb_dec_tool_start_resp(const uint8_t *in, size_t n, pb_tool_start_resp *m);
size_t pb_enc_tool_cancel_req(uint8_t *out, size_t cap, const pb_tool_cancel_req *m);
int pb_dec_tool_cancel_req(const uint8_t *in, size_t n, pb_tool_cancel_req *m);
size_t pb_enc_tool_approval_req(uint8_t *out, size_t cap, const pb_tool_approval_req *m);
int pb_dec_tool_approval_req(const uint8_t *in, size_t n, pb_tool_approval_req *m);
size_t pb_enc_tool_dispatch_resp(uint8_t *out, size_t cap, const pb_tool_dispatch_resp *m);
int pb_dec_tool_dispatch_resp(const uint8_t *in, size_t n, pb_tool_dispatch_resp *m);
size_t pb_enc_tool_event(uint8_t *out, size_t cap, const pb_tool_event *m);
int pb_dec_tool_event(const uint8_t *in, size_t n, pb_tool_event *m);

size_t pb_enc_stt_stream(uint8_t *out, size_t cap, const pb_stt_stream *m);
int pb_dec_stt_stream(const uint8_t *in, size_t n, pb_stt_stream *m);
size_t pb_enc_stt_lifecycle(uint8_t *out, size_t cap, const pb_stt_lifecycle *m);
int pb_dec_stt_lifecycle(const uint8_t *in, size_t n, pb_stt_lifecycle *m);

/* DndVisionEvent (table-vision product-safe projection). */
typedef struct {
    char session_id[PB_ID];
    uint64_t sequence;
    uint64_t observed_at_mono_ns;
    int64_t published_at_unix_ms;
    uint32_t person_count;
    float motion_energy;
    int32_t gaze_cluster;
    int scene_changed;
    char policy_version[64];
    uint32_t frame_width;
    uint32_t frame_height;
} pb_dnd_vision_event;

size_t pb_enc_dnd_vision(uint8_t *out, size_t cap, const pb_dnd_vision_event *m);
int pb_dec_dnd_vision(const uint8_t *in, size_t n, pb_dnd_vision_event *m);

/* JSON line helpers for residual Go dual-run shells (compact, not full JSON). */
/* encode: type name + fields as key=value lines on stdin; binary on stdout via CLI. */

#ifdef __cplusplus
}
#endif

#endif
