#define _POSIX_C_SOURCE 200809L

#include "reflex_session.h"
#include "../../audio-processor/dsp/audio_engine.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_SAMPLES 320
#define FRAME_BYTES (FRAME_SAMPLES * sizeof(int16_t))
#define FRAME_NS 20000000u
#define BENCH_SAMPLES 20000

typedef struct {
    size_t input_bytes;
    size_t total_frames;
    size_t processed_frames;
    size_t legacy_audio_bytes;
    size_t embedded_audio_bytes;
    size_t legacy_speech_frames;
    size_t embedded_speech_frames;
    int speech_mismatches;
    int energy_mismatches;
    int legacy_endpoint_frame;
    int embedded_endpoint_frame;
    int legacy_interrupt_frame;
    int embedded_interrupt_frame;
    int endpoint_reason;
    int cancel_frame;
    int cancel_applied;
    int endpoint_match;
    int interrupt_match;
    int conditioned_pcm_match;
    int cancel_match;
    int overall_match;
    uint64_t process_ns_p50;
    uint64_t process_ns_p95;
    uint64_t process_ns_p99;
} replay_report;

static uint64_t monotonic_ns(void) {
    struct timespec ts = {0};
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *left, const void *right) {
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static void fill_voice(int16_t *frame, unsigned frame_index) {
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        const double sample_index = (double)(frame_index * FRAME_SAMPLES + (unsigned)i);
        frame[i] = (int16_t)(6000.0 * sin(2.0 * pi * 440.0 * sample_index / 16000.0));
    }
}

static audio_engine_session *new_legacy_session(void) {
    return audio_engine_create(0.01f,
                               0.985,
                               1,
                               1,
                               0.0025,
                               0.2,
                               1.75,
                               0.12);
}

