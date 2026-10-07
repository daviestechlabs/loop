#define _POSIX_C_SOURCE 200809L

/* Compare the prior staged formatter with the bounded OpenAI request writer. */

#include "openai_min.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SAMPLE_COUNT = 101,
    ROUNDS_PER_SAMPLE = 1000,
    OUTPUT_CAPACITY = 4096,
};

typedef size_t (*request_builder)(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text,
    int stream
);

typedef struct {
    const char *name;
    const char *model;
    const char *prompt;
} request_fixture;

typedef struct {
    double samples[SAMPLE_COUNT];
    double average_ns;
    double p50_ns;
    double p99_ns;
    uint64_t checksum;
} bench_result;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static uint64_t mix64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static size_t prior_build_chat_request_json(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text,
    int stream
) {
    char escaped_model[256];
    char escaped_user[3584];
    size_t escaped_len;
    int written;
    if (!out || out_cap == 0 || !model || !user_text) return 0;
    if (json_escape_string(
            model,
            escaped_model,
            sizeof(escaped_model),
            &escaped_len) != 0 ||
        json_escape_string(
            user_text,
            escaped_user,
            sizeof(escaped_user),
            &escaped_len) != 0) return 0;
    written = snprintf(
        out,
        out_cap,
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"max_completion_tokens\":256,\"temperature\":0.2,\"stream\":%s,%s"
        "\"include_reasoning\":false,"
        "\"chat_template_kwargs\":{\"enable_thinking\":false}}",
        escaped_model,
        escaped_user,
        stream ? "true" : "false",
        stream ? "\"stream_options\":{\"include_usage\":true}," : "");
    if (written <= 0 || (size_t)written >= out_cap) return 0;
    return (size_t)written;
}

static size_t current_build_chat_request_json(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text,
    int stream
) {
    return stream ?
        openai_chat_request_json_stream(out, out_cap, model, user_text) :
        openai_chat_request_json(out, out_cap, model, user_text);
}

static int verify_span_fixture(const request_fixture *fixture) {
    unsigned char expected[OUTPUT_CAPACITY + 1u];
    unsigned char span[OUTPUT_CAPACITY + 1u];
    size_t prompt_len = strlen(fixture->prompt);
    size_t full_len;
    size_t expected_len;
    size_t span_len;
    size_t cap;

    full_len = openai_chat_request_json_stream_bounded(
        (char *)expected,
        OUTPUT_CAPACITY,
        fixture->model,
        fixture->prompt,
        256u);
    if (full_len == 0u) return -1;
    for (cap = 1u; cap <= full_len + 1u; ++cap) {
        memset(expected, 0xa5, sizeof(expected));
        memset(span, 0xa5, sizeof(span));
        expected_len = openai_chat_request_json_stream_bounded(
            (char *)expected,
            cap,
            fixture->model,
            fixture->prompt,
            256u);
        span_len = openai_chat_request_json_stream_bounded_span(
            (char *)span,
            cap,
            fixture->model,
            fixture->prompt,
            prompt_len,
            256u);
        if (span_len != expected_len || expected[cap] != 0xa5u ||
            span[cap] != 0xa5u ||
            (span_len != 0u &&
             memcmp(expected, span, span_len + 1u) != 0)) {
            fprintf(
                stderr,
                "span fixture=%s cap=%zu expected=%zu current=%zu\n",
                fixture->name,
                cap,
                expected_len,
                span_len);
            return -1;
        }
    }
    return 0;
}

