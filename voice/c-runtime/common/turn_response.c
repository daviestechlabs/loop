/* Canonical public response boundary shared by the pure-C gateways. */

#include "turn_response.h"

#include "utf8.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define BYTE_LANES_ONES UINT64_C(0x0101010101010101)
#define BYTE_LANES_HIGH UINT64_C(0x8080808080808080)
#define BYTE_LANES_SPACE UINT64_C(0x2020202020202020)

#if defined(__GNUC__) || defined(__clang__)
#define TURN_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define TURN_ALWAYS_INLINE inline
#endif

static int byte_lanes_have_value(uint64_t word, uint64_t repeated_value) {
    uint64_t matched = word ^ repeated_value;
    return ((matched - BYTE_LANES_ONES) & ~matched & BYTE_LANES_HIGH) != 0u;
}

static int public_text_word_is_valid_ascii(uint64_t word) {
    uint64_t below_space =
        (word - BYTE_LANES_SPACE) & ~word & BYTE_LANES_HIGH;
    return (word & BYTE_LANES_HIGH) == 0u && below_space == 0u &&
        !byte_lanes_have_value(word, UINT64_C(0x7f7f7f7f7f7f7f7f));
}

static int public_text_word_needs_json_escape(uint64_t word) {
    return byte_lanes_have_value(word, UINT64_C(0x2222222222222222)) ||
        byte_lanes_have_value(word, UINT64_C(0x5c5c5c5c5c5c5c5c));
}

static int safe_identifier(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    size_t length = 0;
    if (!cursor || !*cursor) return 0;
    while (*cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') ||
              *cursor == '-' || *cursor == '_' || *cursor == '.' ||
              *cursor == ':' || *cursor == '@')) return 0;
        cursor++;
        if (++length > 127u) return 0;
    }
    return 1;
}

static int public_text_valid(
    const char *value,
    size_t value_cap,
    size_t *value_len,
    int *json_raw
) {
    const unsigned char *cursor = (const unsigned char *)value;
    size_t length = 0;
    int non_ascii = 0;
    if (!value || value_cap == 0 || !value_len || !json_raw) return 0;
    *value_len = 0u;
    *json_raw = 1;
    while (length < value_cap && cursor[length]) {
        if (cursor[length] < 0x20u || cursor[length] == 0x7fu) return 0;
        if (cursor[length] >= 0x80u) non_ascii = 1;
        if (cursor[length] == (unsigned char)'"' ||
            cursor[length] == (unsigned char)'\\')
            *json_raw = 0;
        length++;
    }
    if (length == value_cap ||
        (non_ascii && !utf8_validate_v1(cursor, length))) return 0;
    *value_len = length;
    return 1;
}

static TURN_ALWAYS_INLINE int public_text_span_valid_impl(
    const char *value,
    size_t value_cap,
    size_t length,
    size_t *value_len,
    int *json_raw,
    int require_terminator
) {
    const unsigned char *cursor = (const unsigned char *)value;
    size_t offset = 0u;
    int non_ascii = 0;
    if (!value || value_cap == 0u || length == 0u || length >= value_cap ||
        !value_len || !json_raw ||
        (require_terminator && value[length] != '\0')) return 0;
    *value_len = 0u;
    *json_raw = 1;
    while (length - offset >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, cursor + offset, sizeof(word));
        if (!public_text_word_is_valid_ascii(word)) break;
        if (public_text_word_needs_json_escape(word)) *json_raw = 0;
        offset += sizeof(word);
    }
    while (offset < length) {
        if (cursor[offset] < 0x20u || cursor[offset] == 0x7fu) return 0;
        if (cursor[offset] >= 0x80u) non_ascii = 1;
        if (cursor[offset] == (unsigned char)'"' ||
            cursor[offset] == (unsigned char)'\\')
            *json_raw = 0;
        offset++;
    }
    if (non_ascii && !utf8_validate_v1(cursor, length)) return 0;
    *value_len = length;
    return 1;
}

static TURN_ALWAYS_INLINE int public_text_span_valid(
    const char *value,
    size_t value_cap,
    size_t length,
    size_t *value_len,
    int *json_raw
) {
    return public_text_span_valid_impl(
        value, value_cap, length, value_len, json_raw, 1);
}

