/*
 * Pure-C cascade-router service.
 * Subscribes: ai.turn.start, ai.turn.token, ai.turn.cancel
 * Product math: sanitize + streaming segmenter host + phatic + route classifier.
 * Speakable segments → tts_segment events + ai.turn.tts.speak glue.
 */
#define _POSIX_C_SOURCE 200809L

#include "../common/service.h"
#include "../common/dnd_tools.h"
#include "../common/dnd_dialogue.h"
#include "../common/http_min.h"
#include "../common/openai_min.h"
#include "../common/rag_select.h"
#include "../common/vbus_subject.h"
#include "../common/voice_ascii.h"
#include "../common/dnd_retrieval.h"
#include "../common/dnd_grounding.h"
#include "../common/response_style.h"
#include "../common/loop_rules.h"
#include "../common/loop_scene.h"
#include "../common/model_request_capture.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"

#include "phatic_policy.h"
#include "route_classifier.h"
#include "systemone.h"
#include "speech_display_stream.h"
#include "speech_sanitize.h"
#include "speech_markup.h"
#include "speech_seg_host.h"
#include "speech_segment.h"
#include "turn_budget.h"

#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>

#define CR_MAX_STREAMS 32
#define CR_LLM_WORKER_LANES 4
#define CR_LLM_QUEUE_PER_LANE 8
#define CR_LLM_JOBS_PER_LANE (CR_LLM_QUEUE_PER_LANE + 1)
#define CR_LLM_RESPONSE_MAX (4u * 1024u * 1024u)
#define CR_HTTP_PRECONNECT_TIMEOUT_MS 250
#define CR_CANCEL_TOMBSTONES 64
#define CR_CANCEL_TTL_NS (10ull * 1000000000ull)
#define CR_DEFAULT_STREAM_IDLE_TIMEOUT_MS 30000
#define CR_MAX_STREAM_IDLE_TIMEOUT_MS 300000
#define CR_DEFAULT_FIRST_SEGMENT_CHARS 5
#define CR_MIN_FIRST_SEGMENT_CHARS 5
#define CR_MIN_SEGMENT_CHARS 8
#define CR_MAX_FIRST_SEGMENT_CHARS 80

#if defined(__GNUC__) || defined(__clang__)
#define CR_NOINLINE __attribute__((noinline))
#else
#define CR_NOINLINE
#endif

typedef struct cr_llm_job cr_llm_job;
typedef struct cr_state cr_state;

typedef struct {
    char request_id[128];
    uint64_t expires_ns;
} cr_cancel_tombstone;

typedef struct {
    uint64_t anchor_mono_ms;
    int64_t anchor_at_ms;
    turn_input_stage_timestamps_c input;
    int64_t first_text_at_ms;
} cr_stage_clock;

typedef struct {
    char request_id[128];
    size_t request_id_len;
    char response_subject[320];
    size_t response_subject_len;
    speech_seg_host_v1 host;
    speech_markup_v1 markup;
    char pending_segment[SPEECH_SEG_HOST_MAX_PART];
    size_t pending_segment_len;
    char full_text[2048];
    size_t full_text_len;
    uint64_t expires_ns;
    int32_t next_segment_index;
    int has_pending_segment;
    int eager_tts_segments;
    int enable_tts;
    int in_use;
    cr_stage_clock stages;
} cr_stream;

struct cr_llm_job {
    char request_id[128];
    char user_id[128];
    char session_id[128];
    char response_subject[320];
    char prompt[2048];
    char knowledge_scope[32];
    char campaign_id[128];
    char scene_id[65];
    char character_id[128];
    char encounter_id[128];
    size_t prompt_len;
    uint32_t max_completion_tokens;
    int enable_tts;
    int enable_rag;
    int premium;
    int model_request_capture;
    int tool_turn;
    dnd_initiative_request_c *initiative;
    dnd_campaign_request_c *campaign_request;
    dnd_encounter_action_c *encounter_action;
    int64_t deadline_ms;
    turn_input_stage_timestamps_c input_stages;
    atomic_int canceled;
    dnd_pending_ticket pending_ticket;
    dnd_pending_view *dialogue;
    int dialogue_command;
    int dnd_profile;
    int loop_profile;
};

_Static_assert(sizeof(cr_llm_job) <= 4096u, "cascade turn job exceeds its bound");
_Static_assert(
    CR_CANCEL_TOMBSTONES <= 64,
    "cascade cancel occupancy exceeds its word");
_Static_assert(
    sizeof(((cr_cancel_tombstone *)0)->request_id) - 1u <= UINT8_MAX,
    "cascade cancel request length exceeds its byte");

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    uint8_t queue[CR_LLM_QUEUE_PER_LANE];
    size_t queue_head;
    cr_llm_job *active;
    size_t queued;
    cr_llm_job jobs[CR_LLM_JOBS_PER_LANE];
    uint8_t free_stack[CR_LLM_JOBS_PER_LANE];
    size_t free_count;
    int stop;
    int initialized;
    int thread_started;
    vbus_client *nc;
    cr_state *owner;
    http_min_client http;
    char grounded[DND_RAG_PROMPT_CAP];
    char request_json[(DND_RAG_PROMPT_CAP + DND_GROUNDING_TEXT_CAP) * 6u + 1024u];
    uint8_t retrieval_wire[DND_RAG_CITATIONS_WIRE_CAP];
    size_t retrieval_len;
    char dialogue_prompt[DND_RAG_PROMPT_CAP];
    session_history_response_c loop_history;
} cr_llm_lane;

struct cr_state {
    vbus_client *nc;
    cr_stream streams[CR_MAX_STREAMS];
    cr_llm_lane lanes[CR_LLM_WORKER_LANES];
    char llm_url[512];
    char llm_model[128];
    dnd_grounding_prompt grounding;
    voice_response_prompts response_prompts;
    char tool_url[512];
    char tool_secret[256];
    int tool_timeout_ms;
    speech_seg_config_v1 segment_config;
    int llm_timeout_ms;
    int rag_timeout_ms;
    uint32_t llm_max_completion_tokens;
    uint32_t grounded_max_completion_tokens;
    int eager_tts_segments;
    int64_t default_turn_budget_ms;
    int default_turn_budget_invalid;
    int workers_started;
    uint64_t stream_idle_ns;
    uint64_t next_stream_maintenance_ns;
    cr_cancel_tombstone canceled[CR_CANCEL_TOMBSTONES];
    uint64_t canceled_occupied;
    uint8_t canceled_lengths[CR_CANCEL_TOMBSTONES];
    size_t canceled_next;
    dnd_pending_store pending;
    pthread_mutex_t pending_mutex;
};

static uint64_t mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void finish_pending(cr_state *st, dnd_pending_ticket ticket) {
    if (!ticket.generation) return;
    pthread_mutex_lock(&st->pending_mutex);
    (void)dnd_pending_finish(&st->pending, ticket, mono_ns() / UINT64_C(1000000));
    pthread_mutex_unlock(&st->pending_mutex);
}

static int current_pending(cr_state *st, cr_llm_job *job, uint64_t scene_revision) {
    dnd_pending_view ignored;
    int rc;
    if (!job->pending_ticket.generation) return 1;
    pthread_mutex_lock(&st->pending_mutex);
    uint64_t now = mono_ns() / UINT64_C(1000000);
    rc = scene_revision ? dnd_pending_observe_scene(&st->pending, job->pending_ticket,
        now, scene_revision) : dnd_pending_read(&st->pending, job->pending_ticket, now, &ignored);
    pthread_mutex_unlock(&st->pending_mutex);
    if (rc < 0) atomic_store_explicit(&job->canceled, 1, memory_order_relaxed);
    return rc >= 0;
}

static uint64_t stream_deadline(const cr_state *st, uint64_t now_ns) {
    if (!st || now_ns == 0 || st->stream_idle_ns == 0) return 0;
    if (now_ns > UINT64_MAX - st->stream_idle_ns) return UINT64_MAX;
    return now_ns + st->stream_idle_ns;
}

static void stream_schedule_maintenance(cr_state *st, const cr_stream *stream) {
    uint64_t deadline;
    uint64_t flush_deadline = 0u;
    if (!st || !stream || !stream->in_use) return;
    deadline = stream->expires_ns;
    if (stream->host.len != 0u && stream->host.last_append_ns != 0u) {
        if (stream->host.last_append_ns > UINT64_MAX - stream->host.flush_timeout_ns)
            flush_deadline = 1u;
        else
            flush_deadline = stream->host.last_append_ns + stream->host.flush_timeout_ns;
        if (deadline == 0u || flush_deadline < deadline) deadline = flush_deadline;
    }
    if (deadline != 0u &&
        (st->next_stream_maintenance_ns == 0u ||
         deadline < st->next_stream_maintenance_ns))
        st->next_stream_maintenance_ns = deadline;
}

static int64_t now_unix_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0 ||
        (uint64_t)ts.tv_sec > (uint64_t)INT64_MAX / UINT64_C(1000)) return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000) +
        (int64_t)ts.tv_nsec / INT64_C(1000000);
}

static int extract_turn_budget(
    const cr_state *st,
    const turn_start_c *turn,
    int64_t now_ms,
    turn_budget_info_v1 *info
) {
    const char *keys[2];
    const char *values[2];
    size_t count = 0;
    if (!st || !turn || !info || now_ms <= 0) return -1;
    if (turn->has_meta_budget) {
        keys[count] = TURN_BUDGET_KEY_MS;
        values[count] = turn->meta_budget_ms;
        count++;
    }
    if (turn->has_meta_deadline) {
        keys[count] = TURN_BUDGET_KEY_DEADLINE_UNIX_MS;
        values[count] = turn->meta_deadline_unix_ms;
        count++;
    }
    if (turn_budget_extract_v1(keys, values, count, now_ms, info) !=
        TURN_BUDGET_OK) return -1;
    if (info->invalid_budget_ms || info->invalid_deadline_unix_ms) return -1;
    if (!info->has_deadline) {
        if (st->default_turn_budget_invalid) return -1;
        if (st->default_turn_budget_ms > 0 &&
            turn_budget_apply_default_ms_v1(
                info, st->default_turn_budget_ms, now_ms) != TURN_BUDGET_OK)
            return -1;
    }
    return info->invalid_budget_ms || info->invalid_deadline_unix_ms ? -1 : 0;
}

static int stage_clock_init(cr_stage_clock *clock) {
    struct timespec mono;
    struct timespec real;
    if (!clock) return -1;
    memset(clock, 0, sizeof(*clock));
    if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0 ||
        clock_gettime(CLOCK_REALTIME, &real) != 0 || mono.tv_sec < 0 ||
        real.tv_sec < 0 ||
        (uint64_t)mono.tv_sec > UINT64_MAX / UINT64_C(1000) ||
        (uint64_t)real.tv_sec > (uint64_t)INT64_MAX / UINT64_C(1000))
        return -1;
    clock->anchor_mono_ms =
        (uint64_t)mono.tv_sec * UINT64_C(1000) + (uint64_t)(mono.tv_nsec / 1000000L);
    clock->anchor_at_ms =
        (int64_t)real.tv_sec * INT64_C(1000) + (int64_t)(real.tv_nsec / 1000000L);
    return clock->anchor_mono_ms != 0 && clock->anchor_at_ms > 0 ? 0 : -1;
}

static int64_t stage_clock_now_ms(const cr_stage_clock *clock) {
    uint64_t now_ms;
    uint64_t elapsed;
    uint64_t now_ns;
    if (!clock || clock->anchor_mono_ms == 0 || clock->anchor_at_ms <= 0) return 0;
    now_ns = mono_ns();
    if (now_ns == 0) return 0;
    now_ms = now_ns / UINT64_C(1000000);
    if (now_ms < clock->anchor_mono_ms) return 0;
    elapsed = now_ms - clock->anchor_mono_ms;
    if (elapsed > (uint64_t)(INT64_MAX - clock->anchor_at_ms)) return 0;
    return clock->anchor_at_ms + (int64_t)elapsed;
}

static int stage_clock_mark_first_text(cr_stage_clock *clock) {
    if (!clock) return -1;
    if (clock->first_text_at_ms > 0) return 0;
    clock->first_text_at_ms = stage_clock_now_ms(clock);
    return clock->first_text_at_ms > 0 ? 0 : -1;
}

static CR_NOINLINE int copy_bounded_string(
    char *out,
    size_t out_cap,
    const char *input
);

static int copy_bounded_span(
    char *out,
    size_t out_cap,
    const char *input,
    size_t input_len
) {
    if (!out || !input || input_len >= out_cap) return -1;
    memcpy(out, input, input_len);
    out[input_len] = '\0';
    return 0;
}

static void remember_early_cancel(cr_state *st, const char *request_id) {
    cr_cancel_tombstone *entry;
    size_t request_id_len;
    uint64_t slot_bit;
    if (!st || !request_id || !request_id[0]) return;
    request_id_len = strnlen(
        request_id,
        sizeof(((cr_cancel_tombstone *)0)->request_id));
    if (request_id_len == 0u ||
        request_id_len >= sizeof(((cr_cancel_tombstone *)0)->request_id))
        return;
    entry = &st->canceled[st->canceled_next];
    slot_bit = UINT64_C(1) << st->canceled_next;
    memset(entry, 0, sizeof(*entry));
    st->canceled_lengths[st->canceled_next] = 0u;
    st->canceled_occupied &= ~slot_bit;
    if (copy_bounded_span(
            entry->request_id,
            sizeof(entry->request_id),
            request_id,
            request_id_len) != 0) return;
    entry->expires_ns = mono_ns() + CR_CANCEL_TTL_NS;
    st->canceled_lengths[st->canceled_next] = (uint8_t)request_id_len;
    st->canceled_occupied |= slot_bit;
    st->canceled_next = (st->canceled_next + 1u) % CR_CANCEL_TOMBSTONES;
}