static int verify_one_fixture(const request_fixture *fixture, int stream) {
    unsigned char prior[OUTPUT_CAPACITY + 1u];
    unsigned char current[OUTPUT_CAPACITY + 1u];
    size_t expected_len;
    size_t current_len;
    size_t cap;
    memset(prior, 0xa5, sizeof(prior));
    memset(current, 0xa5, sizeof(current));
    expected_len = prior_build_chat_request_json(
        (char *)prior,
        OUTPUT_CAPACITY,
        fixture->model,
        fixture->prompt,
        stream);
    current_len = current_build_chat_request_json(
        (char *)current,
        OUTPUT_CAPACITY,
        fixture->model,
        fixture->prompt,
        stream);
    if (expected_len == 0 || current_len != expected_len ||
        memcmp(prior, current, expected_len + 1u) != 0) {
        fprintf(
            stderr,
            "fixture=%s stream=%d full expected=%zu current=%zu\n",
            fixture->name,
            stream,
            expected_len,
            current_len);
        return -1;
    }
    for (cap = 1; cap <= expected_len + 1u; ++cap) {
        size_t prior_len;
        size_t bounded_len;
        memset(prior, 0xa5, sizeof(prior));
        memset(current, 0xa5, sizeof(current));
        prior_len = prior_build_chat_request_json(
            (char *)prior,
            cap,
            fixture->model,
            fixture->prompt,
            stream);
        bounded_len = current_build_chat_request_json(
            (char *)current,
            cap,
            fixture->model,
            fixture->prompt,
            stream);
        if (bounded_len != prior_len || prior[cap] != 0xa5u ||
            current[cap] != 0xa5u ||
            (bounded_len != 0 &&
             memcmp(prior, current, bounded_len + 1u) != 0)) {
            fprintf(
                stderr,
                "fixture=%s stream=%d cap=%zu expected=%zu current=%zu "
                "prior_guard=%u current_guard=%u\n",
                fixture->name,
                stream,
                cap,
                prior_len,
                bounded_len,
                (unsigned)prior[cap],
                (unsigned)current[cap]);
            return -1;
        }
    }
    return 0;
}

static int verify_builders(const request_fixture *fixtures, size_t fixture_count) {
    char maximum_model[129];
    char oversized_model[129];
    char oversized_prompt[601];
    char embedded_nul[] = {'a', '\0', 'b', '\0'};
    char escaped_then_nul[] = {'"', 'a', '\0', 'b', '\0'};
    char stale_length[] = {'a', 'b', 'c', 'x'};
    char output[OUTPUT_CAPACITY];
    request_fixture boundary_fixtures[] = {
        {"empty", "", ""},
        {"maximum-model", maximum_model, "prompt"},
    };
    const size_t boundary_count =
        sizeof(boundary_fixtures) / sizeof(boundary_fixtures[0]);
    size_t i;
    for (i = 0; i < fixture_count; ++i) {
        if (verify_one_fixture(&fixtures[i], 0) != 0 ||
            verify_one_fixture(&fixtures[i], 1) != 0 ||
            verify_span_fixture(&fixtures[i]) != 0) return -1;
    }
    memset(maximum_model, '"', sizeof(maximum_model) - 2u);
    maximum_model[sizeof(maximum_model) - 2u] = 'A';
    maximum_model[sizeof(maximum_model) - 1u] = '\0';
    for (i = 0; i < boundary_count; ++i) {
        if (verify_one_fixture(&boundary_fixtures[i], 0) != 0 ||
            verify_one_fixture(&boundary_fixtures[i], 1) != 0) return -1;
    }
    memset(oversized_model, '"', sizeof(oversized_model) - 1u);
    oversized_model[sizeof(oversized_model) - 1u] = '\0';
    memset(oversized_prompt, 1, sizeof(oversized_prompt) - 1u);
    oversized_prompt[sizeof(oversized_prompt) - 1u] = '\0';
    if (prior_build_chat_request_json(
            output, sizeof(output), oversized_model, "prompt", 1) != 0 ||
        current_build_chat_request_json(
            output, sizeof(output), oversized_model, "prompt", 1) != 0 ||
        prior_build_chat_request_json(
            output, sizeof(output), "default", oversized_prompt, 1) != 0 ||
        current_build_chat_request_json(
            output, sizeof(output), "default", oversized_prompt, 1) != 0 ||
        current_build_chat_request_json(NULL, 0, "default", "prompt", 1) != 0 ||
        current_build_chat_request_json(output, 0, "default", "prompt", 1) != 0 ||
        current_build_chat_request_json(
            output, sizeof(output), NULL, "prompt", 1) != 0 ||
        current_build_chat_request_json(
            output, sizeof(output), "default", NULL, 1) != 0)
        return -1;
    if (openai_chat_request_json_stream_bounded_span(
            output, sizeof(output), "default", NULL, 0u, 256u) != 0u ||
        openai_chat_request_json_stream_bounded_span(
            output,
            sizeof(output),
            "default",
            embedded_nul,
            sizeof(embedded_nul) - 1u,
            256u) != 0u ||
        openai_chat_request_json_stream_bounded_span(
            output,
            sizeof(output),
            "default",
            escaped_then_nul,
            sizeof(escaped_then_nul) - 1u,
            256u) != 0u ||
        openai_chat_request_json_stream_bounded_span(
            output,
            sizeof(output),
            "default",
            stale_length,
            sizeof(stale_length) - 1u,
            256u) != 0u ||
        openai_chat_request_json_stream_bounded_span(
            output, sizeof(output), "default", "prompt", 5u, 256u) != 0u ||
        openai_chat_request_json_stream_bounded_span(
            output, sizeof(output), "default", "prompt", 3584u, 256u) != 0u)
        return -1;
    return 0;
}