static TURN_ALWAYS_INLINE int public_text_borrowed_span_valid(
    const char *value,
    size_t value_cap,
    size_t length,
    size_t *value_len,
    int *json_raw
) {
    return public_text_span_valid_impl(
        value, value_cap, length, value_len, json_raw, 0);
}

static const char *event_type_name(int type_id) {
    static const char *const names[] = {
        "", "started", "thinking_started", "thinking_ended", "text_delta",
        "text_completed", "tts_segment", "pcm_started", "pcm_chunk", "pcm_ended",
        "completed", "canceled", "failed"
    };
    if (type_id < 1 || type_id >= (int)(sizeof(names) / sizeof(names[0]))) return "";
    return names[type_id];
}

static size_t event_type_length(int type_id) {
    static const uint8_t lengths[] = {
        0u, 7u, 16u, 14u, 10u, 14u, 11u, 11u, 9u, 9u, 9u, 8u, 6u
    };
    return lengths[type_id];
}

static int canonical_event_type(const turn_event_c *event) {
    const char *canonical;
    if (!event) return 0;
    canonical = event_type_name(event->type_id);
    return canonical[0] && strcmp(event->type, canonical) == 0;
}

static void response_event_base(
    turn_response_event *out,
    const turn_event_c *event,
    const char *request_id,
    size_t request_id_len
) {
    out->request_id = request_id;
    out->type = event->type;
    out->request_id_len = (uint16_t)request_id_len;
    out->type_len = (uint16_t)event_type_length(event->type_id);
    out->text_len = 0u;
    out->text_json_raw = 0u;
    out->text_validated = 0u;
    out->text = "";
    out->speech_text = "";
    out->display_text = "";
    out->audio = NULL;
    out->audio_len = 0;
    out->type_id = event->type_id;
    out->sample_rate = 0;
    out->channels = 0;
    out->bit_depth = 0;
    out->sequence = 0;
    out->segment_index = 0;
    out->is_final = 0;
    out->audio_encoding = 0;
    out->stages = NULL;
    out->tool_result = NULL;
    out->provider_model = NULL;
    out->provider_usage = NULL;
    out->retrieval = NULL;
    out->encounter = NULL;
    out->roster = NULL;
    out->initiative = NULL;
    out->current_tts_stage_wire = NULL;
    out->runtime_identity_sha256 = NULL;
    out->runtime_identity_sha256_len = 0u;
}

static int stage_timestamps_valid(const turn_event_c *event) {
    const turn_stage_timestamps_c *stages;
    int any;
    int input_any;
    if (!event) return 0;
    stages = &event->stages;
    input_any = stages->input.audio_committed_at_ms != 0 ||
        stages->input.stt_request_received_at_ms != 0 ||
        stages->input.stt_provider_request_started_at_ms != 0 ||
        stages->input.stt_provider_ready_at_ms != 0 ||
        stages->input.stt_transcript_published_at_ms != 0;
    any = input_any || stages->first_text_at_ms != 0 ||
        stages->tts_segment_emitted_at_ms != 0 ||
        stages->tts_request_received_at_ms != 0 ||
        stages->tts_provider_request_started_at_ms != 0 ||
        stages->tts_provider_ready_at_ms != 0 || stages->pcm_started_at_ms != 0 ||
        stages->pcm_first_chunk_at_ms != 0;
    if (!any) return 1;
    if (event->type_id != 7 && event->type_id != 8) return 0;
    if (input_any &&
        (stages->input.audio_committed_at_ms <= 0 ||
         stages->input.stt_request_received_at_ms <
            stages->input.audio_committed_at_ms ||
         stages->input.stt_provider_request_started_at_ms <
            stages->input.stt_request_received_at_ms ||
         stages->input.stt_provider_ready_at_ms <
            stages->input.stt_provider_request_started_at_ms ||
         stages->input.stt_transcript_published_at_ms <
            stages->input.stt_provider_ready_at_ms ||
         stages->first_text_at_ms <
            stages->input.stt_transcript_published_at_ms)) return 0;
    if ((stages->first_text_at_ms == 0) !=
        (stages->tts_segment_emitted_at_ms == 0)) return 0;
    if (stages->first_text_at_ms > 0 &&
        (stages->tts_segment_emitted_at_ms < stages->first_text_at_ms ||
         stages->tts_request_received_at_ms <
            stages->tts_segment_emitted_at_ms)) return 0;
    if (stages->tts_request_received_at_ms <= 0 ||
        stages->tts_provider_request_started_at_ms <
            stages->tts_request_received_at_ms ||
        stages->tts_provider_ready_at_ms <
            stages->tts_provider_request_started_at_ms ||
        stages->pcm_started_at_ms < stages->tts_provider_ready_at_ms)
        return 0;
    if (event->type_id == 7) return stages->pcm_first_chunk_at_ms == 0;
    return stages->pcm_first_chunk_at_ms >= stages->pcm_started_at_ms;
}

