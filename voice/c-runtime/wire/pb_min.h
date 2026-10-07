#ifndef VOICE_C_PB_MIN_H
#define VOICE_C_PB_MIN_H

#include <stddef.h>
#include <stdint.h>
#include "../common/dnd_retrieval_types.h"
#include "../../../contracts/handler-base/c-pb/pb_dnd_request.h"
#include "../../../contracts/handler-base/c-pb/pb_dnd_campaign.h"
#include "../../../contracts/handler-base/c-pb/pb_dnd_action.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal protobuf wire helpers for product paths (no protobuf-c / no Go). */

typedef struct {
    int error;
    char message[256];
    char type[64];
} error_response_c;

size_t pb_encode_error_response(
    uint8_t *out,
    size_t out_cap,
    const char *message,
    const char *type
);
int pb_decode_error_response(const uint8_t *data, size_t len, error_response_c *out);

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} pb_reader;

void pb_reader_init(pb_reader *r, const uint8_t *data, size_t len);
int pb_read_tag(pb_reader *r, uint32_t *field, uint32_t *wire);
int pb_skip(pb_reader *r, uint32_t wire);
int pb_read_varint(pb_reader *r, uint64_t *out);
int pb_read_bytes(pb_reader *r, const uint8_t **out, size_t *out_len);
int pb_read_string(pb_reader *r, char *out, size_t out_cap);

/* STTStreamMessage subset used by audio-processor C service. */
typedef struct {
    char type[32];
    const uint8_t *audio;
    size_t audio_len;
    int32_t sample_rate;
    int32_t channels;
    int32_t bit_depth;
    int64_t timestamp_ms;
    char utterance_id[128];
    char speaker_id[64];
    char state[32];
    char user_id[128]; /* Field 13: server owner on an admitted Loop start. */
} stt_stream_message_c;

/* The decoder initializes every scalar and terminates every string.
 * Bytes after a string terminator are unspecified. */
int pb_decode_stt_stream_message(const uint8_t *data, size_t len, stt_stream_message_c *out);

/* Encode STTStreamMessage subset: type(1), audio(2), sample_rate(5), channels(6), bit_depth(7). */
size_t pb_encode_stt_stream_message(
    uint8_t *out,
    size_t out_cap,
    const char *type,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth
);

size_t pb_encode_stt_stream_message_at(
    uint8_t *out,
    size_t out_cap,
    const char *type,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int64_t timestamp_ms
);

/* Append the server owner to one canonical start, never an audio chunk. */
size_t pb_append_stt_owner(uint8_t *out, size_t capacity, size_t length,
    const char *user_id);

enum {
    STT_LIFECYCLE_UNSPECIFIED = 0,
    STT_LIFECYCLE_STREAM_STARTED = 1,
    STT_LIFECYCLE_SPEECH_STARTED = 2,
    STT_LIFECYCLE_SPEECH_ENDED = 3,
    STT_LIFECYCLE_STREAM_ENDED = 4,
    STT_LIFECYCLE_TRANSCRIPTION_FAILED = 5
};

/* Encode canonical STTLifecycleEvent fields: session_id(1), type(3), timestamp(5). */
size_t pb_encode_stt_lifecycle(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    const char *type,
    int64_t timestamp_ms
);

/* The caller proves the session span is canonical and bounded. */
size_t pb_encode_stt_lifecycle_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    size_t session_id_len,
    int type_id,
    int64_t timestamp_ms
);

/* Decode the canonical STTLifecycleEvent subset. */
typedef struct {
    char session_id[128];
    char utterance_id[128];
    char type[32];
    int type_id;
    int64_t timestamp_ms;
} stt_lifecycle_c;

int pb_decode_stt_lifecycle(const uint8_t *data, size_t len, stt_lifecycle_c *out);

/* Encode canonical STTInterrupt fields: session_id(1), type(2), timestamp(3). */
size_t pb_encode_stt_interrupt(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    int64_t timestamp_ms
);