static int consume_early_cancel(
    cr_state *st,
    const char *request_id,
    size_t request_id_len
) {
    uint64_t occupied;
    uint64_t now;
    size_t i;
    if (!st || !request_id || request_id_len == 0u) return 0;
    occupied = st->canceled_occupied;
    if (occupied == 0u) return 0;
    now = mono_ns();
    for (i = 0; occupied != 0u; ++i, occupied >>= 1u) {
        cr_cancel_tombstone *entry = &st->canceled[i];
        uint64_t slot_bit;
        if ((occupied & UINT64_C(1)) == 0u) continue;
        slot_bit = UINT64_C(1) << i;
        if (entry->expires_ns <= now) {
            memset(entry, 0, sizeof(*entry));
            st->canceled_lengths[i] = 0u;
            st->canceled_occupied &= ~slot_bit;
            continue;
        }
        if ((size_t)st->canceled_lengths[i] == request_id_len &&
            memcmp(entry->request_id, request_id, request_id_len) == 0) {
            memset(entry, 0, sizeof(*entry));
            st->canceled_lengths[i] = 0u;
            st->canceled_occupied &= ~slot_bit;
            return 1;
        }
    }
    return 0;
}

static int publish_event(vbus_client *nc, const char *subject, const char *rid, const char *type,
                         const char *text) {
    uint8_t buf[4096];
    size_t n = pb_encode_turn_event(buf, sizeof(buf), rid, type, text);
    if (!nc || n == 0 || !subject || !subject[0]) return -1;
    return vbus_publish(nc, subject, buf, n);
}

static int publish_route_event(
    vbus_client *nc,
    const char *subject,
    const char *request_id,
    size_t request_id_len,
    const char *route_text,
    size_t route_text_len
) {
    uint8_t buf[256];
    size_t n = pb_encode_turn_route_event_prepared(
        buf,
        sizeof(buf),
        request_id,
        request_id_len,
        route_text,
        route_text_len);
    if (!nc || n == 0u || !subject || !subject[0]) return -1;
    return vbus_publish(nc, subject, buf, n);
}

static int publish_model_failure(
    vbus_client *nc,
    const cr_llm_job *job,
    int tts_started,
    const char *message
) {
    uint8_t cancel_wire[512];
    uint8_t failure_wire[4096];
    size_t cancel_len;
    size_t failure_len;
    if (!nc || !job || !message) return -1;
    if (!tts_started)
        return publish_event(
            nc, job->response_subject, job->request_id, "failed", message);
    cancel_len = pb_encode_turn_cancel(
        cancel_wire,
        sizeof(cancel_wire),
        job->request_id,
        job->user_id,
        TURN_CANCEL_REASON_UPSTREAM_FAILURE);
    failure_len = pb_encode_turn_event(
        failure_wire,
        sizeof(failure_wire),
        job->request_id,
        "failed",
        message);
    if (cancel_len == 0 || failure_len == 0) return -1;
    return vbus_publish_pair(
        nc,
        SUBJ_TURN_CANCEL,
        cancel_wire,
        cancel_len,
        job->response_subject,
        failure_wire,
        failure_len);
}

static int publish_stream_failure(
    vbus_client *nc,
    const cr_stream *stream,
    const char *message
) {
    uint8_t cancel_wire[512];
    uint8_t failure_wire[4096];
    size_t cancel_len;
    size_t failure_len;
    if (!nc || !stream || !stream->request_id[0] ||
        !stream->response_subject[0] || !message) return -1;
    if (!stream->eager_tts_segments || stream->next_segment_index <= 0)
        return publish_event(
            nc,
            stream->response_subject,
            stream->request_id,
            "failed",
            message);
    cancel_len = pb_encode_turn_cancel(
        cancel_wire,
        sizeof(cancel_wire),
        stream->request_id,
        "",
        TURN_CANCEL_REASON_UPSTREAM_FAILURE);
    failure_len = pb_encode_turn_event(
        failure_wire,
        sizeof(failure_wire),
        stream->request_id,
        "failed",
        message);
    if (cancel_len == 0u || failure_len == 0u) return -1;
    return vbus_publish_pair(
        nc,
        SUBJ_TURN_CANCEL,
        cancel_wire,
        cancel_len,
        stream->response_subject,
        failure_wire,
        failure_len);
}

static size_t encode_text_event(
    uint8_t *wire,
    size_t wire_cap,
    const char *rid,
    size_t rid_len,
    int type_id,
    const char *model_text,
    size_t input_len,
    int32_t segment_index,
    int is_final,
    int canonical_ascii
) {
    char speech[2048];
    char display[2048];
    size_t speech_len = 0;
    size_t display_len = 0;
    if (!wire || !model_text) return 0;
    if (input_len < sizeof(speech) &&
        (canonical_ascii || speech_is_canonical_ascii_v1(model_text, input_len))) {
        return pb_encode_turn_text_event_prepared(
            wire,
            wire_cap,
            rid,
            rid_len,
            type_id,
            model_text,
            input_len,
            model_text,
            input_len,
            model_text,
            input_len,
            segment_index,
            is_final);
    }
    if (speech_sanitize_v1(
            model_text, input_len, 1, speech, sizeof(speech), &speech_len) !=
            SPEECH_SANITIZE_OK || speech_len == 0 || speech_len >= sizeof(speech)) return 0;
    if (memchr(model_text, '<', input_len) == NULL) {
        memcpy(display, speech, speech_len);
        display_len = speech_len;
    } else if (speech_sanitize_v1(
                   model_text,
                   input_len,
                   0,
                   display,
                   sizeof(display),
                   &display_len) != SPEECH_SANITIZE_OK ||
               display_len == 0 || display_len >= sizeof(display)) {
        return 0;
    }
    speech[speech_len] = '\0';
    display[display_len] = '\0';
    return pb_encode_turn_text_event_prepared(
        wire, wire_cap, rid, rid_len, type_id,
        display, display_len, speech, speech_len, display, display_len,
        segment_index, is_final);
}

static int publish_text_event(
    vbus_client *nc,
    const char *subject,
    const char *rid,
    size_t rid_len,
    int type_id,
    const char *model_text,
    int32_t segment_index,
    int is_final,
    int canonical_ascii
) {
    uint8_t wire[8192];
    size_t wire_len = encode_text_event(
        wire,
        sizeof(wire),
        rid,
        rid_len,
        type_id,
        model_text,
        model_text ? strlen(model_text) : 0u,
        segment_index,
        is_final,
        canonical_ascii);
    return wire_len && nc ? vbus_publish(nc, subject, wire, wire_len) : -1;
}

static int publish_stable_display_delta(
    vbus_client *nc,
    const cr_stream *stream,
    const char *display_delta,
    size_t display_delta_len
) {
    uint8_t wire[8192];
    size_t wire_len;
    if (!nc || !stream || !stream->response_subject[0] ||
        !stream->request_id[0] || stream->request_id_len == 0u ||
        !display_delta || display_delta_len == 0u) return -1;
    wire_len = pb_encode_turn_text_event_prepared(
        wire,
        sizeof(wire),
        stream->request_id,
        stream->request_id_len,
        4,
        display_delta,
        display_delta_len,
        display_delta,
        display_delta_len,
        display_delta,
        display_delta_len,
        0,
        0);
    return wire_len ?
        vbus_publish(nc, stream->response_subject, wire, wire_len) : -1;
}

static CR_NOINLINE int copy_bounded_string(
    char *out,
    size_t out_cap,
    const char *input
) {
    size_t input_len;
    if (!out || out_cap == 0 || !input) return -1;
    input_len = strnlen(input, out_cap);
    if (input_len >= out_cap) return -1;
    return copy_bounded_span(out, out_cap, input, input_len);
}

static int copy_event_subject_span(
    char *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    size_t *out_len
) {
    static const char prefix[] = SUBJ_TURN_EVENTS_PFX ".";
    size_t prefix_len = sizeof(prefix) - 1u;
    if (!out || !request_id || request_id_len == 0u ||
        prefix_len >= out_cap || request_id_len >= out_cap - prefix_len)
        return -1;
    memcpy(out, prefix, prefix_len);
    memcpy(out + prefix_len, request_id, request_id_len);
    out[prefix_len + request_id_len] = '\0';
    if (out_len) *out_len = prefix_len + request_id_len;
    return 0;
}

static size_t encode_tts_speak(
    uint8_t *buf,
    size_t buf_cap,
    const char *rid,
    size_t rid_len,
    const char *resp_subj,
    size_t resp_subj_len,
    const char *text,
    size_t text_len,
    int32_t segment_index,
    int is_final,
    int stream_finality_deferred,
    const turn_input_stage_timestamps_c *input_stages,
    int64_t first_text_at_ms,
    int64_t segment_emitted_at_ms
) {
    static const turn_input_stage_timestamps_c no_input_stages;
    char derived_response_subject[256];
    turn_tts_segment_view_c req = {0};
    if (!buf || !rid || rid_len == 0u || !text || segment_index < 0 ||
        (text_len == 0u &&
         !(stream_finality_deferred && is_final && segment_index > 0))) return 0;
    if (!resp_subj || resp_subj_len == 0u) {
        if (copy_event_subject_span(
                derived_response_subject,
                sizeof(derived_response_subject),
                rid,
                rid_len,
                &resp_subj_len) != 0) return 0;
        resp_subj = derived_response_subject;
    }
    req.request_id = rid;
    req.request_id_len = rid_len;
    req.text = text;
    req.text_len = text_len;
    req.response_subject = resp_subj;
    req.response_subject_len = resp_subj_len;
    req.segment_index = segment_index;
    req.is_final = is_final != 0;
    req.output_sample_rate = 24000;
    req.output_channels = 1;
    req.output_bit_depth = 16;
    req.output_encoding = 1;
    req.query_hash = 0;
    req.stream_finality_deferred = stream_finality_deferred != 0;
    req.input_stages = input_stages ? *input_stages : no_input_stages;
    req.first_text_at_ms = first_text_at_ms;
    req.segment_emitted_at_ms = segment_emitted_at_ms;
    return pb_encode_turn_tts_segment_prepared(buf, buf_cap, &req);
}

static int valid_request_id_span(const char *rid, size_t rid_len) {
    size_t i;
    if (!rid || rid_len == 0u) return 0;
    if (rid_len >= sizeof(((cr_stream *)0)->request_id)) return 0;
    for (i = 0; i < rid_len; ++i) {
        unsigned char c = (unsigned char)rid[i];
        if (!(voice_ascii_is_alnum(c) || c == '-' || c == '_' || c == '.' || c == ':'))
            return 0;
    }
    return 1;
}

static int valid_request_id(const char *rid) {
    size_t n;
    if (!rid || !rid[0]) return 0;
    n = strlen(rid);
    return valid_request_id_span(rid, n);
}

static int sanitize_for_route(const char *in, size_t in_len, char *out, size_t out_cap, size_t *out_len) {
    size_t n = 0;
    if (speech_sanitize_v1(in, in_len, 0, out, out_cap, &n) != SPEECH_SANITIZE_OK) {
        return -1;
    }
    if (n >= out_cap) n = out_cap - 1;
    out[n] = '\0';
    if (out_len) *out_len = n;
    return 0;
}

static int emit_segment(
    vbus_client *nc,
    const char *event_subject,
    size_t event_subject_len,
    const char *rid,
    size_t rid_len,
    const char *text,
    size_t text_len,
    int32_t segment_index,
    int is_final,
    int stream_finality_deferred,
    cr_stage_clock *stages
) {
    uint8_t event_wire[8192];
    uint8_t speak_wire[4096];
    int64_t segment_emitted_at_ms;
    size_t event_wire_len;
    size_t speak_wire_len;
    if (!text || text_len == 0u) return 0;
    if (!stages || stages->first_text_at_ms <= 0) return -1;
    segment_emitted_at_ms = stage_clock_now_ms(stages);
    if (segment_emitted_at_ms < stages->first_text_at_ms) return -1;
    event_wire_len = encode_text_event(
        event_wire,
        sizeof(event_wire),
        rid,
        rid_len,
        6,
        text,
        text_len,
        segment_index,
        is_final,
        0);
    speak_wire_len = encode_tts_speak(
        speak_wire,
        sizeof(speak_wire),
        rid,
        rid_len,
        event_subject,
        event_subject_len,
        text,
        text_len,
        segment_index,
        is_final,
        stream_finality_deferred,
        &stages->input,
        stages->first_text_at_ms,
        segment_emitted_at_ms);
    if (!nc || !event_subject || event_subject_len == 0u || event_wire_len == 0 ||
        speak_wire_len == 0) return -1;
    return vbus_publish_pair(
        nc,
        event_subject,
        event_wire,
        event_wire_len,
        SUBJ_TURN_TTS_SPEAK,
        speak_wire,
        speak_wire_len);
}

static int emit_stream_final_marker(vbus_client *nc, cr_stream *stream) {
    uint8_t speak_wire[4096];
    int64_t segment_emitted_at_ms;
    size_t speak_wire_len;
    if (!nc || !stream || !stream->eager_tts_segments ||
        stream->next_segment_index <= 0 ||
        stream->next_segment_index == INT32_MAX ||
        stream->response_subject_len == 0u ||
        stream->stages.first_text_at_ms <= 0) return -1;
    segment_emitted_at_ms = stage_clock_now_ms(&stream->stages);
    if (segment_emitted_at_ms < stream->stages.first_text_at_ms) return -1;
    speak_wire_len = encode_tts_speak(
        speak_wire,
        sizeof(speak_wire),
        stream->request_id,
        stream->request_id_len,
        stream->response_subject,
        stream->response_subject_len,
        "",
        0u,
        stream->next_segment_index,
        1,
        1,
        &stream->stages.input,
        stream->stages.first_text_at_ms,
        segment_emitted_at_ms);
    if (speak_wire_len == 0u ||
        vbus_publish(
            nc, SUBJ_TURN_TTS_SPEAK, speak_wire, speak_wire_len) != 0)
        return -1;
    stream->next_segment_index++;
    return 0;
}

/* Eager streams dispatch now. Compatibility streams retain one text segment
 * until a successor or stream completion determines finality. */
static CR_NOINLINE int stream_queue_segment(
    vbus_client *nc,
    cr_stream *stream,
    const char *text,
    size_t text_len
) {
    if (!nc || !stream || !text || text_len == 0u) return -1;
    if (stream->eager_tts_segments) {
        if (emit_segment(
                nc,
                stream->response_subject,
                stream->response_subject_len,
                stream->request_id,
                stream->request_id_len,
                text,
                text_len,
                stream->next_segment_index++,
                0,
                1,
                &stream->stages) != 0) return -1;
        return 0;
    }
    if (stream->has_pending_segment) {
        if (emit_segment(
                nc,
                stream->response_subject,
                stream->response_subject_len,
                stream->request_id,
                stream->request_id_len,
                stream->pending_segment,
                stream->pending_segment_len,
                stream->next_segment_index++,
                0,
                stream->eager_tts_segments,
                &stream->stages) != 0) return -1;
    }
    if (copy_bounded_span(
            stream->pending_segment,
            sizeof(stream->pending_segment),
            text,
            text_len) != 0) return -1;
    stream->pending_segment_len = text_len;
    stream->has_pending_segment = 1;
    return 0;
}

