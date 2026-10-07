#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "audio_engine.h"
#include "telemetry.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static frame_telemetry_t captured_frame;
static stage_event_t captured_vad;
static uint32_t captured_frame_seq[3];
static uint64_t captured_turn_lo[3];
static int frame_count;
static int vad_count;

enum {
    concurrent_writer_count = 8,
    concurrent_events_per_writer = 2000,
};

typedef struct {
    telemetry_ring_t *ring;
    pthread_barrier_t *barrier;
    uint32_t writer_id;
} concurrent_writer_args;

typedef struct {
    int frame_count;
    int stage_count;
    int pointers_aligned;
    uint32_t last_frame_seq;
} small_ring_capture;

static void capture_small_ring(int is_frame, const void *data, size_t size, void *user) {
    small_ring_capture *capture = (small_ring_capture *)user;

    if (is_frame) {
        if (size != sizeof(frame_telemetry_t) ||
            (uintptr_t)data % _Alignof(frame_telemetry_t) != 0) {
            capture->pointers_aligned = 0;
            return;
        }
        capture->frame_count++;
        capture->last_frame_seq = ((const frame_telemetry_t *)data)->frame_seq;
        return;
    }
    if (size != sizeof(stage_event_t) ||
        (uintptr_t)data % _Alignof(stage_event_t) != 0) {
        capture->pointers_aligned = 0;
        return;
    }
    capture->stage_count++;
}

static int test_small_ring_layout(void) {
    telemetry_ring_t *invalid_zero;
    telemetry_ring_t *invalid_huge;
    telemetry_ring_t *ring;
    frame_telemetry_t frame = {0};
    stage_event_t stage = {0};
    ring_stats_t stats = {0};
    small_ring_capture capture = {.pointers_aligned = 1};

    invalid_zero = telemetry_ring_new(0);
    invalid_huge = telemetry_ring_new(SIZE_MAX);
    if (invalid_zero != NULL || invalid_huge != NULL) {
        fprintf(stderr, "telemetry ring accepted an invalid capacity\n");
        telemetry_ring_free(invalid_zero);
        telemetry_ring_free(invalid_huge);
        return -1;
    }
    ring = telemetry_ring_new(2);
    if (!ring) {
        fprintf(stderr, "failed to allocate small telemetry ring\n");
        return -1;
    }
    frame.frame_seq = 1;
    telemetry_emit_frame(ring, &frame);
    telemetry_emit_stage(ring, &stage);
    if (telemetry_get_ring_stats(ring, &stats) != 0 || stats.capacity != 2 ||
        stats.occupancy != 2 || stats.full_events != 0 ||
        telemetry_consume(ring, capture_small_ring, &capture) != 2 ||
        capture.frame_count != 1 || capture.stage_count != 1 || !capture.pointers_aligned) {
        fprintf(stderr, "small telemetry ring capacity or alignment failed\n");
        telemetry_ring_free(ring);
        return -1;
    }

    frame.frame_seq = 2;
    telemetry_emit_frame(ring, &frame);
    frame.frame_seq = 3;
    telemetry_emit_frame(ring, &frame);
    frame.frame_seq = 4;
    telemetry_emit_frame(ring, &frame);
    capture = (small_ring_capture){.pointers_aligned = 1};
    if (telemetry_get_ring_stats(ring, &stats) != 0 || stats.occupancy != 2 ||
        stats.full_events != 1 ||
        telemetry_consume(ring, capture_small_ring, &capture) != 2 ||
        capture.frame_count != 2 || capture.last_frame_seq != 3 || !capture.pointers_aligned) {
        fprintf(stderr, "small telemetry ring drop policy failed\n");
        telemetry_ring_free(ring);
        return -1;
    }
    telemetry_ring_free(ring);
    return 0;
}

static void *emit_concurrently(void *opaque) {
    concurrent_writer_args *args = (concurrent_writer_args *)opaque;
    if (pthread_barrier_wait(args->barrier) == PTHREAD_BARRIER_SERIAL_THREAD) {
        // The serial thread still participates in the same emit loop.
    }
    for (uint32_t index = 0; index < concurrent_events_per_writer; ++index) {
        stage_event_t event = {0};
        event.stage_id = STAGE_KIND_VAD;
        event.extra_u32 = (args->writer_id << 24) | index;
        telemetry_emit_stage(args->ring, &event);
    }
    return NULL;
}