static int run_replay(const uint8_t *pcm,
                      size_t pcm_len,
                      int assistant_speaking,
                      long cancel_frame,
                      replay_report *report) {
    if (pcm == NULL || pcm_len == 0u || pcm_len % FRAME_BYTES != 0u ||
        report == NULL || (assistant_speaking != 0 && assistant_speaking != 1)) {
        return -1;
    }

    reflex_config_v1 config;
    reflex_config_default_v1(&config);
    if (pcm_len > config.max_utterance_bytes) {
        return -1;
    }

    const size_t total_frames = pcm_len / FRAME_BYTES;
    if (cancel_frame < -1 || (cancel_frame >= 0 && (size_t)cancel_frame > total_frames)) {
        return -1;
    }

    memset(report, 0, sizeof(*report));
    report->input_bytes = pcm_len;
    report->total_frames = total_frames;
    report->legacy_endpoint_frame = -1;
    report->embedded_endpoint_frame = -1;
    report->legacy_interrupt_frame = -1;
    report->embedded_interrupt_frame = -1;
    report->cancel_frame = (int)cancel_frame;
    report->endpoint_reason = REFLEX_ENDPOINT_NONE;

    audio_engine_session *legacy = new_legacy_session();
    reflex_session_v1 *embedded = NULL;
    uint8_t *legacy_audio = NULL;
    uint8_t *embedded_audio = NULL;
    size_t legacy_audio_len = 0u;
    size_t embedded_audio_len = 0u;
    uint64_t process_timings[1500] = {0};
    size_t process_timing_count = 0u;
    int status = -1;
    if (legacy == NULL || reflex_session_init_v1(&config, &embedded) != REFLEX_OK) {
        goto cleanup;
    }
    const audio_process_options legacy_process_options = {
        .interrupt_threshold = config.interrupt_energy_threshold,
        .interrupt_duration_s =
            (float)((double)config.interrupt_hold_ns / 1000000000.0),
        .flags = assistant_speaking != 0
            ? AUDIO_PROCESS_INTERRUPT_ENABLED
            : 0u,
    };

    uint64_t now_ns = 1000000000u;
    reflex_decision_v1 embedded_decision = {0};
    for (size_t frame_index = 0; frame_index < total_frames; frame_index++) {
        if (cancel_frame >= 0 && frame_index == (size_t)cancel_frame) {
            (void)audio_engine_finish_processing(legacy, 1);
            reflex_session_cancel_v1(embedded, &embedded_decision);
            report->cancel_applied =
                (embedded_decision.flags & REFLEX_DECISION_CANCELLED) != 0u;
            break;
        }

        const uint8_t *frame = pcm + frame_index * FRAME_BYTES;
        audio_decision legacy_decision = {0};
        audio_engine_process(
            legacy,
            frame,
            FRAME_BYTES,
            now_ns,
            legacy_process_options,
            &legacy_decision);
        audio_monitor_tick legacy_tick = {0};
        if (audio_engine_monitor_tick(
            legacy,
            config.max_utterance_bytes,
            config.max_utterance_bytes,
            3600.0f,
            now_ns,
            0u,
            1,
            config.min_speech_ns,
            config.endpoint_silence_ns,
            config.max_utterance_ns,
            1,
            0,
            FRAME_NS,
            FRAME_NS,
            &legacy_tick) == 0) {
            goto cleanup;
        }
        const int legacy_endpoint = legacy_tick.reason == 5;

        const uint64_t process_started_ns = monotonic_ns();
        const int process_status = reflex_session_process_v1(
            embedded,
            frame,
            FRAME_BYTES,
            now_ns,
            assistant_speaking ? REFLEX_INPUT_ASSISTANT_SPEAKING : 0u,
            &embedded_decision);
        const uint64_t process_elapsed_ns =
            monotonic_ns() - process_started_ns;
        if (process_status != REFLEX_OK) {
            goto cleanup;
        }
        process_timings[process_timing_count++] = process_elapsed_ns;

        const int embedded_speech =
            (embedded_decision.flags & REFLEX_DECISION_SPEECH) != 0u;
        const int legacy_speech =
            (legacy_decision.flags & AUDIO_DECISION_SPEECH) != 0u;
        report->legacy_speech_frames += legacy_speech;
        report->embedded_speech_frames += embedded_speech != 0;
        if (embedded_speech != legacy_speech) {
            report->speech_mismatches++;
        }
        if (fabsf(embedded_decision.energy - legacy_decision.energy) >= 0.02f) {
            report->energy_mismatches++;
        }

        if (legacy_endpoint != 0 && report->legacy_endpoint_frame < 0) {
            report->legacy_endpoint_frame = (int)frame_index;
        }
        if ((embedded_decision.flags & REFLEX_DECISION_ENDPOINT) != 0u &&
            report->embedded_endpoint_frame < 0) {
            report->embedded_endpoint_frame = (int)frame_index;
            report->endpoint_reason = (int)embedded_decision.endpoint_reason;
        }

        if (assistant_speaking != 0 &&
            report->legacy_interrupt_frame < 0 &&
            (legacy_decision.flags & AUDIO_DECISION_INTERRUPT) != 0u) {
            report->legacy_interrupt_frame = (int)frame_index;
        }
        if ((embedded_decision.flags & REFLEX_DECISION_INTERRUPT) != 0u &&
            report->embedded_interrupt_frame < 0) {
            report->embedded_interrupt_frame = (int)frame_index;
        }

        report->processed_frames++;
        now_ns += FRAME_NS;
        if (report->legacy_endpoint_frame >= 0 ||
            report->embedded_endpoint_frame >= 0) {
            break;
        }
    }

    if (cancel_frame >= 0 &&
        (size_t)cancel_frame == total_frames &&
        report->processed_frames == total_frames) {
        (void)audio_engine_finish_processing(legacy, 1);
        reflex_session_cancel_v1(embedded, &embedded_decision);
        report->cancel_applied =
            (embedded_decision.flags & REFLEX_DECISION_CANCELLED) != 0u;
    }

    reflex_decision_v1 snapshot_decision = {0};
    int snapshot_status = reflex_session_snapshot_v1(
        embedded, NULL, 0u, &embedded_audio_len, &snapshot_decision);
    if (embedded_audio_len != 0u && snapshot_status != REFLEX_ERR_CAPACITY) {
        goto cleanup;
    }
    if (embedded_audio_len == 0u && snapshot_status != REFLEX_OK) {
        goto cleanup;
    }
    if (embedded_audio_len != 0u) {
        embedded_audio = malloc(embedded_audio_len);
        if (embedded_audio == NULL ||
            reflex_session_snapshot_v1(embedded,
                                       embedded_audio,
                                       embedded_audio_len,
                                       &embedded_audio_len,
                                       &snapshot_decision) != REFLEX_OK) {
            goto cleanup;
        }
    }

    audio_finalized_utterance finalized = audio_engine_finalize(legacy);
    legacy_audio = finalized.buffer;
    legacy_audio_len = finalized.length;
    report->legacy_audio_bytes = legacy_audio_len;
    report->embedded_audio_bytes = embedded_audio_len;
    report->endpoint_match =
        report->legacy_endpoint_frame == report->embedded_endpoint_frame;
    report->interrupt_match =
        report->legacy_interrupt_frame == report->embedded_interrupt_frame;
    report->conditioned_pcm_match =
        legacy_audio_len == embedded_audio_len &&
        (legacy_audio_len == 0u ||
         memcmp(legacy_audio, embedded_audio, legacy_audio_len) == 0);
    report->cancel_match =
        cancel_frame < 0 ||
        (report->cancel_applied != 0 &&
         legacy_audio_len == 0u &&
         embedded_audio_len == 0u);
    report->overall_match =
        report->speech_mismatches == 0 &&
        report->energy_mismatches == 0 &&
        report->endpoint_match != 0 &&
        report->interrupt_match != 0 &&
        report->conditioned_pcm_match != 0 &&
        report->cancel_match != 0;
    if (process_timing_count != 0u) {
        qsort(process_timings,
              process_timing_count,
              sizeof(process_timings[0]),
              compare_u64);
        report->process_ns_p50 =
            process_timings[(process_timing_count * 50u) / 100u];
        report->process_ns_p95 =
            process_timings[(process_timing_count * 95u) / 100u];
        report->process_ns_p99 =
            process_timings[(process_timing_count * 99u) / 100u];
    }
    status = 0;

cleanup:
    audio_engine_free_buffer(legacy_audio);
    free(embedded_audio);
    audio_engine_destroy(legacy);
    reflex_session_destroy_v1(embedded);
    return status;
}