/* Release lookahead once buffered successor text proves the pending segment
 * cannot be final. The unfinished successor stays in the segment host. */
static int stream_release_pending_before_remainder(vbus_client *nc, cr_stream *stream) {
    if (!nc || !stream) return -1;
    if (stream->eager_tts_segments) return 0;
    if (!stream->has_pending_segment || stream->host.len == 0) return 0;
    if (emit_segment(
            nc,
            stream->response_subject,
            stream->response_subject_len,
            stream->request_id,
            stream->request_id_len,
            stream->pending_segment,
            stream->pending_segment_len,
            stream->next_segment_index++,
            0,
            stream->eager_tts_segments,
            &stream->stages) != 0) return -1;
    stream->has_pending_segment = 0;
    stream->pending_segment_len = 0u;
    stream->pending_segment[0] = '\0';
    return 0;
}

static int stream_finish_segments(vbus_client *nc, cr_stream *stream) {
    char tail[SPEECH_SEG_HOST_MAX_PART];
    int emitted = 0;
    if (!nc || !stream) return -1;
    if (speech_seg_host_flush_all_v1(&stream->host, tail, &emitted) != SPEECH_SEG_OK)
        return -1;
    if (emitted && stream_queue_segment(nc, stream, tail, strlen(tail)) != 0)
        return -1;
    if (stream->eager_tts_segments)
        return emit_stream_final_marker(nc, stream);
    if (!stream->has_pending_segment) return -1;
    if (emit_segment(
            nc,
            stream->response_subject,
            stream->response_subject_len,
            stream->request_id,
            stream->request_id_len,
            stream->pending_segment,
            stream->pending_segment_len,
            stream->next_segment_index++,
            1,
            stream->eager_tts_segments,
            &stream->stages) != 0) return -1;
    stream->has_pending_segment = 0;
    stream->pending_segment_len = 0u;
    stream->pending_segment[0] = '\0';
    return 0;
}

/* One-shot split path for finals (no streaming host). */
static int publish_speakable(
    const cr_state *st,
    vbus_client *nc,
    const char *event_subject,
    size_t event_subject_len,
    const char *rid,
    const char *spoken_in,
    cr_stage_clock *stages
) {
    char sanitized[2048];
    size_t san_len = 0;
    speech_seg_config_v1 cfg;
    size_t offs[SPEECH_SEG_MAX_PARTS];
    size_t lens[SPEECH_SEG_MAX_PARTS];
    size_t nparts = 0, rem_off = 0, rem_len = 0;
    size_t i;
    const char *body;
    size_t body_len;
    size_t rid_len;

    cfg = st->segment_config;

    if (!spoken_in ||
        speech_sanitize_v1(
            spoken_in, strlen(spoken_in), 1, sanitized, sizeof(sanitized), &san_len) !=
            SPEECH_SANITIZE_OK || san_len == 0 || san_len >= sizeof(sanitized)) return -1;
    sanitized[san_len] = '\0';
    body = sanitized;
    body_len = san_len;
    rid_len = strlen(rid);

    if (speech_seg_split_speakable_v1(
            body, body_len, &cfg, offs, lens, SPEECH_SEG_MAX_PARTS, &nparts, &rem_off, &rem_len
        ) != SPEECH_SEG_OK) {
        return -1;
    }

    if (nparts == 0) {
        if (body_len > 0) {
            char one[2048];
            size_t take = body_len < sizeof(one) - 1 ? body_len : sizeof(one) - 1;
            memcpy(one, body, take);
            one[take] = '\0';
            return emit_segment(
                nc, event_subject, event_subject_len, rid, rid_len, one,
                take, 0, 1, 0, stages);
        }
        return 0;
    }
    for (i = 0; i < nparts; ++i) {
        char part[SPEECH_SEG_HOST_MAX_PART];
        size_t take = lens[i] < sizeof(part) - 1 ? lens[i] : sizeof(part) - 1;
        memcpy(part, body + offs[i], take);
        part[take] = '\0';
        if (emit_segment(
                nc, event_subject, event_subject_len, rid, rid_len, part,
                take, (int32_t)i,
                i + 1 == nparts && rem_len == 0, 0, stages) != 0) return -1;
    }
    if (rem_len > 0) {
        char tail[SPEECH_SEG_HOST_MAX_PART];
        size_t take = rem_len < sizeof(tail) - 1 ? rem_len : sizeof(tail) - 1;
        memcpy(tail, body + rem_off, take);
        tail[take] = '\0';
        if (emit_segment(
                nc, event_subject, event_subject_len, rid, rid_len, tail,
                take, (int32_t)nparts, 1, 0, stages) != 0)
            return -1;
    }
    return 0;
}

static cr_stream *stream_get(cr_state *st, const char *rid, int create) {
    int i, free_i = -1;
    uint64_t now_ns = mono_ns();
    for (i = 0; i < CR_MAX_STREAMS; ++i) {
        if (st->streams[i].in_use && strcmp(st->streams[i].request_id, rid) == 0) {
            st->streams[i].expires_ns = stream_deadline(st, now_ns);
            stream_schedule_maintenance(st, &st->streams[i]);
            return &st->streams[i];
        }
        if (!st->streams[i].in_use && free_i < 0) free_i = i;
    }
    if (!create || free_i < 0) return NULL;
    {
        memset(&st->streams[free_i], 0, sizeof(st->streams[free_i]));
        if (copy_bounded_string(
                st->streams[free_i].request_id,
                sizeof(st->streams[free_i].request_id),
                rid) != 0) return NULL;
        st->streams[free_i].request_id_len = strlen(st->streams[free_i].request_id);
        speech_seg_host_init_v1(
            &st->streams[free_i].host, &st->segment_config);
        if (stage_clock_init(&st->streams[free_i].stages) != 0) {
            speech_seg_host_free_v1(&st->streams[free_i].host);
            memset(&st->streams[free_i], 0, sizeof(st->streams[free_i]));
            return NULL;
        }
        st->streams[free_i].expires_ns = stream_deadline(st, now_ns);
        st->streams[free_i].eager_tts_segments = st->eager_tts_segments;
        st->streams[free_i].in_use = 1;
        stream_schedule_maintenance(st, &st->streams[free_i]);
        return &st->streams[free_i];
    }
}

static void stream_drop(cr_state *st, const char *rid) {
    int i;
    for (i = 0; i < CR_MAX_STREAMS; ++i) {
        if (st->streams[i].in_use && strcmp(st->streams[i].request_id, rid) == 0) {
            speech_seg_host_free_v1(&st->streams[i].host);
            memset(&st->streams[i], 0, sizeof(st->streams[i]));
            return;
        }
    }
}

static uint32_t request_hash(const char *request_id) {
    const unsigned char *p = (const unsigned char *)request_id;
    uint32_t hash = 2166136261u;
    while (*p) {
        hash ^= *p++;
        hash *= 16777619u;
    }
    return hash;
}

static cr_llm_lane *llm_lane_for_request(cr_state *st, const char *request_id) {
    if (!st || !request_id) return NULL;
    return &st->lanes[request_hash(request_id) % CR_LLM_WORKER_LANES];
}

static cr_llm_job *llm_lane_acquire_job(cr_llm_lane *lane) {
    cr_llm_job *job = NULL;
    if (!lane || !lane->initialized) return NULL;
    pthread_mutex_lock(&lane->mutex);
    if (!lane->stop && lane->free_count != 0) {
        size_t index = lane->free_stack[--lane->free_count];
        job = &lane->jobs[index];
        /* Release clears the complete slot before it returns to free_stack. */
        atomic_init(&job->canceled, 0);
    }
    pthread_mutex_unlock(&lane->mutex);
    return job;
}

static int llm_lane_job_index(
    const cr_llm_lane *lane,
    const cr_llm_job *job,
    size_t *index_out
) {
    size_t index;

    if (!lane || !job || !index_out) return -1;
    for (index = 0; index < CR_LLM_JOBS_PER_LANE; ++index) {
        if (job == &lane->jobs[index]) {
            *index_out = index;
            return 0;
        }
    }
    return -1;
}

static void llm_lane_release_job_locked(cr_llm_lane *lane, cr_llm_job *job) {
    size_t index;
    if (llm_lane_job_index(lane, job, &index) != 0 ||
        lane->free_count >= CR_LLM_JOBS_PER_LANE) return;
    free(job->initiative);
    free(job->campaign_request);
    free(job->encounter_action);
    free(job->dialogue);
    memset(job, 0, sizeof(*job));
    lane->free_stack[lane->free_count++] = (uint8_t)index;
}

static void llm_lane_release_job(cr_llm_lane *lane, cr_llm_job *job) {
    if (!lane || !job) return;
    pthread_mutex_lock(&lane->mutex);
    llm_lane_release_job_locked(lane, job);
    pthread_mutex_unlock(&lane->mutex);
}

typedef struct {
    vbus_client *publish_nc;
    cr_llm_job *job;
    cr_stream stream;
    openai_sse_decoder decoder;
    speech_display_acc_v1 display;
    char full_text[2048];
    size_t full_text_len;
    int failed;
    int model_text_started;
} cr_llm_stream;

static int llm_rendered_delta(const char *content, size_t content_len, void *user) {
    cr_llm_stream *stream = (cr_llm_stream *)user;
    char parts[SPEECH_SEG_MAX_PARTS][SPEECH_SEG_HOST_MAX_PART];
    char display_delta[sizeof(stream->full_text)];
    size_t display_delta_len = 0;
    size_t part_count = 0;
    size_t i;
    if (!stream || !content || content_len == 0 || stream->failed ||
        atomic_load_explicit(&stream->job->canceled, memory_order_relaxed) != 0)
        return -1;
    if (content_len >= sizeof(stream->full_text) - stream->full_text_len) {
        stream->failed = 1;
        return -1;
    }
    memcpy(stream->full_text + stream->full_text_len, content, content_len);
    stream->full_text_len += content_len;
    stream->full_text[stream->full_text_len] = '\0';
    if (speech_display_acc_add_v1(
            &stream->display,
            content,
            content_len,
            display_delta,
            sizeof(display_delta) - 1u,
            &display_delta_len) != SPEECH_DISPLAY_OK ||
        display_delta_len >= sizeof(display_delta)) {
        stream->failed = 1;
        return -1;
    }
    if (display_delta_len != 0) {
        display_delta[display_delta_len] = '\0';
        if (stage_clock_mark_first_text(&stream->stream.stages) != 0) {
            stream->failed = 1;
            return -1;
        }
        if (publish_stable_display_delta(
                stream->publish_nc,
                &stream->stream,
                display_delta,
                display_delta_len) != 0) {
            stream->failed = 1;
            return -1;
        }
    }
    if (stream->job->enable_tts) {
        if (speech_seg_host_add_v1(
                &stream->stream.host,
                mono_ns(),
                content,
                content_len,
                parts,
                SPEECH_SEG_MAX_PARTS,
                &part_count) != SPEECH_SEG_OK) {
            stream->failed = 1;
            return -1;
        }
        for (i = 0; i < part_count; ++i) {
            if (stream_queue_segment(
                    stream->publish_nc,
                    &stream->stream,
                    parts[i],
                    strlen(parts[i])) != 0) {
                stream->failed = 1;
                return -1;
            }
        }
        if (stream_release_pending_before_remainder(
                stream->publish_nc, &stream->stream) != 0) {
            stream->failed = 1;
            return -1;
        }
    }
    return 0;
}

static int llm_content(cr_llm_stream *stream, const char *content, size_t content_len, int final) {
    char rendered[SPEECH_MARKUP_INPUT_CAP];
    size_t length = 0;
    if (!stream || stream->failed ||
        atomic_load_explicit(&stream->job->canceled, memory_order_relaxed)) return -1;
    if (speech_markup_feed_v1(&stream->stream.markup, content, content_len, final,
            rendered, sizeof(rendered), &length) != SPEECH_MARKUP_OK) {
        stream->failed = 1;
        return -1;
    }
    for (size_t i = 0; i < length; ++i)
        if (!memchr(" \t\r\n\f\v", rendered[i], 6u)) stream->model_text_started = 1;
    if (length && !stream->full_text_len &&
        stream->job->dialogue_command == DND_DIALOGUE_CANCEL_AND_ASK) {
        char receipt[256];
        size_t receipt_len;
        size_t leading = 0;
        while (leading < length && memchr(" \t\r\n\f\v", rendered[leading], 6u)) ++leading;
        if (leading == length) return 0;
        length -= leading;
        if (leading) memmove(rendered, rendered + leading, length);
        /* Admission captured this view before successfully clearing the proposal.
         * Render the receipt only alongside actual model text. An empty or
         * failed model stream must not complete on the receipt alone. */
        if (dnd_dialogue_reply(DND_DIALOGUE_CANCEL, stream->job->dialogue,
                receipt, sizeof(receipt) - 1u) != 0) {
            stream->failed = 1;
            return -1;
        }
        receipt_len = strlen(receipt);
        receipt[receipt_len++] = ' ';
        receipt[receipt_len] = '\0';
        if (llm_rendered_delta(receipt, receipt_len, stream) != 0) return -1;
    }
    return length ? llm_rendered_delta(rendered, length, stream) : 0;
}

static int llm_delta(const char *content, size_t content_len, void *user) {
    return llm_content(user, content, content_len, 0);
}

/* Speak a useful uncertainty answer before slower imagined narration. Keep
 * it in the same display, history, cancellation, and segment sequence. */
static int loop_scene_stream_preface(cr_llm_stream *stream, const char *text) {
    char tail[SPEECH_SEG_HOST_MAX_PART];
    int emitted = 0, rc;
    int eager = stream->stream.eager_tts_segments;
    stream->stream.eager_tts_segments = 1;
    rc = llm_rendered_delta(text, strlen(text), stream);
    if (!rc && stream->job->enable_tts) {
        rc = speech_seg_host_flush_all_v1(&stream->stream.host, tail, &emitted);
        if (rc == SPEECH_SEG_OK && emitted)
            rc = stream_queue_segment(stream->publish_nc, &stream->stream, tail, strlen(tail));
    }
    stream->stream.eager_tts_segments = eager;
    return rc;
}

