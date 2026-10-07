#define _POSIX_C_SOURCE 200809L

#include "speech_seg_host.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    BENCH_ITERATIONS = 1000000
};

static uint64_t monotonic_ns(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static int benchmark_word_limit(size_t iterations) {
    static const char *const inputs[] = {
        "Warding Step works once per turn when you see the target and remain within 15 feet and beyond",
        "Café Warding Step works once per turn when you see the target and remain within 15 feet and beyond"
    };
    const speech_seg_config_v1 cfg = {0, 8, 80};
    for (size_t c = 0; c < sizeof(inputs) / sizeof(inputs[0]); ++c) {
        size_t length = strlen(inputs[c]);
        volatile uint64_t checksum = 0;
        uint64_t start = monotonic_ns();
        if (!start) return 1;
        for (size_t i = 0; i < iterations; ++i) {
            size_t so, sl, ro, rl;
            if (speech_seg_next_v1(inputs[c], length, &cfg, &so, &sl, &ro, &rl) != SPEECH_SEG_OK)
                return 1;
            checksum += sl + ro + rl + so;
        }
        uint64_t end = monotonic_ns();
        if (end < start || !checksum) return 1;
        printf("BenchmarkSpeechSegmentWordLimit\tcase=%zu\titerations=%zu\tns_per_op=%.3f\tchecksum=%" PRIu64 "\n",
            c, iterations, (double)(end - start) / (double)iterations, checksum);
    }
    return 0;
}

int main(int argc, char **argv) {
    static const char first_word[] = "Well ";
    speech_seg_config_v1 config;
    volatile uint64_t checksum = 0;
    uint64_t started;
    uint64_t finished;
    size_t iterations = BENCH_ITERATIONS;
    size_t iteration;

    if (argc == 2 && strcmp(argv[1], "--verify") == 0) {
        iterations = 1u;
    } else if (argc != 1) {
        return 2;
    }

    speech_seg_config_default_v1(&config);
    config.min_segment_chars = 8;
    config.max_segment_chars = 80;
    config.first_segment_chars = 5;

    started = monotonic_ns();
    if (started == 0) return 1;
    for (iteration = 0; iteration < iterations; ++iteration) {
        speech_seg_host_v1 host;
        char parts[1][SPEECH_SEG_HOST_MAX_PART];
        size_t part_count = 0;

        speech_seg_host_init_v1(&host, &config);
        if (speech_seg_host_add_v1(
                &host,
                (uint64_t)iteration + UINT64_C(1),
                first_word,
                sizeof(first_word) - 1u,
                parts,
                1u,
                &part_count) != SPEECH_SEG_OK ||
            part_count != 1u || strcmp(parts[0], "Well") != 0) {
            speech_seg_host_free_v1(&host);
            return 1;
        }
        checksum += (uint64_t)(unsigned char)parts[0][0] +
            (uint64_t)part_count;
        speech_seg_host_free_v1(&host);
    }
    finished = monotonic_ns();
    if (finished < started || checksum == 0) return 1;

    printf(
        "BenchmarkSpeechSegmentHostLifecycle\titerations=%d\ttotal_ns=%" PRIu64
        "\tns_per_op=%.3f\thost_bytes=%zu\tchecksum=%" PRIu64 "\n",
        (int)iterations,
        finished - started,
        (double)(finished - started) / (double)iterations,
        sizeof(speech_seg_host_v1),
        checksum);
    return benchmark_word_limit(iterations);
}