/* Complete audio-input waterfall. Zero means the turn did not originate as audio. */
typedef struct {
    int64_t audio_committed_at_ms;
    int64_t stt_request_received_at_ms;
    int64_t stt_provider_request_started_at_ms;
    int64_t stt_provider_ready_at_ms;
    int64_t stt_transcript_published_at_ms;
} turn_input_stage_timestamps_c;

/* Canonical STTTranscription subset published on the same subject family. */
typedef struct {
    char session_id[128];
    char transcript[2048];
    int32_t sequence;
    int is_partial;
    int is_final;
    int64_t timestamp_ms;
    int has_voice_activity;
    char state[32];
    int commit_for_turn;
    int stream_state;
    turn_input_stage_timestamps_c input_stages;
} stt_transcription_c;

size_t pb_encode_stt_transcription(
    uint8_t *out,
    size_t out_cap,
    const stt_transcription_c *in
);

int pb_decode_stt_transcription(
    const uint8_t *data,
    size_t len,
    stt_transcription_c *out
);

/* Bounded browser and product context carried across the pure-C turn path.
 * These fields are requests and telemetry. They never replace server identity
 * or entitlement state. Empty strings mean that the key was absent. */
typedef struct {
    char turn_source[128];
    char turn_kind[9];
    char client_trace_id[128];
    char client_transport[64];
    char client_surface[64];
    char interaction_profile[32];
    char voice_mode[32];
    char turn_profile[32];
    char retrieval_skip[6];
    char agent_id[64];
    char task_intent[64];
    char turn_max_tokens[8];
    char input_mode[32];
    char capability_id[128];
    char parent_bundle[128];
    char prompt_hash[65];
    char product_session_id[128];
    char campaign_id[128];
    char scene_id[65];
    char character_id[128];
    char encounter_id[128];
    char requested_npc_id[128];
    char knowledge_scope[32];
    char dnd_session_recap[6];
    char dnd_forget_session_recap[6];
    char recap_session_id[128];
    char dnd_cold_open[6];
    char evaluation[6];
    char retrieval_force[6];
    char memory_context_version[128];
    char audio_group_session[6];
    char audio_participant_id[64];
    char audio_participant_label[241];
} turn_client_metadata_c;

/* Copy one admitted metadata value through the canonical wire field bounds.
 * Returns 1 for a known field, 0 for an unknown field, and -1 on invalid input.
 * This maps fields only; the public edge must apply its admission policy first. */
int pb_turn_client_metadata_set(turn_client_metadata_c *out, const char *key, const char *value);

/* TurnStart subset for the interactive C plane. */
typedef struct {
    char request_id[128];
    size_t request_id_len;
    char text[2048];
    size_t text_len;
    char session_id[128];
    size_t session_id_len;
    char response_subject[256];
    size_t response_subject_len;
    char user_id[128];
    size_t user_id_len;
    int premium;
    int enable_rag;
    int enable_tts;
    turn_client_metadata_c metadata;
    int model_request_capture;
    char meta_budget_ms[32];           /* metadata["turn_budget_ms"] */
    char meta_deadline_unix_ms[32];    /* metadata["turn_deadline_unix_ms"] */
    int has_meta_budget;
    int has_meta_deadline;
    turn_input_stage_timestamps_c input_stages;
    dnd_initiative_request_c dnd_initiative;
    dnd_campaign_request_c dnd_campaign;
    dnd_encounter_action_c dnd_encounter_action;
} turn_start_c;

int pb_decode_turn_start(const uint8_t *data, size_t len, turn_start_c *out);

/* Encode canonical TurnStartRequest fields 1,2,3,5,6,7,8,11,12. */
size_t pb_encode_turn_start(
    uint8_t *out,
    size_t out_cap,
    const turn_start_c *in
);

/* Encode a decoded TurnStart with its validated cached string lengths. */
size_t pb_encode_turn_start_prepared(
    uint8_t *out,
    size_t out_cap,
    const turn_start_c *in
);

typedef struct {
    char request_id[128];
    char user_id[128];
    char reason[256];
} turn_cancel_c;