static int llm_http_body(const uint8_t *data, size_t data_len, void *user) {
    cr_llm_stream *stream = (cr_llm_stream *)user;
    if (!stream || (!data && data_len != 0) ||
        atomic_load_explicit(&stream->job->canceled, memory_order_relaxed) != 0)
        return -1;
    return openai_sse_feed(&stream->decoder, (const char *)data, data_len);
}

static int retrieve_grounded_prompt(cr_llm_lane *lane, cr_llm_job *job) {
    cr_state *owner = lane->owner;
    rag_search_request_c req = {0};
    rag_search_response_c response;
    uint8_t request_wire[4096], response_wire[DND_RAG_WIRE_CAP];
    size_t request_len, response_len = 0;
    int64_t now = now_unix_ms();
    int timeout = owner->rag_timeout_ms;
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed)) return -1;
    if (!now || now > INT64_MAX - timeout) return -2;
    if (job->deadline_ms) {
        if (job->deadline_ms <= now) return -2;
        if (job->deadline_ms - now < timeout) timeout = (int)(job->deadline_ms - now);
    }
    if (copy_bounded_string(req.request_id, sizeof(req.request_id), job->request_id) != 0 ||
        copy_bounded_string(req.user_id, sizeof(req.user_id), job->user_id) != 0 ||
        copy_bounded_string(req.session_id, sizeof(req.session_id), job->session_id) != 0 ||
        copy_bounded_string(req.campaign_id, sizeof(req.campaign_id), job->campaign_id) != 0 ||
        copy_bounded_string(req.character_id, sizeof(req.character_id), job->character_id) != 0 ||
        copy_bounded_string(req.knowledge_scope, sizeof(req.knowledge_scope), job->knowledge_scope) != 0 ||
        copy_bounded_span(req.query, sizeof(req.query), job->prompt, job->prompt_len) != 0) return -2;
    req.premium = job->premium;
    req.top_k = DND_RAG_HITS_MAX;
    req.deadline_unix_ms = now + timeout;
    request_len = pb_encode_rag_search_request(request_wire, sizeof(request_wire), &req);
    if (!request_len || vbus_request_cancel(
            lane->nc, SUBJ_RAG_SEARCH, request_wire, request_len,
            response_wire, sizeof(response_wire), &response_len,
            timeout, &job->canceled) != 0)
        return atomic_load_explicit(&job->canceled, memory_order_relaxed) ? -1 : -2;
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed)) return -1;
    if (pb_decode_rag_search_response(response_wire, response_len, &response) != 0 ||
        strcmp(response.request_id, job->request_id) != 0 || !response.used_rag ||
        response.error[0] || !response.documents.count ||
        dnd_rag_grounded_prompt(&response.documents, job->prompt,
            lane->grounded, sizeof(lane->grounded)) != 0) return -2;
    lane->retrieval_len = pb_encode_grounded_retrieval_provenance(
        lane->retrieval_wire, sizeof(lane->retrieval_wire), &response.documents, &owner->grounding.identity);
    if (!lane->retrieval_len) return -2;
    return 1;
}

static void process_dialogue_job(cr_llm_lane *lane, cr_llm_job *job) {
    char text[512];
    cr_stage_clock stages = {0};
    if (!current_pending(lane->owner, job, 0) ||
        atomic_load_explicit(&job->canceled, memory_order_relaxed)) {
        (void)publish_event(lane->nc, job->response_subject, job->request_id,
            "canceled", "dialogue_superseded");
        return;
    }
    if (!job->dialogue || dnd_dialogue_reply(job->dialogue_command, job->dialogue,
            text, sizeof(text)) != 0 || stage_clock_init(&stages) != 0 ||
            stage_clock_mark_first_text(&stages) != 0) {
        (void)publish_model_failure(lane->nc, job, 0, "dialogue response unavailable");
        return;
    }
    stages.input = job->input_stages;
    if (publish_text_event(lane->nc, job->response_subject, job->request_id,
            strlen(job->request_id), 5, text, 0, 1, 0) != 0 ||
        (job->enable_tts && !atomic_load_explicit(&job->canceled, memory_order_relaxed) &&
            publish_speakable(lane->owner, lane->nc, job->response_subject,
                strlen(job->response_subject), job->request_id, text, &stages) != 0)) {
        (void)publish_model_failure(lane->nc, job, job->enable_tts, "dialogue dispatch failed");
        return;
    }
    (void)publish_event(lane->nc, job->response_subject, job->request_id, "done", "");
}

static void process_tool_job(cr_llm_lane *lane, cr_llm_job *job) {
    cr_state *owner = lane->owner;
    dnd_tool_result result;
    turn_tool_result_c provenance = {0};
    cr_stage_clock stages = {0};
    uint8_t wire[DND_TOOL_EVENT_MAX];
    size_t length;
    int rc;
    if (job->tool_turn == 7) rc = dnd_scene_execute(owner->tool_url, owner->tool_secret,
        job->request_id, job->user_id, job->session_id, job->prompt,
        job->campaign_id, job->scene_id, job->deadline_ms, owner->tool_timeout_ms, &job->canceled, &result);
    else if (job->tool_turn == 6) rc = dnd_action_execute(owner->tool_url, owner->tool_secret,
        job->request_id, job->user_id, job->session_id, job->prompt, job->campaign_id,
        job->encounter_id, job->encounter_action, job->deadline_ms, owner->tool_timeout_ms, &job->canceled, &result);
    else if (job->tool_turn == 5) rc = dnd_campaign_execute(owner->tool_url, owner->tool_secret,
        job->request_id, job->user_id, job->session_id, job->prompt, job->campaign_id,
        job->campaign_request, job->deadline_ms, owner->tool_timeout_ms, &job->canceled, &result);
    else if (job->tool_turn == 4) rc = dnd_initiative_execute(owner->tool_url, owner->tool_secret,
        job->request_id, job->user_id, job->session_id, job->prompt, job->campaign_id,
        job->encounter_id, job->initiative, job->deadline_ms, owner->tool_timeout_ms, &job->canceled, &result);
    else if (job->tool_turn == 3) rc = dnd_roster_execute(owner->tool_url, owner->tool_secret,
        job->request_id, job->user_id, job->session_id, job->prompt,
        job->campaign_id, job->deadline_ms, owner->tool_timeout_ms, &job->canceled, &result);
    else if (job->tool_turn == 2) rc = dnd_encounter_execute(owner->tool_url, owner->tool_secret,
        job->request_id, job->user_id, job->session_id, job->prompt,
        job->campaign_id, job->encounter_id, job->deadline_ms, owner->tool_timeout_ms, &job->canceled, &result);
    else rc = dnd_tools_execute(owner->tool_url, owner->tool_secret,
        job->request_id, job->user_id, job->session_id, job->prompt,
        job->deadline_ms, owner->tool_timeout_ms, &job->canceled, &result);
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed) ||
        rc == DND_TOOL_CANCELED) {
        (void)publish_event(lane->nc, job->response_subject, job->request_id,
                            "canceled", "barge_in");
        return;
    }
    if (rc != DND_TOOL_OK || now_unix_ms() >= job->deadline_ms) {
        (void)publish_event(lane->nc, job->response_subject, job->request_id,
            "failed", rc == DND_TOOL_DEADLINE ? "tool deadline exceeded" : "authenticated tool result unavailable");
        return;
    }
    if (!current_pending(owner, job, job->tool_turn == 7 ? result.scene_revision : 0)) {
        (void)publish_event(lane->nc, job->response_subject, job->request_id,
            "canceled", "dialogue_superseded");
        return;
    }
    if (stage_clock_init(&stages) != 0 || stage_clock_mark_first_text(&stages) != 0) {
        (void)publish_event(lane->nc, job->response_subject, job->request_id,
                            "failed", "cascade timing clock failed");
        return;
    }
    stages.input = job->input_stages;
    memcpy(provenance.tool_id, result.tool_id, sizeof(result.tool_id));
    memcpy(provenance.tool_call_id, result.call_id, sizeof(result.call_id));
    memcpy(provenance.output_sha256, result.output_sha256, sizeof(result.output_sha256));
    provenance.elapsed_ms = result.elapsed_ms;
    provenance.present = 1;
    length = encode_text_event(wire, sizeof(wire), job->request_id,
        strlen(job->request_id), 5, result.text, strlen(result.text), 0, 1, 1);
    length = pb_append_turn_tool_result(wire, sizeof(wire), length, &provenance);
    if (result.encounter_length) {
        turn_encounter_c encounter = {result.encounter_wire, result.encounter_length};
        length = pb_append_turn_encounter(wire, sizeof(wire), length, &encounter);
    }
    if (result.roster_length) {
        turn_roster_c roster = {result.roster_wire, result.roster_length};
        length = pb_append_turn_roster(wire, sizeof(wire), length, &roster);
    }
    if (result.initiative_length) {
        turn_initiative_c initiative = {result.initiative_wire, result.initiative_length};
        length = pb_append_turn_initiative(wire, sizeof(wire), length, &initiative);
    }
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed)) return;
    if (!length || vbus_publish(lane->nc, job->response_subject, wire, length) != 0 ||
        (job->enable_tts && publish_speakable(owner, lane->nc, job->response_subject,
            strlen(job->response_subject), job->request_id, result.text, &stages) != 0)) {
        (void)publish_model_failure(lane->nc, job, job->enable_tts, "tool response dispatch failed");
        return;
    }
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed)) return;
    length = pb_encode_turn_event(wire, sizeof(wire), job->request_id, "done", "");
    length = pb_append_turn_tool_result(wire, sizeof(wire), length, &provenance);
    if (length) (void)vbus_publish(lane->nc, job->response_subject, wire, length);
}

/* The existing C session owner binds the authenticated user and conversation.
 * Loop jobs for one conversation share a lane, so appends stay ordered. */
static int loop_append(cr_llm_lane *lane, cr_llm_job *job,
    const char *role, const char *text) {
    session_append_request_c request = {0};
    session_append_response_c response;
    uint8_t wire[4608], reply[512];
    size_t length, reply_len = 0;
    if ((strcmp(role, "user") && strcmp(role, "assistant")) ||
        copy_bounded_string(request.session_id, sizeof(request.session_id), job->session_id) ||
        copy_bounded_string(request.user_id, sizeof(request.user_id), job->user_id) ||
        copy_bounded_string(request.message.role, sizeof(request.message.role), role) ||
        copy_bounded_string(request.message.content, sizeof(request.message.content), text) ||
        copy_bounded_string(request.message.request_id, sizeof(request.message.request_id), job->request_id)) return -1;
    request.message.timestamp_ms = now_unix_ms();
    length = pb_encode_session_append_request(wire, sizeof(wire), &request);
    if (!length || vbus_request_cancel(lane->nc, SUBJ_SESSION_APPEND, wire, length,
        reply, sizeof(reply), &reply_len, 500, &job->canceled) ||
        pb_decode_session_append_response(reply, reply_len, &response) ||
        strcmp(response.session_id, job->session_id) || response.message_count < 1) return -1;
    return 0;
}

static int loop_read(cr_llm_lane *lane, cr_llm_job *job) {
    session_get_request_c request = {0};
    uint8_t wire[512], reply[SESSION_HISTORY_MAX * 4608u + 256u];
    size_t length, reply_len = 0;
    if (copy_bounded_string(request.session_id, sizeof(request.session_id), job->session_id) ||
        copy_bounded_string(request.user_id, sizeof(request.user_id), job->user_id)) return -1;
    request.last_n = (int32_t)SESSION_HISTORY_MAX;
    length = pb_encode_session_get_request(wire, sizeof(wire), &request);
    if (!length || vbus_request_cancel(lane->nc, SUBJ_SESSION_GET, wire, length,
        reply, sizeof(reply), &reply_len, 500, &job->canceled) ||
        pb_decode_session_history_response(reply, reply_len, &lane->loop_history) ||
        strcmp(lane->loop_history.session_id, job->session_id)) return -1;
    return loop_append(lane, job, "user", job->prompt);
}

/* Bounded clarifications need no model-authored rules or presence claim. */
static void process_loop_clarification(cr_llm_lane *lane, cr_llm_job *job, const char *text) {
    cr_stage_clock stages = {0};
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed)) return;
    if (stage_clock_init(&stages) || stage_clock_mark_first_text(&stages) ||
        loop_append(lane, job, "assistant", text)) {
        (void)publish_model_failure(lane->nc, job, 0, "conversation clarification unavailable");
        return;
    }
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed)) return;
    stages.input = job->input_stages;
    if (publish_text_event(lane->nc, job->response_subject, job->request_id,
            strlen(job->request_id), 5, text, 0, 1, 0) ||
        (job->enable_tts && publish_speakable(lane->owner, lane->nc,
            job->response_subject, strlen(job->response_subject), job->request_id,
            text, &stages))) {
        (void)publish_model_failure(lane->nc, job, job->enable_tts,
            "conversation clarification dispatch failed");
        return;
    }
    if (!atomic_load_explicit(&job->canceled, memory_order_relaxed))
        (void)publish_event(lane->nc, job->response_subject, job->request_id, "done", "");
}