static void run_sample(
    request_builder builder,
    const request_fixture *fixtures,
    size_t fixture_count,
    double *elapsed_ns,
    uint64_t *checksum
) {
    char output[OUTPUT_CAPACITY];
    uint64_t started = monotonic_ns();
    size_t round;
    for (round = 0; round < ROUNDS_PER_SAMPLE; ++round) {
        const request_fixture *fixture = &fixtures[round % fixture_count];
        int stream = (int)(round & 1u);
        size_t length = builder(
            output,
            sizeof(output),
            fixture->model,
            fixture->prompt,
            stream);
        if (length == 0) {
            *elapsed_ns = -1.0;
            return;
        }
        *checksum ^= mix64(
            (uint64_t)(unsigned char)output[(round * 17u) % length] +
            (uint64_t)length + (uint64_t)round);
    }
    *elapsed_ns = (double)(monotonic_ns() - started) / ROUNDS_PER_SAMPLE;
}

static int benchmark_builders(
    const request_fixture *fixtures,
    size_t fixture_count,
    bench_result *prior,
    bench_result *current
) {
    size_t sample;
    double prior_total = 0.0;
    double current_total = 0.0;
    memset(prior, 0, sizeof(*prior));
    memset(current, 0, sizeof(*current));
    for (sample = 0; sample < SAMPLE_COUNT; ++sample) {
        if ((sample & 1u) == 0) {
            run_sample(
                prior_build_chat_request_json,
                fixtures,
                fixture_count,
                &prior->samples[sample],
                &prior->checksum);
            run_sample(
                current_build_chat_request_json,
                fixtures,
                fixture_count,
                &current->samples[sample],
                &current->checksum);
        } else {
            run_sample(
                current_build_chat_request_json,
                fixtures,
                fixture_count,
                &current->samples[sample],
                &current->checksum);
            run_sample(
                prior_build_chat_request_json,
                fixtures,
                fixture_count,
                &prior->samples[sample],
                &prior->checksum);
        }
        if (prior->samples[sample] < 0.0 || current->samples[sample] < 0.0)
            return -1;
        prior_total += prior->samples[sample];
        current_total += current->samples[sample];
    }
    prior->average_ns = prior_total / SAMPLE_COUNT;
    current->average_ns = current_total / SAMPLE_COUNT;
    qsort(prior->samples, SAMPLE_COUNT, sizeof(prior->samples[0]), compare_double);
    qsort(current->samples, SAMPLE_COUNT, sizeof(current->samples[0]), compare_double);
    prior->p50_ns = prior->samples[SAMPLE_COUNT / 2u];
    prior->p99_ns = prior->samples[(SAMPLE_COUNT * 99u) / 100u];
    current->p50_ns = current->samples[SAMPLE_COUNT / 2u];
    current->p99_ns = current->samples[(SAMPLE_COUNT * 99u) / 100u];
    return prior->checksum == current->checksum ? 0 : -1;
}

