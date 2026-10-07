#define _POSIX_C_SOURCE 200809L

/* Compare the prior staged TTS formatter with the bounded request writer. */

#include "openai_min.h"
#include "speech_sanitize.h"
#include "voice_ascii.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SAMPLE_COUNT = 101,
    ROUNDS_PER_SAMPLE = 1000,
    INSTRUCTION_ROUNDS = 2000000,
    OUTPUT_CAPACITY = 14336,
};

typedef size_t (*request_builder)(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
);

typedef struct {
    const char *name;
    const char *text;
    const char *voice;
    const char *turn_id;
    uint32_t query_hash;
} request_fixture;

typedef struct {
    double samples[SAMPLE_COUNT];
    double average_ns;
    double p50_ns;
    double p99_ns;
    uint64_t checksum;
} bench_result;

typedef struct {
    const char *request_id;
    size_t request_id_len;
    const char *subject;
    size_t subject_len;
} dispatch_fixture;

typedef uint64_t (*dispatch_builder)(const dispatch_fixture *fixture);

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

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

static size_t prior_build_tts_request_json(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
) {
    char escaped_text[12288];
    char escaped_voice[768];
    char escaped_turn_id[768];
    size_t escaped_len;
    int written;
    if (!out || out_cap == 0 || !text || !voice || !turn_id) return 0;
    if (json_escape_string(
            text,
            escaped_text,
            sizeof(escaped_text),
            &escaped_len) != 0 ||
        json_escape_string(
            voice,
            escaped_voice,
            sizeof(escaped_voice),
            &escaped_len) != 0 ||
        json_escape_string(
            turn_id,
            escaped_turn_id,
            sizeof(escaped_turn_id),
            &escaped_len) != 0) return 0;
    written = snprintf(
        out,
        out_cap,
        "{\"text\":\"%s\",\"voice\":\"%s\",\"turn_id\":\"%s\","
        "\"query_hash\":%u,\"frames_per_chunk\":1}",
        escaped_text,
        escaped_voice,
        escaped_turn_id,
        query_hash);
    if (written <= 0 || (size_t)written >= out_cap) return 0;
    return (size_t)written;
}

static size_t current_build_tts_request_json(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
) {
    if (!text || !voice || !turn_id) return 0;
    return tts_pcm_request_json_spans(
        out,
        out_cap,
        text,
        strlen(text),
        voice,
        strlen(voice),
        turn_id,
        strlen(turn_id),
        query_hash);
}

static size_t prepared_id_build_tts_request_json(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
) {
    if (!text || !voice || !turn_id) return 0;
    return tts_pcm_request_json_prepared_id_spans(
        out,
        out_cap,
        text,
        strlen(text),
        voice,
        strlen(voice),
        turn_id,
        strlen(turn_id),
        query_hash);
}

static size_t prepared_text_id_build_tts_request_json(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
) {
    if (!text || !voice || !turn_id) return 0;
    return tts_pcm_request_json_prepared_spans(
        out,
        out_cap,
        text,
        strlen(text),
        voice,
        strlen(voice),
        turn_id,
        strlen(turn_id),
        query_hash,
        1);
}

static size_t separate_sanitized_build_tts_request_json(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
) {
    char sanitized[2048];
    size_t sanitized_len = 0u;
    if (!text || !voice || !turn_id ||
        speech_sanitize_v1(
            text,
            strlen(text),
            1,
            sanitized,
            sizeof(sanitized),
            &sanitized_len) != SPEECH_SANITIZE_OK ||
        sanitized_len >= sizeof(sanitized)) return 0u;
    sanitized[sanitized_len] = '\0';
    return tts_pcm_request_json_prepared_id_spans(
        out,
        out_cap,
        sanitized,
        sanitized_len,
        voice,
        strlen(voice),
        turn_id,
        strlen(turn_id),
        query_hash);
}

static size_t proven_sanitized_build_tts_request_json(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
) {
    char sanitized[2048];
    size_t sanitized_len = 0u;
    int json_plain = 0;
    if (!text || !voice || !turn_id ||
        speech_sanitize_json_plain_v1(
            text,
            strlen(text),
            1,
            sanitized,
            sizeof(sanitized),
            &sanitized_len,
            &json_plain) != SPEECH_SANITIZE_OK ||
        sanitized_len >= sizeof(sanitized)) return 0u;
    sanitized[sanitized_len] = '\0';
    return tts_pcm_request_json_prepared_spans(
        out,
        out_cap,
        sanitized,
        sanitized_len,
        voice,
        strlen(voice),
        turn_id,
        strlen(turn_id),
        query_hash,
        json_plain);
}

static volatile uint32_t dispatch_lane_observed;

static BENCH_NOINLINE uint32_t dispatch_request_hash(
    const char *request_id,
    size_t request_id_len
) {
    const unsigned char *input = (const unsigned char *)request_id;
    uint32_t hash = 2166136261u;
    size_t i;
    for (i = 0u; i < request_id_len; ++i) {
        hash ^= input[i];
        hash *= 16777619u;
    }
    return hash;
}

static BENCH_NOINLINE int dispatch_request_id_valid(
    const char *request_id,
    size_t request_id_len
) {
    size_t i;
    if (!request_id || request_id_len == 0u || request_id_len >= 128u)
        return 0;
    for (i = 0u; i < request_id_len; ++i) {
        unsigned char c = (unsigned char)request_id[i];
        if (!(voice_ascii_is_alnum(c) || c == '-' || c == '_' || c == '.' ||
              c == ':')) return 0;
    }
    return 1;
}

static BENCH_NOINLINE int dispatch_request_id_prepare(
    const char *request_id,
    size_t request_id_len,
    uint32_t *hash_out
) {
    uint32_t hash = 2166136261u;
    size_t i;
    if (!request_id || !hash_out || request_id_len == 0u ||
        request_id_len >= 128u) return 0;
    for (i = 0u; i < request_id_len; ++i) {
        unsigned char c = (unsigned char)request_id[i];
        if (!(voice_ascii_is_alnum(c) || c == '-' || c == '_' || c == '.' ||
              c == ':')) return 0;
        hash ^= c;
        hash *= 16777619u;
    }
    *hash_out = hash;
    return 1;
}

static BENCH_NOINLINE size_t dispatch_string_subject_len(
    const char *subject
) {
    return subject ? strlen(subject) : 0u;
}

static BENCH_NOINLINE size_t dispatch_prepared_subject_len(
    const char *subject,
    size_t subject_len
) {
    return subject && subject_len != 0u ? subject_len : 0u;
}