static void process_llm_job(cr_llm_lane *lane, cr_llm_job *job) {
    cr_state *owner = lane->owner;
    cr_llm_stream stream;
    http_min_header headers[] = {{"Accept", "text/event-stream"}};
    const char *prompt = job ? job->prompt : NULL;
    size_t prompt_len = job ? job->prompt_len : 0u;
    size_t request_len;
    size_t response_len = 0;
    int status = 0;
    int rc;
    char scene_preface[640] = {0};
    if (!owner || !lane->nc || !job) return;
    lane->retrieval_len = 0;
    lane->loop_history.count = 0;
    if (job->tool_turn == 8) {
        process_dialogue_job(lane, job);
        return;
    }
    if (!current_pending(owner, job, 0)) {
        (void)publish_event(lane->nc, job->response_subject, job->request_id,
            "canceled", "dialogue_superseded");
        return;
    }
    if (job->tool_turn) {
        process_tool_job(lane, job);
        return;
    }
    if (!owner->llm_url[0]) {
        (void)publish_event(lane->nc, job->response_subject, job->request_id,
                            "failed", "model endpoint unavailable");
        return;
    }
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed) != 0) {
        uint8_t canceled_wire[512];
        size_t canceled_len = pb_encode_turn_event(
            canceled_wire, sizeof(canceled_wire), job->request_id, "canceled", "barge_in");
        if (canceled_len != 0)
            (void)vbus_publish(lane->nc, job->response_subject, canceled_wire, canceled_len);
        return;
    }
    if (job->loop_profile && loop_read(lane, job)) {
        (void)publish_event(lane->nc, job->response_subject, job->request_id,
            atomic_load_explicit(&job->canceled, memory_order_relaxed) ? "canceled" : "failed",
            "conversation context unavailable");
        return;
    }
    if (job->loop_profile) {
        char reply[640];
        const char *narration = NULL;
        int mixed = loop_scene_mixed_reply(job->prompt, job->prompt_len,
            &lane->loop_history, scene_preface, sizeof(scene_preface), &narration);
        if (mixed < 0) {
            (void)publish_model_failure(lane->nc, job, 0, "invalid mixed scene question");
            return;
        }
        if (mixed == 2) {
            process_loop_clarification(lane, job, scene_preface);
            return;
        }
        if (mixed == 1) { prompt = narration; prompt_len = strlen(narration); }
        int scene = loop_scene_reply(job->prompt, job->prompt_len,
            &lane->loop_history, reply, sizeof(reply));
        if (scene < 0) {
            (void)publish_model_failure(lane->nc, job, 0, "invalid conversation scene question");
            return;
        }
        if (scene) {
            process_loop_clarification(lane, job, reply);
            return;
        }
    }
    if (job->enable_rag) {
        int rag_result = retrieve_grounded_prompt(lane, job);
        if (rag_result < 0) {
            (void)publish_event(
                lane->nc,
                job->response_subject,
                job->request_id,
                rag_result == -1 ? "canceled" : "failed",
                rag_result == -1 ? "barge_in" : "authorized retrieval unavailable");
            return;
        }
        if (rag_result > 0) {
            prompt = lane->grounded;
            prompt_len = strlen(lane->grounded);
        }
    }
    if (job->loop_profile && !lane->retrieval_len) {
        char scene_name[201];
        int clarify = dnd_scene_question(job->prompt, scene_name) ? 0 :
            loop_rules_clarification(job->prompt, job->prompt_len, &lane->loop_history);
        if (clarify < 0) {
            (void)publish_model_failure(lane->nc, job, 0, "invalid conversation rules question");
            return;
        }
        if (clarify) {
            process_loop_clarification(lane, job, LOOP_RULES_REPLY);
            return;
        }
    }
    if (job->dialogue && (job->dialogue->held || job->dialogue_command == DND_DIALOGUE_CANCEL_AND_ASK)) {
        if (dnd_dialogue_prompt(job->dialogue, job->dialogue_command == DND_DIALOGUE_CANCEL_AND_ASK,
                prompt, lane->dialogue_prompt,
                sizeof(lane->dialogue_prompt)) != 0) {
            (void)publish_model_failure(lane->nc, job, 0, "dialogue context capacity exceeded");
            return;
        }
        prompt = lane->dialogue_prompt;
        prompt_len = strlen(prompt);
    }
    /* Each nested owner initializes its own live state. Avoid clearing the
     * 46 KiB stack object, including the SSE decoder that initializes next. */
    stream.publish_nc = lane->nc;
    stream.job = job;
    stream.full_text_len = 0;
    stream.full_text[0] = '\0';
    stream.failed = 0;
    stream.model_text_started = 0;
    stream.stream.pending_segment[0] = '\0';
    stream.stream.full_text[0] = '\0';
    stream.stream.full_text_len = 0;
    stream.stream.expires_ns = 0;
    stream.stream.next_segment_index = 0;
    stream.stream.has_pending_segment = 0;
    stream.stream.eager_tts_segments = owner->eager_tts_segments;
    stream.stream.enable_tts = 0;
    stream.stream.in_use = 0;
    if (stage_clock_init(&stream.stream.stages) != 0) {
        (void)publish_event(
            stream.publish_nc,
            job->response_subject,
            job->request_id,
            "failed",
            "cascade timing clock failed");
        return;
    }
    stream.stream.stages.input = job->input_stages;
    if (copy_bounded_string(
            stream.stream.request_id,
            sizeof(stream.stream.request_id),
            job->request_id) != 0 ||
        copy_bounded_string(
            stream.stream.response_subject,
            sizeof(stream.stream.response_subject),
            job->response_subject) != 0 ||
        !stream.stream.request_id[0] || !stream.stream.response_subject[0]) return;
    stream.stream.request_id_len = strlen(stream.stream.request_id);
    stream.stream.response_subject_len =
        strlen(stream.stream.response_subject);
    speech_seg_host_init_v1(
        &stream.stream.host, &owner->segment_config);
    speech_markup_init_v1(&stream.stream.markup);
    speech_display_acc_init_v1(&stream.display);
    stream.stream.in_use = 1;
    if (openai_sse_init(&stream.decoder, llm_delta, &stream) != 0) {
        speech_seg_host_free_v1(&stream.stream.host);
        return;
    }
    const char *system_text = job->loop_profile ?
        (lane->retrieval_len ? owner->response_prompts.loop_grounded : owner->response_prompts.loop_direct) :
        job->dnd_profile ? (lane->retrieval_len ? owner->response_prompts.dnd_grounded : owner->response_prompts.dnd_direct) :
        (lane->retrieval_len ? owner->response_prompts.grounded : owner->response_prompts.direct);
    size_t system_len = job->loop_profile ?
        (lane->retrieval_len ? owner->response_prompts.loop_grounded_len : owner->response_prompts.loop_direct_len) :
        job->dnd_profile ? (lane->retrieval_len ? owner->response_prompts.dnd_grounded_len : owner->response_prompts.dnd_direct_len) :
        (lane->retrieval_len ? owner->response_prompts.grounded_len : owner->response_prompts.direct_len);
    openai_history_message history[SESSION_HISTORY_MAX];
    for (size_t i = 0; i < lane->loop_history.count; ++i) {
        history[i].role = lane->loop_history.messages[i].role;
        history[i].content = lane->loop_history.messages[i].content;
        history[i].content_len = strlen(history[i].content);
    }
    if (job->loop_profile) {
        request_len = openai_chat_request_json_stream_history(
            lane->request_json, sizeof(lane->request_json), owner->llm_model,
            system_text, system_len, history, lane->loop_history.count,
            prompt, prompt_len, job->max_completion_tokens, DND_RAG_PROMPT_CAP - 1u);
    } else {
        request_len = openai_chat_request_json_stream_system(
            lane->request_json, sizeof(lane->request_json), owner->llm_model,
            system_text, system_len,
            prompt, prompt_len, job->max_completion_tokens, DND_RAG_PROMPT_CAP - 1u);
    }
    if (request_len == 0) {
        (void)publish_event(
            stream.publish_nc,
            job->response_subject,
            job->request_id,
            "failed",
            "model request capacity exceeded");
        speech_seg_host_free_v1(&stream.stream.host);
        return;
    }
    if (job->model_request_capture) {
        char capture_subject[sizeof(job->response_subject) + sizeof(VOICE_MODEL_REQUEST_SUBJECT_SUFFIX)];
        uint8_t capture_wire[VOICE_MODEL_REQUEST_WIRE_MAX];
        voice_model_request_chunk chunk = {0};
        size_t offset = 0;
        int capture_failed = 0;
        (void)snprintf(capture_subject,sizeof(capture_subject),"%s%s",job->response_subject,
            VOICE_MODEL_REQUEST_SUBJECT_SUFFIX);
        memcpy(chunk.request_id,job->request_id,strlen(job->request_id)+1u);
        chunk.total_bytes=(uint32_t)request_len;
        while (offset<request_len) {
            chunk.bytes=(const uint8_t *)lane->request_json+offset;
            chunk.length=request_len-offset;
            if (chunk.length>VOICE_MODEL_REQUEST_CHUNK_MAX) chunk.length=VOICE_MODEL_REQUEST_CHUNK_MAX;
            chunk.final=offset+chunk.length==request_len;
            size_t capture_len=voice_model_request_encode(capture_wire,sizeof(capture_wire),&chunk);
            if (atomic_load_explicit(&job->canceled,memory_order_relaxed) || !capture_len ||
                vbus_publish(lane->nc,capture_subject,capture_wire,capture_len)!=0) {
                capture_failed=1; break;
            }
            offset+=chunk.length; chunk.sequence++;
        }
        OPENSSL_cleanse(capture_wire,sizeof(capture_wire));
        if (capture_failed) {
            (void)publish_model_failure(lane->nc,job,0,"model request capture failed");
            speech_seg_host_free_v1(&stream.stream.host);
            return;
        }
    }
    if (scene_preface[0] && loop_scene_stream_preface(&stream, scene_preface)) {
        (void)publish_model_failure(lane->nc, job,
            stream.stream.next_segment_index > 0, "scene preface dispatch failed");
        speech_seg_host_free_v1(&stream.stream.host);
        return;
    }
    rc = http_min_client_post_stream_headers_cancel(
        &lane->http,
        "application/json",
        headers,
        sizeof(headers) / sizeof(headers[0]),
        (const uint8_t *)lane->request_json,
        request_len,
        llm_http_body,
        &stream,
        CR_LLM_RESPONSE_MAX,
        &response_len,
        &status,
        owner->llm_timeout_ms,
        &job->canceled);
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed) != 0 ||
        rc == HTTP_MIN_ERR_CANCELED) {
        (void)publish_event(
            stream.publish_nc,
            job->response_subject,
            job->request_id,
            "canceled",
            "barge_in");
        speech_seg_host_free_v1(&stream.stream.host);
        svc_log("cascade-router", "llm canceled request=%s", job->request_id);
        return;
    }
    if (rc != HTTP_MIN_OK || status < 200 || status >= 300 || response_len == 0 ||
        stream.failed || openai_sse_finish(&stream.decoder) != 0) {
        (void)publish_model_failure(
            stream.publish_nc,
            job,
            stream.stream.next_segment_index > 0,
            "model stream failed");
        speech_seg_host_free_v1(&stream.stream.host);
        svc_log(
            "cascade-router",
            "llm stream failed request=%s status=%d rc=%d bytes=%zu",
            job->request_id,
            status,
            rc,
            response_len);
        return;
    }
    if (stream.decoder.finish_reason != OPENAI_FINISH_STOP) {
        (void)publish_model_failure(stream.publish_nc, job,
            stream.stream.next_segment_index > 0, "model completion token limit reached");
        speech_seg_host_free_v1(&stream.stream.host);
        return;
    }
    if (llm_content(&stream, NULL, 0, 1) != 0 || !stream.full_text_len ||
        (scene_preface[0] && !stream.model_text_started)) {
        (void)publish_model_failure(stream.publish_nc, job,
            stream.stream.next_segment_index > 0, "invalid model presentation text");
        speech_seg_host_free_v1(&stream.stream.host);
        return;
    }
    {
        size_t canonical_len = 0;
        const char *canonical = speech_display_acc_canonical_ascii_v1(
            &stream.display, &canonical_len);
        int canonical_ascii = canonical && canonical_len < sizeof(stream.full_text);
        const char *final_text = canonical_ascii ? canonical : stream.full_text;
        int final_rc;
        if (job->loop_profile && loop_append(lane, job, "assistant", final_text)) {
            (void)publish_model_failure(stream.publish_nc, job,
                stream.stream.next_segment_index > 0, "conversation response commit failed");
            speech_seg_host_free_v1(&stream.stream.host);
            return;
        }

        {
            uint8_t wire[DND_RAG_CITATIONS_WIRE_CAP + 8192u];
            size_t length = encode_text_event(wire, sizeof(wire), job->request_id,
                stream.stream.request_id_len, 5, final_text, strlen(final_text), 0, 1, canonical_ascii);
            if (length && stream.decoder.reported_model[0])
                length = pb_append_turn_provider_model(wire, sizeof(wire), length, stream.decoder.reported_model);
            if (length && stream.decoder.usage_present)
                length = pb_append_turn_provider_usage(wire, sizeof(wire), length,
                    stream.decoder.prompt_tokens, stream.decoder.completion_tokens, stream.decoder.total_tokens);
            if (length && lane->retrieval_len) {
                turn_retrieval_c retrieval;
                if (pb_decode_retrieval_provenance(lane->retrieval_wire, lane->retrieval_len, &retrieval) != 0)
                    length = 0;
                else length = pb_append_turn_retrieval(wire, sizeof(wire), length, &retrieval);
            }
            final_rc = length ? vbus_publish(stream.publish_nc, job->response_subject, wire, length) : -1;
        }
        if (final_rc != 0 ||
            (job->enable_tts &&
             stream_finish_segments(stream.publish_nc, &stream.stream) != 0)) {
            (void)publish_model_failure(
                stream.publish_nc,
                job,
                stream.stream.next_segment_index > 0,
                "invalid model output");
            speech_seg_host_free_v1(&stream.stream.host);
            return;
        }
    }
    (void)publish_event(
        stream.publish_nc, job->response_subject, job->request_id, "done", "");
    speech_seg_host_free_v1(&stream.stream.host);
}

static void *llm_lane_worker(void *arg) {
    cr_llm_lane *lane = (cr_llm_lane *)arg;
    if (!lane) return NULL;
    if (lane->owner && lane->owner->llm_url[0])
        (void)http_min_client_warm(
            &lane->http, CR_HTTP_PRECONNECT_TIMEOUT_MS);
    for (;;) {
        cr_llm_job *job;
        pthread_mutex_lock(&lane->mutex);
        while (lane->queued == 0 && !lane->stop)
            pthread_cond_wait(&lane->ready, &lane->mutex);
        if (lane->queued == 0 && lane->stop) {
            pthread_mutex_unlock(&lane->mutex);
            break;
        }
        job = &lane->jobs[lane->queue[lane->queue_head]];
        lane->queue_head = (lane->queue_head + 1u) % CR_LLM_QUEUE_PER_LANE;
        lane->queued--;
        lane->active = job;
        pthread_mutex_unlock(&lane->mutex);

        process_llm_job(lane, job);
        OPENSSL_cleanse(&lane->loop_history, sizeof(lane->loop_history));
        finish_pending(lane->owner, job->pending_ticket);

        pthread_mutex_lock(&lane->mutex);
        lane->active = NULL;
        llm_lane_release_job_locked(lane, job);
        pthread_mutex_unlock(&lane->mutex);
    }
    return NULL;
}