#define TURN_CANCEL_REASON_UPSTREAM_FAILURE "upstream_failure"

size_t pb_encode_turn_cancel(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *user_id,
    const char *reason
);
int pb_decode_turn_cancel(const uint8_t *data, size_t len, turn_cancel_c *out);

typedef struct {
    char request_id[128];
    size_t request_id_len;
    char user_id[128];
    size_t user_id_len;
    char text[2048];
    size_t text_len;
    char voice_id[128];
    size_t voice_id_len;
    char response_subject[256];
    size_t response_subject_len;
    int32_t segment_index;
    int is_final;
    int32_t output_sample_rate;
    int32_t output_channels;
    int32_t output_bit_depth;
    int32_t output_encoding;
    uint32_t query_hash;
    int64_t first_text_at_ms;
    int64_t segment_emitted_at_ms;
    int stream_finality_deferred;
    turn_input_stage_timestamps_c input_stages;
} turn_tts_segment_c;

/* String pointers borrow from the input buffer and expire with that buffer. */
typedef struct {
    const char *request_id;
    size_t request_id_len;
    const char *user_id;
    size_t user_id_len;
    const char *text;
    size_t text_len;
    const char *voice_id;
    size_t voice_id_len;
    const char *response_subject;
    size_t response_subject_len;
    int32_t segment_index;
    int is_final;
    int32_t output_sample_rate;
    int32_t output_channels;
    int32_t output_bit_depth;
    int32_t output_encoding;
    uint32_t query_hash;
    int64_t first_text_at_ms;
    int64_t segment_emitted_at_ms;
    int stream_finality_deferred;
    turn_input_stage_timestamps_c input_stages;
} turn_tts_segment_view_c;

size_t pb_encode_turn_tts_segment(
    uint8_t *out,
    size_t out_cap,
    const turn_tts_segment_c *in
);
/* Encode a TTS segment from validated borrowed spans without string copies. */
size_t pb_encode_turn_tts_segment_prepared(
    uint8_t *out,
    size_t out_cap,
    const turn_tts_segment_view_c *in
);
int pb_decode_turn_tts_segment(
    const uint8_t *data,
    size_t len,
    turn_tts_segment_c *out
);
int pb_decode_turn_tts_segment_view(
    const uint8_t *data,
    size_t len,
    turn_tts_segment_view_c *out
);
/* Decode canonical traffic directly and validate the TTS dispatch identifier.
 * Compatible protobuf shapes use the general decoder as a fallback. */
int pb_decode_turn_tts_segment_view_admitted(
    const uint8_t *data,
    size_t len,
    turn_tts_segment_view_c *out,
    uint32_t *request_hash
);

typedef struct {
    char request_id[128];
    char user_id[128];
    char query[2048];
    char collection[512];
    int32_t top_k;
    int32_t rerank_top_k;
    int enable_rerank;
    char session_id[128];
    char knowledge_scope[32];
    char campaign_id[128];
    char character_id[128];
    int premium;
    int64_t deadline_unix_ms;
} rag_search_request_c;

size_t pb_encode_rag_search_request(
    uint8_t *out,
    size_t out_cap,
    const rag_search_request_c *in
);
int pb_decode_rag_search_request(
    const uint8_t *data,
    size_t len,
    rag_search_request_c *out
);

typedef struct {
    char request_id[128];
    char context_text[2048];
    int used_rag;
    char error[256];
    dnd_rag_result documents;
} rag_search_response_c;

size_t pb_encode_retrieval_citation(
    uint8_t *out, size_t out_cap, const dnd_rag_citation *in);
int pb_decode_retrieval_citation(
    const uint8_t *data, size_t len, dnd_rag_citation *out);

size_t pb_encode_rag_search_response(
    uint8_t *out,
    size_t out_cap,
    const rag_search_response_c *in
);
int pb_decode_rag_search_response(
    const uint8_t *data,
    size_t len,
    rag_search_response_c *out
);

typedef struct {
    char role[16];
    char content[4096];
    int64_t timestamp_ms;
    char request_id[128];
} session_message_c;