static void test_config_and_validation(void) {
    reflex_config_v1 config;
    reflex_config_default_v1(&config);
    assert(config.abi_version == REFLEX_SESSION_ABI_V1);
    assert(config.struct_size == sizeof(config));

    reflex_session_v1 *session = NULL;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_OK);
    assert(session != NULL);

    uint8_t odd[3] = {0};
    reflex_decision_v1 decision = {0};
    assert(reflex_session_process_v1(session,
                                     odd,
                                     sizeof(odd),
                                     1u,
                                     0u,
                                     &decision) == REFLEX_ERR_FORMAT);
    assert(decision.rejected_frames == 1u);
    assert(decision.processed_frames == 0u);
    reflex_session_destroy_v1(session);

    config.abi_version++;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_ERR_ABI);

    reflex_config_default_v1(&config);
    config.high_pass_alpha = NAN;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_ERR_ARGUMENT);
    assert(session == NULL);

    reflex_config_default_v1(&config);
    config.energy_threshold = INFINITY;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_ERR_ARGUMENT);
    assert(session == NULL);

    reflex_config_default_v1(&config);
    config.interrupt_energy_threshold = NAN;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_ERR_ARGUMENT);
    assert(session == NULL);

    reflex_config_default_v1(&config);
    config.flags |= 1u << 31;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_ERR_ARGUMENT);
    assert(session == NULL);
}

static void test_legacy_replay_parity(void) {
    const int speech_frames = 32;
    const int total_frames = speech_frames + 44;
    uint8_t *pcm = calloc((size_t)total_frames, FRAME_BYTES);
    assert(pcm != NULL);
    for (int frame_index = 0; frame_index < total_frames; frame_index++) {
        if (frame_index < speech_frames) {
            fill_voice((int16_t *)(pcm + (size_t)frame_index * FRAME_BYTES),
                       (unsigned)frame_index);
        }
    }

    replay_report report = {0};
    assert(run_replay(pcm,
                      (size_t)total_frames * FRAME_BYTES,
                      1,
                      -1,
                      &report) == 0);
    assert(report.overall_match != 0);
    assert(report.legacy_endpoint_frame >= 0);
    assert(report.embedded_endpoint_frame == report.legacy_endpoint_frame);
    assert(report.legacy_interrupt_frame ==
           (int)(500000000u / FRAME_NS));
    assert(report.embedded_interrupt_frame == report.legacy_interrupt_frame);
    assert(report.legacy_audio_bytes ==
           report.processed_frames * FRAME_BYTES);
    free(pcm);
}