static void stop_llm_workers(cr_state *st) {
    int i;
    for (i = 0; i < CR_LLM_WORKER_LANES; ++i) {
        cr_llm_lane *lane = &st->lanes[i];
        size_t offset;
        if (!lane->initialized) continue;
        pthread_mutex_lock(&lane->mutex);
        lane->stop = 1;
        if (lane->active)
            atomic_store_explicit(&lane->active->canceled, 1, memory_order_relaxed);
        for (offset = 0; offset < lane->queued; ++offset) {
            size_t position = (lane->queue_head + offset) % CR_LLM_QUEUE_PER_LANE;
            cr_llm_job *job = &lane->jobs[lane->queue[position]];

            atomic_store_explicit(&job->canceled, 1, memory_order_relaxed);
        }
        pthread_cond_broadcast(&lane->ready);
        pthread_mutex_unlock(&lane->mutex);
    }
    for (i = 0; i < CR_LLM_WORKER_LANES; ++i) {
        cr_llm_lane *lane = &st->lanes[i];
        if (!lane->initialized) continue;
        if (lane->thread_started) pthread_join(lane->thread, NULL);
        http_min_client_destroy(&lane->http);
        if (lane->nc) vbus_close(lane->nc);
        pthread_cond_destroy(&lane->ready);
        pthread_mutex_destroy(&lane->mutex);
        lane->initialized = 0;
    }
    st->workers_started = 0;
}

static int start_llm_workers(cr_state *st) {
    int i;
    for (i = 0; i < CR_LLM_WORKER_LANES; ++i) {
        cr_llm_lane *lane = &st->lanes[i];
        memset(lane, 0, sizeof(*lane));
        lane->http.fd = -1;
        lane->owner = st;
        if (pthread_mutex_init(&lane->mutex, NULL) != 0) goto fail;
        if (pthread_cond_init(&lane->ready, NULL) != 0) {
            pthread_mutex_destroy(&lane->mutex);
            goto fail;
        }
        lane->initialized = 1;
        if (st->llm_url[0] && http_min_client_init(&lane->http, st->llm_url) != HTTP_MIN_OK)
            goto fail;
        lane->free_count = CR_LLM_JOBS_PER_LANE;
        {
            size_t j;
            for (j = 0; j < CR_LLM_JOBS_PER_LANE; ++j)
                lane->free_stack[j] = (uint8_t)(CR_LLM_JOBS_PER_LANE - 1u - j);
        }
        lane->nc = svc_connect_bus();
        if (!lane->nc || pthread_create(&lane->thread, NULL, llm_lane_worker, lane) != 0)
            goto fail;
        lane->thread_started = 1;
    }
    st->workers_started = 1;
    return 0;

fail:
    stop_llm_workers(st);
    return -1;
}

static int queue_llm_job(cr_state *st, cr_llm_job *job) {
    cr_llm_lane *lane;
    size_t index;
    size_t offset;
    size_t position;
    int needs_signal;
    if (!st || !job || !st->workers_started) return -1;
    lane = llm_lane_for_request(st, job->loop_profile ? job->session_id : job->request_id);
    if (!lane) return -1;
    pthread_mutex_lock(&lane->mutex);
    if (lane->stop || lane->queued >= CR_LLM_QUEUE_PER_LANE ||
        (lane->active && strcmp(lane->active->request_id, job->request_id) == 0)) {
        pthread_mutex_unlock(&lane->mutex);
        return -1;
    }
    for (offset = 0; offset < lane->queued; ++offset) {
        cr_llm_job *queued;

        position = (lane->queue_head + offset) % CR_LLM_QUEUE_PER_LANE;
        queued = &lane->jobs[lane->queue[position]];
        if (strcmp(queued->request_id, job->request_id) == 0) {
            pthread_mutex_unlock(&lane->mutex);
            return -1;
        }
    }
    if (llm_lane_job_index(lane, job, &index) != 0) {
        pthread_mutex_unlock(&lane->mutex);
        return -1;
    }
    needs_signal = lane->queued == 0;
    position = (lane->queue_head + lane->queued) % CR_LLM_QUEUE_PER_LANE;
    lane->queue[position] = (uint8_t)index;
    lane->queued++;
    pthread_mutex_unlock(&lane->mutex);
    if (needs_signal) pthread_cond_signal(&lane->ready);
    return 0;
}

static int cancel_llm_job(cr_state *st, const char *request_id) {
    int i;
    int matched = 0;
    if (!st || !request_id || !request_id[0] || !st->workers_started) return 0;
    for (i = 0; i < CR_LLM_WORKER_LANES; ++i) {
        cr_llm_lane *lane = &st->lanes[i];
        size_t offset;
        pthread_mutex_lock(&lane->mutex);
        if (lane->active && strcmp(lane->active->request_id, request_id) == 0) {
            atomic_store_explicit(&lane->active->canceled, 1, memory_order_relaxed);
            matched = 1;
        }
        for (offset = 0; offset < lane->queued; ++offset) {
            size_t position = (lane->queue_head + offset) % CR_LLM_QUEUE_PER_LANE;
            cr_llm_job *job = &lane->jobs[lane->queue[position]];

            if (strcmp(job->request_id, request_id) == 0) {
                atomic_store_explicit(&job->canceled, 1, memory_order_relaxed);
                matched = 1;
            }
        }
        pthread_mutex_unlock(&lane->mutex);
    }
    return matched;
}

static void cancel_other_pending(cr_state *st, const turn_start_c *turn,
    const char *previous_request) {
    char requests[CR_LLM_WORKER_LANES * CR_LLM_JOBS_PER_LANE + 1u][128];
    size_t count = 0;
    if (!st->workers_started) return;
    if (previous_request[0] && strcmp(previous_request, turn->request_id)) {
        memcpy(requests[count], previous_request, strlen(previous_request) + 1u);
        ++count;
    }
    for (size_t i = 0; i < CR_LLM_WORKER_LANES; ++i) {
        cr_llm_lane *lane = &st->lanes[i];
        pthread_mutex_lock(&lane->mutex);
        for (size_t offset = 0; offset <= lane->queued; ++offset) {
            cr_llm_job *job = offset == lane->queued ? lane->active :
                &lane->jobs[lane->queue[(lane->queue_head + offset) % CR_LLM_QUEUE_PER_LANE]];
            if (job && job->pending_ticket.generation &&
                !strcmp(job->user_id, turn->user_id) &&
                !strcmp(job->session_id, turn->session_id) &&
                strcmp(job->request_id, turn->request_id)) {
                atomic_store_explicit(&job->canceled, 1, memory_order_relaxed);
                if (strcmp(job->request_id, previous_request) &&
                    count < CR_LLM_WORKER_LANES * CR_LLM_JOBS_PER_LANE + 1u) {
                    memcpy(requests[count], job->request_id, sizeof(requests[count]));
                    ++count;
                }
            }
        }
        pthread_mutex_unlock(&lane->mutex);
    }
    /* Invalidate queued speech too. No bus operation runs under either lock. */
    for (size_t i = 0; i < count; ++i) {
        uint8_t wire[512];
        size_t length = pb_encode_turn_cancel(wire, sizeof(wire), requests[i],
            turn->user_id, "dialogue_superseded");
        if (length) (void)vbus_publish(st->nc, SUBJ_TURN_CANCEL, wire, length);
    }
}

static int prepare_dialogue(cr_state *st, const turn_start_c *turn, int tool_turn,
    const uint8_t *wire, size_t wire_len, dnd_pending_ticket *ticket,
    dnd_pending_view *view, int *command) {
    dnd_scene_intent intent = {0};
    char previous_request[DND_PENDING_ID_CAP] = {0};
    uint8_t digest[SHA256_DIGEST_LENGTH];
    int rc;
    if (strcmp(turn->metadata.interaction_profile, "dnd_app")) return 0;
    if (!turn->metadata.campaign_id[0] || !turn->metadata.scene_id[0]) {
        return !tool_turn && dnd_dialogue_command(turn->text, 0) ? DND_PENDING_INVALID : 0;
    }
    if (tool_turn == 7 && !dnd_scene_parse(turn->text, &intent)) return DND_PENDING_INVALID;
    if (!SHA256(wire, wire_len, digest)) return DND_PENDING_INVALID;
    dnd_pending_key key = {turn->user_id, turn->session_id,
        turn->metadata.campaign_id, turn->metadata.scene_id};
    pthread_mutex_lock(&st->pending_mutex);
    uint64_t now = mono_ns() / UINT64_C(1000000);
    (void)dnd_pending_latest_request(&st->pending, turn->user_id, turn->session_id,
        now, previous_request);
    rc = intent.proposed_spell[0] ? dnd_pending_begin(&st->pending, key,
        turn->request_id, digest, now, ticket) : dnd_pending_resume(&st->pending, key,
        turn->request_id, digest, now, ticket);
    if (rc == DND_PENDING_NOT_FOUND) {
        rc = DND_PENDING_OK;
    } else if (rc == DND_PENDING_OK) {
        if (intent.proposed_spell[0]) rc = dnd_pending_hold(&st->pending, *ticket, now, intent.proposed_spell);
        if (rc == 0 && intent.character_name[0])
            rc = dnd_pending_focus(&st->pending, *ticket, now, intent.character_name);
        if (rc == 0) rc = dnd_pending_read(&st->pending, *ticket, now, view);
    } else if (rc > 0) {
        /* Do not admit a second worker for an existing or finished turn. */
        memset(ticket, 0, sizeof(*ticket));
        rc = DND_PENDING_CONFLICT;
    }
    if (rc == 0) {
        *command = tool_turn ? DND_DIALOGUE_NONE : dnd_dialogue_command(turn->text, view->held);
        if ((*command == DND_DIALOGUE_CANCEL || *command == DND_DIALOGUE_CANCEL_AND_ASK) && ticket->generation)
            rc = dnd_pending_cancel(&st->pending, *ticket, now);
    }
    pthread_mutex_unlock(&st->pending_mutex);
    if (rc == 0 && ticket->generation) cancel_other_pending(st, turn, previous_request);
    return rc;
}

