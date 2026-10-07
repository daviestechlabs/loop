/* Bounded model-I/O JSON helpers with no full JSON library. */
#ifndef VOICE_C_OPENAI_MIN_H
#define VOICE_C_OPENAI_MIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keep this marker aligned with the reviewed serving image. */
#define VOICE_VLLM_API_CONTRACT_VERSION "0.29.0"

int json_escape_string(
    const char *in,
    char *out,
    size_t out_cap,
    size_t *out_len
);

/*
 * Build chat/completions JSON body into out (NUL-terminated if room).
 * model + user message only. Thinking is always locked off so the
 * planner/classifier owns RAG, not a chain-of-thought loop.
 * Returns length or 0 on failure.
 */
size_t openai_chat_request_json(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text
);

/* Streaming variant for OpenAI-compatible chat/completions. */
size_t openai_chat_request_json_stream(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text
);

/* Streaming variant with one caller-owned completion-token bound. */
size_t openai_chat_request_json_stream_bounded(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text,
    uint32_t max_completion_tokens
);

/* Build the bounded streaming body from an exact, terminated user-text span. */
size_t openai_chat_request_json_stream_bounded_span(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text,
    size_t user_text_len,
    uint32_t max_completion_tokens
);

/* A turn owner may admit a larger, explicitly bounded retrieval context.
 * input_limit is 1..65535 bytes; output capacity remains a separate bound. */
size_t openai_chat_request_json_stream_bounded_span_limit(
    char *out, size_t out_cap, const char *model, const char *user_text,
    size_t user_text_len, uint32_t max_completion_tokens, size_t input_limit);

/* A canonical system message followed by one bounded user/context message.
 * Both spans must be exact and terminated. System text is at most 4095 bytes. */
size_t openai_chat_request_json_stream_system(
    char *out, size_t out_cap, const char *model,
    const char *system_text, size_t system_text_len,
    const char *user_text, size_t user_text_len,
    uint32_t max_completion_tokens, size_t input_limit);

typedef struct {
    const char *role;
    const char *content;
    size_t content_len;
} openai_history_message;

/* History is source data, never a privileged role. At most eight messages;
 * the total history plus current user content must fit input_limit. */
size_t openai_chat_request_json_stream_history(
    char *out, size_t out_cap, const char *model,
    const char *system_text, size_t system_text_len,
    const openai_history_message *history, size_t history_count,
    const char *user_text, size_t user_text_len,
    uint32_t max_completion_tokens, size_t input_limit);

/* Build the native Orpheus PCM request body without temporary escape buffers. */
size_t tts_pcm_request_json(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
);

/* Build the same body from exact, terminated input spans. */
size_t tts_pcm_request_json_spans(
    char *out,
    size_t out_cap,
    const char *text,
    size_t text_len,
    const char *voice,
    size_t voice_len,
    const char *turn_id,
    size_t turn_id_len,
    uint32_t query_hash
);

/* The caller must prove turn_id contains only JSON-plain, non-NUL bytes. */
size_t tts_pcm_request_json_prepared_id_spans(
    char *out,
    size_t out_cap,
    const char *text,
    size_t text_len,
    const char *voice,
    size_t voice_len,
    const char *turn_id,
    size_t turn_id_len,
    uint32_t query_hash
);

/* turn_id must be JSON plain. text_json_plain requires the same proof. */
size_t tts_pcm_request_json_prepared_spans(
    char *out,
    size_t out_cap,
    const char *text,
    size_t text_len,
    const char *voice,
    size_t voice_len,
    const char *turn_id,
    size_t turn_id_len,
    uint32_t query_hash,
    int text_json_plain
);

typedef int (*openai_delta_callback)(
    const char *content,
    size_t content_len,
    void *user
);

enum {
    OPENAI_FINISH_NONE = 0,
    OPENAI_FINISH_STOP = 1,
    OPENAI_FINISH_LENGTH = 2
};

/* Server limit is 1..4096. Optional canonical decimal metadata is 1..1000000
 * and can only lower that limit. Returns zero for invalid input. */
uint32_t openai_completion_limit(uint32_t server_limit, const char *requested);

/* Incremental SSE decoder for chat completion deltas. The decoder accepts
 * arbitrary HTTP-body chunk boundaries, requires a finish reason before the
 * terminal data: [DONE], and rejects oversized or malformed data events. */
typedef struct openai_sse_decoder {
    char line[16384];
    char content[8192];
    size_t line_len;
    /* A validated prefix remains in line until the next line replaces it. */
    size_t vllm_line_prefix_len;
    openai_delta_callback callback;
    void *callback_user;
    int vllm_prefix_match;
    int finish_reason; /* OPENAI_FINISH_*; length is a valid but incomplete stream. */
    int done;
    int failed;
    /* Provider-reported name, not a registry or weights attestation. */
    char reported_model[256];
    int model_conflict;
    uint64_t prompt_tokens, completion_tokens, total_tokens;
    int usage_present, usage_invalid;
} openai_sse_decoder;

int openai_sse_init(
    openai_sse_decoder *decoder,
    openai_delta_callback callback,
    void *callback_user
);
int openai_sse_feed(openai_sse_decoder *decoder, const char *data, size_t data_len);
int openai_sse_finish(openai_sse_decoder *decoder);

/*
 * Extract first assistant message content from chat completions JSON body.
 * Handles simple escaped quotes/newlines. Returns 0 on success.
 */
int openai_chat_extract_content(
    const char *json,
    size_t json_len,
    char *out,
    size_t out_cap,
    size_t *out_len
);

/* Extract and unescape one non-empty JSON string value by exact field name.
 * The parser is length-bounded and does not require a trailing NUL. */
int json_extract_string_field(
    const char *json,
    size_t json_len,
    const char *field,
    char *out,
    size_t out_cap,
    size_t *out_len
);

/* Validate the canonical top-level STT response and extract its transcript.
 * The text and transcript aliases must match when both are present. */
int stt_json_extract_transcript(
    const char *json,
    size_t json_len,
    char *out,
    size_t out_cap,
    size_t *out_len
);

#ifdef __cplusplus
}
#endif

#endif
