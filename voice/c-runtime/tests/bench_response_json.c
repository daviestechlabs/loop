#define _POSIX_C_SOURCE 200809L

#include "byte_ring.h"
#include "turn_response_json.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BENCH_OUTPUT_CAP 256u
#define BENCH_RING_FRAME_CAP 4096u
#define BENCH_RING_AUDIO_BYTES 960u
#define BENCH_TEXT \
    "The ward holds; answer with one precise move, then guard the eastern door."
#define BENCH_ESCAPED_TEXT \
    "The dragon says, \"hold\"; guard C:\\vault\\east while Caf\xc3\xa9 watches."
#define BENCH_FILTER_TEXT \
    "The dragon says, \"hold\"; guard C:\\vault\\east. " \
    "Mirt answers, \"wait\"; watch C:\\vault\\west. " \
    "The dragon says, \"hold\"; guard C:\\vault\\east. " \
    "Mirt answers, \"wait\"; watch C:\\vault\\west. " \
    "The dragon says, \"hold\"; guard C:\\vault\\east. " \
    "Mirt answers, \"wait\"; watch C:\\vault\\west. " \
    "The dragon says, \"hold\"; guard C:\\vault\\east. " \
    "Mirt answers, \"wait\"; watch C:\\vault\\west."

typedef size_t (*encode_fn)(char *, size_t, int64_t);

typedef struct {
    int32_t sample_rate;
    int32_t channels;
    int32_t bit_depth;
    int32_t audio_encoding;
    int32_t sequence;
    int32_t segment_index;
    int is_final;
} pcm_case;

typedef size_t (*pcm_encode_fn)(char *, size_t, const pcm_case *);
typedef size_t (*ring_encode_fn)(
    voice_byte_ring *, const turn_response_event *, int64_t);

static size_t reference_started(
    char *out,
    size_t out_cap,
    int64_t timestamp_ms
) {
    int written;
    if (!out || out_cap == 0u || timestamp_ms <= 0) return 0u;
    written = snprintf(
        out,
        out_cap,
        "{\"type\":\"started\",\"request_id\":\"request\","
        "\"timestamp\":%" PRId64 "}\n",
        timestamp_ms);
    if (written < 0 || (size_t)written >= out_cap) {
        out[0] = '\0';
        return 0u;
    }
    return (size_t)written;
}

static size_t product_started(
    char *out,
    size_t out_cap,
    int64_t timestamp_ms
) {
    static const turn_response_event event = {
        .request_id = "request",
        .type = "started",
        .request_id_len = sizeof("request") - 1u,
        .type_len = sizeof("started") - 1u,
        .type_id = 1,
    };
    return turn_response_json_encode(out, out_cap, &event, timestamp_ms);
}

static size_t product_text_fallback(
    char *out,
    size_t out_cap,
    int64_t timestamp_ms
) {
    static const turn_response_event event = {
        .request_id = "request",
        .type = "text_delta",
        .request_id_len = sizeof("request") - 1u,
        .type_len = sizeof("text_delta") - 1u,
        .text = BENCH_TEXT,
        .display_text = BENCH_TEXT,
        .type_id = 4,
    };
    return turn_response_json_encode(out, out_cap, &event, timestamp_ms);
}

static size_t product_text_cached(
    char *out,
    size_t out_cap,
    int64_t timestamp_ms
) {
    static const turn_response_event event = {
        .request_id = "request",
        .type = "text_delta",
        .request_id_len = sizeof("request") - 1u,
        .type_len = sizeof("text_delta") - 1u,
        .text_len = sizeof(BENCH_TEXT) - 1u,
        .text_json_raw = 1u,
        .text = BENCH_TEXT,
        .display_text = BENCH_TEXT,
        .type_id = 4,
    };
    return turn_response_json_encode(out, out_cap, &event, timestamp_ms);
}

