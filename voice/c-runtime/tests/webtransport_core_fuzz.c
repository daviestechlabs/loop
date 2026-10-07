/* Coverage-guided parser harness for untrusted WebTransport input. */
#include "gateway_webtransport_core.h"
#include "turn_response.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int fuzz_safe_identifier(const char *value) {
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

static int fuzz_public_event_matches_bound(
    const uint8_t *wire,
    size_t wire_len,
    const turn_event_c *public_event,
    const turn_event_c *bound_event
) {
    turn_event_c expected;
    turn_event_c normalized;
    const uint8_t *display;
    if (!public_event || !bound_event) return 0;
    expected = *bound_event;
    memset(expected.text, 0, sizeof(expected.text));
    memset(expected.speech_text, 0, sizeof(expected.speech_text));
    normalized = *public_event;
    if (public_event->current_tts_stage_wire != NULL) {
        const uint8_t *stage_wire = public_event->current_tts_stage_wire;
        if (!wire || stage_wire < wire || stage_wire >= wire + wire_len)
            return 0;
        normalized.current_tts_stage_wire = NULL;
    }
    if (public_event->display_text_borrowed != 0u) {
        display = (const uint8_t *)public_event->display_text_view;
        if (public_event->display_text_borrowed != 1u || !wire || !display ||
            display < wire || display > wire + wire_len ||
            public_event->display_text_len >
                (size_t)((wire + wire_len) - display) ||
            public_event->display_text_len != bound_event->display_text_len ||
            memcmp(
                display, bound_event->display_text,
                public_event->display_text_len) != 0)
            return 0;
        memcpy(
            normalized.display_text, bound_event->display_text,
            sizeof(bound_event->display_text));
        normalized.display_text_borrowed = 0u;
    }
    return memcmp(&normalized, &expected, sizeof(expected)) == 0;
}