typedef struct {
    char session_id[128];
    char user_id[128];
    session_message_c message;
    const uint8_t *message_wire;
    size_t message_wire_len;
} session_append_request_c;

typedef struct {
    char session_id[128];
    char user_id[128];
    /* Borrowed from the input buffer and valid for the input lifetime. */
    const uint8_t *message_wire;
    size_t message_wire_len;
} session_append_view_c;

int pb_decode_session_message(
    const uint8_t *data,
    size_t len,
    session_message_c *out
);
size_t pb_encode_session_message(
    uint8_t *out,
    size_t out_cap,
    const session_message_c *in
);
size_t pb_encode_session_append_request(
    uint8_t *out,
    size_t out_cap,
    const session_append_request_c *in
);
int pb_decode_session_append_request(
    const uint8_t *data,
    size_t len,
    session_append_request_c *out
);
int pb_decode_session_append_view(
    const uint8_t *data,
    size_t len,
    session_append_view_c *out
);
size_t pb_encode_session_append_response(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    int32_t message_count
);
typedef struct {
    char session_id[128];
    int32_t message_count;
} session_append_response_c;
int pb_decode_session_append_response(
    const uint8_t *data,
    size_t len,
    session_append_response_c *out
);

typedef struct {
    char session_id[128];
    char user_id[128];
    int32_t last_n;
} session_get_request_c;

int pb_decode_session_get_request(
    const uint8_t *data,
    size_t len,
    session_get_request_c *out
);
size_t pb_encode_session_get_request(
    uint8_t *out,
    size_t out_cap,
    const session_get_request_c *in
);
/* Encodes field 1. Canonical encoded SessionMessage field-2 records may be
 * appended to the returned offset by the bounded store. */
size_t pb_encode_session_get_response_prefix(
    uint8_t *out,
    size_t out_cap,
    const char *session_id
);
typedef struct {
    char session_id[128];
    session_message_c first_message;
    session_message_c last_message;
    size_t message_count;
} session_get_response_c;
int pb_decode_session_get_response(
    const uint8_t *data,
    size_t len,
    session_get_response_c *out
);

#define SESSION_HISTORY_MAX 8u
typedef struct {
    char session_id[128];
    session_message_c messages[SESSION_HISTORY_MAX];
    size_t count;
} session_history_response_c;

/* Bounded recent user/assistant messages. System/tool roles are not admitted. */
int pb_decode_session_history_response(const uint8_t *data, size_t len,
    session_history_response_c *out);

typedef struct {
    char session_id[128];
    char user_id[128];
} session_id_request_c;

int pb_decode_session_id_request(
    const uint8_t *data,
    size_t len,
    session_id_request_c *out
);
size_t pb_encode_session_id_request(
    uint8_t *out,
    size_t out_cap,
    const session_id_request_c *in
);
size_t pb_encode_session_delete_response(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    int deleted
);
typedef struct {
    char session_id[128];
    int deleted;
} session_delete_response_c;
int pb_decode_session_delete_response(
    const uint8_t *data,
    size_t len,
    session_delete_response_c *out
);
size_t pb_encode_session_summary_response(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    const char *user_id,
    const char *summary,
    const char *source,
    int64_t updated_at_ms
);
typedef struct {
    char session_id[128];
    char user_id[128];
    char summary[512];
    char source[128];
    int64_t updated_at_ms;
} session_summary_response_c;
int pb_decode_session_summary_response(
    const uint8_t *data,
    size_t len,
    session_summary_response_c *out
);

/* Encode canonical TurnEvent: request_id(1), enum type(3), text(5). */
/* Append a provider-reported model name to an already encoded event. */
size_t pb_append_turn_provider_model(uint8_t *out, size_t capacity, size_t used, const char *model);

size_t pb_encode_turn_event(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text
);

/* Encode the fixed route event shape from validated one-byte-length spans. */
size_t pb_encode_turn_route_event_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    const char *route_text,
    size_t route_text_len
);