int turn_encounter_valid(const turn_encounter_c *encounter, const turn_tool_result_c *tool) {
    turn_encounter_state_c state;
    turn_encounter_participant_c participant;
    pb_reader reader;
    int rc;
    if (!encounter || !pb_turn_tool_result_valid(tool) || strcmp(tool->tool_id, "dnd-encounter-state") ||
        pb_decode_turn_encounter(encounter->data, encounter->length, &state) != 0 ||
        strcmp(state.tool_call_id, tool->tool_call_id) || strcmp(state.output_sha256, tool->output_sha256)) return 0;
    pb_reader_init(&reader, encounter->data, encounter->length);
    while ((rc = pb_encounter_participant_next(&reader, &participant)) == 1) {
        size_t length;
        int raw;
        if (!public_text_valid(participant.name, sizeof(participant.name), &length, &raw)) return 0;
    }
    return rc == 0;
}

int turn_roster_valid(const turn_roster_c *roster, const turn_tool_result_c *tool) {
    turn_campaign_roster_c state;
    turn_campaign_character_c character;
    pb_reader reader;
    int rc;
    if (!roster || !pb_turn_tool_result_valid(tool) || strcmp(tool->tool_id, "dnd-campaign-state") ||
        pb_decode_turn_roster(roster->data, roster->length, &state) ||
        strcmp(state.tool_call_id, tool->tool_call_id) || strcmp(state.output_sha256, tool->output_sha256)) return 0;
    pb_reader_init(&reader, roster->data, roster->length);
    while ((rc = pb_roster_character_next(&reader, &character)) == 1) {
        size_t length;
        int raw;
        if (!public_text_valid(character.name, sizeof(character.name), &length, &raw)) return 0;
    }
    return rc == 0;
}

int turn_state_result_valid(const turn_encounter_c *encounter, const turn_roster_c *roster,
    const turn_initiative_c *initiative, const turn_tool_result_c *tool) {
    turn_encounter_state_c state;
    turn_initiative_result_c result;
    turn_initiative_roll_c roll;
    pb_reader reader;
    int rc;
    if (roster) return !encounter && !initiative && turn_roster_valid(roster, tool);
    if (!encounter) return !initiative && (!tool ||
        (pb_turn_tool_result_valid(tool) && (!strcmp(tool->tool_id, "dnd-dice-roll") ||
                                           !strcmp(tool->tool_id, "dnd-scene-presence"))));
    if (!turn_encounter_valid(encounter, tool) ||
        pb_decode_turn_encounter(encounter->data, encounter->length, &state)) return 0;
    /* The snapshot decoder admits the exact operation set. Only initiative
     * requires dice receipts; other signed state operations must omit them. */
    if (strcmp(state.operation, "roll_initiative")) return !initiative;
    if (!initiative || strcmp(state.status, "active") ||
        pb_decode_turn_initiative(initiative->data, initiative->length, &result)) return 0;
    pb_reader_init(&reader, initiative->data, initiative->length);
    while ((rc = pb_initiative_roll_next(&reader, &roll)) == 1) {
        turn_encounter_participant_c participant;
        pb_reader parts;
        int found = 0, part_rc;
        pb_reader_init(&parts, encounter->data, encounter->length);
        while ((part_rc = pb_encounter_participant_next(&parts, &participant)) == 1) {
            if (!strcmp(participant.id, roll.character_id)) {
                if (participant.initiative != roll.total) return 0;
                found = 1;
            }
        }
        if (part_rc || !found) return 0;
    }
    return rc == 0;
}

