#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    BENCH_BUFFER_BYTES = 8192,
    BENCH_READ_BYTES = 4096,
    BENCH_FRAME_BYTES = 1160,
    BENCH_FRAME_COUNT = 1982,
    BENCH_GATES_PER_SAMPLE = 8,
    BENCH_SAMPLES = 21,
};

typedef struct {
    uint8_t data[BENCH_BUFFER_BYTES];
    size_t head;
    size_t len;
} bench_window;

typedef struct {
    double samples[BENCH_SAMPLES];
    uint64_t parsed_frames;
    uint64_t compaction_bytes;
    uint64_t checksum;
} bench_result;

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

static uint64_t monotonic_ns(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void frame_length_write(uint8_t *frame, uint32_t length) {
    memcpy(frame, &length, sizeof(length));
}

static int frame_length_read(const uint8_t *frame, size_t available, size_t *length) {
    uint32_t encoded;

    if (!frame || !length || available < sizeof(encoded)) return -1;
    memcpy(&encoded, frame, sizeof(encoded));
    if (encoded < sizeof(encoded) || encoded > BENCH_BUFFER_BYTES) return -1;
    *length = encoded;
    return 0;
}

static uint8_t *build_stream(size_t *stream_bytes) {
    uint8_t *stream;
    size_t frame;
    size_t total = (size_t)BENCH_FRAME_BYTES * BENCH_FRAME_COUNT;

    if (!stream_bytes) return NULL;
    stream = (uint8_t *)malloc(total);
    if (!stream) return NULL;
    for (frame = 0; frame < BENCH_FRAME_COUNT; frame++) {
        uint8_t *current = stream + frame * BENCH_FRAME_BYTES;
        size_t offset;

        frame_length_write(current, BENCH_FRAME_BYTES);
        for (offset = sizeof(uint32_t); offset < BENCH_FRAME_BYTES; offset++)
            current[offset] = (uint8_t)((frame * 17u + offset * 29u) & 0xffu);
    }
    *stream_bytes = total;
    return stream;
}

static int consume_frames(bench_window *window, int sliding, uint64_t *parsed_frames,
                          uint64_t *compaction_bytes, uint64_t *checksum) {
    while (window->len >= sizeof(uint32_t)) {
        size_t frame_bytes;
        uint8_t *frame = window->data + window->head;

        if (frame_length_read(frame, window->len, &frame_bytes) != 0) return -1;
        if (window->len < frame_bytes) return 0;
        *checksum += frame[sizeof(uint32_t)] + frame[frame_bytes - 1u];
        (*parsed_frames)++;
        window->len -= frame_bytes;
        if (sliding) {
            window->head += frame_bytes;
            if (window->len == 0) window->head = 0;
        } else {
            memmove(window->data, frame + frame_bytes, window->len);
            *compaction_bytes += window->len;
        }
    }
    return 0;
}

static BENCH_NOINLINE int parse_gate(const uint8_t *stream, size_t stream_bytes, int sliding,
                                     uint64_t *parsed_frames, uint64_t *compaction_bytes,
                                     uint64_t *checksum) {
    bench_window window;
    size_t input = 0;

    memset(&window, 0, sizeof(window));
    while (input < stream_bytes) {
        size_t chunk = stream_bytes - input;
        size_t tail;

        if (chunk > BENCH_READ_BYTES) chunk = BENCH_READ_BYTES;
        tail = window.head + window.len;
        if (chunk > BENCH_BUFFER_BYTES - tail) {
            if (!sliding || window.head == 0) return -1;
            memmove(window.data, window.data + window.head, window.len);
            *compaction_bytes += window.len;
            window.head = 0;
            tail = window.len;
        }
        memcpy(window.data + tail, stream + input, chunk);
        window.len += chunk;
        input += chunk;
        if (consume_frames(&window, sliding, parsed_frames, compaction_bytes, checksum) != 0)
            return -1;
    }
    return window.len == 0 ? 0 : -1;
}

static int run_candidate(const uint8_t *stream, size_t stream_bytes, int sliding,
                         bench_result *result) {
    size_t sample;

    memset(result, 0, sizeof(*result));
    for (sample = 0; sample < BENCH_SAMPLES; sample++) {
        uint64_t started = monotonic_ns();
        size_t gate;

        for (gate = 0; gate < BENCH_GATES_PER_SAMPLE; gate++) {
            if (parse_gate(stream, stream_bytes, sliding, &result->parsed_frames,
                           &result->compaction_bytes, &result->checksum) != 0)
                return -1;
        }
        result->samples[sample] = (double)(monotonic_ns() - started) / BENCH_GATES_PER_SAMPLE;
    }
    return 0;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;

    return (a > b) - (a < b);
}

static void print_result(const char *name, bench_result *result, size_t stream_bytes) {
    uint64_t gates = (uint64_t)BENCH_SAMPLES * BENCH_GATES_PER_SAMPLE;
    double copy_amplification =
        ((double)stream_bytes * (double)gates + (double)result->compaction_bytes) /
        ((double)stream_bytes * (double)gates);

    qsort(result->samples, BENCH_SAMPLES, sizeof(result->samples[0]), compare_double);
    printf("%s median_us=%.3f p99_us=%.3f compaction_bytes_per_gate=%.0f "
           "copy_amplification=%.4fx checksum=%llu\n",
           name, result->samples[BENCH_SAMPLES / 2u] / 1000.0,
           result->samples[BENCH_SAMPLES - 1u] / 1000.0,
           (double)result->compaction_bytes / (double)gates, copy_amplification,
           (unsigned long long)result->checksum);
}

int main(int argc, char **argv) {
    uint8_t *stream;
    size_t stream_bytes = 0;
    uint64_t expected_frames = (uint64_t)BENCH_SAMPLES * BENCH_GATES_PER_SAMPLE * BENCH_FRAME_COUNT;
    bench_result compacting;
    bench_result sliding;
    int verify = argc == 2 && strcmp(argv[1], "--verify") == 0;
    int ok;

    if (argc > 2 || (argc == 2 && !verify)) {
        fprintf(stderr, "usage: %s [--verify]\n", argv[0]);
        return 2;
    }
    stream = build_stream(&stream_bytes);
    if (!stream || run_candidate(stream, stream_bytes, 0, &compacting) != 0 ||
        run_candidate(stream, stream_bytes, 1, &sliding) != 0) {
        free(stream);
        fputs("VBus receive-window benchmark failed\n", stderr);
        return 1;
    }
    free(stream);
    print_result("compacting", &compacting, stream_bytes);
    print_result("sliding", &sliding, stream_bytes);
    ok = compacting.parsed_frames == expected_frames && sliding.parsed_frames == expected_frames &&
         compacting.checksum == sliding.checksum && compacting.compaction_bytes > 0 &&
         sliding.compaction_bytes * 8u < compacting.compaction_bytes;
    if (verify)
        printf("verify=%s frames=%llu stream_bytes_per_gate=%zu\n", ok ? "pass" : "fail",
               (unsigned long long)expected_frames, stream_bytes);
    return ok ? 0 : 1;
}