static void test_snapshot_capacity(void) {
    reflex_config_v1 config;
    reflex_config_default_v1(&config);
    reflex_session_v1 *session = NULL;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_OK);

    int16_t frame[FRAME_SAMPLES] = {0};
    fill_voice(frame, 0u);
    reflex_decision_v1 decision = {0};
    assert(reflex_session_process_v1(session,
                                     (const uint8_t *)frame,
                                     sizeof(frame),
                                     1u,
                                     0u,
                                     &decision) == REFLEX_OK);

    uint8_t too_small[FRAME_BYTES / 2] = {0};
    size_t required = 0u;
    assert(reflex_session_snapshot_v1(session,
                                      too_small,
                                      sizeof(too_small),
                                      &required,
                                      &decision) == REFLEX_ERR_CAPACITY);
    assert(required == FRAME_BYTES);

    uint8_t output[FRAME_BYTES] = {0};
    size_t written = 0u;
    assert(reflex_session_finalize_v1(session,
                                      output,
                                      sizeof(output),
                                      &written,
                                      REFLEX_ENDPOINT_CLIENT_END,
                                      &decision) == REFLEX_OK);
    assert(written == FRAME_BYTES);
    assert((decision.flags & REFLEX_DECISION_COMPLETE) != 0u);
    reflex_session_destroy_v1(session);
}

static void test_cancel_and_reset(void) {
    reflex_config_v1 config;
    reflex_config_default_v1(&config);
    reflex_session_v1 *session = NULL;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_OK);

    int16_t frame[FRAME_SAMPLES];
    fill_voice(frame, 0u);
    reflex_decision_v1 decision = {0};
    assert(reflex_session_process_v1(session,
                                     (const uint8_t *)frame,
                                     sizeof(frame),
                                     1u,
                                     0u,
                                     &decision) == REFLEX_OK);
    reflex_session_cancel_v1(session, &decision);
    assert((decision.flags & REFLEX_DECISION_CANCELLED) != 0u);
    assert(decision.endpoint_reason == REFLEX_ENDPOINT_CANCELLED);
    assert(decision.utterance_bytes == 0u);
    assert(reflex_session_process_v1(session,
                                     (const uint8_t *)frame,
                                     sizeof(frame),
                                     2u,
                                     0u,
                                     &decision) == REFLEX_ERR_STATE);

    reflex_session_reset_v1(session);
    assert(reflex_session_process_v1(session,
                                     (const uint8_t *)frame,
                                     sizeof(frame),
                                     3u,
                                     0u,
                                     &decision) == REFLEX_OK);
    reflex_session_destroy_v1(session);
}

static void test_decision_only_mode(void) {
    reflex_config_v1 config;
    reflex_session_v1 *session = NULL;
    int16_t frame[FRAME_SAMPLES];
    uint8_t output[FRAME_BYTES];
    reflex_decision_v1 decision = {0};
    size_t required = 0u;
    size_t written = 0u;
    reflex_config_default_v1(&config);
    config.flags &= ~(uint32_t)REFLEX_CONFIG_RETAIN_UTTERANCE;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_OK);
    fill_voice(frame, 0u);
    assert(reflex_session_process_v1(
               session, (const uint8_t *)frame, sizeof(frame), 1u, 0u,
               &decision) == REFLEX_OK);
    assert(decision.utterance_bytes == FRAME_BYTES);
    assert(reflex_session_snapshot_v1(
               session, NULL, 0u, &required, &decision) == REFLEX_ERR_STATE);
    assert(required == FRAME_BYTES);
    assert(decision.status == REFLEX_ERR_STATE);
    assert(reflex_session_finalize_v1(
               session, output, sizeof(output), &written,
               REFLEX_ENDPOINT_CLIENT_END, &decision) == REFLEX_ERR_STATE);
    assert(written == FRAME_BYTES);
    assert(decision.status == REFLEX_ERR_STATE);
    assert((decision.flags & REFLEX_DECISION_COMPLETE) == 0u);
    reflex_session_destroy_v1(session);
}