/* Fixed, content-free timing keys carried in TurnEvent metadata field 16. */
typedef struct {
    turn_input_stage_timestamps_c input;
    int64_t first_text_at_ms;
    int64_t tts_segment_emitted_at_ms;
    int64_t tts_request_received_at_ms;
    int64_t tts_provider_request_started_at_ms;
    int64_t tts_provider_ready_at_ms;
    int64_t pcm_started_at_ms;
    int64_t pcm_first_chunk_at_ms;
} turn_stage_timestamps_c;

enum { TURN_STAGE_WIRE_CAPACITY = 768 };

/* Prepared canonical metadata fields for two adjacent TTS stage events. */
typedef struct {
    uint8_t data[TURN_STAGE_WIRE_CAPACITY];
    size_t len;
    int64_t request_received_at_ms;
    int64_t pcm_started_at_ms;
    int complete;
    int current_tts_template;
} turn_stage_wire_c;

int pb_prepare_turn_stage_wire(
    turn_stage_wire_c *prepared,
    const turn_stage_timestamps_c *stages
);

int pb_complete_turn_stage_wire(
    turn_stage_wire_c *prepared,
    int64_t pcm_first_chunk_at_ms
);

size_t pb_encode_turn_event_stages(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const turn_stage_timestamps_c *stages
);

/* The prepared stage object must come from the prepare/complete functions. */
size_t pb_encode_turn_event_stage_wire(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const turn_stage_wire_c *prepared
);

/* Encode canonical pcm_started(7) or pcm_ended(9) from a validated ID span.
 * pcm_started requires prepared stages. pcm_ended rejects prepared stages. */
size_t pb_encode_turn_pcm_boundary_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    int type_id,
    const turn_stage_wire_c *prepared
);

/* Encode canonical text channels and segment/finality metadata. */
size_t pb_encode_turn_text_event(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *legacy_text,
    const char *speech_text,
    const char *display_text,
    int32_t segment_index,
    int is_final
);

/* The caller proves the event type and each text span are canonical and bounded. */
size_t pb_encode_turn_text_event_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    int type_id,
    const char *legacy_text,
    size_t legacy_text_len,
    const char *speech_text,
    size_t speech_text_len,
    const char *display_text,
    size_t display_text_len,
    int32_t segment_index,
    int is_final
);

/* Encode a canonical PCM TurnEvent including audio/format/sequence fields. */
size_t pb_encode_turn_audio_event(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final
);

size_t pb_encode_turn_audio_event_stages(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_timestamps_c *stages
);

size_t pb_encode_turn_audio_event_stage_wire(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_wire_c *prepared
);

/* Encode one canonical raw-PCM chunk from a validated request identifier span. */
size_t pb_encode_turn_pcm_chunk_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_wire_c *prepared
);

/* Encode a canonical PCM chunk around borrowed audio bytes.
 * Concatenating prefix, audio, and suffix produces the canonical wire body. */
size_t pb_encode_turn_pcm_chunk_spans_prepared(
    uint8_t *prefix,
    size_t prefix_cap,
    size_t *prefix_len,
    uint8_t *suffix,
    size_t suffix_cap,
    size_t *suffix_len,
    const char *request_id,
    size_t request_id_len,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_wire_c *prepared
);

typedef struct {
    uint8_t prefix[134];
    uint8_t segment[6];
    uint16_t prefix_len;
    uint16_t audio_len;
    uint8_t segment_len;
} turn_pcm_chunk_current_wire_c;

/* Prepare the invariant prefix and segment scalar for one current PCM stream. */
int pb_prepare_turn_pcm_chunk_current_wire(
    turn_pcm_chunk_current_wire_c *wire,
    const char *request_id,
    size_t request_id_len,
    size_t audio_len,
    int32_t segment_index
);

/* Encode one changing suffix and return the complete three-span body length. */
size_t pb_encode_turn_pcm_chunk_current_suffix_prepared(
    const turn_pcm_chunk_current_wire_c *wire,
    uint8_t *suffix,
    size_t suffix_cap,
    size_t *suffix_len,
    int32_t sequence,
    int is_final,
    const turn_stage_wire_c *prepared
);

