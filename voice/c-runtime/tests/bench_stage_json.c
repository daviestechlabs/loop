/* Compare snprintf-per-field stage JSON with the bounded integer writer. */
#define _POSIX_C_SOURCE 200809L

#include "stage_json.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BENCH_OUTPUT_CAP 1024u

enum {
    BENCH_SHAPE_FULL = 0,
    BENCH_SHAPE_SPARSE,
    BENCH_SHAPE_TTS_STARTED,
    BENCH_SHAPE_TTS_FIRST_PCM,
    CURRENT_TTS_SUFFIX_CASES = 9,
    VERIFY_SHAPE_COUNT = 7 + CURRENT_TTS_SUFFIX_CASES,
};

typedef int (*serialize_fn)(
    const turn_stage_timestamps_c *,
    char *,
    size_t
);

static int snprintf_stage_metadata(
    const turn_stage_timestamps_c *stages,
    char *out,
    size_t out_cap
) {
    int written;
    size_t used;
    int fields = 0;
    if (!stages || !out || out_cap == 0) return -1;
    if (stages->tts_request_received_at_ms == 0) {
        out[0] = '\0';
        return 0;
    }
    written = snprintf(out, out_cap, ",\"metadata\":{");
    if (written <= 0 || (size_t)written >= out_cap) return -1;
    used = (size_t)written;
#define APPEND_STAGE(member, key) do { \
        if (stages->member > 0) { \
            written = snprintf( \
                out + used, out_cap - used, "%s\"%s\":\"%lld\"", \
                fields ? "," : "", (key), (long long)stages->member); \
            if (written <= 0 || (size_t)written >= out_cap - used) return -1; \
            used += (size_t)written; \
            fields++; \
        } \
    } while (0)
    APPEND_STAGE(input.audio_committed_at_ms, "stage_audio_committed_at_ms");
    APPEND_STAGE(input.stt_request_received_at_ms, "stage_stt_request_received_at_ms");
    APPEND_STAGE(
        input.stt_provider_request_started_at_ms,
        "stage_stt_provider_request_started_at_ms");
    APPEND_STAGE(input.stt_provider_ready_at_ms, "stage_stt_provider_ready_at_ms");
    APPEND_STAGE(
        input.stt_transcript_published_at_ms,
        "stage_stt_transcript_published_at_ms");
    APPEND_STAGE(first_text_at_ms, "stage_first_text_at_ms");
    APPEND_STAGE(tts_segment_emitted_at_ms, "stage_tts_segment_emitted_at_ms");
    APPEND_STAGE(tts_request_received_at_ms, "stage_tts_request_received_at_ms");
    APPEND_STAGE(
        tts_provider_request_started_at_ms,
        "stage_tts_provider_request_started_at_ms");
    APPEND_STAGE(tts_provider_ready_at_ms, "stage_tts_provider_ready_at_ms");
    APPEND_STAGE(pcm_started_at_ms, "stage_pcm_started_at_ms");
    APPEND_STAGE(pcm_first_chunk_at_ms, "stage_pcm_first_chunk_at_ms");
#undef APPEND_STAGE
    if (used + 2u > out_cap) return -1;
    out[used++] = '}';
    out[used] = '\0';
    return fields > 0 ? 0 : -1;
}

static void fill_stages(turn_stage_timestamps_c *stages, int64_t first) {
    memset(stages, 0, sizeof(*stages));
    stages->input.audio_committed_at_ms = first;
    stages->input.stt_request_received_at_ms = first + 1;
    stages->input.stt_provider_request_started_at_ms = first + 2;
    stages->input.stt_provider_ready_at_ms = first + 3;
    stages->input.stt_transcript_published_at_ms = first + 4;
    stages->first_text_at_ms = first + 5;
    stages->tts_segment_emitted_at_ms = first + 6;
    stages->tts_request_received_at_ms = first + 7;
    stages->tts_provider_request_started_at_ms = first + 8;
    stages->tts_provider_ready_at_ms = first + 9;
    stages->pcm_started_at_ms = first + 10;
    stages->pcm_first_chunk_at_ms = first + 11;
}

static void fill_sparse_stages(
    turn_stage_timestamps_c *stages,
    int64_t first
) {
    memset(stages, 0, sizeof(*stages));
    stages->tts_request_received_at_ms = first;
    stages->pcm_started_at_ms = first + 1;
}

static void fill_tts_stages(
    turn_stage_timestamps_c *stages,
    int64_t first,
    int first_pcm
) {
    memset(stages, 0, sizeof(*stages));
    stages->first_text_at_ms = first;
    stages->tts_segment_emitted_at_ms = first + 1;
    stages->tts_request_received_at_ms = first + 2;
    stages->tts_provider_request_started_at_ms = first + 3;
    stages->tts_provider_ready_at_ms = first + 4;
    stages->pcm_started_at_ms = first + 5;
    if (first_pcm) stages->pcm_first_chunk_at_ms = first + 6;
}