static int fuzz_turn_requests_equal(
    const gw_wt_turn_request *left,
    const gw_wt_turn_request *right
) {
    return strcmp(left->request_id, right->request_id) == 0 &&
        strcmp(left->session_id, right->session_id) == 0 &&
        strcmp(left->text, right->text) == 0 &&
        strcmp(left->identity_token, right->identity_token) == 0 &&
        left->enable_rag == right->enable_rag &&
        left->enable_tts == right->enable_tts &&
        left->audio_first == right->audio_first &&
        left->audio_stream == right->audio_stream &&
        memcmp(&left->metadata, &right->metadata, sizeof(left->metadata)) == 0 &&
        strcmp(left->meta_budget_ms, right->meta_budget_ms) == 0 &&
        strcmp(left->meta_deadline_unix_ms, right->meta_deadline_unix_ms) == 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    gw_wt_turn_request request;
    gw_wt_turn_request active_request;
    gw_wt_datagram datagram;
    gw_wt_audio_reorder reorder;
    turn_response_state response;
    turn_response_state bound_response;
    turn_response_state active_response;
    turn_response_state public_response;
    turn_event_c event;
    turn_event_c bound_event;
    turn_event_c active_event;
    turn_event_c public_event;
    turn_response_event safe_event;
    turn_response_event bound_safe_event;
    turn_response_event active_safe_event;
    turn_response_event public_safe_event;
    uint8_t public_wire[TURN_RESPONSE_MAX_AUDIO_EVENT + 8192u];
    uint8_t bound_public_wire[TURN_RESPONSE_MAX_AUDIO_EVENT + 8192u];
    uint8_t active_public_wire[TURN_RESPONSE_MAX_AUDIO_EVENT + 8192u];
    uint8_t edge_public_wire[TURN_RESPONSE_MAX_AUDIO_EVENT + 8192u];
    uint64_t value;
    size_t consumed;
    size_t subject_slot;
    char subject[GW_WT_EVENT_SUBJECT_LENGTH + 2u];
    char subject_nonce[GW_WT_EVENT_NONCE_HEX_LEN + 1u];

    {
        int checked_status = gw_wt_turn_request_parse(data, size, &request);
        int active_status;
        memset(&active_request, 0xa5, sizeof(active_request));
        active_status = gw_wt_turn_request_parse_active(
            data, size, &active_request);
        if (active_status != checked_status ||
            (active_status == GW_WT_OK &&
             !fuzz_turn_requests_equal(&request, &active_request)))
            abort();
    }
    (void)gw_wt_datagram_parse(data, size, &datagram);
    if (size) (void)gw_wt_stream_input_parse(data[0], data + 1u, size - 1u, &datagram);
    (void)gw_wt_quic_varint_decode(data, size, &value, &consumed);
    consumed = size < sizeof(subject) - 1u ? size : sizeof(subject) - 1u;
    memcpy(subject, data, consumed);
    subject[consumed] = '\0';
    (void)gw_wt_response_subject_slot(subject, &subject_slot);
    memset(subject_nonce, 0, sizeof(subject_nonce));
    consumed = size < sizeof(subject_nonce) ? size : sizeof(subject_nonce);
    memcpy(subject_nonce, data, consumed);
    (void)gw_wt_response_subject_write(
        subject, sizeof(subject), size == 0u ? 0u : (size_t)data[0],
        subject_nonce);
    memset(&response, 0, sizeof(response));
    response.wait_for_pcm = size != 0 && (data[0] & 1u) != 0;
    if (pb_decode_turn_event(data, size, &event) == 0) {
        size_t request_id_len = strlen(event.request_id);
        enum turn_response_action bound_action;
        enum turn_response_action active_action;
        enum turn_response_action public_action;
        enum turn_response_action action =
            turn_response_filter_decoded(&response, &event, &safe_event);
        memset(&bound_response, 0, sizeof(bound_response));
        bound_response.wait_for_pcm = size != 0 && (data[0] & 1u) != 0;
        active_response = bound_response;
        public_response = bound_response;
        memset(&bound_safe_event, 0, sizeof(bound_safe_event));
        memset(&active_safe_event, 0, sizeof(active_safe_event));
        memset(&public_safe_event, 0, sizeof(public_safe_event));
        memset(&bound_event, 0, sizeof(bound_event));
        memset(&active_event, 0, sizeof(active_event));
        memset(&public_event, 0, sizeof(public_event));
        if (pb_decode_turn_event_bound(
                data, size, event.request_id, request_id_len, &bound_event) != 0 ||
            bound_event.request_id[0] != '\0')
            abort();
        if (pb_decode_turn_event_active(
                data, size, event.request_id, request_id_len,
                &active_event) != 0 ||
            memcmp(&active_event, &bound_event, sizeof(bound_event)) != 0)
            abort();
        if (pb_decode_turn_event_public_active(
                data, size, event.request_id, request_id_len,
                &public_event) != 0)
            abort();
        if (!fuzz_public_event_matches_bound(
                data, size, &public_event, &bound_event))
            abort();
        if (fuzz_safe_identifier(event.request_id)) {
            bound_action = turn_response_filter_bound_n(
                &bound_response, &bound_event, event.request_id,
                request_id_len, &bound_safe_event);
            active_action = turn_response_filter_active_n(
                &active_response, &bound_event, event.request_id,
                request_id_len, &active_safe_event);
            public_action = turn_response_filter_public_active_n(
                &public_response, &public_event, event.request_id,
                request_id_len, &public_safe_event);
            if (bound_action != action ||
                active_action != bound_action ||
                public_action != bound_action ||
                memcmp(&bound_response, &response, sizeof(response)) != 0 ||
                memcmp(
                    &active_response, &bound_response,
                    sizeof(active_response)) != 0 ||
                memcmp(
                    &public_response, &bound_response,
                    sizeof(public_response)) != 0 ||
                memcmp(
                    &active_safe_event, &bound_safe_event,
                    sizeof(active_safe_event)) != 0)
                abort();
            if (action == TURN_RESPONSE_FORWARD ||
                action == TURN_RESPONSE_FORWARD_TERMINAL ||
                action == TURN_RESPONSE_FORWARD_AND_COMPLETE) {
                size_t public_len = turn_response_protobuf_encode(
                    public_wire, sizeof(public_wire), &safe_event);
                size_t bound_public_len = turn_response_protobuf_encode(
                    bound_public_wire, sizeof(bound_public_wire), &bound_safe_event);
                size_t active_public_len = turn_response_protobuf_encode(
                    active_public_wire, sizeof(active_public_wire), &active_safe_event);
                size_t edge_public_len = turn_response_protobuf_encode(
                    edge_public_wire, sizeof(edge_public_wire), &public_safe_event);
                if (bound_public_len != public_len ||
                    active_public_len != public_len ||
                    edge_public_len != public_len ||
                    memcmp(bound_public_wire, public_wire, public_len) != 0 ||
                    memcmp(active_public_wire, public_wire, public_len) != 0 ||
                    memcmp(edge_public_wire, public_wire, public_len) != 0)
                    abort();
            }
        }
    }
    {
        int bound_status;
        int public_status;
        memset(&bound_event, 0, sizeof(bound_event));
        memset(&public_event, 0, sizeof(public_event));
        bound_status = pb_decode_turn_event_bound(
            data, size, "fuzz-request", sizeof("fuzz-request") - 1u,
            &bound_event);
        public_status = pb_decode_turn_event_public_active(
            data, size, "fuzz-request", sizeof("fuzz-request") - 1u,
            &public_event);
        if ((bound_status == 0) != (public_status == 0)) abort();
        if (bound_status == 0) {
            if (!fuzz_public_event_matches_bound(
                    data, size, &public_event, &bound_event))
                abort();
            memset(&response, 0, sizeof(response));
            response.wait_for_pcm = size != 0 && (data[0] & 1u) != 0;
            if (turn_response_filter_bound_n(
                    &response, &bound_event, "fuzz-request",
                    sizeof("fuzz-request") - 1u, &safe_event) ==
                TURN_RESPONSE_FORWARD)
                (void)turn_response_protobuf_encode(
                    public_wire, sizeof(public_wire), &safe_event);
        }
    }
    memset(&reorder, 0, sizeof(reorder));
    if (size > 4u) {
        uint32_t sequence = ((uint32_t)data[0] << 24u) |
                            ((uint32_t)data[1] << 16u) |
                            ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
        size_t payload_len = size - 4u;
        const uint8_t *payload;
        if (payload_len > GW_WT_AUDIO_PAYLOAD_CAP)
            payload_len = GW_WT_AUDIO_PAYLOAD_CAP;
        payload_len &= ~(size_t)1u;
        (void)gw_wt_audio_reorder_insert(&reorder, sequence, data + 4u, payload_len);
        (void)gw_wt_audio_reorder_peek(&reorder, &payload, &consumed);
        (void)gw_wt_audio_reorder_pop(&reorder);
        (void)gw_wt_audio_reorder_commit_state(
            &reorder, reorder.next_sequence, (uint32_t)reorder.unique_audio_bytes);
    }
    return 0;
}