/* Encode one suffix after the caller admits its prepared stream state.
 * The wire must be an unchanged successful preparation. The sequence must be
 * in [0, 16383], and the optional stage wire must be valid and complete. */
size_t pb_encode_turn_pcm_chunk_current_suffix_admitted(
    const turn_pcm_chunk_current_wire_c *wire,
    uint8_t *suffix,
    size_t suffix_cap,
    size_t *suffix_len,
    int32_t sequence,
    int is_final,
    const turn_stage_wire_c *prepared
);

typedef struct {
    char tool_id[32];
    char tool_call_id[80];
    char output_sha256[65];
    int64_t elapsed_ms;
    int present;
} turn_tool_result_c;

int pb_turn_tool_result_valid(const turn_tool_result_c *tool);
size_t pb_append_turn_tool_result(uint8_t *out, size_t capacity, size_t used,
                                  const turn_tool_result_c *tool);

/* Public encounter snapshots use bounded borrowed wire, not a combat-state copy.
 * Larger tool states fail this product boundary without a partial projection. */
#define DND_TURN_PARTICIPANTS_MAX 64u
#define DND_TURN_ENCOUNTER_MAX 8192u
typedef struct { const uint8_t *data; size_t length; } turn_encounter_c;
typedef struct {
    char id[65], name[121], conditions[16][32];
    int32_t initiative, max_hp, current_hp;
    size_t condition_count;
} turn_encounter_participant_c;
typedef struct {
    char campaign_id[65], encounter_id[65], status[16], active_participant_id[65];
    char tool_call_id[80], output_sha256[65], operation[24];
    int64_t version;
    int32_t round, active_index;
    size_t participant_count;
    turn_encounter_c wire;
} turn_encounter_state_c;
int pb_decode_turn_encounter(const uint8_t *data, size_t length, turn_encounter_state_c *out);
size_t pb_encode_turn_encounter(uint8_t *out, size_t capacity, const turn_encounter_state_c *state,
                                const turn_encounter_participant_c *participants);
/* Iterate field 8 from a validated snapshot; return 1, 0 at end, or -1. */
int pb_encounter_participant_next(pb_reader *reader, turn_encounter_participant_c *out);
size_t pb_append_turn_encounter(uint8_t *out, size_t capacity, size_t used,
                               const turn_encounter_c *encounter);

#define DND_TURN_CHARACTERS_MAX 200u
#define DND_TURN_ROSTER_MAX 65536u
#define DND_TURN_INITIATIVE_MAX 8192u
#define DND_TOOL_EVENT_MAX 131072u
typedef struct { const uint8_t *data; size_t length; } turn_roster_c;
typedef struct { const uint8_t *data; size_t length; } turn_initiative_c;
typedef struct {
    char id[65], name[201], kind[16];
    int32_t max_hp;
} turn_campaign_character_c;
typedef struct {
    char campaign_id[65], status[16], tool_call_id[80], output_sha256[65];
    int64_t version;
    size_t character_count;
    turn_roster_c wire;
} turn_campaign_roster_c;
typedef struct {
    char character_id[65], expression[16];
    uint32_t rolls[2], kept_index;
    size_t roll_count;
    int32_t total;
} turn_initiative_roll_c;
typedef struct {
    int64_t campaign_version;
    char campaign_sha256[65];
    size_t roll_count;
    turn_initiative_c wire;
} turn_initiative_result_c;
int pb_decode_turn_roster(const uint8_t *data, size_t length, turn_campaign_roster_c *out);
int pb_roster_character_next(pb_reader *reader, turn_campaign_character_c *out);
size_t pb_encode_turn_roster(uint8_t *out, size_t capacity, const turn_campaign_roster_c *roster,
                             const turn_campaign_character_c *characters);
size_t pb_append_turn_roster(uint8_t *out, size_t capacity, size_t used, const turn_roster_c *roster);
int pb_decode_turn_initiative(const uint8_t *data, size_t length, turn_initiative_result_c *out);
int pb_initiative_roll_next(pb_reader *reader, turn_initiative_roll_c *out);
size_t pb_encode_turn_initiative(uint8_t *out, size_t capacity, const turn_initiative_result_c *result,
                                 const turn_initiative_roll_c *rolls);