static uint64_t outer_monotonic_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    const clockid_t clock_id = CLOCK_MONOTONIC_RAW;
#else
    const clockid_t clock_id = CLOCK_MONOTONIC;
#endif
    if (clock_gettime(clock_id, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void capture_event(int is_frame, const void *data, size_t size, void *user) {
    (void)user;
    if (is_frame && size == sizeof(frame_telemetry_t)) {
        const frame_telemetry_t *frame = (const frame_telemetry_t *)data;
        if (frame_count == 0) captured_frame = *frame;
        if (frame_count < 3) {
            captured_frame_seq[frame_count] = frame->frame_seq;
            captured_turn_lo[frame_count] = frame->turn_id_lo;
        }
        frame_count++;
        return;
    }
    if (!is_frame && size == sizeof(stage_event_t)) {
        const stage_event_t *stage = (const stage_event_t *)data;
        if (stage->stage_id == STAGE_KIND_VAD) {
            if (vad_count == 0) captured_vad = *stage;
            vad_count++;
        }
    }
}

static int close_enough(double inner_ms, double outer_ms) {
    if (inner_ms <= 0.0 || outer_ms <= 0.0) return 0;
    double max_relative_error = 0.10;
    const char *configured_error = getenv("TIMING_MAX_RELATIVE_ERROR");
    if (configured_error && configured_error[0] != '\0') {
        const double parsed_error = strtod(configured_error, NULL);
        if (parsed_error > 0.0) max_relative_error = parsed_error;
    }
    const double relative_error = fabs(inner_ms - outer_ms) / outer_ms;
    if (relative_error > max_relative_error) {
        fprintf(stderr,
                "inner/outer monotonic timing differs by %.2f%% "
                "(inner=%.6fms outer=%.6fms)\n",
                relative_error * 100.0,
                inner_ms,
                outer_ms);
        return 0;
    }
    return 1;
}

int main(void) {
    int exit_code = 1;
    int writer_barrier_initialized = 0;
    audio_engine_session *session = NULL;
    int16_t *pcm = NULL;

    if (test_small_ring_layout() != 0) return 1;

    telemetry_init_global_ring(32768);
    telemetry_ring_t *ring = telemetry_get_global_ring();
    if (!ring) {
        fprintf(stderr, "failed to initialize telemetry ring\n");
        return 1;
    }

    const uint64_t raw_before = outer_monotonic_ns();
    const uint64_t helper_now = telemetry_monotonic_ns();
    const uint64_t raw_after = outer_monotonic_ns();
    if (raw_before == 0 || helper_now < raw_before || helper_now > raw_after) {
        fprintf(stderr, "telemetry duration clock does not match outer monotonic raw clock\n");
        goto cleanup;
    }

    session = audio_engine_create(
        0.01f, 0.995, 1, 1, 0.0001, 0.95, 1.5, 0.1);
    if (!session) {
        fprintf(stderr, "failed to create audio engine\n");
        goto cleanup;
    }
    enum { sample_count = 16000 };
    pcm = (int16_t *)malloc(sample_count * sizeof(*pcm));
    if (!pcm) goto cleanup;
    for (int i = 0; i < sample_count; ++i) {
        pcm[i] = (int16_t)((i & 16) ? 6000 : -6000);
    }

    const audio_process_options process_options = {0};
    audio_decision decision = {0};
    const uint64_t outer_started_ns = outer_monotonic_ns();
    audio_engine_process(
        session, (const uint8_t *)pcm, sizeof(*pcm) * sample_count,
        outer_started_ns, process_options, &decision);
    const uint64_t outer_ended_ns = outer_monotonic_ns();

    const int drained = telemetry_consume(ring, capture_event, NULL);
    if (drained < 2 || frame_count != 1 || vad_count != 1) {
        fprintf(stderr,
                "missing measured frame/VAD telemetry "
                "(drained=%d frames=%d vad=%d)\n",
                drained,
                frame_count,
                vad_count);
        goto cleanup;
    }
    if (captured_frame.tsc_end <= captured_frame.tsc_start) {
        fprintf(stderr, "cycle counter did not advance across audio processing\n");
        goto cleanup;
    }
    if (!(captured_frame.rtf_this_frame > 0.0f) ||
        !isfinite(captured_frame.rtf_this_frame)) {
        fprintf(stderr, "frame RTF is not a real positive measurement\n");
        goto cleanup;
    }
    if (!(captured_vad.latency_ms > 0.0f) || !isfinite(captured_vad.latency_ms)) {
        fprintf(stderr, "VAD latency is not a real positive measurement\n");
        goto cleanup;
    }

    const double audio_ms = (double)sample_count / 16000.0 * 1000.0;
    const double inner_ms = (double)captured_frame.rtf_this_frame * audio_ms;
    const double outer_ms = (double)(outer_ended_ns - outer_started_ns) / 1000000.0;
    if (!close_enough(inner_ms, outer_ms)) goto cleanup;
    if ((double)captured_vad.latency_ms > inner_ms) {
        fprintf(stderr, "VAD stage latency exceeds total measured process latency\n");
        goto cleanup;
    }

    audio_engine_process(
        session, (const uint8_t *)pcm, sizeof(*pcm) * sample_count,
        outer_monotonic_ns(), process_options, &decision);
    (void)telemetry_consume(ring, capture_event, NULL);
    (void)audio_engine_finish_processing(session, 1);
    audio_engine_process(
        session, (const uint8_t *)pcm, sizeof(*pcm) * sample_count,
        outer_monotonic_ns(), process_options, &decision);
    (void)telemetry_consume(ring, capture_event, NULL);
    if (frame_count != 3 || captured_frame_seq[0] != 0 ||
        captured_frame_seq[1] != 1 || captured_frame_seq[2] != 0) {
        fprintf(stderr,
                "frame sequence is not per-turn monotonic "
                "(count=%d seq=%u,%u,%u)\n",
                frame_count,
                captured_frame_seq[0],
                captured_frame_seq[1],
                captured_frame_seq[2]);
        goto cleanup;
    }
    if (captured_turn_lo[0] == 0 ||
        captured_turn_lo[0] != captured_turn_lo[1] ||
        captured_turn_lo[1] == captured_turn_lo[2]) {
        fprintf(stderr,
                "process-local qh did not persist within and rotate between utterances "
                "(qh=%llu,%llu,%llu)\n",
                (unsigned long long)captured_turn_lo[0],
                (unsigned long long)captured_turn_lo[1],
                (unsigned long long)captured_turn_lo[2]);
        goto cleanup;
    }

    pthread_t writers[concurrent_writer_count];
    concurrent_writer_args writer_args[concurrent_writer_count];
    pthread_barrier_t writer_barrier;
    if (pthread_barrier_init(&writer_barrier, NULL, concurrent_writer_count) != 0) {
        fprintf(stderr, "failed to initialize concurrent telemetry barrier\n");
        goto cleanup;
    }
    writer_barrier_initialized = 1;
    const uint64_t emitted_before = telemetry_emitted_count(ring);
    for (uint32_t index = 0; index < concurrent_writer_count; ++index) {
        writer_args[index] = (concurrent_writer_args){
            .ring = ring,
            .barrier = &writer_barrier,
            .writer_id = index,
        };
        if (pthread_create(&writers[index], NULL, emit_concurrently, &writer_args[index]) != 0) {
            fprintf(stderr, "failed to create concurrent telemetry writer %u\n", index);
            return 1;
        }
    }
    for (uint32_t index = 0; index < concurrent_writer_count; ++index) {
        if (pthread_join(writers[index], NULL) != 0) {
            fprintf(stderr, "failed to join concurrent telemetry writer %u\n", index);
            return 1;
        }
    }
    pthread_barrier_destroy(&writer_barrier);
    writer_barrier_initialized = 0;

    const size_t expected_events =
        concurrent_writer_count * concurrent_events_per_writer;
    ring_stats_t stats = {0};
    if (telemetry_get_ring_stats(ring, &stats) != 0 ||
        stats.occupancy != expected_events ||
        telemetry_emitted_count(ring) - emitted_before != expected_events) {
        fprintf(stderr,
                "concurrent telemetry lost events "
                "(occupancy=%zu emitted_delta=%llu expected=%zu)\n",
                stats.occupancy,
                (unsigned long long)(telemetry_emitted_count(ring) - emitted_before),
                expected_events);
        goto cleanup;
    }

    printf("timing test: PASS inner=%.6fms outer=%.6fms rtf=%.8f "
           "vad=%.6fms concurrent_events=%zu\n",
           inner_ms,
           outer_ms,
           captured_frame.rtf_this_frame,
           captured_vad.latency_ms,
           expected_events);

    exit_code = 0;

cleanup:
    if (writer_barrier_initialized) pthread_barrier_destroy(&writer_barrier);
    free(pcm);
    audio_engine_destroy(session);
    telemetry_free_global_ring();
    return exit_code;
}