static void fill_equal_tts_stages(
    turn_stage_timestamps_c *stages,
    int64_t value
) {
    memset(stages, 0, sizeof(*stages));
    stages->first_text_at_ms = value;
    stages->tts_segment_emitted_at_ms = value;
    stages->tts_request_received_at_ms = value;
    stages->tts_provider_request_started_at_ms = value;
    stages->tts_provider_ready_at_ms = value;
    stages->pcm_started_at_ms = value;
    stages->pcm_first_chunk_at_ms = value;
}

static int verify_equivalence(void) {
    static const uint32_t current_tts_suffixes[CURRENT_TTS_SUFFIX_CASES] = {
        UINT32_C(0),
        UINT32_C(1),
        UINT32_C(9),
        UINT32_C(10),
        UINT32_C(99),
        UINT32_C(100),
        UINT32_C(9999),
        UINT32_C(10000),
        UINT32_C(999999),
    };
    turn_stage_timestamps_c shapes[VERIFY_SHAPE_COUNT];
    char baseline[BENCH_OUTPUT_CAP];
    char candidate[BENCH_OUTPUT_CAP];
    size_t shape;
    fill_stages(&shapes[0], INT64_C(1720000000000));
    fill_stages(&shapes[1], INT64_MAX - INT64_C(12));
    fill_stages(&shapes[2], INT64_C(999995));
    memset(&shapes[3], 0, sizeof(shapes[3]));
    shapes[3].tts_request_received_at_ms = 8;
    shapes[3].pcm_started_at_ms = 11;
    fill_tts_stages(&shapes[4], INT64_C(1720000000000), 0);
    fill_tts_stages(&shapes[5], INT64_C(1720000000000), 1);
    fill_tts_stages(&shapes[6], INT64_C(1720000000000), 0);
    shapes[6].first_text_at_ms = INT64_C(1719999999999);
    for (shape = 0; shape < CURRENT_TTS_SUFFIX_CASES; ++shape)
        fill_equal_tts_stages(
            &shapes[7u + shape],
            INT64_C(1720000000000) +
                (int64_t)current_tts_suffixes[shape]);
    for (shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); ++shape) {
        size_t full_len;
        size_t cap;
        if (snprintf_stage_metadata(
                &shapes[shape], baseline, sizeof(baseline)) != 0 ||
            turn_stage_metadata_json_v1(
                &shapes[shape], candidate, sizeof(candidate)) != 0 ||
            strcmp(baseline, candidate) != 0) return 0;
        full_len = strlen(baseline);
        for (cap = 1; cap <= full_len + 1u; ++cap) {
            int baseline_result = snprintf_stage_metadata(
                &shapes[shape], baseline, cap);
            int candidate_result = turn_stage_metadata_json_v1(
                &shapes[shape], candidate, cap);
            if ((baseline_result == 0) != (candidate_result == 0) ||
                (baseline_result == 0 && strcmp(baseline, candidate) != 0)) return 0;
        }
    }
    memset(&shapes[0], 0, sizeof(shapes[0]));
    baseline[0] = 'x';
    candidate[0] = 'x';
    return snprintf_stage_metadata(
               &shapes[0], baseline, sizeof(baseline)) == 0 &&
        turn_stage_metadata_json_v1(
            &shapes[0], candidate, sizeof(candidate)) == 0 &&
        baseline[0] == '\0' && candidate[0] == '\0';
}