static void on_turn_start(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    cr_state *st = (cr_state *)user;
    turn_start_c turn;
    char derived_event_subject[320];
    const char *event_subject;
    char clean[2048];
    size_t event_subject_len;
    size_t clean_len = 0;
    uint64_t packed;
    uint32_t phatic;
    uint32_t route = ROUTE_CLASSIFIER_ESCALATE_V1;
    const char *route_name = "escalate";
    size_t route_name_len = sizeof("escalate") - 1u;
    const char *route_text;
    size_t route_text_len;
    int tool_turn = 0;
    int governed_retrieval = 0;
    int64_t deadline_ms = 0;
    dnd_pending_ticket pending_ticket = {0};
    dnd_pending_view dialogue = {0};
    int dialogue_command = DND_DIALOGUE_NONE;
    int loop_profile;
    (void)subject;
    (void)reply;

    if (pb_decode_turn_start(data, data_len, &turn) != 0) {
        svc_log("cascade-router", "turn start decode failed");
        return;
    }
    loop_profile = !strcmp(turn.metadata.interaction_profile, "realtime_voice") &&
        !strcmp(turn.metadata.client_surface, "loop");
    if (!valid_request_id_span(turn.request_id, turn.request_id_len) ||
        turn.text_len == 0u) {
        svc_log("cascade-router", "reject turn with invalid request_id or empty text");
        return;
    }
    if (turn.response_subject_len == 0u) {
        if (copy_event_subject_span(
                derived_event_subject,
                sizeof(derived_event_subject),
                turn.request_id,
                turn.request_id_len,
                &event_subject_len) != 0) return;
        event_subject = derived_event_subject;
    } else {
        event_subject_len = turn.response_subject_len;
        event_subject = turn.response_subject;
    }
    if (event_subject_len >= 256u ||
        !vbus_publish_subject_span_valid(event_subject, event_subject_len)) {
        svc_log("cascade-router", "reject invalid response subject request=%s", turn.request_id);
        return;
    }
    {
        turn_budget_info_v1 budget;
        int64_t now_ms = now_unix_ms();
        int64_t remaining_ms;
        if (now_ms <= 0) {
            (void)publish_event(
                st->nc,
                event_subject,
                turn.request_id,
                "failed",
                "turn admission clock unavailable");
            svc_log("cascade-router", "reject admission clock request=%s", turn.request_id);
            return;
        }
        if (extract_turn_budget(st, &turn, now_ms, &budget) != 0) {
            (void)publish_event(
                st->nc,
                event_subject,
                turn.request_id,
                "failed",
                "invalid turn budget metadata");
            svc_log("cascade-router", "reject invalid budget request=%s", turn.request_id);
            return;
        }
        remaining_ms = turn_budget_remaining_ms_v1(&budget, now_ms);
        if (budget.has_deadline && remaining_ms == 0) {
            (void)publish_event(
                st->nc,
                event_subject,
                turn.request_id,
                "failed",
                "turn deadline exceeded before accept");
            svc_log("cascade-router", "reject expired deadline request=%s", turn.request_id);
            return;
        }
        deadline_ms = now_ms + st->tool_timeout_ms;
        if (budget.has_deadline && remaining_ms < st->tool_timeout_ms)
            deadline_ms = now_ms + remaining_ms;
    }
    if (consume_early_cancel(st, turn.request_id, turn.request_id_len)) {
        (void)publish_event(st->nc, event_subject, turn.request_id, "canceled", "client_disconnect");
        svc_log("cascade-router", "consume early cancel request=%s", turn.request_id);
        return;
    }

    route_text_len = turn.text_len;
    if (sanitize_for_route(turn.text, route_text_len, clean, sizeof(clean), &clean_len) != 0 ||
        clean_len == 0) {
        (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "invalid turn text");
        svc_log("cascade-router", "reject unsanitizable turn request=%s", turn.request_id);
        return;
    }
    route_text = clean;
    route_text_len = clean_len;

    if (strcmp(turn.metadata.interaction_profile, "dnd_app") == 0) {
        char expression[64];
        char scene_name[201];
        tool_turn = dnd_scene_question(turn.text, scene_name) ? 7 : dnd_action_intent(turn.text) ? 6 : dnd_campaign_intent(turn.text) ? 5 : dnd_initiative_intent(turn.text) ? 4 : dnd_roster_intent(turn.text) ? 3 :
            dnd_encounter_intent(turn.text) ? 2 : dnd_dice_intent(turn.text, expression);
        if ((tool_turn == 2 || tool_turn == 4 || tool_turn == 6) && (!turn.metadata.campaign_id[0] || !turn.metadata.encounter_id[0])) {
            (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "encounter context required");
            return;
        }
        if ((tool_turn == 3 || tool_turn == 5) && !turn.metadata.campaign_id[0]) {
            (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "campaign context required");
            return;
        }
        if (tool_turn == 7 && (!turn.metadata.campaign_id[0] || !turn.metadata.scene_id[0])) {
            (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "Choose a campaign and scene before checking character presence.");
            return;
        }
        if (tool_turn == 4 && !dnd_initiative_request_valid(&turn.dnd_initiative)) {
            (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "explicit initiative selections and versions required");
            return;
        }
        if (tool_turn == 5 && (!dnd_campaign_request_valid(&turn.dnd_campaign) ||
            dnd_campaign_intent(turn.text) != (int)turn.dnd_campaign.operation)) {
            (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "explicit campaign setup data required");
            return;
        }
        if (tool_turn == 6 && (!dnd_encounter_action_valid(&turn.dnd_encounter_action) ||
            dnd_action_intent(turn.text) != (int)turn.dnd_encounter_action.operation)) {
            (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "explicit encounter change required");
            return;
        }
        if (tool_turn < 0 || (tool_turn && !st->tool_url[0])) {
            (void)publish_event(st->nc, event_subject, turn.request_id, "failed",
                tool_turn < 0 ? "unsupported dice command" : "authenticated tool service unavailable");
            return;
        }
        governed_retrieval = !tool_turn &&
            (turn.enable_rag || strcmp(turn.metadata.retrieval_force, "true") == 0 ||
             turn.metadata.knowledge_scope[0]);
    }

    if (turn.dnd_initiative.count && tool_turn != 4) {
        (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "initiative request needs the D&D initiative command");
        return;
    }
    if (turn.dnd_campaign.operation && tool_turn != 5) {
        (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "campaign setup needs the matching D&D command");
        return;
    }
    if (turn.dnd_encounter_action.operation && tool_turn != 6) {
        (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "encounter change needs the matching D&D command");
        return;
    }

    if (prepare_dialogue(st, &turn, tool_turn, data, data_len, &pending_ticket,
            &dialogue, &dialogue_command) != 0) {
        (void)publish_event(st->nc, event_subject, turn.request_id, "failed",
            "D&D dialogue context unavailable; choose a campaign and scene or start a new conversation.");
        goto pending_done;
    }
    if (dialogue_command != DND_DIALOGUE_NONE && dialogue_command != DND_DIALOGUE_CANCEL_AND_ASK) {
        tool_turn = 8;
        governed_retrieval = 0;
    }
    phatic = tool_turn || governed_retrieval ? 0u : phatic_policy_match_v2(turn.text, turn.text_len);
    if ((phatic & 0xff) != 0) {
        const char *reply_text = phatic_policy_reply_v1(phatic & 0xff);
        cr_stage_clock stages = {0};
        if (turn.enable_tts &&
            (stage_clock_init(&stages) != 0 ||
             stage_clock_mark_first_text(&stages) != 0)) {
            (void)publish_event(
                st->nc, event_subject, turn.request_id, "failed", "cascade timing clock failed");
            goto pending_done;
        }
        stages.input = turn.input_stages;
        if (publish_text_event(
                st->nc, event_subject, turn.request_id, turn.request_id_len,
                5, reply_text, 0, 1, 0) != 0)
            svc_log("cascade-router", "final event publish failed request=%s", turn.request_id);
        if (turn.enable_tts &&
            publish_speakable(
                st,
                st->nc,
                event_subject,
                event_subject_len,
                turn.request_id,
                reply_text,
                &stages) != 0)
            (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "TTS dispatch failed");
        publish_event(st->nc, event_subject, turn.request_id, "done", "");
        goto pending_done;
    }

    packed = (tool_turn || dialogue_command == DND_DIALOGUE_CANCEL_AND_ASK) ? UINT64_MAX : (governed_retrieval ?
        ROUTE_CLASSIFIER_RETRIEVE_THEN_ESCALATE_V1 : route_classifier_packed_v1(route_text, route_text_len));
    if (packed != UINT64_MAX) {
        route = (uint32_t)(packed & 0x3u);
        if (route == ROUTE_CLASSIFIER_ANSWER_V1) {
            /* Only a whole social utterance can authorize a canned reply.
             * The classifier's broad answer label cannot discard other content. */
            route = ROUTE_CLASSIFIER_ESCALATE_V1;
        } else if (route == ROUTE_CLASSIFIER_RETRIEVE_THEN_ESCALATE_V1) {
            route_name = "retrieve_then_escalate";
            route_name_len = sizeof("retrieve_then_escalate") - 1u;
        } else {
            route = ROUTE_CLASSIFIER_ESCALATE_V1;
        }
    }

    /*
     * Lab SystemOne heuristics feeder (replaces retired Qwen cascade JSON).
     * Loop bypasses this synchronous lab hop. C routing remains its authority.
     * Empty SYSTEMONE_BASE_URL => skip; C classifier remains prod authority.
     * HTTP/parse/timeout/noul failures fail closed to escalate-safe.
     */
    if (!tool_turn && !loop_profile) {
        systemone_config_v1 s1_cfg;
        systemone_decision_v1 s1_decision;
        systemone_status_v1 s1_st;
        const char *c_route_name = route_name;
        systemone_config_from_env_v1(&s1_cfg);
        if (systemone_is_enabled_v1(&s1_cfg)) {
            s1_st = systemone_lab_request_v1(
                &s1_cfg, route_text, route_text_len, &s1_decision, 0);
            if (s1_st == SYSTEMONE_STATUS_READY_V1) {
                const char *s1_name = NULL;
                size_t s1_name_len = 0;
                uint32_t s1_enum = ROUTE_CLASSIFIER_ESCALATE_V1;
                systemone_status_v1 map_st = systemone_map_to_cascade_route_v1(
                    &s1_cfg, &s1_decision, &s1_name, &s1_name_len, &s1_enum);
                if (map_st == SYSTEMONE_STATUS_READY_V1 && s1_name != NULL && s1_name_len > 0) {
                    route = s1_enum;
                    route_name = s1_name;
                    route_name_len = s1_name_len;
                } else {
                    route = ROUTE_CLASSIFIER_ESCALATE_V1;
                    route_name = "escalate";
                    route_name_len = sizeof("escalate") - 1u;
                    s1_st = map_st;
                }
            } else if (s1_st != SYSTEMONE_STATUS_SKIPPED_UNSET_V1) {
                route = ROUTE_CLASSIFIER_ESCALATE_V1;
                route_name = "escalate";
                route_name_len = sizeof("escalate") - 1u;
            }
            systemone_ft_emit_jsonl_v1(
                &s1_cfg,
                turn.request_id,
                route_text,
                route_text_len,
                &s1_decision,
                s1_st,
                c_route_name);
        }
    }

    publish_route_event(
        st->nc,
        event_subject,
        turn.request_id,
        turn.request_id_len,
        route_name,
        route_name_len);

    {
        /* Retrieval and native OpenAI SSE stay in one bounded worker lane.
         * The VBus callback remains free to observe barge-in. */
        if (st->workers_started) {
            cr_llm_lane *lane = llm_lane_for_request(st, loop_profile ? turn.session_id : turn.request_id);
            cr_llm_job *job = llm_lane_acquire_job(lane);
            if (!job) {
                (void)publish_event(
                    st->nc, event_subject, turn.request_id, "failed", "model job capacity exhausted");
                goto pending_done;
            }
            job->enable_tts = turn.enable_tts;
            job->tool_turn = tool_turn;
            if (tool_turn == 4) {
                job->initiative = malloc(sizeof(*job->initiative));
                if (!job->initiative) {
                    (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "initiative job capacity unavailable");
                    llm_lane_release_job(lane, job);
                    goto pending_done;
                }
                *job->initiative = turn.dnd_initiative;
            }
            if (tool_turn == 5) {
                job->campaign_request = malloc(sizeof(*job->campaign_request));
                if (!job->campaign_request) {
                    (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "campaign job capacity unavailable");
                    llm_lane_release_job(lane, job);
                    goto pending_done;
                }
                *job->campaign_request = turn.dnd_campaign;
            }
            job->pending_ticket = pending_ticket;
            job->dialogue_command = dialogue_command;
            job->loop_profile = loop_profile;
            job->dnd_profile = !strcmp(turn.metadata.interaction_profile, "dnd_app");
            if (pending_ticket.generation || dialogue_command != DND_DIALOGUE_NONE) {
                job->dialogue = malloc(sizeof(*job->dialogue));
                if (!job->dialogue) {
                    (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "dialogue job capacity unavailable");
                    llm_lane_release_job(lane, job);
                    goto pending_done;
                }
                *job->dialogue = dialogue;
            }
            job->deadline_ms = deadline_ms;
            if (tool_turn == 6) {
                job->encounter_action = malloc(sizeof(*job->encounter_action));
                if (!job->encounter_action) {
                    (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "encounter job capacity unavailable");
                    llm_lane_release_job(lane, job);
                    goto pending_done;
                }
                *job->encounter_action = turn.dnd_encounter_action;
            }
            job->premium = turn.premium;
            job->model_request_capture = turn.model_request_capture;
            job->enable_rag = !tool_turn && (governed_retrieval || (turn.premium && turn.enable_rag &&
                route == ROUTE_CLASSIFIER_RETRIEVE_THEN_ESCALATE_V1));
            job->max_completion_tokens = openai_completion_limit(job->enable_rag ?
                st->grounded_max_completion_tokens : st->llm_max_completion_tokens,
                turn.metadata.turn_max_tokens);
            if (!job->max_completion_tokens) {
                (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "invalid completion token limit");
                llm_lane_release_job(lane, job);
                goto pending_done;
            }
            if (tool_turn) {
                route_text = turn.text;
                route_text_len = turn.text_len;
            }
            job->prompt_len = route_text_len;
            job->input_stages = turn.input_stages;
            if (copy_bounded_span(
                    job->request_id,
                    sizeof(job->request_id),
                    turn.request_id,
                    turn.request_id_len) != 0 ||
                copy_bounded_string(job->knowledge_scope, sizeof(job->knowledge_scope),
                    turn.metadata.knowledge_scope[0] ? turn.metadata.knowledge_scope : "shared_rulebook") != 0 ||
                copy_bounded_string(job->campaign_id, sizeof(job->campaign_id), turn.metadata.campaign_id) != 0 ||
                copy_bounded_string(job->scene_id, sizeof(job->scene_id), turn.metadata.scene_id) != 0 ||
                copy_bounded_string(job->character_id, sizeof(job->character_id), turn.metadata.character_id) != 0 ||
                copy_bounded_string(job->encounter_id, sizeof(job->encounter_id), turn.metadata.encounter_id) != 0 ||
                copy_bounded_span(
                    job->user_id,
                    sizeof(job->user_id),
                    turn.user_id,
                    turn.user_id_len) != 0 ||
                copy_bounded_span(
                    job->session_id,
                    sizeof(job->session_id),
                    turn.session_id,
                    turn.session_id_len) != 0 ||
                copy_bounded_span(
                    job->response_subject,
                    sizeof(job->response_subject),
                    event_subject,
                    event_subject_len) != 0 ||
                copy_bounded_span(
                    job->prompt,
                    sizeof(job->prompt),
                    route_text,
                    route_text_len) != 0 ||
                !job->request_id[0] || !job->response_subject[0] ||
                !job->prompt[0] ||
                queue_llm_job(st, job) != 0) {
                (void)publish_event(
                    st->nc, event_subject, turn.request_id, "failed", "model queue capacity exhausted");
                llm_lane_release_job(lane, job);
                goto pending_done;
            }
            /* The worker now owns completion of this generation ticket. */
            pending_ticket.generation = 0;
        } else {
            if (pending_ticket.generation || dialogue_command != DND_DIALOGUE_NONE) {
                (void)publish_event(st->nc, event_subject, turn.request_id, "failed", "dialogue worker unavailable");
                goto pending_done;
            }
            cr_stream *s = stream_get(st, turn.request_id, 1);
            if (!s) {
                (void)publish_event(
                    st->nc, event_subject, turn.request_id, "failed", "stream capacity exhausted");
                goto pending_done;
            }
            if (copy_bounded_span(
                    s->response_subject,
                    sizeof(s->response_subject),
                    event_subject,
                    event_subject_len) != 0 || !s->response_subject[0]) {
                (void)publish_event(
                    st->nc, event_subject, turn.request_id, "failed", "invalid event subject");
                stream_drop(st, turn.request_id);
                goto pending_done;
            }
            s->response_subject_len = event_subject_len;
            s->enable_tts = turn.enable_tts;
            s->stages.input = turn.input_stages;
        }
    }
pending_done:
    finish_pending(st, pending_ticket);
}

/* Idle-flush all open stream hosts; emit segments + keep host if remainder empty done not forced. */
static void poll_idle_flushes(cr_state *st) {
    int i;
    uint64_t now;
    if (!st || st->next_stream_maintenance_ns == 0u) return;
    now = mono_ns();
    if (now == 0u || st->next_stream_maintenance_ns > now) return;
    st->next_stream_maintenance_ns = 0u;
    for (i = 0; i < CR_MAX_STREAMS; ++i) {
        cr_stream *s = &st->streams[i];
        char part[SPEECH_SEG_HOST_MAX_PART];
        int em = 0;
        if (!s->in_use) continue;
        if (now != 0 && s->expires_ns != 0 && s->expires_ns <= now) {
            char rid[sizeof(s->request_id)];
            memcpy(rid, s->request_id, sizeof(rid));
            (void)publish_stream_failure(
                st->nc, s, "model stream timeout");
            stream_drop(st, rid);
            svc_log("cascade-router", "expire idle token stream request=%s", rid);
            continue;
        }
        if (speech_seg_host_flush_if_idle_v1(&s->host, now, part, &em) != SPEECH_SEG_OK) {
            stream_schedule_maintenance(st, s);
            continue;
        }
        if (em) {
            if (stream_queue_segment(st->nc, s, part, strlen(part)) != 0) {
                svc_log("cascade-router", "idle flush publish failed request=%s", s->request_id);
                (void)publish_stream_failure(
                    st->nc, s, "model stream segment failed");
                stream_drop(st, s->request_id);
                continue;
            }
            svc_log("cascade-router", "idle flush request=%s", s->request_id);
        }
        stream_schedule_maintenance(st, s);
    }
}

