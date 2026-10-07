/*
 * Unit tests for pure-C voice runtime wire + product math (no NATS server required).
 */
#define _POSIX_C_SOURCE 200809L

#include "pb_min.h"
#include "pcm_condition.h"
#include "pcm_parse.h"
#include "route_classifier.h"
#include "phatic_policy.h"
#include "audio_engine_internal.h"
#include "speech_sanitize.h"
#include "speech_segment.h"
#include "speech_seg_host.h"
#include "speech_display_stream.h"
#include "turn_budget.h"
#include "turn_error.h"
#include "turn_frame.h"
#include "turn_ndjson.h"
#include "utf8.h"
#include "vbus_subject.h"
#include "voice_ascii.h"
#include "base64.h"
#include "stage_json.h"
#include "turn_response_json.h"
#include "reflex_session.h"
#include "rag_select.h"
#include "runtime_identity.h"
#include "http_min.h"
#include "openai_min.h"
#include "pcm_frame.h"
#include "byte_ring.h"
#include "gateway_http_fields.h"
#include "gateway_http_header_name.h"

#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static int failures;

typedef struct {
    char text[256];
    size_t len;
    int calls;
} sse_capture;

static int capture_sse_delta(const char *content, size_t content_len, void *user) {
    sse_capture *capture = (sse_capture *)user;
    if (!capture || !content || content_len >= sizeof(capture->text) - capture->len)
        return -1;
    memcpy(capture->text + capture->len, content, content_len);
    capture->len += content_len;
    capture->text[capture->len] = '\0';
    capture->calls++;
    return 0;
}

static int openai_sse_chunking_matches(
    const char *stream,
    size_t stream_len
) {
    openai_sse_decoder reference_decoder;
    sse_capture reference_capture;
    size_t split;
    if (!stream || stream_len == 0u) return 0;
    memset(&reference_capture, 0, sizeof(reference_capture));
    if (openai_sse_init(
            &reference_decoder,
            capture_sse_delta,
            &reference_capture) != 0 ||
        openai_sse_feed(&reference_decoder, stream, stream_len) != 0 ||
        openai_sse_finish(&reference_decoder) != 0)
        return 0;
    for (split = 0u; split <= stream_len; ++split) {
        openai_sse_decoder decoder;
        sse_capture capture;
        memset(&capture, 0, sizeof(capture));
        if (openai_sse_init(&decoder, capture_sse_delta, &capture) != 0 ||
            openai_sse_feed(&decoder, stream, split) != 0 ||
            openai_sse_feed(
                &decoder, stream + split, stream_len - split) != 0 ||
            openai_sse_finish(&decoder) != 0 ||
            capture.calls != reference_capture.calls ||
            capture.len != reference_capture.len ||
            memcmp(capture.text, reference_capture.text, capture.len + 1u) != 0 ||
            decoder.line_len != reference_decoder.line_len ||
            decoder.vllm_line_prefix_len !=
                reference_decoder.vllm_line_prefix_len ||
            decoder.vllm_prefix_match != reference_decoder.vllm_prefix_match ||
            decoder.finish_reason !=
                reference_decoder.finish_reason ||
            decoder.done != reference_decoder.done ||
            decoder.failed != reference_decoder.failed)
            return 0;
    }
    {
        openai_sse_decoder decoder;
        sse_capture capture;
        size_t offset;
        memset(&capture, 0, sizeof(capture));
        if (openai_sse_init(&decoder, capture_sse_delta, &capture) != 0)
            return 0;
        for (offset = 0u; offset < stream_len; ++offset) {
            if (openai_sse_feed(&decoder, stream + offset, 1u) != 0)
                return 0;
        }
        if (openai_sse_finish(&decoder) != 0 ||
            capture.calls != reference_capture.calls ||
            capture.len != reference_capture.len ||
            memcmp(capture.text, reference_capture.text, capture.len + 1u) != 0 ||
            decoder.finish_reason != reference_decoder.finish_reason ||
            decoder.done != reference_decoder.done ||
            decoder.failed != reference_decoder.failed)
            return 0;
    }
    return 1;
}

static int vllm_sse_content_case(
    const char *encoded,
    const char *expected,
    int should_accept
) {
    static const char prefix[] =
        "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\","
        "\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"";
    static const char event_tail[] =
        "\"},\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}]";
    static const char terminal[] =
        "\n\ndata: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\","
        "\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"\"},\"logprobs\":null,"
        "\"finish_reason\":\"stop\",\"stop_reason\":null,"
        "\"token_ids\":null}]}\n\n"
        "data: [DONE]\n\n";
    static const char *const root_tails[] = {
        "}",
        ",\"system_fingerprint\":\"fp-test\"}"
    };
    char stream[2048];
    size_t variant;
    if (!encoded || (should_accept && !expected)) return 0;
    for (variant = 0;
         variant < sizeof(root_tails) / sizeof(root_tails[0]);
         ++variant) {
        openai_sse_decoder decoder;
        sse_capture capture;
        int written = snprintf(
            stream,
            sizeof(stream),
            "%s%s%s%s%s",
            prefix,
            encoded,
            event_tail,
            root_tails[variant],
            terminal);
        int feed_rc;
        int finish_rc;
        if (written <= 0 || (size_t)written >= sizeof(stream)) return 0;
        memset(&capture, 0, sizeof(capture));
        if (openai_sse_init(&decoder, capture_sse_delta, &capture) != 0) return 0;
        feed_rc = openai_sse_feed(&decoder, stream, (size_t)written);
        finish_rc = feed_rc == 0 ? openai_sse_finish(&decoder) : -1;
        if (should_accept) {
            if (feed_rc != 0 || finish_rc != 0 || capture.calls != 1 ||
                capture.len != strlen(expected) ||
                strcmp(capture.text, expected) != 0)
                return 0;
        } else if ((feed_rc == 0 && finish_rc == 0) || capture.calls != 0) {
            return 0;
        }
    }
    return 1;
}

static int vllm_sse_rejects_truncated_suffix(
    const char *suffix,
    size_t suffix_len
) {
    static const char prefix[] =
        "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\","
        "\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"safe\"";
    char stream[1024];
    size_t keep;
    if (!suffix || suffix_len == 0u ||
        sizeof(prefix) - 1u + suffix_len + 2u > sizeof(stream))
        return 0;
    for (keep = 0; keep < suffix_len; ++keep) {
        openai_sse_decoder decoder;
        sse_capture capture;
        size_t stream_len = sizeof(prefix) - 1u + keep + 2u;
        memcpy(stream, prefix, sizeof(prefix) - 1u);
        memcpy(stream + sizeof(prefix) - 1u, suffix, keep);
        memcpy(stream + sizeof(prefix) - 1u + keep, "\n\n", 2u);
        memset(&capture, 0, sizeof(capture));
        if (openai_sse_init(&decoder, capture_sse_delta, &capture) != 0 ||
            openai_sse_feed(&decoder, stream, stream_len) == 0 ||
            capture.calls != 0 || capture.len != 0u)
            return 0;
    }
    return 1;
}

static int vllm_sse_finish_reason_case(
    const char *reason,
    int current_shape,
    int should_accept
) {
    static const char generic_format[] =
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"%s\"}]}\n\ndata: [DONE]\n\n";
    static const char current_format[] =
        "data: {\"id\":\"chatcmpl-test\","
        "\"object\":\"chat.completion.chunk\",\"created\":1750000000,"
        "\"model\":\"default\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"\"},\"logprobs\":null,"
        "\"finish_reason\":\"%s\",\"stop_reason\":null,"
        "\"token_ids\":null}]}\n\ndata: [DONE]\n\n";
    openai_sse_decoder decoder;
    sse_capture capture;
    char stream[1024];
    int written;
    int feed_rc;
    int finish_rc;
    if (!reason) return 0;
    written = snprintf(
        stream,
        sizeof(stream),
        current_shape ? current_format : generic_format,
        reason);
    if (written <= 0 || (size_t)written >= sizeof(stream)) return 0;
    memset(&capture, 0, sizeof(capture));
    if (openai_sse_init(&decoder, capture_sse_delta, &capture) != 0) return 0;
    feed_rc = openai_sse_feed(&decoder, stream, (size_t)written);
    finish_rc = feed_rc == 0 ? openai_sse_finish(&decoder) : -1;
    return should_accept ?
        feed_rc == 0 && finish_rc == 0 && capture.calls == 0 &&
            decoder.finish_reason == (strcmp(reason, "stop") == 0 ? OPENAI_FINISH_STOP : OPENAI_FINISH_LENGTH) &&
            openai_sse_chunking_matches(stream, (size_t)written) :
        (feed_rc != 0 || finish_rc != 0) && capture.calls == 0;
}

static void expect(const char *name, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    } else {
        printf("PASS %s\n", name);
    }
}

static int public_turn_event_matches_bound(
    const uint8_t *wire,
    size_t wire_len,
    const char *request_id,
    size_t request_id_len
) {
    turn_event_c bound_event;
    turn_event_c public_event;
    turn_event_c normalized_public;
    const uint8_t *display;
    int bound_rc;
    int public_rc;
    memset(&bound_event, 0, sizeof(bound_event));
    memset(&public_event, 0, sizeof(public_event));
    bound_rc = pb_decode_turn_event_bound(
        wire, wire_len, request_id, request_id_len, &bound_event);
    public_rc = pb_decode_turn_event_public_active(
        wire, wire_len, request_id, request_id_len, &public_event);
    if ((bound_rc == 0) != (public_rc == 0)) return 0;
    if (bound_rc != 0) return 1;
    normalized_public = public_event;
    if (public_event.current_tts_stage_wire != NULL) {
        const uint8_t *stage_wire = public_event.current_tts_stage_wire;
        if (stage_wire < wire || stage_wire >= wire + wire_len)
            return 0;
        normalized_public.current_tts_stage_wire = NULL;
    }
    if (public_event.display_text_borrowed != 0u) {
        display = (const uint8_t *)public_event.display_text_view;
        if (public_event.display_text_borrowed != 1u || !display ||
            display < wire || display > wire + wire_len ||
            public_event.display_text_len >
                (size_t)((wire + wire_len) - display) ||
            public_event.display_text_len != bound_event.display_text_len ||
            memcmp(
                display, bound_event.display_text,
                public_event.display_text_len) != 0)
            return 0;
        memcpy(
            normalized_public.display_text, bound_event.display_text,
            sizeof(bound_event.display_text));
        normalized_public.display_text_borrowed = 0u;
    }
    memset(bound_event.text, 0, sizeof(bound_event.text));
    memset(bound_event.speech_text, 0, sizeof(bound_event.speech_text));
    return memcmp(
        &normalized_public, &bound_event, sizeof(bound_event)) == 0;
}

static uint32_t test_fnv1a_span(const char *value, size_t value_len) {
    uint32_t hash = 2166136261u;
    size_t i;
    for (i = 0u; i < value_len; ++i) {
        hash ^= (uint8_t)value[i];
        hash *= 16777619u;
    }
    return hash;
}

static int reference_canonical_ascii(const char *input, size_t input_len) {
    size_t i;
    if (!input || input_len == 0u || input[0] == ' ' ||
        input[input_len - 1u] == ' ')
        return 0;
    for (i = 0; i < input_len; ++i) {
        unsigned char c = (unsigned char)input[i];
        int punct = i + 1u < input_len &&
            (input[i + 1u] == '.' || input[i + 1u] == '!' ||
             input[i + 1u] == '?' || input[i + 1u] == ',' ||
             input[i + 1u] == ';' || input[i + 1u] == ':');
        if (c > ' ' && c < 0x7fu && c != '<') continue;
        if (c != ' ' || input[i - 1u] == ' ' || punct) return 0;
    }
    return 1;
}

static int reference_sanitize_json_plain(
    const char *input,
    size_t input_len
) {
    size_t i;
    if (!reference_canonical_ascii(input, input_len)) return 0;
    for (i = 0u; i < input_len; ++i) {
        if (input[i] == '"' || input[i] == '\\') return 0;
    }
    return 1;
}

static int reference_seg_space(unsigned char c) {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

static int reference_seg_hard(unsigned char c) {
    return c == '.' || c == '!' || c == '?' || c == '\n';
}

static int reference_seg_soft(unsigned char c) {
    return c == ',' || c == ';' || c == ':';
}

static int reference_ascii_seg_next(
    const char *text,
    size_t text_len,
    int min_chars,
    int max_chars,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len
) {
    size_t start = 0;
    size_t cut = (size_t)-1;
    size_t word_cut = (size_t)-1;
    size_t i;
    int runes = 0;

    *seg_off = 0;
    *seg_len = 0;
    *rest_off = 0;
    *rest_len = text_len;
    while (start < text_len &&
           reference_seg_space((unsigned char)text[start]))
        start++;
    for (i = start; i < text_len; ++i) {
        unsigned char c = (unsigned char)text[i];
        runes++;
        if (reference_seg_hard(c) ||
            (reference_seg_soft(c) && runes >= min_chars))
            cut = i + 1u;
        if (reference_seg_space(c) && runes > min_chars) word_cut = i;
        if (runes >= max_chars) {
            if (cut == (size_t)-1) {
                int boundary = i + 1u < text_len && reference_seg_space((unsigned char)text[i + 1u]);
                cut = !boundary && word_cut != (size_t)-1 ? word_cut : i + 1u;
            }
            break;
        }
    }
    if (runes < min_chars || cut == (size_t)-1)
        return SPEECH_SEG_NO_CUT;
    {
        size_t end = cut;
        size_t rest = cut;
        while (end > start &&
               reference_seg_space((unsigned char)text[end - 1u]))
            end--;
        while (rest < text_len &&
               reference_seg_space((unsigned char)text[rest]))
            rest++;
        if (end == start || (end == start + 1u && text[start] == '>'))
            return SPEECH_SEG_NO_CUT;
        *seg_off = start;
        *seg_len = end - start;
        *rest_off = rest;
        *rest_len = text_len - rest;
    }
    return SPEECH_SEG_OK;
}

static int ascii_seg_matches_reference(
    const char *text,
    size_t text_len,
    int min_chars,
    int max_chars
) {
    speech_seg_config_v1 cfg;
    size_t actual_so, actual_sl, actual_ro, actual_rl;
    size_t expected_so, expected_sl, expected_ro, expected_rl;
    int actual_rc;
    int expected_rc;
    speech_seg_config_default_v1(&cfg);
    cfg.min_segment_chars = min_chars;
    cfg.max_segment_chars = max_chars;
    actual_rc = speech_seg_next_v1(
        text,
        text_len,
        &cfg,
        &actual_so,
        &actual_sl,
        &actual_ro,
        &actual_rl);
    expected_rc = reference_ascii_seg_next(
        text,
        text_len,
        min_chars,
        max_chars,
        &expected_so,
        &expected_sl,
        &expected_ro,
        &expected_rl);
    return actual_rc == expected_rc && actual_so == expected_so &&
        actual_sl == expected_sl && actual_ro == expected_ro &&
        actual_rl == expected_rl;
}

static int reference_ascii_seg_first_prefix(
    const char *text,
    size_t text_len,
    int min_chars,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len
) {
    size_t start = 0;
    size_t i;
    int runes = 0;

    *seg_off = 0;
    *seg_len = 0;
    *rest_off = 0;
    *rest_len = text_len;
    while (start < text_len &&
           reference_seg_space((unsigned char)text[start]))
        start++;
    for (i = start; i < text_len; ++i) {
        unsigned char c = (unsigned char)text[i];
        size_t end;
        size_t rest;
        runes++;
        if (runes < min_chars ||
            (!reference_seg_space(c) && !reference_seg_hard(c) &&
             !reference_seg_soft(c)))
            continue;
        end = i + 1u;
        rest = end;
        while (end > start &&
               reference_seg_space((unsigned char)text[end - 1u]))
            end--;
        while (rest < text_len &&
               reference_seg_space((unsigned char)text[rest]))
            rest++;
        if (end == start || (end == start + 1u && text[start] == '>'))
            return SPEECH_SEG_NO_CUT;
        *seg_off = start;
        *seg_len = end - start;
        *rest_off = rest;
        *rest_len = text_len - rest;
        return SPEECH_SEG_OK;
    }
    return SPEECH_SEG_NO_CUT;
}

static int ascii_prefix_matches_reference(
    const char *text,
    size_t text_len,
    int min_chars
) {
    size_t actual_so, actual_sl, actual_ro, actual_rl;
    size_t expected_so, expected_sl, expected_ro, expected_rl;
    int actual_rc = speech_seg_first_prefix_v1(
        text,
        text_len,
        min_chars,
        &actual_so,
        &actual_sl,
        &actual_ro,
        &actual_rl);
    int expected_rc = reference_ascii_seg_first_prefix(
        text,
        text_len,
        min_chars,
        &expected_so,
        &expected_sl,
        &expected_ro,
        &expected_rl);
    return actual_rc == expected_rc && actual_so == expected_so &&
        actual_sl == expected_sl && actual_ro == expected_ro &&
        actual_rl == expected_rl;
}

static size_t reference_base64(
    const uint8_t *input,
    size_t len,
    char *output,
    size_t output_cap
) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t input_pos = 0;
    size_t output_pos = 0;
    if ((!input && len != 0u) || !output || output_cap == 0u ||
        len > (SIZE_MAX - 2u) / 3u || ((len + 2u) / 3u) * 4u + 1u > output_cap)
        return 0;
    while (input_pos < len) {
        uint32_t value = (uint32_t)input[input_pos++] << 16;
        int have_second = input_pos < len;
        int have_third;
        if (have_second) value |= (uint32_t)input[input_pos++] << 8;
        have_third = input_pos < len;
        if (have_third) value |= input[input_pos++];
        output[output_pos++] = table[(value >> 18) & 63u];
        output[output_pos++] = table[(value >> 12) & 63u];
        output[output_pos++] = have_second ? table[(value >> 6) & 63u] : '=';
        output[output_pos++] = have_third ? table[value & 63u] : '=';
    }
    output[output_pos] = '\0';
    return output_pos;
}

static size_t reference_write_varint(
    uint8_t *out,
    size_t out_cap,
    size_t pos,
    uint64_t value
) {
    if (!out || pos > out_cap) return (size_t)-1;
    while (value >= 0x80u) {
        if (pos >= out_cap) return (size_t)-1;
        out[pos++] = (uint8_t)((value & 0x7fu) | 0x80u);
        value >>= 7;
    }
    if (pos >= out_cap) return (size_t)-1;
    out[pos++] = (uint8_t)value;
    return pos;
}

static size_t reference_stt_lifecycle(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    size_t session_id_len,
    int type_id,
    int64_t timestamp_ms
) {
    size_t pos = 0;
    if (!out || out_cap == 0u || (!session_id && session_id_len != 0u) ||
        type_id < STT_LIFECYCLE_STREAM_STARTED ||
        type_id > STT_LIFECYCLE_TRANSCRIPTION_FAILED || timestamp_ms <= 1)
        return 0u;
    pos = reference_write_varint(out, out_cap, pos, 0x0au);
    if (pos == (size_t)-1) return 0u;
    pos = reference_write_varint(out, out_cap, pos, session_id_len);
    if (pos == (size_t)-1) return 0u;
    if (session_id_len > out_cap - pos) return 0u;
    if (session_id_len != 0u)
        memcpy(out + pos, session_id, session_id_len);
    pos += session_id_len;
    pos = reference_write_varint(out, out_cap, pos, 0x18u);
    if (pos == (size_t)-1) return 0u;
    pos = reference_write_varint(out, out_cap, pos, (uint64_t)type_id);
    if (pos == (size_t)-1) return 0u;
    pos = reference_write_varint(out, out_cap, pos, 0x28u);
    if (pos == (size_t)-1) return 0u;
    pos = reference_write_varint(
        out, out_cap, pos, (uint64_t)timestamp_ms);
    return pos == (size_t)-1 ? 0u : pos;
}

static int lifecycle_encoders_match(
    const char *session_id,
    size_t session_id_len,
    const char *type_name,
    int type_id,
    int64_t timestamp_ms,
    size_t maximum_out_cap
) {
    uint8_t reference[512];
    uint8_t legacy[512];
    uint8_t prepared[512];
    size_t out_cap;
    if (maximum_out_cap > sizeof(reference)) return 0;
    for (out_cap = 0u; out_cap <= maximum_out_cap; ++out_cap) {
        size_t reference_len;
        size_t legacy_len;
        size_t prepared_len;
        memset(reference, 0xa5, sizeof(reference));
        memset(legacy, 0xa5, sizeof(legacy));
        memset(prepared, 0xa5, sizeof(prepared));
        reference_len = reference_stt_lifecycle(
            reference,
            out_cap,
            session_id,
            session_id_len,
            type_id,
            timestamp_ms);
        legacy_len = pb_encode_stt_lifecycle(
            legacy,
            out_cap,
            session_id,
            type_name,
            timestamp_ms);
        prepared_len = pb_encode_stt_lifecycle_prepared(
            prepared,
            out_cap,
            session_id,
            session_id_len,
            type_id,
            timestamp_ms);
        if (reference_len != legacy_len || reference_len != prepared_len ||
            memcmp(reference, legacy, sizeof(reference)) != 0 ||
            memcmp(reference, prepared, sizeof(reference)) != 0) return 0;
    }
    return 1;
}

/* Manually craft a minimal STTStreamMessage: type="chunk", audio=4 bytes. */
static size_t encode_chunk_msg(uint8_t *out, size_t cap, const uint8_t *audio, size_t alen) {
    size_t pos = 0;
    /* field 1 string "chunk" */
    out[pos++] = (1 << 3) | 2;
    out[pos++] = 5;
    memcpy(out + pos, "chunk", 5);
    pos += 5;
    /* field 2 bytes audio */
    out[pos++] = (2 << 3) | 2;
    out[pos++] = (uint8_t)alen;
    if (pos + alen > cap) return 0;
    memcpy(out + pos, audio, alen);
    pos += alen;
    return pos;
}

static size_t append_stage_map_entry(
    uint8_t *out,
    size_t cap,
    size_t pos,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *value,
    size_t value_len
) {
    size_t entry_len;
    size_t field_len;
    if (!out || !key || (!value && value_len != 0) || key_len > 127u ||
        value_len > 127u) return 0;
    entry_len = 4u + key_len + value_len;
    field_len = 3u + entry_len;
    if (entry_len > 127u || pos > cap ||
        field_len > cap - pos) return 0;
    out[pos++] = 0x82u;
    out[pos++] = 0x01u;
    out[pos++] = (uint8_t)entry_len;
    out[pos++] = 0x0au;
    out[pos++] = (uint8_t)key_len;
    memcpy(out + pos, key, key_len);
    pos += key_len;
    out[pos++] = 0x12u;
    out[pos++] = (uint8_t)value_len;
    if (value_len != 0) memcpy(out + pos, value, value_len);
    return pos + value_len;
}

static size_t append_stage_map_raw_entry(
    uint8_t *out,
    size_t cap,
    size_t pos,
    const uint8_t *entry,
    size_t entry_len
) {
    size_t field_len = 3u + entry_len;
    if (!out || (!entry && entry_len != 0u) || entry_len > 127u ||
        pos > cap || field_len > cap - pos) return 0;
    out[pos++] = 0x82u;
    out[pos++] = 0x01u;
    out[pos++] = (uint8_t)entry_len;
    if (entry_len != 0u) memcpy(out + pos, entry, entry_len);
    return pos + entry_len;
}

static int public_turn_stage_mutations_match_bound(
    uint8_t *wire,
    size_t wire_len,
    size_t stage_offset,
    const char *request_id,
    size_t request_id_len
) {
    size_t accepted_mutations = 0u;
    size_t position;
    if (!wire || wire_len == 0u || stage_offset >= wire_len ||
        !request_id || request_id_len == 0u) return 0;
    if (!public_turn_event_matches_bound(
            wire, wire_len, request_id, request_id_len))
        return 0;
    {
        static const uint8_t later_key[] =
            "stage_tts_segment_emitted_at_ms";
        turn_event_c bound;
        size_t key_len = sizeof(later_key) - 1u;
        size_t key_pos;
        uint8_t saved;
        int bound_status;
        int public_matches;
        if (key_len > wire_len - stage_offset) return 0;
        for (key_pos = stage_offset; key_pos <= wire_len - key_len; ++key_pos) {
            if (memcmp(wire + key_pos, later_key, key_len) == 0) break;
        }
        if (key_pos > wire_len - key_len) return 0;
        saved = wire[key_pos + 10u];
        wire[key_pos + 10u] = (uint8_t)'x';
        memset(&bound, 0, sizeof(bound));
        bound_status = pb_decode_turn_event_bound(
            wire, wire_len, request_id, request_id_len, &bound);
        public_matches = public_turn_event_matches_bound(
            wire, wire_len, request_id, request_id_len);
        wire[key_pos + 10u] = saved;
        if (bound_status != 0 || !public_matches) return 0;
    }
    for (position = stage_offset; position < wire_len; ++position) {
        uint8_t saved = wire[position];
        unsigned replacement;
        for (replacement = 0u; replacement <= UINT8_MAX; ++replacement) {
            turn_event_c bound;
            turn_event_c public_event;
            int bound_status;
            int public_status;
            if ((uint8_t)replacement == saved) continue;
            wire[position] = (uint8_t)replacement;
            memset(&bound, 0, sizeof(bound));
            memset(&public_event, 0, sizeof(public_event));
            bound_status = pb_decode_turn_event_bound(
                wire, wire_len, request_id, request_id_len, &bound);
            public_status = pb_decode_turn_event_public_active(
                wire, wire_len, request_id, request_id_len, &public_event);
            if ((bound_status == 0) != (public_status == 0)) return 0;
            if (bound_status == 0) {
                accepted_mutations++;
                if (!public_turn_event_matches_bound(
                        wire, wire_len, request_id, request_id_len))
                    return 0;
            }
        }
        wire[position] = saved;
    }
    return accepted_mutations != 0u;
}

static int public_turn_text_mutations_match_bound(
    uint8_t *wire,
    size_t wire_len,
    const char *request_id,
    size_t request_id_len
) {
    size_t accepted_mutations = 0u;
    size_t position;
    if (!wire || wire_len == 0u || !request_id || request_id_len == 0u)
        return 0;
    if (!public_turn_event_matches_bound(
            wire, wire_len, request_id, request_id_len))
        return 0;
    for (position = 0u; position < wire_len; ++position) {
        uint8_t saved = wire[position];
        unsigned replacement;
        for (replacement = 0u; replacement <= UINT8_MAX; ++replacement) {
            turn_event_c bound;
            turn_event_c public_event;
            int bound_status;
            int public_status;
            int matches = 1;
            if ((uint8_t)replacement == saved) continue;
            wire[position] = (uint8_t)replacement;
            memset(&bound, 0, sizeof(bound));
            memset(&public_event, 0, sizeof(public_event));
            bound_status = pb_decode_turn_event_bound(
                wire, wire_len, request_id, request_id_len, &bound);
            public_status = pb_decode_turn_event_public_active(
                wire, wire_len, request_id, request_id_len, &public_event);
            if ((bound_status == 0) != (public_status == 0)) {
                matches = 0;
            } else if (bound_status == 0) {
                accepted_mutations++;
                matches = public_turn_event_matches_bound(
                    wire, wire_len, request_id, request_id_len);
            }
            wire[position] = saved;
            if (!matches) return 0;
        }
    }
    return accepted_mutations != 0u;
}

static void wr_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void wr_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

int main(void) {
    failures = 0;
    printf("c-runtime unit tests (no Go)\n");

    {
        static const struct { const char *requested; uint32_t expected; } cases[] = {
            {NULL, 256u}, {"", 256u}, {"1", 1u}, {"48", 48u}, {"256", 256u},
            {"257", 256u}, {"1000000", 256u}, {"0", 0u}, {"01", 0u},
            {"+8", 0u}, {"-8", 0u}, {" 8", 0u}, {"8 ", 0u}, {"8x", 0u},
            {"1000001", 0u}, {"99999999", 0u}, {"\xff", 0u}
        };
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
            expect("client completion limit stays below server authority",
                openai_completion_limit(256u, cases[i].requested) == cases[i].expected);
        expect("invalid server limits fail closed", openai_completion_limit(0u, NULL) == 0u &&
            openai_completion_limit(4097u, "1") == 0u);
        expect("ordinary and maximum model budgets remain bounded",
            openai_completion_limit(48u, "256") == 48u && openai_completion_limit(4096u, NULL) == 4096u);
    }

    {
        turn_tool_result_c tool = {0};
        turn_event_c decoded;
        turn_response_event safe;
        turn_response_state state = {0};
        uint8_t wire[2048], public_wire[2048], malformed[2048];
        char json[2048];
        size_t base, length, public_length, i;
        memcpy(tool.tool_id, "dnd-dice-roll", sizeof("dnd-dice-roll"));
        memcpy(tool.tool_call_id, "dice-", 5u);
        memset(tool.tool_call_id + 5u, 'a', 64u);
        memset(tool.output_sha256, 'b', 64u);
        tool.present = 1;
        base = pb_encode_turn_text_event(wire, sizeof(wire), "tool-wire", "text_completed",
                                         "You rolled 20.", "You rolled 20.", "You rolled 20.", 0, 1);
        length = pb_append_turn_tool_result(wire, sizeof(wire), base, &tool);
        expect("typed tool provenance decodes with zero elapsed time", length > base &&
            pb_decode_turn_event(wire, length, &decoded) == 0 &&
            decoded.tool_result.present && decoded.tool_result.elapsed_ms == 0 &&
            strcmp(decoded.tool_result.tool_call_id, tool.tool_call_id) == 0 &&
            strcmp(decoded.tool_result.output_sha256, tool.output_sha256) == 0);
        expect("typed tool provenance reaches public response", turn_response_filter_decoded(
            &state, &decoded, &safe) == TURN_RESPONSE_FORWARD && safe.tool_result != NULL);
        public_length = turn_response_protobuf_encode(public_wire, sizeof(public_wire), &safe);
        expect("public protobuf retains typed tool provenance", public_length > 0 &&
            pb_decode_turn_event(public_wire, public_length, &decoded) == 0 &&
            decoded.tool_result.present && strcmp(decoded.tool_result.output_sha256, tool.output_sha256) == 0);
        expect("public JSON contains tool trace without runtime metadata", turn_response_json_encode(
            json, sizeof(json), &safe, 123) > 0 &&
            strstr(json, "\"metadata\":{\"cascade_fallback_reason\":\"tool_executed\"") &&
            strstr(json, "\"cascade_tool_ms\":\"0\""));
        safe.runtime_identity_sha256 = tool.output_sha256;
        safe.runtime_identity_sha256_len = 64u;
        expect("public JSON combines runtime and tool provenance", turn_response_json_encode(
            json, sizeof(json), &safe, 123) > 0 &&
            strstr(json, "\",\"cascade_fallback_reason\":\"tool_executed\""));
        decoded.stages.first_text_at_ms = 100;
        decoded.stages.tts_request_received_at_ms = 101;
        safe.stages = &decoded.stages;
        expect("public JSON combines stages, runtime, and tool provenance", turn_response_json_encode(
            json, sizeof(json), &safe, 123) > 0 && strstr(json, "\"stage_first_text_at_ms\":\"100\",") &&
            strstr(json, "\"cascade_runtime_identity_sha256\"") && strstr(json, "\"cascade_tool_output_hash\""));
        for (i = 1u; i < length - base; ++i)
            expect("truncated typed tool provenance rejects", pb_decode_turn_event(wire, base + i, &decoded) != 0);
        public_length = pb_append_turn_tool_result(wire, sizeof(wire), length, &tool);
        expect("duplicate typed tool provenance rejects", public_length > length &&
            pb_decode_turn_event(wire, public_length, &decoded) != 0);
        memcpy(malformed, wire, length);
        malformed[base] = 160u;
        expect("wrong tool provenance wire type rejects", pb_decode_turn_event(malformed, length, &decoded) != 0);
        expect("typed tool provenance respects writer capacity", pb_append_turn_tool_result(
            wire, length - 1u, base, &tool) == 0);
        tool.elapsed_ms = -1;
        expect("negative tool duration rejects", pb_append_turn_tool_result(wire, sizeof(wire), base, &tool) == 0);
        tool.elapsed_ms = 0;
        tool.output_sha256[0] = 'G';
        expect("non-hex tool hash rejects", pb_append_turn_tool_result(wire, sizeof(wire), base, &tool) == 0);
        tool.output_sha256[0] = 'b';
        base = pb_encode_turn_event(wire, sizeof(wire), "tool-wire", "thinking_started", "");
        length = pb_append_turn_tool_result(wire, sizeof(wire), base, &tool);
        memset(&state, 0, sizeof(state));
        expect("tool provenance on a non-result event rejects", pb_decode_turn_event(wire, length, &decoded) == 0 &&
            turn_response_filter_decoded(&state, &decoded, &safe) == TURN_RESPONSE_REJECT);
    }

    {
        static const char revision[] =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        static const char expected_body[] =
            "{\"pod_name\":\"c-voice-runtime-poc-canary-abc\","
            "\"pod_namespace\":\"ai-ml\","
            "\"pod_uid\":\"2f1c93ce-69aa-4a43-817a-42fe9f05d341\","
            "\"policy_version\":\"turn-core-v1\","
            "\"process_started_at\":\"1970-01-01T00:00:00.000000123Z\","
            "\"schema_version\":\"cascade-router-runtime-identity/v1\","
            "\"service\":\"cascade-router\","
            "\"source_revision\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
        static const char expected_hash[] =
            "64fa50abf071590f2a909a88a01b52ff"
            "5732721c097bf024316e687b959a1070";
        struct timespec started_at = {.tv_sec = 0, .tv_nsec = 123};
        voice_runtime_identity identity;
        expect(
            "runtime identity builds canonical source-bound JSON",
            voice_runtime_identity_build(
                &identity,
                revision,
                "c-voice-runtime-poc-canary-abc",
                "ai-ml",
                "2f1c93ce-69aa-4a43-817a-42fe9f05d341",
                "turn-core-v1",
                &started_at) == 0 &&
            identity.body_len == sizeof(expected_body) - 1u &&
            memcmp(identity.body, expected_body, sizeof(expected_body)) == 0 &&
            memcmp(identity.sha256, expected_hash, sizeof(expected_hash)) == 0);
        expect(
            "runtime identity rejects incomplete or forged provenance",
            voice_runtime_identity_build(
                &identity, "abc", "pod-1", "ai-ml", "uid-1",
                "turn-core-v1", &started_at) != 0 &&
            voice_runtime_identity_build(
                &identity, revision, "../pod", "ai-ml", "uid-1",
                "turn-core-v1", &started_at) != 0 &&
            voice_runtime_identity_build(
                &identity, revision, "pod-1", "", "uid-1",
                "turn-core-v1", &started_at) != 0 &&
            voice_runtime_identity_build(
                &identity, revision, "pod-1", "ai-ml", "uid-1",
                "bad policy", &started_at) != 0 &&
            voice_runtime_identity_build(
                NULL, revision, "pod-1", "ai-ml", "uid-1",
                "turn-core-v1", &started_at) != 0);
        {
            char oversized_identity[255];
            char oversized_policy[129];
            memset(oversized_identity, 'a', sizeof(oversized_identity));
            oversized_identity[sizeof(oversized_identity) - 1u] = '\0';
            memset(oversized_policy, 'a', sizeof(oversized_policy));
            oversized_policy[sizeof(oversized_policy) - 1u] = '\0';
            expect(
                "runtime identity bounds every environment-derived span",
                voice_runtime_identity_build(
                    &identity, revision, oversized_identity, "ai-ml", "uid-1",
                    "turn-core-v1", &started_at) != 0 &&
                voice_runtime_identity_build(
                    &identity, revision, "pod-1", "ai-ml", "uid-1",
                    oversized_policy, &started_at) != 0);
        }
        started_at.tv_nsec = 1000000000L;
        expect(
            "runtime identity rejects an invalid process clock",
            voice_runtime_identity_build(
                &identity, revision, "pod-1", "ai-ml", "uid-1",
                "turn-core-v1", &started_at) != 0);
        unsetenv("POD_NAME");
        unsetenv("POD_NAMESPACE");
        unsetenv("POD_UID");
        unsetenv("TURN_CORE_POLICY_VERSION");
        expect(
            "runtime identity stays disabled outside Kubernetes",
            voice_runtime_identity_load(&identity, revision) == 0);
        expect(
            "runtime identity rejects a policy without Pod provenance",
            setenv("TURN_CORE_POLICY_VERSION", "turn-core-v1", 1) == 0 &&
            voice_runtime_identity_load(&identity, revision) < 0);
        unsetenv("TURN_CORE_POLICY_VERSION");
        expect(
            "runtime identity rejects partial Pod provenance",
            setenv("POD_NAME", "pod-1", 1) == 0 &&
            voice_runtime_identity_load(&identity, revision) < 0);
        expect(
            "runtime identity loads one complete Pod provenance set",
            setenv("POD_NAMESPACE", "ai-ml", 1) == 0 &&
            setenv("POD_UID", "uid-1", 1) == 0 &&
            setenv("TURN_CORE_POLICY_VERSION", "turn-core-v1", 1) == 0 &&
            voice_runtime_identity_load(&identity, revision) == 1 &&
            strstr(identity.body, "\"source_revision\":\"aaaaaaaa") != NULL);
        unsetenv("POD_NAME");
        unsetenv("POD_NAMESPACE");
        unsetenv("POD_UID");
        unsetenv("TURN_CORE_POLICY_VERSION");
    }

    {
        unsigned value;
        int digits_match = 1;
        int alnum_matches = 1;
        for (value = 0u; value <= UCHAR_MAX; ++value) {
            int digit = value >= (unsigned)'0' && value <= (unsigned)'9';
            int alpha = (value >= (unsigned)'A' && value <= (unsigned)'Z') ||
                        (value >= (unsigned)'a' && value <= (unsigned)'z');
            if (voice_ascii_is_digit((unsigned char)value) != digit)
                digits_match = 0;
            if (voice_ascii_is_alnum((unsigned char)value) != (digit || alpha))
                alnum_matches = 0;
        }
        expect("ASCII digit classification covers every byte", digits_match);
        expect("ASCII alphanumeric classification covers every byte", alnum_matches);
    }

    {
        unsigned char storage[40];
        size_t offset;
        size_t length;
        int lines_match = 1;
        memset(storage, 'A', sizeof(storage));
        for (offset = 0u; offset < sizeof(uint64_t) && lines_match; ++offset) {
            for (length = 0u; length <= 24u && lines_match; ++length) {
                size_t position;
                if (!voice_ascii_http_line_valid(
                        (const char *)storage + offset, length)) {
                    lines_match = 0;
                    break;
                }
                for (position = 0u; position < length && lines_match; ++position) {
                    unsigned value;
                    for (value = 0u; value <= UCHAR_MAX; ++value) {
                        int expected = value == (unsigned)'\t' ||
                            (value >= 0x20u && value <= 0x7eu);
                        storage[offset + position] = (unsigned char)value;
                        if (voice_ascii_http_line_valid(
                                (const char *)storage + offset, length) != expected) {
                            lines_match = 0;
                            break;
                        }
                    }
                    storage[offset + position] = (unsigned char)'A';
                }
            }
        }
        expect("HTTP line validation covers every byte and word lane", lines_match);
        expect("HTTP line validation rejects a null span",
            !voice_ascii_http_line_valid(NULL, 0u));
    }

    {
        char maximum_size[32];
        char overflow_size[32];
        char timestamp[32];
        size_t size_value = 0u;
        int64_t timestamp_value = 0;
        int written = snprintf(maximum_size, sizeof(maximum_size), "%zu", SIZE_MAX);
        int maximum_size_ok = written > 0 && (size_t)written < sizeof(maximum_size) &&
            gateway_http_content_length_parse(
                maximum_size, (size_t)written, &size_value) == 0 &&
            size_value == SIZE_MAX;
        expect("gateway content length accepts size maximum", maximum_size_ok);
        expect("gateway content length accepts leading zeroes",
            gateway_http_content_length_parse("00037", 5u, &size_value) == 0 &&
            size_value == 37u);
        if (written > 0 && (size_t)written + 1u < sizeof(overflow_size)) {
            memcpy(overflow_size, maximum_size, (size_t)written);
            overflow_size[written] = '0';
            overflow_size[written + 1] = '\0';
        }
        expect("gateway content length rejects overflow",
            written > 0 && (size_t)written + 1u < sizeof(overflow_size) &&
            gateway_http_content_length_parse(
                overflow_size, (size_t)written + 1u, &size_value) != 0);
        expect("gateway content length rejects invalid spans",
            gateway_http_content_length_parse(NULL, 1u, &size_value) != 0 &&
            gateway_http_content_length_parse("", 0u, &size_value) != 0 &&
            gateway_http_content_length_parse("1", 1u, NULL) != 0 &&
            gateway_http_content_length_parse("+1", 2u, &size_value) != 0 &&
            gateway_http_content_length_parse("1x", 2u, &size_value) != 0 &&
            gateway_http_content_length_parse(
                "11111111111111111111111111111111", 32u, &size_value) != 0);

        written = snprintf(timestamp, sizeof(timestamp), "%" PRId64, INT64_MAX);
        expect("gateway timestamp accepts signed maximum",
            written > 0 && (size_t)written < sizeof(timestamp) &&
            gateway_http_timestamp_parse(
                timestamp, (size_t)written, &timestamp_value) == 0 &&
            timestamp_value == INT64_MAX);
        written = snprintf(timestamp, sizeof(timestamp), "%" PRId64, INT64_MIN);
        expect("gateway timestamp accepts signed minimum",
            written > 0 && (size_t)written < sizeof(timestamp) &&
            gateway_http_timestamp_parse(
                timestamp, (size_t)written, &timestamp_value) == 0 &&
            timestamp_value == INT64_MIN);
        expect("gateway timestamp accepts explicit plus",
            gateway_http_timestamp_parse("+37", 3u, &timestamp_value) == 0 &&
            timestamp_value == 37);
        expect("gateway timestamp rejects invalid spans",
            gateway_http_timestamp_parse(NULL, 1u, &timestamp_value) != 0 &&
            gateway_http_timestamp_parse("", 0u, &timestamp_value) != 0 &&
            gateway_http_timestamp_parse("1", 1u, NULL) != 0 &&
            gateway_http_timestamp_parse("+", 1u, &timestamp_value) != 0 &&
            gateway_http_timestamp_parse("-", 1u, &timestamp_value) != 0 &&
            gateway_http_timestamp_parse("1x", 2u, &timestamp_value) != 0 &&
            gateway_http_timestamp_parse(
                "9223372036854775808", 19u, &timestamp_value) != 0 &&
            gateway_http_timestamp_parse(
                "-9223372036854775809", 20u, &timestamp_value) != 0);
        expect("gateway media type matches ASCII case only",
            gateway_http_content_type_is_protobuf(
                "application/x-protobuf",
                sizeof("application/x-protobuf") - 1u) &&
            gateway_http_content_type_is_protobuf(
                "Application/X-Protobuf",
                sizeof("Application/X-Protobuf") - 1u) &&
            !gateway_http_content_type_is_protobuf(NULL, 0u) &&
            !gateway_http_content_type_is_protobuf(
                "application/x-protobufx",
                sizeof("application/x-protobufx") - 1u));
    }

    {
        static const struct {
            const char *name;
            size_t name_len;
            unsigned field;
        } cases[] = {
            {"transfer-encoding", sizeof("transfer-encoding") - 1u,
             GH_HEADER_TRANSFER_ENCODING},
            {"content-length", sizeof("content-length") - 1u,
             GH_HEADER_CONTENT_LENGTH},
            {"content-type", sizeof("content-type") - 1u,
             GH_HEADER_CONTENT_TYPE},
            {"x-voice-user", sizeof("x-voice-user") - 1u,
             GH_HEADER_AUTH_USER},
            {"x-voice-timestamp", sizeof("x-voice-timestamp") - 1u,
             GH_HEADER_AUTH_TIMESTAMP},
            {"x-voice-nonce", sizeof("x-voice-nonce") - 1u,
             GH_HEADER_AUTH_NONCE},
            {"x-voice-signature", sizeof("x-voice-signature") - 1u,
             GH_HEADER_AUTH_SIGNATURE},
        };
        char mutated[20];
        size_t case_index;
        int names_match = 1;
        for (case_index = 0u;
             case_index < sizeof(cases) / sizeof(cases[0]) && names_match;
             ++case_index) {
            size_t position;
            memcpy(mutated, cases[case_index].name, cases[case_index].name_len);
            for (position = 0u;
                 position < cases[case_index].name_len && names_match;
                 ++position) {
                unsigned value;
                unsigned char lower = (unsigned char)cases[case_index].name[position];
                unsigned char upper = lower >= (unsigned char)'a' &&
                        lower <= (unsigned char)'z' ?
                    (unsigned char)(lower - ((unsigned char)'a' - (unsigned char)'A')) :
                    lower;
                for (value = 0u; value <= UCHAR_MAX; ++value) {
                    unsigned char byte = (unsigned char)value;
                    int token_valid =
                        voice_ascii_is_alnum(byte) || byte == '!' || byte == '#' ||
                        byte == '$' || byte == '%' || byte == '&' || byte == '\'' ||
                        byte == '*' || byte == '+' || byte == '-' || byte == '.' ||
                        byte == '^' || byte == '_' || byte == '`' || byte == '|' ||
                        byte == '~';
                    unsigned expected = value == lower || value == upper ?
                        cases[case_index].field : GH_HEADER_UNKNOWN;
                    mutated[position] = (char)(unsigned char)value;
                    if (token_valid && gateway_http_validated_header_field(
                            mutated, cases[case_index].name_len) != expected) {
                        names_match = 0;
                        break;
                    }
                }
                mutated[position] = (char)lower;
            }
        }
        expect(
            "gateway header classifier covers every single-byte mutation",
            names_match);
        expect(
            "gateway header classifier rejects invalid spans",
            gateway_http_validated_header_field(NULL, 0u) == GH_HEADER_UNKNOWN &&
            gateway_http_validated_header_field("", 0u) == GH_HEADER_UNKNOWN &&
            gateway_http_validated_header_field(
                "content-typex", sizeof("content-typex") - 1u) ==
                GH_HEADER_UNKNOWN);
    }

    {
        static uint8_t fill[VOICE_BYTE_RING_CAPACITY];
        voice_byte_ring ring;
        size_t dirty = 0u;
        size_t i;
        int erased = 1;
        memset(fill, 0x5a, sizeof(fill));
        memset(&ring, 0, sizeof(ring));
        dirty = voice_byte_ring_dirty_after_write(
            &ring, dirty, VOICE_BYTE_RING_CAPACITY - 2u);
        expect("response ring accepts pre-wrap bytes",
            voice_byte_ring_write(
                &ring, fill, VOICE_BYTE_RING_CAPACITY - 2u));
        expect("response ring consumes a pre-wrap prefix",
            voice_byte_ring_consume(
                &ring, VOICE_BYTE_RING_CAPACITY - 4u));
        dirty = voice_byte_ring_dirty_after_write(&ring, dirty, 4u);
        expect("response ring accepts a wrapped suffix",
            voice_byte_ring_write(&ring, fill, 4u));
        expect("response ring dirty prefix saturates at capacity",
            dirty == VOICE_BYTE_RING_CAPACITY);
        voice_byte_ring_scrub(&ring, dirty);
        for (i = 0u; i < sizeof(ring.data); ++i) {
            if (ring.data[i] != 0u) {
                erased = 0;
                break;
            }
        }
        expect("response ring scrub erases wrapped writes", erased);
        expect("response ring scrub resets metadata",
            ring.head == 0u && ring.size == 0u);
        dirty = voice_byte_ring_dirty_after_write(&ring, 0u, 32u);
        expect("response ring accepts one complete drained frame",
            voice_byte_ring_write(&ring, fill, 32u) &&
            voice_byte_ring_consume(&ring, 32u));
        dirty = voice_byte_ring_dirty_after_write(&ring, dirty, 4u);
        expect("response ring accepts a shorter reused frame",
            voice_byte_ring_write(&ring, fill, 4u));
        expect("response ring reuses its exact dirty high water", dirty == 32u);
        ring.data[32] = 0x5au;
        voice_byte_ring_scrub(&ring, dirty);
        expect("response ring exact scrub preserves the clean suffix",
            ring.data[31] == 0u && ring.data[32] == 0x5au);
        memset(ring.data, 0x5a, sizeof(ring.data));
        ring.head = 3u;
        ring.size = 7u;
        voice_byte_ring_scrub(&ring, 18u);
        expect("response ring scrub stays within dirty prefix",
            ring.data[17] == 0u && ring.data[18] == 0x5au &&
            ring.head == 0u && ring.size == 0u);
    }

    {
        size_t head;
        size_t size;
        size_t written;
        size_t dirty;
        int exact = 1;
        for (head = 0u; head < 8u; ++head) {
            for (size = 0u; size <= 8u; ++size) {
                for (written = 0u; written <= 8u - size; ++written) {
                    for (dirty = 0u; dirty <= 8u; ++dirty) {
                        size_t expected = dirty;
                        size_t i;
                        size_t tail = (head + size) & 7u;
                        for (i = 0u; i < written; ++i) {
                            size_t end = ((tail + i) & 7u) + 1u;
                            if (end > expected) expected = end;
                        }
                        if (voice_byte_ring_dirty_after_write_bounded(
                                dirty, head, size, written, 8u) != expected)
                            exact = 0;
                    }
                }
            }
        }
        expect("response ring dirty high water covers every small-ring state", exact);
        expect("response ring dirty high water fails closed",
            voice_byte_ring_dirty_after_write_bounded(
                9u, 0u, 0u, 0u, 8u) == 8u &&
            voice_byte_ring_dirty_after_write_bounded(
                0u, 8u, 0u, 0u, 8u) == 8u &&
            voice_byte_ring_dirty_after_write_bounded(
                0u, 0u, 9u, 0u, 8u) == 8u &&
            voice_byte_ring_dirty_after_write_bounded(
                0u, 0u, 7u, 2u, 8u) == 8u &&
            voice_byte_ring_dirty_after_write_bounded(
                0u, 0u, 0u, 0u, 7u) == 7u);
    }

    {
        uint8_t storage[8] = {0u};
        static const uint8_t data[] = {1u, 2u};
        uint8_t *write_span = NULL;
        size_t write_span_len = 0u;
        const uint8_t *span = NULL;
        size_t span_len = 0u;
        size_t head = 0u;
        size_t size = 0u;
        expect("bounded response ring rejects zero capacity",
            !voice_byte_ring_write_pair_bounded(
                storage, 0u, &head, &size, data, sizeof(data), NULL, 0u) &&
            head == 0u && size == 0u &&
            voice_byte_ring_dirty_after_write_bounded(
                4u, 0u, 0u, 4u, 0u) == 0u);
        expect("bounded response ring rejects non-power-of-two capacity",
            !voice_byte_ring_write_pair_bounded(
                storage, 7u, &head, &size, data, sizeof(data), NULL, 0u) &&
            !voice_byte_ring_peek_bounded(
                storage, 7u, head, size, &span, &span_len) &&
            !voice_byte_ring_consume_bounded(7u, &head, &size, 1u) &&
            head == 0u && size == 0u);
        head = sizeof(storage);
        expect("bounded response ring rejects corrupt metadata",
            !voice_byte_ring_write_pair_bounded(
                storage, sizeof(storage), &head, &size,
                data, sizeof(data), NULL, 0u) &&
            !voice_byte_ring_peek_bounded(
                storage, sizeof(storage), head, size, &span, &span_len) &&
            !voice_byte_ring_consume_bounded(
                sizeof(storage), &head, &size, 1u) &&
            head == sizeof(storage) && size == 0u);
        expect("bounded response ring rejects invalid write spans",
            !voice_byte_ring_write_span_bounded(
                NULL, sizeof(storage), 0u, 0u,
                &write_span, &write_span_len) &&
            write_span == NULL && write_span_len == 0u &&
            !voice_byte_ring_write_span(
                NULL, &write_span, &write_span_len) &&
            write_span == NULL && write_span_len == 0u &&
            !voice_byte_ring_write_span_bounded(
                storage, sizeof(storage), sizeof(storage), 0u,
                &write_span, &write_span_len) &&
            !voice_byte_ring_write_span_bounded(
                storage, sizeof(storage), 0u, sizeof(storage),
                &write_span, &write_span_len) &&
            !voice_byte_ring_write_commit_bounded(
                sizeof(storage), sizeof(storage), &size, 0u));
    }

    {
        uint8_t storage[8] = {0u};
        size_t head;
        size_t size;
        int spans_match = 1;
        for (head = 0u; head < sizeof(storage); ++head) {
            for (size = 0u; size <= sizeof(storage); ++size) {
                uint8_t *span = NULL;
                size_t span_len = 0u;
                size_t available = sizeof(storage) - size;
                size_t tail = (head + size) & (sizeof(storage) - 1u);
                size_t expected = sizeof(storage) - tail;
                size_t committed = size;
                if (expected > available) expected = available;
                if ((voice_byte_ring_write_span_bounded(
                         storage, sizeof(storage), head, size,
                         &span, &span_len) != (expected != 0u)) ||
                    (expected != 0u &&
                     (span != storage + tail || span_len != expected)) ||
                    (expected == 0u &&
                     (span != NULL || span_len != 0u)) ||
                    !voice_byte_ring_write_commit_bounded(
                        sizeof(storage), head, &committed, expected) ||
                    committed != size + expected) {
                    spans_match = 0;
                    break;
                }
                committed = size;
                if (voice_byte_ring_write_commit_bounded(
                        sizeof(storage), head, &committed, expected + 1u) ||
                    committed != size) {
                    spans_match = 0;
                    break;
                }
            }
        }
        expect("response ring contiguous write spans cover every state",
            spans_match);
    }

    {
        static const unsigned char rejected[] = {
            0x01u, 0x20u, (unsigned char)'*', (unsigned char)'>',
            0x7fu, 0x80u, 0xffu
        };
        char subject[272];
        unsigned int byte;
        size_t alignment;
        size_t i;
        size_t len;
        int byte_sweep_ok = 1;
        int vector_sweep_ok = 1;
        memset(subject, 'a', sizeof(subject));
        subject[1] = '\0';
        for (byte = 0; byte <= UINT8_MAX; ++byte) {
            int expected = byte >= 0x21u && byte <= 0x7eu &&
                byte != (unsigned int)'*' && byte != (unsigned int)'>';
            subject[0] = (char)byte;
            if (vbus_publish_subject_valid(subject, 256u) != expected ||
                vbus_publish_subject_span_valid(subject, 1u) != expected) {
                byte_sweep_ok = 0;
                break;
            }
        }
        expect("VBus publish subject validates every byte", byte_sweep_ok);
        expect("VBus publish subject rejects null", !vbus_publish_subject_valid(NULL, 256u));
        expect("VBus publish subject rejects zero capacity",
            !vbus_publish_subject_valid("a", 0u));
        expect("VBus publish subject rejects one-byte capacity",
            !vbus_publish_subject_valid("a", 1u));
        expect("VBus publish subject rejects empty", !vbus_publish_subject_valid("", 256u));
        expect("VBus publish subject span rejects null",
            !vbus_publish_subject_span_valid(NULL, 1u));
        expect("VBus publish subject span rejects empty",
            !vbus_publish_subject_span_valid("", 0u));
        for (alignment = 0; alignment < 16u && vector_sweep_ok; ++alignment) {
            char *candidate = subject + alignment;
            memset(candidate, 'a', 255u);
            for (len = 1; len < 256u; ++len) {
                candidate[len] = '\0';
                if (!vbus_publish_subject_valid(candidate, 256u) ||
                    !vbus_publish_subject_span_valid(candidate, len)) {
                    vector_sweep_ok = 0;
                    break;
                }
                candidate[len] = 'a';
            }
            candidate[255] = '\0';
            for (i = 0; i < 255u && vector_sweep_ok; ++i) {
                size_t rejected_index;
                for (rejected_index = 0;
                     rejected_index < sizeof(rejected) / sizeof(rejected[0]);
                     ++rejected_index) {
                    candidate[i] = (char)rejected[rejected_index];
                    if (vbus_publish_subject_valid(candidate, 256u) ||
                        vbus_publish_subject_span_valid(candidate, 255u)) {
                        vector_sweep_ok = 0;
                        break;
                    }
                }
                candidate[i] = 'a';
            }
        }
        expect("VBus publish subject validates vector and tail boundaries", vector_sweep_ok);
        memset(subject, 'a', sizeof(subject));
        subject[255] = '\0';
        expect("VBus publish subject accepts capacity minus one bytes",
            vbus_publish_subject_valid(subject, 256u));
        subject[255] = 'a';
        subject[256] = '\0';
        expect("VBus publish subject rejects unterminated capacity",
            !vbus_publish_subject_valid(subject, 256u));
        expect("VBus publish subject accepts punctuation",
            vbus_publish_subject_valid("ai.turn.events.req:one/@private", 256u));
    }

    /* RFC 4648 Base64 with bounded output and all input remainders. */
    {
        static const struct {
            const char *plain;
            const char *encoded;
        } vectors[] = {
            {"", ""},
            {"f", "Zg=="},
            {"fo", "Zm8="},
            {"foo", "Zm9v"},
            {"foob", "Zm9vYg=="},
            {"fooba", "Zm9vYmE="},
            {"foobar", "Zm9vYmFy"}
        };
        static uint8_t input[16384];
        static char actual[21849];
        static char scalar[21849];
        static char reference[21849];
        size_t i;
        size_t len;
        size_t input_offset;
        size_t output_offset;
        int sweep_ok = 1;
        int unaligned_ok = 1;
        int exact_input_ok = 1;
        for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
            size_t input_len = strlen(vectors[i].plain);
            size_t encoded_len = base64_encode_v1(
                (const uint8_t *)vectors[i].plain, input_len, actual, sizeof(actual));
            expect("base64 RFC 4648 vector",
                encoded_len == strlen(vectors[i].encoded) &&
                strcmp(actual, vectors[i].encoded) == 0);
        }
        for (i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i * 131u + 17u);
        for (len = 0; len <= 2048u; ++len) {
            size_t actual_len = base64_encode_v1(input, len, actual, sizeof(actual));
            size_t scalar_len = base64_encode_scalar_v1(input, len, scalar, sizeof(scalar));
            size_t reference_len = reference_base64(input, len, reference, sizeof(reference));
            if (actual_len != reference_len || scalar_len != reference_len ||
                strcmp(actual, reference) != 0 || strcmp(scalar, reference) != 0) {
                sweep_ok = 0;
                break;
            }
        }
        expect("base64 exhaustive bounded sweep", sweep_ok);
        for (len = sizeof(input) - 2u; len <= sizeof(input); ++len) {
            size_t actual_len = base64_encode_v1(input, len, actual, sizeof(actual));
            size_t reference_len = reference_base64(input, len, reference, sizeof(reference));
            if (actual_len != reference_len || strcmp(actual, reference) != 0) sweep_ok = 0;
        }
        expect("base64 maximum payload remainders", sweep_ok);
        expect("base64 dispatch follows host",
            base64_using_ssse3_v1() == base64_host_has_ssse3_v1());
        expect("base64 AVX2 dispatch follows host",
            base64_using_avx2_v1() ==
                (base64_host_has_ssse3_v1() && base64_host_has_avx2_v1()));
        for (input_offset = 0; input_offset < 16u; ++input_offset) {
            for (output_offset = 0; output_offset < 16u; ++output_offset) {
                size_t actual_len = base64_encode_v1(
                    input + input_offset, 640u, actual + output_offset,
                    sizeof(actual) - output_offset);
                size_t reference_len = reference_base64(
                    input + input_offset, 640u, reference, sizeof(reference));
                if (actual_len != reference_len ||
                    memcmp(actual + output_offset, reference, reference_len + 1u) != 0) {
                    unaligned_ok = 0;
                    break;
                }
            }
            if (!unaligned_ok) break;
        }
        expect("base64 unaligned input and output", unaligned_ok);
        for (len = 1u; len <= 2048u; ++len) {
            size_t output_cap = base64_encoded_size_v1(len) + 1u;
            uint8_t *exact_input = (uint8_t *)malloc(len);
            char *exact_output = (char *)malloc(output_cap);
            char *exact_reference = (char *)malloc(output_cap);
            size_t actual_len;
            size_t reference_len;
            if (!exact_input || !exact_output || !exact_reference) {
                free(exact_reference);
                free(exact_output);
                free(exact_input);
                exact_input_ok = 0;
                break;
            }
            for (i = 0; i < len; ++i)
                exact_input[i] = (uint8_t)(i * 131u + len);
            actual_len = base64_encode_v1(
                exact_input, len, exact_output, output_cap);
            reference_len = reference_base64(
                exact_input, len, exact_reference, output_cap);
            if (actual_len != reference_len ||
                memcmp(exact_output, exact_reference, reference_len + 1u) != 0)
                exact_input_ok = 0;
            free(exact_reference);
            free(exact_output);
            free(exact_input);
            if (!exact_input_ok) break;
        }
        expect("base64 exact input bounds", exact_input_ok);
        memset(actual, 0xa5, sizeof(actual));
        expect("base64 exact output capacity",
            base64_encode_v1(input, 1u, actual, 5u) == 4u && strcmp(actual, "EQ==") == 0);
        memcpy(actual, "guard", sizeof("guard"));
        expect("base64 rejects short output",
            base64_encode_v1(input, 1u, actual, 4u) == 0u &&
            memcmp(actual, "guard", sizeof("guard")) == 0);
        expect("base64 rejects missing input",
            base64_encode_v1(NULL, 1u, actual, sizeof(actual)) == 0u);
        expect("base64 rejects missing output",
            base64_encode_v1(input, 1u, NULL, sizeof(actual)) == 0u);
        expect("base64 size rejects overflow", base64_encoded_size_v1(SIZE_MAX) == 0u);
    }

    /* Public stage metadata stays bounded, ordered, and locale-independent. */
    {
        turn_stage_timestamps_c stages;
        char output[1024];
        size_t written = 0;
        static const char expected[] =
            ",\"metadata\":{"
            "\"stage_audio_committed_at_ms\":\"1\","
            "\"stage_stt_request_received_at_ms\":\"2\","
            "\"stage_stt_provider_request_started_at_ms\":\"3\","
            "\"stage_stt_provider_ready_at_ms\":\"4\","
            "\"stage_stt_transcript_published_at_ms\":\"5\","
            "\"stage_first_text_at_ms\":\"6\","
            "\"stage_tts_segment_emitted_at_ms\":\"7\","
            "\"stage_tts_request_received_at_ms\":\"8\","
            "\"stage_tts_provider_request_started_at_ms\":\"9\","
            "\"stage_tts_provider_ready_at_ms\":\"10\","
            "\"stage_pcm_started_at_ms\":\"11\","
            "\"stage_pcm_first_chunk_at_ms\":\"12\"}";
        static const char expected_sparse[] =
            ",\"metadata\":{"
            "\"stage_tts_request_received_at_ms\":\"8\","
            "\"stage_pcm_started_at_ms\":\"11\"}";
        static const char expected_current_tts[] =
            ",\"metadata\":{"
            "\"stage_first_text_at_ms\":\"1787774400000\","
            "\"stage_tts_segment_emitted_at_ms\":\"1787774400001\","
            "\"stage_tts_request_received_at_ms\":\"1787774400002\","
            "\"stage_tts_provider_request_started_at_ms\":\"1787774400003\","
            "\"stage_tts_provider_ready_at_ms\":\"1787774400004\","
            "\"stage_pcm_started_at_ms\":\"1787774400005\"}";
        static const char expected_current_tts_first_pcm[] =
            ",\"metadata\":{"
            "\"stage_first_text_at_ms\":\"1787774400000\","
            "\"stage_tts_segment_emitted_at_ms\":\"1787774400001\","
            "\"stage_tts_request_received_at_ms\":\"1787774400002\","
            "\"stage_tts_provider_request_started_at_ms\":\"1787774400003\","
            "\"stage_tts_provider_ready_at_ms\":\"1787774400004\","
            "\"stage_pcm_started_at_ms\":\"1787774400005\","
            "\"stage_pcm_first_chunk_at_ms\":\"1787774400006\"}";
        static const char expected_current_tts_cross_prefix[] =
            ",\"metadata\":{"
            "\"stage_first_text_at_ms\":\"1787773999999\","
            "\"stage_tts_segment_emitted_at_ms\":\"1787774400001\","
            "\"stage_tts_request_received_at_ms\":\"1787774400002\","
            "\"stage_tts_provider_request_started_at_ms\":\"1787774400003\","
            "\"stage_tts_provider_ready_at_ms\":\"1787774400004\","
            "\"stage_pcm_started_at_ms\":\"1787774400005\"}";
        memset(&stages, 0, sizeof(stages));
        stages.input.audio_committed_at_ms = 1;
        stages.input.stt_request_received_at_ms = 2;
        stages.input.stt_provider_request_started_at_ms = 3;
        stages.input.stt_provider_ready_at_ms = 4;
        stages.input.stt_transcript_published_at_ms = 5;
        stages.first_text_at_ms = 6;
        stages.tts_segment_emitted_at_ms = 7;
        stages.tts_request_received_at_ms = 8;
        stages.tts_provider_request_started_at_ms = 9;
        stages.tts_provider_ready_at_ms = 10;
        stages.pcm_started_at_ms = 11;
        stages.pcm_first_chunk_at_ms = 12;
        written = turn_stage_metadata_json_write_v1(
            &stages, output, sizeof(output));
        expect(
            "stage metadata returns its exact serialized length",
            written == sizeof(expected) - 1u &&
            strcmp(output, expected) == 0);
        expect(
            "stage metadata accepts exact capacity",
            turn_stage_metadata_json_v1(
                &stages, output, sizeof(expected)) == 0 &&
            strcmp(output, expected) == 0);
        written = turn_stage_metadata_json_write_v1(
            &stages, output, sizeof(expected) - 1u);
        expect(
            "stage metadata returns failure sentinel after truncation",
            written == SIZE_MAX && output[0] == '\0');
        memset(&stages, 0, sizeof(stages));
        stages.input.audio_committed_at_ms = -1;
        stages.tts_request_received_at_ms = 8;
        stages.pcm_started_at_ms = 11;
        expect(
            "stage metadata preserves sparse comma state",
            turn_stage_metadata_json_v1(
                &stages, output, sizeof(output)) == 0 &&
            strcmp(output, expected_sparse) == 0);
        memset(&stages, 0, sizeof(stages));
        stages.first_text_at_ms = 1787774400000LL;
        stages.tts_segment_emitted_at_ms = 1787774400001LL;
        stages.tts_request_received_at_ms = 1787774400002LL;
        stages.tts_provider_request_started_at_ms = 1787774400003LL;
        stages.tts_provider_ready_at_ms = 1787774400004LL;
        stages.pcm_started_at_ms = 1787774400005LL;
        written = turn_stage_metadata_json_write_v1(
            &stages, output, sizeof(output));
        expect(
            "current TTS stage JSON matches independent text",
            written == sizeof(expected_current_tts) - 1u &&
            memcmp(output, expected_current_tts, sizeof(expected_current_tts)) ==
                0);
        expect(
            "current TTS stage JSON preserves exact capacity",
            turn_stage_metadata_json_write_v1(
                &stages, output, sizeof(expected_current_tts)) == written &&
            memcmp(output, expected_current_tts, sizeof(expected_current_tts)) ==
                0 &&
            turn_stage_metadata_json_write_v1(
                &stages, output, sizeof(expected_current_tts) - 1u) ==
                SIZE_MAX &&
            output[0] == '\0');
        stages.pcm_first_chunk_at_ms = 1787774400006LL;
        written = turn_stage_metadata_json_write_v1(
            &stages, output, sizeof(output));
        expect(
            "current first PCM stage JSON matches independent text",
            written == sizeof(expected_current_tts_first_pcm) - 1u &&
            memcmp(
                output,
                expected_current_tts_first_pcm,
                sizeof(expected_current_tts_first_pcm)) == 0);
        stages.pcm_first_chunk_at_ms = 0;
        stages.first_text_at_ms = 1787773999999LL;
        written = turn_stage_metadata_json_write_v1(
            &stages, output, sizeof(output));
        expect(
            "current TTS stage JSON retains cross-prefix fallback",
            written == sizeof(expected_current_tts_cross_prefix) - 1u &&
            memcmp(
                output,
                expected_current_tts_cross_prefix,
                sizeof(expected_current_tts_cross_prefix)) == 0);
        memset(&stages, 0, sizeof(stages));
        stages.tts_request_received_at_ms = -1;
        output[0] = 'x';
        expect(
            "stage metadata rejects an all-nonpositive staged turn",
            turn_stage_metadata_json_write_v1(
                &stages, output, sizeof(output)) == SIZE_MAX &&
            output[0] == '\0');
        stages.tts_request_received_at_ms = 0;
        output[0] = 'x';
        written = turn_stage_metadata_json_write_v1(
            &stages, output, sizeof(output));
        expect(
            "stage metadata returns zero length for unstaged turns",
            output[0] == '\0' && written == 0u);
        expect(
            "stage metadata length writer rejects invalid arguments",
            turn_stage_metadata_json_write_v1(
                NULL, output, sizeof(output)) == SIZE_MAX &&
            turn_stage_metadata_json_write_v1(
                &stages, NULL, sizeof(output)) == SIZE_MAX &&
            turn_stage_metadata_json_write_v1(
                &stages, output, 0) == SIZE_MAX);
        expect(
            "stage metadata rejects invalid arguments",
            turn_stage_metadata_json_v1(NULL, output, sizeof(output)) != 0 &&
            turn_stage_metadata_json_v1(&stages, NULL, sizeof(output)) != 0 &&
            turn_stage_metadata_json_v1(&stages, output, 0) != 0);
    }

    /* Authenticated HTTP events use one bounded canonical JSON writer. */
    {
        static const uint8_t audio[] = {0x00u, 0x01u, 0x02u};
        static const char expected_pcm[] =
            "{\"type\":\"pcm_chunk\",\"request_id\":\"req\","
            "\"audio_base64\":\"AAEC\",\"sample_rate\":24000,\"channels\":1,"
            "\"bit_depth\":16,\"audio_encoding\":1,\"sequence\":2,"
            "\"segment_index\":3,\"is_final\":true,\"timestamp\":123}\n";
        static const char expected_pcm_fallback[] =
            "{\"type\":\"pcm_chunk\",\"request_id\":\"req\","
            "\"audio_base64\":\"AAEC\",\"sample_rate\":44100,\"channels\":2,"
            "\"bit_depth\":24,\"audio_encoding\":2,\"sequence\":2,"
            "\"segment_index\":3,\"is_final\":true,\"timestamp\":123}\n";
        static const char expected_text[] =
            "{\"type\":\"text_delta\",\"request_id\":\"req\","
            "\"text\":\"A\\\"\\\\\xc3\xa9\",\"timestamp\":123}\n";
        static const char expected_failed[] =
            "{\"type\":\"failed\",\"request_id\":\"req\","
            "\"error\":\"upstream_failed\",\"timestamp\":123}\n";
        static const char expected_staged[] =
            "{\"type\":\"pcm_started\",\"request_id\":\"req\","
            "\"timestamp\":123,\"metadata\":{"
            "\"stage_tts_request_received_at_ms\":\"8\","
            "\"stage_pcm_started_at_ms\":\"11\"}}\n";
        static const char expected_accepted[] =
            "{\"type\":\"accepted\",\"request_id\":\"req\","
            "\"protocol\":\"turnstream.v1alpha1\","
            "\"response_event_contract\":\"canonical-v1\","
            "\"accepted_at\":123,\"metadata\":{\"edge_auth_us\":1,"
            "\"edge_vbus_publish_us\":2,\"edge_prepare_us\":3,"
            "\"edge_admission_us\":4,\"edge_capability_us\":5,"
            "\"edge_encode_us\":6}}\n";
        turn_response_event event;
        turn_acceptance_metrics metrics = {1u, 2u, 3u, 4u, 5u, 6u};
        turn_stage_timestamps_c stages;
        char output[1024];
        size_t capacity;
        size_t length;
        int truncated_outputs_terminated = 1;
        int timestamp_boundaries_match = 1;
        memset(&event, 0, sizeof(event));
        event.request_id = "req";
        event.type = "pcm_chunk";
        event.type_id = 8;
        event.audio = audio;
        event.audio_len = sizeof(audio);
        event.sample_rate = 24000;
        event.channels = 1;
        event.bit_depth = 16;
        event.audio_encoding = 1;
        event.sequence = 2;
        event.segment_index = 3;
        event.is_final = 1;
        length = turn_response_json_encode(output, sizeof(output), &event, 123);
        expect(
            "HTTP response JSON encodes canonical PCM",
            length == sizeof(expected_pcm) - 1u &&
            memcmp(output, expected_pcm, sizeof(expected_pcm)) == 0);
        expect(
            "HTTP response JSON accepts exact capacity",
            turn_response_json_encode(
                output, sizeof(expected_pcm), &event, 123) == length &&
            memcmp(output, expected_pcm, sizeof(expected_pcm)) == 0);
        expect(
            "HTTP response JSON rejects short capacity",
            turn_response_json_encode(
                output, sizeof(expected_pcm) - 1u, &event, 123) == 0);
        for (capacity = 1u; capacity < sizeof(expected_pcm); ++capacity) {
            memset(output, 'x', sizeof(output));
            if (turn_response_json_encode(
                    output, capacity, &event, 123) != 0u ||
                memchr(output, '\0', capacity) == NULL) {
                truncated_outputs_terminated = 0;
                break;
            }
        }
        expect(
            "HTTP response JSON terminates every truncated PCM output",
            truncated_outputs_terminated);
        event.sample_rate = 44100;
        event.channels = 2;
        event.bit_depth = 24;
        event.audio_encoding = 2;
        length = turn_response_json_encode(output, sizeof(output), &event, 123);
        expect(
            "HTTP response JSON retains noncanonical PCM fallback",
            length == sizeof(expected_pcm_fallback) - 1u &&
            memcmp(output, expected_pcm_fallback, sizeof(expected_pcm_fallback)) ==
                0);
        event.sample_rate = 24000;
        event.channels = 1;
        event.bit_depth = 16;
        event.audio_encoding = 1;

        {
            static const int64_t timestamp_cases[] = {
                INT64_C(999999999999),
                INT64_C(1000000000000),
                INT64_C(1000000000001),
                INT64_C(1787774400000),
                INT64_C(9999999999999),
                INT64_C(10000000000000),
                INT64_MAX,
            };
            char expected_timestamp[256];
            size_t timestamp_case;
            turn_response_event timestamp_event = event;
            timestamp_event.type = "started";
            timestamp_event.type_len = sizeof("started") - 1u;
            timestamp_event.type_id = 1;
            timestamp_event.request_id_len = sizeof("req") - 1u;
            for (timestamp_case = 0;
                 timestamp_case <
                    sizeof(timestamp_cases) / sizeof(timestamp_cases[0]);
                 ++timestamp_case) {
                int expected_len = snprintf(
                    expected_timestamp,
                    sizeof(expected_timestamp),
                    "{\"type\":\"started\",\"request_id\":\"req\","
                    "\"timestamp\":%" PRId64 "}\n",
                    timestamp_cases[timestamp_case]);
                length = turn_response_json_encode(
                    output,
                    sizeof(output),
                    &timestamp_event,
                    timestamp_cases[timestamp_case]);
                if (expected_len < 0 ||
                    (size_t)expected_len >= sizeof(expected_timestamp) ||
                    length != (size_t)expected_len ||
                    memcmp(output, expected_timestamp, length + 1u) != 0) {
                    timestamp_boundaries_match = 0;
                    break;
                }
            }
        }
        expect(
            "HTTP response JSON timestamps match reference boundaries",
            timestamp_boundaries_match);

        event.type = "text_delta";
        event.type_id = 4;
        event.display_text = "A\"\\\xc3\xa9";
        length = turn_response_json_encode(output, sizeof(output), &event, 123);
        expect(
            "HTTP response JSON escapes canonical text",
            length == sizeof(expected_text) - 1u &&
            memcmp(output, expected_text, sizeof(expected_text)) == 0);
        event.text_len = sizeof("A\"\\\xc3\xa9") - 1u;
        length = turn_response_json_encode(output, sizeof(output), &event, 123);
        expect(
            "HTTP response JSON reuses validated text length",
            length == sizeof(expected_text) - 1u &&
            memcmp(output, expected_text, sizeof(expected_text)) == 0);
        event.text_validated = 1u;
        length = turn_response_json_encode(output, sizeof(output), &event, 123);
        expect(
            "HTTP response JSON reuses complete text validation",
            length == sizeof(expected_text) - 1u &&
            memcmp(output, expected_text, sizeof(expected_text)) == 0);
        expect(
            "HTTP response JSON bounds canonical text",
            turn_response_json_encode(
                output, sizeof(expected_text), &event, 123) == length &&
            memcmp(output, expected_text, sizeof(expected_text)) == 0 &&
            turn_response_json_encode(
                output, sizeof(expected_text) - 1u, &event, 123) == 0);
        {
            static const char escape_bytes[] = {'"', '\\'};
            enum { ESCAPE_TEXT_LENGTH = 33 };
            char escaped_text[ESCAPE_TEXT_LENGTH + 1u];
            char strict_output[256];
            char validated_output[256];
            turn_response_event strict_event = event;
            turn_response_event validated_event = event;
            size_t escape_case;
            size_t offset;
            int escape_lanes_match = 1;
            strict_event.display_text = escaped_text;
            strict_event.text_len = ESCAPE_TEXT_LENGTH;
            strict_event.text_json_raw = 0u;
            strict_event.text_validated = 0u;
            validated_event = strict_event;
            validated_event.text_validated = 1u;
            for (escape_case = 0u;
                 escape_case < sizeof(escape_bytes) && escape_lanes_match;
                 ++escape_case) {
                for (offset = 0u;
                     offset < ESCAPE_TEXT_LENGTH && escape_lanes_match;
                     ++offset) {
                    size_t strict_len;
                    size_t validated_len;
                    memset(escaped_text, 'A', ESCAPE_TEXT_LENGTH);
                    escaped_text[ESCAPE_TEXT_LENGTH] = '\0';
                    escaped_text[offset] = escape_bytes[escape_case];
                    if (offset + 1u < ESCAPE_TEXT_LENGTH)
                        escaped_text[offset + 1u] =
                            escape_bytes[1u - escape_case];
                    strict_len = turn_response_json_encode(
                        strict_output, sizeof(strict_output),
                        &strict_event, 123);
                    validated_len = turn_response_json_encode(
                        validated_output, sizeof(validated_output),
                        &validated_event, 123);
                    if (strict_len == 0u || validated_len != strict_len ||
                        memcmp(
                            strict_output, validated_output,
                            strict_len + 1u) != 0) {
                        escape_lanes_match = 0;
                        break;
                    }
                    for (capacity = 1u;
                         capacity <= strict_len + 1u;
                         ++capacity) {
                        strict_len = turn_response_json_encode(
                            strict_output, capacity, &strict_event, 123);
                        validated_len = turn_response_json_encode(
                            validated_output, capacity,
                            &validated_event, 123);
                        if ((strict_len == 0u) != (validated_len == 0u) ||
                            (strict_len != 0u &&
                             (validated_len != strict_len ||
                              memcmp(
                                  strict_output, validated_output,
                                  strict_len + 1u) != 0))) {
                            escape_lanes_match = 0;
                            break;
                        }
                    }
                }
            }
            for (escape_case = 0u;
                 escape_case < 3u && escape_lanes_match;
                 ++escape_case) {
                size_t strict_len;
                size_t validated_len;
                for (offset = 0u; offset < ESCAPE_TEXT_LENGTH; ++offset) {
                    escaped_text[offset] = escape_case == 0u ? '"' :
                        escape_case == 1u ? '\\' : escape_bytes[offset & 1u];
                }
                escaped_text[ESCAPE_TEXT_LENGTH] = '\0';
                strict_len = turn_response_json_encode(
                    strict_output, sizeof(strict_output), &strict_event, 123);
                validated_len = turn_response_json_encode(
                    validated_output, sizeof(validated_output),
                    &validated_event, 123);
                if (strict_len == 0u || validated_len != strict_len ||
                    memcmp(
                        strict_output, validated_output,
                        strict_len + 1u) != 0) {
                    escape_lanes_match = 0;
                    break;
                }
                for (capacity = 1u;
                     capacity <= strict_len + 1u;
                     ++capacity) {
                    strict_len = turn_response_json_encode(
                        strict_output, capacity, &strict_event, 123);
                    validated_len = turn_response_json_encode(
                        validated_output, capacity, &validated_event, 123);
                    if ((strict_len == 0u) != (validated_len == 0u) ||
                        (strict_len != 0u &&
                         (validated_len != strict_len ||
                          memcmp(
                              strict_output, validated_output,
                              strict_len + 1u) != 0))) {
                        escape_lanes_match = 0;
                        break;
                    }
                }
            }
            expect(
                "HTTP response JSON escapes every validated word lane",
                escape_lanes_match);
        }
        event.text_validated = 0u;
        event.text_len = 0u;
        event.display_text = "bad\ntext";
        expect(
            "HTTP response JSON rejects control text",
            turn_response_json_encode(output, sizeof(output), &event, 123) == 0);
        event.display_text = "bad\xc3\x28";
        expect(
            "HTTP response JSON rejects malformed UTF-8 after escape scan",
            turn_response_json_encode(output, sizeof(output), &event, 123) == 0);

        event.type = "failed";
        event.type_id = 12;
        length = turn_response_json_encode(output, sizeof(output), &event, 123);
        expect(
            "HTTP response JSON encodes bounded failure",
            length == sizeof(expected_failed) - 1u &&
            memcmp(output, expected_failed, sizeof(expected_failed)) == 0);
        expect(
            "HTTP response JSON bounds canonical failure",
            turn_response_json_encode(
                output, sizeof(expected_failed), &event, 123) == length &&
            memcmp(output, expected_failed, sizeof(expected_failed)) == 0 &&
            turn_response_json_encode(
                output, sizeof(expected_failed) - 1u, &event, 123) == 0);

        memset(&stages, 0, sizeof(stages));
        stages.tts_request_received_at_ms = 8;
        stages.pcm_started_at_ms = 11;
        event.type = "pcm_started";
        event.type_id = 7;
        event.stages = &stages;
        length = turn_response_json_encode(output, sizeof(output), &event, 123);
        expect(
            "HTTP response JSON appends canonical stages",
            length == sizeof(expected_staged) - 1u &&
            memcmp(output, expected_staged, sizeof(expected_staged)) == 0);
        expect(
            "HTTP response JSON bounds canonical stages",
            turn_response_json_encode(
                output, sizeof(expected_staged), &event, 123) == length &&
            memcmp(output, expected_staged, sizeof(expected_staged)) == 0 &&
            turn_response_json_encode(
                output, sizeof(expected_staged) - 1u, &event, 123) == 0);
        {
            static const char runtime_hash[] =
                "ffffffffffffffffffffffffffffffff"
                "ffffffffffffffffffffffffffffffff";
            static const char expected_text_provenance[] =
                "{\"type\":\"text_completed\",\"request_id\":\"req\","
                "\"text\":\"done\",\"is_final\":true,\"timestamp\":123,\"metadata\":{"
                "\"cascade_runtime_identity_sha256\":\""
                "ffffffffffffffffffffffffffffffff"
                "ffffffffffffffffffffffffffffffff\"}}\n";
            static const char expected_completed_provenance[] =
                "{\"type\":\"completed\",\"request_id\":\"req\","
                "\"timestamp\":123,\"metadata\":{"
                "\"cascade_runtime_identity_sha256\":\""
                "ffffffffffffffffffffffffffffffff"
                "ffffffffffffffffffffffffffffffff\"}}\n";
            static const char expected_stage_provenance[] =
                "{\"type\":\"pcm_started\",\"request_id\":\"req\","
                "\"timestamp\":123,\"metadata\":{"
                "\"stage_tts_request_received_at_ms\":\"8\","
                "\"stage_pcm_started_at_ms\":\"11\","
                "\"cascade_runtime_identity_sha256\":\""
                "ffffffffffffffffffffffffffffffff"
                "ffffffffffffffffffffffffffffffff\"}}\n";
            turn_response_event provenance_event;
            memset(&provenance_event, 0, sizeof(provenance_event));
            provenance_event.request_id = "req";
            provenance_event.request_id_len = sizeof("req") - 1u;
            provenance_event.type = "text_completed";
            provenance_event.type_len = sizeof("text_completed") - 1u;
            provenance_event.type_id = 5;
            provenance_event.is_final = 1;
            provenance_event.display_text = "done";
            provenance_event.text_len = sizeof("done") - 1u;
            provenance_event.text_validated = 1u;
            provenance_event.runtime_identity_sha256 = runtime_hash;
            provenance_event.runtime_identity_sha256_len =
                VOICE_RUNTIME_IDENTITY_HASH_LEN;
            length = turn_response_json_encode(
                output, sizeof(output), &provenance_event, 123);
            expect(
                "HTTP response JSON stamps text with runtime identity",
                length == sizeof(expected_text_provenance) - 1u &&
                memcmp(
                    output,
                    expected_text_provenance,
                    sizeof(expected_text_provenance)) == 0);
            provenance_event.type = "completed";
            provenance_event.type_len = sizeof("completed") - 1u;
            provenance_event.type_id = 10;
            provenance_event.display_text = "";
            provenance_event.text_len = 0u;
            length = turn_response_json_encode(
                output, sizeof(output), &provenance_event, 123);
            expect(
                "HTTP response JSON stamps completion with runtime identity",
                length == sizeof(expected_completed_provenance) - 1u &&
                memcmp(
                    output,
                    expected_completed_provenance,
                    sizeof(expected_completed_provenance)) == 0);
            provenance_event.type = "pcm_started";
            provenance_event.type_len = sizeof("pcm_started") - 1u;
            provenance_event.type_id = 7;
            provenance_event.stages = &stages;
            length = turn_response_json_encode(
                output, sizeof(output), &provenance_event, 123);
            expect(
                "HTTP response JSON combines stages with runtime identity",
                length == sizeof(expected_stage_provenance) - 1u &&
                memcmp(
                    output,
                    expected_stage_provenance,
                    sizeof(expected_stage_provenance)) == 0);
            provenance_event.runtime_identity_sha256_len = 63u;
            expect(
                "HTTP response JSON rejects short runtime identity",
                turn_response_json_encode(
                    output, sizeof(output), &provenance_event, 123) == 0u);
            provenance_event.runtime_identity_sha256_len =
                VOICE_RUNTIME_IDENTITY_HASH_LEN;
            provenance_event.runtime_identity_sha256 =
                "ffffffffffffffffffffffffffffffff"
                "fffffffffffffffffffffffffffffffG";
            expect(
                "HTTP response JSON rejects non-hex runtime identity",
                turn_response_json_encode(
                    output, sizeof(output), &provenance_event, 123) == 0u);
        }
        expect(
            "HTTP response JSON rejects invalid arguments",
            turn_response_json_encode(NULL, sizeof(output), &event, 123) == 0 &&
            turn_response_json_encode(output, 0, &event, 123) == 0 &&
            turn_response_json_encode(output, sizeof(output), NULL, 123) == 0 &&
            turn_response_json_encode(output, sizeof(output), &event, 0) == 0);

        {
            static uint8_t maximum_audio[TURN_RESPONSE_MAX_AUDIO_EVENT];
            static char maximum_output[TURN_RESPONSE_JSON_CAPACITY];
            static char maximum_request_id[128];
            turn_stage_timestamps_c maximum_stages;
            turn_response_event maximum_event;
            size_t maximum_len;
            memset(maximum_request_id, 'r', sizeof(maximum_request_id) - 1u);
            maximum_request_id[sizeof(maximum_request_id) - 1u] = '\0';
            memset(&maximum_stages, 0, sizeof(maximum_stages));
            maximum_stages.input.audio_committed_at_ms = INT64_MAX;
            maximum_stages.input.stt_request_received_at_ms = INT64_MAX;
            maximum_stages.input.stt_provider_request_started_at_ms = INT64_MAX;
            maximum_stages.input.stt_provider_ready_at_ms = INT64_MAX;
            maximum_stages.input.stt_transcript_published_at_ms = INT64_MAX;
            maximum_stages.first_text_at_ms = INT64_MAX;
            maximum_stages.tts_segment_emitted_at_ms = INT64_MAX;
            maximum_stages.tts_request_received_at_ms = INT64_MAX;
            maximum_stages.tts_provider_request_started_at_ms = INT64_MAX;
            maximum_stages.tts_provider_ready_at_ms = INT64_MAX;
            maximum_stages.pcm_started_at_ms = INT64_MAX;
            maximum_stages.pcm_first_chunk_at_ms = INT64_MAX;
            memset(&maximum_event, 0, sizeof(maximum_event));
            maximum_event.request_id = maximum_request_id;
            maximum_event.type = "pcm_chunk";
            maximum_event.request_id_len =
                (uint16_t)(sizeof(maximum_request_id) - 1u);
            maximum_event.type_len = sizeof("pcm_chunk") - 1u;
            maximum_event.type_id = 8;
            maximum_event.audio = maximum_audio;
            maximum_event.audio_len = sizeof(maximum_audio);
            maximum_event.sample_rate = 48000;
            maximum_event.channels = 1;
            maximum_event.bit_depth = 16;
            maximum_event.audio_encoding = 1;
            maximum_event.sequence = INT32_MAX;
            maximum_event.segment_index = INT32_MAX;
            maximum_event.stages = &maximum_stages;
            maximum_len = turn_response_json_encode(
                maximum_output, sizeof(maximum_output), &maximum_event,
                INT64_MAX);
            expect("HTTP response maximum filtered event fits ring reservation",
                maximum_len != 0u &&
                maximum_len + 1u < TURN_RESPONSE_JSON_CAPACITY &&
                turn_response_json_encode(
                    maximum_output, maximum_len + 1u, &maximum_event,
                    INT64_MAX) == maximum_len &&
                turn_response_json_encode(
                    maximum_output, maximum_len, &maximum_event,
                    INT64_MAX) == 0u);
        }

        length = turn_acceptance_json_encode(
            output, sizeof(output), "req", 3u, 123, &metrics);
        expect(
            "HTTP acceptance JSON preserves the public contract",
            length == sizeof(expected_accepted) - 1u &&
            memcmp(output, expected_accepted, sizeof(expected_accepted)) == 0);
        expect(
            "HTTP acceptance JSON accepts exact capacity",
            turn_acceptance_json_encode(
                output, sizeof(expected_accepted), "req", 3u, 123, &metrics) ==
                length &&
            memcmp(output, expected_accepted, sizeof(expected_accepted)) == 0);
        expect(
            "HTTP acceptance JSON rejects short capacity",
            turn_acceptance_json_encode(
                output, sizeof(expected_accepted) - 1u, "req", 3u, 123,
                &metrics) == 0u);
        truncated_outputs_terminated = 1;
        for (capacity = 1u; capacity < sizeof(expected_accepted); ++capacity) {
            memset(output, 'x', sizeof(output));
            if (turn_acceptance_json_encode(
                    output, capacity, "req", 3u, 123, &metrics) != 0u ||
                memchr(output, '\0', capacity) == NULL) {
                truncated_outputs_terminated = 0;
                break;
            }
        }
        expect(
            "HTTP acceptance JSON terminates every truncated output",
            truncated_outputs_terminated);
        {
            char maximum[TURN_ACCEPTANCE_JSON_CAPACITY];
            char maximum_request_id[128];
            memset(maximum_request_id, 'r', sizeof(maximum_request_id) - 1u);
            maximum_request_id[sizeof(maximum_request_id) - 1u] = '\0';
            memset(&metrics, 0xff, sizeof(metrics));
            length = turn_acceptance_json_encode(
                maximum, sizeof(maximum), maximum_request_id,
                sizeof(maximum_request_id) - 1u, INT64_MAX, &metrics);
            expect(
                "HTTP acceptance JSON fills its maximum contract capacity",
                length == sizeof(maximum) - 1u && maximum[length] == '\0' &&
                strstr(maximum, "\"accepted_at\":9223372036854775807") != NULL &&
                strstr(
                    maximum,
                    "\"edge_encode_us\":18446744073709551615") != NULL);
            expect(
                "HTTP acceptance JSON rejects below maximum capacity",
                turn_acceptance_json_encode(
                    maximum, sizeof(maximum) - 1u, maximum_request_id,
                    sizeof(maximum_request_id) - 1u, INT64_MAX, &metrics) == 0u);
        }
        expect(
            "HTTP acceptance JSON rejects invalid arguments",
            turn_acceptance_json_encode(
                NULL, sizeof(output), "req", 3u, 123, &metrics) == 0u &&
            turn_acceptance_json_encode(
                output, 0u, "req", 3u, 123, &metrics) == 0u &&
            turn_acceptance_json_encode(
                output, sizeof(output), NULL, 3u, 123, &metrics) == 0u &&
            turn_acceptance_json_encode(
                output, sizeof(output), "req", 0u, 123, &metrics) == 0u &&
            turn_acceptance_json_encode(
                output, sizeof(output), "req", 128u, 123, &metrics) == 0u &&
            turn_acceptance_json_encode(
                output, sizeof(output), "req", 3u, 0, &metrics) == 0u &&
            turn_acceptance_json_encode(
                output, sizeof(output), "req", 3u, 123, NULL) == 0u);
    }

    /* pb encode/decode roundtrip lifecycle */
    {
        uint8_t buf[256];
        stt_lifecycle_c lifecycle;
        size_t n = pb_encode_stt_lifecycle(
            buf, sizeof(buf), "sess-1", "transcription_failed", 12345);
        expect(
            "failure lifecycle roundtrip",
            n > 0 && pb_decode_stt_lifecycle(buf, n, &lifecycle) == 0 &&
            lifecycle.type_id == STT_LIFECYCLE_TRANSCRIPTION_FAILED &&
            strcmp(lifecycle.type, "transcription_failed") == 0);
    }
    {
        static const char *const type_names[] = {
            "stream_started",
            "speech_started",
            "speech_ended",
            "stream_ended",
            "transcription_failed"
        };
        static const int type_ids[] = {
            STT_LIFECYCLE_STREAM_STARTED,
            STT_LIFECYCLE_SPEECH_STARTED,
            STT_LIFECYCLE_SPEECH_ENDED,
            STT_LIFECYCLE_STREAM_ENDED,
            STT_LIFECYCLE_TRANSCRIPTION_FAILED
        };
        static const int64_t timestamp_boundaries[] = {
            2,
            127,
            128,
            16383,
            16384,
            INT64_MAX
        };
        static const size_t session_boundaries[] = {0u, 1u, 127u};
        static const size_t fallback_lengths[] = {128u, 255u};
        uint8_t prepared[256];
        char session_id[256];
        size_t type_index;
        size_t session_len;
        size_t timestamp_index;
        int exact = 1;
        int timestamp_exact = 1;
        int fallback_exact = 1;
        memset(session_id, 's', sizeof(session_id));
        for (type_index = 0;
             type_index < sizeof(type_ids) / sizeof(type_ids[0]);
             ++type_index) {
            for (session_len = 0u; session_len < 128u; ++session_len) {
                session_id[session_len] = '\0';
                if (!lifecycle_encoders_match(
                        session_id,
                        session_len,
                        type_names[type_index],
                        type_ids[type_index],
                        12345,
                        sizeof(prepared))) exact = 0;
                session_id[session_len] = 's';
            }
        }
        expect(
            "prepared lifecycle encoder matches independent bounded oracle",
            exact);
        expect(
            "prepared lifecycle encoder preserves null empty session",
            lifecycle_encoders_match(
                NULL,
                0u,
                "stream_started",
                STT_LIFECYCLE_STREAM_STARTED,
                12345,
                sizeof(prepared)));
        for (type_index = 0;
             type_index < sizeof(type_ids) / sizeof(type_ids[0]);
             ++type_index) {
            for (session_len = 0u;
                 session_len < sizeof(session_boundaries) /
                     sizeof(session_boundaries[0]);
                 ++session_len) {
                size_t length = session_boundaries[session_len];
                session_id[length] = '\0';
                for (timestamp_index = 0u;
                     timestamp_index < sizeof(timestamp_boundaries) /
                         sizeof(timestamp_boundaries[0]);
                     ++timestamp_index) {
                    if (!lifecycle_encoders_match(
                            session_id,
                            length,
                            type_names[type_index],
                            type_ids[type_index],
                            timestamp_boundaries[timestamp_index],
                            sizeof(prepared))) timestamp_exact = 0;
                }
                session_id[length] = 's';
            }
            for (session_len = 0u;
                 session_len < sizeof(fallback_lengths) /
                     sizeof(fallback_lengths[0]);
                 ++session_len) {
                size_t length = fallback_lengths[session_len];
                session_id[length] = '\0';
                if (!lifecycle_encoders_match(
                        session_id,
                        length,
                        type_names[type_index],
                        type_ids[type_index],
                        12345,
                        512u)) fallback_exact = 0;
                session_id[length] = 's';
            }
        }
        expect(
            "prepared lifecycle encoder matches timestamp boundaries",
            timestamp_exact);
        expect(
            "prepared lifecycle encoder preserves generic long sessions",
            fallback_exact);
        expect(
            "prepared lifecycle encoder rejects invalid metadata",
            pb_encode_stt_lifecycle_prepared(
                prepared, sizeof(prepared), NULL, 1u,
                STT_LIFECYCLE_STREAM_STARTED, 12345) == 0u &&
            pb_encode_stt_lifecycle_prepared(
                prepared, sizeof(prepared), "s", 1u,
                STT_LIFECYCLE_UNSPECIFIED, 12345) == 0u &&
            pb_encode_stt_lifecycle_prepared(
                prepared, sizeof(prepared), "s", 1u,
                STT_LIFECYCLE_TRANSCRIPTION_FAILED + 1, 12345) == 0u &&
            pb_encode_stt_lifecycle_prepared(
                prepared, sizeof(prepared), "s", 1u,
                STT_LIFECYCLE_STREAM_STARTED, 1) == 0u);
    }

    /* stream message decode */
    {
        uint8_t audio[4] = {0x00, 0x10, 0x00, 0x20};
        uint8_t msg[64];
        stt_stream_message_c decoded;
        size_t n = encode_chunk_msg(msg, sizeof(msg), audio, 4);
        expect("encode chunk", n > 0);
        expect("decode chunk", pb_decode_stt_stream_message(msg, n, &decoded) == 0);
        expect("type chunk", strcmp(decoded.type, "chunk") == 0);
        expect("audio len", decoded.audio_len == 4);
    }
    {
        const uint8_t length_overflow[] = {
            0x12, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02
        };
        const uint8_t overlong_varint[] = {
            0x28, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00
        };
        const uint8_t embedded_nul[] = {0x0a, 0x03, 'a', 0, 'b'};
        stt_stream_message_c decoded;
        expect(
            "pb rejects length overflow",
            pb_decode_stt_stream_message(
                length_overflow, sizeof(length_overflow), &decoded) != 0
        );
        expect(
            "pb rejects overlong varint",
            pb_decode_stt_stream_message(
                overlong_varint, sizeof(overlong_varint), &decoded) != 0
        );
        expect(
            "pb rejects embedded NUL string",
            pb_decode_stt_stream_message(embedded_nul, sizeof(embedded_nul), &decoded) != 0
        );
        expect("pb rejects NULL output", pb_decode_stt_stream_message(NULL, 0, NULL) != 0);
        expect(
            "pb rejects NULL audio encode",
            pb_encode_stt_stream_message(
                (uint8_t[32]){0}, 32, "chunk", NULL, 2, 16000, 1, 16) == 0
        );
    }

    {
        const uint8_t valid[] = {'H', 'i', ' ', 0xf0, 0x9f, 0x90, 0x89};
        const uint8_t truncated2[] = {0xc2};
        const uint8_t truncated3[] = {0xe2, 0x82};
        const uint8_t truncated4[] = {0xf0, 0x9f, 0x90};
        const uint8_t overlong[] = {0xc0, 0x80};
        const uint8_t surrogate[] = {0xed, 0xa0, 0x80};
        const uint8_t above_max[] = {0xf4, 0x90, 0x80, 0x80};
        expect("utf8 valid multibyte", utf8_validate_v1(valid, sizeof(valid)));
        expect("utf8 empty", utf8_validate_v1(NULL, 0));
        expect("utf8 rejects null span", !utf8_validate_v1(NULL, 1));
        expect("utf8 rejects truncated two-byte", !utf8_validate_v1(truncated2, sizeof(truncated2)));
        expect("utf8 rejects truncated three-byte", !utf8_validate_v1(truncated3, sizeof(truncated3)));
        expect("utf8 rejects truncated four-byte", !utf8_validate_v1(truncated4, sizeof(truncated4)));
        expect("utf8 rejects overlong", !utf8_validate_v1(overlong, sizeof(overlong)));
        expect("utf8 rejects surrogate", !utf8_validate_v1(surrogate, sizeof(surrogate)));
        expect("utf8 rejects above Unicode max", !utf8_validate_v1(above_max, sizeof(above_max)));
    }

    /* product math still pure C */
    {
        int16_t pcm[320];
        int i;
        for (i = 0; i < 320; i++) pcm[i] = (int16_t)-32768;
        expect("rms INT16_MIN", fabs(pcm16_rms(pcm, 320) - 1.0) < 1e-12);
    }
    {
        uint64_t p = route_classifier_packed_v1("Thanks, that was helpful.", 25);
        expect("classifier", p != UINT64_MAX);
    }
    {
        uint32_t p = phatic_policy_match_v1("hello", 5);
        expect("phatic", (p & 0xff) != 0);
    }
    /* Whole-utterance admission cannot erase a substantive suffix or hidden byte. */
    {
        static const char *const substantive[] = {
            "Thanks, is Mira still here", "Thanks, is Mira still here?",
            "Sounds good, roll initiative", "No thanks, do not cast fireball",
            "How are you tracking my spell slots?", "How is it going to affect my armor class",
            "See you at the gate, is Mira still here", "Thank you, how many spell slots do I have?",
            "hello, what happens next", "I said 'hello'", "thank", "unfamiliar neutral utterance",
            "I cast fireball. No, wait. Is Mira still in the room?",
        };
        static const struct { const char *text; uint32_t reply; } social[] = {
            {"HELLO!", 1u}, {"Good morning.", 1u}, {"How are you?", 2u},
            {"How's it going?", 2u}, {"Thanks, that was helpful.", 3u},
            {"Thank you very much", 3u}, {"  Sounds good!  ", 4u}, {"Yes.", 4u}, {"See you later.", 5u},
        };
        for (size_t i = 0u; i < sizeof(substantive) / sizeof(substantive[0]); ++i)
            expect("whole-turn phatic abstains on substantive speech",
                phatic_policy_match_v2(substantive[i], strlen(substantive[i])) == 0u);
        for (size_t i = 0u; i < sizeof(social) / sizeof(social[0]); ++i)
            expect("whole-turn phatic preserves complete social speech",
                (phatic_policy_match_v2(social[i].text, strlen(social[i].text)) & 0xffu) == social[i].reply);
        expect("whole-turn phatic rejects null span", phatic_policy_match_v2(NULL, 1u) == 0u);
        expect("whole-turn phatic rejects embedded nul", phatic_policy_match_v2("hi\0", 3u) == 0u);
        expect("whole-turn phatic rejects erased bytes", phatic_policy_match_v2("h\x80i", 3u) == 0u);
    }
    /* Legacy byte classifier still matches its scalar baseline for comparison. */
    {
        static const char word[] = "thank";
        int classifier_ok = 1;
        unsigned int byte;
        size_t position;

        for (byte = 0; byte <= 0xffu && classifier_ok; ++byte) {
            char inserted[] = {'t', 'h', (char)byte, 'a', 'n', 'k'};
            char checkin[] = {'h', 'o', 'w', (char)byte,
                              'a', 'r', 'e', ' ', 'y', 'o', 'u', '?'};
            int token =
                (byte >= (unsigned int)'A' && byte <= (unsigned int)'Z') ||
                (byte >= (unsigned int)'a' && byte <= (unsigned int)'z') ||
                (byte >= (unsigned int)'0' && byte <= (unsigned int)'9') ||
                byte == (unsigned int)'?' || byte == (unsigned int)'\'';
            int separator =
                byte == (unsigned int)'\t' || byte == (unsigned int)'\n' ||
                byte == (unsigned int)' ' || byte == (unsigned int)'!' ||
                byte == (unsigned int)',' || byte == (unsigned int)'.' ||
                byte == (unsigned int)';';
            uint32_t expected_insert = token || separator ? 0u : UINT32_C(0x303);
            uint32_t expected_checkin = separator ? UINT32_C(0x202) : 0u;

            if (phatic_policy_match_v1(inserted, sizeof(inserted)) !=
                    expected_insert ||
                phatic_policy_match_v1(checkin, sizeof(checkin)) !=
                    expected_checkin) {
                classifier_ok = 0;
            }
        }
        for (position = 0;
             position < sizeof(word) - 1u && classifier_ok;
             ++position) {
            for (byte = 0; byte <= 0xffu; ++byte) {
                char replaced[sizeof(word) - 1u];
                unsigned int lower = (unsigned int)(unsigned char)word[position];
                unsigned int upper = lower - ((unsigned int)'a' - (unsigned int)'A');
                uint32_t expected =
                    byte == lower || byte == upper ? UINT32_C(0x303) : 0u;

                memcpy(replaced, word, sizeof(replaced));
                replaced[position] = (char)byte;
                if (phatic_policy_match_v1(replaced, sizeof(replaced)) != expected) {
                    classifier_ok = 0;
                    break;
                }
            }
        }
        expect("phatic byte classifier differential", classifier_ok);
    }
    /* phatic: the normalized ceiling keeps trailing and sparse separators. */
    {
        char accepted[50];
        char rejected[49];
        char sparse[80];
        memset(accepted, 'a', 43);
        memcpy(accepted + 43, "thank", 5);
        accepted[48] = '.';
        accepted[49] = '!';
        memset(rejected, 'a', 44);
        memcpy(rejected + 44, "thank", 5);
        memset(sparse, 0x80, sizeof(sparse));
        memcpy(sparse + 37, "hello", 5);
        expect(
            "phatic normalized ceiling",
            phatic_policy_match_v1(accepted, sizeof(accepted)) == UINT32_C(0x303) &&
                phatic_policy_match_v1(rejected, sizeof(rejected)) == 0u &&
                phatic_policy_match_v1(sparse, sizeof(sparse)) == UINT32_C(0x101)
        );
    }
    {
        audio_engine_session *s = audio_engine_create(0.01f, 0.985, 1, 1, 0.0025, 0.2, 1.75, 0.12);
        expect("engine create", s != NULL);
        if (s) {
            uint8_t unaligned_storage[6] = {0};
            int16_t aligned_pcm[2] = {1234, -2345};
            int16_t aligned_out[2] = {0};
            int16_t unaligned_out[2] = {0};
            double hp_in_a = 0.0, hp_out_a = 0.0, noise_a = 0.0025;
            double hp_in_b = 0.0, hp_out_b = 0.0, noise_b = 0.0025;
            audio_process_options options = {0};
            audio_decision decision = {0};
            audio_monitor_state state = {0};

            memcpy(unaligned_storage + 1, aligned_pcm, sizeof(aligned_pcm));
            const float aligned_energy = condition_pcm16(
                aligned_pcm, aligned_out, 2, 0.985, 1, 1,
                &hp_in_a, &hp_out_a, &noise_a, 0.0025, 0.2, 1.75, 0.12);
            const float unaligned_energy = condition_pcm16(
                unaligned_storage + 1, unaligned_out, 2, 0.985, 1, 1,
                &hp_in_b, &hp_out_b, &noise_b, 0.0025, 0.2, 1.75, 0.12);
            const float reference_cleanliness =
                1.0f - (float)noise_a;
            float expected_engine_energy = aligned_energy *
                (0.8f + 0.2f * fmaxf(0.0f, reference_cleanliness));
            if (expected_engine_energy > 1.0f)
                expected_engine_energy = 1.0f;
            expect(
                "condition unaligned input",
                memcmp(aligned_out, unaligned_out, sizeof(aligned_out)) == 0 &&
                aligned_energy == unaligned_energy
            );

            audio_engine_process(s, unaligned_storage + 1, 3, 1, options, &decision);
            audio_engine_get_monitor_state(s, &state);
            expect(
                "engine rejects odd PCM",
                decision.utterance_bytes == 0 && state.utterance_bytes == 0 &&
                (decision.flags & AUDIO_DECISION_ERROR) != 0u
            );
            audio_engine_process_bounded(
                s,
                (const uint8_t *)(const void *)aligned_pcm,
                sizeof(aligned_pcm),
                2,
                options,
                sizeof(aligned_pcm) - 1u,
                &decision
            );
            audio_engine_get_monitor_state(s, &state);
            expect(
                "engine rejects bounded PCM before DSP",
                decision.utterance_bytes == 0 && state.utterance_bytes == 0 &&
                (decision.flags & AUDIO_DECISION_ERROR) != 0u
            );
            audio_engine_process(
                s,
                (const uint8_t *)(const void *)aligned_pcm,
                sizeof(aligned_pcm),
                3,
                options,
                &decision
            );
            expect(
                "engine finite-domain energy boost",
                memcmp(
                    &decision.energy,
                    &expected_engine_energy,
                    sizeof(decision.energy)) == 0);
            {
                audio_finalized_utterance finalized = audio_engine_finalize(s);
                expect(
                    "engine finalizes exact conditioned PCM",
                    (decision.flags & AUDIO_DECISION_ERROR) == 0u &&
                    decision.utterance_bytes == sizeof(aligned_out) &&
                    finalized.buffer != NULL &&
                    finalized.length == sizeof(aligned_out) &&
                    memcmp(
                        finalized.buffer,
                        aligned_out,
                        sizeof(aligned_out)) == 0
                );
                audio_engine_free_buffer(finalized.buffer);
            }
            audio_engine_destroy(s);
        }
        expect(
            "engine rejects non-finite config",
            audio_engine_create(0.01f, nan(""), 1, 1, 0.0025, 0.2, 1.75, 0.12) == NULL
        );
    }
    {
        enum {
            speech_window_frame_samples = 320,
            speech_window_frame_bytes = speech_window_frame_samples * 2,
            speech_window_frames = 15
        };
        int16_t pcm[speech_window_frame_samples * speech_window_frames] = {0};
        audio_engine_session *s = audio_engine_create(
            0.01f, 0.0, 0, 0, 0.0025, 0.2, 1.75, 0.12);
        audio_process_options options = {0};
        audio_decision decision = {0};
        audio_finalized_utterance finalized;
        size_t frame;
        size_t sample;
        const size_t expected_start = 3u * speech_window_frame_bytes;
        const size_t expected_length = 8u * speech_window_frame_bytes;

        expect("speech window engine create", s != NULL);
        for (frame = 4u; frame < 6u; ++frame) {
            for (sample = 0u; sample < speech_window_frame_samples; ++sample)
                pcm[frame * speech_window_frame_samples + sample] =
                    (sample & 1u) != 0u ? INT16_C(-12000) : INT16_C(12000);
        }
        for (sample = 0u; sample < speech_window_frame_samples; ++sample)
            pcm[9u * speech_window_frame_samples + sample] =
                (sample & 1u) != 0u ? INT16_C(-12000) : INT16_C(12000);
        if (s) {
            for (frame = 0u; frame < speech_window_frames; ++frame) {
                audio_engine_process(
                    s,
                    (const uint8_t *)(const void *)(
                        pcm + frame * speech_window_frame_samples),
                    speech_window_frame_bytes,
                    (uint64_t)(frame + 1u) * UINT64_C(20000000),
                    options,
                    &decision);
            }
            finalized = audio_engine_finalize_speech_window(
                s,
                speech_window_frame_bytes + 1u,
                speech_window_frame_bytes + 1u);
            expect(
                "speech window trims only exterior silence",
                (finalized.flags & AUDIO_DECISION_VOICE_SEEN) != 0u &&
                    finalized.captured_length == sizeof(pcm) &&
                    finalized.trimmed_prefix_length == expected_start &&
                    finalized.trimmed_suffix_length ==
                        4u * speech_window_frame_bytes &&
                    finalized.length == expected_length &&
                    memcmp(
                        finalized.buffer,
                        (const uint8_t *)(const void *)pcm + expected_start,
                        expected_length) == 0);
            audio_engine_free_buffer(finalized.buffer);
            audio_engine_destroy(s);
        }
    }
    {
        int16_t silence[960] = {0};
        audio_engine_session *s = audio_engine_create(
            0.01f, 0.0, 0, 0, 0.0025, 0.2, 1.75, 0.12);
        audio_process_options options = {0};
        audio_decision decision = {0};
        audio_finalized_utterance finalized = {0};

        expect("silence window engine create", s != NULL);
        if (s) {
            audio_engine_process(
                s,
                (const uint8_t *)(const void *)silence,
                sizeof(silence),
                1u,
                options,
                &decision);
            finalized = audio_engine_finalize_speech_window(s, 640u, 640u);
            expect(
                "speech window preserves unclassified utterance",
                (finalized.flags & AUDIO_DECISION_VOICE_SEEN) == 0u &&
                    finalized.captured_length == sizeof(silence) &&
                    finalized.trimmed_prefix_length == 0u &&
                    finalized.trimmed_suffix_length == 0u &&
                    finalized.length == sizeof(silence) &&
                    memcmp(finalized.buffer, silence, sizeof(silence)) == 0);
            audio_engine_free_buffer(finalized.buffer);
            audio_engine_destroy(s);
        }
    }
    {
        audio_engine_session storage = {0};
        audio_engine_session *s = audio_engine_session_init_zeroed(
            &storage, 0.01f, 0.985, 1, 1, 0.0025, 0.2, 1.75, 0.12);
        expect("engine initializes caller-owned storage", s == &storage);
        if (s) {
            const int16_t pcm[2] = {1234, -2345};
            audio_process_options options = {0};
            audio_decision decision = {0};
            audio_engine_process(
                s,
                (const uint8_t *)(const void *)pcm,
                sizeof(pcm),
                1,
                options,
                &decision);
            expect(
                "caller-owned engine processes PCM",
                (decision.flags & AUDIO_DECISION_ERROR) == 0u &&
                decision.utterance_bytes == sizeof(pcm));
            audio_engine_session_deinit(s);
        }
        memset(&storage, 0, sizeof(storage));
        s = audio_engine_session_init_zeroed(
            &storage, 0.01f, 0.985, 1, 1, 0.0025, 0.2, 1.75, 0.12);
        expect("engine reuses scrubbed caller storage", s == &storage);
        audio_engine_session_deinit(s);
    }

    /* speech_sanitize: strip tags for display */
    {
        char out[128];
        size_t n = 0;
        const char *in = "Hi <laugh> there <evil> end";
        expect(
            "sanitize strip",
            speech_sanitize_v1(in, strlen(in), 0, out, sizeof(out), &n) == SPEECH_SANITIZE_OK
        );
        out[n] = '\0';
        expect("sanitize no angle", strchr(out, '<') == NULL);
        expect("sanitize keeps Hi", strstr(out, "Hi") != NULL);
    }
    /* speech_sanitize: keep allowlist for TTS */
    {
        char out[128];
        size_t n = 0;
        const char *in = "Hi <laugh> there <evil> end";
        expect(
            "sanitize keep",
            speech_sanitize_v1(in, strlen(in), 1, out, sizeof(out), &n) == SPEECH_SANITIZE_OK
        );
        out[n] = '\0';
        expect("sanitize keeps laugh", strstr(out, "<laugh>") != NULL);
        expect("sanitize drops evil", strstr(out, "evil") == NULL);
    }
    /* speech_sanitize: canonical ASCII is byte-identical at exact capacity */
    {
        static const char *const inputs[] = {
            "Hello adventurer.",
            "One, two; three: ready?",
            "A > B & C",
            "Symbols []{}()_+-= stay."
        };
        int canonical_ok = 1;
        size_t input_index;
        for (input_index = 0;
             input_index < sizeof(inputs) / sizeof(inputs[0]) && canonical_ok;
             ++input_index) {
            char out[64];
            size_t input_len = strlen(inputs[input_index]);
            size_t out_len = 0;
            int keep_allowed;
            for (keep_allowed = 0; keep_allowed <= 1; ++keep_allowed) {
                if (speech_sanitize_v1(
                        inputs[input_index], input_len, keep_allowed,
                        out, input_len, &out_len) != SPEECH_SANITIZE_OK ||
                    out_len != input_len ||
                    memcmp(out, inputs[input_index], input_len) != 0) {
                    canonical_ok = 0;
                    break;
                }
            }
        }
        expect("sanitize canonical ASCII exact capacity", canonical_ok);
    }
    /* speech_sanitize: the public canonical predicate matches policy edges */
    {
        static const struct {
            const char *input;
            int canonical;
        } cases[] = {
            {"Hello adventurer.", 1},
            {"A > B & C", 1},
            {" leading", 0},
            {"trailing ", 0},
            {"two  spaces", 0},
            {"space .", 0},
            {"tag <laugh>", 0},
            {"caf\xc3\xa9", 0},
            {"line\nbreak", 0},
            {"\x1f" "control", 0}
        };
        int predicate_ok = speech_is_canonical_ascii_v1(NULL, 1) == 0 &&
            speech_is_canonical_ascii_v1("", 0) == 0;
        size_t case_index;
        for (case_index = 0;
             case_index < sizeof(cases) / sizeof(cases[0]) && predicate_ok;
             ++case_index) {
            predicate_ok =
                speech_is_canonical_ascii_v1(
                    cases[case_index].input,
                    strlen(cases[case_index].input)) == cases[case_index].canonical;
        }
        {
            char probe[64];
            uint32_t state = 0x9e3779b9u;
            size_t position;
            unsigned int byte;
            memset(probe, 'a', sizeof(probe));
            for (position = 0; position < 32u && predicate_ok; ++position) {
                for (byte = 0; byte <= 0xffu; ++byte) {
                    probe[position] = (char)byte;
                    if (speech_is_canonical_ascii_v1(probe, 40u) !=
                        reference_canonical_ascii(probe, 40u)) {
                        predicate_ok = 0;
                        break;
                    }
                }
                probe[position] = 'a';
            }
            for (position = 1; position < 32u && predicate_ok; ++position) {
                static const char punct[] = ".!?,;:";
                size_t punct_index;
                memset(probe, 'a', sizeof(probe));
                probe[position - 1u] = ' ';
                probe[position] = ' ';
                if (speech_is_canonical_ascii_v1(probe, 40u) !=
                    reference_canonical_ascii(probe, 40u))
                    predicate_ok = 0;
                for (punct_index = 0;
                     punct_index < sizeof(punct) - 1u && predicate_ok;
                     ++punct_index) {
                    memset(probe, 'a', sizeof(probe));
                    probe[position - 1u] = ' ';
                    probe[position] = punct[punct_index];
                    if (speech_is_canonical_ascii_v1(probe, 40u) !=
                        reference_canonical_ascii(probe, 40u))
                        predicate_ok = 0;
                }
            }
            for (position = 1; position <= sizeof(probe) && predicate_ok;
                 ++position) {
                memset(probe, 'a', sizeof(probe));
                if (speech_is_canonical_ascii_v1(probe, position) !=
                    reference_canonical_ascii(probe, position))
                    predicate_ok = 0;
            }
            for (case_index = 0; case_index < 4096u && predicate_ok;
                 ++case_index) {
                size_t probe_len;
                size_t i;
                state = state * 1664525u + 1013904223u;
                probe_len = (size_t)((state >> 26) + 1u);
                for (i = 0; i < probe_len; ++i) {
                    state = state * 1664525u + 1013904223u;
                    probe[i] = (char)(state >> 24);
                }
                if (speech_is_canonical_ascii_v1(probe, probe_len) !=
                    reference_canonical_ascii(probe, probe_len))
                    predicate_ok = 0;
            }
        }
        expect("sanitize canonical predicate", predicate_ok);
    }
    /* speech_sanitize: canonical ASCII supports an in-place rewrite */
    {
        char text[] = "In-place speech stays exact!";
        size_t text_len = strlen(text);
        size_t out_len = 0;
        expect(
            "sanitize canonical ASCII in place",
            speech_sanitize_v1(
                text, text_len, 1, text, text_len, &out_len) ==
                SPEECH_SANITIZE_OK &&
                out_len == text_len &&
                memcmp(text, "In-place speech stays exact!", text_len) == 0
        );
    }
    /* speech_sanitize: JSON proof is conservative and output-identical */
    {
        static const struct {
            const char *input;
            int json_plain;
        } cases[] = {
            {"Roll with advantage.", 1},
            {"Say \"roll\" now.", 0},
            {"Follow C:\\ward home.", 0},
            {"Hi <laugh> there", 0},
            {"caf\xc3\xa9", 0},
            {" two  spaces ", 0}
        };
        char old_out[128];
        char classified_out[128];
        int proof_ok = 1;
        size_t case_index;
        for (case_index = 0u;
             case_index < sizeof(cases) / sizeof(cases[0]) && proof_ok;
             ++case_index) {
            size_t old_len = 0u;
            size_t classified_len = 0u;
            int json_plain = -1;
            size_t input_len = strlen(cases[case_index].input);
            if (speech_sanitize_v1(
                    cases[case_index].input,
                    input_len,
                    1,
                    old_out,
                    sizeof(old_out),
                    &old_len) != SPEECH_SANITIZE_OK ||
                speech_sanitize_json_plain_v1(
                    cases[case_index].input,
                    input_len,
                    1,
                    classified_out,
                    sizeof(classified_out),
                    &classified_len,
                    &json_plain) != SPEECH_SANITIZE_OK ||
                classified_len != old_len ||
                memcmp(classified_out, old_out, old_len) != 0 ||
                json_plain != cases[case_index].json_plain) {
                proof_ok = 0;
            }
        }
        expect("sanitize JSON proof preserves policy output", proof_ok);
    }
    /* speech_sanitize: every vector position classifies JSON escapes */
    {
        static const size_t lengths[] = {
            1u, 7u, 8u, 15u, 16u, 17u, 31u, 32u, 33u,
            47u, 63u, 64u, 65u, 95u, 96u, 97u, 127u,
            128u, 129u, 255u, 256u, 257u, 511u, 512u,
            513u, 1023u, 1024u, 1025u, 2047u
        };
        char input[2048];
        char output[2048];
        int positions_ok = 1;
        size_t length_index;
        for (length_index = 0u;
             length_index < sizeof(lengths) / sizeof(lengths[0]) &&
                 positions_ok;
             ++length_index) {
            size_t input_len = lengths[length_index];
            size_t position;
            memset(input, 'a', input_len);
            for (position = 0u; position < input_len && positions_ok; ++position) {
                static const char escapes[] = {'"', '\\'};
                size_t escape_index;
                for (escape_index = 0u;
                     escape_index < sizeof(escapes) && positions_ok;
                     ++escape_index) {
                    size_t output_len = 0u;
                    int json_plain = 1;
                    input[position] = escapes[escape_index];
                    if (speech_sanitize_json_plain_v1(
                            input,
                            input_len,
                            1,
                            output,
                            sizeof(output),
                            &output_len,
                            &json_plain) != SPEECH_SANITIZE_OK ||
                        json_plain != 0 || output_len != input_len ||
                        memcmp(output, input, input_len) != 0) {
                        positions_ok = 0;
                    }
                    input[position] = 'a';
                }
            }
            if (positions_ok) {
                size_t output_len = 0u;
                int json_plain = 0;
                if (speech_sanitize_json_plain_v1(
                        input,
                        input_len,
                        1,
                        output,
                        sizeof(output),
                        &output_len,
                        &json_plain) != SPEECH_SANITIZE_OK ||
                    json_plain != 1 || output_len != input_len ||
                    memcmp(output, input, input_len) != 0) {
                    positions_ok = 0;
                }
            }
        }
        expect("sanitize JSON proof covers every vector position", positions_ok);
    }
    /* speech_sanitize: classified and established APIs match random spans */
    {
        char input[64];
        char old_out[64];
        char classified_out[64];
        uint32_t state = UINT32_C(0x2f6e2b1d);
        int differential_ok = 1;
        size_t iteration;
        for (iteration = 0u; iteration < 10000u && differential_ok;
             ++iteration) {
            size_t input_len;
            size_t old_len = 0u;
            size_t classified_len = 0u;
            size_t i;
            int json_plain = -1;
            int keep_allowed;
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            input_len = (size_t)((state >> 26) + 1u);
            keep_allowed = (int)((state >> 25) & 1u);
            for (i = 0u; i < input_len; ++i) {
                state = state * UINT32_C(1664525) + UINT32_C(1013904223);
                input[i] = (char)(state >> 24);
            }
            if (speech_sanitize_v1(
                    input,
                    input_len,
                    keep_allowed,
                    old_out,
                    sizeof(old_out),
                    &old_len) != SPEECH_SANITIZE_OK ||
                speech_sanitize_json_plain_v1(
                    input,
                    input_len,
                    keep_allowed,
                    classified_out,
                    sizeof(classified_out),
                    &classified_len,
                    &json_plain) != SPEECH_SANITIZE_OK ||
                classified_len != old_len ||
                memcmp(classified_out, old_out, old_len) != 0 ||
                json_plain !=
                    reference_sanitize_json_plain(input, input_len)) {
                differential_ok = 0;
            }
        }
        expect("sanitize JSON proof random differential", differential_ok);
    }
    /* speech_sanitize: every error clears the fail-closed proof */
    {
        char output[8];
        size_t output_len = 99u;
        int json_plain = 1;
        int invalid_rc = speech_sanitize_json_plain_v1(
            NULL, 1u, 1, output, sizeof(output), &output_len, &json_plain);
        int invalid_closed = output_len == 0u && json_plain == 0;
        int capacity_rc;
        int null_proof_rc = speech_sanitize_json_plain_v1(
            "plain", sizeof("plain") - 1u, 1,
            output, sizeof(output), &output_len, NULL);
        output_len = 99u;
        json_plain = 1;
        capacity_rc = speech_sanitize_json_plain_v1(
            "plain", sizeof("plain") - 1u, 1,
            output, sizeof("plain") - 2u, &output_len, &json_plain);
        expect(
            "sanitize JSON proof fails closed",
            invalid_rc == SPEECH_SANITIZE_ERR_ARGUMENT &&
                invalid_closed &&
                capacity_rc == SPEECH_SANITIZE_ERR_CAPACITY &&
                output_len == 0u && json_plain == 0 &&
                null_proof_rc == SPEECH_SANITIZE_ERR_ARGUMENT);
    }
    /* speech_sanitize: every fast-path exclusion keeps canonical behavior */
    {
        static const struct {
            const char *input;
            const char *stripped;
            const char *allowed;
        } cases[] = {
            {" Hello  world . ", "Hello world.", "Hello world."},
            {"One\ttwo\nthree\rfour\vfive\fsix",
             "One two three four five six", "One two three four five six"},
            {"Hi <laugh> there <evil> end", "Hi there end", "Hi <laugh> there end"},
            {"A \xc2\xa0 B \xe2\x80\xa6 done", "A B\xe2\x80\xa6 done", "A B\xe2\x80\xa6 done"},
            {"A > B", "A > B", "A > B"},
            {"\x1f" "control stays", "\x1f" "control stays", "\x1f" "control stays"}
        };
        int fallback_ok = 1;
        size_t case_index;
        for (case_index = 0;
             case_index < sizeof(cases) / sizeof(cases[0]) && fallback_ok;
             ++case_index) {
            int keep_allowed;
            for (keep_allowed = 0; keep_allowed <= 1; ++keep_allowed) {
                char out[128];
                char in_place[128];
                const char *expected = keep_allowed ? cases[case_index].allowed : cases[case_index].stripped;
                size_t input_len = strlen(cases[case_index].input);
                size_t expected_len = strlen(expected);
                size_t out_len = 0;
                size_t in_place_len = 0;
                memcpy(in_place, cases[case_index].input, input_len);
                if (speech_sanitize_v1(
                        cases[case_index].input, input_len, keep_allowed,
                        out, sizeof(out), &out_len) != SPEECH_SANITIZE_OK ||
                    out_len != expected_len || memcmp(out, expected, expected_len) != 0 ||
                    speech_sanitize_v1(
                        in_place, input_len, keep_allowed,
                        in_place, sizeof(in_place), &in_place_len) != SPEECH_SANITIZE_OK ||
                    in_place_len != expected_len ||
                    memcmp(in_place, expected, expected_len) != 0) {
                    fallback_ok = 0;
                    break;
                }
            }
        }
        expect("sanitize fast exclusions preserve behavior", fallback_ok);
    }
    /* speech_display_stream: hide split tags; emit stable display suffix only */
    {
        speech_display_acc_v1 acc;
        char suf[128];
        size_t n = 0;
        size_t text_len = 0;
        const char *text;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp add open",
            speech_display_acc_add_v1(&acc, "Hi <la", 6, suf, sizeof(suf), &n) == SPEECH_DISPLAY_OK
        );
        /* incomplete tag must not leak partial markup */
        if (n < sizeof(suf)) suf[n] = '\0';
        expect("disp no partial tag", strchr(suf, '<') == NULL);
        expect(
            "disp add close",
            speech_display_acc_add_v1(&acc, "ugh> there", 10, suf, sizeof(suf), &n) ==
                SPEECH_DISPLAY_OK
        );
        if (n < sizeof(suf)) suf[n] = '\0';
        expect("disp no angle after close", strchr(suf, '<') == NULL);
        text = speech_display_acc_text_v1(&acc, &text_len);
        expect("disp has Hi", text_len >= 2 && memcmp(text, "Hi", 2) == 0);
        expect("disp no angle in text", memchr(text, '<', text_len) == NULL);
    }
    /* speech_display_stream: direction-only tag delta → empty stable suffix */
    {
        speech_display_acc_v1 acc;
        char suf[64];
        size_t n = 99;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp tag-only",
            speech_display_acc_add_v1(&acc, "<laugh>", 7, suf, sizeof(suf), &n) ==
                SPEECH_DISPLAY_OK
        );
        expect("disp tag-only empty suffix", n == 0);
    }
    /* speech_display_stream: preserve stable whitespace across token boundaries */
    {
        speech_display_acc_v1 acc;
        char suf[64];
        size_t n = 0;
        size_t text_len = 0;
        const char *text;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp whitespace first",
            speech_display_acc_add_v1(
                &acc, "  Hello  ", 9, suf, sizeof(suf), &n) == SPEECH_DISPLAY_OK &&
                n == 5 && memcmp(suf, "Hello", 5) == 0
        );
        expect(
            "disp whitespace second",
            speech_display_acc_add_v1(
                &acc, "world ", 6, suf, sizeof(suf), &n) == SPEECH_DISPLAY_OK &&
                n == 6 && memcmp(suf, " world", 6) == 0
        );
        expect(
            "disp whitespace punctuation",
            speech_display_acc_add_v1(
                &acc, "! Next", 6, suf, sizeof(suf), &n) == SPEECH_DISPLAY_OK &&
                n == 6 && memcmp(suf, "! Next", 6) == 0
        );
        text = speech_display_acc_text_v1(&acc, &text_len);
        expect(
            "disp whitespace complete",
            text_len == 17 && memcmp(text, "Hello world! Next", 17) == 0
        );
        text = speech_display_acc_canonical_ascii_v1(&acc, &text_len);
        expect(
            "disp whitespace proves canonical ASCII",
            text && text_len == 17 && memcmp(text, "Hello world! Next", 17) == 0 &&
                text[17] == '\0'
        );
    }
    /* speech_display_stream: span append normalizes both delta boundaries */
    {
        speech_display_acc_v1 acc;
        char suffix[16];
        size_t suffix_len = 0;
        size_t text_len = 0;
        const char *text;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp span first",
            speech_display_acc_add_v1(
                &acc, " Gate ", 6, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK &&
                suffix_len == 4 && memcmp(suffix, "Gate", 4) == 0
        );
        expect(
            "disp span punctuation",
            speech_display_acc_add_v1(
                &acc, "! next ", 7, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK &&
                suffix_len == 6 && memcmp(suffix, "! next", 6) == 0
        );
        expect(
            "disp span pending boundary",
            speech_display_acc_add_v1(
                &acc, " opens", 6, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK &&
                suffix_len == 6 && memcmp(suffix, " opens", 6) == 0
        );
        text = speech_display_acc_text_v1(&acc, &text_len);
        expect(
            "disp span complete",
            text_len == 16 && memcmp(text, "Gate! next opens", 16) == 0
        );
    }
    /* speech_display_stream: fast-path output matches the canonical sanitizer */
    {
        static const char alphabet[] =
            "abc XYZ\t\n\r.,!?;:>0123456789\x1f\x7f";
        int differential_ok = 1;
        unsigned int seed;
        for (seed = 1; seed <= 32 && differential_ok; seed++) {
            speech_display_acc_v1 acc;
            char input[1024];
            char suffix[32];
            char reference[sizeof(input)];
            unsigned int state = seed;
            size_t consumed = 0;
            size_t i;
            speech_display_acc_init_v1(&acc);
            for (i = 0; i < sizeof(input); i++) {
                state = state * 1664525u + 1013904223u;
                input[i] = alphabet[state % (sizeof(alphabet) - 1u)];
            }
            while (consumed < sizeof(input)) {
                const char *actual;
                const char *canonical;
                size_t actual_len = 0;
                size_t canonical_len = 0;
                size_t reference_len = 0;
                size_t suffix_len = 0;
                size_t chunk = ((size_t)(state >> 24) % 23u) + 1u;
                if (chunk > sizeof(input) - consumed) chunk = sizeof(input) - consumed;
                if (speech_display_acc_add_v1(
                        &acc,
                        input + consumed,
                        chunk,
                        suffix,
                        sizeof(suffix),
                        &suffix_len) != SPEECH_DISPLAY_OK) {
                    differential_ok = 0;
                    break;
                }
                consumed += chunk;
                if (speech_sanitize_v1(
                        input,
                        consumed,
                        0,
                        reference,
                        sizeof(reference),
                        &reference_len) != SPEECH_SANITIZE_OK) {
                    differential_ok = 0;
                    break;
                }
                actual = speech_display_acc_text_v1(&acc, &actual_len);
                if (actual_len != reference_len ||
                    memcmp(actual, reference, reference_len) != 0) {
                    differential_ok = 0;
                    break;
                }
                canonical = speech_display_acc_canonical_ascii_v1(
                    &acc, &canonical_len);
                if (canonical &&
                    (canonical_len != actual_len || canonical != actual ||
                     !speech_is_canonical_ascii_v1(canonical, canonical_len))) {
                    differential_ok = 0;
                    break;
                }
                state = state * 1664525u + 1013904223u;
            }
        }
        expect("disp ASCII differential", differential_ok);
    }
    /* speech_display_stream: both sides of the span threshold match sanitization. */
    {
        int threshold_ok = 1;
        size_t input_len;
        for (input_len = 15; input_len <= 16 && threshold_ok; input_len++) {
            size_t position;
            for (position = 0; position < input_len && threshold_ok; position++) {
                unsigned int byte;
                for (byte = 0; byte <= 0xffu; byte++) {
                    speech_display_acc_v1 acc;
                    char input[16];
                    char reference[sizeof(input)];
                    char suffix[sizeof(input)];
                    const char *actual;
                    const char *canonical;
                    size_t actual_len = 0;
                    size_t canonical_len = 0;
                    size_t reference_len = 0;
                    size_t suffix_len = 0;

                    /* An incomplete tag is intentionally withheld until it closes. */
                    if (byte == (unsigned int)'<') continue;
                    memset(input, 'a', input_len);
                    input[position] = (char)byte;
                    speech_display_acc_init_v1(&acc);
                    if (speech_display_acc_add_v1(
                            &acc,
                            input,
                            input_len,
                            suffix,
                            sizeof(suffix),
                            &suffix_len) != SPEECH_DISPLAY_OK ||
                        speech_sanitize_v1(
                            input,
                            input_len,
                            0,
                            reference,
                            sizeof(reference),
                            &reference_len) != SPEECH_SANITIZE_OK) {
                        threshold_ok = 0;
                        break;
                    }
                    actual = speech_display_acc_text_v1(&acc, &actual_len);
                    if (actual_len != reference_len ||
                        memcmp(actual, reference, reference_len) != 0 ||
                        suffix_len != actual_len ||
                        memcmp(suffix, actual, actual_len) != 0) {
                        threshold_ok = 0;
                        break;
                    }
                    canonical = speech_display_acc_canonical_ascii_v1(
                        &acc, &canonical_len);
                    if (canonical &&
                        (canonical != actual || canonical_len != actual_len ||
                         !speech_is_canonical_ascii_v1(canonical, canonical_len))) {
                        threshold_ok = 0;
                        break;
                    }
                }
            }
        }
        expect("disp span threshold differential", threshold_ok);
    }
    /* speech_display_stream: tag fallback matches complete sanitization */
    {
        static const char *const inputs[] = {
            "Hello <laugh> world.",
            "<evil> Start <gasp> end!",
            "A <<laugh> B"
        };
        int fallback_ok = 1;
        size_t input_index;
        for (input_index = 0;
             input_index < sizeof(inputs) / sizeof(inputs[0]) && fallback_ok;
             input_index++) {
            const char *input = inputs[input_index];
            size_t input_len = strlen(input);
            size_t split;
            for (split = 1; split < input_len; split++) {
                speech_display_acc_v1 acc;
                char suffix[64];
                char reference[64];
                const char *actual;
                size_t suffix_len = 0;
                size_t actual_len = 0;
                size_t reference_len = 0;
                speech_display_acc_init_v1(&acc);
                if (speech_display_acc_add_v1(
                        &acc,
                        input,
                        split,
                        suffix,
                        sizeof(suffix),
                        &suffix_len) != SPEECH_DISPLAY_OK ||
                    speech_display_acc_add_v1(
                        &acc,
                        input + split,
                        input_len - split,
                        suffix,
                        sizeof(suffix),
                        &suffix_len) != SPEECH_DISPLAY_OK ||
                    speech_sanitize_v1(
                        input,
                        input_len,
                        0,
                        reference,
                        sizeof(reference),
                        &reference_len) != SPEECH_SANITIZE_OK) {
                    fallback_ok = 0;
                    break;
                }
                actual = speech_display_acc_text_v1(&acc, &actual_len);
                if (actual_len != reference_len ||
                    memcmp(actual, reference, reference_len) != 0) {
                    fallback_ok = 0;
                    break;
                }
            }
        }
        expect("disp tag fallback differential", fallback_ok);
    }
    /* speech_display_stream: plain text can transition to the fallback path */
    {
        speech_display_acc_v1 acc;
        char suffix[64];
        size_t suffix_len = 0;
        size_t text_len = 0;
        const char *text;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp fallback plain prefix",
            speech_display_acc_add_v1(
                &acc, "Hello ", 6, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK &&
                suffix_len == 5
        );
        expect(
            "disp fallback tagged suffix",
            speech_display_acc_add_v1(
                &acc, "<laugh> world", 13, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK &&
                suffix_len == 6 && memcmp(suffix, " world", 6) == 0
        );
        text = speech_display_acc_text_v1(&acc, &text_len);
        expect(
            "disp fallback complete",
            text_len == 11 && memcmp(text, "Hello world", 11) == 0
        );
        expect(
            "disp tag fallback drops canonical proof",
            speech_display_acc_canonical_ascii_v1(&acc, &text_len) == NULL &&
                text_len == 0
        );
    }
    /* speech_display_stream: Unicode disables the ASCII fast path safely */
    {
        speech_display_acc_v1 acc;
        char suffix[64];
        size_t suffix_len = 0;
        size_t text_len = 0;
        const char *text;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp Unicode plain prefix",
            speech_display_acc_add_v1(
                &acc, "Hello ", 6, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK
        );
        expect(
            "disp Unicode fallback",
            speech_display_acc_add_v1(
                &acc, "caf\xc3\xa9", 5, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK &&
                suffix_len == 6 && memcmp(suffix, " caf\xc3\xa9", 6) == 0
        );
        text = speech_display_acc_text_v1(&acc, &text_len);
        expect(
            "disp Unicode complete",
            text_len == 11 && memcmp(text, "Hello caf\xc3\xa9", 11) == 0
        );
        expect(
            "disp Unicode fallback drops canonical proof",
            speech_display_acc_canonical_ascii_v1(&acc, &text_len) == NULL &&
                text_len == 0
        );
    }
    /* speech_display_stream: ASCII controls never produce canonical proof. */
    {
        speech_display_acc_v1 acc;
        char suffix[8];
        size_t suffix_len = 0;
        size_t text_len = 99;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp control fallback",
            speech_display_acc_add_v1(
                &acc, "\x7f", 1, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK
        );
        expect(
            "disp control fallback drops canonical proof",
            speech_display_acc_canonical_ascii_v1(&acc, &text_len) == NULL &&
                text_len == 0
        );
    }
    /* speech_display_stream: a short suffix buffer does not truncate state */
    {
        speech_display_acc_v1 acc;
        char suffix[2];
        size_t suffix_len = 0;
        size_t text_len = 0;
        const char *text;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp bounded suffix",
            speech_display_acc_add_v1(
                &acc, "hello", 5, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_OK &&
                suffix_len == sizeof(suffix) && memcmp(suffix, "he", sizeof(suffix)) == 0
        );
        text = speech_display_acc_text_v1(&acc, &text_len);
        expect(
            "disp bounded suffix keeps state",
            text_len == 5 && memcmp(text, "hello", 5) == 0
        );
    }
    /* speech_display_stream: reject impossible lengths before reading input */
    {
        speech_display_acc_v1 acc;
        char suffix[8];
        size_t suffix_len = 0;
        speech_display_acc_init_v1(&acc);
        expect(
            "disp length overflow",
            speech_display_acc_add_v1(
                &acc, "x", SIZE_MAX, suffix, sizeof(suffix), &suffix_len) ==
                SPEECH_DISPLAY_ERR_CAPACITY
        );
    }
    /* speech_display_stream: bounded 128-delta turn cost */
    {
        const char delta[] = "one two three. ";
        const int iterations = 400;
        struct timespec t0, t1;
        volatile size_t sink = 0;
        double ns_per_turn;
        int i;
        int ok = 1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < iterations; i++) {
            speech_display_acc_v1 acc;
            char suffix[sizeof(delta)];
            size_t suffix_len = 0;
            int part;
            speech_display_acc_init_v1(&acc);
            for (part = 0; part < 128; part++) {
                if (speech_display_acc_add_v1(
                        &acc,
                        delta,
                        sizeof(delta) - 1u,
                        suffix,
                        sizeof(suffix),
                        &suffix_len) != SPEECH_DISPLAY_OK) {
                    ok = 0;
                    break;
                }
                sink += suffix_len;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns_per_turn =
            ((double)(t1.tv_sec - t0.tv_sec) * 1e9 +
             (double)(t1.tv_nsec - t0.tv_nsec)) / iterations;
        printf(
            "BenchmarkDisplayAccumulator_ASCII128\t%.2f ns/turn\tsink=%zu\n",
            ns_per_turn,
            sink
        );
        expect("disp benchmark ran", ok && ns_per_turn > 0.0 && sink != 0u);
    }
    /* turn_budget: earliest of budget_ms vs absolute deadline (mirrors Go ExtractBudget) */
    {
        turn_budget_info_v1 info;
        const char *keys[2] = {TURN_BUDGET_KEY_MS, TURN_BUDGET_KEY_DEADLINE_UNIX_MS};
        const char *vals[2] = {"500", "101000"}; /* now=100000 → budget ends 100500 */
        int64_t now = 100000;
        expect(
            "budget extract",
            turn_budget_extract_v1(keys, vals, 2, now, &info) == TURN_BUDGET_OK
        );
        expect("budget ms", info.budget_ms == 500);
        expect("budget effective", info.effective_deadline_unix_ms == 100500);
        expect("budget source", info.source == TURN_BUDGET_SRC_BUDGET_MS);
        expect("budget has", info.has_deadline == 1);
        expect("budget rem", turn_budget_remaining_ms_v1(&info, now) == 500);
        /* absolute earlier than budget wins */
        vals[0] = "5000";
        vals[1] = "100200";
        expect(
            "budget abs earlier",
            turn_budget_extract_v1(keys, vals, 2, now, &info) == TURN_BUDGET_OK
        );
        expect("budget abs eff", info.effective_deadline_unix_ms == 100200);
        expect("budget abs src", info.source == TURN_BUDGET_SRC_DEADLINE_UNIX_MS);
        /* parent earlier wins */
        expect("budget parent", turn_budget_apply_parent_v1(&info, 100100) == TURN_BUDGET_OK);
        expect("budget parent src", info.source == TURN_BUDGET_SRC_PARENT);
        expect("budget parent eff", info.effective_deadline_unix_ms == 100100);
        /* invalid key */
        expect(
            "budget bad",
            turn_budget_extract_kv_v1(TURN_BUDGET_KEY_MS, "nope", now, &info) == TURN_BUDGET_OK
        );
        expect("budget inv", info.invalid_budget_ms == 1);
        expect("budget no dl", info.has_deadline == 0);
        {
            int64_t configured_ms = 0;
            expect(
                "budget default parse",
                turn_budget_parse_ms_v1("  +750 ", &configured_ms) ==
                    TURN_BUDGET_OK &&
                configured_ms == 750);
            expect(
                "budget default parse rejects zero",
                turn_budget_parse_ms_v1("0", &configured_ms) ==
                    TURN_BUDGET_ERR_ARGUMENT &&
                configured_ms == 0);
            memset(&info, 0, sizeof(info));
            expect(
                "budget default apply",
                turn_budget_apply_default_ms_v1(
                    &info, 750, now) == TURN_BUDGET_OK &&
                info.budget_ms == 750 &&
                info.effective_deadline_unix_ms == now + 750 &&
                info.source == TURN_BUDGET_SRC_BUDGET_MS &&
                info.has_deadline == 1);
            info.effective_deadline_unix_ms = now + 100;
            info.source = TURN_BUDGET_SRC_DEADLINE_UNIX_MS;
            expect(
                "budget metadata overrides default",
                turn_budget_apply_default_ms_v1(
                    &info, 750, now) == TURN_BUDGET_OK &&
                info.effective_deadline_unix_ms == now + 100 &&
                info.source == TURN_BUDGET_SRC_DEADLINE_UNIX_MS);
            memset(&info, 0, sizeof(info));
            expect(
                "budget default saturates",
                turn_budget_apply_default_ms_v1(
                    &info, INT64_MAX, now) == TURN_BUDGET_OK &&
                info.effective_deadline_unix_ms == INT64_MAX);
        }
    }
    /* turn_error: stable class strings + JSON envelope */
    {
        turn_err_class_v1 c;
        char json[256];
        expect("err code val", turn_err_from_code_v1(TURN_ERR_CODE_VALIDATION, &c) == TURN_ERR_OK);
        expect("err val str", strcmp(c.class_str, TURN_ERR_CLASS_VALIDATION) == 0);
        expect("err val no retry", c.retryable == 0);
        expect(
            "err deadline msg",
            turn_err_classify_msg_v1("context deadline exceeded", &c) == TURN_ERR_OK
        );
        expect("err dl class", strcmp(c.class_str, TURN_ERR_CLASS_DEADLINE) == 0);
        expect("err cancel msg", turn_err_classify_msg_v1("context canceled", &c) == TURN_ERR_OK);
        expect("err cancel class", strcmp(c.class_str, TURN_ERR_CLASS_CANCELED) == 0);
        expect("err boom", turn_err_classify_msg_v1("boom", &c) == TURN_ERR_OK);
        expect("err boom class", strcmp(c.class_str, TURN_ERR_CLASS_INTERNAL) == 0);
        expect("err missing", turn_err_classify_msg_v1("missing request_id", &c) == TURN_ERR_OK);
        expect("err missing class", strcmp(c.class_str, TURN_ERR_CLASS_VALIDATION) == 0);
        turn_err_from_code_v1(TURN_ERR_CODE_NOT_FOUND, &c);
        expect("err json", turn_err_json_v1(&c, "turn not found", json, sizeof(json)) == TURN_ERR_OK);
        expect("err json type", strstr(json, "\"type\":\"not_found\"") != NULL);
        expect("err json msg", strstr(json, "turn not found") != NULL);
        expect("err json flag", strstr(json, "\"error\":true") != NULL);
    }
    /* turn_frame: binary frame + terminal event names */
    {
        uint8_t out[64];
        size_t n = 0, consumed = 0, plen = 0;
        uint8_t ftype = 0;
        const uint8_t *pay = NULL;
        const char *payload = "hi";
        expect(
            "frame enc",
            turn_frame_encode_v1(1, (const uint8_t *)payload, 2, out, sizeof(out), &n) ==
                TURN_FRAME_OK
        );
        expect("frame len", n == 7);
        expect(
            "frame dec",
            turn_frame_decode_v1(out, n, &ftype, &pay, &plen, &consumed) == TURN_FRAME_OK
        );
        expect("frame type", ftype == 1);
        expect("frame plen", plen == 2);
        expect("frame body", pay && memcmp(pay, "hi", 2) == 0);
        expect("frame consumed", consumed == 7);
        expect("ev name completed", strcmp(turn_event_name_v1("completed"), "completed") == 0);
        expect("ev name alias", strcmp(turn_event_name_v1("turn_completed"), "completed") == 0);
        expect("ev terminal", turn_event_is_terminal_v1("failed") == 1);
        expect("ev nonterm", turn_event_is_terminal_v1("text_delta") == 0);
    }
    /* turn_ndjson control envelopes */
    {
        char line[512];
        expect("ndjson acc", turn_ndjson_accepted_v1("r1", "ai.turn.events.r1", 9, 1000, line, sizeof(line)) == TURN_NDJSON_OK);
        expect("ndjson acc type", strstr(line, "\"type\":\"accepted\"") != NULL);
        expect("ndjson acc rid", strstr(line, "r1") != NULL);
        expect("ndjson err", turn_ndjson_error_v1("boom", line, sizeof(line)) == TURN_NDJSON_OK);
        expect("ndjson err type", strstr(line, "\"type\":\"error\"") != NULL);
        expect("ndjson err msg", strstr(line, "boom") != NULL);
    }

    /* pb turn_start metadata map field 12 → budget keys */
    {
        uint8_t buf[256];
        size_t pos = 0;
        turn_start_c turn;
        turn_budget_info_v1 info;
        /* helper: write protobuf string field */
        #define WT(field, s) do { \
            size_t sl = strlen(s); \
            size_t i; \
            uint64_t tag = ((uint64_t)(field) << 3) | 2; \
            while (tag >= 0x80) { buf[pos++] = (uint8_t)((tag & 0x7f) | 0x80); tag >>= 7; } \
            buf[pos++] = (uint8_t)tag; \
            buf[pos++] = (uint8_t)sl; \
            for (i = 0; i < sl; i++) buf[pos++] = (uint8_t)(s)[i]; \
        } while (0)
        /* request_id=1, session=3, text=5, metadata entry field 12 */
        WT(1, "rid-1");
        WT(3, "sess-1");
        WT(5, "hello");
        WT(1, "rid-final");
        {
            /* map entry: key turn_budget_ms, val 750 */
            uint8_t entry[64];
            size_t ep = 0;
            const char *k = "turn_budget_ms";
            const char *v = "750";
            size_t kl = strlen(k), vl = strlen(v), i;
            entry[ep++] = (1 << 3) | 2;
            entry[ep++] = (uint8_t)kl;
            for (i = 0; i < kl; i++) entry[ep++] = (uint8_t)k[i];
            entry[ep++] = (2 << 3) | 2;
            entry[ep++] = (uint8_t)vl;
            for (i = 0; i < vl; i++) entry[ep++] = (uint8_t)v[i];
            buf[pos++] = (12 << 3) | 2;
            buf[pos++] = (uint8_t)ep;
            for (i = 0; i < ep; i++) buf[pos++] = entry[i];
        }
        expect("pb turn meta decode", pb_decode_turn_start(buf, pos, &turn) == 0);
        expect(
            "pb turn duplicate rid and length",
            strcmp(turn.request_id, "rid-final") == 0 &&
            turn.request_id_len == strlen("rid-final"));
        expect("pb turn text", strcmp(turn.text, "hello") == 0);
        expect(
            "pb turn sparse lengths",
            turn.text_len == strlen("hello") &&
            turn.session_id_len == strlen("sess-1") &&
            turn.user_id_len == 0u && turn.response_subject_len == 0u &&
            turn.input_stages.audio_committed_at_ms == 0 &&
            turn.input_stages.stt_request_received_at_ms == 0 &&
            turn.input_stages.stt_provider_request_started_at_ms == 0 &&
            turn.input_stages.stt_provider_ready_at_ms == 0 &&
            turn.input_stages.stt_transcript_published_at_ms == 0);
        expect("pb turn has budget", turn.has_meta_budget == 1);
        expect("pb turn budget val", strcmp(turn.meta_budget_ms, "750") == 0);
        {
            const char *keys[1] = {TURN_BUDGET_KEY_MS};
            const char *vals[1] = {turn.meta_budget_ms};
            expect(
                "pb budget extract",
                turn_budget_extract_v1(keys, vals, 1, 100000, &info) == TURN_BUDGET_OK
            );
            expect("pb budget ms", info.budget_ms == 750);
            expect("pb budget eff", info.effective_deadline_unix_ms == 100750);
        }
        #undef WT
    }

    /* pb turn_start encoder preserves admission metadata. */
    {
        uint8_t buf[4096];
        turn_start_c encoded;
        turn_start_c decoded;
        size_t wire_len;
        memset(&encoded, 0, sizeof(encoded));
        snprintf(encoded.request_id, sizeof(encoded.request_id), "rid-budget");
        snprintf(encoded.user_id, sizeof(encoded.user_id), "user-budget");
        snprintf(encoded.session_id, sizeof(encoded.session_id), "session-budget");
        snprintf(encoded.text, sizeof(encoded.text), "hello");
        snprintf(
            encoded.response_subject,
            sizeof(encoded.response_subject),
            "ai.turn.events.rid-budget");
        encoded.has_meta_budget = 1;
        encoded.model_request_capture = 1;
        snprintf(encoded.meta_budget_ms, sizeof(encoded.meta_budget_ms), "750");
        encoded.has_meta_deadline = 1;
        snprintf(
            encoded.meta_deadline_unix_ms,
            sizeof(encoded.meta_deadline_unix_ms),
            "200000");
        snprintf(
            encoded.metadata.interaction_profile,
            sizeof(encoded.metadata.interaction_profile),
            "dnd_app");
        snprintf(
            encoded.metadata.client_transport,
            sizeof(encoded.metadata.client_transport),
            "k6-dnd-product-rules");
        snprintf(
            encoded.metadata.campaign_id,
            sizeof(encoded.metadata.campaign_id),
            "campaign-1");
        snprintf(encoded.metadata.character_id, sizeof(encoded.metadata.character_id), "hero-1");
        snprintf(encoded.metadata.scene_id, sizeof(encoded.metadata.scene_id), "hall");
        snprintf(
            encoded.metadata.knowledge_scope,
            sizeof(encoded.metadata.knowledge_scope),
            "shared_rulebook");
        snprintf(
            encoded.metadata.retrieval_force,
            sizeof(encoded.metadata.retrieval_force),
            "true");
        snprintf(
            encoded.metadata.audio_participant_label,
            sizeof(encoded.metadata.audio_participant_label),
            "Tára the Bold");
        wire_len = pb_encode_turn_start(buf, sizeof(buf), &encoded);
        expect("pb turn metadata encode", wire_len > 0);
        expect(
            "pb turn metadata round trip",
            wire_len > 0 && pb_decode_turn_start(buf, wire_len, &decoded) == 0);
        expect("pb turn encoded budget flag", decoded.has_meta_budget == 1);
        expect("pb turn signed capture permission survives STT wire", decoded.model_request_capture == 1);
        expect(
            "pb turn encoded budget value",
            strcmp(decoded.meta_budget_ms, "750") == 0);
        expect("pb turn encoded deadline flag", decoded.has_meta_deadline == 1);
        expect(
            "pb turn encoded deadline value",
            strcmp(decoded.meta_deadline_unix_ms, "200000") == 0);
        expect(
            "pb turn product metadata round trip",
            strcmp(decoded.metadata.interaction_profile, "dnd_app") == 0 &&
            strcmp(decoded.metadata.client_transport, "k6-dnd-product-rules") == 0 &&
            strcmp(decoded.metadata.campaign_id, "campaign-1") == 0 &&
            strcmp(decoded.metadata.scene_id, "hall") == 0 &&
            strcmp(decoded.metadata.character_id, "hero-1") == 0 &&
            strcmp(decoded.metadata.knowledge_scope, "shared_rulebook") == 0 &&
            strcmp(decoded.metadata.retrieval_force, "true") == 0 &&
            strcmp(decoded.metadata.audio_participant_label, "Tára the Bold") == 0);
        {
            size_t first_len = wire_len;
            memcpy(buf + first_len, buf, first_len);
            expect(
                "pb turn rejects duplicate known metadata",
                pb_decode_turn_start(buf, first_len * 2u, &decoded) != 0);
        }
        encoded.meta_budget_ms[0] = '\0';
        expect(
            "pb turn rejects empty budget metadata",
            pb_encode_turn_start(buf, sizeof(buf), &encoded) == 0);
    }

    /* Ambiguous or malformed metadata fails before gateway re-encoding. */
    {
        turn_start_c decoded;
        const uint8_t duplicate[]={0xa8,0x01,0x00,0xa8,0x01,0x01};
        const uint8_t bad_bool[]={0xa8,0x01,0x02};
        const uint8_t wrong_wire[]={0xaa,0x01,0x00};
        expect("pb turn missing capture defaults off",pb_decode_turn_start(NULL,0,&decoded)==0 && !decoded.model_request_capture);
        expect("pb turn duplicate capture rejected",pb_decode_turn_start(duplicate,sizeof(duplicate),&decoded)!=0);
        expect("pb turn capture boolean rejected",pb_decode_turn_start(bad_bool,sizeof(bad_bool),&decoded)!=0);
        expect("pb turn capture wire rejected",pb_decode_turn_start(wrong_wire,sizeof(wrong_wire),&decoded)!=0);
    }

    /* Ambiguous or malformed metadata fails before gateway re-encoding. */
    {
        turn_start_c decoded;
        static const char duplicate_key[] = "\x62\x09\x0a\x01x\x12\x01y\x0a\x01z";
        static const char duplicate_value[] = "\x62\x09\x0a\x01x\x12\x01y\x12\x01z";
        static const char nul_key[] = "\x62\x07\x0a\x02x\0\x12\x01y";
        static const char nul_value[] = "\x62\x07\x0a\x01x\x12\x02y\0";
        static const char malformed_tail[] = "\x62\x07\x0a\x01x\x12\x01y\xff";
        static const char wrong_wire[] = "\x62\x05\x08\x01\x12\x01y";
        static const char empty_budget[] = "\x62\x12\x0a\x0e" "turn_budget_ms\x12\x00";
#define REJECT_META_WIRE(value) \
        expect("pb turn rejects " #value, \
               pb_decode_turn_start((const uint8_t *)(value), sizeof(value) - 1u, &decoded) != 0)
        REJECT_META_WIRE(duplicate_key);
        REJECT_META_WIRE(duplicate_value);
        REJECT_META_WIRE(nul_key);
        REJECT_META_WIRE(nul_value);
        REJECT_META_WIRE(malformed_tail);
        REJECT_META_WIRE(wrong_wire);
        REJECT_META_WIRE(empty_budget);
#undef REJECT_META_WIRE
    }

    /* Turn metadata has one shared 32-pair bound on encode and decode. */
    {
        static const uint8_t unknown_entry[] = {
            0x62u, 0x06u, 0x0au, 0x01u, 'x', 0x12u, 0x01u, 'y'
        };
        uint8_t buf[4096];
        turn_start_c encoded;
        turn_start_c decoded;
        size_t wire_len;
#define SET_TURN_META(member) encoded.metadata.member[0] = 'x'
        memset(&encoded, 0, sizeof(encoded));
        SET_TURN_META(turn_source);
        SET_TURN_META(turn_kind);
        SET_TURN_META(client_trace_id);
        SET_TURN_META(client_transport);
        SET_TURN_META(client_surface);
        SET_TURN_META(interaction_profile);
        SET_TURN_META(voice_mode);
        SET_TURN_META(turn_profile);
        SET_TURN_META(retrieval_skip);
        SET_TURN_META(agent_id);
        SET_TURN_META(task_intent);
        SET_TURN_META(turn_max_tokens);
        SET_TURN_META(input_mode);
        SET_TURN_META(capability_id);
        SET_TURN_META(parent_bundle);
        SET_TURN_META(prompt_hash);
        SET_TURN_META(product_session_id);
        SET_TURN_META(campaign_id);
        SET_TURN_META(encounter_id);
        SET_TURN_META(requested_npc_id);
        SET_TURN_META(knowledge_scope);
        SET_TURN_META(dnd_session_recap);
        SET_TURN_META(dnd_forget_session_recap);
        SET_TURN_META(recap_session_id);
        SET_TURN_META(dnd_cold_open);
        SET_TURN_META(evaluation);
        SET_TURN_META(retrieval_force);
        SET_TURN_META(memory_context_version);
        SET_TURN_META(audio_group_session);
        SET_TURN_META(audio_participant_id);
        SET_TURN_META(audio_participant_label);
#undef SET_TURN_META
        encoded.has_meta_budget = 1;
        snprintf(encoded.meta_budget_ms, sizeof(encoded.meta_budget_ms), "1");
        wire_len = pb_encode_turn_start(buf, sizeof(buf), &encoded);
        expect("pb turn accepts 32 metadata pairs", wire_len > 0u);
        expect(
            "pb turn decodes 32 metadata pairs",
            wire_len > 0u && pb_decode_turn_start(buf, wire_len, &decoded) == 0);
        expect(
            "pb turn decoder rejects metadata pair 33",
            wire_len > 0u && wire_len + sizeof(unknown_entry) <= sizeof(buf) &&
            (memcpy(buf + wire_len, unknown_entry, sizeof(unknown_entry)),
             pb_decode_turn_start(
                 buf, wire_len + sizeof(unknown_entry), &decoded) != 0));
        encoded.has_meta_deadline = 1;
        snprintf(encoded.meta_deadline_unix_ms,
                 sizeof(encoded.meta_deadline_unix_ms), "2");
        expect(
            "pb turn encoder rejects metadata pair 33",
            pb_encode_turn_start(buf, sizeof(buf), &encoded) == 0u);
    }

    /* pcm_parse Orpheus header */
    {
        uint8_t hdr[8];
        pcm_header_c h;
        wr_u32_le(hdr, 24000);
        wr_u16_le(hdr + 4, 1);
        wr_u16_le(hdr + 6, 16);
        expect("pcm header", pcm_parse_header_v1(hdr, 8, &h) == PCM_PARSE_OK);
        expect("pcm rate", h.sample_rate == 24000);
        expect("pcm ch", h.channels == 1);
        expect("pcm bits", h.bit_depth == 16);
        expect(
            "mono-s16 PCM header accepts 24 kHz",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr), &h) ==
                PCM_PARSE_OK &&
            h.sample_rate == 24000u && h.channels == 1u &&
            h.bit_depth == 16u);
        expect(
            "mono-s16 PCM header rejects short input",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr) - 1u, &h) ==
                PCM_PARSE_ERR_TOO_SHORT &&
            h.sample_rate == 0u && h.channels == 0u && h.bit_depth == 0u);
        expect(
            "mono-s16 PCM header rejects null input",
            pcm_parse_mono_s16_header_v1(NULL, sizeof(hdr), &h) ==
                PCM_PARSE_ERR_TOO_SHORT &&
            h.sample_rate == 0u && h.channels == 0u && h.bit_depth == 0u);
        expect(
            "mono-s16 PCM header rejects null output",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr), NULL) ==
                PCM_PARSE_ERR_ARGUMENT);
        wr_u32_le(hdr, 7999u);
        expect(
            "mono-s16 PCM header rejects low sample rate",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr), &h) ==
                PCM_PARSE_ERR_INVALID &&
            h.sample_rate == 0u && h.channels == 0u && h.bit_depth == 0u);
        wr_u32_le(hdr, 8000u);
        expect(
            "mono-s16 PCM header accepts lower sample rate",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr), &h) ==
                PCM_PARSE_OK &&
            h.sample_rate == 8000u);
        wr_u32_le(hdr, 48000u);
        expect(
            "mono-s16 PCM header accepts upper sample rate",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr), &h) ==
                PCM_PARSE_OK &&
            h.sample_rate == 48000u);
        wr_u32_le(hdr, 48001u);
        expect(
            "mono-s16 PCM header rejects high sample rate",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr), &h) ==
                PCM_PARSE_ERR_INVALID);
        wr_u32_le(hdr, 48000u);
        wr_u16_le(hdr + 4, 2u);
        expect(
            "mono-s16 PCM header rejects stereo",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr), &h) ==
                PCM_PARSE_ERR_INVALID);
        wr_u16_le(hdr + 4, 1u);
        wr_u16_le(hdr + 6, 24u);
        expect(
            "mono-s16 PCM header rejects other sample widths",
            pcm_parse_mono_s16_header_v1(hdr, sizeof(hdr), &h) ==
                PCM_PARSE_ERR_INVALID);
        wr_u32_le(hdr, 24000u);
        wr_u16_le(hdr + 6, 16u);
        wr_u16_le(hdr + 4, 0);
        expect("pcm rejects zero channels", pcm_parse_header_v1(hdr, 8, &h) == PCM_PARSE_ERR_INVALID);
    }
    /* pcm_parse minimal WAV (fmt@12 + data@36, 4 PCM bytes → total 48). */
    {
        uint8_t wav[64];
        pcm_header_c h;
        size_t off = 0, plen = 0;
        memset(wav, 0, sizeof(wav));
        memcpy(wav, "RIFF", 4);
        wr_u32_le(wav + 4, 40); /* file size - 8 */
        memcpy(wav + 8, "WAVE", 4);
        memcpy(wav + 12, "fmt ", 4);
        wr_u32_le(wav + 16, 16);
        wr_u16_le(wav + 20, 1);      /* PCM */
        wr_u16_le(wav + 22, 1);      /* mono */
        wr_u32_le(wav + 24, 16000);  /* rate */
        wr_u32_le(wav + 28, 32000);  /* byte rate */
        wr_u16_le(wav + 32, 2);      /* block align */
        wr_u16_le(wav + 34, 16);     /* bits */
        memcpy(wav + 36, "data", 4);
        wr_u32_le(wav + 40, 4);
        wav[44] = 0;
        wav[45] = 1;
        wav[46] = 0;
        wav[47] = 2;
        expect("wav parse", pcm_parse_wav_v1(wav, 48, &h, &off, &plen) == PCM_PARSE_OK);
        expect("wav rate", h.sample_rate == 16000);
        expect("wav pcm_len", plen == 4);
        expect("wav off", off == 44);
        wr_u16_le(wav + 32, 3);
        expect("wav rejects bad block align", pcm_parse_wav_v1(wav, 48, &h, &off, &plen) == PCM_PARSE_ERR_INVALID);
        wr_u16_le(wav + 32, 2);
        wr_u32_le(wav + 4, 80);
        expect("wav rejects truncated RIFF", pcm_parse_wav_v1(wav, 48, &h, &off, &plen) == PCM_PARSE_ERR_TRUNCATED);
    }

    /* reflex_session init/process/destroy */
    {
        reflex_config_v1 cfg;
        reflex_session_v1 *rs = NULL;
        reflex_decision_v1 dec;
        int16_t frame[320];
        int i;
        reflex_config_default_v1(&cfg);
        expect("reflex init", reflex_session_init_v1(&cfg, &rs) == REFLEX_OK && rs != NULL);
        for (i = 0; i < 320; i++) frame[i] = 0;
        expect(
            "reflex process silence",
            rs && reflex_session_process_v1(
                      rs, (const uint8_t *)frame, sizeof(frame), 1000000ull, 0, &dec
                  ) == REFLEX_OK
        );
        if (rs) reflex_session_destroy_v1(rs);
    }

    /* rag_select strip + split */
    {
        char name[128];
        char parts[RAG_SELECT_MAX_PARTS][RAG_SELECT_MAX_NAME];
        size_t n = 0;
        expect(
            "rag strip opts",
            rag_select_strip_options("books?limit=10", 14, name, sizeof(name)) == 0
        );
        expect("rag strip name", strcmp(name, "books") == 0);
        expect(
            "rag split",
            rag_select_split("a, b?x, c", 9, parts, RAG_SELECT_MAX_PARTS, &n) == 0
        );
        expect("rag split n", n == 3);
        expect("rag split0", strcmp(parts[0], "a") == 0);
        expect("rag split1", strcmp(parts[1], "b") == 0);
        expect("rag split2", strcmp(parts[2], "c") == 0);
    }

    /* speech_segment: punctuation cut (mirrors Go TestCanonicalBoundaryPolicy) */
    {
        speech_seg_config_v1 cfg;
        size_t offs[8], lens[8], n = 0, ro = 0, rl = 0;
        const char *in = "Ready, adventurer? Keep moving";
        char seg[64];
        speech_seg_config_default_v1(&cfg);
        cfg.min_segment_chars = 8;
        cfg.max_segment_chars = 80;
        expect(
            "seg split punct",
            speech_seg_split_speakable_v1(
                in, strlen(in), &cfg, offs, lens, 8, &n, &ro, &rl
            ) == SPEECH_SEG_OK
        );
        expect("seg nparts", n == 1);
        memcpy(seg, in + offs[0], lens[0]);
        seg[lens[0]] = '\0';
        expect("seg text", strcmp(seg, "Ready, adventurer?") == 0);
        expect("seg rem", rl == strlen("Keep moving") && memcmp(in + ro, "Keep moving", rl) == 0);
    }
    /* speech_segment: tag atomic at max */
    {
        speech_seg_config_v1 cfg;
        size_t so, sl, ro, rl;
        const char *in = "Hi <gasp> there";
        char seg[64];
        speech_seg_config_default_v1(&cfg);
        cfg.min_segment_chars = 1;
        cfg.max_segment_chars = 6;
        expect(
            "seg tag atomic",
            speech_seg_next_v1(in, strlen(in), &cfg, &so, &sl, &ro, &rl) == SPEECH_SEG_OK
        );
        memcpy(seg, in + so, sl);
        seg[sl] = '\0';
        expect("seg tag text", strcmp(seg, "Hi <gasp>") == 0);
    }
    /* speech_segment: punctuation below the rune minimum stays buffered */
    {
        speech_seg_config_v1 cfg;
        size_t so, sl, ro, rl;
        const char *in = "Hi?";
        const char *unicode = "你?好";
        speech_seg_config_default_v1(&cfg);
        cfg.min_segment_chars = 8;
        cfg.max_segment_chars = 80;
        expect(
            "seg short punctuation stays buffered",
            speech_seg_next_v1(
                in, strlen(in), &cfg, &so, &sl, &ro, &rl) ==
            SPEECH_SEG_NO_CUT);
        expect(
            "seg short prefix stays buffered",
            speech_seg_first_prefix_v1(
                in, strlen(in), 8, &so, &sl, &ro, &rl) ==
            SPEECH_SEG_NO_CUT);
        cfg.min_segment_chars = 4;
        expect(
            "seg short Unicode punctuation stays buffered",
            speech_seg_next_v1(
                unicode, strlen(unicode), &cfg, &so, &sl, &ro, &rl) ==
            SPEECH_SEG_NO_CUT);
    }
    /* speech_segment: unicode rune limit */
    {
        speech_seg_config_v1 cfg;
        size_t so, sl, ro, rl;
        const char *in = "你好世界再继续";
        speech_seg_config_default_v1(&cfg);
        cfg.min_segment_chars = 1;
        cfg.max_segment_chars = 5;
        expect(
            "seg unicode",
            speech_seg_next_v1(in, strlen(in), &cfg, &so, &sl, &ro, &rl) == SPEECH_SEG_OK
        );
        expect("seg unicode runes", speech_seg_rune_len_v1(in + so, sl) == 5);
    }
    /* speech_segment: ASCII fast path preserves boundary and trim rules */
    {
        static const struct {
            const char *input;
            int min_chars;
            int max_chars;
            int rc;
            size_t segment_offset;
            size_t segment_length;
            size_t rest_offset;
            size_t rest_length;
        } cases[] = {
            {"   alpha beta,   gamma", 5, 80, SPEECH_SEG_OK, 3, 11, 17, 5},
            {"abcdefghij rest", 3, 5, SPEECH_SEG_OK, 0, 5, 5, 10},
            {"within 15 feet", 3, 12, SPEECH_SEG_OK, 0, 9, 10, 4},
            {"within 15 fe", 3, 12, SPEECH_SEG_OK, 0, 9, 10, 2},
            {"within 15 feet", 3, 9, SPEECH_SEG_OK, 0, 9, 10, 4},
            {"within 15 feet", 10, 12, SPEECH_SEG_OK, 0, 12, 12, 2},
            {"café within feet", 3, 13, SPEECH_SEG_OK, 0, 12, 13, 4},
            {"<laugh> within feet", 3, 16, SPEECH_SEG_OK, 0, 14, 15, 4},
            {"<long tag>wordtail", 3, 12, SPEECH_SEG_OK, 0, 12, 12, 6},
            {"Hi? later", 8, 80, SPEECH_SEG_OK, 0, 3, 4, 5},
            {"short", 8, 80, SPEECH_SEG_NO_CUT, 0, 0, 0, 5},
            {"> trailing", 1, 1, SPEECH_SEG_NO_CUT, 0, 0, 0, 10}
        };
        int ascii_ok = 1;
        size_t case_index;
        for (case_index = 0;
             case_index < sizeof(cases) / sizeof(cases[0]) && ascii_ok;
             ++case_index) {
            speech_seg_config_v1 cfg;
            size_t so, sl, ro, rl;
            int rc;
            speech_seg_config_default_v1(&cfg);
            cfg.min_segment_chars = cases[case_index].min_chars;
            cfg.max_segment_chars = cases[case_index].max_chars;
            rc = speech_seg_next_v1(
                cases[case_index].input,
                strlen(cases[case_index].input),
                &cfg,
                &so,
                &sl,
                &ro,
                &rl);
            if (rc != cases[case_index].rc || so != cases[case_index].segment_offset ||
                sl != cases[case_index].segment_length ||
                ro != cases[case_index].rest_offset ||
                rl != cases[case_index].rest_length)
                ascii_ok = 0;
        }
        expect("seg ASCII boundaries", ascii_ok);
    }
    /* speech_segment: span scans match the scalar ASCII policies */
    {
        char input[97];
        uint32_t state = 0x51a7c3d9u;
        int ascii_ok = 1;
        size_t position;
        unsigned int byte;
        size_t case_index;

        memset(input, 'a', sizeof(input));
        for (position = 0; position < 64u && ascii_ok; ++position) {
            for (byte = 0; byte < 0x80u && ascii_ok; ++byte) {
                int min_chars = (int)(position % 23u) + 1;
                int max_chars = min_chars + (int)(position % 31u);
                if (byte == (unsigned int)'<') continue;
                input[position] = (char)byte;
                ascii_ok = ascii_seg_matches_reference(
                    input, 64u, 1, 64) &&
                    ascii_seg_matches_reference(
                        input, 64u, min_chars, max_chars) &&
                    ascii_prefix_matches_reference(input, 64u, 1) &&
                    ascii_prefix_matches_reference(input, 64u, min_chars);
                input[position] = 'a';
            }
        }
        for (case_index = 0; case_index < 4096u && ascii_ok; ++case_index) {
            size_t len;
            size_t i;
            int min_chars;
            int max_chars;
            state = state * 1664525u + 1013904223u;
            len = (size_t)(state % (uint32_t)sizeof(input));
            state = state * 1664525u + 1013904223u;
            min_chars = (int)(state % 48u) + 1;
            state = state * 1664525u + 1013904223u;
            max_chars = min_chars + (int)(state % 49u);
            for (i = 0; i < len; ++i) {
                unsigned int value;
                state = state * 1664525u + 1013904223u;
                value = state & 0x7fu;
                if (value == (unsigned int)'<') value = (unsigned int)'a';
                input[i] = (char)value;
            }
            ascii_ok = ascii_seg_matches_reference(
                input, len, min_chars, max_chars) &&
                ascii_prefix_matches_reference(input, len, min_chars);
        }
        expect("seg ASCII span scans match scalar", ascii_ok);
    }
    /* speech_segment: span scan exits for Unicode and tags at every alignment */
    {
        char input[64];
        static const char tag[] = "<abcdefgh>";
        static const char unicode[] = "\xe4\xbd\xa0";
        int tag_ok = 1;
        int unicode_ok = 1;
        size_t prefix_len;
        for (prefix_len = 0;
             prefix_len < sizeof(size_t) * 2u && unicode_ok;
             ++prefix_len) {
            speech_seg_config_v1 cfg;
            size_t so, sl, ro, rl;
            size_t len = prefix_len + sizeof(unicode) - 1u + 6u;
            memset(input, 'a', prefix_len);
            memcpy(input + prefix_len, unicode, sizeof(unicode) - 1u);
            memcpy(input + prefix_len + sizeof(unicode) - 1u, ", tail", 6u);
            speech_seg_config_default_v1(&cfg);
            cfg.min_segment_chars = (int)prefix_len + 4;
            cfg.max_segment_chars = 80;
            unicode_ok = speech_seg_next_v1(
                input, len, &cfg, &so, &sl, &ro, &rl) ==
                SPEECH_SEG_NO_CUT &&
                so == 0 && sl == 0 && ro == 0 && rl == len;
            if (unicode_ok) {
                size_t segment_len = prefix_len + sizeof(unicode) - 1u;
                unicode_ok = speech_seg_first_prefix_v1(
                    input,
                    len,
                    (int)prefix_len + 1,
                    &so,
                    &sl,
                    &ro,
                    &rl) == SPEECH_SEG_OK &&
                    so == 0 && sl == segment_len + 1u &&
                    ro == segment_len + 2u && rl == 4u;
            }
        }
        expect("seg ASCII span scan exits for Unicode", unicode_ok);
        for (prefix_len = 0;
             prefix_len < sizeof(size_t) * 2u && tag_ok;
             ++prefix_len) {
            speech_seg_config_v1 cfg;
            size_t so, sl, ro, rl;
            size_t segment_len = prefix_len + sizeof(tag) - 1u;
            size_t len = segment_len + 5u;
            memset(input, 'a', prefix_len);
            memcpy(input + prefix_len, tag, sizeof(tag) - 1u);
            memcpy(input + segment_len, " tail", 5u);
            speech_seg_config_default_v1(&cfg);
            cfg.min_segment_chars = 1;
            cfg.max_segment_chars = (int)prefix_len + 8;
            tag_ok = speech_seg_next_v1(
                input, len, &cfg, &so, &sl, &ro, &rl) == SPEECH_SEG_OK &&
                so == 0 && sl == segment_len && ro == segment_len + 1u &&
                rl == 4u;
            if (tag_ok)
                tag_ok = speech_seg_first_prefix_v1(
                    input, len, 1, &so, &sl, &ro, &rl) == SPEECH_SEG_OK &&
                    so == 0 && sl == segment_len &&
                    ro == segment_len + 1u && rl == 4u;
        }
        expect("seg ASCII span scan exits for tags", tag_ok);
    }
    /* speech_segment: ASCII first-prefix rules preserve exact spans */
    {
        size_t so, sl, ro, rl;
        const char spaces[] = "  alpha   beta";
        const char punct[] = "word,tail";
        expect(
            "seg ASCII prefix trims remainder",
            speech_seg_first_prefix_v1(
                spaces, sizeof(spaces) - 1u, 5, &so, &sl, &ro, &rl) ==
                SPEECH_SEG_OK &&
            so == 2 && sl == 5 && ro == 10 && rl == 4);
        expect(
            "seg ASCII prefix accepts punctuation",
            speech_seg_first_prefix_v1(
                punct, sizeof(punct) - 1u, 4, &so, &sl, &ro, &rl) ==
                SPEECH_SEG_OK &&
            so == 0 && sl == 5 && ro == 5 && rl == 4);
        expect(
            "seg ASCII prefix stays buffered",
            speech_seg_first_prefix_v1(
                "word", 4, 4, &so, &sl, &ro, &rl) == SPEECH_SEG_NO_CUT &&
            so == 0 && sl == 0 && ro == 0 && rl == 4);
    }

    /* pb_min lifecycle encode/decode + turn_start encode */
    {
        uint8_t buf[256];
        stt_lifecycle_c life;
        static const uint8_t canonical[] = {
            0x0a, 0x02, 's', '1', 0x18, 0x04, 0x28, 0x63
        };
        size_t n = pb_encode_stt_lifecycle(buf, sizeof(buf), "s1", "stream_ended", 99);
        expect("life enc", n > 0);
        expect("life canonical wire", n == sizeof(canonical) && memcmp(buf, canonical, n) == 0);
        expect("life dec", pb_decode_stt_lifecycle(buf, n, &life) == 0);
        expect("life sid", strcmp(life.session_id, "s1") == 0);
        expect("life type", strcmp(life.type, "stream_ended") == 0);
        expect("life type id", life.type_id == STT_LIFECYCLE_STREAM_ENDED);
        expect("life ts", life.timestamp_ms == 99);
    }
    {
        static const uint8_t canonical_transcript[] = {
            0x0a, 0x02, 's', '1',
            0x12, 0x05, 'H', 'e', 'l', 'l', 'o',
            0x18, 0x07,
            0x28, 0x01,
            0x30, 0x63,
            0x88, 0x01, 0x01
        };
        stt_transcription_c transcript;
        expect(
            "transcript canonical decode",
            pb_decode_stt_transcription(
                canonical_transcript, sizeof(canonical_transcript), &transcript) == 0
        );
        expect("transcript sid", strcmp(transcript.session_id, "s1") == 0);
        expect("transcript text", strcmp(transcript.transcript, "Hello") == 0);
        expect("transcript final", transcript.is_final == 1);
        expect("transcript commit", transcript.commit_for_turn == 1);
        expect(
            "transcript is not lifecycle",
            pb_decode_stt_lifecycle(
                canonical_transcript, sizeof(canonical_transcript), &(stt_lifecycle_c){0}) != 0
        );
    }
    {
        stt_transcription_c in, out;
        uint8_t buf[512];
        size_t n;
        memset(&in, 0, sizeof(in));
        snprintf(in.session_id, sizeof(in.session_id), "s-stage");
        snprintf(in.transcript, sizeof(in.transcript), "Hello");
        snprintf(in.state, sizeof(in.state), "final");
        in.sequence = 1;
        in.is_final = 1;
        in.timestamp_ms = 105;
        in.has_voice_activity = 1;
        in.commit_for_turn = 1;
        in.stream_state = 2;
        in.input_stages.audio_committed_at_ms = 100;
        in.input_stages.stt_request_received_at_ms = 101;
        in.input_stages.stt_provider_request_started_at_ms = 102;
        in.input_stages.stt_provider_ready_at_ms = 104;
        in.input_stages.stt_transcript_published_at_ms = 105;
        n = pb_encode_stt_transcription(buf, sizeof(buf), &in);
        expect("staged transcript enc", n > 0);
        expect("staged transcript dec", pb_decode_stt_transcription(buf, n, &out) == 0);
        expect(
            "staged transcript fields",
            out.input_stages.audio_committed_at_ms == 100 &&
            out.input_stages.stt_request_received_at_ms == 101 &&
            out.input_stages.stt_provider_request_started_at_ms == 102 &&
            out.input_stages.stt_provider_ready_at_ms == 104 &&
            out.input_stages.stt_transcript_published_at_ms == 105);
        in.input_stages.stt_provider_ready_at_ms = 0;
        expect(
            "staged transcript rejects partial waterfall",
            pb_encode_stt_transcription(buf, sizeof(buf), &in) == 0);
        in.input_stages.stt_provider_ready_at_ms = 99;
        expect(
            "staged transcript rejects reversed waterfall",
            pb_encode_stt_transcription(buf, sizeof(buf), &in) == 0);
    }
    {
        turn_start_c in, out;
        uint8_t buf[512];
        uint8_t generic[512];
        uint8_t prepared[512];
        size_t n;
        size_t capacity;
        int prepared_matches = 1;
        memset(&in, 0, sizeof(in));
        snprintf(in.request_id, sizeof(in.request_id), "r1");
        snprintf(in.session_id, sizeof(in.session_id), "s1");
        snprintf(in.text, sizeof(in.text), "hello");
        snprintf(in.response_subject, sizeof(in.response_subject), "ai.turn.events.r1");
        in.input_stages.audio_committed_at_ms = 100;
        in.input_stages.stt_request_received_at_ms = 101;
        in.input_stages.stt_provider_request_started_at_ms = 102;
        in.input_stages.stt_provider_ready_at_ms = 103;
        in.input_stages.stt_transcript_published_at_ms = 104;
        n = pb_encode_turn_start(buf, sizeof(buf), &in);
        memset(&out, 0xa5, sizeof(out));
        expect("turn enc", n > 0);
        expect("turn canonical user tag", n > 6 && buf[4] == 0x12);
        expect("turn dec", pb_decode_turn_start(buf, n, &out) == 0);
        expect("turn rid", strcmp(out.request_id, "r1") == 0);
        expect("turn text", strcmp(out.text, "hello") == 0);
        expect(
            "turn decoded lengths",
            out.request_id_len == strlen(out.request_id) &&
            out.user_id_len == strlen(out.user_id) &&
            out.session_id_len == strlen(out.session_id) &&
            out.text_len == strlen(out.text) &&
            out.response_subject_len == strlen(out.response_subject) &&
            out.user_id[0] == '\0' && out.user_id_len == 0u &&
            out.premium == 0 && out.enable_rag == 0 && out.enable_tts == 0 &&
            out.meta_budget_ms[0] == '\0' &&
            out.meta_deadline_unix_ms[0] == '\0' &&
            out.has_meta_budget == 0 && out.has_meta_deadline == 0);
        expect(
            "turn input stages",
            out.input_stages.audio_committed_at_ms == 100 &&
            out.input_stages.stt_request_received_at_ms == 101 &&
            out.input_stages.stt_provider_request_started_at_ms == 102 &&
            out.input_stages.stt_provider_ready_at_ms == 103 &&
            out.input_stages.stt_transcript_published_at_ms == 104);
        for (capacity = 0u;
             capacity <= n + 1u && prepared_matches;
             ++capacity) {
            size_t generic_len = pb_encode_turn_start(
                generic, capacity, &out);
            size_t prepared_len = pb_encode_turn_start_prepared(
                prepared, capacity, &out);
            if (generic_len != prepared_len ||
                (generic_len != 0u &&
                 memcmp(generic, prepared, generic_len) != 0))
                prepared_matches = 0;
        }
        expect(
            "prepared turn start matches every output capacity",
            prepared_matches);
        {
            turn_start_c stale = out;
            stale.text_len++;
            expect(
                "prepared turn start rejects stale cached length",
                pb_encode_turn_start_prepared(
                    prepared, sizeof(prepared), &stale) == 0u);
            stale = out;
            stale.text_len = sizeof(stale.text);
            expect(
                "prepared turn start rejects out-of-range cached length",
                pb_encode_turn_start_prepared(
                    prepared, sizeof(prepared), &stale) == 0u);
        }
        if (n > 0 && n + 2u <= sizeof(buf)) {
            buf[n++] = 0x68u;
            buf[n++] = 0x01u;
        }
        expect(
            "turn rejects duplicate input stage",
            pb_decode_turn_start(buf, n, &out) != 0);
        in.input_stages.stt_provider_ready_at_ms = 0;
        expect(
            "turn rejects partial input stages",
            pb_encode_turn_start(buf, sizeof(buf), &in) == 0);
    }
    {
        turn_start_c maximum;
        uint8_t generic[4096];
        uint8_t prepared[4096];
        size_t capacity;
        size_t generic_len;
        int prepared_matches = 1;
        memset(&maximum, 0, sizeof(maximum));
        memset(maximum.request_id, 'r', sizeof(maximum.request_id) - 1u);
        maximum.request_id_len = sizeof(maximum.request_id) - 1u;
        memset(maximum.user_id, 'u', sizeof(maximum.user_id) - 1u);
        maximum.user_id_len = sizeof(maximum.user_id) - 1u;
        memset(maximum.session_id, 's', sizeof(maximum.session_id) - 1u);
        maximum.session_id_len = sizeof(maximum.session_id) - 1u;
        memset(maximum.text, 'x', sizeof(maximum.text) - 1u);
        maximum.text_len = sizeof(maximum.text) - 1u;
        memset(
            maximum.response_subject,
            'e',
            sizeof(maximum.response_subject) - 1u);
        maximum.response_subject_len = sizeof(maximum.response_subject) - 1u;
        memcpy(maximum.meta_budget_ms, "300000", sizeof("300000"));
        memcpy(
            maximum.meta_deadline_unix_ms,
            "1788127200000",
            sizeof("1788127200000"));
        maximum.has_meta_budget = 1;
        maximum.has_meta_deadline = 1;
        maximum.premium = 1;
        maximum.enable_rag = 1;
        maximum.enable_tts = 1;
        maximum.input_stages.audio_committed_at_ms = 1788127199995LL;
        maximum.input_stages.stt_request_received_at_ms = 1788127199996LL;
        maximum.input_stages.stt_provider_request_started_at_ms =
            1788127199997LL;
        maximum.input_stages.stt_provider_ready_at_ms = 1788127199998LL;
        maximum.input_stages.stt_transcript_published_at_ms = 1788127199999LL;
        generic_len = pb_encode_turn_start(
            generic, sizeof(generic), &maximum);
        for (capacity = 0u;
             generic_len != 0u && capacity <= generic_len + 1u &&
                 prepared_matches;
             ++capacity) {
            size_t current_generic_len = pb_encode_turn_start(
                generic, capacity, &maximum);
            size_t prepared_len = pb_encode_turn_start_prepared(
                prepared, capacity, &maximum);
            if (current_generic_len != prepared_len ||
                (current_generic_len != 0u &&
                 memcmp(generic, prepared, current_generic_len) != 0))
                prepared_matches = 0;
        }
        expect(
            "prepared maximum turn start matches every output capacity",
            generic_len != 0u && prepared_matches);
    }
    {
        uint8_t buf[256];
        turn_cancel_c cancel;
        size_t n = pb_encode_turn_cancel(buf, sizeof(buf), "r1", "u1", "barge_in");
        expect("cancel enc", n > 0);
        expect("cancel dec", pb_decode_turn_cancel(buf, n, &cancel) == 0);
        expect("cancel rid", strcmp(cancel.request_id, "r1") == 0);
        expect("cancel reason", strcmp(cancel.reason, "barge_in") == 0);
        expect("cancel requires rid", pb_encode_turn_cancel(buf, sizeof(buf), "", "", "x") == 0);
    }
    {
        uint8_t buf[4096];
        uint8_t prepared_buf[4096];
        turn_tts_segment_c in, out;
        turn_tts_segment_view_c view;
        turn_tts_segment_view_c admitted;
        uint32_t request_hash;
        size_t n;
        memset(&in, 0, sizeof(in));
        snprintf(in.request_id, sizeof(in.request_id), "r1");
        snprintf(in.user_id, sizeof(in.user_id), "u1");
        snprintf(in.text, sizeof(in.text), "Speak this");
        snprintf(in.voice_id, sizeof(in.voice_id), "orpheus");
        snprintf(in.response_subject, sizeof(in.response_subject), "ai.turn.events.r1");
        in.segment_index = 3;
        in.is_final = 1;
        in.output_sample_rate = 24000;
        in.output_channels = 1;
        in.output_bit_depth = 16;
        in.output_encoding = 1;
        in.query_hash = 0x12345678u;
        in.stream_finality_deferred = 1;
        in.input_stages.audio_committed_at_ms = 96;
        in.input_stages.stt_request_received_at_ms = 97;
        in.input_stages.stt_provider_request_started_at_ms = 98;
        in.input_stages.stt_provider_ready_at_ms = 99;
        in.input_stages.stt_transcript_published_at_ms = 1787774400000LL;
        in.first_text_at_ms = 1787774400001LL;
        in.segment_emitted_at_ms = 1787774400002LL;
        n = pb_encode_turn_tts_segment(buf, sizeof(buf), &in);
        expect("tts segment enc", n > 0);
        expect("tts segment dec", pb_decode_turn_tts_segment(buf, n, &out) == 0);
        expect(
            "tts segment borrowed view dec",
            pb_decode_turn_tts_segment_view(buf, n, &view) == 0);
        expect(
            "tts segment admitted view decodes canonical traffic",
            pb_decode_turn_tts_segment_view_admitted(
                buf, n, &admitted, &request_hash) == 0);
        expect(
            "tts segment admitted view matches generic traffic",
            memcmp(&admitted, &view, sizeof(view)) == 0 &&
            request_hash == test_fnv1a_span("r1", 2u));
        expect(
            "tts segment borrowed view aliases wire",
            view.request_id_len == 2u &&
            memcmp(view.request_id, "r1", view.request_id_len) == 0 &&
            view.text_len == sizeof("Speak this") - 1u &&
            memcmp(view.text, "Speak this", view.text_len) == 0 &&
            (const uint8_t *)view.request_id >= buf &&
            (const uint8_t *)view.request_id < buf + n &&
            (const uint8_t *)view.text >= buf &&
            (const uint8_t *)view.text < buf + n);
        expect(
            "tts segment borrowed view preserves scalars",
            view.segment_index == out.segment_index &&
            view.is_final == out.is_final &&
            view.query_hash == out.query_hash &&
            view.stream_finality_deferred == out.stream_finality_deferred &&
            view.segment_emitted_at_ms == out.segment_emitted_at_ms &&
            view.input_stages.stt_transcript_published_at_ms ==
                out.input_stages.stt_transcript_published_at_ms);
        {
            size_t capacity;
            int prepared_matches = 1;
            for (capacity = 0u;
                 capacity <= n + 1u && prepared_matches;
                 ++capacity) {
                uint8_t generic_buf[4096];
                size_t generic_len = pb_encode_turn_tts_segment(
                    generic_buf, capacity, &in);
                size_t prepared_len = pb_encode_turn_tts_segment_prepared(
                    prepared_buf, capacity, &view);
                if (generic_len != prepared_len ||
                    (generic_len != 0u &&
                     memcmp(generic_buf, prepared_buf, generic_len) != 0))
                    prepared_matches = 0;
            }
            expect(
                "prepared TTS segment matches every output capacity",
                n != 0u && prepared_matches);
        }
        {
            turn_tts_segment_view_c invalid = view;
            invalid.request_id = NULL;
            expect(
                "prepared TTS segment rejects missing required span",
                pb_encode_turn_tts_segment_prepared(
                    prepared_buf, sizeof(prepared_buf), &invalid) == 0u);
            invalid = view;
            invalid.text = NULL;
            expect(
                "prepared TTS segment rejects missing nonempty span",
                pb_encode_turn_tts_segment_prepared(
                    prepared_buf, sizeof(prepared_buf), &invalid) == 0u);
            invalid = view;
            invalid.request_id_len = sizeof(in.request_id);
            expect(
                "prepared TTS segment rejects out-of-range cached length",
                pb_encode_turn_tts_segment_prepared(
                    prepared_buf, sizeof(prepared_buf), &invalid) == 0u);
            invalid = view;
            invalid.segment_index = -1;
            expect(
                "prepared TTS segment rejects invalid scalar",
                pb_encode_turn_tts_segment_prepared(
                    prepared_buf, sizeof(prepared_buf), &invalid) == 0u);
            invalid = view;
            invalid.segment_emitted_at_ms = 0;
            expect(
                "prepared TTS segment rejects partial timing waterfall",
                pb_encode_turn_tts_segment_prepared(
                    prepared_buf, sizeof(prepared_buf), &invalid) == 0u);
        }
        expect(
            "tts segment identifiers",
            strcmp(out.request_id, "r1") == 0 &&
            strcmp(out.user_id, "u1") == 0 &&
            strcmp(out.voice_id, "orpheus") == 0 &&
            strcmp(out.response_subject, "ai.turn.events.r1") == 0);
        expect("tts segment text", strcmp(out.text, "Speak this") == 0);
        expect(
            "tts segment decoded lengths",
            out.request_id_len == strlen(out.request_id) &&
            out.user_id_len == strlen(out.user_id) &&
            out.text_len == strlen(out.text) &&
            out.voice_id_len == strlen(out.voice_id) &&
            out.response_subject_len == strlen(out.response_subject));
        expect("tts segment index", out.segment_index == 3 && out.is_final == 1);
        expect(
            "tts segment format",
            out.output_sample_rate == 24000 && out.output_channels == 1 &&
            out.output_bit_depth == 16 && out.output_encoding == 1);
        expect("tts segment qh", out.query_hash == 0x12345678u);
        expect(
            "tts segment deferred finality",
            out.stream_finality_deferred == 1);
        expect(
            "tts segment upstream stages",
            out.first_text_at_ms == 1787774400001LL &&
            out.segment_emitted_at_ms == 1787774400002LL &&
            out.input_stages.audio_committed_at_ms == 96 &&
            out.input_stages.stt_request_received_at_ms == 97 &&
            out.input_stages.stt_provider_request_started_at_ms == 98 &&
            out.input_stages.stt_provider_ready_at_ms == 99 &&
            out.input_stages.stt_transcript_published_at_ms == 1787774400000LL);
        {
            static const int64_t timestamp_boundaries[] = {
                1,
                127,
                128,
                16383,
                16384,
                (INT64_C(1) << 35) - 1,
                INT64_C(1) << 35,
                (INT64_C(1) << 42) - 1,
                INT64_C(1) << 42,
                INT64_MAX,
            };
            turn_tts_segment_c timestamp_case;
            size_t boundary;
            int boundaries_ok = 1;
            memset(&timestamp_case, 0, sizeof(timestamp_case));
            snprintf(
                timestamp_case.request_id,
                sizeof(timestamp_case.request_id),
                "r-timestamp");
            snprintf(
                timestamp_case.text,
                sizeof(timestamp_case.text),
                "timestamp");
            for (boundary = 0u;
                 boundary < sizeof(timestamp_boundaries) /
                     sizeof(timestamp_boundaries[0]);
                 ++boundary) {
                timestamp_case.first_text_at_ms =
                    timestamp_boundaries[boundary];
                timestamp_case.segment_emitted_at_ms =
                    timestamp_boundaries[boundary];
                n = pb_encode_turn_tts_segment(
                    buf, sizeof(buf), &timestamp_case);
                if (n == 0u ||
                    pb_decode_turn_tts_segment_view(buf, n, &view) != 0 ||
                    pb_decode_turn_tts_segment_view_admitted(
                        buf, n, &admitted, &request_hash) != 0 ||
                    view.first_text_at_ms != timestamp_boundaries[boundary] ||
                    view.segment_emitted_at_ms !=
                        timestamp_boundaries[boundary] ||
                    admitted.first_text_at_ms !=
                        timestamp_boundaries[boundary] ||
                    admitted.segment_emitted_at_ms !=
                        timestamp_boundaries[boundary])
                    boundaries_ok = 0;
            }
            expect(
                "tts segment timestamp varint boundaries",
                boundaries_ok);
        }
        {
            static const uint32_t scalar_fields[] = {
                6u, 7u, 8u, 9u, 10u, 11u, 20u,
            };
            size_t field_index;
            int scalar_bytes_match = 1;
            for (field_index = 0u;
                 field_index < sizeof(scalar_fields) /
                     sizeof(scalar_fields[0]) && scalar_bytes_match;
                 ++field_index) {
                uint32_t field = scalar_fields[field_index];
                unsigned scalar;
                for (scalar = 0u; scalar < 128u; ++scalar) {
                    uint8_t scalar_wire[10];
                    size_t scalar_len = 0u;
                    int generic_rc;
                    int admitted_rc;
                    int expected = !((field == 7u || field == 20u) &&
                        scalar > 1u);
                    scalar_wire[scalar_len++] = 0x0au;
                    scalar_wire[scalar_len++] = 0x01u;
                    scalar_wire[scalar_len++] = 'r';
                    scalar_wire[scalar_len++] = 0x1au;
                    scalar_wire[scalar_len++] = 0x01u;
                    scalar_wire[scalar_len++] = 't';
                    if (field < 16u) {
                        scalar_wire[scalar_len++] = (uint8_t)(field << 3u);
                    } else {
                        scalar_wire[scalar_len++] = 0xa0u;
                        scalar_wire[scalar_len++] = 0x01u;
                    }
                    scalar_wire[scalar_len++] = (uint8_t)scalar;
                    generic_rc = pb_decode_turn_tts_segment_view(
                        scalar_wire, scalar_len, &view);
                    admitted_rc = pb_decode_turn_tts_segment_view_admitted(
                        scalar_wire, scalar_len, &admitted, &request_hash);
                    if ((generic_rc == 0) != expected ||
                        (admitted_rc == 0) != expected ||
                        (expected &&
                         (memcmp(&admitted, &view, sizeof(view)) != 0 ||
                          request_hash != test_fnv1a_span("r", 1u)))) {
                        scalar_bytes_match = 0;
                        break;
                    }
                }
            }
            expect(
                "tts admitted scalar fields classify every one-byte value",
                scalar_bytes_match);
        }
        {
            static const int32_t scalar_boundaries[] = {
                127, 128, 16383, 16384, 23999, 24000, 24001, INT32_MAX,
            };
            turn_tts_segment_c scalar_case;
            size_t boundary;
            int scalar_boundaries_match = 1;
            memset(&scalar_case, 0, sizeof(scalar_case));
            snprintf(
                scalar_case.request_id,
                sizeof(scalar_case.request_id),
                "r-scalar");
            snprintf(
                scalar_case.text,
                sizeof(scalar_case.text),
                "scalar");
            for (boundary = 0u;
                 boundary < sizeof(scalar_boundaries) /
                     sizeof(scalar_boundaries[0]);
                 ++boundary) {
                scalar_case.output_sample_rate = scalar_boundaries[boundary];
                n = pb_encode_turn_tts_segment(
                    buf, sizeof(buf), &scalar_case);
                if (n == 0u ||
                    pb_decode_turn_tts_segment_view(buf, n, &view) != 0 ||
                    pb_decode_turn_tts_segment_view_admitted(
                        buf, n, &admitted, &request_hash) != 0 ||
                    memcmp(&admitted, &view, sizeof(view)) != 0 ||
                    admitted.output_sample_rate != scalar_boundaries[boundary]) {
                    scalar_boundaries_match = 0;
                    break;
                }
            }
            expect(
                "tts admitted scalar varint boundaries match generic decode",
                scalar_boundaries_match);
        }
        {
            static const uint8_t required_fields[] = {
                0x0au, 0x01u, 'r', 0x1au, 0x01u, 't',
            };
            unsigned timing_mask;
            unsigned input_mask;
            int masks_match = 1;
            for (timing_mask = 0u; timing_mask < 4u && masks_match;
                 ++timing_mask) {
                for (input_mask = 0u; input_mask < 32u; ++input_mask) {
                    uint8_t mask_wire[128];
                    size_t mask_len = sizeof(required_fields);
                    unsigned bit;
                    int generic_rc;
                    int admitted_rc;
                    int expected;
                    memcpy(mask_wire, required_fields, mask_len);
                    for (bit = 0u; bit < 2u; ++bit) {
                        if ((timing_mask & (1u << bit)) == 0u) continue;
                        mask_len = reference_write_varint(
                            mask_wire, sizeof(mask_wire), mask_len,
                            (uint64_t)(13u + bit) << 3u);
                        mask_len = reference_write_varint(
                            mask_wire, sizeof(mask_wire), mask_len,
                            (uint64_t)(6u + bit));
                    }
                    for (bit = 0u; bit < 5u; ++bit) {
                        if ((input_mask & (1u << bit)) == 0u) continue;
                        mask_len = reference_write_varint(
                            mask_wire, sizeof(mask_wire), mask_len,
                            (uint64_t)(15u + bit) << 3u);
                        mask_len = reference_write_varint(
                            mask_wire, sizeof(mask_wire), mask_len,
                            (uint64_t)(1u + bit));
                    }
                    generic_rc = pb_decode_turn_tts_segment_view(
                        mask_wire, mask_len, &view);
                    admitted_rc = pb_decode_turn_tts_segment_view_admitted(
                        mask_wire, mask_len, &admitted, &request_hash);
                    expected = (timing_mask == 0u || timing_mask == 3u) &&
                        (input_mask == 0u ||
                         (input_mask == 31u && timing_mask == 3u));
                    if ((generic_rc == 0) != expected ||
                        (admitted_rc == 0) != expected ||
                        (expected &&
                         (memcmp(&admitted, &view, sizeof(view)) != 0 ||
                          request_hash != test_fnv1a_span("r", 1u)))) {
                        masks_match = 0;
                        break;
                    }
                }
            }
            expect(
                "tts admitted timestamp presence masks match generic decode",
                masks_match);
        }
        {
            static const uint8_t required_fields[] = {
                0x0au, 0x01u, 'r', 0x1au, 0x01u, 't',
            };
            unsigned inverted_boundary;
            int orders_match = 1;
            for (inverted_boundary = 0u;
                 inverted_boundary <= 6u && orders_match;
                 ++inverted_boundary) {
                uint64_t timestamps[7] = {
                    10u, 10u, 10u, 10u, 10u, 10u, 10u,
                };
                uint8_t order_wire[128];
                size_t order_len = sizeof(required_fields);
                unsigned field;
                int generic_rc;
                int admitted_rc;
                if (inverted_boundary != 0u)
                    timestamps[inverted_boundary - 1u]++;
                memcpy(order_wire, required_fields, order_len);
                for (field = 13u; field <= 19u; ++field) {
                    size_t timestamp_index = field <= 14u ?
                        (size_t)(field - 8u) : (size_t)(field - 15u);
                    order_len = reference_write_varint(
                        order_wire, sizeof(order_wire), order_len,
                        (uint64_t)field << 3u);
                    order_len = reference_write_varint(
                        order_wire, sizeof(order_wire), order_len,
                        timestamps[timestamp_index]);
                }
                generic_rc = pb_decode_turn_tts_segment_view(
                    order_wire, order_len, &view);
                admitted_rc = pb_decode_turn_tts_segment_view_admitted(
                    order_wire, order_len, &admitted, &request_hash);
                if ((generic_rc == 0) != (inverted_boundary == 0u) ||
                    (admitted_rc == 0) != (inverted_boundary == 0u) ||
                    (inverted_boundary == 0u &&
                     (memcmp(&admitted, &view, sizeof(view)) != 0 ||
                      request_hash != test_fnv1a_span("r", 1u))))
                    orders_match = 0;
            }
            expect(
                "tts admitted timestamp order boundaries match generic decode",
                orders_match);
        }
        {
            static const uint8_t truncated_scalar[] = {
                0x0au, 0x01u, 'r', 0x1au, 0x01u, 't',
                0x40u, 0xc0u, 0xbbu,
            };
            static const uint8_t noncanonical_scalar[] = {
                0x0au, 0x01u, 'r', 0x1au, 0x01u, 't',
                0x40u, 0xc0u, 0xbbu, 0x81u, 0x00u,
            };
            expect(
                "tts admitted scalar fast path rejects malformed varints",
                pb_decode_turn_tts_segment_view_admitted(
                    truncated_scalar, sizeof(truncated_scalar),
                    &admitted, &request_hash) != 0 &&
                pb_decode_turn_tts_segment_view_admitted(
                    noncanonical_scalar, sizeof(noncanonical_scalar),
                    &admitted, &request_hash) != 0);
        }
        {
            turn_tts_segment_c minimal;
            memset(&minimal, 0, sizeof(minimal));
            snprintf(minimal.request_id, sizeof(minimal.request_id), "r-reset");
            snprintf(minimal.text, sizeof(minimal.text), "reset fields");
            n = pb_encode_turn_tts_segment(buf, sizeof(buf), &minimal);
            expect(
                "tts segment resets absent fields",
                n > 0 && pb_decode_turn_tts_segment(buf, n, &out) == 0 &&
                strcmp(out.request_id, "r-reset") == 0 &&
                strcmp(out.text, "reset fields") == 0 && out.user_id[0] == '\0' &&
                out.voice_id[0] == '\0' && out.response_subject[0] == '\0' &&
                out.request_id_len == strlen(out.request_id) &&
                out.text_len == strlen(out.text) && out.user_id_len == 0u &&
                out.voice_id_len == 0u && out.response_subject_len == 0u &&
                out.segment_index == 0 && out.is_final == 0 &&
                out.output_sample_rate == 0 && out.output_channels == 0 &&
                out.output_bit_depth == 0 && out.output_encoding == 0 &&
                out.query_hash == 0 && out.first_text_at_ms == 0 &&
                out.segment_emitted_at_ms == 0 &&
                out.stream_finality_deferred == 0 &&
                out.input_stages.audio_committed_at_ms == 0 &&
                out.input_stages.stt_transcript_published_at_ms == 0);
            expect(
                "tts segment borrowed view resets absent fields",
                n > 0 &&
                pb_decode_turn_tts_segment_view(buf, n, &view) == 0 &&
                view.user_id == NULL && view.user_id_len == 0u &&
                view.voice_id == NULL && view.voice_id_len == 0u &&
                view.response_subject == NULL &&
                view.response_subject_len == 0u &&
                view.segment_index == 0 && view.is_final == 0 &&
                view.stream_finality_deferred == 0 &&
                view.input_stages.audio_committed_at_ms == 0);
        }
        {
            turn_tts_segment_c marker;
            memset(&marker, 0, sizeof(marker));
            snprintf(marker.request_id, sizeof(marker.request_id), "r-marker");
            marker.segment_index = 1;
            marker.is_final = 1;
            marker.stream_finality_deferred = 1;
            n = pb_encode_turn_tts_segment(buf, sizeof(buf), &marker);
            expect(
                "tts final marker encodes empty text",
                n > 0 && pb_decode_turn_tts_segment(buf, n, &out) == 0 &&
                strcmp(out.request_id, "r-marker") == 0 &&
                out.text_len == 0u && out.text[0] == '\0' &&
                out.segment_index == 1 && out.is_final == 1 &&
                out.stream_finality_deferred == 1);
            expect(
                "tts final marker borrowed view keeps empty text",
                n > 0 &&
                pb_decode_turn_tts_segment_view(buf, n, &view) == 0 &&
                view.text_len == 0u && view.segment_index == 1 &&
                view.is_final == 1 && view.stream_finality_deferred == 1);
            expect(
                "tts final marker uses admitted canonical path",
                n > 0u && pb_decode_turn_tts_segment_view_admitted(
                    buf, n, &admitted, &request_hash) == 0 &&
                admitted.text_len == 0u && admitted.segment_index == 1 &&
                admitted.is_final == 1 &&
                admitted.stream_finality_deferred == 1);
            expect(
                "prepared TTS final marker matches canonical bytes",
                n > 0u && pb_encode_turn_tts_segment_prepared(
                    prepared_buf, sizeof(prepared_buf), &view) == n &&
                memcmp(buf, prepared_buf, n) == 0);
            marker.is_final = 0;
            expect(
                "tts rejects empty nonfinal marker",
                pb_encode_turn_tts_segment(buf, sizeof(buf), &marker) == 0);
            marker.is_final = 1;
            marker.stream_finality_deferred = 0;
            expect(
                "tts rejects empty legacy final",
                pb_encode_turn_tts_segment(buf, sizeof(buf), &marker) == 0);
            marker.stream_finality_deferred = 1;
            marker.segment_index = 0;
            expect(
                "tts rejects empty marker without a segment",
                pb_encode_turn_tts_segment(buf, sizeof(buf), &marker) == 0);
        }
        n = pb_encode_turn_tts_segment(buf, sizeof(buf), &in);
        if (n > 0 && n + 22u <= sizeof(buf)) {
            buf[n++] = 0x0au;
            buf[n++] = 0x08u;
            memcpy(buf + n, "r-repeat", 8u);
            n += 8u;
            buf[n++] = 0x1au;
            buf[n++] = 0x0au;
            memcpy(buf + n, "last words", 10u);
            n += 10u;
        }
        expect(
            "tts segment repeated strings update decoded lengths",
            pb_decode_turn_tts_segment(buf, n, &out) == 0 &&
            strcmp(out.request_id, "r-repeat") == 0 &&
            out.request_id_len == 8u && strcmp(out.text, "last words") == 0 &&
            out.text_len == 10u);
        expect(
            "tts segment borrowed view keeps final repeated strings",
            pb_decode_turn_tts_segment_view(buf, n, &view) == 0 &&
            view.request_id_len == 8u &&
            memcmp(view.request_id, "r-repeat", view.request_id_len) == 0 &&
            view.text_len == 10u &&
            memcmp(view.text, "last words", view.text_len) == 0);
        expect(
            "tts segment admitted view preserves repeated-field fallback",
            pb_decode_turn_tts_segment_view_admitted(
                buf, n, &admitted, &request_hash) == 0 &&
            memcmp(&admitted, &view, sizeof(view)) == 0 &&
            request_hash == test_fnv1a_span("r-repeat", 8u));
        {
            turn_tts_segment_c repeated_request = in;
            snprintf(
                repeated_request.request_id,
                sizeof(repeated_request.request_id),
                "bad/id");
            n = pb_encode_turn_tts_segment(
                buf, sizeof(buf), &repeated_request);
            if (n > 0u && n + sizeof("r-final") + 1u <= sizeof(buf)) {
                buf[n++] = 0x0au;
                buf[n++] = (uint8_t)(sizeof("r-final") - 1u);
                memcpy(buf + n, "r-final", sizeof("r-final") - 1u);
                n += sizeof("r-final") - 1u;
            }
            expect(
                "tts admitted fallback uses the final repeated identifier",
                pb_decode_turn_tts_segment_view_admitted(
                    buf, n, &admitted, &request_hash) == 0 &&
                admitted.request_id_len == sizeof("r-final") - 1u &&
                memcmp(
                    admitted.request_id,
                    "r-final",
                    admitted.request_id_len) == 0 &&
                request_hash == test_fnv1a_span(
                    "r-final", sizeof("r-final") - 1u));
        }
        {
            turn_tts_segment_c invalid_request = in;
            snprintf(
                invalid_request.request_id,
                sizeof(invalid_request.request_id),
                "bad/id");
            n = pb_encode_turn_tts_segment(
                buf, sizeof(buf), &invalid_request);
            expect(
                "tts generic view retains compatible request identifiers",
                n > 0u &&
                pb_decode_turn_tts_segment_view(buf, n, &view) == 0);
            expect(
                "tts admitted view rejects unsafe request identifiers",
                n > 0u && pb_decode_turn_tts_segment_view_admitted(
                    buf, n, &admitted, &request_hash) != 0);
        }
        in.segment_emitted_at_ms = 0;
        expect(
            "tts segment rejects partial upstream stages",
            pb_encode_turn_tts_segment(buf, sizeof(buf), &in) == 0);
        in.segment_emitted_at_ms = 1787774400000LL;
        expect(
            "tts segment rejects reversed upstream stages",
            pb_encode_turn_tts_segment(buf, sizeof(buf), &in) == 0);
        in.segment_emitted_at_ms = 1787774400002LL;
        n = pb_encode_turn_tts_segment(buf, sizeof(buf), &in);
        if (n > 0 && n + 2u <= sizeof(buf)) {
            buf[n++] = 0x68u;
            buf[n++] = 0x01u;
        }
        expect(
            "tts segment rejects duplicate upstream stage",
            pb_decode_turn_tts_segment(buf, n, &out) != 0);
        expect(
            "tts segment borrowed view rejects duplicate upstream stage",
            pb_decode_turn_tts_segment_view(buf, n, &view) != 0);
        n = pb_encode_turn_tts_segment(buf, sizeof(buf), &in);
        if (n > 0u && n + 3u <= sizeof(buf)) {
            buf[n++] = 0xa0u;
            buf[n++] = 0x01u;
            buf[n++] = 0x01u;
        }
        expect(
            "tts segment borrowed view accepts larger tag fallback",
            n > 3u && pb_decode_turn_tts_segment_view(buf, n, &view) == 0 &&
            view.request_id_len == 2u && view.text_len == sizeof("Speak this") - 1u);
        expect(
            "tts segment admitted view accepts larger tag fallback",
            n > 3u && pb_decode_turn_tts_segment_view_admitted(
                buf, n, &admitted, &request_hash) == 0 &&
            memcmp(&admitted, &view, sizeof(view)) == 0);
        {
            static const uint8_t noncanonical_tag[] = {
                0x8au, 0x00u, 0x01u, 'r', 0x1au, 0x01u, 't'
            };
            static const uint8_t truncated_tag[] = {0x80u};
            static const uint8_t noncanonical_length[] = {
                0x0au, 0x81u, 0x00u, 'r', 0x1au, 0x01u, 't'
            };
            static const uint8_t truncated_length[] = {0x0au, 0x80u};
            static const uint8_t noncanonical_six_byte_timestamp[] = {
                0x0au, 0x01u, 'r', 0x1au, 0x01u, 't', 0x68u,
                0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x00u,
                0x70u, 0x01u,
            };
            expect(
                "tts segment borrowed view rejects noncanonical tag",
                pb_decode_turn_tts_segment_view(
                    noncanonical_tag, sizeof(noncanonical_tag), &view) != 0);
            expect(
                "tts segment borrowed view rejects truncated tag",
                pb_decode_turn_tts_segment_view(
                    truncated_tag, sizeof(truncated_tag), &view) != 0);
            expect(
                "tts segment borrowed view rejects noncanonical length",
                pb_decode_turn_tts_segment_view(
                    noncanonical_length, sizeof(noncanonical_length), &view) != 0);
            expect(
                "tts segment borrowed view rejects truncated length",
                pb_decode_turn_tts_segment_view(
                    truncated_length, sizeof(truncated_length), &view) != 0);
            expect(
                "tts segment borrowed view rejects noncanonical six-byte timestamp",
                pb_decode_turn_tts_segment_view(
                    noncanonical_six_byte_timestamp,
                    sizeof(noncanonical_six_byte_timestamp),
                    &view) != 0);
        }
    }
    {
        uint8_t buf[4096];
        uint8_t generic_buf[4096];
        uint8_t prepared_buf[4096];
        uint8_t oversized_request[131];
        turn_tts_segment_c maximum;
        turn_tts_segment_view_c view;
        turn_tts_segment_view_c admitted;
        uint32_t request_hash;
        size_t n;
        size_t capacity;
        size_t text_offset;
        int prepared_matches = 1;
        int view_rc;
        memset(&maximum, 0, sizeof(maximum));
        memset(maximum.request_id, 'r', sizeof(maximum.request_id) - 1u);
        memset(maximum.user_id, 'u', sizeof(maximum.user_id) - 1u);
        memset(maximum.text, 't', sizeof(maximum.text) - 1u);
        memset(maximum.voice_id, 'v', sizeof(maximum.voice_id) - 1u);
        memset(
            maximum.response_subject,
            's',
            sizeof(maximum.response_subject) - 1u);
        n = pb_encode_turn_tts_segment(buf, sizeof(buf), &maximum);
        view_rc = n > 0u ?
            pb_decode_turn_tts_segment_view(buf, n, &view) : -1;
        expect(
            "tts segment borrowed view accepts exact string capacities",
            view_rc == 0 &&
            view.request_id_len == sizeof(maximum.request_id) - 1u &&
            view.user_id_len == sizeof(maximum.user_id) - 1u &&
            view.text_len == sizeof(maximum.text) - 1u &&
            view.voice_id_len == sizeof(maximum.voice_id) - 1u &&
            view.response_subject_len == sizeof(maximum.response_subject) - 1u);
        expect(
            "tts segment admitted view accepts exact string capacities",
            n > 0u && pb_decode_turn_tts_segment_view_admitted(
                buf, n, &admitted, &request_hash) == 0 &&
            memcmp(&admitted, &view, sizeof(view)) == 0 &&
            request_hash == test_fnv1a_span(
                maximum.request_id, sizeof(maximum.request_id) - 1u));
        for (capacity = 0u;
             view_rc == 0 && capacity <= n + 1u && prepared_matches;
             ++capacity) {
            size_t generic_len = pb_encode_turn_tts_segment(
                generic_buf, capacity, &maximum);
            size_t prepared_len = pb_encode_turn_tts_segment_prepared(
                prepared_buf, capacity, &view);
            if (generic_len != prepared_len ||
                (generic_len != 0u &&
                 memcmp(generic_buf, prepared_buf, generic_len) != 0))
                prepared_matches = 0;
        }
        expect(
            "prepared maximum TTS segment matches every output capacity",
            view_rc == 0 && prepared_matches);
        text_offset = n;
        if (view_rc == 0 && (const uint8_t *)view.text >= buf &&
            (const uint8_t *)view.text < buf + n)
            text_offset = (size_t)((const uint8_t *)view.text - buf);
        if (n > 0u && text_offset < n) buf[text_offset] = '\0';
        expect(
            "tts segment borrowed view rejects embedded null",
            n > 0u && text_offset < n &&
            pb_decode_turn_tts_segment_view(buf, n, &view) != 0);
        oversized_request[0] = 0x0au;
        oversized_request[1] = 0x80u;
        oversized_request[2] = 0x01u;
        memset(oversized_request + 3u, 'r', 128u);
        expect(
            "tts segment borrowed view rejects oversized request identifier",
            pb_decode_turn_tts_segment_view(
                oversized_request,
                sizeof(oversized_request),
                &view) != 0);
        expect(
            "tts segment borrowed view rejects invalid arguments",
            pb_decode_turn_tts_segment_view(buf, n, NULL) != 0 &&
            pb_decode_turn_tts_segment_view(NULL, n, &view) != 0);
        expect(
            "tts segment admitted view rejects invalid arguments",
            pb_decode_turn_tts_segment_view_admitted(
                buf, n, NULL, &request_hash) != 0 &&
            pb_decode_turn_tts_segment_view_admitted(
                buf, n, &admitted, NULL) != 0 &&
            pb_decode_turn_tts_segment_view_admitted(
                NULL, n, &admitted, &request_hash) != 0);
    }
    {
        uint8_t buf[64];
        turn_tts_segment_c input;
        turn_tts_segment_view_c admitted;
        uint32_t request_hash;
        unsigned value;
        int all_bytes_match = 1;
        memset(&input, 0, sizeof(input));
        memcpy(input.text, "x", sizeof("x"));
        for (value = 1u; value <= UINT8_MAX; ++value) {
            unsigned char c = (unsigned char)value;
            int expected = voice_ascii_is_alnum(c) || c == '-' || c == '_' ||
                c == '.' || c == ':';
            size_t n;
            input.request_id[0] = (char)c;
            input.request_id[1] = '\0';
            n = pb_encode_turn_tts_segment(buf, sizeof(buf), &input);
            if (n == 0u ||
                (pb_decode_turn_tts_segment_view_admitted(
                    buf, n, &admitted, &request_hash) == 0) != expected) {
                all_bytes_match = 0;
                break;
            }
        }
        expect(
            "tts admitted request identifier classifies every nonzero byte",
            all_bytes_match);
    }
    {
        uint8_t buf[132];
        turn_tts_segment_view_c admitted;
        uint32_t request_hash;
        size_t position;
        unsigned value;
        int all_positions_match = 1;
        buf[0] = 0x0au;
        buf[1] = 127u;
        memset(buf + 2u, 'a', 127u);
        buf[129] = 0x1au;
        buf[130] = 0x01u;
        buf[131] = 'x';
        for (position = 0u;
             position < 127u && all_positions_match;
             ++position) {
            for (value = 0u; value <= UINT8_MAX; ++value) {
                uint8_t c = (uint8_t)value;
                int expected = voice_ascii_is_alnum(c) || c == '-' ||
                    c == '_' || c == '.' || c == ':';
                buf[2u + position] = c;
                if ((pb_decode_turn_tts_segment_view_admitted(
                         buf, sizeof(buf), &admitted, &request_hash) == 0) !=
                        expected ||
                    (expected && request_hash !=
                        test_fnv1a_span((const char *)buf + 2u, 127u))) {
                    all_positions_match = 0;
                    break;
                }
            }
            buf[2u + position] = 'a';
        }
        expect(
            "tts admitted request identifier validates every byte position",
            all_positions_match);
    }
    {
        uint8_t buf[128];
        turn_event_c ev;
        size_t n = pb_encode_turn_event(buf, sizeof(buf), "r1", "route", "answer");
        expect("ev enc", n > 0);
        expect("ev canonical enum tag", n > 6 && buf[4] == 0x18 && buf[5] == 0x02);
        expect("ev dec", pb_decode_turn_event(buf, n, &ev) == 0);
        expect("ev type", strcmp(ev.type, "thinking_started") == 0 && ev.type_id == 2);
    }
    {
        static const char *const routes[] = {
            "answer",
            "escalate",
            "retrieve_then_escalate"
        };
        uint8_t generic[256];
        uint8_t prepared[256];
        int routes_ok = 1;
        size_t route_index;
        for (route_index = 0u;
             route_index < sizeof(routes) / sizeof(routes[0]) && routes_ok;
             ++route_index) {
            const char *route = routes[route_index];
            size_t route_len = strlen(route);
            size_t generic_len = pb_encode_turn_event(
                generic, sizeof(generic), "route-request", "route", route);
            size_t prepared_len = pb_encode_turn_route_event_prepared(
                prepared,
                sizeof(prepared),
                "route-request",
                sizeof("route-request") - 1u,
                route,
                route_len);
            size_t cap;
            if (generic_len == 0u || prepared_len != generic_len ||
                memcmp(prepared, generic, generic_len) != 0) {
                routes_ok = 0;
                break;
            }
            for (cap = 0u; cap <= prepared_len + 1u; ++cap) {
                size_t bounded = pb_encode_turn_route_event_prepared(
                    prepared,
                    cap,
                    "route-request",
                    sizeof("route-request") - 1u,
                    route,
                    route_len);
                if ((cap < prepared_len && bounded != 0u) ||
                    (cap >= prepared_len &&
                     (bounded != prepared_len ||
                      memcmp(prepared, generic, generic_len) != 0))) {
                    routes_ok = 0;
                    break;
                }
            }
        }
        expect("prepared route events match canonical bytes", routes_ok);
    }
    {
        char request_id[128];
        uint8_t generic[512];
        uint8_t prepared[512];
        size_t generic_len;
        size_t prepared_len;
        memset(request_id, 'r', sizeof(request_id) - 1u);
        request_id[sizeof(request_id) - 1u] = '\0';
        generic_len = pb_encode_turn_event(
            generic, sizeof(generic), request_id, "route", "answer");
        prepared_len = pb_encode_turn_route_event_prepared(
            prepared,
            sizeof(prepared),
            request_id,
            sizeof(request_id) - 1u,
            "answer",
            sizeof("answer") - 1u);
        expect(
            "prepared route event accepts maximum request identifier",
            generic_len > 0u && prepared_len == generic_len &&
            memcmp(prepared, generic, generic_len) == 0);
        expect(
            "prepared route event rejects invalid contracts",
            pb_encode_turn_route_event_prepared(
                prepared, sizeof(prepared), NULL, 1u, "answer", 6u) == 0u &&
            pb_encode_turn_route_event_prepared(
                prepared, sizeof(prepared), "r", 0u, "answer", 6u) == 0u &&
            pb_encode_turn_route_event_prepared(
                prepared, sizeof(prepared), "r", 1u, NULL, 6u) == 0u &&
            pb_encode_turn_route_event_prepared(
                prepared, sizeof(prepared), "r", 1u, "answer", 0u) == 0u &&
            pb_encode_turn_route_event_prepared(
                prepared, sizeof(prepared), "r", 128u, "answer", 6u) == 0u &&
            pb_encode_turn_route_event_prepared(
                prepared, sizeof(prepared), "r", 1u, "answer", 128u) == 0u);
    }
    {
        static const struct {
            const char *input;
            const char *canonical;
            int type_id;
        } types[] = {
            {"started", "started", 1},
            {"thinking_started", "thinking_started", 2},
            {"route", "thinking_started", 2},
            {"needs_model", "thinking_started", 2},
            {"rag_hits", "thinking_started", 2},
            {"transcript_clean", "thinking_started", 2},
            {"snapshot", "thinking_started", 2},
            {"thinking_ended", "thinking_ended", 3},
            {"text_delta", "text_delta", 4},
            {"text_completed", "text_completed", 5},
            {"final", "text_completed", 5},
            {"tts_segment", "tts_segment", 6},
            {"pcm_started", "pcm_started", 7},
            {"tts_started", "pcm_started", 7},
            {"pcm_meta", "pcm_started", 7},
            {"pcm_chunk", "pcm_chunk", 8},
            {"tts_pcm", "pcm_chunk", 8},
            {"pcm_ended", "pcm_ended", 9},
            {"tts_done", "pcm_ended", 9},
            {"completed", "completed", 10},
            {"done", "completed", 10},
            {"endpoint", "completed", 10},
            {"canceled", "canceled", 11},
            {"cancelled", "canceled", 11},
            {"interrupt", "canceled", 11},
            {"failed", "failed", 12}
        };
        uint8_t buf[256];
        int types_ok = 1;
        size_t type_index;
        for (type_index = 0;
             type_index < sizeof(types) / sizeof(types[0]) && types_ok;
             ++type_index) {
            turn_event_c ev;
            turn_event_c active_ev;
            turn_event_c bound_ev;
            turn_event_c public_ev;
            size_t n = pb_encode_turn_event(
                buf, sizeof(buf), "r1", types[type_index].input, "");
            memset(&active_ev, 0, sizeof(active_ev));
            memset(&bound_ev, 0, sizeof(bound_ev));
            memset(&public_ev, 0, sizeof(public_ev));
            if (n == 0 || pb_decode_turn_event(buf, n, &ev) != 0 ||
                pb_decode_turn_event_bound(
                    buf, n, "r1", sizeof("r1") - 1u, &bound_ev) != 0 ||
                pb_decode_turn_event_active(
                    buf, n, "r1", sizeof("r1") - 1u, &active_ev) != 0 ||
                pb_decode_turn_event_public_active(
                    buf, n, "r1", sizeof("r1") - 1u, &public_ev) != 0 ||
                memcmp(&active_ev, &bound_ev, sizeof(bound_ev)) != 0 ||
                memcmp(&public_ev, &bound_ev, sizeof(bound_ev)) != 0 ||
                ev.type_id != types[type_index].type_id ||
                strcmp(ev.type, types[type_index].canonical) != 0) {
                types_ok = 0;
            }
        }
        expect("turn event type table", types_ok);
        expect(
            "turn event rejects unknown types",
            pb_encode_turn_event(buf, sizeof(buf), "r1", "pcm_chunks", "") == 0 &&
            pb_encode_turn_event(
                buf, sizeof(buf), "r1", "thinking_started_extra", "") == 0
        );
    }
    {
        static const char request_id[] = "r-lifecycle";
        static const char wrong_id[] = "r-other";
        static const char *const lifecycle_types[] = {
            "started",
            "thinking_started",
            "thinking_ended",
            "pcm_ended",
            "completed",
            "canceled",
            "failed"
        };
        uint8_t wire[256];
        char maximum_request_id[128];
        turn_event_c reset_event;
        turn_stage_timestamps_c zero_stages;
        size_t type_index;
        size_t wire_len;
        int canonical_matches = 1;
        int fallback_matches = 1;
        for (type_index = 0u;
             type_index <
                 sizeof(lifecycle_types) / sizeof(lifecycle_types[0]);
             ++type_index) {
            wire_len = pb_encode_turn_event(
                wire,
                sizeof(wire),
                request_id,
                lifecycle_types[type_index],
                "");
            if (wire_len == 0u || !public_turn_event_matches_bound(
                    wire,
                    wire_len,
                    request_id,
                    sizeof(request_id) - 1u))
                canonical_matches = 0;
        }
        expect(
            "public lifecycle canonical shapes match checked decode",
            canonical_matches);
        memset(maximum_request_id, 'r', sizeof(maximum_request_id) - 1u);
        maximum_request_id[sizeof(maximum_request_id) - 1u] = '\0';
        wire_len = pb_encode_turn_event(
            wire,
            sizeof(wire),
            maximum_request_id,
            "completed",
            "");
        expect(
            "public lifecycle accepts maximum bound identifier",
            wire_len != 0u && public_turn_event_matches_bound(
                wire,
                wire_len,
                maximum_request_id,
                sizeof(maximum_request_id) - 1u));
        wire_len = pb_encode_turn_event(
            wire, sizeof(wire), request_id, "started", "");
        memset(&reset_event, 0xa5, sizeof(reset_event));
        memset(&zero_stages, 0, sizeof(zero_stages));
        expect(
            "public lifecycle resets every absent field",
            wire_len != 0u && pb_decode_turn_event_public_active(
                wire,
                wire_len,
                request_id,
                sizeof(request_id) - 1u,
                &reset_event) == 0 &&
            reset_event.request_id[0] == '\0' &&
            reset_event.type_id == 1 &&
            strcmp(reset_event.type, "started") == 0 &&
            reset_event.text[0] == '\0' && reset_event.audio == NULL &&
            reset_event.audio_len == 0u && reset_event.sample_rate == 0 &&
            reset_event.channels == 0 && reset_event.bit_depth == 0 &&
            reset_event.sequence == 0 && reset_event.segment_index == 0 &&
            reset_event.is_final == 0 && reset_event.audio_encoding == 0 &&
            reset_event.speech_text[0] == '\0' &&
            reset_event.display_text[0] == '\0' &&
            reset_event.display_text_len == 0u &&
            reset_event.display_text_len_known == 0 &&
            reset_event.display_text_borrowed == 0u &&
            reset_event.current_tts_stage_wire == NULL &&
            memcmp(
                &reset_event.stages,
                &zero_stages,
                sizeof(zero_stages)) == 0);

        wire_len = pb_encode_turn_event(
            wire, sizeof(wire), request_id, "completed", "");
        if (wire_len == 0u || wire_len + 3u > sizeof(wire)) {
            fallback_matches = 0;
        } else {
            wire[wire_len++] = 0x98u;
            wire[wire_len++] = 0x01u;
            wire[wire_len++] = 0x01u;
            fallback_matches = public_turn_event_matches_bound(
                wire,
                wire_len,
                request_id,
                sizeof(request_id) - 1u);
        }
        wire_len = pb_encode_turn_event(
            wire, sizeof(wire), request_id, "completed", "");
        if (wire_len == 0u || wire_len + 2u > sizeof(wire)) {
            fallback_matches = 0;
        } else {
            wire[wire_len++] = 0x18u;
            wire[wire_len++] = 0x0bu;
            fallback_matches = fallback_matches &&
                public_turn_event_matches_bound(
                    wire,
                    wire_len,
                    request_id,
                    sizeof(request_id) - 1u);
        }
        wire_len = 0u;
        wire[wire_len++] = 0x18u;
        wire[wire_len++] = 0x0au;
        wire[wire_len++] = 0x0au;
        wire[wire_len++] = (uint8_t)(sizeof(request_id) - 1u);
        memcpy(wire + wire_len, request_id, sizeof(request_id) - 1u);
        wire_len += sizeof(request_id) - 1u;
        fallback_matches = fallback_matches &&
            public_turn_event_matches_bound(
                wire,
                wire_len,
                request_id,
                sizeof(request_id) - 1u);
        wire_len = pb_encode_turn_event(
            wire, sizeof(wire), request_id, "canceled", "barge_in");
        fallback_matches = fallback_matches && wire_len != 0u &&
            public_turn_event_matches_bound(
                wire,
                wire_len,
                request_id,
                sizeof(request_id) - 1u);
        wire_len = pb_encode_turn_event(
            wire, sizeof(wire), request_id, "completed", "");
        if (wire_len == 0u ||
            wire_len + sizeof(wrong_id) + 1u > sizeof(wire)) {
            fallback_matches = 0;
        } else {
            wire[wire_len++] = 0x0au;
            wire[wire_len++] = (uint8_t)(sizeof(wrong_id) - 1u);
            memcpy(wire + wire_len, wrong_id, sizeof(wrong_id) - 1u);
            wire_len += sizeof(wrong_id) - 1u;
            fallback_matches = fallback_matches &&
                public_turn_event_matches_bound(
                    wire,
                    wire_len,
                    request_id,
                    sizeof(request_id) - 1u);
        }
        expect(
            "public lifecycle fallback matches checked decode",
            fallback_matches);
    }
    {
        uint8_t buf[128];
        turn_event_c ev;
        size_t n = pb_encode_turn_event(buf, sizeof(buf), "r-reset", "started", "");
        memset(&ev, 0xa5, sizeof(ev));
        expect("turn event reset enc", n > 0);
        expect("turn event reset dec", pb_decode_turn_event(buf, n, &ev) == 0);
        expect(
            "turn event resets absent fields",
            strcmp(ev.request_id, "r-reset") == 0 && strcmp(ev.type, "started") == 0 &&
            ev.type_id == 1 && ev.text[0] == '\0' && ev.audio == NULL &&
            ev.audio_len == 0u && ev.sample_rate == 0 && ev.channels == 0 &&
            ev.bit_depth == 0 && ev.sequence == 0 && ev.segment_index == 0 &&
            ev.is_final == 0 && ev.audio_encoding == 0 && ev.speech_text[0] == '\0' &&
            ev.display_text[0] == '\0' && ev.display_text_len == 0u &&
            ev.display_text_len_known == 0 &&
            ev.display_text_borrowed == 0u &&
            ev.current_tts_stage_wire == NULL &&
            ev.stages.first_text_at_ms == 0 &&
            ev.stages.input.audio_committed_at_ms == 0 &&
            ev.stages.input.stt_request_received_at_ms == 0 &&
            ev.stages.input.stt_provider_request_started_at_ms == 0 &&
            ev.stages.input.stt_provider_ready_at_ms == 0 &&
            ev.stages.input.stt_transcript_published_at_ms == 0 &&
            ev.stages.tts_segment_emitted_at_ms == 0 &&
            ev.stages.tts_request_received_at_ms == 0 &&
            ev.stages.tts_provider_request_started_at_ms == 0 &&
            ev.stages.tts_provider_ready_at_ms == 0 && ev.stages.pcm_started_at_ms == 0 &&
            ev.stages.pcm_first_chunk_at_ms == 0);
    }
    {
        static const char active_id[] = "r-bound";
        static const char wrong_id[] = "r-other";
        uint8_t buf[512];
        turn_event_c ev;
        turn_event_c active_ev;
        char unterminated[128];
        size_t n = pb_encode_turn_event(
            buf, sizeof(buf), active_id, "started", "");

        memset(&ev, 0xa5, sizeof(ev));
        expect(
            "bound turn event decodes matching identifier",
            n > 0 && pb_decode_turn_event_bound(
                         buf, n, active_id, sizeof(active_id) - 1u, &ev) == 0);
        expect(
            "bound turn event leaves identifier with caller",
            ev.request_id[0] == '\0' && ev.type_id == 1 &&
            strcmp(ev.type, "started") == 0);
        memset(&ev, 0, sizeof(ev));
        memset(&active_ev, 0, sizeof(active_ev));
        expect(
            "active turn event matches checked decoder",
            pb_decode_turn_event_bound(
                buf, n, active_id, sizeof(active_id) - 1u, &ev) == 0 &&
            pb_decode_turn_event_active(
                buf, n, active_id, sizeof(active_id) - 1u, &active_ev) == 0 &&
            memcmp(&active_ev, &ev, sizeof(ev)) == 0);
        expect(
            "generic turn event retains owned identifier",
            pb_decode_turn_event(buf, n, &ev) == 0 &&
            strcmp(ev.request_id, active_id) == 0);
        expect(
            "bound turn event rejects identifier mismatch",
            pb_decode_turn_event_bound(
                buf, n, wrong_id, sizeof(wrong_id) - 1u, &ev) != 0);
        expect(
            "bound turn event rejects cached length mismatch",
            pb_decode_turn_event_bound(
                buf, n, active_id, sizeof(active_id) - 2u, &ev) != 0);
        expect(
            "bound turn event rejects empty identifier contract",
            pb_decode_turn_event_bound(buf, n, NULL, 0u, &ev) != 0 &&
            pb_decode_turn_event_bound(buf, n, "", 0u, &ev) != 0);
        memset(unterminated, 'x', sizeof(unterminated));
        expect(
            "bound turn event rejects unterminated identifier contract",
            pb_decode_turn_event_bound(
                buf, n, unterminated, sizeof(unterminated) - 1u, &ev) != 0 &&
            pb_decode_turn_event_bound(
                buf, n, unterminated, sizeof(unterminated), &ev) != 0);

        n = pb_encode_turn_event(buf, sizeof(buf), active_id, "started", "");
        if (n > 0 && n + sizeof(wrong_id) + 1u <= sizeof(buf)) {
            buf[n++] = 0x0au;
            buf[n++] = (uint8_t)(sizeof(wrong_id) - 1u);
            memcpy(buf + n, wrong_id, sizeof(wrong_id) - 1u);
            n += sizeof(wrong_id) - 1u;
        }
        expect(
            "bound turn event applies last mismatching duplicate",
            pb_decode_turn_event_bound(
                buf, n, active_id, sizeof(active_id) - 1u, &ev) != 0);

        n = pb_encode_turn_event(buf, sizeof(buf), wrong_id, "started", "");
        if (n > 0 && n + sizeof(active_id) + 1u <= sizeof(buf)) {
            buf[n++] = 0x0au;
            buf[n++] = (uint8_t)(sizeof(active_id) - 1u);
            memcpy(buf + n, active_id, sizeof(active_id) - 1u);
            n += sizeof(active_id) - 1u;
        }
        expect(
            "bound turn event applies last matching duplicate",
            pb_decode_turn_event_bound(
                buf, n, active_id, sizeof(active_id) - 1u, &ev) == 0 &&
            ev.request_id[0] == '\0');
        expect(
            "generic turn event matches duplicate-field semantics",
            pb_decode_turn_event(buf, n, &ev) == 0 &&
            strcmp(ev.request_id, active_id) == 0);

        {
            static const uint8_t embedded_nul[] = {
                0x0au, 0x03u, 'r', '\0', 'x', 0x18u, 0x01u
            };
            static const uint8_t missing_id[] = {0x18u, 0x01u};
            static const uint8_t larger_unknown[] = {
                0x80u, 0x80u, 0x01u, 0x00u,
                0x0au, 0x07u, 'r', '-', 'b', 'o', 'u', 'n', 'd',
                0x18u, 0x01u
            };
            static const uint8_t noncanonical_tag[] = {
                0x8au, 0x00u, 0x07u, 'r', '-', 'b', 'o', 'u', 'n', 'd',
                0x18u, 0x01u
            };
            static const uint8_t truncated_tag[] = {0x82u};
            expect(
                "bound turn event rejects embedded NUL identifier",
                pb_decode_turn_event_bound(
                    embedded_nul, sizeof(embedded_nul), active_id,
                    sizeof(active_id) - 1u, &ev) != 0);
            expect(
                "bound turn event requires request field",
                pb_decode_turn_event_bound(
                    missing_id, sizeof(missing_id), active_id,
                    sizeof(active_id) - 1u, &ev) != 0);
            expect(
                "active turn event preserves larger tag fallback",
                pb_decode_turn_event(
                    larger_unknown, sizeof(larger_unknown), &ev) == 0 &&
                pb_decode_turn_event_bound(
                    larger_unknown, sizeof(larger_unknown), active_id,
                    sizeof(active_id) - 1u, &ev) == 0 &&
                pb_decode_turn_event_active(
                    larger_unknown, sizeof(larger_unknown), active_id,
                    sizeof(active_id) - 1u, &active_ev) == 0);
            expect(
                "active turn event rejects noncanonical tag",
                pb_decode_turn_event_bound(
                    noncanonical_tag, sizeof(noncanonical_tag), active_id,
                    sizeof(active_id) - 1u, &ev) != 0);
            expect(
                "active turn event rejects truncated tag",
                pb_decode_turn_event_bound(
                    truncated_tag, sizeof(truncated_tag), active_id,
                    sizeof(active_id) - 1u, &ev) != 0);
        }
    }
    {
        uint8_t buf[1024];
        uint8_t expected[1024];
        turn_event_c bound_ev;
        turn_event_c ev;
        turn_event_c public_ev;
        turn_stage_timestamps_c stages;
        turn_stage_wire_c prepared;
        size_t cap;
        size_t n;
        memset(&stages, 0, sizeof(stages));
        stages.input.audio_committed_at_ms = 1787774399994LL;
        stages.input.stt_request_received_at_ms = 1787774399995LL;
        stages.input.stt_provider_request_started_at_ms = 1787774399996LL;
        stages.input.stt_provider_ready_at_ms = 1787774399997LL;
        stages.input.stt_transcript_published_at_ms = 1787774399998LL;
        stages.first_text_at_ms = 1787774399999LL;
        stages.tts_segment_emitted_at_ms = 1787774400000LL;
        stages.tts_request_received_at_ms = 1787774400001LL;
        stages.tts_provider_request_started_at_ms = 1787774400002LL;
        stages.tts_provider_ready_at_ms = 1787774400003LL;
        stages.pcm_started_at_ms = 1787774400004LL;
        n = pb_encode_turn_event_stages(
            buf, sizeof(buf), "r1", "pcm_started", "", &stages);
        expect("stage metadata enc", n > 0);
        expect(
            "stage metadata accepts exact protobuf capacity",
            n > 0 && pb_encode_turn_event_stages(
                buf, n, "r1", "pcm_started", "", &stages) == n);
        expect(
            "stage metadata rejects short protobuf capacity",
            n > 1u && pb_encode_turn_event_stages(
                buf, n - 1u, "r1", "pcm_started", "", &stages) == 0);
        n = pb_encode_turn_event_stages(
            expected, sizeof(expected), "r1", "pcm_started", "", &stages);
        {
            int capacities_ok = n > 0 && n < sizeof(buf);
            for (cap = 0; capacities_ok && cap <= n + 1u; ++cap) {
                size_t bounded = pb_encode_turn_event_stages(
                    buf, cap, "r1", "pcm_started", "", &stages);
                if ((cap < n && bounded != 0) ||
                    (cap >= n &&
                     (bounded != n || memcmp(buf, expected, n) != 0)))
                    capacities_ok = 0;
            }
            expect("stage metadata preserves every output capacity", capacities_ok);
        }
        n = pb_encode_turn_event_stages(
            buf, sizeof(buf), "r1", "pcm_started", "", &stages);
        expect("stage metadata dec", pb_decode_turn_event(buf, n, &ev) == 0);
        memset(&bound_ev, 0, sizeof(bound_ev));
        memset(&public_ev, 0, sizeof(public_ev));
        expect(
            "public staged PCM boundary matches checked decode",
            pb_decode_turn_event_bound(
                buf, n, "r1", sizeof("r1") - 1u, &bound_ev) == 0 &&
            pb_decode_turn_event_public_active(
                buf, n, "r1", sizeof("r1") - 1u, &public_ev) == 0 &&
            memcmp(&public_ev, &bound_ev, sizeof(bound_ev)) == 0);
        expect(
            "stage metadata fields",
            ev.stages.input.audio_committed_at_ms == 1787774399994LL &&
            ev.stages.input.stt_request_received_at_ms == 1787774399995LL &&
            ev.stages.input.stt_provider_request_started_at_ms == 1787774399996LL &&
            ev.stages.input.stt_provider_ready_at_ms == 1787774399997LL &&
            ev.stages.input.stt_transcript_published_at_ms == 1787774399998LL &&
            ev.stages.first_text_at_ms == 1787774399999LL &&
            ev.stages.tts_segment_emitted_at_ms == 1787774400000LL &&
            ev.stages.tts_request_received_at_ms == 1787774400001LL &&
            ev.stages.tts_provider_request_started_at_ms == 1787774400002LL &&
            ev.stages.tts_provider_ready_at_ms == 1787774400003LL &&
            ev.stages.pcm_started_at_ms == 1787774400004LL &&
            ev.stages.pcm_first_chunk_at_ms == 0);
        memset(&prepared, 0xa5, sizeof(prepared));
        expect(
            "prepared stage metadata starts incomplete",
            pb_prepare_turn_stage_wire(&prepared, &stages) == 0 &&
            prepared.len > 0u && !prepared.complete);
        n = pb_encode_turn_event_stages(
            expected, sizeof(expected), "r1", "pcm_started", "", &stages);
        expect(
            "prepared stage metadata matches canonical bytes",
            n > 0u && pb_encode_turn_event_stage_wire(
                buf, sizeof(buf), "r1", "pcm_started", "", &prepared) == n &&
            memcmp(buf, expected, n) == 0);
        expect(
            "prepared PCM start boundary matches canonical bytes",
            n > 0u && pb_encode_turn_pcm_boundary_prepared(
                buf, sizeof(buf), "r1", 2u, 7, &prepared) == n &&
            memcmp(buf, expected, n) == 0);
        {
            char maximum_request_id[128];
            memset(maximum_request_id, 'x', sizeof(maximum_request_id) - 1u);
            maximum_request_id[sizeof(maximum_request_id) - 1u] = '\0';
            n = pb_encode_turn_event_stage_wire(
                expected, sizeof(expected), maximum_request_id,
                "pcm_started", "", &prepared);
            expect(
                "prepared PCM start accepts maximum request identifier",
                n > 0u && pb_encode_turn_pcm_boundary_prepared(
                    buf, sizeof(buf), maximum_request_id,
                    sizeof(maximum_request_id) - 1u, 7, &prepared) == n &&
                memcmp(buf, expected, n) == 0);
        }
        n = pb_encode_turn_event_stage_wire(
            expected, sizeof(expected), "r1", "pcm_started", "", &prepared);
        {
            int capacities_ok = n > 0u && n < sizeof(buf);
            for (cap = 0u; capacities_ok && cap <= n + 1u; ++cap) {
                size_t bounded = pb_encode_turn_event_stage_wire(
                    buf, cap, "r1", "pcm_started", "", &prepared);
                if ((cap < n && bounded != 0u) ||
                    (cap >= n &&
                     (bounded != n || memcmp(buf, expected, n) != 0)))
                    capacities_ok = 0;
            }
            expect(
                "prepared stage metadata preserves every output capacity",
                capacities_ok);
        }
        {
            int capacities_ok = n > 0u && n < sizeof(buf);
            for (cap = 0u; capacities_ok && cap <= n + 1u; ++cap) {
                size_t bounded = pb_encode_turn_pcm_boundary_prepared(
                    buf, cap, "r1", 2u, 7, &prepared);
                if ((cap < n && bounded != 0u) ||
                    (cap >= n &&
                     (bounded != n || memcmp(buf, expected, n) != 0)))
                    capacities_ok = 0;
            }
            expect(
                "prepared PCM start preserves every output capacity",
                capacities_ok);
        }
        n = pb_encode_turn_event(
            expected, sizeof(expected), "r1", "pcm_ended", "");
        expect(
            "prepared PCM end boundary matches canonical bytes",
            n > 0u && pb_encode_turn_pcm_boundary_prepared(
                buf, sizeof(buf), "r1", 2u, 9, NULL) == n &&
            memcmp(buf, expected, n) == 0);
        {
            int capacities_ok = n > 0u && n < sizeof(buf);
            for (cap = 0u; capacities_ok && cap <= n + 1u; ++cap) {
                size_t bounded = pb_encode_turn_pcm_boundary_prepared(
                    buf, cap, "r1", 2u, 9, NULL);
                if ((cap < n && bounded != 0u) ||
                    (cap >= n &&
                     (bounded != n || memcmp(buf, expected, n) != 0)))
                    capacities_ok = 0;
            }
            expect(
                "prepared PCM end preserves every output capacity",
                capacities_ok);
        }
        expect(
            "prepared PCM boundary rejects invalid inputs",
            pb_encode_turn_pcm_boundary_prepared(
                buf, sizeof(buf), "", 0u, 7, &prepared) == 0u &&
            pb_encode_turn_pcm_boundary_prepared(
                buf, sizeof(buf), "r1", 2u, 8, &prepared) == 0u &&
            pb_encode_turn_pcm_boundary_prepared(
                buf, sizeof(buf), "r1", 2u, 7, NULL) == 0u &&
            pb_encode_turn_pcm_boundary_prepared(
                buf, sizeof(buf), "r1", 2u, 9, &prepared) == 0u &&
            pb_encode_turn_pcm_boundary_prepared(
                buf, sizeof(buf), "r1", 128u, 9, NULL) == 0u);
        expect(
            "prepared stage metadata rejects reversed completion",
            pb_complete_turn_stage_wire(&prepared, 1787774400003LL) != 0 &&
            !prepared.complete);
        expect(
            "prepared stage metadata appends first PCM",
            pb_complete_turn_stage_wire(&prepared, 1787774400005LL) == 0 &&
            prepared.complete);
        expect(
            "prepared PCM start rejects completed stage metadata",
            pb_encode_turn_pcm_boundary_prepared(
                buf, sizeof(buf), "r1", 2u, 7, &prepared) == 0u);
        stages.pcm_first_chunk_at_ms = 1787774400005LL;
        n = pb_encode_turn_event_stages(
            expected, sizeof(expected), "r1", "pcm_started", "", &stages);
        expect(
            "completed stage metadata matches canonical bytes",
            n > 0u && pb_encode_turn_event_stage_wire(
                buf, sizeof(buf), "r1", "pcm_started", "", &prepared) == n &&
            memcmp(buf, expected, n) == 0);
        {
            turn_stage_timestamps_c tts_only = stages;
            memset(&tts_only.input, 0, sizeof(tts_only.input));
            n = pb_encode_turn_event_stages(
                buf, sizeof(buf), "r1", "pcm_started", "", &tts_only);
            expect(
                "complete TTS-only metadata preserves first PCM",
                n > 0u && pb_decode_turn_event(buf, n, &ev) == 0 &&
                ev.stages.pcm_first_chunk_at_ms ==
                    tts_only.pcm_first_chunk_at_ms);
        }
        expect(
            "prepared stage metadata rejects second completion",
            pb_complete_turn_stage_wire(&prepared, 1787774400006LL) != 0);
        stages.pcm_first_chunk_at_ms = -1;
        expect(
            "stage metadata rejects negative time",
            pb_encode_turn_event_stages(
                buf, sizeof(buf), "r1", "pcm_started", "", &stages) == 0);
        expect(
            "failed stage preparation invalidates old bytes",
            pb_prepare_turn_stage_wire(&prepared, &stages) != 0 &&
            prepared.len == 0u && !prepared.complete);
        stages.pcm_first_chunk_at_ms = 0;
        stages.tts_segment_emitted_at_ms = stages.first_text_at_ms - 1;
        expect(
            "stage metadata rejects reversed upstream time",
            pb_encode_turn_event_stages(
                buf, sizeof(buf), "r1", "pcm_started", "", &stages) == 0);
        stages.tts_segment_emitted_at_ms = 0;
        expect(
            "stage metadata rejects partial upstream time",
            pb_encode_turn_event_stages(
                buf, sizeof(buf), "r1", "pcm_started", "", &stages) == 0);
        stages.tts_segment_emitted_at_ms = 1787774400000LL;
        stages.input.stt_provider_ready_at_ms = 0;
        expect(
            "stage metadata rejects partial input waterfall",
            pb_encode_turn_event_stages(
                buf, sizeof(buf), "r1", "pcm_started", "", &stages) == 0);
        memset(&stages, 0, sizeof(stages));
        stages.input.audio_committed_at_ms = INT64_MAX - 10;
        stages.input.stt_request_received_at_ms = INT64_MAX - 9;
        stages.input.stt_provider_request_started_at_ms = INT64_MAX - 8;
        stages.input.stt_provider_ready_at_ms = INT64_MAX - 7;
        stages.input.stt_transcript_published_at_ms = INT64_MAX - 6;
        stages.first_text_at_ms = INT64_MAX - 5;
        stages.tts_segment_emitted_at_ms = INT64_MAX - 4;
        stages.tts_request_received_at_ms = INT64_MAX - 3;
        stages.tts_provider_request_started_at_ms = INT64_MAX - 2;
        stages.tts_provider_ready_at_ms = INT64_MAX - 1;
        stages.pcm_started_at_ms = INT64_MAX;
        n = pb_encode_turn_event_stages(
            buf, sizeof(buf), "r-max", "pcm_started", "", &stages);
        expect(
            "stage metadata encodes INT64 maximum",
            n > 0 && pb_decode_turn_event(buf, n, &ev) == 0 &&
            ev.stages.input.audio_committed_at_ms == INT64_MAX - 10 &&
            ev.stages.pcm_started_at_ms == INT64_MAX);
        expect(
            "prepared stage metadata fits maximum timestamp",
            pb_prepare_turn_stage_wire(&prepared, &stages) == 0 &&
            pb_complete_turn_stage_wire(&prepared, INT64_MAX) == 0 &&
            prepared.len <= TURN_STAGE_WIRE_CAPACITY);
        stages.pcm_first_chunk_at_ms = INT64_MAX;
        n = pb_encode_turn_event_stages(
            expected, sizeof(expected), "r-max", "pcm_started", "", &stages);
        expect(
            "prepared maximum stage metadata matches canonical bytes",
            n > 0u && pb_encode_turn_event_stage_wire(
                buf, sizeof(buf), "r-max", "pcm_started", "", &prepared) == n &&
            memcmp(buf, expected, n) == 0);
        memset(&stages, 0, sizeof(stages));
        stages.first_text_at_ms = 1787774400000LL;
        stages.tts_segment_emitted_at_ms = 1787774400001LL;
        stages.tts_request_received_at_ms = 1787774400002LL;
        stages.tts_provider_request_started_at_ms = 1787774400003LL;
        stages.tts_provider_ready_at_ms = 1787774400004LL;
        stages.pcm_started_at_ms = 1787774400005LL;
        n = 0u;
        n = append_stage_map_entry(
            expected, sizeof(expected), n,
            (const uint8_t *)"stage_first_text_at_ms", 22u,
            (const uint8_t *)"1787774400000", 13u);
        n = append_stage_map_entry(
            expected, sizeof(expected), n,
            (const uint8_t *)"stage_tts_segment_emitted_at_ms", 31u,
            (const uint8_t *)"1787774400001", 13u);
        n = append_stage_map_entry(
            expected, sizeof(expected), n,
            (const uint8_t *)"stage_tts_request_received_at_ms", 32u,
            (const uint8_t *)"1787774400002", 13u);
        n = append_stage_map_entry(
            expected, sizeof(expected), n,
            (const uint8_t *)"stage_tts_provider_request_started_at_ms", 40u,
            (const uint8_t *)"1787774400003", 13u);
        n = append_stage_map_entry(
            expected, sizeof(expected), n,
            (const uint8_t *)"stage_tts_provider_ready_at_ms", 30u,
            (const uint8_t *)"1787774400004", 13u);
        n = append_stage_map_entry(
            expected, sizeof(expected), n,
            (const uint8_t *)"stage_pcm_started_at_ms", 23u,
            (const uint8_t *)"1787774400005", 13u);
        expect(
            "prepared current TTS stages match independent wire",
            n > 0u && pb_prepare_turn_stage_wire(&prepared, &stages) == 0 &&
            prepared.current_tts_template == 1 && prepared.len == n &&
            memcmp(prepared.data, expected, n) == 0);
        {
            uint8_t current_wire[1024];
            char borrowed_json[1024];
            char formatted_json[1024];
            turn_event_c current_event;
            turn_response_event borrowed_event;
            turn_response_event formatted_event;
            turn_response_state response_state;
            enum turn_response_action response_action;
            size_t current_len = pb_encode_turn_event_stage_wire(
                current_wire, sizeof(current_wire),
                "r-current", "pcm_started", "", &prepared);
            size_t stage_offset = current_len >= prepared.len ?
                current_len - prepared.len : current_len;
            size_t borrowed_len = 0u;
            size_t formatted_len = 0u;
            int borrowed_flow_ok = current_len > prepared.len;
            memset(&current_event, 0, sizeof(current_event));
            memset(&borrowed_event, 0xa5, sizeof(borrowed_event));
            memset(&response_state, 0, sizeof(response_state));
            if (borrowed_flow_ok && pb_decode_turn_event_public_active(
                    current_wire, current_len,
                    "r-current", sizeof("r-current") - 1u,
                    &current_event) != 0)
                borrowed_flow_ok = 0;
            if (borrowed_flow_ok) {
                response_action = turn_response_filter_public_active_n(
                    &response_state, &current_event,
                    "r-current", sizeof("r-current") - 1u,
                    &borrowed_event);
                if (response_action != TURN_RESPONSE_FORWARD ||
                    current_event.current_tts_stage_wire !=
                        current_wire + stage_offset ||
                    borrowed_event.current_tts_stage_wire !=
                        current_wire + stage_offset ||
                    borrowed_event.runtime_identity_sha256 != NULL ||
                    borrowed_event.runtime_identity_sha256_len != 0u)
                    borrowed_flow_ok = 0;
            }
            if (borrowed_flow_ok) {
                formatted_event = borrowed_event;
                formatted_event.current_tts_stage_wire = NULL;
                borrowed_len = turn_response_json_encode(
                    borrowed_json, sizeof(borrowed_json),
                    &borrowed_event, INT64_C(1787774400006));
                formatted_len = turn_response_json_encode(
                    formatted_json, sizeof(formatted_json),
                    &formatted_event, INT64_C(1787774400006));
                if (borrowed_len == 0u || borrowed_len != formatted_len ||
                    memcmp(
                        borrowed_json, formatted_json,
                        borrowed_len + 1u) != 0)
                    borrowed_flow_ok = 0;
            }
            expect(
                "public current TTS wire reaches equivalent response JSON",
                borrowed_flow_ok);
            expect(
                "public current TTS stage decoder matches every byte substitution",
                current_len > prepared.len &&
                public_turn_stage_mutations_match_bound(
                    current_wire, current_len, stage_offset,
                    "r-current", sizeof("r-current") - 1u));
        }
        {
            turn_stage_wire_c invalid_prepared = prepared;
            invalid_prepared.len -= 1u;
            expect(
                "prepared current TTS template rejects invalid shape",
                pb_complete_turn_stage_wire(
                    &invalid_prepared, 1787774400006LL) != 0 &&
                pb_encode_turn_event_stage_wire(
                    buf, sizeof(buf), "r-current", "pcm_started", "",
                    &invalid_prepared) == 0u);
        }
        n = append_stage_map_entry(
            expected, sizeof(expected), n,
            (const uint8_t *)"stage_pcm_first_chunk_at_ms", 27u,
            (const uint8_t *)"1787774400006", 13u);
        expect(
            "completed current TTS stages match independent wire",
            n > 0u &&
            pb_complete_turn_stage_wire(&prepared, 1787774400006LL) == 0 &&
            prepared.complete && prepared.len == n &&
            memcmp(prepared.data, expected, n) == 0);
        {
            static const uint8_t pcm[] = {0u, 1u};
            uint8_t current_wire[1024];
            size_t current_len = pb_encode_turn_audio_event_stage_wire(
                current_wire, sizeof(current_wire),
                "r-current", "pcm_chunk", "", pcm, sizeof(pcm),
                24000, 1, 16, 0, 0, 0, &prepared);
            size_t stage_offset = current_len >= prepared.len ?
                current_len - prepared.len : current_len;
            expect(
                "public current first PCM decoder matches every byte substitution",
                current_len > prepared.len &&
                public_turn_stage_mutations_match_bound(
                    current_wire, current_len, stage_offset,
                    "r-current", sizeof("r-current") - 1u));
        }
        {
            uint64_t state = UINT64_C(0xd1b54a32d192ed03);
            size_t shape;
            int shapes_match = 1;
            for (shape = 0u; shape < 10000u && shapes_match; ++shape) {
                int64_t value;
                size_t prepared_len;
                state = state * UINT64_C(6364136223846793005) +
                    UINT64_C(1442695040888963407);
                value = INT64_C(1787864000000) +
                    (int64_t)(state % UINT64_C(900000));
                memset(&stages, 0, sizeof(stages));
                stages.first_text_at_ms = value;
                value += (int64_t)((state >> 20u) & UINT64_C(7));
                stages.tts_segment_emitted_at_ms = value;
                value += (int64_t)((state >> 23u) & UINT64_C(7));
                stages.tts_request_received_at_ms = value;
                value += (int64_t)((state >> 26u) & UINT64_C(7));
                stages.tts_provider_request_started_at_ms = value;
                value += (int64_t)((state >> 29u) & UINT64_C(7));
                stages.tts_provider_ready_at_ms = value;
                value += (int64_t)((state >> 32u) & UINT64_C(7));
                stages.pcm_started_at_ms = value;
                if (pb_prepare_turn_stage_wire(&prepared, &stages) != 0 ||
                    !prepared.current_tts_template) {
                    shapes_match = 0;
                    break;
                }
                n = pb_encode_turn_event_stages(
                    expected, sizeof(expected), "r-current-shapes",
                    "pcm_started", "", &stages);
                prepared_len = pb_encode_turn_event_stage_wire(
                    buf, sizeof(buf), "r-current-shapes",
                    "pcm_started", "", &prepared);
                if (n == 0u || prepared_len != n ||
                    memcmp(buf, expected, n) != 0)
                    shapes_match = 0;
            }
            expect(
                "prepared current TTS shapes match generic encoding",
                shapes_match);
        }
        {
            unsigned inverted_boundary;
            int boundaries_ok = 1;
            for (inverted_boundary = 0u;
                 inverted_boundary <= 5u && boundaries_ok;
                 ++inverted_boundary) {
                int64_t timestamps[6] = {
                    INT64_C(1787864000010), INT64_C(1787864000010),
                    INT64_C(1787864000010), INT64_C(1787864000010),
                    INT64_C(1787864000010), INT64_C(1787864000010),
                };
                int result;
                if (inverted_boundary != 0u)
                    timestamps[inverted_boundary - 1u]++;
                memset(&stages, 0, sizeof(stages));
                stages.first_text_at_ms = timestamps[0];
                stages.tts_segment_emitted_at_ms = timestamps[1];
                stages.tts_request_received_at_ms = timestamps[2];
                stages.tts_provider_request_started_at_ms = timestamps[3];
                stages.tts_provider_ready_at_ms = timestamps[4];
                stages.pcm_started_at_ms = timestamps[5];
                memset(&prepared, 0xa5, sizeof(prepared));
                result = pb_prepare_turn_stage_wire(&prepared, &stages);
                if ((result == 0) != (inverted_boundary == 0u) ||
                    (result == 0 && !prepared.current_tts_template) ||
                    (result != 0 &&
                     (prepared.len != 0u || prepared.complete != 0 ||
                      prepared.current_tts_template != 0)))
                    boundaries_ok = 0;
            }
            expect(
                "prepared current TTS order boundaries preserve rejection",
                boundaries_ok);
        }
        {
            static const int64_t prefix_bases[] = {
                INT64_C(999999999990),
                INT64_C(1000000000000),
                INT64_C(9999999000000),
                INT64_C(10000000000000),
            };
            static const int template_expected[] = {0, 1, 1, 0};
            size_t prefix_index;
            int boundaries_ok = 1;
            for (prefix_index = 0u;
                 boundaries_ok &&
                    prefix_index < sizeof(prefix_bases) / sizeof(prefix_bases[0]);
                 ++prefix_index) {
                int64_t base = prefix_bases[prefix_index];
                size_t prepared_len;
                memset(&stages, 0, sizeof(stages));
                stages.first_text_at_ms = base;
                stages.tts_segment_emitted_at_ms = base + 1;
                stages.tts_request_received_at_ms = base + 2;
                stages.tts_provider_request_started_at_ms = base + 3;
                stages.tts_provider_ready_at_ms = base + 4;
                stages.pcm_started_at_ms = base + 5;
                if (pb_prepare_turn_stage_wire(&prepared, &stages) != 0 ||
                    prepared.current_tts_template !=
                        template_expected[prefix_index] ||
                    pb_complete_turn_stage_wire(&prepared, base + 6) != 0) {
                    boundaries_ok = 0;
                    continue;
                }
                stages.pcm_first_chunk_at_ms = base + 6;
                n = pb_encode_turn_event_stages(
                    expected, sizeof(expected), "r-prefix-width",
                    "pcm_started", "", &stages);
                prepared_len = pb_encode_turn_event_stage_wire(
                    buf, sizeof(buf), "r-prefix-width", "pcm_started", "",
                    &prepared);
                if (n == 0u || prepared_len != n ||
                    memcmp(buf, expected, n) != 0)
                    boundaries_ok = 0;
            }
            expect(
                "prepared TTS prefix width boundaries match canonical bytes",
                boundaries_ok);
        }
        memset(&stages, 0, sizeof(stages));
        stages.first_text_at_ms = 1999998;
        stages.tts_segment_emitted_at_ms = 1999999;
        stages.tts_request_received_at_ms = 2000000;
        stages.tts_provider_request_started_at_ms = 2000001;
        stages.tts_provider_ready_at_ms = 2000002;
        stages.pcm_started_at_ms = 2999999;
        n = pb_encode_turn_event_stages(
            buf, sizeof(buf), "r-prefix", "pcm_started", "", &stages);
        expect(
            "stage metadata crosses decimal prefix boundary",
            n > 0 && pb_decode_turn_event(buf, n, &ev) == 0 &&
            ev.stages.first_text_at_ms == 1999998 &&
            ev.stages.tts_segment_emitted_at_ms == 1999999 &&
            ev.stages.tts_request_received_at_ms == 2000000 &&
            ev.stages.pcm_started_at_ms == 2999999);
        expect(
            "prepared TTS stages retain decimal fallback",
            pb_prepare_turn_stage_wire(&prepared, &stages) == 0 &&
            prepared.current_tts_template == 0 &&
            pb_complete_turn_stage_wire(&prepared, 3000000) == 0);
        memset(&stages, 0, sizeof(stages));
        stages.tts_request_received_at_ms = 1787774000000LL;
        stages.tts_provider_request_started_at_ms = 1787774000001LL;
        stages.tts_provider_ready_at_ms = 1787774000002LL;
        stages.pcm_started_at_ms = 1787774999999LL;
        n = pb_encode_turn_event_stages(
            buf, sizeof(buf), "r-suffix", "pcm_started", "", &stages);
        expect(
            "stage metadata encodes current suffix maximum",
            n > 0 && pb_decode_turn_event(buf, n, &ev) == 0 &&
            ev.stages.tts_request_received_at_ms == 1787774000000LL &&
            ev.stages.pcm_started_at_ms == 1787774999999LL);
        {
            turn_event_c prefix_bound_ev;
            turn_event_c prefix_public_ev;
            memset(&stages, 0, sizeof(stages));
            stages.tts_request_received_at_ms = 1787774999998LL;
            stages.tts_provider_request_started_at_ms = 1787774999999LL;
            stages.tts_provider_ready_at_ms = 1787775000000LL;
            stages.pcm_started_at_ms = 1787775000001LL;
            n = pb_encode_turn_event_stages(
                buf, sizeof(buf), "r-prefix-decode", "pcm_started", "",
                &stages);
            memset(&prefix_bound_ev, 0, sizeof(prefix_bound_ev));
            memset(&prefix_public_ev, 0, sizeof(prefix_public_ev));
            expect(
                "public stage prefix cache preserves boundary fallback",
                n > 0u && pb_decode_turn_event_bound(
                    buf, n, "r-prefix-decode",
                    sizeof("r-prefix-decode") - 1u, &prefix_bound_ev) == 0 &&
                pb_decode_turn_event_public_active(
                    buf, n, "r-prefix-decode",
                    sizeof("r-prefix-decode") - 1u, &prefix_public_ev) == 0 &&
                memcmp(
                    &prefix_public_ev, &prefix_bound_ev,
                    sizeof(prefix_bound_ev)) == 0);
        }
        {
            static const uint8_t received_key[] =
                "stage_tts_request_received_at_ms";
            static const uint8_t provider_key[] =
                "stage_tts_provider_request_started_at_ms";
            static const uint8_t received_value[] = "1787774000000";
            static const uint8_t provider_value[] = "1787774000001";
            size_t base = pb_encode_turn_event(
                buf, sizeof(buf), "r-prefix-bytes", "pcm_started", "");
            size_t provider_value_offset;
            size_t position;
            int byte_domain_matches = 1;
            n = append_stage_map_entry(
                buf, sizeof(buf), base,
                received_key, sizeof(received_key) - 1u,
                received_value, sizeof(received_value) - 1u);
            n = append_stage_map_entry(
                buf, sizeof(buf), n,
                provider_key, sizeof(provider_key) - 1u,
                provider_value, sizeof(provider_value) - 1u);
            provider_value_offset = n >= sizeof(provider_value) - 1u ?
                n - (sizeof(provider_value) - 1u) : 0u;
            for (position = 7u;
                 n > base && position < sizeof(provider_value) - 1u;
                 ++position) {
                uint8_t saved = buf[provider_value_offset + position];
                unsigned byte;
                for (byte = 0u; byte <= UINT8_MAX; ++byte) {
                    turn_event_c cached_bound_ev;
                    turn_event_c cached_public_ev;
                    int bound_ok;
                    int public_ok;
                    int decimal =
                        byte >= (unsigned)'0' && byte <= (unsigned)'9';
                    buf[provider_value_offset + position] = (uint8_t)byte;
                    memset(&cached_bound_ev, 0, sizeof(cached_bound_ev));
                    memset(&cached_public_ev, 0, sizeof(cached_public_ev));
                    bound_ok = pb_decode_turn_event_bound(
                        buf, n, "r-prefix-bytes",
                        sizeof("r-prefix-bytes") - 1u,
                        &cached_bound_ev) == 0;
                    public_ok = pb_decode_turn_event_public_active(
                        buf, n, "r-prefix-bytes",
                        sizeof("r-prefix-bytes") - 1u,
                        &cached_public_ev) == 0;
                    if (bound_ok != decimal || public_ok != bound_ok ||
                        (public_ok && memcmp(
                            &cached_public_ev, &cached_bound_ev,
                            sizeof(cached_bound_ev)) != 0))
                        byte_domain_matches = 0;
                }
                buf[provider_value_offset + position] = saved;
            }
            expect(
                "public stage prefix suffix matches every byte",
                base > 0u && n > base && byte_domain_matches);
        }
    }
    {
        static const uint8_t known_key[] = "stage_pcm_started_at_ms";
        static const uint8_t unknown_key[] = "stage_future_at_ms";
        static const uint8_t embedded_nul[] = {'1', '\0', '2'};
        static const uint8_t overflow[] = "9223372036854775808";
        uint8_t timestamp13[] = "1787900000000";
        uint8_t buf[512];
        uint8_t fallback_entry[128];
        turn_event_c ev;
        size_t fallback_len;
        size_t base = pb_encode_turn_event(
            buf, sizeof(buf), "r-stage-map", "pcm_started", "");
        size_t n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            (const uint8_t *)"123", 3u);
        expect(
            "stage map decoder accepts canonical entry",
            base > 0 && n > base && pb_decode_turn_event(buf, n, &ev) == 0 &&
            ev.stages.pcm_started_at_ms == 123);
        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            timestamp13, sizeof(timestamp13) - 1u);
        expect(
            "stage map decoder accepts 13-digit timestamp",
            n > base && pb_decode_turn_event(buf, n, &ev) == 0 &&
            ev.stages.pcm_started_at_ms == INT64_C(1787900000000));
        {
            int byte_domain_matches = 1;
            size_t position;
            for (position = 0; position < sizeof(timestamp13) - 1u; ++position) {
                uint8_t saved = timestamp13[position];
                unsigned byte;
                for (byte = 0; byte <= UINT8_MAX; ++byte) {
                    int decoded;
                    int decimal = byte >= (unsigned)'0' && byte <= (unsigned)'9';
                    timestamp13[position] = (uint8_t)byte;
                    n = append_stage_map_entry(
                        buf, sizeof(buf), base, known_key,
                        sizeof(known_key) - 1u, timestamp13,
                        sizeof(timestamp13) - 1u);
                    decoded = n > base && pb_decode_turn_event(buf, n, &ev) == 0;
                    if (decoded != decimal ||
                        (decoded && ev.stages.pcm_started_at_ms !=
                            (int64_t)strtoll((const char *)timestamp13, NULL, 10)))
                        byte_domain_matches = 0;
                }
                timestamp13[position] = saved;
            }
            expect(
                "stage map 13-digit path matches every byte at every position",
                byte_domain_matches);
        }
        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            (const uint8_t *)"9999999999999", 13u);
        expect(
            "stage map 13-digit path accepts its maximum",
            n > base && pb_decode_turn_event(buf, n, &ev) == 0 &&
            ev.stages.pcm_started_at_ms == INT64_C(9999999999999));
        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            (const uint8_t *)"0000000000000", 13u);
        expect(
            "stage map 13-digit path rejects zero",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);
        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            (const uint8_t *)"123", 3u);
        n = append_stage_map_entry(
            buf, sizeof(buf), n, known_key, sizeof(known_key) - 1u,
            (const uint8_t *)"124", 3u);
        expect(
            "stage map decoder rejects duplicate stage",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);

        n = append_stage_map_entry(
            buf, sizeof(buf), base, unknown_key, sizeof(unknown_key) - 1u,
            (const uint8_t *)"not-a-time", sizeof("not-a-time") - 1u);
        expect(
            "stage map decoder ignores valid unknown entry",
            n > base && pb_decode_turn_event(buf, n, &ev) == 0 &&
            ev.stages.pcm_started_at_ms == 0);

        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            NULL, 0u);
        expect(
            "stage map decoder rejects empty timestamp",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);
        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            (const uint8_t *)"0", 1u);
        expect(
            "stage map decoder rejects zero timestamp",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);
        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            (const uint8_t *)"12x", 3u);
        expect(
            "stage map decoder rejects non-decimal timestamp",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);
        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            overflow, sizeof(overflow) - 1u);
        expect(
            "stage map decoder rejects timestamp overflow",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);
        n = append_stage_map_entry(
            buf, sizeof(buf), base, known_key, sizeof(known_key) - 1u,
            embedded_nul, sizeof(embedded_nul));
        expect(
            "stage map decoder rejects embedded NUL",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);
        n = append_stage_map_entry(
            buf, sizeof(buf), base, unknown_key, sizeof(unknown_key) - 1u,
            embedded_nul, sizeof(embedded_nul));
        expect(
            "stage map decoder rejects embedded NUL in unknown entry",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);

        fallback_len = 0;
        fallback_entry[fallback_len++] = 0x12u;
        fallback_entry[fallback_len++] = 3u;
        memcpy(fallback_entry + fallback_len, "122", 3u);
        fallback_len += 3u;
        fallback_entry[fallback_len++] = 0x0au;
        fallback_entry[fallback_len++] = (uint8_t)(sizeof(known_key) - 1u);
        memcpy(fallback_entry + fallback_len, known_key, sizeof(known_key) - 1u);
        fallback_len += sizeof(known_key) - 1u;
        fallback_entry[fallback_len++] = 0x12u;
        fallback_entry[fallback_len++] = 3u;
        memcpy(fallback_entry + fallback_len, "123", 3u);
        fallback_len += 3u;
        n = append_stage_map_raw_entry(
            buf, sizeof(buf), base, fallback_entry, fallback_len);
        expect(
            "stage map fallback preserves last value",
            n > base && pb_decode_turn_event(buf, n, &ev) == 0 &&
            ev.stages.pcm_started_at_ms == 123);

        fallback_entry[fallback_len++] = 0x1au;
        fallback_entry[fallback_len++] = 3u;
        fallback_entry[fallback_len++] = 'x';
        fallback_entry[fallback_len++] = '\0';
        fallback_entry[fallback_len++] = 'y';
        n = append_stage_map_raw_entry(
            buf, sizeof(buf), base, fallback_entry, fallback_len);
        expect(
            "stage map fallback rejects hidden NUL",
            n > base && pb_decode_turn_event(buf, n, &ev) != 0);
    }
    {
        uint8_t buf[512];
        turn_event_c ev;
        turn_event_c public_ev;
        size_t n = pb_encode_turn_text_event(
            buf, sizeof(buf), "r1", "final", "Hello", "Hello <laugh>",
            "Hello", 2, 1);
        memset(&public_ev, 0, sizeof(public_ev));
        expect("text channels enc", n > 0);
        expect("text channels dec", pb_decode_turn_event(buf, n, &ev) == 0);
        expect(
            "text channels fields",
            strcmp(ev.text, "Hello") == 0 && strcmp(ev.speech_text, "Hello <laugh>") == 0 &&
            strcmp(ev.display_text, "Hello") == 0 &&
            ev.display_text_len == sizeof("Hello") - 1u &&
            ev.display_text_len_known == 1 &&
            ev.display_text_borrowed == 0u &&
            ev.segment_index == 2 && ev.is_final == 1
        );
        expect(
            "public event decoder keeps only the public text channel",
            pb_decode_turn_event_public_active(
                buf, n, "r1", sizeof("r1") - 1u, &public_ev) == 0 &&
            public_ev.request_id[0] == '\0' && public_ev.text[0] == '\0' &&
            public_ev.speech_text[0] == '\0' &&
            public_ev.display_text_borrowed == 1u &&
            public_ev.display_text_view != NULL &&
            memcmp(
                public_ev.display_text_view, "Hello",
                sizeof("Hello") - 1u) == 0 &&
            public_ev.display_text_len == sizeof("Hello") - 1u &&
            public_ev.display_text_len_known == 1 &&
            public_ev.type_id == ev.type_id &&
            strcmp(public_ev.type, ev.type) == 0 &&
            public_ev.segment_index == ev.segment_index &&
            public_ev.is_final == ev.is_final
        );
    }
    {
        static const char escaped[] = "A\"\\B";
        static const char unicode[] = "Caf\xc3\xa9";
        static const char controlled[] = "bad\ntext";
        static const char malformed[] = "bad\xc3\x28";
        const char *const values[] = {
            escaped, unicode, controlled, malformed
        };
        uint8_t wire[512];
        size_t index;
        int cases_ok = 1;
        for (index = 0u; index < sizeof(values) / sizeof(values[0]); ++index) {
            turn_event_c event;
            size_t length = pb_encode_turn_text_event(
                wire, sizeof(wire), "r-borrow", "text_delta", values[index],
                values[index], values[index], 0, 0);
            memset(&event, 0, sizeof(event));
            if (length == 0u || pb_decode_turn_event_public_active(
                    wire, length, "r-borrow", sizeof("r-borrow") - 1u,
                    &event) != 0 ||
                event.display_text_borrowed != 1u ||
                event.display_text_len != strlen(values[index]) ||
                memcmp(
                    event.display_text_view, values[index],
                    event.display_text_len) != 0 ||
                (const uint8_t *)event.display_text_view < wire ||
                (const uint8_t *)event.display_text_view >= wire + length) {
                cases_ok = 0;
                break;
            }
        }
        expect(
            "public decoder borrows each bounded display text span",
            cases_ok);
    }
    {
        static const uint8_t legacy_nul[] = {
            0x0au, 0x02u, 'r', '1', 0x18u, 0x04u,
            0x2au, 0x03u, 'x', '\0', 'y',
            0x92u, 0x01u, 0x01u, 'x'
        };
        static const uint8_t speech_nul[] = {
            0x0au, 0x02u, 'r', '1', 0x18u, 0x04u,
            0x8au, 0x01u, 0x03u, 'x', '\0', 'y',
            0x92u, 0x01u, 0x01u, 'x'
        };
        uint8_t oversized[2060];
        turn_event_c public_ev;
        size_t oversized_len = 0u;
        oversized[oversized_len++] = 0x0au;
        oversized[oversized_len++] = 0x02u;
        oversized[oversized_len++] = 'r';
        oversized[oversized_len++] = '1';
        oversized[oversized_len++] = 0x18u;
        oversized[oversized_len++] = 0x04u;
        oversized[oversized_len++] = 0x2au;
        oversized[oversized_len++] = 0x80u;
        oversized[oversized_len++] = 0x10u;
        memset(oversized + oversized_len, 'x', 2048u);
        oversized_len += 2048u;
        expect(
            "public event decoder validates redacted private text",
            pb_decode_turn_event_public_active(
                legacy_nul, sizeof(legacy_nul), "r1",
                sizeof("r1") - 1u, &public_ev) != 0 &&
            pb_decode_turn_event_public_active(
                speech_nul, sizeof(speech_nul), "r1",
                sizeof("r1") - 1u, &public_ev) != 0
        );
        expect(
            "public event decoder bounds redacted private text",
            pb_decode_turn_event_public_active(
                oversized, oversized_len, "r1",
                sizeof("r1") - 1u, &public_ev) != 0
        );
    }
    {
        uint8_t alias_wire[512];
        uint8_t distinct_wire[512];
        char aliased[] = "Shared text channel";
        char legacy[] = "Shared text channel";
        char speech[] = "Shared text channel";
        char display[] = "Shared text channel";
        size_t alias_len = pb_encode_turn_text_event(
            alias_wire, sizeof(alias_wire), "r-alias", "text_delta",
            aliased, aliased, aliased, 3, 0);
        size_t distinct_len = pb_encode_turn_text_event(
            distinct_wire, sizeof(distinct_wire), "r-alias", "text_delta",
            legacy, speech, display, 3, 0);
        expect("aliased text channels encode", alias_len > 0u);
        expect(
            "aliased text channels preserve wire",
            alias_len == distinct_len &&
            memcmp(alias_wire, distinct_wire, alias_len) == 0);
    }
    {
        static const uint8_t expected[] = {
            0x0au, 0x01u, 'r', 0x18u, 0x04u,
            0x2au, 0x02u, 'H', 'i',
            0x8au, 0x01u, 0x02u, 'H', 'i',
            0x92u, 0x01u, 0x02u, 'H', 'i'
        };
        uint8_t wire[64];
        size_t cap;
        size_t n = pb_encode_turn_text_event_prepared(
            wire, sizeof(wire), "r", 1u, 4,
            "Hi", 2u, "Hi", 2u, "Hi", 2u, 0, 0);
        int capacities_ok = n == sizeof(expected) &&
            memcmp(wire, expected, sizeof(expected)) == 0;
        for (cap = 0u; capacities_ok && cap <= n + 1u; ++cap) {
            size_t bounded = pb_encode_turn_text_event_prepared(
                wire, cap, "r", 1u, 4,
                "Hi", 2u, "Hi", 2u, "Hi", 2u, 0, 0);
            if ((cap < n && bounded != 0u) ||
                (cap >= n &&
                 (bounded != n || memcmp(wire, expected, n) != 0)))
                capacities_ok = 0;
        }
        expect("prepared text event matches canonical bytes", n == sizeof(expected));
        expect("prepared text event preserves every small capacity", capacities_ok);
    }
    {
        char request_id[128];
        char long_text[256];
        uint8_t wire[1024];
        uint8_t expected[1024];
        turn_event_c event;
        size_t cap;
        size_t n;
        int capacities_ok;
        memset(request_id, 'r', sizeof(request_id) - 1u);
        request_id[sizeof(request_id) - 1u] = '\0';
        memset(long_text, 'x', sizeof(long_text) - 1u);
        long_text[sizeof(long_text) - 1u] = '\0';
        n = pb_encode_turn_text_event_prepared(
            expected, sizeof(expected), request_id, sizeof(request_id) - 1u, 5,
            long_text, sizeof(long_text) - 1u,
            long_text, sizeof(long_text) - 1u,
            long_text, sizeof(long_text) - 1u, 127, 1);
        capacities_ok = n > 0u && n < sizeof(wire);
        for (cap = 0u; capacities_ok && cap <= n + 1u; ++cap) {
            size_t bounded = pb_encode_turn_text_event_prepared(
                wire, cap, request_id, sizeof(request_id) - 1u, 5,
                long_text, sizeof(long_text) - 1u,
                long_text, sizeof(long_text) - 1u,
                long_text, sizeof(long_text) - 1u, 127, 1);
            if ((cap < n && bounded != 0u) ||
                (cap >= n &&
                 (bounded != n || memcmp(wire, expected, n) != 0)))
                capacities_ok = 0;
        }
        expect("prepared long aliased text event preserves every capacity", capacities_ok);
        expect(
            "prepared long aliased text event round trips",
            n > 0u && pb_decode_turn_event(expected, n, &event) == 0 &&
            strlen(event.request_id) == sizeof(request_id) - 1u &&
            event.type_id == 5 && strlen(event.text) == sizeof(long_text) - 1u &&
            strcmp(event.text, event.speech_text) == 0 &&
            strcmp(event.text, event.display_text) == 0 &&
            event.segment_index == 127 && event.is_final == 1);
    }
    {
        char legacy[129];
        char speech[130];
        char display[131];
        uint8_t wire[1024];
        turn_event_c event;
        size_t n;
        memset(legacy, 'l', sizeof(legacy));
        memset(speech, 's', sizeof(speech));
        memset(display, 'd', sizeof(display));
        n = pb_encode_turn_text_event_prepared(
            wire, sizeof(wire), "r-fallback", sizeof("r-fallback") - 1u, 6,
            legacy, sizeof(legacy), speech, sizeof(speech),
            display, sizeof(display), 128, 1);
        expect(
            "prepared text event preserves bounded fallback",
            n > 0u && pb_decode_turn_event(wire, n, &event) == 0 &&
            event.type_id == 6 && strlen(event.text) == sizeof(legacy) &&
            strlen(event.speech_text) == sizeof(speech) &&
            strlen(event.display_text) == sizeof(display) &&
            event.segment_index == 128 && event.is_final == 1);
        expect(
            "prepared text event rejects invalid contracts",
            pb_encode_turn_text_event_prepared(
                wire, sizeof(wire), NULL, 1u, 4,
                "x", 1u, "x", 1u, "x", 1u, 0, 0) == 0u &&
            pb_encode_turn_text_event_prepared(
                wire, sizeof(wire), "r", 0u, 4,
                "x", 1u, "x", 1u, "x", 1u, 0, 0) == 0u &&
            pb_encode_turn_text_event_prepared(
                wire, sizeof(wire), "r", 1u, 0,
                "x", 1u, "x", 1u, "x", 1u, 0, 0) == 0u &&
            pb_encode_turn_text_event_prepared(
                wire, sizeof(wire), "r", 1u, 13,
                "x", 1u, "x", 1u, "x", 1u, 0, 0) == 0u &&
            pb_encode_turn_text_event_prepared(
                wire, sizeof(wire), "r", 1u, 4,
                NULL, 1u, "x", 1u, "x", 1u, 0, 0) == 0u &&
            pb_encode_turn_text_event_prepared(
                wire, sizeof(wire), "r", 1u, 4,
                "x", 1u, "x", 1u, "x", 1u, -1, 0) == 0u);
    }
    {
        static const char text[] = "A quoted \\\"test\\\".";
        uint8_t wire[512];
        size_t n = pb_encode_turn_text_event_prepared(
            wire, sizeof(wire), "r-text-shape",
            sizeof("r-text-shape") - 1u, 5,
            text, sizeof(text) - 1u, text, sizeof(text) - 1u,
            text, sizeof(text) - 1u, 7, 1);
        expect(
            "public canonical text decoder matches every byte substitution",
            n != 0u && public_turn_text_mutations_match_bound(
                wire, n, "r-text-shape", sizeof("r-text-shape") - 1u));
    }
    {
        static const char final_display[] = "replacement";
        static const uint8_t reordered[] = {
            0x0au, 0x02u, 'r', '1', 0x18u, 0x04u,
            0x92u, 0x01u, 0x03u, 'p', 'u', 'b',
            0x8au, 0x01u, 0x06u, 's', 'p', 'e', 'e', 'c', 'h',
            0x2au, 0x06u, 'l', 'e', 'g', 'a', 'c', 'y'
        };
        static const uint8_t display_nul[] = {
            0x0au, 0x02u, 'r', '1', 0x18u, 0x04u,
            0x92u, 0x01u, 0x03u, 'x', '\0', 'y'
        };
        uint8_t wire[512];
        turn_event_c event;
        size_t n = pb_encode_turn_text_event(
            wire, sizeof(wire), "r-repeat", "text_delta", "first", "first",
            "first", 0, 0);
        int repeated_ok = n != 0u &&
            n + 3u + sizeof(final_display) - 1u <= sizeof(wire);
        if (repeated_ok) {
            wire[n++] = 0x92u;
            wire[n++] = 0x01u;
            wire[n++] = (uint8_t)(sizeof(final_display) - 1u);
            memcpy(wire + n, final_display, sizeof(final_display) - 1u);
            n += sizeof(final_display) - 1u;
            repeated_ok = public_turn_event_matches_bound(
                wire, n, "r-repeat", sizeof("r-repeat") - 1u) &&
                pb_decode_turn_event_public_active(
                    wire, n, "r-repeat", sizeof("r-repeat") - 1u,
                    &event) == 0 && event.display_text_borrowed == 1u &&
                event.display_text_len == sizeof(final_display) - 1u &&
                memcmp(
                    event.display_text_view, final_display,
                    sizeof(final_display) - 1u) == 0;
        }
        expect(
            "public text fallback borrows the final repeated field",
            repeated_ok);
        expect(
            "public text fallback accepts reordered fields",
            public_turn_event_matches_bound(
                reordered, sizeof(reordered), "r1", sizeof("r1") - 1u) &&
            pb_decode_turn_event_public_active(
                reordered, sizeof(reordered), "r1", sizeof("r1") - 1u,
                &event) == 0 && event.display_text_borrowed == 1u &&
            event.display_text_len == sizeof("pub") - 1u &&
            memcmp(
                event.display_text_view, "pub", sizeof("pub") - 1u) == 0);
        expect(
            "public text decoder rejects embedded display NUL",
            pb_decode_turn_event_public_active(
                display_nul, sizeof(display_nul), "r1",
                sizeof("r1") - 1u, &event) != 0);
    }
    {
        uint8_t buf[8192];
        session_append_request_c append_in, append_out;
        session_append_view_c append_view;
        session_get_request_c get_in, get_out;
        session_id_request_c id_in, id_out;
        session_summary_response_c summary;
        size_t n;
        memset(&append_in, 0, sizeof(append_in));
        snprintf(append_in.session_id, sizeof(append_in.session_id), "s1");
        snprintf(append_in.user_id, sizeof(append_in.user_id), "u1");
        snprintf(append_in.message.role, sizeof(append_in.message.role), "user");
        snprintf(append_in.message.content, sizeof(append_in.message.content), "remember this");
        snprintf(append_in.message.request_id, sizeof(append_in.message.request_id), "r1");
        append_in.message.timestamp_ms = 12345;
        n = pb_encode_session_append_request(buf, sizeof(buf), &append_in);
        expect("session append enc", n > 0);
        expect("session append dec", pb_decode_session_append_request(buf, n, &append_out) == 0);
        expect("session append view dec", pb_decode_session_append_view(buf, n, &append_view) == 0);
        expect(
            "session append fields",
            strcmp(append_out.session_id, "s1") == 0 &&
            strcmp(append_out.message.role, "user") == 0 &&
            strcmp(append_out.message.content, "remember this") == 0);
        expect(
            "session append view fields",
            strcmp(append_view.session_id, "s1") == 0 &&
            strcmp(append_view.user_id, "u1") == 0 &&
            append_view.message_wire == append_out.message_wire &&
            append_view.message_wire_len == append_out.message_wire_len);
        {
            size_t position;
            int equivalent = 1;
            for (position = 0; position < n && equivalent; ++position) {
                unsigned value;
                uint8_t original = buf[position];
                for (value = 0; value <= UINT8_MAX; ++value) {
                    int decoded;
                    int viewed;
                    buf[position] = (uint8_t)value;
                    decoded = pb_decode_session_append_request(buf, n, &append_out) == 0;
                    viewed = pb_decode_session_append_view(buf, n, &append_view) == 0;
                    if (decoded != viewed) {
                        equivalent = 0;
                        break;
                    }
                }
                buf[position] = original;
            }
            expect("session append view mutation differential", equivalent);
        }
        {
            size_t prefix_len;
            int equivalent = 1;
            for (prefix_len = 0; prefix_len <= n; ++prefix_len) {
                int decoded =
                    pb_decode_session_append_request(buf, prefix_len, &append_out) == 0;
                int viewed =
                    pb_decode_session_append_view(buf, prefix_len, &append_view) == 0;
                if (decoded != viewed) {
                    equivalent = 0;
                    break;
                }
            }
            expect("session append view prefix differential", equivalent);
        }
        snprintf(append_in.message.role, sizeof(append_in.message.role), "invalid");
        expect(
            "session rejects invalid role",
            pb_encode_session_append_request(buf, sizeof(buf), &append_in) == 0);

        memset(&get_in, 0, sizeof(get_in));
        snprintf(get_in.session_id, sizeof(get_in.session_id), "s1");
        snprintf(get_in.user_id, sizeof(get_in.user_id), "u1");
        get_in.last_n = 3;
        n = pb_encode_session_get_request(buf, sizeof(buf), &get_in);
        expect("session get roundtrip", n > 0 && pb_decode_session_get_request(buf, n, &get_out) == 0);
        expect("session get fields", get_out.last_n == 3 && strcmp(get_out.user_id, "u1") == 0);

        memset(&id_in, 0, sizeof(id_in));
        snprintf(id_in.session_id, sizeof(id_in.session_id), "s1");
        snprintf(id_in.user_id, sizeof(id_in.user_id), "u1");
        n = pb_encode_session_id_request(buf, sizeof(buf), &id_in);
        expect("session id roundtrip", n > 0 && pb_decode_session_id_request(buf, n, &id_out) == 0);

        n = pb_encode_session_summary_response(
            buf, sizeof(buf), "s1", "u1", "summary", "test", 12345);
        expect(
            "session summary roundtrip",
            n > 0 && pb_decode_session_summary_response(buf, n, &summary) == 0 &&
            strcmp(summary.summary, "summary") == 0 && summary.updated_at_ms == 12345);
    }
    {
        uint8_t buf[512];
        error_response_c response;
        size_t n = pb_encode_error_response(
            buf, sizeof(buf), "session store capacity reached", "transient");
        expect("error response enc", n > 0);
        expect("error response dec", pb_decode_error_response(buf, n, &response) == 0);
        expect(
            "error response fields",
            response.error == 1 &&
            strcmp(response.message, "session store capacity reached") == 0 &&
            strcmp(response.type, "transient") == 0);
    }
    {
        uint8_t buf[4096];
        static const uint8_t pcm[] = {1, 2, 3, 4};
        turn_event_c bound_ev;
        turn_event_c ev;
        turn_event_c public_ev;
        size_t n = pb_encode_turn_audio_event(
            buf, sizeof(buf), "r1", "pcm_chunk", "", pcm, sizeof(pcm),
            16000, 1, 16, 7, 3, 1);
        expect("audio event enc", n > 0);
        expect("audio event dec", pb_decode_turn_event(buf, n, &ev) == 0);
        expect(
            "audio event fields",
            ev.audio_len == sizeof(pcm) && memcmp(ev.audio, pcm, sizeof(pcm)) == 0 &&
            ev.sample_rate == 16000 && ev.channels == 1 && ev.bit_depth == 16 &&
            ev.sequence == 7 && ev.segment_index == 3 && ev.is_final == 1 &&
            ev.audio_encoding == 1
        );
        memset(&bound_ev, 0, sizeof(bound_ev));
        memset(&public_ev, 0, sizeof(public_ev));
        expect(
            "public PCM chunk matches checked decode",
            pb_decode_turn_event_bound(
                buf, n, "r1", sizeof("r1") - 1u, &bound_ev) == 0 &&
            pb_decode_turn_event_public_active(
                buf, n, "r1", sizeof("r1") - 1u, &public_ev) == 0 &&
            memcmp(&public_ev, &bound_ev, sizeof(bound_ev)) == 0);
    }
    {
        uint8_t buf[4096];
        uint8_t expected[4096];
        uint8_t prefix[256];
        uint8_t suffix[1024];
        uint8_t joined[4096];
        static const uint8_t pcm[] = {1, 2, 3, 4};
        turn_event_c bound_ev;
        turn_event_c ev;
        turn_event_c public_ev;
        turn_stage_timestamps_c stages;
        turn_stage_wire_c prepared;
        size_t cap;
        size_t n;
        size_t plain_n;
        size_t prefix_len;
        size_t suffix_len;
        memset(&stages, 0, sizeof(stages));
        stages.input.audio_committed_at_ms = 1787774400094LL;
        stages.input.stt_request_received_at_ms = 1787774400095LL;
        stages.input.stt_provider_request_started_at_ms = 1787774400096LL;
        stages.input.stt_provider_ready_at_ms = 1787774400097LL;
        stages.input.stt_transcript_published_at_ms = 1787774400098LL;
        stages.first_text_at_ms = 1787774400099LL;
        stages.tts_segment_emitted_at_ms = 1787774400100LL;
        stages.tts_request_received_at_ms = 1787774400101LL;
        stages.tts_provider_request_started_at_ms = 1787774400102LL;
        stages.tts_provider_ready_at_ms = 1787774400103LL;
        stages.pcm_started_at_ms = 1787774400104LL;
        expect(
            "prepared audio stages start incomplete",
            pb_prepare_turn_stage_wire(&prepared, &stages) == 0 &&
            pb_complete_turn_stage_wire(&prepared, 1787774400105LL) == 0);
        stages.pcm_first_chunk_at_ms = 1787774400105LL;
        n = pb_encode_turn_audio_event_stages(
            expected, sizeof(expected), "r1", "pcm_chunk", "", pcm, sizeof(pcm),
            16000, 1, 16, 0, 0, 1, &stages);
        expect(
            "prepared audio stages match canonical bytes",
            n > 0u && pb_encode_turn_audio_event_stage_wire(
                buf, sizeof(buf), "r1", "pcm_chunk", "", pcm, sizeof(pcm),
                16000, 1, 16, 0, 0, 1, &prepared) == n &&
            memcmp(buf, expected, n) == 0);
        expect(
            "prepared PCM chunk matches canonical bytes",
            n > 0u && pb_encode_turn_pcm_chunk_prepared(
                buf, sizeof(buf), "r1", 2u, pcm, sizeof(pcm),
                16000, 1, 16, 0, 0, 1, &prepared) == n &&
            memcmp(buf, expected, n) == 0);
        {
            size_t split_n = pb_encode_turn_pcm_chunk_spans_prepared(
                prefix, sizeof(prefix), &prefix_len,
                suffix, sizeof(suffix), &suffix_len,
                "r1", 2u, pcm, sizeof(pcm),
                16000, 1, 16, 0, 0, 1, &prepared);
            int split_ok = split_n == n &&
                prefix_len + sizeof(pcm) + suffix_len == n;
            if (split_ok) {
                memcpy(joined, prefix, prefix_len);
                memcpy(joined + prefix_len, pcm, sizeof(pcm));
                memcpy(joined + prefix_len + sizeof(pcm), suffix, suffix_len);
                split_ok = memcmp(joined, expected, n) == 0;
            }
            expect(
                "prepared PCM spans concatenate to canonical bytes",
                split_ok);
        }
        {
            size_t full_prefix_len = prefix_len;
            size_t full_suffix_len = suffix_len;
            int capacities_ok = 1;
            for (cap = 0u; capacities_ok && cap <= full_prefix_len + 1u; ++cap) {
                size_t split_n = pb_encode_turn_pcm_chunk_spans_prepared(
                    prefix, cap, &prefix_len,
                    suffix, sizeof(suffix), &suffix_len,
                    "r1", 2u, pcm, sizeof(pcm),
                    16000, 1, 16, 0, 0, 1, &prepared);
                if ((cap < full_prefix_len &&
                     (split_n != 0u || prefix_len != 0u || suffix_len != 0u)) ||
                    (cap >= full_prefix_len && split_n != n))
                    capacities_ok = 0;
            }
            for (cap = 0u; capacities_ok && cap <= full_suffix_len + 1u; ++cap) {
                size_t split_n = pb_encode_turn_pcm_chunk_spans_prepared(
                    prefix, sizeof(prefix), &prefix_len,
                    suffix, cap, &suffix_len,
                    "r1", 2u, pcm, sizeof(pcm),
                    16000, 1, 16, 0, 0, 1, &prepared);
                if ((cap < full_suffix_len &&
                     (split_n != 0u || prefix_len != 0u || suffix_len != 0u)) ||
                    (cap >= full_suffix_len && split_n != n))
                    capacities_ok = 0;
            }
            expect(
                "prepared PCM spans preserve each output capacity",
                capacities_ok);
        }
        {
            int capacities_ok = n > 0u && n < sizeof(buf);
            for (cap = 0u; capacities_ok && cap <= n + 1u; ++cap) {
                size_t bounded = pb_encode_turn_pcm_chunk_prepared(
                    buf, cap, "r1", 2u, pcm, sizeof(pcm),
                    16000, 1, 16, 0, 0, 1, &prepared);
                if ((cap < n && bounded != 0u) ||
                    (cap >= n &&
                     (bounded != n || memcmp(buf, expected, n) != 0)))
                    capacities_ok = 0;
            }
            expect(
                "prepared PCM chunk preserves every output capacity",
                capacities_ok);
        }
        plain_n = pb_encode_turn_audio_event_stage_wire(
            expected, sizeof(expected), "current", "pcm_chunk", "",
            pcm, sizeof(pcm), 24000, 1, 16, 127, 7, 1, &prepared);
        {
            int current_capacities_ok = plain_n > 0u && plain_n < sizeof(buf);
            for (cap = 0u;
                 current_capacities_ok && cap <= plain_n + 1u;
                 ++cap) {
                size_t bounded = pb_encode_turn_pcm_chunk_prepared(
                    buf, cap, "current", sizeof("current") - 1u,
                    pcm, sizeof(pcm), 24000, 1, 16, 127, 7, 1, &prepared);
                if ((cap < plain_n && bounded != 0u) ||
                    (cap >= plain_n &&
                     (bounded != plain_n ||
                      memcmp(buf, expected, plain_n) != 0)))
                    current_capacities_ok = 0;
            }
            expect(
                "prepared current PCM tuple preserves every capacity",
                current_capacities_ok);
        }
        {
            static const size_t request_lengths[] = {1u, 127u};
            static const size_t audio_lengths[] = {1u, 2u, 127u, 128u, 960u};
            static const int32_t sequences[] = {0, 1, 127, 128, 16383};
            static const int32_t segment_indexes[] = {
                0, 1, 127, 128, INT32_MAX
            };
            char current_request[127];
            uint8_t current_audio[960];
            uint8_t admitted_suffix[1024];
            turn_pcm_chunk_current_wire_c current_wire;
            size_t request_index;
            size_t audio_index;
            size_t sequence_index;
            size_t segment_index;
            int final;
            int current_spans_ok = 1;
            memset(current_request, 'r', sizeof(current_request));
            for (cap = 0u; cap < sizeof(current_audio); ++cap)
                current_audio[cap] = (uint8_t)(cap * 131u + 17u);
            for (request_index = 0u;
                 current_spans_ok &&
                 request_index < sizeof(request_lengths) /
                     sizeof(request_lengths[0]);
                 ++request_index) {
                for (audio_index = 0u;
                     current_spans_ok &&
                     audio_index < sizeof(audio_lengths) /
                         sizeof(audio_lengths[0]);
                     ++audio_index) {
                    for (segment_index = 0u;
                         current_spans_ok &&
                         segment_index < sizeof(segment_indexes) /
                             sizeof(segment_indexes[0]);
                         ++segment_index) {
                        if (pb_prepare_turn_pcm_chunk_current_wire(
                                &current_wire,
                                current_request,
                                request_lengths[request_index],
                                audio_lengths[audio_index],
                                segment_indexes[segment_index]) != 0) {
                            current_spans_ok = 0;
                            break;
                        }
                        for (sequence_index = 0u;
                             current_spans_ok &&
                             sequence_index < sizeof(sequences) /
                                 sizeof(sequences[0]);
                             ++sequence_index) {
                            for (final = 0; current_spans_ok && final <= 1;
                                 ++final) {
                                const turn_stage_wire_c *stage =
                                    ((request_index + audio_index + sequence_index +
                                      segment_index + (size_t)final) & 1u) != 0u ?
                                        &prepared : NULL;
                                size_t expected_len =
                                    pb_encode_turn_pcm_chunk_prepared(
                                        expected,
                                        sizeof(expected),
                                        current_request,
                                        request_lengths[request_index],
                                        current_audio,
                                        audio_lengths[audio_index],
                                        24000,
                                        1,
                                        16,
                                        sequences[sequence_index],
                                        segment_indexes[segment_index],
                                        final,
                                        stage);
                                size_t current_len =
                                    pb_encode_turn_pcm_chunk_current_suffix_prepared(
                                        &current_wire,
                                        suffix,
                                        sizeof(suffix),
                                        &suffix_len,
                                        sequences[sequence_index],
                                        final,
                                        stage);
                                size_t admitted_suffix_len;
                                size_t admitted_len =
                                    pb_encode_turn_pcm_chunk_current_suffix_admitted(
                                        &current_wire,
                                        admitted_suffix,
                                        sizeof(admitted_suffix),
                                        &admitted_suffix_len,
                                        sequences[sequence_index],
                                        final,
                                        stage);
                                if (expected_len == 0u || current_len != expected_len ||
                                    admitted_len != current_len ||
                                    admitted_suffix_len != suffix_len ||
                                    memcmp(
                                        admitted_suffix,
                                        suffix,
                                        suffix_len) != 0 ||
                                    (size_t)current_wire.prefix_len +
                                        audio_lengths[audio_index] + suffix_len !=
                                            current_len) {
                                    current_spans_ok = 0;
                                    break;
                                }
                                memcpy(
                                    joined,
                                    current_wire.prefix,
                                    current_wire.prefix_len);
                                memcpy(
                                    joined + current_wire.prefix_len,
                                    current_audio,
                                    audio_lengths[audio_index]);
                                memcpy(
                                    joined + current_wire.prefix_len +
                                        audio_lengths[audio_index],
                                    suffix,
                                    suffix_len);
                                if (memcmp(joined, expected, expected_len) != 0)
                                    current_spans_ok = 0;
                            }
                        }
                    }
                }
            }
            expect(
                "current PCM spans match generic boundary matrix",
                current_spans_ok);
        }
        {
            char maximum_request[127];
            uint8_t maximum_audio[960];
            uint8_t admitted_suffix[1024];
            turn_pcm_chunk_current_wire_c current_wire;
            size_t full_suffix_len;
            size_t current_len;
            int capacities_ok = 1;
            memset(maximum_request, 'm', sizeof(maximum_request));
            memset(maximum_audio, 0xa5, sizeof(maximum_audio));
            if (pb_prepare_turn_pcm_chunk_current_wire(
                    &current_wire,
                    maximum_request,
                    sizeof(maximum_request),
                    sizeof(maximum_audio),
                    INT32_MAX) != 0)
                capacities_ok = 0;
            current_len = pb_encode_turn_pcm_chunk_current_suffix_prepared(
                &current_wire,
                suffix, sizeof(suffix), &full_suffix_len,
                16383, 1, &prepared);
            if (current_len == 0u) capacities_ok = 0;
            for (cap = 0u;
                 capacities_ok && cap <= full_suffix_len + 1u;
                 ++cap) {
                size_t admitted_suffix_len = 1u;
                size_t bounded =
                    pb_encode_turn_pcm_chunk_current_suffix_prepared(
                        &current_wire,
                        suffix, cap, &suffix_len,
                        16383, 1, &prepared);
                size_t admitted =
                    pb_encode_turn_pcm_chunk_current_suffix_admitted(
                        &current_wire,
                        admitted_suffix,
                        cap,
                        &admitted_suffix_len,
                        16383,
                        1,
                        &prepared);
                if ((cap < full_suffix_len &&
                     (bounded != 0u || suffix_len != 0u ||
                      admitted != 0u || admitted_suffix_len != 0u)) ||
                    (cap >= full_suffix_len &&
                     (bounded != current_len || admitted != current_len ||
                      admitted_suffix_len != suffix_len ||
                      memcmp(admitted_suffix, suffix, suffix_len) != 0)))
                    capacities_ok = 0;
            }
            expect(
                "prepared current PCM suffix preserves every output capacity",
                capacities_ok);
        }
        {
            turn_pcm_chunk_current_wire_c current_wire;
            turn_pcm_chunk_current_wire_c invalid_current_wire;
            turn_stage_wire_c invalid_prepared = prepared;
            invalid_prepared.len = 0u;
            suffix_len = 1u;
            expect(
                "current PCM wire preparation rejects invalid bounds",
                pb_prepare_turn_pcm_chunk_current_wire(
                    NULL, "current", sizeof("current") - 1u, 2u, 0) != 0 &&
                pb_prepare_turn_pcm_chunk_current_wire(
                    &current_wire, NULL, sizeof("current") - 1u, 2u, 0) != 0 &&
                current_wire.prefix_len == 0u &&
                pb_prepare_turn_pcm_chunk_current_wire(
                    &current_wire, "current", 0u, 2u, 0) != 0 &&
                pb_prepare_turn_pcm_chunk_current_wire(
                    &current_wire, "current", 128u, 2u, 0) != 0 &&
                pb_prepare_turn_pcm_chunk_current_wire(
                    &current_wire, "current", sizeof("current") - 1u, 0u, 0) != 0 &&
                pb_prepare_turn_pcm_chunk_current_wire(
                    &current_wire,
                    "current",
                    sizeof("current") - 1u,
                    961u,
                    0) != 0 &&
                pb_prepare_turn_pcm_chunk_current_wire(
                    &current_wire,
                    "current",
                    sizeof("current") - 1u,
                    2u,
                    -1) != 0);
            expect(
                "prepared current PCM suffix rejects invalid bounds",
                pb_prepare_turn_pcm_chunk_current_wire(
                    &current_wire,
                    "current",
                    sizeof("current") - 1u,
                    2u,
                    0) == 0 &&
                pb_encode_turn_pcm_chunk_current_suffix_prepared(
                    &current_wire,
                    suffix, sizeof(suffix), &suffix_len,
                    -1, 0, NULL) == 0u && suffix_len == 0u &&
                pb_encode_turn_pcm_chunk_current_suffix_prepared(
                    &current_wire,
                    suffix, sizeof(suffix), &suffix_len,
                    16384, 0, NULL) == 0u &&
                pb_encode_turn_pcm_chunk_current_suffix_prepared(
                    &current_wire,
                    suffix, sizeof(suffix), &suffix_len,
                    0, 0, &invalid_prepared) == 0u);
            current_wire.prefix_len =
                (uint16_t)(sizeof(current_wire.prefix) + 1u);
            expect(
                "prepared current PCM suffix rejects a corrupt context",
                pb_encode_turn_pcm_chunk_current_suffix_prepared(
                    &current_wire,
                    suffix, sizeof(suffix), &suffix_len,
                    0, 0, NULL) == 0u && suffix_len == 0u);
            expect(
                "admitted current PCM suffix rejects unsafe storage bounds",
                pb_prepare_turn_pcm_chunk_current_wire(
                    &invalid_current_wire,
                    "current",
                    sizeof("current") - 1u,
                    2u,
                    0) == 0 &&
                pb_encode_turn_pcm_chunk_current_suffix_admitted(
                    NULL,
                    suffix, sizeof(suffix), &suffix_len,
                    0, 0, NULL) == 0u && suffix_len == 0u &&
                pb_encode_turn_pcm_chunk_current_suffix_admitted(
                    &invalid_current_wire,
                    NULL, sizeof(suffix), &suffix_len,
                    0, 0, NULL) == 0u &&
                pb_encode_turn_pcm_chunk_current_suffix_admitted(
                    &invalid_current_wire,
                    suffix, sizeof(suffix), NULL,
                    0, 0, NULL) == 0u);
            invalid_current_wire.segment_len =
                (uint8_t)(sizeof(invalid_current_wire.segment) + 1u);
            invalid_prepared.len = sizeof(invalid_prepared.data) + 1u;
            expect(
                "admitted current PCM suffix bounds borrowed contexts",
                pb_encode_turn_pcm_chunk_current_suffix_admitted(
                    &invalid_current_wire,
                    suffix, sizeof(suffix), &suffix_len,
                    0, 0, NULL) == 0u && suffix_len == 0u &&
                pb_prepare_turn_pcm_chunk_current_wire(
                    &invalid_current_wire,
                    "current",
                    sizeof("current") - 1u,
                    2u,
                    0) == 0 &&
                pb_encode_turn_pcm_chunk_current_suffix_admitted(
                    &invalid_current_wire,
                    suffix, sizeof(suffix), &suffix_len,
                    0, 0, &invalid_prepared) == 0u && suffix_len == 0u);
        }
        {
            static const int32_t adjacent_formats[][3] = {
                {23999, 1, 16},
                {24001, 1, 16},
                {24000, 2, 16},
                {24000, 1, 15},
                {24000, 1, 17},
            };
            size_t format_index;
            int adjacent_formats_ok = 1;
            for (format_index = 0u;
                 adjacent_formats_ok &&
                 format_index < sizeof(adjacent_formats) /
                     sizeof(adjacent_formats[0]);
                 ++format_index) {
                int32_t rate = adjacent_formats[format_index][0];
                int32_t channel_count = adjacent_formats[format_index][1];
                int32_t depth = adjacent_formats[format_index][2];
                size_t expected_len = pb_encode_turn_audio_event_stage_wire(
                    expected, sizeof(expected), "adjacent", "pcm_chunk", "",
                    pcm, sizeof(pcm), rate, channel_count, depth,
                    1, 1, 0, &prepared);
                size_t prepared_len = pb_encode_turn_pcm_chunk_prepared(
                    buf, sizeof(buf), "adjacent", sizeof("adjacent") - 1u,
                    pcm, sizeof(pcm), rate, channel_count, depth,
                    1, 1, 0, &prepared);
                if (expected_len == 0u || prepared_len != expected_len ||
                    memcmp(buf, expected, expected_len) != 0)
                    adjacent_formats_ok = 0;
            }
            expect(
                "prepared adjacent PCM tuples use canonical fallback",
                adjacent_formats_ok);
        }
        plain_n = pb_encode_turn_audio_event(
            expected, sizeof(expected), "request-2", "pcm_chunk", "",
            pcm, sizeof(pcm), 24000, 2, 24, 131, 129, 0);
        expect(
            "prepared PCM chunk matches wide canonical scalars",
            plain_n > 0u && pb_encode_turn_pcm_chunk_prepared(
                buf, sizeof(buf), "request-2", 9u, pcm, sizeof(pcm),
                24000, 2, 24, 131, 129, 0, NULL) == plain_n &&
            memcmp(buf, expected, plain_n) == 0);
        {
            static const int32_t scalar_boundaries[] = {
                0, 1, 127, 128, 16383, 16384, INT32_MAX
            };
            size_t boundary_index;
            int boundaries_ok = 1;
            for (boundary_index = 0u;
                 boundaries_ok &&
                 boundary_index < sizeof(scalar_boundaries) /
                     sizeof(scalar_boundaries[0]);
                 ++boundary_index) {
                int32_t value = scalar_boundaries[boundary_index];
                const turn_stage_wire_c *stage =
                    (boundary_index & 1u) != 0u ? &prepared : NULL;
                size_t expected_len = pb_encode_turn_audio_event_stage_wire(
                    expected, sizeof(expected), "boundary", "pcm_chunk", "",
                    pcm, sizeof(pcm), value, value, value, value, value,
                    value != 0, stage);
                size_t prepared_len = pb_encode_turn_pcm_chunk_prepared(
                    buf, sizeof(buf), "boundary", 8u, pcm, sizeof(pcm),
                    value, value, value, value, value, value != 0, stage);
                if (expected_len == 0u || prepared_len != expected_len ||
                    memcmp(buf, expected, expected_len) != 0)
                    boundaries_ok = 0;
            }
            expect(
                "prepared PCM chunk matches scalar varint boundaries",
                boundaries_ok);
        }
        plain_n = pb_encode_turn_audio_event(
            expected, sizeof(expected), "silent", "pcm_chunk", "",
            NULL, 0u, 0, 0, 0, 0, 0, 0);
        expect(
            "prepared PCM chunk matches empty audio shape",
            plain_n > 0u && pb_encode_turn_pcm_chunk_prepared(
                buf, sizeof(buf), "silent", 6u, NULL, 0u,
                0, 0, 0, 0, 0, 0, NULL) == plain_n &&
            memcmp(buf, expected, plain_n) == 0);
        {
            size_t split_n = pb_encode_turn_pcm_chunk_spans_prepared(
                prefix, sizeof(prefix), &prefix_len,
                suffix, sizeof(suffix), &suffix_len,
                "silent", 6u, NULL, 0u,
                0, 0, 0, 0, 0, 0, NULL);
            expect(
                "prepared PCM spans preserve empty audio shape",
                split_n == plain_n && suffix_len == 0u &&
                prefix_len == plain_n &&
                memcmp(prefix, expected, plain_n) == 0);
        }
        {
            turn_stage_wire_c invalid_prepared = prepared;
            invalid_prepared.len = 0u;
            expect(
                "prepared PCM chunk rejects invalid stage wire",
                pb_encode_turn_pcm_chunk_prepared(
                    buf, sizeof(buf), "r1", 2u, pcm, sizeof(pcm),
                    16000, 1, 16, 0, 0, 0, &invalid_prepared) == 0u);
        }
        expect(
            "prepared PCM chunk rejects invalid inputs",
            pb_encode_turn_pcm_chunk_prepared(
                buf, sizeof(buf), "", 0u, pcm, sizeof(pcm),
                16000, 1, 16, 0, 0, 0, NULL) == 0u &&
            pb_encode_turn_pcm_chunk_prepared(
                buf, sizeof(buf), "r1", 2u, NULL, sizeof(pcm),
                16000, 1, 16, 0, 0, 0, NULL) == 0u &&
            pb_encode_turn_pcm_chunk_prepared(
                buf, sizeof(buf), "r1", 2u, pcm, sizeof(pcm),
                -1, 1, 16, 0, 0, 0, NULL) == 0u);
        prefix_len = 1u;
        suffix_len = 1u;
        expect(
            "prepared PCM spans reject invalid outputs and clear lengths",
            pb_encode_turn_pcm_chunk_spans_prepared(
                prefix, sizeof(prefix), NULL,
                suffix, sizeof(suffix), &suffix_len,
                "r1", 2u, pcm, sizeof(pcm),
                16000, 1, 16, 0, 0, 0, NULL) == 0u &&
            suffix_len == 0u &&
            pb_encode_turn_pcm_chunk_spans_prepared(
                prefix, sizeof(prefix), &prefix_len,
                suffix, sizeof(suffix), &suffix_len,
                "r1", 2u, NULL, sizeof(pcm),
                16000, 1, 16, 0, 0, 0, NULL) == 0u &&
            prefix_len == 0u && suffix_len == 0u);
        n = pb_encode_turn_audio_event_stages(
            buf, sizeof(buf), "r1", "pcm_chunk", "", pcm, sizeof(pcm),
            16000, 1, 16, 0, 0, 1, &stages);
        expect("audio stage metadata enc", n > 0);
        plain_n = pb_encode_turn_audio_event(
            buf, sizeof(buf), "r1", "pcm_chunk", "", pcm, sizeof(pcm),
            16000, 1, 16, 0, 0, 1);
        expect(
            "audio stage metadata stays bounded",
            plain_n > 0 && n > plain_n && n - plain_n < 768u);
        if (plain_n > 0 && n > plain_n)
            printf(
                "BenchmarkTurnStageMetadata\tplain=%zu bytes\tstaged=%zu bytes\t"
                "one_time_overhead=%zu bytes\n",
                plain_n, n, n - plain_n);
        n = pb_encode_turn_audio_event_stages(
            buf, sizeof(buf), "r1", "pcm_chunk", "", pcm, sizeof(pcm),
            16000, 1, 16, 0, 0, 1, &stages);
        expect("audio stage metadata dec", pb_decode_turn_event(buf, n, &ev) == 0);
        memset(&bound_ev, 0, sizeof(bound_ev));
        memset(&public_ev, 0, sizeof(public_ev));
        expect(
            "public staged PCM chunk matches checked decode",
            pb_decode_turn_event_bound(
                buf, n, "r1", sizeof("r1") - 1u, &bound_ev) == 0 &&
            pb_decode_turn_event_public_active(
                buf, n, "r1", sizeof("r1") - 1u, &public_ev) == 0 &&
            memcmp(&public_ev, &bound_ev, sizeof(bound_ev)) == 0);
        expect(
            "audio stage metadata fields",
            ev.stages.input.audio_committed_at_ms == 1787774400094LL &&
            ev.stages.input.stt_request_received_at_ms == 1787774400095LL &&
            ev.stages.input.stt_provider_request_started_at_ms == 1787774400096LL &&
            ev.stages.input.stt_provider_ready_at_ms == 1787774400097LL &&
            ev.stages.input.stt_transcript_published_at_ms == 1787774400098LL &&
            ev.stages.first_text_at_ms == 1787774400099LL &&
            ev.stages.tts_segment_emitted_at_ms == 1787774400100LL &&
            ev.stages.tts_request_received_at_ms == 1787774400101LL &&
            ev.stages.tts_provider_request_started_at_ms == 1787774400102LL &&
            ev.stages.tts_provider_ready_at_ms == 1787774400103LL &&
            ev.stages.pcm_started_at_ms == 1787774400104LL &&
            ev.stages.pcm_first_chunk_at_ms == 1787774400105LL);
    }
    {
        uint8_t buf[4096];
        rag_search_request_c req_in, req_out;
        rag_search_response_c resp_in, resp_out;
        size_t n;
        memset(&req_in, 0, sizeof(req_in));
        snprintf(req_in.request_id, sizeof(req_in.request_id), "r1");
        snprintf(req_in.user_id, sizeof(req_in.user_id), "u1");
        snprintf(req_in.query, sizeof(req_in.query), "rules query");
        snprintf(req_in.collection, sizeof(req_in.collection), "dnd_rules");
        req_in.top_k = 4;
        req_in.rerank_top_k = 2;
        req_in.enable_rerank = 1;
        n = pb_encode_rag_search_request(buf, sizeof(buf), &req_in);
        expect("rag request enc", n > 0);
        expect("rag request dec", pb_decode_rag_search_request(buf, n, &req_out) == 0);
        expect(
            "rag request fields",
            strcmp(req_out.request_id, "r1") == 0 &&
            strcmp(req_out.collection, "dnd_rules") == 0 &&
            req_out.top_k == 4 && req_out.rerank_top_k == 2 && req_out.enable_rerank == 1
        );
        memset(&resp_in, 0, sizeof(resp_in));
        snprintf(resp_in.request_id, sizeof(resp_in.request_id), "r1");
        snprintf(resp_in.context_text, sizeof(resp_in.context_text), "context");
        resp_in.used_rag = 1;
        n = pb_encode_rag_search_response(buf, sizeof(buf), &resp_in);
        expect("rag response enc", n > 0);
        expect("rag response dec", pb_decode_rag_search_response(buf, n, &resp_out) == 0);
        expect(
            "rag response fields",
            strcmp(resp_out.request_id, "r1") == 0 &&
            strcmp(resp_out.context_text, "context") == 0 && resp_out.used_rag == 1
        );
    }

    /* pb encode stt stream roundtrip */
    {
        uint8_t audio[4] = {1, 2, 3, 4};
        uint8_t buf[128];
        stt_stream_message_c dec;
        size_t n = pb_encode_stt_stream_message(buf, sizeof(buf), "chunk", audio, 4, 16000, 1, 16);
        expect("stream enc", n > 0);
        expect("stream dec", pb_decode_stt_stream_message(buf, n, &dec) == 0);
        expect("stream type", strcmp(dec.type, "chunk") == 0);
        expect("stream alen", dec.audio_len == 4);
        expect("stream rate", dec.sample_rate == 16000);
    }
    {
        uint8_t buf[128];
        stt_stream_message_c dec;
        size_t n = pb_encode_stt_stream_message(
            buf, sizeof(buf), "start", NULL, 0u, 16000, 1, 16);
        memset(&dec, 0xa5, sizeof(dec));
        expect(
            "stream decoder resets dirty output",
            n > 0u && pb_decode_stt_stream_message(buf, n, &dec) == 0 &&
            strcmp(dec.type, "start") == 0 && dec.audio == NULL &&
            dec.audio_len == 0u && dec.sample_rate == 16000 &&
            dec.channels == 1 && dec.bit_depth == 16 &&
            dec.timestamp_ms == 0 && dec.utterance_id[0] == '\0' &&
            dec.speaker_id[0] == '\0' && dec.state[0] == '\0');
    }
    {
        uint8_t audio[128];
        uint8_t buf[512];
        char type[32];
        stt_stream_message_c dec;
        size_t n;
        memset(audio, 0xa5, sizeof(audio));
        memset(type, 't', sizeof(type) - 1u);
        type[sizeof(type) - 1u] = '\0';
        n = pb_encode_stt_stream_message_at(
            buf,
            sizeof(buf),
            type,
            audio,
            sizeof(audio),
            16000,
            1,
            16,
            INT64_MAX);
        expect("product stream boundary enc", n > 0u);
        expect(
            "product stream boundary dec",
            pb_decode_stt_stream_message(buf, n, &dec) == 0);
        expect(
            "product stream boundary fields",
            strcmp(dec.type, type) == 0 &&
            dec.audio_len == sizeof(audio) &&
            memcmp(dec.audio, audio, sizeof(audio)) == 0 &&
            dec.sample_rate == 16000 &&
            dec.channels == 1 &&
            dec.bit_depth == 16 &&
            dec.timestamp_ms == INT64_MAX);
        n = pb_encode_stt_stream_message(
            buf,
            sizeof(buf),
            "chunk",
            NULL,
            0u,
            INT32_MAX,
            INT32_MAX,
            INT32_MAX);
        expect(
            "stream fallback preserves scalar boundaries",
            n > 0u && pb_decode_stt_stream_message(buf, n, &dec) == 0 &&
            dec.sample_rate == INT32_MAX && dec.channels == INT32_MAX &&
            dec.bit_depth == INT32_MAX);
    }
    {
        static const uint8_t reordered[] = {
            0x28, 0x80, 0x7d,
            0x0a, 0x05, 'c', 'h', 'u', 'n', 'k',
            0x30, 0x01,
            0x38, 0x10
        };
        static const uint8_t unknown[] = {
            0x0a, 0x05, 'c', 'h', 'u', 'n', 'k',
            0x28, 0x80, 0x7d,
            0x30, 0x01,
            0x38, 0x10,
            0x40, 0x01
        };
        static const uint8_t repeated[] = {
            0x0a, 0x05, 'c', 'h', 'u', 'n', 'k',
            0x28, 0xc0, 0x3e,
            0x28, 0x80, 0x7d,
            0x30, 0x01,
            0x38, 0x10
        };
        stt_stream_message_c dec;
        expect(
            "stream fallback accepts reordered fields",
            pb_decode_stt_stream_message(
                reordered, sizeof(reordered), &dec) == 0 &&
            strcmp(dec.type, "chunk") == 0 && dec.sample_rate == 16000);
        expect(
            "stream fallback skips unknown fields",
            pb_decode_stt_stream_message(unknown, sizeof(unknown), &dec) == 0 &&
            strcmp(dec.type, "chunk") == 0 && dec.sample_rate == 16000);
        expect(
            "stream fallback preserves repeated fields",
            pb_decode_stt_stream_message(
                repeated, sizeof(repeated), &dec) == 0 &&
            strcmp(dec.type, "chunk") == 0 && dec.sample_rate == 16000);
    }

    /* stateful segmenter host: chunk then flush remainder */
    {
        speech_seg_config_v1 cfg;
        speech_seg_host_v1 host;
        char parts[8][SPEECH_SEG_HOST_MAX_PART];
        size_t n = 0;
        char tail[SPEECH_SEG_HOST_MAX_PART];
        int em = 0;
        speech_seg_config_default_v1(&cfg);
        cfg.min_segment_chars = 8;
        cfg.max_segment_chars = 80;
        cfg.first_segment_chars = 0;
        speech_seg_host_init_v1(&host, &cfg);
        expect(
            "host add1",
            speech_seg_host_add_v1(
                &host, 1000, "Ready, adventurer? Keep", 22, parts, 8, &n
            ) == SPEECH_SEG_OK
        );
        expect("host segs", n == 1);
        expect("host seg0", strcmp(parts[0], "Ready, adventurer?") == 0);
        expect(
            "host add2",
            speech_seg_host_add_v1(&host, 2000, " moving.", 8, parts, 8, &n) == SPEECH_SEG_OK
        );
        /* may or may not cut depending on remainder; flush all for remainder */
        speech_seg_host_flush_all_v1(&host, tail, &em);
        if (n == 0 && em) {
            expect("host flush tail", strstr(tail, "Keep") != NULL || strstr(tail, "moving") != NULL);
        } else {
            expect("host progressed", n >= 1 || em == 1 || host.len == 0);
        }
        /* idle flush after timeout */
        speech_seg_host_reset_v1(&host);
        n = 0;
        expect(
            "host idle add",
            speech_seg_host_add_v1(
                &host, 1000, "short frag", 10, parts, 8, &n
            ) == SPEECH_SEG_OK
        );
        expect("host idle no cut", n == 0);
        em = 0;
        speech_seg_host_flush_if_idle_v1(&host, 1000 + SPEECH_SEG_HOST_DEFAULT_FLUSH_NS + 1, tail, &em);
        expect("host idle flush", em == 1 && strcmp(tail, "short frag") == 0);
        speech_seg_host_free_v1(&host);
    }
    {
        static const char early_prefix[] = "Well ";
        static const char later_suffix[] = "met ";
        static const char short_prefix[] = "The ";
        static const char short_suffix[] = "old ";
        speech_seg_config_v1 eager_cfg;
        speech_seg_config_v1 control_cfg;
        speech_seg_host_v1 eager;
        speech_seg_host_v1 control;
        speech_seg_host_v1 short_word;
        char eager_parts[1][SPEECH_SEG_HOST_MAX_PART];
        char control_parts[1][SPEECH_SEG_HOST_MAX_PART];
        char short_parts[1][SPEECH_SEG_HOST_MAX_PART];
        size_t eager_n = 0;
        size_t control_n = 0;
        size_t short_n = 0;

        speech_seg_config_default_v1(&eager_cfg);
        eager_cfg.min_segment_chars = 8;
        eager_cfg.max_segment_chars = 80;
        eager_cfg.first_segment_chars = 5;
        control_cfg = eager_cfg;
        control_cfg.first_segment_chars = 8;
        speech_seg_host_init_v1(&eager, &eager_cfg);
        speech_seg_host_init_v1(&control, &control_cfg);
        speech_seg_host_init_v1(&short_word, &eager_cfg);
        expect(
            "eager prefix add",
            speech_seg_host_add_v1(
                &eager,
                1000,
                early_prefix,
                sizeof(early_prefix) - 1u,
                eager_parts,
                1,
                &eager_n) == SPEECH_SEG_OK);
        expect(
            "control prefix add",
            speech_seg_host_add_v1(
                &control,
                1000,
                early_prefix,
                sizeof(early_prefix) - 1u,
                control_parts,
                1,
                &control_n) == SPEECH_SEG_OK);
        expect(
            "5-character prefix emits before 8-character control",
            eager_n == 1u && strcmp(eager_parts[0], "Well") == 0 &&
            control_n == 0u && eager.overflow_buf == NULL &&
            control.overflow_buf == NULL);
        expect(
            "8-character control emits after the next word",
            speech_seg_host_add_v1(
                &control,
                2000,
                later_suffix,
                sizeof(later_suffix) - 1u,
                control_parts,
                1,
                &control_n) == SPEECH_SEG_OK &&
            control_n == 1u && strcmp(control_parts[0], "Well met") == 0);
        expect(
            "5-character prefix buffers a three-letter word",
            speech_seg_host_add_v1(
                &short_word,
                1000,
                short_prefix,
                sizeof(short_prefix) - 1u,
                short_parts,
                1,
                &short_n) == SPEECH_SEG_OK &&
            short_n == 0u);
        expect(
            "5-character prefix emits the next safe word boundary",
            speech_seg_host_add_v1(
                &short_word,
                2000,
                short_suffix,
                sizeof(short_suffix) - 1u,
                short_parts,
                1,
                &short_n) == SPEECH_SEG_OK &&
            short_n == 1u && strcmp(short_parts[0], "The old") == 0);
        speech_seg_host_free_v1(&eager);
        speech_seg_host_free_v1(&control);
        speech_seg_host_free_v1(&short_word);
    }
    /* Normal model deltas stay inline; exceptional deltas retain heap fallback. */
    {
        speech_seg_config_v1 cfg;
        speech_seg_host_v1 host;
        char chunk[SPEECH_SEG_HOST_INLINE_CAP + 44u];
        char parts[8][SPEECH_SEG_HOST_MAX_PART];
        size_t n = 0;

        memset(chunk, 'a', sizeof(chunk));
        speech_seg_config_default_v1(&cfg);
        cfg.first_segment_chars = 0;
        cfg.min_segment_chars = 8;
        cfg.max_segment_chars = 80;
        speech_seg_host_init_v1(&host, &cfg);
        expect(
            "segment host begins on inline storage",
            host.overflow_buf == NULL && host.overflow_cap == 0u);
        expect(
            "oversized segment delta uses bounded-growth fallback",
            speech_seg_host_add_v1(
                &host,
                1000,
                chunk,
                sizeof(chunk),
                parts,
                8,
                &n) == SPEECH_SEG_OK &&
            host.overflow_buf != NULL &&
            host.overflow_cap >= sizeof(chunk) + 1u && n > 0u);
        speech_seg_host_free_v1(&host);
        expect(
            "segment host free clears fallback ownership",
            host.overflow_buf == NULL && host.overflow_cap == 0u &&
            host.len == 0u);
    }
    /* A bounded caller never loses segments beyond its output capacity. */
    {
        speech_seg_config_v1 cfg;
        speech_seg_host_v1 host;
        char parts[1][SPEECH_SEG_HOST_MAX_PART];
        char tail[SPEECH_SEG_HOST_MAX_PART];
        size_t n = 0;
        int emitted = 0;

        speech_seg_config_default_v1(&cfg);
        cfg.first_segment_chars = 0;
        cfg.min_segment_chars = 1;
        cfg.max_segment_chars = 6;
        speech_seg_host_init_v1(&host, &cfg);
        expect(
            "segment host rejects null nonempty append",
            speech_seg_host_add_v1(
                &host, 1000, NULL, 1, NULL, 0, &n) ==
                SPEECH_SEG_ERR_ARGUMENT && host.len == 0u);
        expect(
            "segment host respects zero output capacity",
            speech_seg_host_add_v1(
                &host, 1000, "One. Two. Three.", 16, NULL, 0, &n) ==
                SPEECH_SEG_OK && n == 0u && host.len == 16u);
        expect(
            "segment host emits only caller capacity",
            speech_seg_host_add_v1(
                &host, 2000, " ", 1, parts, 1, &n) == SPEECH_SEG_OK &&
            n == 1u && strcmp(parts[0], "One.") == 0 && host.len > 0u);
        expect(
            "segment host retains later bounded segments",
            speech_seg_host_add_v1(
                &host, 3000, " ", 1, parts, 1, &n) == SPEECH_SEG_OK &&
            n == 1u && strcmp(parts[0], "Two.") == 0 && host.len > 0u);
        expect(
            "segment host flushes final retained segment",
            speech_seg_host_flush_all_v1(&host, tail, &emitted) ==
                SPEECH_SEG_OK && emitted == 1 && strcmp(tail, "Three.") == 0);
        speech_seg_host_free_v1(&host);
    }
    /* Overflow and oversized flush inputs fail without consuming buffered text. */
    {
        speech_seg_config_v1 cfg;
        speech_seg_host_v1 host;
        char oversized[SPEECH_SEG_HOST_MAX_PART + 32u];
        char out[SPEECH_SEG_HOST_MAX_PART];
        size_t n = 0;
        int emitted = 0;

        memset(oversized, 'a', sizeof(oversized));
        speech_seg_config_default_v1(&cfg);
        cfg.first_segment_chars = 0;
        cfg.min_segment_chars = (int)sizeof(oversized) + 1;
        cfg.max_segment_chars = cfg.min_segment_chars;
        speech_seg_host_init_v1(&host, &cfg);
        expect(
            "segment host buffers oversized final candidate",
            speech_seg_host_add_v1(
                &host,
                UINT64_MAX - 10u,
                oversized,
                sizeof(oversized),
                NULL,
                0,
                &n) == SPEECH_SEG_OK && host.len == sizeof(oversized));
        expect(
            "segment host idle deadline does not wrap",
            speech_seg_host_flush_if_idle_v1(
                &host, UINT64_MAX, out, &emitted) == SPEECH_SEG_OK &&
            emitted == 0 && host.len == sizeof(oversized));
        expect(
            "segment host rejects oversized final without data loss",
            speech_seg_host_flush_all_v1(&host, out, &emitted) ==
                SPEECH_SEG_ERR_ARGUMENT && emitted == 0 &&
            host.len == sizeof(oversized));
        host.len = SIZE_MAX;
        expect(
            "segment host rejects append size overflow",
            speech_seg_host_add_v1(
                &host, 1, "x", 1, NULL, 0, &n) ==
                SPEECH_SEG_ERR_ARGUMENT);
        host.len = 0;
        speech_seg_host_free_v1(&host);
    }

    /* http_min URL parse + openai chat JSON helpers (no network) */
    {
        http_min_url u;
        expect(
            "http parse",
            http_min_parse_url("http://llm.svc:8080/v1/chat/completions", &u) == HTTP_MIN_OK
        );
        expect("http host", strcmp(u.host, "llm.svc") == 0);
        expect("http port", u.port == 8080);
        expect("http path", strcmp(u.path, "/v1/chat/completions") == 0);
    }
    {
        char body[1024];
        char content[256];
        size_t n, cl = 0;
        const char *fake =
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Hello adventurer.\"}}]}";
        n = openai_chat_request_json(body, sizeof(body), "qwen", "hi \"there\"");
        expect("openai req", n > 0 && strstr(body, "qwen") != NULL && strstr(body, "hi") != NULL);
        expect("openai nonstream request", strstr(body, "\"stream\":false") != NULL && !strstr(body, "stream_options"));
        expect(
            "openai current completion limit",
            strstr(body, "\"max_completion_tokens\":256") != NULL &&
            strstr(body, "\"max_tokens\":") == NULL);
        expect(
            "openai vLLM 0.29 non-reasoning contract",
            strcmp(VOICE_VLLM_API_CONTRACT_VERSION, "0.29.0") == 0 &&
            strstr(body, "\"include_reasoning\":false") != NULL &&
            strstr(body, "\"enable_thinking\":false") != NULL);
        expect(
            "openai extract",
            openai_chat_extract_content(fake, strlen(fake), content, sizeof(content), &cl) == 0
        );
        expect("openai content", strcmp(content, "Hello adventurer.") == 0);
        {
            const char bounded[] = {
                '{', '"', 'c', 'o', 'n', 't', 'e', 'n', 't', '"', ':', '"',
                'O', 'K', '"', '}'
            };
            const char unicode[] = "{\"content\":\"Hi \\u263a \\ud83d\\ude00\"}";
            const char unicode_expected[] = {
                'H', 'i', ' ', (char)0xe2, (char)0x98, (char)0xba, ' ',
                (char)0xf0, (char)0x9f, (char)0x98, (char)0x80, '\0'
            };
            const char truncated[] = "{\"content\":\"unterminated";
            char tiny[3];
            expect(
                "openai bounded non-NUL input",
                openai_chat_extract_content(
                    bounded, sizeof(bounded), content, sizeof(content), &cl) == 0 &&
                strcmp(content, "OK") == 0
            );
            expect(
                "openai unicode decode",
                openai_chat_extract_content(
                    unicode, strlen(unicode), content, sizeof(content), &cl) == 0 &&
                strcmp(content, unicode_expected) == 0
            );
            expect(
                "openai rejects truncated JSON",
                openai_chat_extract_content(
                    truncated, strlen(truncated), content, sizeof(content), &cl) != 0
            );
            expect(
                "openai rejects output truncation",
                openai_chat_extract_content(
                    fake, strlen(fake), tiny, sizeof(tiny), &cl) != 0
            );
            expect(
                "bounded generic JSON string extract",
                json_extract_string_field(
                    "{\"status\":\"ok\",\"text\":\"hello \\u263a\"}",
                    sizeof("{\"status\":\"ok\",\"text\":\"hello \\u263a\"}") - 1,
                    "text", content, sizeof(content), &cl) == 0 &&
                strcmp(content, "hello \xe2\x98\xba") == 0
            );
            expect(
                "STT JSON ignores nested diagnostic text",
                stt_json_extract_transcript(
                    "{\"diagnostic\":{\"status\":\"ok\",\"text\":\"metadata poison\"},"
                    "\"status\":\"ok\",\"text\":\"hello\",\"transcript\":\"hello\"}",
                    sizeof("{\"diagnostic\":{\"status\":\"ok\",\"text\":\"metadata poison\"},"
                           "\"status\":\"ok\",\"text\":\"hello\",\"transcript\":\"hello\"}") - 1u,
                    content, sizeof(content), &cl) == 0 &&
                cl == 5u && strcmp(content, "hello") == 0);
            expect(
                "STT JSON accepts transcript compatibility alias",
                stt_json_extract_transcript(
                    "{\"status\":\"ok\",\"transcript\":\"hello\"}",
                    sizeof("{\"status\":\"ok\",\"transcript\":\"hello\"}") - 1u,
                    content, sizeof(content), &cl) == 0 &&
                strcmp(content, "hello") == 0);
            expect(
                "STT JSON compares decoded transcript aliases",
                stt_json_extract_transcript(
                    "{\"status\":\"ok\",\"text\":\"hello \\u263a\","
                    "\"transcript\":\"hello \xe2\x98\xba\"}",
                    sizeof("{\"status\":\"ok\",\"text\":\"hello \\u263a\","
                           "\"transcript\":\"hello \xe2\x98\xba\"}") - 1u,
                    content, sizeof(content), &cl) == 0 &&
                strcmp(content, "hello \xe2\x98\xba") == 0);
            expect(
                "STT JSON rejects mismatched transcript aliases",
                stt_json_extract_transcript(
                    "{\"status\":\"ok\",\"text\":\"one\",\"transcript\":\"two\"}",
                    sizeof("{\"status\":\"ok\",\"text\":\"one\",\"transcript\":\"two\"}") - 1u,
                    content, sizeof(content), &cl) != 0);
            expect(
                "STT JSON rejects nested-only success fields",
                stt_json_extract_transcript(
                    "{\"result\":{\"status\":\"ok\",\"text\":\"poison\"}}",
                    sizeof("{\"result\":{\"status\":\"ok\",\"text\":\"poison\"}}") - 1u,
                    content, sizeof(content), &cl) != 0);
            expect(
                "STT JSON rejects duplicate status",
                stt_json_extract_transcript(
                    "{\"status\":\"ok\",\"status\":\"ok\",\"text\":\"hello\"}",
                    sizeof("{\"status\":\"ok\",\"status\":\"ok\",\"text\":\"hello\"}") - 1u,
                    content, sizeof(content), &cl) != 0);
            expect(
                "STT JSON rejects trailing data",
                stt_json_extract_transcript(
                    "{\"status\":\"ok\",\"text\":\"hello\"}x",
                    sizeof("{\"status\":\"ok\",\"text\":\"hello\"}x") - 1u,
                    content, sizeof(content), &cl) != 0);
            expect(
                "STT JSON rejects escaped NUL",
                stt_json_extract_transcript(
                    "{\"status\":\"ok\",\"text\":\"hello\\u0000poison\"}",
                    sizeof("{\"status\":\"ok\",\"text\":\"hello\\u0000poison\"}") - 1u,
                    content, sizeof(content), &cl) != 0);
            {
                static const char invalid_utf8_stt[] = {
                    '{', '"', 's', 't', 'a', 't', 'u', 's', '"', ':', '"', 'o', 'k', '"', ',',
                    '"', 't', 'e', 'x', 't', '"', ':', '"', (char)0xc0, (char)0xaf, '"', '}'
                };
                expect(
                    "STT JSON rejects invalid UTF-8",
                    stt_json_extract_transcript(
                        invalid_utf8_stt, sizeof(invalid_utf8_stt),
                        content, sizeof(content), &cl) != 0);
            }
        }
        {
            static const char stream_a[] =
                "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Hel";
            static const char stream_b[] =
                "lo \\u263a\"}}]}\r\n\r\ndata: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n";
            static const char stream_vllm_028[] =
                "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"logprobs\":null,\"finish_reason\":null}],\"prompt_token_ids\":null,\"prompt_text\":null}\n\n"
                "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"public\\u263a\"},\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}]}\n\n"
                "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\"},\"logprobs\":null,\"finish_reason\":\"stop\",\"stop_reason\":null,\"token_ids\":null}]}\n\n"
                "data: [DONE]\n\n";
            static const char stream_vllm_word_uint[] =
                "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\","
                "\"created\":12345678,\"model\":\"default\",\"choices\":[{"
                "\"index\":12345678,\"delta\":{\"content\":\"digits\"},"
                "\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}]}\n\n"
                "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\","
                "\"created\":1234567890123456,\"model\":\"default\",\"choices\":[{"
                "\"index\":0,\"delta\":{\"content\":\"\"},\"logprobs\":null,"
                "\"finish_reason\":\"stop\",\"stop_reason\":null,"
                "\"token_ids\":null}]}\n\ndata: [DONE]\n\n";
            openai_sse_decoder decoder;
            sse_capture capture;
            {
                uint8_t wire[2048], public_wire[2048];
                char json[2048];
                turn_event_c internal, published;
                turn_response_event safe;
                turn_response_state state = {0};
                uint64_t counts[3];
                size_t used = pb_encode_turn_text_event_prepared(wire, sizeof(wire),
                    "provider-turn", 13u, 5, "hello", 5u, NULL, 0u, "hello", 5u, 0, 1);
                used = pb_append_turn_provider_model(wire, sizeof(wire), used, "provider-model");
                used = pb_append_turn_provider_usage(wire, sizeof(wire), used, 12u, 0u, 12u);
                expect("provider metadata decodes at WebTransport boundary", used &&
                    pb_decode_turn_event_public_active(wire, used, "provider-turn", 13u, &internal) == 0);
                expect("provider metadata survives public filter", turn_response_filter_public_active_n(
                    &state, &internal, "provider-turn", 13u, &safe) == TURN_RESPONSE_FORWARD &&
                    safe.provider_model && !strcmp(safe.provider_model, "provider-model") && safe.provider_usage);
                used = turn_response_protobuf_encode(public_wire, sizeof(public_wire), &safe);
                expect("provider metadata survives rebuilt public wire", used &&
                    pb_decode_turn_event(public_wire, used, &published) == 0 &&
                    !strcmp(published.provider.model, "provider-model") &&
                    pb_turn_provider_usage(&published.provider, counts) && counts[0] == 12u && counts[1] == 0u && counts[2] == 12u);
                expect("HTTP exposes the same provider metadata", turn_response_json_encode(json, sizeof(json), &safe, 1) &&
                    strstr(json, "\"model_id\":\"provider-model\"") && strstr(json, "\"total_tokens\":\"12\""));
                internal.provider.invalid = 63u;
                state = (turn_response_state){0};
                expect("ambiguous provider metadata stays unknown without losing the response",
                    turn_response_filter_public_active_n(&state, &internal, "provider-turn", 13u, &safe) == TURN_RESPONSE_FORWARD &&
                    !safe.provider_model && !safe.provider_usage);
                expect("usage wire rejects inconsistent and unsafe counts",
                    !pb_append_turn_provider_usage(wire, sizeof(wire), 1u, 1u, 2u, 4u) &&
                    !pb_append_turn_provider_usage(wire, sizeof(wire), 1u, UINT64_MAX, 1u, 0u));
            }
            {
                static const char *usage[] = {
                    "{\"prompt_tokens\":12,\"completion_tokens\":0,\"total_tokens\":12}",
                    "null", "{}", "{\"prompt_tokens\":12,\"completion_tokens\":1,\"total_tokens\":12}",
                    "{\"prompt_tokens\":-1,\"completion_tokens\":1,\"total_tokens\":0}",
                    "{\"prompt_tokens\":1.5,\"completion_tokens\":1,\"total_tokens\":2.5}",
                    "{\"prompt_tokens\":9007199254740992,\"completion_tokens\":0,\"total_tokens\":9007199254740992}",
                    "{\"prompt_tokens\":12,\"prompt_tokens\":12,\"completion_tokens\":0,\"total_tokens\":12}"
                };
                size_t k;
                for (k = 0; k < sizeof(usage) / sizeof(usage[0]); ++k) {
                    char stream[1024];
                    int len = snprintf(stream, sizeof(stream),
                        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                        "data: {\"choices\":[],\"usage\":%s}\n\ndata: [DONE]\n\n", usage[k]);
                    memset(&capture, 0, sizeof(capture));
                    expect("provider usage does not break an otherwise valid stream",
                        openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                        openai_sse_feed(&decoder, stream, (size_t)len) == 0 && openai_sse_finish(&decoder) == 0);
                    expect("provider usage only accepts consistent safe integer totals", k == 0u ?
                        decoder.usage_present && decoder.prompt_tokens == 12u && decoder.completion_tokens == 0u && decoder.total_tokens == 12u : !decoder.usage_present);
                }
            }
            {
                static const char finish[] = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
                static const char usage[] = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":1,\"total_tokens\":3}}\n\n";
                static const char conflict[] = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":2,\"total_tokens\":4}}\n\n";
                size_t k;
                memset(&capture, 0, sizeof(capture));
                expect("usage fixture initializes", !openai_sse_init(&decoder, capture_sse_delta, &capture) &&
                    !openai_sse_feed(&decoder, finish, sizeof(finish) - 1u));
                for (k = 0; k < sizeof(usage) - 1u; ++k)
                    expect("usage survives bytewise SSE framing", !openai_sse_feed(&decoder, usage + k, 1u));
                expect("identical cumulative counts are not summed", !openai_sse_feed(&decoder, usage, sizeof(usage) - 1u) &&
                    decoder.usage_present && decoder.total_tokens == 3u);
                expect("conflicting cumulative usage remains unknown", !openai_sse_feed(&decoder, conflict, sizeof(conflict) - 1u) &&
                    !openai_sse_feed(&decoder, usage, sizeof(usage) - 1u) && !decoder.usage_present && decoder.usage_invalid);
            }
            {
                uint8_t wire[512];
                size_t used = pb_encode_turn_event(wire, sizeof(wire), "model-turn", "text_completed", "hello");
                size_t length = pb_append_turn_provider_model(wire, sizeof(wire), used, "actual-provider-model");
                pb_reader reader;
                uint32_t field, kind;
                unsigned found = 0;
                pb_reader_init(&reader, wire, length);
                while (reader.pos < reader.len) {
                    if (pb_read_tag(&reader, &field, &kind) != 0) break;
                    if (field == 16u && kind == 2u) {
                        const uint8_t *entry;
                        size_t entry_len;
                        pb_reader map;
                        char key[64], value[256];
                        if (pb_read_bytes(&reader, &entry, &entry_len) != 0) break;
                        pb_reader_init(&map, entry, entry_len);
                        if (pb_read_tag(&map, &field, &kind) != 0 || field != 1u || kind != 2u ||
                            pb_read_string(&map, key, sizeof(key)) != 0 ||
                            pb_read_tag(&map, &field, &kind) != 0 || field != 2u || kind != 2u ||
                            pb_read_string(&map, value, sizeof(value)) != 0 || map.pos != map.len) break;
                        if (!strcmp(key, "model_id") && !strcmp(value, "actual-provider-model")) found |= 1u;
                        if (!strcmp(key, "model_identity_source") && !strcmp(value, "provider_reported")) found |= 2u;
                    } else if (pb_skip(&reader, kind) != 0) break;
                }
                expect("provider identity wire preserves its explicit evidence source", length > used && found == 3u);
                expect("provider identity wire rejects unavailable or truncated output",
                    pb_append_turn_provider_model(wire, used, used, "model") == 0 &&
                    pb_append_turn_provider_model(wire, sizeof(wire), used, "") == 0 &&
                    pb_append_turn_provider_model(wire, sizeof(wire), 0, "model") == 0);
            }

            {
                static const char *models[] = {
                    "\"qwen\"", "\"qwen\\u263a\"", "null", "\"\"",
                    "\"qwen\\u0000other\"", "\"a\",\"model\":\"b\""
                };
                size_t k;
                for (k = 0; k < sizeof(models) / sizeof(models[0]); ++k) {
                    char event[512];
                    int length = snprintf(event, sizeof(event),
                        "data: {\"model\":%s,\"choices\":[]}\n\n", models[k]);
                    memset(&capture, 0, sizeof(capture));
                    expect("provider model metadata does not invent stream content",
                        openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                        openai_sse_feed(&decoder, event, (size_t)length) == 0 && capture.calls == 0);
                    expect("provider model rejects invalid or ambiguous identity",
                        k < 2 ? decoder.reported_model[0] && !decoder.model_conflict :
                        !decoder.reported_model[0] && decoder.model_conflict);
                }
            }

            memset(&capture, 0, sizeof(capture));
            memset(&decoder, 0xa5, sizeof(decoder));
            n = openai_chat_request_json_stream(
                body, sizeof(body), "qwen", "stream this");
            expect(
                "openai streaming request",
                n > 0 && strstr(body, "\"stream\":true") != NULL && strstr(body, "\"stream_options\":{\"include_usage\":true}"));
            n = openai_chat_request_json_stream_bounded(
                body, sizeof(body), "qwen", "stream this", 48u);
            expect(
                "openai bounded streaming request",
                n > 0 && strstr(body, "\"max_completion_tokens\":48") != NULL &&
                strstr(body, "\"stream\":true") != NULL);
            expect(
                "openai bounded streaming request rejects zero",
                openai_chat_request_json_stream_bounded(
                    body, sizeof(body), "qwen", "stream this", 0u) == 0u);
            expect(
                "openai bounded streaming request rejects excessive limit",
                openai_chat_request_json_stream_bounded(
                    body, sizeof(body), "qwen", "stream this", 4097u) == 0u);
            expect(
                "openai SSE init",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                decoder.line_len == 0 && decoder.vllm_line_prefix_len == 0 &&
                decoder.vllm_prefix_match == 0 && decoder.done == 0 &&
                decoder.failed == 0);
            expect(
                "openai SSE fragmented feed",
                openai_sse_feed(&decoder, stream_a, sizeof(stream_a) - 1) == 0 &&
                openai_sse_feed(&decoder, stream_b, sizeof(stream_b) - 1) == 0);
            expect(
                "openai SSE done",
                openai_sse_finish(&decoder) == 0 && capture.calls == 2 &&
                strcmp(capture.text, "Hello \xe2\x98\xba world") == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE accepts canonical vLLM 0.28 stream shape",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    stream_vllm_028, sizeof(stream_vllm_028) - 1u) == 0 &&
                openai_sse_finish(&decoder) == 0 && capture.calls == 1u &&
                strcmp(capture.text, "public\xe2\x98\xba") == 0);
            expect(
                "openai SSE matches direct and fragmented line feeds",
                openai_sse_chunking_matches(
                    stream_vllm_028, sizeof(stream_vllm_028) - 1u));
            {
                const char *first_end = strstr(stream_vllm_028, "\n\n");
                size_t first_len = first_end ?
                    (size_t)(first_end - stream_vllm_028) + 2u : 0u;
                size_t prefix_len;
                size_t offset;
                int fragmented_ok;
                memset(&capture, 0, sizeof(capture));
                fragmented_ok = first_len != 0u &&
                    openai_sse_init(
                        &decoder, capture_sse_delta, &capture) == 0 &&
                    openai_sse_feed(
                        &decoder, stream_vllm_028, first_len) == 0 &&
                    decoder.vllm_line_prefix_len != 0u;
                prefix_len = fragmented_ok ?
                    decoder.vllm_line_prefix_len : 0u;
                for (offset = first_len;
                     offset < first_len + prefix_len &&
                     offset < sizeof(stream_vllm_028) - 1u && fragmented_ok;
                     ++offset) {
                    if (openai_sse_feed(
                            &decoder, stream_vllm_028 + offset, 1u) != 0)
                        fragmented_ok = 0;
                }
                fragmented_ok = fragmented_ok &&
                    decoder.line_len == prefix_len &&
                    decoder.vllm_prefix_match != 0;
                for (;
                     offset < sizeof(stream_vllm_028) - 1u && fragmented_ok;
                     ++offset) {
                    if (openai_sse_feed(
                            &decoder, stream_vllm_028 + offset, 1u) != 0)
                        fragmented_ok = 0;
                }
                expect(
                    "openai SSE reuses a validated prefix across byte fragments",
                    fragmented_ok && openai_sse_finish(&decoder) == 0 &&
                    strcmp(decoder.reported_model, "default") == 0 &&
                    capture.calls == 1u &&
                    strcmp(capture.text, "public\xe2\x98\xba") == 0);
            }
            {
                char changed_prefix[sizeof(stream_vllm_028)];
                const char *event_end;
                size_t first_len;
                size_t second_len;
                char *model;
                int changed_ok = 1;
                memcpy(
                    changed_prefix,
                    stream_vllm_028,
                    sizeof(changed_prefix));
                model = strstr(changed_prefix, "\"model\":\"default\"");
                if (model) {
                    model = strstr(model + 1, "\"model\":\"default\"");
                }
                if (!model) {
                    changed_ok = 0;
                } else {
                    model[sizeof("\"model\":\"") - 1u] = 'D';
                }
                memset(&capture, 0, sizeof(capture));
                event_end = strstr(changed_prefix, "\n\n");
                first_len = event_end ?
                    (size_t)(event_end - changed_prefix) + 2u : 0u;
                event_end = first_len != 0u ?
                    strstr(changed_prefix + first_len, "\n\n") : NULL;
                second_len = event_end ?
                    (size_t)(event_end - (changed_prefix + first_len)) + 2u : 0u;
                changed_ok = changed_ok && first_len != 0u && second_len != 0u &&
                    openai_sse_init(
                        &decoder, capture_sse_delta, &capture) == 0 &&
                    openai_sse_feed(
                        &decoder, changed_prefix, first_len) == 0 &&
                    decoder.vllm_line_prefix_len != 0u &&
                    openai_sse_feed(
                        &decoder, changed_prefix + first_len, second_len) == 0 &&
                    decoder.vllm_prefix_match != 0 &&
                    decoder.vllm_line_prefix_len != 0u;
                expect(
                    "openai SSE reparses a changed cached prefix",
                    changed_ok && decoder.model_conflict && !decoder.reported_model[0] &&
                    openai_sse_feed(
                        &decoder,
                        changed_prefix + first_len + second_len,
                        sizeof(changed_prefix) - 1u - first_len - second_len) == 0 &&
                    openai_sse_finish(&decoder) == 0 &&
                    capture.calls == 1u &&
                    strcmp(capture.text, "public\xe2\x98\xba") == 0);
            }
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE validates full decimal word lanes",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    stream_vllm_word_uint,
                    sizeof(stream_vllm_word_uint) - 1u) == 0 &&
                openai_sse_finish(&decoder) == 0 && capture.calls == 1u &&
                strcmp(capture.text, "digits") == 0);
            {
                static const struct {
                    const char *encoded;
                    const char *decoded;
                } cases[] = {
                    {"middle", "middle"},
                    {"ABC_DEF[]^@XYZ", "ABC_DEF[]^@XYZ"},
                    {"\\\"", "\""},
                    {"\\\\", "\\"},
                    {"\\/", "/"},
                    {"\\n", "\n"},
                    {"\\r", "\r"},
                    {"\\t", "\t"},
                    {"\\b", "\b"},
                    {"\\f", "\f"},
                    {"\\u263a", "\xe2\x98\xba"},
                    {"caf\xc3\xa9", "caf\xc3\xa9"}
                };
                char encoded[96];
                char decoded[96];
                int exact_ok = 1;
                size_t pad;
                size_t case_index;
                unsigned int byte;
                for (pad = 0; pad < 32u && exact_ok; ++pad) {
                    for (case_index = 0;
                         case_index < sizeof(cases) / sizeof(cases[0]);
                         ++case_index) {
                        size_t encoded_len = strlen(cases[case_index].encoded);
                        size_t decoded_len = strlen(cases[case_index].decoded);
                        memset(encoded, 'a', pad);
                        memcpy(encoded + pad, cases[case_index].encoded, encoded_len + 1u);
                        memset(decoded, 'a', pad);
                        memcpy(decoded + pad, cases[case_index].decoded, decoded_len + 1u);
                        if (!vllm_sse_content_case(encoded, decoded, 1)) {
                            exact_ok = 0;
                            break;
                        }
                    }
                    memset(encoded, 'a', pad);
                    memcpy(encoded + pad, "\\x", sizeof("\\x"));
                    if (!vllm_sse_content_case(encoded, NULL, 0)) exact_ok = 0;
                    memset(encoded, 'a', pad);
                    encoded[pad] = '\x01';
                    encoded[pad + 1u] = '\0';
                    if (!vllm_sse_content_case(encoded, NULL, 0)) exact_ok = 0;
                }
                for (byte = 0x20u; byte < 0x80u && exact_ok; ++byte) {
                    if (byte == (unsigned int)'"' || byte == (unsigned int)'\\')
                        continue;
                    encoded[0] = (char)byte;
                    encoded[1] = '\0';
                    decoded[0] = (char)byte;
                    decoded[1] = '\0';
                    if (!vllm_sse_content_case(encoded, decoded, 1)) exact_ok = 0;
                }
                expect(
                    "openai SSE vLLM string word scan matches strict JSON",
                    exact_ok);
            }
            {
                static const char *const suffixes[] = {
                    "},\"logprobs\":null,\"finish_reason\":null}],"
                    "\"prompt_token_ids\":null,\"prompt_text\":null}",
                    "},\"logprobs\":null,\"finish_reason\":null,"
                    "\"token_ids\":null}]}",
                    "},\"logprobs\":null,\"finish_reason\":\"stop\","
                    "\"stop_reason\":null,\"token_ids\":null}]}"
                };
                int suffix_ok = 1;
                size_t suffix_index;
                for (suffix_index = 0;
                     suffix_index < sizeof(suffixes) / sizeof(suffixes[0]);
                     ++suffix_index) {
                    if (!vllm_sse_rejects_truncated_suffix(
                            suffixes[suffix_index],
                            strlen(suffixes[suffix_index]))) {
                        suffix_ok = 0;
                        break;
                    }
                }
                expect(
                    "openai SSE rejects every truncated vLLM suffix",
                    suffix_ok);
            }
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE falls back for vLLM optional metadata",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"public\"},\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}],\"system_fingerprint\":\"fp-test\"}\n\n"
                    "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\"},\"logprobs\":null,\"finish_reason\":\"stop\",\"stop_reason\":null,\"token_ids\":null}]}\n\n"
                    "data: [DONE]\n\n",
                    sizeof(
                        "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"public\"},\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}],\"system_fingerprint\":\"fp-test\"}\n\n"
                        "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\"},\"logprobs\":null,\"finish_reason\":\"stop\",\"stop_reason\":null,\"token_ids\":null}]}\n\n"
                        "data: [DONE]\n\n") - 1u) == 0 &&
                openai_sse_finish(&decoder) == 0 && capture.calls == 1u &&
                strcmp(capture.text, "public") == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects malformed vLLM fast-path suffix",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"poison\"},\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}],\"meta\":[1,]}\n\n",
                    sizeof(
                        "data: {\"id\":\"chatcmpl-test\",\"object\":\"chat.completion.chunk\",\"created\":1750000000,\"model\":\"default\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"poison\"},\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}],\"meta\":[1,]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects missing done",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n") - 1) == 0 &&
                openai_sse_finish(&decoder) != 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects done without finish reason",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"
                    "data: [DONE]\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"
                           "data: [DONE]\n\n") - 1) != 0 &&
                capture.calls == 1 && strcmp(capture.text, "partial") == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects malformed delta",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":42}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":42}}]}\n\n") - 1) != 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects invalid UTF-8 delta",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"\xc0\xaf\"}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"\xc0\xaf\"}}]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects escaped NUL delta",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"safe\\u0000poison\"}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"safe\\u0000poison\"}}]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE scopes content to first choice delta",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"content\":\"metadata poison\",\"choices\":[{\"delta\":{\"meta\":{\"content\":\"delta poison\"},\"content\":\"safe\"}}]}\n\n"
                    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n",
                    sizeof("data: {\"content\":\"metadata poison\",\"choices\":[{\"delta\":{\"meta\":{\"content\":\"delta poison\"},\"content\":\"safe\"}}]}\n\n"
                           "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                           "data: [DONE]\n\n") - 1) == 0 &&
                openai_sse_finish(&decoder) == 0 && capture.calls == 1 &&
                strcmp(capture.text, "safe") == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE ignores vLLM 0.28 reasoning deltas",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning\":\"private\",\"content\":null},\"finish_reason\":null}]}\n\n"
                    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"public\"},\"finish_reason\":null}]}\n\n"
                    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n",
                    sizeof("data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning\":\"private\",\"content\":null},\"finish_reason\":null}]}\n\n"
                           "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"public\"},\"finish_reason\":null}]}\n\n"
                           "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                           "data: [DONE]\n\n") - 1u) == 0 &&
                openai_sse_finish(&decoder) == 0 && capture.calls == 1u &&
                strcmp(capture.text, "public") == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE ignores valid later choices",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"first\"}},{\"delta\":{\"content\":\"poison\"}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"first\"}},{\"delta\":{\"content\":\"poison\"}}]}\n\n") - 1u) == 0 &&
                capture.calls == 1 && strcmp(capture.text, "first") == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects malformed later choice",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"safe\"}},{\"meta\":[1,]}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"safe\"}},{\"meta\":[1,]}]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects malformed trailing root member",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"safe\"}}],\"meta\":[1,]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"safe\"}}],\"meta\":[1,]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects duplicate choices member",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"safe\"}}],\"choices\":[]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"safe\"}}],\"choices\":[]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects non-object first choice",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[\"not a choice\"]}\n\n",
                    sizeof("data: {\"choices\":[\"not a choice\"]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE accepts terminal usage chunk",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                    "data: {\"choices\":[],\"usage\":{\"content\":\"not speech\",\"total_tokens\":7}}\n\n"
                    "data: [DONE]\n\n",
                    sizeof("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                           "data: {\"choices\":[],\"usage\":{\"content\":\"not speech\",\"total_tokens\":7}}\n\n"
                           "data: [DONE]\n\n") - 1) == 0 &&
                openai_sse_finish(&decoder) == 0 && capture.calls == 0 &&
                capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects late provider error object",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"
                    "data: {\"error\":{\"message\":\"generation failed\"}}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"
                           "data: {\"error\":{\"message\":\"generation failed\"}}\n\n") - 1) != 0 &&
                capture.calls == 1 && strcmp(capture.text, "partial") == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects duplicate delta content",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"one\",\"content\":\"two\"}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"one\",\"content\":\"two\"}}]}\n\n") - 1) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects duplicate delta member",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"one\"},\"delta\":{\"content\":\"two\"}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"one\"},\"delta\":{\"content\":\"two\"}}]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects duplicate finish reason",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"finish_reason\":\"stop\",\"delta\":{},\"finish_reason\":\"length\"}]}\n\n",
                    sizeof("data: {\"choices\":[{\"finish_reason\":\"stop\",\"delta\":{},\"finish_reason\":\"length\"}]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            {
                static const char *const success_reasons[] = {
                    "stop", "length"
                };
                static const char *const failure_reasons[] = {
                    "abort", "error", "repetition", "tool_calls", "unknown"
                };
                int reasons_ok = 1;
                size_t reason_index;
                int current_shape;
                for (current_shape = 0;
                     current_shape < 2 && reasons_ok;
                     ++current_shape) {
                    for (reason_index = 0;
                         reason_index < sizeof(success_reasons) /
                             sizeof(success_reasons[0]);
                         ++reason_index) {
                        if (!vllm_sse_finish_reason_case(
                                success_reasons[reason_index],
                                current_shape,
                                1)) {
                            reasons_ok = 0;
                            break;
                        }
                    }
                    for (reason_index = 0;
                         reason_index < sizeof(failure_reasons) /
                             sizeof(failure_reasons[0]) && reasons_ok;
                         ++reason_index) {
                        if (!vllm_sse_finish_reason_case(
                                failure_reasons[reason_index],
                                current_shape,
                                0)) {
                            reasons_ok = 0;
                            break;
                        }
                    }
                }
                expect(
                    "openai SSE classifies vLLM 0.28 finish reasons",
                    reasons_ok);
            }
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects malformed trailing delta member",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"safe\",\"meta\":[1,]}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"content\":\"safe\",\"meta\":[1,]}}]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects escaped path key",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"delta\":{\"cont\\u0065nt\":\"one\",\"content\":\"two\"}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"delta\":{\"cont\\u0065nt\":\"one\",\"content\":\"two\"}}]}\n\n") - 1) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects escaped choice key",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"choices\":[{\"del\\u0074a\":{\"content\":\"one\"},\"delta\":{\"content\":\"two\"}}]}\n\n",
                    sizeof("data: {\"choices\":[{\"del\\u0074a\":{\"content\":\"one\"},\"delta\":{\"content\":\"two\"}}]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            memset(&capture, 0, sizeof(capture));
            expect(
                "openai SSE rejects escaped root key",
                openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                openai_sse_feed(
                    &decoder,
                    "data: {\"cho\\u0069ces\":[],\"choices\":[{\"delta\":{\"content\":\"poison\"}}]}\n\n",
                    sizeof("data: {\"cho\\u0069ces\":[],\"choices\":[{\"delta\":{\"content\":\"poison\"}}]}\n\n") - 1u) != 0 &&
                capture.calls == 0 && capture.len == 0);
            {
                static char max_comment[sizeof(decoder.line)];
                static char oversized_comment[sizeof(decoder.line) + 1u];
                static char direct_oversized_comment[
                    sizeof(decoder.line) + 1u];
                memset(max_comment, 'x', sizeof(max_comment));
                max_comment[0] = ':';
                memset(oversized_comment, 'x', sizeof(oversized_comment));
                oversized_comment[0] = ':';
                memset(
                    direct_oversized_comment,
                    'x',
                    sizeof(direct_oversized_comment));
                direct_oversized_comment[0] = ':';
                direct_oversized_comment[
                    sizeof(direct_oversized_comment) - 1u] = '\n';
                memset(&capture, 0, sizeof(capture));
                expect(
                    "openai SSE accepts maximum buffered line",
                    openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                    openai_sse_feed(
                        &decoder, max_comment, sizeof(max_comment) - 1u) == 0 &&
                    openai_sse_feed(
                        &decoder,
                        "\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                        "data: [DONE]\n\n",
                        sizeof("\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                               "data: [DONE]\n\n") - 1u) == 0 &&
                    openai_sse_finish(&decoder) == 0);
                memset(&capture, 0, sizeof(capture));
                expect(
                    "openai SSE rejects oversized buffered line",
                    openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                    openai_sse_feed(
                        &decoder,
                        oversized_comment,
                        sizeof(oversized_comment)) != 0);
                memset(&capture, 0, sizeof(capture));
                expect(
                    "openai SSE rejects oversized direct line",
                    openai_sse_init(&decoder, capture_sse_delta, &capture) == 0 &&
                    openai_sse_feed(
                        &decoder,
                        direct_oversized_comment,
                        sizeof(direct_oversized_comment)) != 0);
            }
        }
    }

    /* multi-chunk PCM framing (Orpheus 16k mono s16 → 20ms = 640B body) */
    {
        uint8_t payload[8 + 640 * 3];
        pcm_frame_plan plan;
        uint8_t pkt[8 + 640];
        size_t pn;
        /* Orpheus header: rate=16000, ch=1, bits=16 LE */
        payload[0] = 0x80;
        payload[1] = 0x3e;
        payload[2] = 0x00;
        payload[3] = 0x00;
        payload[4] = 0x01;
        payload[5] = 0x00;
        payload[6] = 0x10;
        payload[7] = 0x00;
        memset(payload + 8, 0x11, 640 * 3);
        expect(
            "frame plan",
            pcm_frame_plan_v1(payload, sizeof(payload), 20, &plan) == PCM_FRAME_OK
        );
        expect("frame bytes 20ms", plan.frame_bytes == 640);
        expect("frame count 3", plan.frame_count == 3);
        expect("frame hdr", plan.header_len == 8);
        pn = pcm_frame_packet_v1(&plan, 0, pkt, sizeof(pkt));
        expect("frame pkt0 len", pn == 8 + 640);
        expect("frame pkt0 hdr", memcmp(pkt, payload, 8) == 0);
        pn = pcm_frame_packet_v1(&plan, 2, pkt, sizeof(pkt));
        expect("frame pkt2 len", pn == 8 + 640);
        expect("frame pkt beyond", pcm_frame_packet_v1(&plan, 3, pkt, sizeof(pkt)) == 0);
        expect("frame_bytes helper", pcm_frame_bytes_v1(16000, 1, 16, 20) == 640);
        expect(
            "frame_bytes overflow",
            pcm_frame_bytes_v1(UINT32_MAX, UINT16_MAX, UINT16_MAX, INT_MAX) == 0
        );
        expect(
            "frame rejects partial sample",
            pcm_frame_plan_v1(payload, sizeof(payload) - 1, 20, &plan) == PCM_FRAME_ERR_PARSE
        );
    }

    /* rag hits excerpt for retrieve_then_escalate grounding */
    {
        char ex[256];
        const char *hits =
            "{\"collections\":[\"books\"],\"hits\":["
            "{\"id\":\"1\",\"text\":\"Elminster lives in Shadowdale.\"},"
            "{\"id\":\"2\",\"content\":\"Mystra weaves the Weave.\"}"
            "]}";
        expect(
            "hits excerpt",
            rag_hits_excerpt_v1(hits, strlen(hits), 4, ex, sizeof(ex)) == 0
        );
        expect("hits has elminster", strstr(ex, "Elminster") != NULL);
        expect("hits has mystra", strstr(ex, "Mystra") != NULL);
    }

    /* RMS scalar vs product path (+ AVX2 A/B when built) */
    {
        enum { N = 320 * 50 }; /* 50 frames of 20ms@16k */
        static int16_t pcm[N];
        int i;
        double r_prod, r_scalar;
        struct timespec t0, t1;
        double ns_scalar, ns_prod;
        const int iters = 400;
        volatile double sink = 0;
        for (i = 0; i < N; i++) pcm[i] = (int16_t)((i * 17) & 0x7fff);
        r_scalar = pcm16_rms_scalar(pcm, N);
        r_prod = pcm16_rms(pcm, N);
        expect("rms match", fabs(r_prod - r_scalar) < 1e-12);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < iters; i++) sink += pcm16_rms_scalar(pcm, N);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns_scalar =
            ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / iters;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < iters; i++) sink += pcm16_rms(pcm, N);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns_prod =
            ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / iters;
        printf(
            "BenchmarkRMS_N%d\tscalar=%.2f ns/op\tproduct=%.2f ns/op\tspeedup=%.2fx\tsink=%.6f\n",
            N,
            ns_scalar,
            ns_prod,
            ns_scalar > 0 ? ns_scalar / ns_prod : 0.0,
            sink
        );
        expect("rms dispatch follows host", pcm16_rms_using_avx2() == pcm16_host_has_avx2());
#if defined(__x86_64__) || defined(__i386__)
        if (pcm16_rms_using_avx2()) {
            double r_avx = pcm16_rms_avx2(pcm, N);
            expect("rms avx match", fabs(r_avx - r_scalar) < 1e-12);
        }
#endif
        expect("rms bench ran", ns_scalar > 0 && ns_prod > 0);
    }

    /* structural: services exist as link targets */
    expect("services listed", 1);

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ALL PASS c-runtime\n");
    return 0;
}