int turn_retrieval_valid(const turn_retrieval_c *retrieval) {
    turn_retrieval_c checked;
    pb_reader reader;
    dnd_rag_citation citation;
    int rc;
    if (!retrieval || pb_decode_retrieval_provenance(retrieval->data, retrieval->length, &checked) != 0 ||
        checked.count != retrieval->count) return 0;
    pb_reader_init(&reader, checked.data, checked.length);
    while ((rc = pb_retrieval_citation_next(&reader, &citation)) == 1) {
        const char *fields[] = {citation.source, citation.book_slug, citation.collection,
            citation.corpus_version, citation.embedding_model, citation.record_id,
            citation.document_id, citation.content_hash, citation.source_sha256, citation.section};
        size_t i;
        for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
            if (!utf8_validate_v1((const uint8_t *)fields[i], strlen(fields[i]))) return 0;
    }
    return rc == 0;
}

enum turn_response_action turn_response_filter_public_active_n(
    turn_response_state *state,
    const turn_event_c *event,
    const char *request_id,
    size_t request_id_len,
    turn_response_event *safe_event
) {
    const char *display;
    size_t display_len;
    int display_json_raw;
    assert(state && event && request_id && request_id_len != 0u &&
           request_id_len <= 127u && request_id[0] != '\0' && safe_event);
    if (state->terminal ||
        event->type_id < 1 || event->type_id > 12)
        return TURN_RESPONSE_REJECT;
    if (event->type_id == 6) return TURN_RESPONSE_DROP;
    response_event_base(safe_event, event, request_id, request_id_len);
    if (event->retrieval.data || event->retrieval.length || event->retrieval.count) {
        if (event->type_id != 5 || !turn_retrieval_valid(&event->retrieval)) return TURN_RESPONSE_REJECT;
        safe_event->retrieval = &event->retrieval;
    }
    if (event->tool_result.present) {
        if ((event->type_id != 5 && event->type_id != 10) ||
            !pb_turn_tool_result_valid(&event->tool_result)) return TURN_RESPONSE_REJECT;
        safe_event->tool_result = &event->tool_result;
    }
    if (event->encounter.data || event->encounter.length) {
        if (event->type_id != 5 || event->retrieval.data) return TURN_RESPONSE_REJECT;
        safe_event->encounter = &event->encounter;
    }
    if (event->roster.data || event->roster.length) {
        if (event->type_id != 5 || event->retrieval.data) return TURN_RESPONSE_REJECT;
        safe_event->roster = &event->roster;
    }
    if (event->initiative.data || event->initiative.length) {
        if (event->type_id != 5 || event->retrieval.data) return TURN_RESPONSE_REJECT;
        safe_event->initiative = &event->initiative;
    }
    if (event->type_id == 5 && !turn_state_result_valid(safe_event->encounter,
        safe_event->roster, safe_event->initiative, safe_event->tool_result)) return TURN_RESPONSE_REJECT;
    if (event->type_id == 4 || event->type_id == 5) {
        if (state->text_completed || state->model_completed ||
            (event->type_id == 4 && event->is_final) ||
            (event->type_id == 5 && !event->is_final))
            return TURN_RESPONSE_REJECT;
        if (event->display_text_borrowed != 0u) {
            if (event->display_text_borrowed != 1u)
                return TURN_RESPONSE_REJECT;
            display = event->display_text_view;
            if (!public_text_borrowed_span_valid(
                    display, sizeof(event->display_text),
                    event->display_text_len, &display_len,
                    &display_json_raw))
                return TURN_RESPONSE_REJECT;
        } else {
            display = event->display_text;
            if (!display[0] ||
                !(event->display_text_len_known ?
                    public_text_span_valid(
                        display, sizeof(event->display_text),
                        event->display_text_len, &display_len,
                        &display_json_raw) :
                    public_text_valid(
                        display, sizeof(event->display_text), &display_len,
                        &display_json_raw)))
                return TURN_RESPONSE_REJECT;
        }
        safe_event->text = display;
        safe_event->display_text = display;
        safe_event->text_len = (uint16_t)display_len;
        safe_event->text_json_raw = (uint8_t)display_json_raw;
        safe_event->text_validated = 1u;
        safe_event->segment_index = event->segment_index;
        safe_event->is_final = event->is_final;
        if (event->type_id == 5) {
            size_t model_len;
            int raw;
            uint64_t counts[3];
            if ((event->provider.seen & 3u) == 3u && !(event->provider.invalid & 3u) &&
                !strcmp(event->provider.model_source, "provider_reported") &&
                public_text_valid(event->provider.model, sizeof(event->provider.model), &model_len, &raw))
                safe_event->provider_model = event->provider.model;
            if (pb_turn_provider_usage(&event->provider, counts)) safe_event->provider_usage = &event->provider;
            state->text_completed = 1;
        }
        return TURN_RESPONSE_FORWARD;
    }
    if (event->type_id == 7) {
        if (state->pcm_open || state->final_pcm_seen || !stage_timestamps_valid(event))
            return TURN_RESPONSE_REJECT;
        safe_event->stages = &event->stages;
        safe_event->current_tts_stage_wire = event->current_tts_stage_wire;
        state->pcm_open = 1;
        return TURN_RESPONSE_FORWARD;
    }
    if (event->type_id == 8) {
        if (!state->pcm_open || state->final_pcm_seen || event->audio_len == 0 ||
            event->audio_len > TURN_RESPONSE_MAX_AUDIO_EVENT || event->audio_len % 2u != 0 ||
            event->sample_rate < 8000 || event->sample_rate > 48000 ||
            event->channels != 1 || event->bit_depth != 16 ||
            event->audio_encoding != 1 || event->sequence < 0 || event->segment_index < 0 ||
            !stage_timestamps_valid(event) ||
            (state->have_pcm_sequence &&
             (event->segment_index < state->last_pcm_segment ||
              (event->segment_index == state->last_pcm_segment &&
               event->sequence <= state->last_pcm_sequence))))
            return TURN_RESPONSE_REJECT;
        safe_event->audio = event->audio;
        safe_event->audio_len = event->audio_len;
        safe_event->sample_rate = event->sample_rate;
        safe_event->channels = event->channels;
        safe_event->bit_depth = event->bit_depth;
        safe_event->audio_encoding = event->audio_encoding;
        safe_event->sequence = event->sequence;
        safe_event->segment_index = event->segment_index;
        safe_event->is_final = event->is_final;
        safe_event->stages = &event->stages;
        safe_event->current_tts_stage_wire = event->current_tts_stage_wire;
        state->last_pcm_sequence = event->sequence;
        state->last_pcm_segment = event->segment_index;
        state->have_pcm_sequence = 1;
        if (event->is_final) state->final_pcm_seen = 1;
        return TURN_RESPONSE_FORWARD;
    }
    if (event->type_id == 9) {
        if (!state->pcm_open) return TURN_RESPONSE_REJECT;
        state->pcm_open = 0;
        if (state->final_pcm_seen) state->final_pcm_ended = 1;
        if (state->wait_for_pcm && state->model_completed && state->final_pcm_ended) {
            state->terminal = 1;
            return TURN_RESPONSE_FORWARD_AND_COMPLETE;
        }
        return TURN_RESPONSE_FORWARD;
    }
    if (event->type_id == 10) {
        if (state->model_completed || !state->text_completed) return TURN_RESPONSE_REJECT;
        state->model_completed = 1;
        if (state->wait_for_pcm && !state->final_pcm_ended)
            return TURN_RESPONSE_HOLD_COMPLETED;
        state->terminal = 1;
        return TURN_RESPONSE_FORWARD_TERMINAL;
    }
    if (event->type_id == 11 || event->type_id == 12) {
        if (event->type_id == 12)
            safe_event->text = "upstream_failed";
        state->terminal = 1;
        return TURN_RESPONSE_FORWARD_TERMINAL;
    }
    if (event->type_id == 2) {
        if (state->thinking_started) return TURN_RESPONSE_DROP;
        state->thinking_started = 1;
        return TURN_RESPONSE_FORWARD;
    }
    if (event->type_id == 1 || event->type_id == 3) return TURN_RESPONSE_FORWARD;
    return TURN_RESPONSE_REJECT;
}