static size_t product_text_escaped_strict(
    char *out,
    size_t out_cap,
    int64_t timestamp_ms
) {
    static const turn_response_event event = {
        .request_id = "request",
        .type = "text_delta",
        .request_id_len = sizeof("request") - 1u,
        .type_len = sizeof("text_delta") - 1u,
        .text_len = sizeof(BENCH_ESCAPED_TEXT) - 1u,
        .text = BENCH_ESCAPED_TEXT,
        .display_text = BENCH_ESCAPED_TEXT,
        .type_id = 4,
    };
    return turn_response_json_encode(out, out_cap, &event, timestamp_ms);
}

static size_t product_text_escaped_validated(
    char *out,
    size_t out_cap,
    int64_t timestamp_ms
) {
    static const turn_response_event event = {
        .request_id = "request",
        .type = "text_delta",
        .request_id_len = sizeof("request") - 1u,
        .type_len = sizeof("text_delta") - 1u,
        .text_len = sizeof(BENCH_ESCAPED_TEXT) - 1u,
        .text_validated = 1u,
        .text = BENCH_ESCAPED_TEXT,
        .display_text = BENCH_ESCAPED_TEXT,
        .type_id = 4,
    };
    return turn_response_json_encode(out, out_cap, &event, timestamp_ms);
}

static size_t reference_pcm(
    char *out,
    size_t out_cap,
    const pcm_case *values
) {
    int written;
    if (!out || out_cap == 0u || !values) return 0u;
    written = snprintf(
        out,
        out_cap,
        "{\"type\":\"pcm_chunk\",\"request_id\":\"request\","
        "\"audio_base64\":\"AAECAw==\",\"sample_rate\":%" PRId32
        ",\"channels\":%" PRId32 ",\"bit_depth\":%" PRId32
        ",\"audio_encoding\":%" PRId32 ",\"sequence\":%" PRId32
        ",\"segment_index\":%" PRId32 ",\"is_final\":%s,"
        "\"timestamp\":1787774400000}\n",
        values->sample_rate,
        values->channels,
        values->bit_depth,
        values->audio_encoding,
        values->sequence,
        values->segment_index,
        values->is_final ? "true" : "false");
    if (written < 0 || (size_t)written >= out_cap) {
        out[0] = '\0';
        return 0u;
    }
    return (size_t)written;
}

static size_t product_pcm(
    char *out,
    size_t out_cap,
    const pcm_case *values
) {
    static const uint8_t audio[] = {0u, 1u, 2u, 3u};
    turn_response_event event;
    if (!values) return 0u;
    memset(&event, 0, sizeof(event));
    event.request_id = "request";
    event.type = "pcm_chunk";
    event.request_id_len = sizeof("request") - 1u;
    event.type_len = sizeof("pcm_chunk") - 1u;
    event.type_id = 8;
    event.audio = audio;
    event.audio_len = sizeof(audio);
    event.sample_rate = values->sample_rate;
    event.channels = values->channels;
    event.bit_depth = values->bit_depth;
    event.audio_encoding = values->audio_encoding;
    event.sequence = values->sequence;
    event.segment_index = values->segment_index;
    event.is_final = values->is_final;
    return turn_response_json_encode(
        out, out_cap, &event, INT64_C(1787774400000));
}

static void ring_pcm_event_init(turn_response_event *event) {
    static const uint8_t audio[BENCH_RING_AUDIO_BYTES] = {0u};
    memset(event, 0, sizeof(*event));
    event->request_id = "request";
    event->type = "pcm_chunk";
    event->request_id_len = sizeof("request") - 1u;
    event->type_len = sizeof("pcm_chunk") - 1u;
    event->type_id = 8;
    event->audio = audio;
    event->audio_len = sizeof(audio);
    event->sample_rate = 24000;
    event->channels = 1;
    event->bit_depth = 16;
    event->audio_encoding = 1;
    event->segment_index = 2;
}

static size_t ring_encode_staged(
    voice_byte_ring *ring,
    const turn_response_event *event,
    int64_t timestamp_ms
) {
    char frame[BENCH_RING_FRAME_CAP];
    size_t frame_len = turn_response_json_encode(
        frame, sizeof(frame), event, timestamp_ms);
    if (frame_len == 0u || !voice_byte_ring_write(ring, frame, frame_len))
        return 0u;
    return frame_len;
}