static void test_delivery_stall_is_not_silence(void) {
    reflex_config_v1 config;
    reflex_config_default_v1(&config);
    config.flags &= ~(uint32_t)(REFLEX_CONFIG_HIGH_PASS | REFLEX_CONFIG_NOISE_GATE);
    reflex_session_v1 *session = NULL;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_OK);
    int16_t frame[FRAME_SAMPLES];
    reflex_decision_v1 decision = {0};
    uint64_t now = UINT64_C(1000000000);
    fill_voice(frame, 0u);
    assert(reflex_session_process_v1(session, (const uint8_t *)frame,
        sizeof(frame), now, 0u, &decision) == REFLEX_OK);
    memset(frame, 0, sizeof(frame));
    for (unsigned i = 0; i < 25u; ++i) {
        now += i == 12u ? UINT64_C(600000000) : FRAME_NS;
        assert(reflex_session_process_v1(session, (const uint8_t *)frame,
            sizeof(frame), now, 0u, &decision) == REFLEX_OK);
        assert((decision.flags & REFLEX_DECISION_ENDPOINT) == 0u);
    }
    /* A correction resumes after 500 ms of received silence. */
    fill_voice(frame, 1u);
    now += FRAME_NS;
    assert(reflex_session_process_v1(session, (const uint8_t *)frame,
        sizeof(frame), now, 0u, &decision) == REFLEX_OK);
    assert((decision.flags & REFLEX_DECISION_ENDPOINT) == 0u);
    memset(frame, 0, sizeof(frame));
    for (unsigned i = 0; i < 41u; ++i) {
        now += FRAME_NS;
        assert(reflex_session_process_v1(session, (const uint8_t *)frame,
            sizeof(frame), now, 0u, &decision) == REFLEX_OK);
        if (i < 40u) assert((decision.flags & REFLEX_DECISION_ENDPOINT) == 0u);
    }
    assert(decision.endpoint_reason == REFLEX_ENDPOINT_SILENCE);
    reflex_session_destroy_v1(session);
}

static void benchmark_process_p99(void) {
    reflex_config_v1 config;
    reflex_config_default_v1(&config);
    config.max_utterance_bytes = FRAME_BYTES * 128u;
    reflex_session_v1 *session = NULL;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_OK);

    int16_t frame[FRAME_SAMPLES];
    fill_voice(frame, 0u);
    uint64_t timings[BENCH_SAMPLES];
    uint64_t now_ns = 1000000000u;
    reflex_decision_v1 decision = {0};

    for (int i = 0; i < BENCH_SAMPLES; i++) {
        if ((i % 100) == 0) {
            reflex_session_reset_v1(session);
        }
        const uint64_t started_ns = monotonic_ns();
        assert(reflex_session_process_v1(session,
                                         (const uint8_t *)frame,
                                         sizeof(frame),
                                         now_ns,
                                         0u,
                                         &decision) == REFLEX_OK);
        timings[i] = monotonic_ns() - started_ns;
        now_ns += FRAME_NS;
    }
    qsort(timings, BENCH_SAMPLES, sizeof(timings[0]), compare_u64);
    const uint64_t p50_ns = timings[BENCH_SAMPLES / 2];
    const uint64_t p99_ns = timings[(BENCH_SAMPLES * 99) / 100];
    printf("REFLEX_PROCESS_NS p50=%llu p99=%llu samples=%d\n",
           (unsigned long long)p50_ns,
           (unsigned long long)p99_ns,
           BENCH_SAMPLES);
    if (getenv("REFLEX_SKIP_LATENCY_ASSERT") == NULL) {
        assert(p99_ns < 50000u);
    }
    reflex_session_destroy_v1(session);

    config.flags &= ~(uint32_t)REFLEX_CONFIG_RETAIN_UTTERANCE;
    session = NULL;
    assert(reflex_session_init_v1(&config, &session) == REFLEX_OK);
    now_ns = 1000000000u;
    for (int i = 0; i < BENCH_SAMPLES; i++) {
        if ((i % 100) == 0) reflex_session_reset_v1(session);
        const uint64_t started_ns = monotonic_ns();
        assert(reflex_session_process_v1(
                   session, (const uint8_t *)frame, sizeof(frame), now_ns, 0u,
                   &decision) == REFLEX_OK);
        timings[i] = monotonic_ns() - started_ns;
        now_ns += FRAME_NS;
    }
    qsort(timings, BENCH_SAMPLES, sizeof(timings[0]), compare_u64);
    printf("REFLEX_DECISION_ONLY_NS p50=%llu p99=%llu samples=%d\n",
           (unsigned long long)timings[BENCH_SAMPLES / 2],
           (unsigned long long)timings[(BENCH_SAMPLES * 99) / 100],
           BENCH_SAMPLES);
    if (getenv("REFLEX_SKIP_LATENCY_ASSERT") == NULL) {
        assert(timings[(BENCH_SAMPLES * 99) / 100] < 50000u);
    }
    reflex_session_destroy_v1(session);
}