typedef uint32_t (*dispatch_hash_fn)(const char *, size_t);
typedef int (*dispatch_valid_fn)(const char *, size_t);
typedef int (*dispatch_prepare_fn)(const char *, size_t, uint32_t *);
typedef size_t (*dispatch_string_len_fn)(const char *);
typedef size_t (*dispatch_prepared_len_fn)(const char *, size_t);

static dispatch_hash_fn volatile dispatch_hash_call = dispatch_request_hash;
static dispatch_valid_fn volatile dispatch_valid_call =
    dispatch_request_id_valid;
static dispatch_prepare_fn volatile dispatch_prepare_call =
    dispatch_request_id_prepare;
static dispatch_string_len_fn volatile dispatch_string_len_call =
    dispatch_string_subject_len;
static dispatch_prepared_len_fn volatile dispatch_prepared_len_call =
    dispatch_prepared_subject_len;

static BENCH_NOINLINE uint64_t prior_dispatch_metadata(
    const dispatch_fixture *fixture
) {
    uint32_t lane = dispatch_hash_call(
        fixture->request_id, fixture->request_id_len) & 3u;
    size_t published;
    dispatch_lane_observed = lane;
    lane = dispatch_hash_call(
        fixture->request_id, fixture->request_id_len) & 3u;
    published = dispatch_string_len_call(fixture->subject);
    published += dispatch_string_len_call(fixture->subject);
    published += dispatch_string_len_call(fixture->subject);
    return ((uint64_t)lane << 32) | published;
}

static BENCH_NOINLINE uint64_t current_dispatch_metadata(
    const dispatch_fixture *fixture
) {
    uint32_t lane = dispatch_hash_call(
        fixture->request_id, fixture->request_id_len) & 3u;
    size_t published;
    dispatch_lane_observed = lane;
    published = dispatch_prepared_len_call(
        fixture->subject, fixture->subject_len);
    published += dispatch_prepared_len_call(
        fixture->subject, fixture->subject_len);
    published += dispatch_prepared_len_call(
        fixture->subject, fixture->subject_len);
    return ((uint64_t)lane << 32) | published;
}

static BENCH_NOINLINE uint64_t separate_request_preparation(
    const dispatch_fixture *fixture
) {
    uint32_t hash;
    if (!dispatch_valid_call(
            fixture->request_id, fixture->request_id_len)) return 0u;
    hash = dispatch_hash_call(
        fixture->request_id, fixture->request_id_len);
    return UINT64_C(0x100000000) | hash;
}

static BENCH_NOINLINE uint64_t combined_request_preparation(
    const dispatch_fixture *fixture
) {
    uint32_t hash;
    if (!dispatch_prepare_call(
            fixture->request_id, fixture->request_id_len, &hash)) return 0u;
    return UINT64_C(0x100000000) | hash;
}

static int verify_request_preparation(void) {
    char request_id[128];
    uint32_t separate_hash;
    uint32_t combined_hash;
    size_t position;
    unsigned int value;
    if (dispatch_request_id_valid(NULL, 1u) ||
        dispatch_request_id_prepare(NULL, 1u, &combined_hash) ||
        dispatch_request_id_valid(request_id, 0u) ||
        dispatch_request_id_prepare(request_id, 0u, &combined_hash) ||
        dispatch_request_id_valid(request_id, sizeof(request_id)) ||
        dispatch_request_id_prepare(
            request_id, sizeof(request_id), &combined_hash) ||
        dispatch_request_id_prepare(request_id, 1u, NULL)) return -1;
    memset(request_id, 'a', sizeof(request_id));
    for (position = 0u; position < sizeof(request_id) - 1u; ++position) {
        for (value = 0u; value <= UINT8_MAX; ++value) {
            int separate_valid;
            int combined_valid;
            request_id[position] = (char)value;
            separate_valid = dispatch_request_id_valid(
                request_id, sizeof(request_id) - 1u);
            combined_valid = dispatch_request_id_prepare(
                request_id, sizeof(request_id) - 1u, &combined_hash);
            if (separate_valid != combined_valid) return -1;
            if (separate_valid) {
                separate_hash = dispatch_request_hash(
                    request_id, sizeof(request_id) - 1u);
                if (separate_hash != combined_hash) return -1;
            }
        }
        request_id[position] = 'a';
    }
    return 0;
}

static int verify_dispatch_metadata(
    const dispatch_fixture *fixtures,
    size_t fixture_count
) {
    size_t i;
    for (i = 0u; i < fixture_count; ++i) {
        if (fixtures[i].request_id_len == 0u || fixtures[i].subject_len == 0u ||
            fixtures[i].request_id[fixtures[i].request_id_len] != '\0' ||
            fixtures[i].subject[fixtures[i].subject_len] != '\0' ||
            prior_dispatch_metadata(&fixtures[i]) !=
                current_dispatch_metadata(&fixtures[i]) ||
            separate_request_preparation(&fixtures[i]) !=
                combined_request_preparation(&fixtures[i]))
            return -1;
    }
    return 0;
}