static size_t ring_encode_direct(
    voice_byte_ring *ring,
    const turn_response_event *event,
    int64_t timestamp_ms
) {
    uint8_t *span;
    size_t span_len;
    size_t frame_len;
    if (!voice_byte_ring_write_span(ring, &span, &span_len) ||
        span_len < BENCH_RING_FRAME_CAP)
        return 0u;
    frame_len = turn_response_json_encode(
        (char *)span, BENCH_RING_FRAME_CAP, event, timestamp_ms);
    if (frame_len == 0u || !voice_byte_ring_write_commit(ring, frame_len))
        return 0u;
    return frame_len;
}

static int verify_ring_equivalence(void) {
    voice_byte_ring staged;
    voice_byte_ring direct;
    turn_response_event event;
    size_t staged_len;
    size_t direct_len;
    memset(&staged, 0, sizeof(staged));
    memset(&direct, 0, sizeof(direct));
    ring_pcm_event_init(&event);
    staged_len = ring_encode_staged(
        &staged, &event, INT64_C(1787774400000));
    direct_len = ring_encode_direct(
        &direct, &event, INT64_C(1787774400000));
    return staged_len != 0u && direct_len == staged_len &&
        staged.size == staged_len && direct.size == direct_len &&
        memcmp(staged.data, direct.data, staged_len) == 0;
}

static int verify_pcm_equivalence(void) {
    static const pcm_case cases[] = {
        {16000, 1, 16, 1, 0, 0, 1},
        {24000, 1, 16, 1, 7, 2, 0},
        {44100, 1, 16, 1, 99, 10, 1},
        {44100, 2, 24, 2, INT32_MAX, INT32_MAX, 1},
        {16000, 2, 16, 1, -1, -2, 0},
        {24000, 1, 24, 1, INT32_MIN, INT32_MIN, 1},
        {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MIN, 0, 0, 0},
    };
    char baseline[BENCH_OUTPUT_CAP];
    char candidate[BENCH_OUTPUT_CAP];
    size_t value_case;
    for (value_case = 0;
         value_case < sizeof(cases) / sizeof(cases[0]);
         ++value_case) {
        size_t baseline_len = reference_pcm(
            baseline, sizeof(baseline), &cases[value_case]);
        size_t candidate_len = product_pcm(
            candidate, sizeof(candidate), &cases[value_case]);
        size_t full_len = baseline_len;
        size_t out_cap;
        if (baseline_len == 0u || candidate_len != baseline_len ||
            memcmp(baseline, candidate, baseline_len + 1u) != 0)
            return 0;
        for (out_cap = 1u; out_cap <= full_len + 1u; ++out_cap) {
            baseline_len = reference_pcm(
                baseline, out_cap, &cases[value_case]);
            candidate_len = product_pcm(
                candidate, out_cap, &cases[value_case]);
            if ((baseline_len == 0u) != (candidate_len == 0u) ||
                (baseline_len != 0u &&
                 (candidate_len != baseline_len ||
                  memcmp(baseline, candidate, baseline_len + 1u) != 0)))
                return 0;
        }
    }
    return 1;
}

