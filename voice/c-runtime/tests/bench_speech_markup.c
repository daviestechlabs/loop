#define _POSIX_C_SOURCE 200809L
#include "speech_markup.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

int main(void) {
    static const char plain[] = "Sneak Attack deals 1d6 extra damage. Check the conditions before rolling. Use 2 * 3 for multiplication.";
    static const char marked[] = "**Sneak Attack** deals **1d6** extra damage. _Check the conditions_ before rolling. Use `2 * 3` for multiplication.";
    char bounded[2048];
    const char *inputs[] = {plain, marked, bounded};
    const char *names[] = {"plain", "marked", "overlong"};
    const size_t chunks[] = {1u, 16u, 64u, 2047u};
    volatile uint64_t checksum = 0;
    memset(bounded, 'a', sizeof(bounded) - 1u);
    bounded[0] = '*';
    bounded[sizeof(bounded) - 2u] = '*';
    bounded[sizeof(bounded) - 1u] = '\0';
    for (size_t c = 0; c < sizeof(inputs) / sizeof(inputs[0]); ++c) {
        size_t size = strlen(inputs[c]);
        for (size_t p = 0; p < sizeof(chunks) / sizeof(chunks[0]); ++p) {
            size_t iterations = c == 2u ? 5000u : 20000u;
            uint64_t start = monotonic_ns();
            if (!start) return 1;
            for (size_t run = 0; run < iterations; ++run) {
                speech_markup_v1 state;
                char output[2048];
                size_t used = 0, n = 0;
                speech_markup_init_v1(&state);
                for (size_t i = 0; i < size;) {
                    size_t take = chunks[p] < size - i ? chunks[p] : size - i;
                    if (speech_markup_feed_v1(&state, inputs[c] + i, take, 0,
                            output + used, sizeof(output) - used, &n)) return 1;
                    used += n;
                    i += take;
                }
                if (speech_markup_feed_v1(&state, NULL, 0, 1, output + used, sizeof(output) - used, &n)) return 1;
                used += n;
                if (!used || used > size) return 1;
                checksum += (uint64_t)(unsigned char)output[used - 1u] + (uint64_t)used;
            }
            uint64_t end = monotonic_ns();
            if (end < start) return 1;
            printf("BenchmarkSpeechMarkup\tcase=%s\tinput_bytes=%zu\tchunk_bytes=%zu\titerations=%zu"
                "\tns_per_answer=%.3f\tstate_bytes=%zu\tchecksum=%" PRIu64 "\n",
                names[c], size, chunks[p], iterations, (double)(end - start) / (double)iterations,
                sizeof(speech_markup_v1), checksum);
        }
    }
    return 0;
}