static int verify_one_fixture(const request_fixture *fixture) {
    unsigned char prior[OUTPUT_CAPACITY + 1u];
    unsigned char current[OUTPUT_CAPACITY + 1u];
    unsigned char legacy[OUTPUT_CAPACITY + 1u];
    size_t expected_len;
    size_t current_len;
    size_t legacy_len;
    size_t cap;
    memset(prior, 0xa5, sizeof(prior));
    memset(current, 0xa5, sizeof(current));
    expected_len = prior_build_tts_request_json(
        (char *)prior,
        OUTPUT_CAPACITY,
        fixture->text,
        fixture->voice,
        fixture->turn_id,
        fixture->query_hash);
    current_len = current_build_tts_request_json(
        (char *)current,
        OUTPUT_CAPACITY,
        fixture->text,
        fixture->voice,
        fixture->turn_id,
        fixture->query_hash);
    legacy_len = tts_pcm_request_json(
        (char *)legacy,
        OUTPUT_CAPACITY,
        fixture->text,
        fixture->voice,
        fixture->turn_id,
        fixture->query_hash);
    if (expected_len == 0 || current_len != expected_len ||
        legacy_len != expected_len ||
        memcmp(prior, current, expected_len + 1u) != 0 ||
        memcmp(prior, legacy, expected_len + 1u) != 0) {
        fprintf(
            stderr,
            "fixture=%s full expected=%zu current=%zu legacy=%zu\n",
            fixture->name,
            expected_len,
            current_len,
            legacy_len);
        return -1;
    }
    for (cap = 1; cap <= expected_len + 1u; ++cap) {
        size_t prior_len;
        size_t bounded_len;
        memset(prior, 0xa5, sizeof(prior));
        memset(current, 0xa5, sizeof(current));
        prior_len = prior_build_tts_request_json(
            (char *)prior,
            cap,
            fixture->text,
            fixture->voice,
            fixture->turn_id,
            fixture->query_hash);
        bounded_len = current_build_tts_request_json(
            (char *)current,
            cap,
            fixture->text,
            fixture->voice,
            fixture->turn_id,
            fixture->query_hash);
        if (bounded_len != prior_len || prior[cap] != 0xa5u ||
            current[cap] != 0xa5u ||
            (bounded_len != 0 &&
             memcmp(prior, current, bounded_len + 1u) != 0)) {
            fprintf(
                stderr,
                "fixture=%s cap=%zu expected=%zu current=%zu "
                "prior_guard=%u current_guard=%u\n",
                fixture->name,
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

static int verify_empty_voice_fast_path(void) {
    static const char text[] = "Roll with advantage.";
    static const char request_id[] = "req-empty-voice";
    unsigned char prior[OUTPUT_CAPACITY + 1u];
    unsigned char current[OUTPUT_CAPACITY + 1u];
    size_t expected_len;
    size_t cap;
    expected_len = prior_build_tts_request_json(
        (char *)prior,
        OUTPUT_CAPACITY,
        text,
        "",
        request_id,
        1u);
    if (expected_len == 0u) return -1;
    for (cap = 1u; cap <= expected_len + 1u; ++cap) {
        size_t prior_len;
        size_t current_len;
        memset(prior, 0xa5, sizeof(prior));
        memset(current, 0xa5, sizeof(current));
        prior_len = prior_build_tts_request_json(
            (char *)prior, cap, text, "", request_id, 1u);
        current_len = tts_pcm_request_json_prepared_spans(
            (char *)current,
            cap,
            text,
            sizeof(text) - 1u,
            "",
            0u,
            request_id,
            sizeof(request_id) - 1u,
            1u,
            1);
        if (current_len != prior_len || prior[cap] != 0xa5u ||
            current[cap] != 0xa5u ||
            (current_len != 0u &&
             memcmp(current, prior, current_len + 1u) != 0)) return -1;
    }
    memset(current, 0xa5, sizeof(current));
    return tts_pcm_request_json_prepared_spans(
        (char *)current,
        OUTPUT_CAPACITY,
        text,
        sizeof(text) - 1u,
        "x",
        0u,
        request_id,
        sizeof(request_id) - 1u,
        1u,
        1) == 0u && current[0] == '\0' ? 0 : -1;
}

static int verify_span_word_scan(void) {
    unsigned char prior[OUTPUT_CAPACITY];
    unsigned char current[OUTPUT_CAPACITY];
    char storage[40];
    size_t alignment;
    size_t position;
    unsigned value;
    for (alignment = 0u; alignment < 8u; ++alignment) {
        char *input = storage + alignment;
        for (position = 0u; position < 24u; ++position) {
            for (value = 1u; value <= 255u; ++value) {
                size_t prior_len;
                size_t current_len;
                memset(input, 'A', 24u);
                input[position] = (char)value;
                input[24] = '\0';
                prior_len = prior_build_tts_request_json(
                    (char *)prior,
                    sizeof(prior),
                    input,
                    "tara",
                    "req-word-scan",
                    1u);
                current_len = tts_pcm_request_json_spans(
                    (char *)current,
                    sizeof(current),
                    input,
                    24u,
                    "tara",
                    sizeof("tara") - 1u,
                    "req-word-scan",
                    sizeof("req-word-scan") - 1u,
                    1u);
                if (current_len != prior_len ||
                    memcmp(current, prior, prior_len + 1u) != 0) {
                    fprintf(
                        stderr,
                        "word scan alignment=%zu position=%zu value=%u\n",
                        alignment,
                        position,
                        value);
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int verify_builders(
    const request_fixture *fixtures,
    size_t fixture_count
) {
    char exact_text[2053];
    char exact_voice[133];
    char exact_turn_id[133];
    char oversized_text[2049];
    char oversized_voice[129];
    char oversized_turn_id[129];
    char quote_below_fast_path[256];
    char quote_at_fast_path[257];
    char slash_at_fast_path[257];
    char control_at_fast_path[257];
    char mixed_after_fast_path[258];
    char identifier_escape_before_prefix[17];
    char identifier_escape_at_prefix[18];
    char identifier_escape_after_prefix[19];
    char output[OUTPUT_CAPACITY];
    static const char embedded_null[] = {'a', '\0', 'b', '\0'};
    request_fixture boundary_fixtures[] = {
        {"empty", "", "", "", 0},
        {"exact-text", exact_text, "tara", "req-boundary", UINT32_MAX},
        {"exact-voice", "speak", exact_voice, "req-boundary", 1},
        {"exact-turn", "speak", "tara", exact_turn_id, 10},
        {"quote-below-fast-path", quote_below_fast_path,
            "tara", "req-boundary", 11},
        {"quote-at-fast-path", quote_at_fast_path,
            "tara", "req-boundary", 12},
        {"slash-at-fast-path", slash_at_fast_path,
            "tara", "req-boundary", 13},
        {"control-at-fast-path", control_at_fast_path,
            "tara", "req-boundary", 14},
        {"mixed-after-fast-path", mixed_after_fast_path,
            "tara", "req-boundary", 15},
        {"identifier-escape-before-prefix", "speak",
            identifier_escape_before_prefix,
            identifier_escape_before_prefix, 16},
        {"identifier-escape-at-prefix", "speak",
            identifier_escape_at_prefix,
            identifier_escape_at_prefix, 17},
        {"identifier-escape-after-prefix", "speak",
            identifier_escape_after_prefix,
            identifier_escape_after_prefix, 18},
    };
    const size_t boundary_count =
        sizeof(boundary_fixtures) / sizeof(boundary_fixtures[0]);
    size_t i;
    for (i = 0; i < fixture_count; ++i) {
        if (verify_one_fixture(&fixtures[i]) != 0) return -1;
    }
    memset(exact_text, 1, 2047u);
    memset(exact_text + 2047u, 'A', 5u);
    exact_text[2052] = '\0';
    memset(exact_voice, 1, 127u);
    memset(exact_voice + 127u, 'A', 5u);
    exact_voice[132] = '\0';
    memset(exact_turn_id, 1, 127u);
    memset(exact_turn_id + 127u, 'A', 5u);
    exact_turn_id[132] = '\0';
    memset(quote_below_fast_path, '"', 255u);
    quote_below_fast_path[255] = '\0';
    memset(quote_at_fast_path, '"', 256u);
    quote_at_fast_path[256] = '\0';
    memset(slash_at_fast_path, '\\', 256u);
    slash_at_fast_path[256] = '\0';
    memset(control_at_fast_path, 1, 256u);
    control_at_fast_path[256] = '\0';
    memset(mixed_after_fast_path, '"', 256u);
    mixed_after_fast_path[256] = 'A';
    mixed_after_fast_path[257] = '\0';
    memset(identifier_escape_before_prefix, 'A', 15u);
    identifier_escape_before_prefix[15] = '"';
    identifier_escape_before_prefix[16] = '\0';
    memset(identifier_escape_at_prefix, 'A', 16u);
    identifier_escape_at_prefix[16] = '"';
    identifier_escape_at_prefix[17] = '\0';
    memset(identifier_escape_after_prefix, 'A', 17u);
    identifier_escape_after_prefix[17] = '"';
    identifier_escape_after_prefix[18] = '\0';
    for (i = 0; i < boundary_count; ++i) {
        if (verify_one_fixture(&boundary_fixtures[i]) != 0) return -1;
    }
    memset(oversized_text, 1, sizeof(oversized_text) - 1u);
    oversized_text[sizeof(oversized_text) - 1u] = '\0';
    memset(oversized_voice, 1, sizeof(oversized_voice) - 1u);
    oversized_voice[sizeof(oversized_voice) - 1u] = '\0';
    memset(oversized_turn_id, 1, sizeof(oversized_turn_id) - 1u);
    oversized_turn_id[sizeof(oversized_turn_id) - 1u] = '\0';
    if (prior_build_tts_request_json(
            output,
            sizeof(output),
            oversized_text,
            "tara",
            "req-overflow",
            0) != 0 ||
        current_build_tts_request_json(
            output,
            sizeof(output),
            oversized_text,
            "tara",
            "req-overflow",
            0) != 0 ||
        prior_build_tts_request_json(
            output,
            sizeof(output),
            "speak",
            oversized_voice,
            "req-overflow",
            0) != 0 ||
        current_build_tts_request_json(
            output,
            sizeof(output),
            "speak",
            oversized_voice,
            "req-overflow",
            0) != 0 ||
        prior_build_tts_request_json(
            output,
            sizeof(output),
            "speak",
            "tara",
            oversized_turn_id,
            0) != 0 ||
        current_build_tts_request_json(
            output,
            sizeof(output),
            "speak",
            "tara",
            oversized_turn_id,
            0) != 0 ||
        current_build_tts_request_json(
            NULL, 0, "speak", "tara", "req-null", 0) != 0 ||
        current_build_tts_request_json(
            output, 0, "speak", "tara", "req-null", 0) != 0 ||
        current_build_tts_request_json(
            output, sizeof(output), NULL, "tara", "req-null", 0) != 0 ||
        current_build_tts_request_json(
            output, sizeof(output), "speak", NULL, "req-null", 0) != 0 ||
        current_build_tts_request_json(
            output, sizeof(output), "speak", "tara", NULL, 0) != 0)
        return -1;
    if (tts_pcm_request_json_spans(
            output,
            sizeof(output),
            "speak",
            sizeof("speak") - 2u,
            "tara",
            sizeof("tara") - 1u,
            "req-span",
            sizeof("req-span") - 1u,
            0) != 0 ||
        tts_pcm_request_json_spans(
            output,
            sizeof(output),
            embedded_null,
            sizeof(embedded_null) - 1u,
            "tara",
            sizeof("tara") - 1u,
            "req-span",
            sizeof("req-span") - 1u,
            0) != 0 ||
        verify_empty_voice_fast_path() != 0 ||
        verify_span_word_scan() != 0)
        return -1;
    return 0;
}

static int verify_prepared_id_builders(
    const request_fixture *fixtures,
    size_t fixture_count
) {
    unsigned char current[OUTPUT_CAPACITY + 1u];
    unsigned char prepared[OUTPUT_CAPACITY + 1u];
    size_t i;
    for (i = 0u; i < fixture_count; ++i) {
        size_t expected_len;
        size_t current_len;
        size_t prepared_len;
        size_t cap;
        memset(current, 0xa5, sizeof(current));
        memset(prepared, 0xa5, sizeof(prepared));
        current_len = current_build_tts_request_json(
            (char *)current,
            OUTPUT_CAPACITY,
            fixtures[i].text,
            fixtures[i].voice,
            fixtures[i].turn_id,
            fixtures[i].query_hash);
        prepared_len = prepared_id_build_tts_request_json(
            (char *)prepared,
            OUTPUT_CAPACITY,
            fixtures[i].text,
            fixtures[i].voice,
            fixtures[i].turn_id,
            fixtures[i].query_hash);
        if (current_len == 0u || prepared_len != current_len ||
            memcmp(current, prepared, current_len + 1u) != 0) return -1;
        expected_len = current_len;
        for (cap = 1u; cap <= expected_len + 1u; ++cap) {
            memset(current, 0xa5, sizeof(current));
            memset(prepared, 0xa5, sizeof(prepared));
            current_len = current_build_tts_request_json(
                (char *)current,
                cap,
                fixtures[i].text,
                fixtures[i].voice,
                fixtures[i].turn_id,
                fixtures[i].query_hash);
            prepared_len = prepared_id_build_tts_request_json(
                (char *)prepared,
                cap,
                fixtures[i].text,
                fixtures[i].voice,
                fixtures[i].turn_id,
                fixtures[i].query_hash);
            if (prepared_len != current_len ||
                current[cap] != 0xa5u || prepared[cap] != 0xa5u ||
                (current_len != 0u &&
                 memcmp(current, prepared, current_len + 1u) != 0)) return -1;
        }
    }
    return tts_pcm_request_json_prepared_id_spans(
        (char *)prepared,
        OUTPUT_CAPACITY,
        "speak",
        sizeof("speak") - 1u,
        "tara",
        sizeof("tara") - 1u,
        "req-prepared",
        sizeof("req-prepared") - 2u,
        1u) == 0u ? 0 : -1;
}

static int verify_prepared_text_id_builders(
    const request_fixture *fixtures,
    size_t fixture_count
) {
    unsigned char current[OUTPUT_CAPACITY + 1u];
    unsigned char prepared[OUTPUT_CAPACITY + 1u];
    size_t i;
    for (i = 0u; i < fixture_count; ++i) {
        size_t expected_len;
        size_t current_len;
        size_t prepared_len;
        size_t cap;
        memset(current, 0xa5, sizeof(current));
        memset(prepared, 0xa5, sizeof(prepared));
        current_len = prepared_id_build_tts_request_json(
            (char *)current,
            OUTPUT_CAPACITY,
            fixtures[i].text,
            fixtures[i].voice,
            fixtures[i].turn_id,
            fixtures[i].query_hash);
        prepared_len = prepared_text_id_build_tts_request_json(
            (char *)prepared,
            OUTPUT_CAPACITY,
            fixtures[i].text,
            fixtures[i].voice,
            fixtures[i].turn_id,
            fixtures[i].query_hash);
        if (current_len == 0u || prepared_len != current_len ||
            memcmp(current, prepared, current_len + 1u) != 0) return -1;
        expected_len = current_len;
        for (cap = 1u; cap <= expected_len + 1u; ++cap) {
            memset(current, 0xa5, sizeof(current));
            memset(prepared, 0xa5, sizeof(prepared));
            current_len = prepared_id_build_tts_request_json(
                (char *)current,
                cap,
                fixtures[i].text,
                fixtures[i].voice,
                fixtures[i].turn_id,
                fixtures[i].query_hash);
            prepared_len = prepared_text_id_build_tts_request_json(
                (char *)prepared,
                cap,
                fixtures[i].text,
                fixtures[i].voice,
                fixtures[i].turn_id,
                fixtures[i].query_hash);
            if (prepared_len != current_len ||
                current[cap] != 0xa5u || prepared[cap] != 0xa5u ||
                (current_len != 0u &&
                 memcmp(current, prepared, current_len + 1u) != 0)) return -1;
        }
    }
    if (tts_pcm_request_json_prepared_spans(
            (char *)prepared,
            OUTPUT_CAPACITY,
            "speak",
            sizeof("speak") - 2u,
            "tara",
            sizeof("tara") - 1u,
            "req-prepared",
            sizeof("req-prepared") - 1u,
            1u,
            1) != 0u ||
        tts_pcm_request_json_prepared_spans(
            (char *)prepared,
            OUTPUT_CAPACITY,
            "speak",
            12288u,
            "tara",
            sizeof("tara") - 1u,
            "req-prepared",
            sizeof("req-prepared") - 1u,
            1u,
            1) != 0u ||
        tts_pcm_request_json_prepared_spans(
            (char *)prepared,
            OUTPUT_CAPACITY,
            "speak",
            sizeof("speak") - 1u,
            "tara",
            sizeof("tara") - 1u,
            "req-prepared",
            sizeof("req-prepared") - 2u,
            1u,
            1) != 0u ||
        tts_pcm_request_json_prepared_spans(
            (char *)prepared,
            OUTPUT_CAPACITY,
            NULL,
            0u,
            "tara",
            sizeof("tara") - 1u,
            "req-prepared",
            sizeof("req-prepared") - 1u,
            1u,
            1) != 0u) return -1;
    return 0;
}

static int verify_sanitized_proof_builders(
    const request_fixture *fixtures,
    size_t fixture_count
) {
    unsigned char separate[OUTPUT_CAPACITY + 1u];
    unsigned char proven[OUTPUT_CAPACITY + 1u];
    size_t i;
    for (i = 0u; i < fixture_count; ++i) {
        size_t expected_len = separate_sanitized_build_tts_request_json(
            (char *)separate,
            OUTPUT_CAPACITY,
            fixtures[i].text,
            fixtures[i].voice,
            fixtures[i].turn_id,
            fixtures[i].query_hash);
        size_t cap;
        if (expected_len == 0u) return -1;
        for (cap = 1u; cap <= expected_len + 1u; ++cap) {
            size_t separate_len;
            size_t proven_len;
            memset(separate, 0xa5, sizeof(separate));
            memset(proven, 0xa5, sizeof(proven));
            separate_len = separate_sanitized_build_tts_request_json(
                (char *)separate,
                cap,
                fixtures[i].text,
                fixtures[i].voice,
                fixtures[i].turn_id,
                fixtures[i].query_hash);
            proven_len = proven_sanitized_build_tts_request_json(
                (char *)proven,
                cap,
                fixtures[i].text,
                fixtures[i].voice,
                fixtures[i].turn_id,
                fixtures[i].query_hash);
            if (proven_len != separate_len ||
                separate[cap] != 0xa5u || proven[cap] != 0xa5u ||
                (separate_len != 0u &&
                 memcmp(separate, proven, separate_len + 1u) != 0)) return -1;
        }
    }
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
        size_t length = builder(
            output,
            sizeof(output),
            fixture->text,
            fixture->voice,
            fixture->turn_id,
            fixture->query_hash);
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
    request_builder prior_builder,
    request_builder current_builder,
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
                prior_builder,
                fixtures,
                fixture_count,
                &prior->samples[sample],
                &prior->checksum);
            run_sample(
                current_builder,
                fixtures,
                fixture_count,
                &current->samples[sample],
                &current->checksum);
        } else {
            run_sample(
                current_builder,
                fixtures,
                fixture_count,
                &current->samples[sample],
                &current->checksum);
            run_sample(
                prior_builder,
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

static uint64_t run_instruction_builder(
    request_builder builder,
    const request_fixture *fixtures,
    size_t fixture_count
) {
    char output[OUTPUT_CAPACITY];
    uint64_t checksum = 0u;
    size_t round;
    for (round = 0u; round < INSTRUCTION_ROUNDS; ++round) {
        const request_fixture *fixture = &fixtures[round % fixture_count];
        size_t length = builder(
            output,
            sizeof(output),
            fixture->text,
            fixture->voice,
            fixture->turn_id,
            fixture->query_hash);
        if (length == 0u) return 0u;
        checksum ^= mix64(
            (uint64_t)(unsigned char)output[(round * 17u) % length] +
            (uint64_t)length + (uint64_t)round);
    }
    return checksum;
}

static void run_dispatch_sample(
    dispatch_builder builder,
    const dispatch_fixture *fixtures,
    size_t fixture_count,
    double *elapsed_ns,
    uint64_t *checksum
) {
    uint64_t started = monotonic_ns();
    size_t round;
    for (round = 0u; round < ROUNDS_PER_SAMPLE; ++round) {
        uint64_t value = builder(&fixtures[round % fixture_count]);
        *checksum ^= mix64(value + round);
    }
    *elapsed_ns = (double)(monotonic_ns() - started) / ROUNDS_PER_SAMPLE;
}

static int benchmark_dispatch_metadata(
    dispatch_builder prior_builder,
    dispatch_builder current_builder,
    const dispatch_fixture *fixtures,
    size_t fixture_count,
    bench_result *prior,
    bench_result *current
) {
    size_t sample;
    double prior_total = 0.0;
    double current_total = 0.0;
    memset(prior, 0, sizeof(*prior));
    memset(current, 0, sizeof(*current));
    for (sample = 0u; sample < SAMPLE_COUNT; ++sample) {
        if ((sample & 1u) == 0u) {
            run_dispatch_sample(
                prior_builder,
                fixtures,
                fixture_count,
                &prior->samples[sample],
                &prior->checksum);
            run_dispatch_sample(
                current_builder,
                fixtures,
                fixture_count,
                &current->samples[sample],
                &current->checksum);
        } else {
            run_dispatch_sample(
                current_builder,
                fixtures,
                fixture_count,
                &current->samples[sample],
                &current->checksum);
            run_dispatch_sample(
                prior_builder,
                fixtures,
                fixture_count,
                &prior->samples[sample],
                &prior->checksum);
        }
        prior_total += prior->samples[sample];
        current_total += current->samples[sample];
    }
    prior->average_ns = prior_total / SAMPLE_COUNT;
    current->average_ns = current_total / SAMPLE_COUNT;
    qsort(prior->samples, SAMPLE_COUNT, sizeof(prior->samples[0]), compare_double);
    qsort(
        current->samples,
        SAMPLE_COUNT,
        sizeof(current->samples[0]),
        compare_double);
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
        "BenchmarkTTSRequest fixture=%s fixtures=%zu rounds=%d samples=%d "
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

static void print_dispatch_benchmark(
    size_t fixture_count,
    const bench_result *prior,
    const bench_result *current
) {
    printf(
        "BenchmarkTTSDispatchMetadata fixtures=%zu rounds=%d samples=%d "
        "prior_avg_ns=%.2f prior_p50_ns=%.2f prior_p99_ns=%.2f "
        "current_avg_ns=%.2f current_p50_ns=%.2f current_p99_ns=%.2f "
        "speedup_x100=%.0f checksum=%llu\n",
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

static void print_request_preparation_benchmark(
    size_t fixture_count,
    const bench_result *separate,
    const bench_result *combined
) {
    printf(
        "BenchmarkTTSRequestPreparation fixtures=%zu rounds=%d samples=%d "
        "separate_avg_ns=%.2f separate_p50_ns=%.2f separate_p99_ns=%.2f "
        "combined_avg_ns=%.2f combined_p50_ns=%.2f combined_p99_ns=%.2f "
        "speedup_x100=%.0f checksum=%llu\n",
        fixture_count,
        ROUNDS_PER_SAMPLE,
        SAMPLE_COUNT,
        separate->average_ns,
        separate->p50_ns,
        separate->p99_ns,
        combined->average_ns,
        combined->p50_ns,
        combined->p99_ns,
        separate->average_ns * 100.0 / combined->average_ns,
        (unsigned long long)combined->checksum);
}

static void print_prepared_id_benchmark(
    size_t fixture_count,
    const bench_result *current,
    const bench_result *prepared
) {
    printf(
        "BenchmarkTTSPreparedID fixtures=%zu rounds=%d samples=%d "
        "current_avg_ns=%.2f current_p50_ns=%.2f current_p99_ns=%.2f "
        "prepared_avg_ns=%.2f prepared_p50_ns=%.2f prepared_p99_ns=%.2f "
        "speedup_x100=%.0f checksum=%llu\n",
        fixture_count,
        ROUNDS_PER_SAMPLE,
        SAMPLE_COUNT,
        current->average_ns,
        current->p50_ns,
        current->p99_ns,
        prepared->average_ns,
        prepared->p50_ns,
        prepared->p99_ns,
        current->average_ns * 100.0 / prepared->average_ns,
        (unsigned long long)prepared->checksum);
}

static void print_prepared_text_benchmark(
    size_t fixture_count,
    const bench_result *current,
    const bench_result *prepared
) {
    printf(
        "BenchmarkTTSPreparedText fixtures=%zu rounds=%d samples=%d "
        "current_avg_ns=%.2f current_p50_ns=%.2f current_p99_ns=%.2f "
        "prepared_avg_ns=%.2f prepared_p50_ns=%.2f prepared_p99_ns=%.2f "
        "speedup_x100=%.0f checksum=%llu\n",
        fixture_count,
        ROUNDS_PER_SAMPLE,
        SAMPLE_COUNT,
        current->average_ns,
        current->p50_ns,
        current->p99_ns,
        prepared->average_ns,
        prepared->p50_ns,
        prepared->p99_ns,
        current->average_ns * 100.0 / prepared->average_ns,
        (unsigned long long)prepared->checksum);
}

static void print_sanitized_proof_benchmark(
    const char *fixture,
    size_t fixture_count,
    const bench_result *separate,
    const bench_result *proven
) {
    printf(
        "BenchmarkTTSSanitizedTextProof fixture=%s fixtures=%zu "
        "rounds=%d samples=%d "
        "separate_avg_ns=%.2f separate_p50_ns=%.2f separate_p99_ns=%.2f "
        "proven_avg_ns=%.2f proven_p50_ns=%.2f proven_p99_ns=%.2f "
        "speedup_x100=%.0f checksum=%llu\n",
        fixture,
        fixture_count,
        ROUNDS_PER_SAMPLE,
        SAMPLE_COUNT,
        separate->average_ns,
        separate->p50_ns,
        separate->p99_ns,
        proven->average_ns,
        proven->p50_ns,
        proven->p99_ns,
        separate->average_ns * 100.0 / proven->average_ns,
        (unsigned long long)proven->checksum);
}

int main(int argc, char **argv) {
    static const char long_text[] =
        "Beyond the ward, the hidden library opens beneath Candlekeep. "
        "The recovered clue proves which sigil controls the final door.";
    static const char escaped_text[] =
        "The rune says \"speak\\listen\".\nLine two\tcontains a ward.\r";
    static const char mixed_plain_text[] =
        "The ancient gate opens when moonlight touches the unfinished "
        "silver\\road home, beyond Candlekeep and beneath the western ward.";
    char maximum_text[2048];
    char maximum_escaped_text[2048];
    char maximum_control_text[2048];
    char maximum_identifiers[128];
    char control_text[34];
    request_fixture fixtures[] = {
        {"short", "Roll with advantage.", "tara", "req-short", 0},
        {"long", long_text, "tara", "req-long", UINT32_C(1234567890)},
        {"escaped", escaped_text, "voice\\\"alias", "req\\\"escaped", 42},
        {"mixed-plain", mixed_plain_text, "tara", "req-mixed", 43},
        {"maximum", maximum_text, "tara", "req-maximum", UINT32_MAX},
        {"maximum-escaped", maximum_escaped_text, "tara", "req-controls", 7},
        {"maximum-controls", maximum_control_text, "tara", "req-controls", 8},
        {"identifiers", "Speak this line.", maximum_identifiers,
            maximum_identifiers, UINT32_MAX},
        {"controls", control_text, "tara", "req-control", 100},
    };
    request_fixture prepared_id_fixtures[] = {
        {"short", "Roll with advantage.", "tara", "req-short", 0},
        {"long", long_text, "tara", "req-long", UINT32_C(1234567890)},
        {"escaped-text", escaped_text, "voice\\\"alias", "req-escaped", 42},
        {"mixed-plain", mixed_plain_text, "tara", "req-mixed", 43},
        {"maximum", maximum_text, "tara", "req-maximum", UINT32_MAX},
        {"maximum-controls", maximum_control_text, "tara", "req-controls", 8},
        {"maximum-id", "Speak this line.", "tara", maximum_identifiers,
            UINT32_MAX},
    };
    request_fixture prepared_text_fixtures[] = {
        {"short", "Roll with advantage.", "tara", "req-short", 0},
        {"empty-voice", "Roll with advantage.", "", "req-empty-voice", 1},
        {"long", long_text, "tara", "req-long", UINT32_C(1234567890)},
        {"maximum", maximum_text, "tara", "req-maximum", UINT32_MAX},
        {"maximum-id", "Speak this line.", "tara", maximum_identifiers,
            UINT32_MAX},
    };
    request_fixture sanitized_proof_fixtures[] = {
        {"short", "Roll with advantage.", "tara", "req-short", 0},
        {"long", long_text, "tara", "req-long", UINT32_C(1234567890)},
        {"maximum", maximum_text, "tara", "req-maximum", UINT32_MAX},
        {"quoted", "Say \"roll\" now.", "tara", "req-quoted", 7},
        {"slash", "Follow C:\\ward home.", "tara", "req-slash", 8},
        {"tagged", "Wait <laugh> then roll.", "tara", "req-tagged", 9},
        {"unicode", "Meet at caf\xc3\xa9.", "tara", "req-unicode", 10},
    };
    static const dispatch_fixture dispatch_fixtures[] = {
        {
            "req-warm-dispatch-000000000000",
            sizeof("req-warm-dispatch-000000000000") - 1u,
            "ai.turn.events.req-warm-dispatch-000000000000",
            sizeof("ai.turn.events.req-warm-dispatch-000000000000") - 1u,
        },
        {
            "req-warm-dispatch-111111111111",
            sizeof("req-warm-dispatch-111111111111") - 1u,
            "ai.turn.events.req-warm-dispatch-111111111111",
            sizeof("ai.turn.events.req-warm-dispatch-111111111111") - 1u,
        },
        {
            "req-warm-dispatch-222222222222",
            sizeof("req-warm-dispatch-222222222222") - 1u,
            "ai.turn.events.req-warm-dispatch-222222222222",
            sizeof("ai.turn.events.req-warm-dispatch-222222222222") - 1u,
        },
        {
            "req-warm-dispatch-333333333333",
            sizeof("req-warm-dispatch-333333333333") - 1u,
            "ai.turn.events.req-warm-dispatch-333333333333",
            sizeof("ai.turn.events.req-warm-dispatch-333333333333") - 1u,
        },
    };
    bench_result prior;
    bench_result current;
    const size_t fixture_count = sizeof(fixtures) / sizeof(fixtures[0]);
    const size_t prepared_id_fixture_count =
        sizeof(prepared_id_fixtures) / sizeof(prepared_id_fixtures[0]);
    const size_t prepared_text_fixture_count =
        sizeof(prepared_text_fixtures) / sizeof(prepared_text_fixtures[0]);
    const size_t sanitized_proof_fixture_count =
        sizeof(sanitized_proof_fixtures) /
        sizeof(sanitized_proof_fixtures[0]);
    const size_t dispatch_fixture_count =
        sizeof(dispatch_fixtures) / sizeof(dispatch_fixtures[0]);
    size_t i;
    request_builder instruction_builder = NULL;
    int focused_native = 0;
    memset(maximum_text, 'A', sizeof(maximum_text) - 1u);
    maximum_text[sizeof(maximum_text) - 1u] = '\0';
    memset(
        maximum_escaped_text,
        '"',
        sizeof(maximum_escaped_text) - 1u);
    maximum_escaped_text[sizeof(maximum_escaped_text) - 1u] = '\0';
    memset(
        maximum_control_text,
        1,
        sizeof(maximum_control_text) - 1u);
    maximum_control_text[sizeof(maximum_control_text) - 1u] = '\0';
    memset(
        maximum_identifiers,
        'A',
        sizeof(maximum_identifiers) - 1u);
    maximum_identifiers[sizeof(maximum_identifiers) - 1u] = '\0';
    for (i = 0; i < 31u; ++i) control_text[i] = (char)(i + 1u);
    control_text[31] = '"';
    control_text[32] = '\\';
    control_text[33] = '\0';
    if (argc == 2 && strcmp(argv[1], "--sanitized-proof-separate") == 0) {
        instruction_builder = separate_sanitized_build_tts_request_json;
    } else if (argc == 2 && strcmp(argv[1], "--sanitized-proof-proven") == 0) {
        instruction_builder = proven_sanitized_build_tts_request_json;
    } else if (argc == 2 && strcmp(argv[1], "--sanitized-proof") == 0) {
        focused_native = 1;
    } else if (argc > 2 ||
               (argc == 2 && strcmp(argv[1], "--verify") != 0)) {
        fprintf(
            stderr,
            "usage: %s [--verify|--sanitized-proof|"
            "--sanitized-proof-separate|"
            "--sanitized-proof-proven]\n",
            argv[0]);
        return 2;
    }
    if (instruction_builder) {
        uint64_t checksum;
        unsigned char separate[OUTPUT_CAPACITY];
        unsigned char proven[OUTPUT_CAPACITY];
        for (i = 0u; i < 2u; ++i) {
            size_t separate_len = separate_sanitized_build_tts_request_json(
                (char *)separate,
                sizeof(separate),
                sanitized_proof_fixtures[i].text,
                sanitized_proof_fixtures[i].voice,
                sanitized_proof_fixtures[i].turn_id,
                sanitized_proof_fixtures[i].query_hash);
            size_t proven_len = proven_sanitized_build_tts_request_json(
                (char *)proven,
                sizeof(proven),
                sanitized_proof_fixtures[i].text,
                sanitized_proof_fixtures[i].voice,
                sanitized_proof_fixtures[i].turn_id,
                sanitized_proof_fixtures[i].query_hash);
            if (separate_len == 0u || proven_len != separate_len ||
                memcmp(separate, proven, separate_len + 1u) != 0) {
                fputs("TTS sanitized proof instruction differential failed\n", stderr);
                return 1;
            }
        }
        checksum = run_instruction_builder(
            instruction_builder, sanitized_proof_fixtures, 2u);
        if (checksum == 0u) return 1;
        printf(
            "BenchmarkTTSSanitizedTextProofInstructions mode=%s "
            "rounds=%d checksum=%llu\n",
            instruction_builder == separate_sanitized_build_tts_request_json ?
                "separate" : "proven",
            INSTRUCTION_ROUNDS,
            (unsigned long long)checksum);
        return 0;
    }
    if (focused_native) {
        if (verify_sanitized_proof_builders(
                sanitized_proof_fixtures,
                sanitized_proof_fixture_count) != 0 ||
            benchmark_builders(
                separate_sanitized_build_tts_request_json,
                proven_sanitized_build_tts_request_json,
                sanitized_proof_fixtures,
                3u,
                &prior,
                &current) != 0) {
            fputs("TTS sanitized proof benchmark failed\n", stderr);
            return 1;
        }
        print_sanitized_proof_benchmark("plain", 3u, &prior, &current);
        if (benchmark_builders(
                separate_sanitized_build_tts_request_json,
                proven_sanitized_build_tts_request_json,
                sanitized_proof_fixtures + 3u,
                sanitized_proof_fixture_count - 3u,
                &prior,
                &current) != 0) {
            fputs("TTS sanitized fallback proof benchmark failed\n", stderr);
            return 1;
        }
        print_sanitized_proof_benchmark(
            "fallback",
            sanitized_proof_fixture_count - 3u,
            &prior,
            &current);
        return 0;
    }
    if (verify_builders(fixtures, fixture_count) != 0 ||
        verify_prepared_id_builders(
            prepared_id_fixtures, prepared_id_fixture_count) != 0 ||
        verify_prepared_text_id_builders(
            prepared_text_fixtures, prepared_text_fixture_count) != 0 ||
        verify_sanitized_proof_builders(
            sanitized_proof_fixtures, sanitized_proof_fixture_count) != 0 ||
        verify_request_preparation() != 0 ||
        verify_dispatch_metadata(
            dispatch_fixtures, dispatch_fixture_count) != 0) {
        fputs("TTS request writer differential failed\n", stderr);
        return 1;
    }
    if (argc == 2) {
        puts("TTS request writer differential: PASS");
        return 0;
    }
    for (i = 0; i < fixture_count; ++i) {
        if (benchmark_builders(
                prior_build_tts_request_json,
                current_build_tts_request_json,
                &fixtures[i],
                1u,
                &prior,
                &current) != 0) {
            fputs("TTS request writer benchmark failed\n", stderr);
            return 1;
        }
        print_benchmark(fixtures[i].name, 1u, &prior, &current);
    }
    if (benchmark_builders(
            prior_build_tts_request_json,
            current_build_tts_request_json,
            fixtures, fixture_count, &prior, &current) != 0) {
        fputs("TTS request writer aggregate benchmark failed\n", stderr);
        return 1;
    }
    print_benchmark("aggregate", fixture_count, &prior, &current);
    if (benchmark_builders(
            current_build_tts_request_json,
            prepared_id_build_tts_request_json,
            prepared_id_fixtures,
            prepared_id_fixture_count,
            &prior,
            &current) != 0) {
        fputs("TTS prepared ID benchmark failed\n", stderr);
        return 1;
    }
    print_prepared_id_benchmark(
        prepared_id_fixture_count, &prior, &current);
    if (benchmark_builders(
            prepared_id_build_tts_request_json,
            prepared_text_id_build_tts_request_json,
            prepared_text_fixtures,
            prepared_text_fixture_count,
            &prior,
            &current) != 0) {
        fputs("TTS prepared text benchmark failed\n", stderr);
        return 1;
    }
    print_prepared_text_benchmark(
        prepared_text_fixture_count, &prior, &current);
    if (benchmark_builders(
            separate_sanitized_build_tts_request_json,
            proven_sanitized_build_tts_request_json,
            sanitized_proof_fixtures,
            3u,
            &prior,
            &current) != 0) {
        fputs("TTS sanitized plain proof benchmark failed\n", stderr);
        return 1;
    }
    print_sanitized_proof_benchmark("plain", 3u, &prior, &current);
    if (benchmark_builders(
            separate_sanitized_build_tts_request_json,
            proven_sanitized_build_tts_request_json,
            sanitized_proof_fixtures + 3u,
            sanitized_proof_fixture_count - 3u,
            &prior,
            &current) != 0) {
        fputs("TTS sanitized fallback proof benchmark failed\n", stderr);
        return 1;
    }
    print_sanitized_proof_benchmark(
        "fallback", sanitized_proof_fixture_count - 3u, &prior, &current);
    if (benchmark_dispatch_metadata(
            prior_dispatch_metadata,
            current_dispatch_metadata,
            dispatch_fixtures,
            dispatch_fixture_count,
            &prior,
            &current) != 0) {
        fputs("TTS dispatch metadata benchmark failed\n", stderr);
        return 1;
    }
    print_dispatch_benchmark(dispatch_fixture_count, &prior, &current);
    if (benchmark_dispatch_metadata(
            separate_request_preparation,
            combined_request_preparation,
            dispatch_fixtures,
            dispatch_fixture_count,
            &prior,
            &current) != 0) {
        fputs("TTS request preparation benchmark failed\n", stderr);
        return 1;
    }
    print_request_preparation_benchmark(
        dispatch_fixture_count, &prior, &current);
    return 0;
}