static int verify_equivalence(void) {
    static const int64_t timestamp_cases[] = {
        INT64_C(1),
        INT64_C(9),
        INT64_C(10),
        INT64_C(99),
        INT64_C(100),
        INT64_C(999999999999),
        INT64_C(1000000000000),
        INT64_C(1000000000001),
        INT64_C(1787774400000),
        INT64_C(9999999999999),
        INT64_C(10000000000000),
        INT64_MAX,
    };
    char baseline[BENCH_OUTPUT_CAP];
    char candidate[BENCH_OUTPUT_CAP];
    size_t timestamp_case;
    for (timestamp_case = 0;
         timestamp_case < sizeof(timestamp_cases) / sizeof(timestamp_cases[0]);
         ++timestamp_case) {
        size_t baseline_len = reference_started(
            baseline, sizeof(baseline), timestamp_cases[timestamp_case]);
        size_t candidate_len = product_started(
            candidate, sizeof(candidate), timestamp_cases[timestamp_case]);
        size_t full_len = baseline_len;
        size_t out_cap;
        if (baseline_len == 0u || candidate_len != baseline_len ||
            memcmp(baseline, candidate, baseline_len + 1u) != 0)
            return 0;
        for (out_cap = 1u; out_cap <= full_len + 1u; ++out_cap) {
            baseline_len = reference_started(
                baseline, out_cap, timestamp_cases[timestamp_case]);
            candidate_len = product_started(
                candidate, out_cap, timestamp_cases[timestamp_case]);
            if ((baseline_len == 0u) != (candidate_len == 0u) ||
                (baseline_len != 0u &&
                 (candidate_len != baseline_len ||
                  memcmp(baseline, candidate, baseline_len + 1u) != 0)))
                return 0;
        }
    }
    {
        size_t baseline_len = product_text_fallback(
            baseline, sizeof(baseline), INT64_C(1787774400000));
        size_t candidate_len = product_text_cached(
            candidate, sizeof(candidate), INT64_C(1787774400000));
        size_t out_cap;
        if (baseline_len == 0u || candidate_len != baseline_len ||
            memcmp(baseline, candidate, baseline_len + 1u) != 0)
            return 0;
        for (out_cap = 1u; out_cap <= baseline_len + 1u; ++out_cap) {
            baseline_len = product_text_fallback(
                baseline, out_cap, INT64_C(1787774400000));
            candidate_len = product_text_cached(
                candidate, out_cap, INT64_C(1787774400000));
            if ((baseline_len == 0u) != (candidate_len == 0u) ||
                (baseline_len != 0u &&
                 (candidate_len != baseline_len ||
                  memcmp(baseline, candidate, baseline_len + 1u) != 0)))
                return 0;
        }
    }
    {
        size_t strict_len = product_text_escaped_strict(
            baseline, sizeof(baseline), INT64_C(1787774400000));
        size_t validated_len = product_text_escaped_validated(
            candidate, sizeof(candidate), INT64_C(1787774400000));
        size_t out_cap;
        if (strict_len == 0u || validated_len != strict_len ||
            memcmp(baseline, candidate, strict_len + 1u) != 0)
            return 0;
        for (out_cap = 1u; out_cap <= strict_len + 1u; ++out_cap) {
            strict_len = product_text_escaped_strict(
                baseline, out_cap, INT64_C(1787774400000));
            validated_len = product_text_escaped_validated(
                candidate, out_cap, INT64_C(1787774400000));
            if ((strict_len == 0u) != (validated_len == 0u) ||
                (strict_len != 0u &&
                 (validated_len != strict_len ||
                  memcmp(baseline, candidate, strict_len + 1u) != 0)))
                return 0;
        }
    }
    return product_started(candidate, sizeof(candidate), 0) == 0u &&
        verify_pcm_equivalence();
}

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0u;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
}