static int verify_current_wire_equivalence(void) {
    turn_stage_timestamps_c stages;
    turn_stage_wire_c prepared;
    char baseline[BENCH_OUTPUT_CAP];
    char candidate[BENCH_OUTPUT_CAP];
    int first_pcm;
    for (first_pcm = 0; first_pcm <= 1; ++first_pcm) {
        size_t baseline_len;
        size_t candidate_len;
        size_t cap;
        fill_tts_stages(&stages, INT64_C(1720000000000), 0);
        if (pb_prepare_turn_stage_wire(&prepared, &stages) != 0)
            return 0;
        if (first_pcm) {
            if (pb_complete_turn_stage_wire(
                    &prepared, INT64_C(1720000000006)) != 0)
                return 0;
            stages.pcm_first_chunk_at_ms = INT64_C(1720000000006);
        }
        baseline_len = turn_stage_metadata_json_write_v1(
            &stages, baseline, sizeof(baseline));
        candidate_len = turn_stage_metadata_json_write_current_wire_v1(
            &stages, prepared.data, candidate, sizeof(candidate));
        if (baseline_len == SIZE_MAX || candidate_len != baseline_len ||
            memcmp(baseline, candidate, baseline_len + 1u) != 0)
            return 0;
        for (cap = 1u; cap <= baseline_len + 1u; ++cap) {
            size_t baseline_result = turn_stage_metadata_json_write_v1(
                &stages, baseline, cap);
            size_t candidate_result =
                turn_stage_metadata_json_write_current_wire_v1(
                    &stages, prepared.data, candidate, cap);
            if (candidate_result != baseline_result ||
                (baseline_result != SIZE_MAX &&
                 memcmp(baseline, candidate, baseline_result + 1u) != 0) ||
                (baseline_result == SIZE_MAX && candidate[0] != '\0'))
                return 0;
        }
    }
    fill_tts_stages(&stages, INT64_C(1720000000000), 0);
    candidate[0] = 'x';
    return pb_prepare_turn_stage_wire(&prepared, &stages) == 0 &&
        turn_stage_metadata_json_write_current_wire_v1(
            NULL, prepared.data, candidate, sizeof(candidate)) == SIZE_MAX &&
        turn_stage_metadata_json_write_current_wire_v1(
            &stages, NULL, candidate, sizeof(candidate)) == SIZE_MAX &&
        turn_stage_metadata_json_write_current_wire_v1(
            &stages, prepared.data, NULL, sizeof(candidate)) == SIZE_MAX &&
        turn_stage_metadata_json_write_current_wire_v1(
            &stages, prepared.data, candidate, 0u) == SIZE_MAX;
}

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
}

static int run(
    serialize_fn serialize,
    int shape,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_stage_timestamps_c stages;
    char output[BENCH_OUTPUT_CAP];
    uint64_t started;
    size_t checksum_pos;
    size_t i;
    if (!serialize || !elapsed_ns || !checksum || rounds == 0) return 0;
    switch (shape) {
    case BENCH_SHAPE_SPARSE:
        fill_sparse_stages(&stages, INT64_C(1720000000000));
        break;
    case BENCH_SHAPE_TTS_STARTED:
        fill_tts_stages(&stages, INT64_C(1720000000000), 0);
        break;
    case BENCH_SHAPE_TTS_FIRST_PCM:
        fill_tts_stages(&stages, INT64_C(1720000000000), 1);
        break;
    default:
        fill_stages(&stages, INT64_C(1720000000000));
        break;
    }
    if (serialize(&stages, output, sizeof(output)) != 0) return 0;
    checksum_pos = strlen(output) - 3u;
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < rounds; ++i) {
        if (shape == BENCH_SHAPE_SPARSE) {
            stages.pcm_started_at_ms =
                INT64_C(1720000000001) + (int64_t)i;
        } else if (shape == BENCH_SHAPE_TTS_STARTED) {
            stages.pcm_started_at_ms =
                INT64_C(1720000000005) + (int64_t)i;
        } else if (shape == BENCH_SHAPE_TTS_FIRST_PCM) {
            stages.pcm_first_chunk_at_ms =
                INT64_C(1720000000006) + (int64_t)i;
        } else {
            stages.pcm_first_chunk_at_ms =
                INT64_C(1720000000011) + (int64_t)i;
        }
        if (serialize(&stages, output, sizeof(output)) != 0) return 0;
        *checksum = *checksum * UINT64_C(131) +
            (uint8_t)output[checksum_pos - (i & 3u)];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0;
}

static int benchmark_shape(
    const char *shape,
    size_t fields,
    int shape_id,
    size_t rounds,
    size_t samples
) {
    uint64_t baseline_total = 0;
    uint64_t candidate_total = 0;
    size_t sample;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t baseline_ns;
        uint64_t candidate_ns;
        uint64_t baseline_checksum;
        uint64_t candidate_checksum;
        int ok;
        if ((sample & 1u) == 0u) {
            ok = run(
                    snprintf_stage_metadata, shape_id, rounds,
                    &baseline_ns, &baseline_checksum) &&
                run(
                    turn_stage_metadata_json_v1, shape_id, rounds,
                    &candidate_ns, &candidate_checksum);
        } else {
            ok = run(
                    turn_stage_metadata_json_v1, shape_id, rounds,
                    &candidate_ns, &candidate_checksum) &&
                run(
                    snprintf_stage_metadata, shape_id, rounds,
                    &baseline_ns, &baseline_checksum);
        }
        if (!ok || baseline_checksum != candidate_checksum) {
            fprintf(stderr, "FAIL stage JSON benchmark shape=%s\n", shape);
            return 0;
        }
        baseline_total += baseline_ns;
        candidate_total += candidate_ns;
    }
    {
        uint64_t calls = (uint64_t)rounds * (uint64_t)samples;
        printf(
            "BenchmarkStageJSON fields=%zu shape=%s rounds=%zu samples=%zu "
            "snprintf_ns_per_call=%" PRIu64 " bounded_ns_per_call=%" PRIu64
            " speedup_x100=%" PRIu64 "\n",
            fields,
            shape,
            rounds,
            samples,
            baseline_total / calls,
            candidate_total / calls,
            candidate_total == 0 ? 0 : baseline_total * UINT64_C(100) /
                candidate_total);
    }
    return 1;
}

