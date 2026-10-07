/* Compare the former loop, scalar loop, and dispatched production path. */
#define _POSIX_C_SOURCE 200809L

#include "base64.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BENCH_INPUT_CAP 16384u
#define BENCH_OUTPUT_CAP ((((BENCH_INPUT_CAP) + 2u) / 3u) * 4u + 1u)

typedef size_t (*encode_fn)(const uint8_t *, size_t, char *, size_t);

static size_t branchy_base64(
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

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static int run(
    encode_fn encode,
    size_t input_len,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    static uint8_t input[BENCH_INPUT_CAP];
    static char output[BENCH_OUTPUT_CAP];
    uint64_t started;
    size_t i;
    if (!encode || !elapsed_ns || !checksum || input_len == 0u ||
        input_len > sizeof(input)) return 0;
    for (i = 0; i < input_len; ++i) input[i] = (uint8_t)(i * 131u + 17u);
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < rounds; ++i) {
        size_t output_len;
        input[0] = (uint8_t)i;
        output_len = encode(input, input_len, output, sizeof(output));
        if (output_len == 0u) return 0;
        *checksum = *checksum * 131u + (uint8_t)output[i % output_len];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_size(size_t input_len, size_t rounds, size_t samples) {
    uint64_t branchy_total = 0;
    uint64_t scalar_total = 0;
    uint64_t product_total = 0;
    size_t sample;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t branchy_ns;
        uint64_t scalar_ns;
        uint64_t product_ns;
        uint64_t branchy_checksum;
        uint64_t scalar_checksum;
        uint64_t product_checksum;
        int ok;
        if (sample % 3u == 0u) {
            ok = run(
                branchy_base64, input_len, rounds, &branchy_ns, &branchy_checksum) &&
                run(base64_encode_scalar_v1, input_len, rounds, &scalar_ns,
                    &scalar_checksum) &&
                run(base64_encode_v1, input_len, rounds, &product_ns, &product_checksum);
        } else if (sample % 3u == 1u) {
            ok = run(base64_encode_scalar_v1, input_len, rounds, &scalar_ns,
                    &scalar_checksum) &&
                run(base64_encode_v1, input_len, rounds, &product_ns, &product_checksum) &&
                run(branchy_base64, input_len, rounds, &branchy_ns, &branchy_checksum);
        } else {
            ok = run(base64_encode_v1, input_len, rounds, &product_ns, &product_checksum) &&
                run(branchy_base64, input_len, rounds, &branchy_ns, &branchy_checksum) &&
                run(base64_encode_scalar_v1, input_len, rounds, &scalar_ns,
                    &scalar_checksum);
        }
        if (!ok || branchy_checksum != scalar_checksum ||
            branchy_checksum != product_checksum) return 0;
        branchy_total += branchy_ns;
        scalar_total += scalar_ns;
        product_total += product_ns;
    }
    printf(
        "BenchmarkBase64 input_bytes=%zu rounds=%zu samples=%zu "
        "branchy_ns_per_call=%" PRIu64 " scalar_ns_per_call=%" PRIu64
        " product_ns_per_call=%" PRIu64 " scalar_speedup_x100=%" PRIu64 "\n",
        input_len,
        rounds,
        samples,
        branchy_total / (rounds * samples),
        scalar_total / (rounds * samples),
        product_total / (rounds * samples),
        product_total == 0u ? 0u : scalar_total * 100u / product_total);
    return 1;
}

int main(int argc, char **argv) {
    int verify = argc > 1 && strcmp(argv[1], "--verify") == 0;
    size_t small_rounds = verify ? 200u : 100000u;
    size_t large_rounds = verify ? 20u : 10000u;
    size_t samples = verify ? 3u : 6u;
    if (!benchmark_size(640u, small_rounds, samples) ||
        !benchmark_size(960u, small_rounds, samples) ||
        !benchmark_size(BENCH_INPUT_CAP, large_rounds, samples)) {
        fprintf(stderr, "FAIL Base64 benchmark equivalence\n");
        return 1;
    }
    return 0;
}