static int run(
    encode_fn encode,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    char output[BENCH_OUTPUT_CAP];
    uint64_t started;
    size_t i;
    if (!encode || !elapsed_ns || !checksum || rounds == 0u) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0; i < rounds; ++i) {
        int64_t timestamp_ms = INT64_C(1787774000000) +
            (int64_t)(i % 1000000u);
        size_t length = encode(output, sizeof(output), timestamp_ms);
        if (length == 0u) return 0;
        *checksum = *checksum * UINT64_C(131) +
            (uint8_t)output[length - 3u - (i & 3u)];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int run_pcm(
    pcm_encode_fn encode,
    const pcm_case *values,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    char output[BENCH_OUTPUT_CAP];
    uint64_t started;
    size_t i;
    if (!encode || !values || !elapsed_ns || !checksum || rounds == 0u)
        return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        size_t length = encode(output, sizeof(output), values);
        if (length == 0u) return 0;
        *checksum = *checksum * UINT64_C(131) +
            (uint8_t)output[length - 3u - (i & 3u)];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int run_ring(
    ring_encode_fn encode,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    voice_byte_ring ring;
    turn_response_event event;
    uint64_t started;
    size_t i;
    if (!encode || !elapsed_ns || !checksum || rounds == 0u) return 0;
    memset(&ring, 0, sizeof(ring));
    ring_pcm_event_init(&event);
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        size_t length;
        voice_byte_ring_reset(&ring);
        event.sequence = (int32_t)(i % 100000u);
        length = encode(
            &ring, &event,
            INT64_C(1787774000000) + (int64_t)(i % 100000u));
        if (length == 0u || ring.size != length) return 0;
        *checksum = *checksum * UINT64_C(131) +
            ring.data[length - 3u - (i & 3u)];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int run_filter(
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    static const char request_id[] = "request";
    turn_response_state state = {0};
    turn_event_c event = {0};
    turn_response_event safe;
    uint64_t started;
    size_t i;
    if (!elapsed_ns || !checksum || rounds == 0u) return 0;
    memcpy(event.type, "text_delta", sizeof("text_delta"));
    event.type_id = 4;
    event.display_text_view = BENCH_FILTER_TEXT;
    event.display_text_len = sizeof(BENCH_FILTER_TEXT) - 1u;
    event.display_text_len_known = 1;
    event.display_text_borrowed = 1u;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        if (turn_response_filter_public_active_n(
                &state, &event, request_id, sizeof(request_id) - 1u,
                &safe) != TURN_RESPONSE_FORWARD ||
            safe.text_len != sizeof(BENCH_FILTER_TEXT) - 1u ||
            safe.text_json_raw != 0u || safe.text_validated != 1u)
            return 0;
        *checksum = *checksum * UINT64_C(131) + safe.text_len +
            (uint8_t)safe.display_text[i % safe.text_len];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

int main(int argc, char **argv) {
    static const pcm_case current_pcm = {24000, 1, 16, 1, 7, 2, 0};
    int verify = argc > 1 && strcmp(argv[1], "--verify") == 0;
    size_t rounds = verify ? 200u : 200000u;
    size_t ring_rounds = verify ? 200u : 100000u;
    size_t samples = verify ? 2u : 8u;
    uint64_t baseline_total = 0u;
    uint64_t candidate_total = 0u;
    uint64_t pcm_baseline_total = 0u;
    uint64_t pcm_candidate_total = 0u;
    uint64_t text_fallback_total = 0u;
    uint64_t text_cached_total = 0u;
    uint64_t escaped_strict_total = 0u;
    uint64_t escaped_validated_total = 0u;
    uint64_t ring_staged_total = 0u;
    uint64_t ring_direct_total = 0u;
    uint64_t filter_total = 0u;
    uint64_t expected_checksum = 0u;
    uint64_t expected_pcm_checksum = 0u;
    uint64_t expected_text_checksum = 0u;
    uint64_t expected_escaped_checksum = 0u;
    uint64_t expected_ring_checksum = 0u;
    uint64_t expected_filter_checksum = 0u;
    size_t sample;
    if (!verify_equivalence() || !verify_ring_equivalence()) {
        fprintf(stderr, "FAIL response JSON timestamp equivalence\n");
        return 1;
    }
    for (sample = 0; sample < samples; ++sample) {
        uint64_t baseline_ns;
        uint64_t candidate_ns;
        uint64_t baseline_checksum;
        uint64_t candidate_checksum;
        uint64_t pcm_baseline_ns;
        uint64_t pcm_candidate_ns;
        uint64_t pcm_baseline_checksum;
        uint64_t pcm_candidate_checksum;
        uint64_t text_fallback_ns;
        uint64_t text_cached_ns;
        uint64_t text_fallback_checksum;
        uint64_t text_cached_checksum;
        uint64_t escaped_strict_ns;
        uint64_t escaped_validated_ns;
        uint64_t escaped_strict_checksum;
        uint64_t escaped_validated_checksum;
        uint64_t ring_staged_ns;
        uint64_t ring_direct_ns;
        uint64_t ring_staged_checksum;
        uint64_t ring_direct_checksum;
        uint64_t filter_ns;
        uint64_t filter_checksum;
        if (!run(
                reference_started,
                rounds,
                &baseline_ns,
                &baseline_checksum) ||
            !run(
                product_started,
                rounds,
                &candidate_ns,
                &candidate_checksum) ||
            baseline_checksum != candidate_checksum ||
            (sample != 0u && baseline_checksum != expected_checksum)) {
            fprintf(stderr, "FAIL response JSON timestamp benchmark\n");
            return 1;
        }
        if (!run_pcm(
                reference_pcm,
                &current_pcm,
                rounds,
                &pcm_baseline_ns,
                &pcm_baseline_checksum) ||
            !run_pcm(
                product_pcm,
                &current_pcm,
                rounds,
                &pcm_candidate_ns,
                &pcm_candidate_checksum) ||
            pcm_baseline_checksum != pcm_candidate_checksum ||
            (sample != 0u &&
             pcm_baseline_checksum != expected_pcm_checksum)) {
            fprintf(stderr, "FAIL response JSON PCM benchmark\n");
            return 1;
        }
        if (!run(
                product_text_fallback,
                rounds,
                &text_fallback_ns,
                &text_fallback_checksum) ||
            !run(
                product_text_cached,
                rounds,
                &text_cached_ns,
                &text_cached_checksum) ||
            text_fallback_checksum != text_cached_checksum ||
            (sample != 0u &&
             text_fallback_checksum != expected_text_checksum)) {
            fprintf(stderr, "FAIL response JSON text-length benchmark\n");
            return 1;
        }
        if ((sample & 1u) == 0u) {
            if (!run(
                    product_text_escaped_strict,
                    rounds,
                    &escaped_strict_ns,
                    &escaped_strict_checksum) ||
                !run(
                    product_text_escaped_validated,
                    rounds,
                    &escaped_validated_ns,
                    &escaped_validated_checksum)) {
                fprintf(stderr, "FAIL response JSON validation benchmark\n");
                return 1;
            }
        } else if (!run(
                       product_text_escaped_validated,
                       rounds,
                       &escaped_validated_ns,
                       &escaped_validated_checksum) ||
                   !run(
                       product_text_escaped_strict,
                       rounds,
                       &escaped_strict_ns,
                       &escaped_strict_checksum)) {
            fprintf(stderr, "FAIL response JSON validation benchmark\n");
            return 1;
        }
        if (escaped_strict_checksum != escaped_validated_checksum ||
            (sample != 0u &&
             escaped_strict_checksum != expected_escaped_checksum)) {
            fprintf(stderr, "FAIL response JSON validation checksum\n");
            return 1;
        }
        if ((sample & 1u) == 0u) {
            if (!run_ring(
                    ring_encode_staged,
                    ring_rounds,
                    &ring_staged_ns,
                    &ring_staged_checksum) ||
                !run_ring(
                    ring_encode_direct,
                    ring_rounds,
                    &ring_direct_ns,
                    &ring_direct_checksum)) {
                fprintf(stderr, "FAIL response JSON ring benchmark\n");
                return 1;
            }
        } else if (!run_ring(
                       ring_encode_direct,
                       ring_rounds,
                       &ring_direct_ns,
                       &ring_direct_checksum) ||
                   !run_ring(
                       ring_encode_staged,
                       ring_rounds,
                       &ring_staged_ns,
                       &ring_staged_checksum)) {
            fprintf(stderr, "FAIL response JSON ring benchmark\n");
            return 1;
        }
        if (ring_staged_checksum != ring_direct_checksum ||
            (sample != 0u &&
             ring_staged_checksum != expected_ring_checksum)) {
            fprintf(stderr, "FAIL response JSON ring checksum\n");
            return 1;
        }
        if (!run_filter(
                rounds, &filter_ns, &filter_checksum) ||
            (sample != 0u && filter_checksum != expected_filter_checksum)) {
            fprintf(stderr, "FAIL response filter text benchmark\n");
            return 1;
        }
        expected_checksum = baseline_checksum;
        expected_pcm_checksum = pcm_baseline_checksum;
        expected_text_checksum = text_fallback_checksum;
        expected_escaped_checksum = escaped_strict_checksum;
        expected_ring_checksum = ring_staged_checksum;
        expected_filter_checksum = filter_checksum;
        baseline_total += baseline_ns;
        candidate_total += candidate_ns;
        pcm_baseline_total += pcm_baseline_ns;
        pcm_candidate_total += pcm_candidate_ns;
        text_fallback_total += text_fallback_ns;
        text_cached_total += text_cached_ns;
        escaped_strict_total += escaped_strict_ns;
        escaped_validated_total += escaped_validated_ns;
        ring_staged_total += ring_staged_ns;
        ring_direct_total += ring_direct_ns;
        filter_total += filter_ns;
    }
    printf(
        "BenchmarkResponseJSON shape=current-ms rounds=%zu samples=%zu "
        "snprintf_ns_per_call=%llu bounded_ns_per_call=%llu "
        "speedup_x100=%llu checksum=%llu\n",
        rounds,
        samples,
        (unsigned long long)(baseline_total / samples / rounds),
        (unsigned long long)(candidate_total / samples / rounds),
        (unsigned long long)(candidate_total != 0u ?
            baseline_total * UINT64_C(100) / candidate_total : 0u),
        (unsigned long long)expected_checksum);
    printf(
        "BenchmarkResponseJSON shape=canonical-pcm-24k rounds=%zu samples=%zu "
        "snprintf_ns_per_call=%llu bounded_ns_per_call=%llu "
        "speedup_x100=%llu checksum=%llu\n",
        rounds,
        samples,
        (unsigned long long)(pcm_baseline_total / samples / rounds),
        (unsigned long long)(pcm_candidate_total / samples / rounds),
        (unsigned long long)(pcm_candidate_total != 0u ?
            pcm_baseline_total * UINT64_C(100) / pcm_candidate_total : 0u),
        (unsigned long long)expected_pcm_checksum);
    printf(
        "BenchmarkResponseJSON shape=validated-text rounds=%zu samples=%zu "
        "fallback_ns_per_call=%llu cached_ns_per_call=%llu "
        "speedup_x100=%llu checksum=%llu\n",
        rounds,
        samples,
        (unsigned long long)(text_fallback_total / samples / rounds),
        (unsigned long long)(text_cached_total / samples / rounds),
        (unsigned long long)(text_cached_total != 0u ?
            text_fallback_total * UINT64_C(100) / text_cached_total : 0u),
        (unsigned long long)expected_text_checksum);
    printf(
        "BenchmarkResponseJSON shape=validated-escaped-text rounds=%zu samples=%zu "
        "strict_ns_per_call=%llu validated_ns_per_call=%llu "
        "speedup_x100=%llu checksum=%llu\n",
        rounds,
        samples,
        (unsigned long long)(escaped_strict_total / samples / rounds),
        (unsigned long long)(escaped_validated_total / samples / rounds),
        (unsigned long long)(escaped_validated_total != 0u ?
            escaped_strict_total * UINT64_C(100) / escaped_validated_total : 0u),
        (unsigned long long)expected_escaped_checksum);
    printf(
        "BenchmarkResponseJSON shape=direct-ring-pcm rounds=%zu samples=%zu "
        "staged_ns_per_call=%llu direct_ns_per_call=%llu "
        "speedup_x100=%llu checksum=%llu\n",
        ring_rounds,
        samples,
        (unsigned long long)(ring_staged_total / samples / ring_rounds),
        (unsigned long long)(ring_direct_total / samples / ring_rounds),
        (unsigned long long)(ring_direct_total != 0u ?
            ring_staged_total * UINT64_C(100) / ring_direct_total : 0u),
        (unsigned long long)expected_ring_checksum);
    printf(
        "BenchmarkResponseFilter shape=quoted-ascii bytes=%zu rounds=%zu "
        "samples=%zu ns_per_call=%llu checksum=%llu\n",
        sizeof(BENCH_FILTER_TEXT) - 1u,
        rounds,
        samples,
        (unsigned long long)(filter_total / samples / rounds),
        (unsigned long long)expected_filter_checksum);
    return 0;
}