enum turn_response_action turn_response_filter_active_n(
    turn_response_state *state,
    const turn_event_c *event,
    const char *request_id,
    size_t request_id_len,
    turn_response_event *safe_event
) {
    if (event && event->display_text_borrowed != 0u)
        return TURN_RESPONSE_REJECT;
    return turn_response_filter_public_active_n(
        state, event, request_id, request_id_len, safe_event);
}

enum turn_response_action turn_response_filter_bound_n(
    turn_response_state *state,
    const turn_event_c *event,
    const char *request_id,
    size_t request_id_len,
    turn_response_event *safe_event
) {
    if (!state || !event || !request_id || request_id_len == 0u ||
        request_id_len > 127u || request_id[0] == '\0' || !safe_event)
        return TURN_RESPONSE_REJECT;
    return turn_response_filter_active_n(
        state, event, request_id, request_id_len, safe_event);
}

enum turn_response_action turn_response_filter_bound(
    turn_response_state *state,
    const turn_event_c *event,
    const char *request_id,
    turn_response_event *safe_event
) {
    return turn_response_filter_bound_n(
        state, event, request_id, request_id ? strlen(request_id) : 0u,
        safe_event);
}

enum turn_response_action turn_response_filter_decoded(
    turn_response_state *state,
    const turn_event_c *event,
    turn_response_event *safe_event
) {
    if (!event || !safe_identifier(event->request_id)) return TURN_RESPONSE_REJECT;
    return turn_response_filter_bound_n(
        state, event, event->request_id, strlen(event->request_id), safe_event);
}

