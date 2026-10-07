/* Coverage-guided harness for untrusted TTS segment protobuf input. */
#include "pb_min.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int span_matches_owned(
    const char *view,
    size_t view_len,
    const char *owned,
    size_t owned_len
) {
    return view_len == owned_len &&
        (view_len == 0u ||
         (view && owned && memcmp(view, owned, view_len) == 0));
}

static int view_matches_owned(
    const turn_tts_segment_view_c *view,
    const turn_tts_segment_c *owned
) {
    return view && owned &&
        span_matches_owned(
            view->request_id, view->request_id_len,
            owned->request_id, owned->request_id_len) &&
        span_matches_owned(
            view->user_id, view->user_id_len,
            owned->user_id, owned->user_id_len) &&
        span_matches_owned(
            view->text, view->text_len, owned->text, owned->text_len) &&
        span_matches_owned(
            view->voice_id, view->voice_id_len,
            owned->voice_id, owned->voice_id_len) &&
        span_matches_owned(
            view->response_subject, view->response_subject_len,
            owned->response_subject, owned->response_subject_len) &&
        view->segment_index == owned->segment_index &&
        view->is_final == owned->is_final &&
        view->output_sample_rate == owned->output_sample_rate &&
        view->output_channels == owned->output_channels &&
        view->output_bit_depth == owned->output_bit_depth &&
        view->output_encoding == owned->output_encoding &&
        view->query_hash == owned->query_hash &&
        view->first_text_at_ms == owned->first_text_at_ms &&
        view->segment_emitted_at_ms == owned->segment_emitted_at_ms &&
        view->stream_finality_deferred == owned->stream_finality_deferred &&
        view->input_stages.audio_committed_at_ms ==
            owned->input_stages.audio_committed_at_ms &&
        view->input_stages.stt_request_received_at_ms ==
            owned->input_stages.stt_request_received_at_ms &&
        view->input_stages.stt_provider_request_started_at_ms ==
            owned->input_stages.stt_provider_request_started_at_ms &&
        view->input_stages.stt_provider_ready_at_ms ==
            owned->input_stages.stt_provider_ready_at_ms &&
        view->input_stages.stt_transcript_published_at_ms ==
            owned->input_stages.stt_transcript_published_at_ms;
}

static int admitted_request_hash(
    const char *request_id,
    size_t request_id_len,
    uint32_t *hash_out
) {
    uint32_t hash = 2166136261u;
    size_t i;
    if (!request_id || request_id_len == 0u || !hash_out) return 0;
    for (i = 0u; i < request_id_len; ++i) {
        uint8_t c = (uint8_t)request_id[i];
        if (!((c >= (uint8_t)'0' && c <= (uint8_t)'9') ||
              (c >= (uint8_t)'A' && c <= (uint8_t)'Z') ||
              (c >= (uint8_t)'a' && c <= (uint8_t)'z') ||
              c == (uint8_t)'-' || c == (uint8_t)'_' ||
              c == (uint8_t)'.' || c == (uint8_t)':')) return 0;
        hash ^= c;
        hash *= 16777619u;
    }
    *hash_out = hash;
    return 1;
}

static void fuzz_canonical_shape(const uint8_t *data, size_t size) {
    uint8_t wire[4096];
    uint8_t prepared[4096];
    turn_tts_segment_c input;
    turn_tts_segment_c owned;
    turn_tts_segment_view_c view;
    turn_tts_segment_view_c admitted;
    uint32_t expected_hash;
    uint32_t request_hash;
    size_t text_len = size < sizeof(input.text) - 1u ?
        size : sizeof(input.text) - 1u;
    size_t i;
    size_t prepared_len;
    size_t wire_len;
    memset(&input, 0, sizeof(input));
    memcpy(input.request_id, "fuzz-request", sizeof("fuzz-request"));
    for (i = 0u; i < text_len; ++i)
        input.text[i] = (char)('a' + data[i] % 26u);
    if (text_len == 0u) memcpy(input.text, "x", sizeof("x"));
    if (size != 0u) {
        input.segment_index = (int32_t)(data[0] & 0x7fu);
        input.is_final = (int)(data[0] & 1u);
    }
    wire_len = pb_encode_turn_tts_segment(wire, sizeof(wire), &input);
    if (wire_len == 0u ||
        pb_decode_turn_tts_segment_view(wire, wire_len, &view) != 0 ||
        pb_decode_turn_tts_segment_view_admitted(
            wire, wire_len, &admitted, &request_hash) != 0 ||
        pb_decode_turn_tts_segment(wire, wire_len, &owned) != 0 ||
        !view_matches_owned(&view, &owned) ||
        !view_matches_owned(&admitted, &owned) ||
        !admitted_request_hash(
            owned.request_id, owned.request_id_len, &expected_hash) ||
        request_hash != expected_hash) abort();
    prepared_len = pb_encode_turn_tts_segment_prepared(
        prepared, sizeof(prepared), &view);
    if (prepared_len != wire_len ||
        memcmp(prepared, wire, wire_len) != 0) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t canonical[8192];
    uint8_t prepared[8192];
    turn_tts_segment_view_c view;
    turn_tts_segment_view_c admitted;
    turn_tts_segment_view_c roundtrip_view;
    turn_tts_segment_c owned;
    uint32_t expected_hash = 0u;
    uint32_t request_hash = 0u;
    int view_status = pb_decode_turn_tts_segment_view(data, size, &view);
    int owned_status = pb_decode_turn_tts_segment(data, size, &owned);
    int admitted_status = pb_decode_turn_tts_segment_view_admitted(
        data, size, &admitted, &request_hash);
    if ((view_status == 0) != (owned_status == 0)) abort();
    if (view_status == 0) {
        size_t canonical_len;
        size_t prepared_len;
        int request_is_admitted = admitted_request_hash(
            owned.request_id, owned.request_id_len, &expected_hash);
        if (!view_matches_owned(&view, &owned)) abort();
        if ((admitted_status == 0) != request_is_admitted) abort();
        if (admitted_status == 0 &&
            (!view_matches_owned(&admitted, &owned) ||
             request_hash != expected_hash)) abort();
        canonical_len = pb_encode_turn_tts_segment(
            canonical, sizeof(canonical), &owned);
        prepared_len = pb_encode_turn_tts_segment_prepared(
            prepared, sizeof(prepared), &view);
        if (canonical_len == 0u || prepared_len != canonical_len ||
            memcmp(prepared, canonical, canonical_len) != 0 ||
            pb_decode_turn_tts_segment_view(
                prepared, prepared_len, &roundtrip_view) != 0 ||
            !view_matches_owned(&roundtrip_view, &owned)) abort();
    } else if (admitted_status == 0) {
        abort();
    }
    fuzz_canonical_shape(data, size);
    return 0;
}