/*
 * Streaming model tokens: payload "request_id\nchunk" or turn_start-shaped.
 * Empty chunk after newline = flush_all + done.
 */
static void on_token(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    cr_state *st = (cr_state *)user;
    char rid[128];
    char chunk[2048];
    cr_stream *s;
    char parts[SPEECH_SEG_MAX_PARTS][SPEECH_SEG_HOST_MAX_PART];
    size_t nparts = 0, i;
    size_t nl = 0;
    int found_nl = 0;
    int decoded_turn = 0;
    int enable_tts = 1;
    turn_input_stage_timestamps_c input_stages;
    (void)subject;
    (void)reply;

    rid[0] = '\0';
    chunk[0] = '\0';
    memset(&input_stages, 0, sizeof(input_stages));
    if (data_len != 0u && data[0] == 0x0au) {
        turn_start_c t;
        if (pb_decode_turn_start(data, data_len, &t) != 0) return;
        enable_tts = t.enable_tts;
        input_stages = t.input_stages;
        if (copy_bounded_string(rid, sizeof(rid), t.request_id) != 0 ||
            copy_bounded_string(chunk, sizeof(chunk), t.text) != 0) return;
        decoded_turn = 1;
    }
    for (nl = 0; !decoded_turn && nl < data_len; ++nl) {
        if (data[nl] == '\n') {
            size_t rl = nl;
            size_t cl = data_len - nl - 1;
            if (rl == 0 || rl >= sizeof(rid) || cl >= sizeof(chunk) ||
                memchr(data, '\0', data_len) != NULL) {
                svc_log("cascade-router", "reject oversized or binary token bytes=%zu", data_len);
                return;
            }
            memcpy(rid, data, rl);
            rid[rl] = '\0';
            {
                memcpy(chunk, data + nl + 1, cl);
                chunk[cl] = '\0';
            }
            found_nl = 1;
            break;
        }
    }
    if (!found_nl && !decoded_turn) {
        /* try turn_start wire */
        turn_start_c t;
        if (pb_decode_turn_start(data, data_len, &t) != 0) return;
        enable_tts = t.enable_tts;
        input_stages = t.input_stages;
        if (copy_bounded_string(rid, sizeof(rid), t.request_id) != 0 ||
            copy_bounded_string(chunk, sizeof(chunk), t.text) != 0) return;
    }
    if (!valid_request_id(rid)) {
        svc_log("cascade-router", "reject token with invalid request_id");
        return;
    }

    s = stream_get(st, rid, 1);
    if (!s) return;
    if (!found_nl) {
        s->enable_tts = enable_tts;
        s->stages.input = input_stages;
    }
    if (!s->response_subject[0]) {
        if (copy_event_subject_span(
                s->response_subject,
                sizeof(s->response_subject),
                s->request_id,
                s->request_id_len,
                &s->response_subject_len) != 0) {
            stream_drop(st, rid);
            return;
        }
    }

    char rendered[SPEECH_MARKUP_INPUT_CAP];
    size_t rendered_len = 0;
    if (speech_markup_feed_v1(&s->markup, chunk, strlen(chunk), chunk[0] == '\0',
            rendered, sizeof(rendered), &rendered_len) != SPEECH_MARKUP_OK) {
        (void)publish_stream_failure(st->nc, s, "invalid model presentation text");
        stream_drop(st, rid);
        return;
    }

    if (rendered_len) {
        const char *add = rendered;
        size_t add_len = rendered_len;
        if (add_len >= sizeof(s->full_text) - s->full_text_len) {
            (void)publish_stream_failure(
                st->nc, s, "model stream capacity exceeded");
            stream_drop(st, rid);
            return;
        }
        if (stage_clock_mark_first_text(&s->stages) != 0) {
            (void)publish_stream_failure(
                st->nc, s, "cascade timing clock failed");
            stream_drop(st, rid);
            return;
        }
        memcpy(s->full_text + s->full_text_len, add, add_len);
        s->full_text_len += add_len;
        s->full_text[s->full_text_len] = '\0';
        if (s->enable_tts && speech_seg_host_add_v1(
                &s->host, mono_ns(), add, add_len, parts, SPEECH_SEG_MAX_PARTS, &nparts
            ) != SPEECH_SEG_OK) {
            (void)publish_stream_failure(
                st->nc, s, "model stream segmentation failed");
            stream_drop(st, rid);
            return;
        }
        if (s->enable_tts) stream_schedule_maintenance(st, s);
        for (i = 0; s->enable_tts && i < nparts; ++i) {
            if (stream_queue_segment(
                    st->nc, s, parts[i], strlen(parts[i])) != 0) {
                svc_log("cascade-router", "token segment publish failed request=%s", rid);
                (void)publish_stream_failure(
                    st->nc, s, "model stream segment failed");
                stream_drop(st, rid);
                return;
            }
        }
        if (s->enable_tts &&
            stream_release_pending_before_remainder(st->nc, s) != 0) {
            svc_log("cascade-router", "token lookahead publish failed request=%s", rid);
            (void)publish_stream_failure(
                st->nc, s, "model stream segment failed");
            stream_drop(st, rid);
            return;
        }
        svc_log("cascade-router", "token request=%s segs=%zu", rid, nparts);
    }
    if (chunk[0] == '\0') {
        if (s->full_text_len == 0 ||
            publish_text_event(
                st->nc,
                s->response_subject,
                rid,
                s->request_id_len,
                5,
                s->full_text,
                0,
                1,
                0) != 0 ||
            (s->enable_tts && stream_finish_segments(st->nc, s) != 0)) {
            svc_log("cascade-router", "token flush publish failed request=%s", rid);
            (void)publish_stream_failure(
                st->nc, s, "empty or invalid model stream");
        } else {
            publish_event(st->nc, s->response_subject, rid, "done", "");
        }
        stream_drop(st, rid);
        svc_log("cascade-router", "token flush done request=%s", rid);
        return;
    }
}

static void on_cancel(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    cr_state *st = (cr_state *)user;
    turn_cancel_c cancel;
    int matched;
    (void)subject;
    (void)reply;
    if (pb_decode_turn_cancel(data, data_len, &cancel) != 0) {
        svc_log("cascade-router", "reject malformed cancel");
        return;
    }
    if (strcmp(cancel.reason, TURN_CANCEL_REASON_UPSTREAM_FAILURE) == 0) return;
    pthread_mutex_lock(&st->pending_mutex);
    (void)dnd_pending_interrupt(&st->pending, cancel.user_id, cancel.request_id,
        mono_ns() / UINT64_C(1000000));
    pthread_mutex_unlock(&st->pending_mutex);
    stream_drop(st, cancel.request_id);
    matched = cancel_llm_job(st, cancel.request_id);
    if (!matched) remember_early_cancel(st, cancel.request_id);
    svc_log(
        "cascade-router",
        "cancel request=%s llm_matched=%d",
        cancel.request_id,
        matched);
}

int main(void) {
    vbus_stop_flag stop = 0;
    cr_state *st;
    const char *queue;
    int i;
    int run_rc = 0;
    st = (cr_state *)calloc(1, sizeof(*st));
    if (!st) {
        svc_log("cascade-router", "state allocation failed");
        return 1;
    }
    svc_install_signals(&stop);
    st->nc = svc_connect_bus();
    if (!st->nc) {
        free(st);
        return 1;
    }
    {
        const char *url = svc_env("LLM_HTTP_URL", "");
        const char *model = svc_env("LLM_MODEL", "default");
        const char *default_budget = getenv("TURN_BUDGET_MS");
        const char *tool_url = svc_env("TOOL_HTTP_URL", "");
        const char *tool_secret = svc_env("TOOL_HTTP_AUTH_SECRET", "");
        if (copy_bounded_string(st->tool_url, sizeof(st->tool_url), tool_url) != 0 ||
            copy_bounded_string(st->tool_secret, sizeof(st->tool_secret), tool_secret) != 0 ||
            ((tool_url[0] || tool_secret[0]) && !dnd_tools_config_valid(tool_url, tool_secret))) {
            svc_log("cascade-router", "invalid tool HTTP configuration");
            vbus_close(st->nc);
            free(st);
            return 1;
        }
        st->tool_timeout_ms = svc_env_int_range("TOOL_HTTP_TIMEOUT_MS", 5000, 1, 60000);
        if (copy_bounded_string(st->llm_url, sizeof(st->llm_url), url) != 0 ||
            copy_bounded_string(
                st->llm_model, sizeof(st->llm_model), model) != 0 ||
            !st->llm_model[0]) {
            svc_log("cascade-router", "invalid LLM configuration");
            vbus_close(st->nc);
            free(st);
            return 1;
        }
        if (default_budget && default_budget[0] &&
            turn_budget_parse_ms_v1(
                default_budget, &st->default_turn_budget_ms) != TURN_BUDGET_OK)
            st->default_turn_budget_invalid = 1;
        st->llm_timeout_ms = svc_env_int_range(
            "LLM_HTTP_TIMEOUT_MS", 60000, 1, 600000);
        st->llm_max_completion_tokens = (uint32_t)svc_env_int_range(
            "LLM_MAX_COMPLETION_TOKENS", 48, 1, 256);
        st->grounded_max_completion_tokens = openai_completion_limit(4096u,
            svc_env("LLM_GROUNDED_MAX_COMPLETION_TOKENS", "256"));
        if (!st->grounded_max_completion_tokens || st->grounded_max_completion_tokens > 256u) {
            svc_log("cascade-router", "invalid grounded completion token configuration");
            vbus_close(st->nc);
            free(st);
            return 1;
        }
        st->eager_tts_segments = svc_env_int_range(
            "CASCADE_EAGER_TTS_SEGMENTS", 0, 0, 1);
        speech_seg_config_default_v1(&st->segment_config);
        st->segment_config.min_segment_chars = CR_MIN_SEGMENT_CHARS;
        st->segment_config.max_segment_chars = svc_env_int_range(
            "CASCADE_MAX_SEGMENT_CHARS",
            CR_MAX_FIRST_SEGMENT_CHARS,
            CR_MIN_SEGMENT_CHARS,
            CR_MAX_FIRST_SEGMENT_CHARS);
        st->segment_config.first_segment_chars = svc_env_int_range(
            "CASCADE_FIRST_SEGMENT_CHARS",
            CR_DEFAULT_FIRST_SEGMENT_CHARS,
            CR_MIN_FIRST_SEGMENT_CHARS,
            CR_MAX_FIRST_SEGMENT_CHARS);
        speech_seg_config_normalize_v1(&st->segment_config);
        st->rag_timeout_ms = svc_env_int_range(
            "RAG_TIMEOUT_MS", 1500, 1, 600000);
        st->stream_idle_ns =
            (uint64_t)svc_env_int_range(
                "CASCADE_STREAM_IDLE_TIMEOUT_MS",
                CR_DEFAULT_STREAM_IDLE_TIMEOUT_MS,
                1000,
                CR_MAX_STREAM_IDLE_TIMEOUT_MS) * UINT64_C(1000000);
    }
    if (st->llm_url[0] && dnd_grounding_load(getenv("PROMPT_LIBRARY_ROOT"), &st->grounding) != 0) {
        svc_log("cascade-router", "canonical grounding prompt unavailable");
        vbus_close(st->nc);
        free(st);
        return 1;
    }
    if (st->llm_url[0] && voice_response_prompts_load(getenv("PROMPT_LIBRARY_ROOT"),
            &st->grounding, &st->response_prompts) != 0) {
        svc_log("cascade-router", "canonical response style unavailable or exceeds prompt capacity");
        vbus_close(st->nc);
        free(st);
        return 1;
    }
    if (dnd_pending_init(&st->pending, UINT64_C(1800000)) != 0 ||
        pthread_mutex_init(&st->pending_mutex, NULL) != 0) {
        svc_log("cascade-router", "dialogue state initialization failed");
        vbus_close(st->nc);
        free(st);
        return 1;
    }
    if ((st->llm_url[0] || st->tool_url[0]) && start_llm_workers(st) != 0) {
        svc_log("cascade-router", "LLM worker startup failed");
        vbus_close(st->nc);
        pthread_mutex_destroy(&st->pending_mutex);
        free(st);
        return 1;
    }
    queue = svc_env("VBUS_QUEUE_GROUP", "cascade-routers");
    if (vbus_subscribe(st->nc, SUBJ_TURN_START, queue, on_turn_start, st) != 0 ||
        vbus_subscribe(st->nc, SUBJ_TURN_TOKEN, queue, on_token, st) != 0 ||
        vbus_subscribe(st->nc, SUBJ_TURN_CANCEL, NULL, on_cancel, st) != 0) {
        svc_log("cascade-router", "subscribe failed");
        stop_llm_workers(st);
        vbus_close(st->nc);
        pthread_mutex_destroy(&st->pending_mutex);
        free(st);
        return 1;
    }
    svc_log(
        "cascade-router",
        st->workers_started ?
            "pure-C direct admission + bounded cancelable OpenAI SSE workers" :
            "pure-C direct admission + external token stream"
    );
    while (atomic_load_explicit(&stop, memory_order_relaxed) == 0) {
        if (vbus_poll(st->nc, 50) != 0) {
            run_rc = 1;
            break;
        }
        poll_idle_flushes(st);
    }
    for (i = 0; i < CR_MAX_STREAMS; ++i) {
        if (st->streams[i].in_use) speech_seg_host_free_v1(&st->streams[i].host);
    }
    stop_llm_workers(st);
    vbus_close(st->nc);
    pthread_mutex_destroy(&st->pending_mutex);
    free(st);
    return run_rc;
}