enum turn_response_action turn_response_filter(
    turn_response_state *state,
    const turn_event_c *event,
    turn_response_event *safe_event
) {
    if (!event || !canonical_event_type(event)) return TURN_RESPONSE_REJECT;
    return turn_response_filter_decoded(state, event, safe_event);
}

size_t turn_response_protobuf_encode(
    uint8_t *out,
    size_t out_cap,
    const turn_response_event *event
) {
    if (!out || out_cap == 0 || !event || !event->request_id || !event->type ||
        !event->text || !event->speech_text || !event->display_text) return 0;
    if (event->retrieval && (event->type_id != 5 || !event->is_final ||
        !turn_retrieval_valid(event->retrieval))) return 0;
    if ((event->encounter || event->roster || event->initiative) &&
        (event->type_id != 5 || !event->is_final || event->retrieval)) return 0;
    if (event->type_id == 5 && !turn_state_result_valid(event->encounter,
        event->roster, event->initiative, event->tool_result)) return 0;
    if (event->type_id == 4 || event->type_id == 5) {
        size_t text_len = event->text_len != 0u ?
            event->text_len : strlen(event->text);
        size_t display_len = event->display_text == event->text ?
            text_len : strlen(event->display_text);
        size_t used = pb_encode_turn_text_event_prepared(
            out, out_cap, event->request_id, event->request_id_len,
            event->type_id, event->text, text_len, NULL, 0u,
            event->display_text, display_len, event->segment_index,
            event->is_final);
        if (event->provider_model) used = pb_append_turn_provider_model(out, out_cap, used, event->provider_model);
        if (event->provider_usage) {
            uint64_t counts[3];
            if (!pb_turn_provider_usage(event->provider_usage, counts)) return 0;
            used = pb_append_turn_provider_usage(out, out_cap, used, counts[0], counts[1], counts[2]);
        }
        if (event->tool_result) used = pb_append_turn_tool_result(out, out_cap, used, event->tool_result);
        if (event->encounter) used = pb_append_turn_encounter(out, out_cap, used, event->encounter);
        if (event->roster) used = pb_append_turn_roster(out, out_cap, used, event->roster);
        if (event->initiative) used = pb_append_turn_initiative(out, out_cap, used, event->initiative);
        return event->retrieval ? pb_append_turn_retrieval(out, out_cap, used, event->retrieval) : used;
    }
    if (event->type_id == 8)
        return pb_encode_turn_audio_event_stages(
            out, out_cap, event->request_id, event->type, "", event->audio,
            event->audio_len, event->sample_rate, event->channels, event->bit_depth,
            event->sequence, event->segment_index, event->is_final, event->stages);
    {
        size_t used = pb_encode_turn_event_stages(
            out, out_cap, event->request_id, event->type,
            event->type_id == 12 ? event->text : "", event->stages);
        return event->tool_result ?
            pb_append_turn_tool_result(out, out_cap, used, event->tool_result) : used;
    }
}