static void print_benchmark(
    const char *fixture,
    size_t fixture_count,
    const bench_result *prior,
    const bench_result *current
) {
    printf(
        "BenchmarkOpenAIRequest fixture=%s fixtures=%zu rounds=%d samples=%d "
        "prior_avg_ns=%.2f prior_p50_ns=%.2f prior_p99_ns=%.2f "
        "bounded_avg_ns=%.2f bounded_p50_ns=%.2f bounded_p99_ns=%.2f "
        "speedup_x100=%.0f checksum=%llu\n",
        fixture,
        fixture_count,
        ROUNDS_PER_SAMPLE,
        SAMPLE_COUNT,
        prior->average_ns,
        prior->p50_ns,
        prior->p99_ns,
        current->average_ns,
        current->p50_ns,
        current->p99_ns,
        prior->average_ns * 100.0 / current->average_ns,
        (unsigned long long)current->checksum);
}

int main(int argc, char **argv) {
    static const char long_prompt[] =
        "The adventurers enter a moonlit archive beneath Candlekeep. "
        "Explain the hidden ward, cite the recovered clue, and give the table "
        "one clear choice without exposing internal routing or private context.";
    static const char escaped_prompt[] =
        "The rune says \"speak\\listen\".\nLine two\tcontains a ward.\r"
        "Keep the answer bounded and preserve \\ every quoted \"clue\".";
    char maximum_prompt[3072];
    char control_prompt[34];
    char maximum_escaped_prompt[599];
    request_fixture fixtures[] = {
        {
            "short",
            "default",
            "Look up how invisibility changes attack rolls.",
        },
        {"long", "default", long_prompt},
        {"escaped", "model\\\"alias", escaped_prompt},
        {"maximum", "default", maximum_prompt},
        {"controls", "default", control_prompt},
        {"maximum-escaped", "default", maximum_escaped_prompt},
    };
    bench_result prior;
    bench_result current;
    const size_t fixture_count = sizeof(fixtures) / sizeof(fixtures[0]);
    size_t i;
    memset(maximum_prompt, 'A', sizeof(maximum_prompt) - 1u);
    maximum_prompt[sizeof(maximum_prompt) - 1u] = '\0';
    for (i = 0; i < 31u; ++i) control_prompt[i] = (char)(i + 1u);
    control_prompt[31] = '"';
    control_prompt[32] = '\\';
    control_prompt[33] = '\0';
    memset(
        maximum_escaped_prompt,
        1,
        sizeof(maximum_escaped_prompt) - 2u);
    maximum_escaped_prompt[sizeof(maximum_escaped_prompt) - 2u] = 'A';
    maximum_escaped_prompt[sizeof(maximum_escaped_prompt) - 1u] = '\0';
    if (argc > 2 ||
        (argc == 2 && strcmp(argv[1], "--verify") != 0)) {
        fprintf(stderr, "usage: %s [--verify]\n", argv[0]);
        return 2;
    }
    if (verify_builders(fixtures, fixture_count) != 0) {
        fputs("OpenAI request writer differential failed\n", stderr);
        return 1;
    }
    if (argc == 2) {
        puts("OpenAI request writer differential: PASS");
        return 0;
    }
    for (i = 0; i < fixture_count; ++i) {
        if (benchmark_builders(&fixtures[i], 1u, &prior, &current) != 0) {
            fputs("OpenAI request writer benchmark failed\n", stderr);
            return 1;
        }
        print_benchmark(fixtures[i].name, 1u, &prior, &current);
    }
    if (benchmark_builders(
            fixtures, fixture_count, &prior, &current) != 0) {
        fputs("OpenAI request writer aggregate benchmark failed\n", stderr);
        return 1;
    }
    print_benchmark("aggregate", fixture_count, &prior, &current);
    return 0;
}