static int read_pcm_file(const char *path, uint8_t **out_pcm, size_t *out_len) {
    if (path == NULL || out_pcm == NULL || out_len == NULL) {
        return -1;
    }
    *out_pcm = NULL;
    *out_len = 0u;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    const long length = ftell(file);
    if (length <= 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    uint8_t *pcm = malloc((size_t)length);
    if (pcm == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(pcm, 1u, (size_t)length, file) != (size_t)length ||
        fclose(file) != 0) {
        free(pcm);
        return -1;
    }
    *out_pcm = pcm;
    *out_len = (size_t)length;
    return 0;
}

static int parse_long(const char *value, long minimum, long maximum, long *out) {
    if (value == NULL || out == NULL) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return -1;
    }
    *out = parsed;
    return 0;
}

static const char *json_bool(int value) {
    return value != 0 ? "true" : "false";
}

static int run_captured_fixture(const char *path,
                                int assistant_speaking,
                                long cancel_frame) {
    uint8_t *pcm = NULL;
    size_t pcm_len = 0u;
    if (read_pcm_file(path, &pcm, &pcm_len) != 0) {
        fprintf(stderr, "failed to read captured PCM fixture\n");
        return 2;
    }

    replay_report report = {0};
    const int replay_status = run_replay(
        pcm, pcm_len, assistant_speaking, cancel_frame, &report);
    free(pcm);
    if (replay_status != 0) {
        fprintf(stderr,
                "captured PCM must be non-empty 16 kHz mono S16LE in complete 20 ms frames and at most 30 seconds\n");
        return 2;
    }

    printf(
        "{\"schema_version\":1,"
        "\"input_bytes\":%zu,"
        "\"total_frames\":%zu,"
        "\"processed_frames\":%zu,"
        "\"legacy_speech_frames\":%zu,"
        "\"embedded_speech_frames\":%zu,"
        "\"speech_mismatches\":%d,"
        "\"energy_mismatches\":%d,"
        "\"legacy_endpoint_frame\":%d,"
        "\"embedded_endpoint_frame\":%d,"
        "\"endpoint_reason\":%d,"
        "\"legacy_interrupt_frame\":%d,"
        "\"embedded_interrupt_frame\":%d,"
        "\"cancel_frame\":%d,"
        "\"cancel_applied\":%s,"
        "\"legacy_audio_bytes\":%zu,"
        "\"embedded_audio_bytes\":%zu,"
        "\"endpoint_match\":%s,"
        "\"interrupt_match\":%s,"
        "\"conditioned_pcm_match\":%s,"
        "\"cancel_match\":%s,"
        "\"overall_match\":%s,"
        "\"process_ns_p50\":%llu,"
        "\"process_ns_p95\":%llu,"
        "\"process_ns_p99\":%llu}\n",
        report.input_bytes,
        report.total_frames,
        report.processed_frames,
        report.legacy_speech_frames,
        report.embedded_speech_frames,
        report.speech_mismatches,
        report.energy_mismatches,
        report.legacy_endpoint_frame,
        report.embedded_endpoint_frame,
        report.endpoint_reason,
        report.legacy_interrupt_frame,
        report.embedded_interrupt_frame,
        report.cancel_frame,
        json_bool(report.cancel_applied),
        report.legacy_audio_bytes,
        report.embedded_audio_bytes,
        json_bool(report.endpoint_match),
        json_bool(report.interrupt_match),
        json_bool(report.conditioned_pcm_match),
        json_bool(report.cancel_match),
        json_bool(report.overall_match),
        (unsigned long long)report.process_ns_p50,
        (unsigned long long)report.process_ns_p95,
        (unsigned long long)report.process_ns_p99);
    return report.overall_match != 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--captured") == 0) {
        long assistant_speaking = 0;
        long cancel_frame = -1;
        if (parse_long(argv[3], 0, 1, &assistant_speaking) != 0 ||
            parse_long(argv[4], -1, 1500, &cancel_frame) != 0) {
            fputs("invalid captured replay arguments\n", stderr);
            return 2;
        }
        return run_captured_fixture(
            argv[2], (int)assistant_speaking, cancel_frame);
    }
    if (argc != 1) {
        fprintf(stderr,
                "usage: %s [--captured PCM_S16LE ASSISTANT_SPEAKING CANCEL_FRAME]\n",
                argv[0]);
        return 2;
    }

    test_config_and_validation();
    test_legacy_replay_parity();
    test_snapshot_capacity();
    test_cancel_and_reset();
    test_decision_only_mode();
    test_delivery_stall_is_not_silence();
    benchmark_process_p99();
    puts("reflex_session_v1: all gates passed");
    return 0;
}