size_t pb_append_turn_initiative(uint8_t *out, size_t capacity, size_t used, const turn_initiative_c *initiative);

/* Bounded provenance borrowed from the event wire until that wire changes. */
typedef struct {
    const uint8_t *data;
    size_t length;
    size_t count;
} turn_retrieval_c;

size_t pb_encode_retrieval_provenance(uint8_t *out, size_t capacity, const dnd_rag_result *result);
size_t pb_encode_grounded_retrieval_provenance(uint8_t *out, size_t capacity,
    const dnd_rag_result *result, const dnd_grounding_identity *identity);
int pb_decode_retrieval_provenance(const uint8_t *data, size_t length, turn_retrieval_c *out);
int pb_grounding_identity_valid(const dnd_grounding_identity *identity);
/* Returns 1 when present, 0 when absent, and -1 for invalid provenance. */
int pb_retrieval_grounding(const turn_retrieval_c *retrieval, dnd_grounding_identity *identity);
/* Return one citation, zero at the end, or -1 on malformed input. */
int pb_retrieval_citation_next(pb_reader *reader, dnd_rag_citation *citation);
size_t pb_append_turn_retrieval(uint8_t *out, size_t capacity, size_t used,
                                const turn_retrieval_c *retrieval);

/* Only explicitly sourced provider metadata crosses the public boundary. */
typedef struct {
    char model[256], model_source[24];
    char tokens[3][24], usage_source[24];
    unsigned seen, invalid;
} turn_provider_c;
int pb_turn_provider_usage(const turn_provider_c *provider, uint64_t counts[3]);
size_t pb_append_turn_provider_usage(uint8_t *out, size_t capacity, size_t used,
    uint64_t prompt, uint64_t completion, uint64_t total);

/* Decode TurnEvent subset. */
typedef struct {
    char request_id[128];
    char type[32];
    int type_id;
    char text[2048];
    const uint8_t *audio;
    size_t audio_len;
    int32_t sample_rate;
    int32_t channels;
    int32_t bit_depth;
    int32_t sequence;
    int32_t segment_index;
    int is_final;
    int32_t audio_encoding;
    char speech_text[2048];
    union {
        char display_text[2048];
        const char *display_text_view;
    };
    size_t display_text_len;
    int display_text_len_known;
    uint8_t display_text_borrowed;
    /* Exact current TTS stage wire borrowed by the public decoder. */
    const uint8_t *current_tts_stage_wire;
    turn_stage_timestamps_c stages;
    turn_provider_c provider;
    turn_tool_result_c tool_result;
    turn_retrieval_c retrieval;
    turn_encounter_c encounter;
    turn_roster_c roster;
    turn_initiative_c initiative;
} turn_event_c;

int pb_decode_turn_event(const uint8_t *data, size_t len, turn_event_c *out);

/* Decode and bind field 1 to one validated active request identifier.
 * On success, request_id stays empty because the caller already owns that value. */
int pb_decode_turn_event_bound(
    const uint8_t *data,
    size_t len,
    const char *request_id,
    size_t request_id_len,
    turn_event_c *out
);

/* Decode an event for an admitted request with a cached identifier length.
 * Debug builds assert the trusted caller contract. */
int pb_decode_turn_event_active(
    const uint8_t *data,
    size_t len,
    const char *request_id,
    size_t request_id_len,
    turn_event_c *out
);

/* Decode for an authenticated public edge.
 * Private legacy and speech text remain empty after full wire validation.
 * Display text and exact stage wire can borrow the input wire.
 * Borrowed spans expire when the caller changes that wire. */
int pb_decode_turn_event_public_active(
    const uint8_t *data,
    size_t len,
    const char *request_id,
    size_t request_id_len,
    turn_event_c *out
);

#ifdef __cplusplus
}
#endif

#endif