static int run_current_wire(
    int borrowed,
    int first_pcm,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_stage_timestamps_c stages;
    turn_stage_wire_c prepared;
    char output[BENCH_OUTPUT_CAP];
    uint64_t started;
    size_t checksum_pos;
    size_t i;
    size_t length;
    if (!elapsed_ns || !checksum || rounds == 0u) return 0;
    fill_tts_stages(&stages, INT64_C(1720000000000), 0);
    if (pb_prepare_turn_stage_wire(&prepared, &stages) != 0) return 0;
    if (first_pcm) {
        if (pb_complete_turn_stage_wire(
                &prepared, INT64_C(1720000000006)) != 0)
            return 0;
        stages.pcm_first_chunk_at_ms = INT64_C(1720000000006);
    }
    length = borrowed ?
        turn_stage_metadata_json_write_current_wire_v1(
            &stages, prepared.data, output, sizeof(output)) :
        turn_stage_metadata_json_write_v1(
            &stages, output, sizeof(output));
    if (length == SIZE_MAX || length < 4u) return 0;
    checksum_pos = length - 3u;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        length = borrowed ?
            turn_stage_metadata_json_write_current_wire_v1(
                &stages, prepared.data, output, sizeof(output)) :
            turn_stage_metadata_json_write_v1(
                &stages, output, sizeof(output));
        if (length == SIZE_MAX) return 0;
        *checksum = *checksum * UINT64_C(131) +
            (uint8_t)output[checksum_pos - (i & 3u)];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_current_wire(
    const char *shape,
    size_t fields,
    int first_pcm,
    size_t rounds,
    size_t samples
) {
    uint64_t bounded_total = 0u;
    uint64_t borrowed_total = 0u;
    size_t sample;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t bounded_ns;
        uint64_t borrowed_ns;
        uint64_t bounded_checksum;
        uint64_t borrowed_checksum;
        int ok;
        if ((sample & 1u) == 0u) {
            ok = run_current_wire(
                    0, first_pcm, rounds, &bounded_ns, &bounded_checksum) &&
                run_current_wire(
                    1, first_pcm, rounds, &borrowed_ns, &borrowed_checksum);
        } else {
            ok = run_current_wire(
                    1, first_pcm, rounds, &borrowed_ns, &borrowed_checksum) &&
                run_current_wire(
                    0, first_pcm, rounds, &bounded_ns, &bounded_checksum);
        }
        if (!ok || bounded_checksum != borrowed_checksum) {
            fprintf(stderr, "FAIL current wire benchmark shape=%s\n", shape);
            return 0;
        }
        bounded_total += bounded_ns;
        borrowed_total += borrowed_ns;
    }
    {
        uint64_t calls = (uint64_t)rounds * (uint64_t)samples;
        printf(
            "BenchmarkStageJSONWire fields=%zu shape=%s rounds=%zu "
            "samples=%zu bounded_ns_per_call=%" PRIu64
            " borrowed_ns_per_call=%" PRIu64 " speedup_x100=%" PRIu64
            "\n",
            fields,
            shape,
            rounds,
            samples,
            bounded_total / calls,
            borrowed_total / calls,
            borrowed_total == 0u ? 0u :
                bounded_total * UINT64_C(100) / borrowed_total);
    }
    return 1;
}

int main(int argc, char **argv) {
    int verify = argc > 1 && strcmp(argv[1], "--verify") == 0;
    size_t rounds = verify ? 200u : 200000u;
    size_t samples = verify ? 2u : 8u;
    if (!verify_equivalence() || !verify_current_wire_equivalence()) {
        fprintf(stderr, "FAIL stage JSON equivalence\n");
        return 1;
    }
    return benchmark_shape(
            "full", 12u, BENCH_SHAPE_FULL, rounds, samples) &&
        benchmark_shape(
            "sparse", 2u, BENCH_SHAPE_SPARSE, rounds, samples) &&
        benchmark_shape(
            "tts-started", 6u, BENCH_SHAPE_TTS_STARTED, rounds, samples) &&
        benchmark_shape(
            "tts-first-pcm", 7u, BENCH_SHAPE_TTS_FIRST_PCM, rounds, samples) &&
        benchmark_current_wire(
            "tts-started", 6u, 0, rounds, samples) &&
        benchmark_current_wire(
            "tts-first-pcm", 7u, 1, rounds, samples) ?
        0 : 1;
}
