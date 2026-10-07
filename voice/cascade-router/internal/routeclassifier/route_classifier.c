#include "route_classifier.h"

#include <math.h>
#include <string.h>

#include "route_classifier_model_v1.h"

/* Headers produced before artifact materialization had no abstention threshold. */
#ifndef ROUTE_CLASSIFIER_ANSWER_THRESHOLD_V1
#define ROUTE_CLASSIFIER_ANSWER_THRESHOLD_V1 0.0f
#endif
#ifndef ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1
#define ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1 0u
#endif
#ifndef ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MIN_V1
#define ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MIN_V1 3u
#endif
#ifndef ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1
#define ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1 5u
#endif
#ifndef ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_WEIGHT_V1
#define ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_WEIGHT_V1 0.2f
#endif
#ifndef ROUTE_CLASSIFIER_SPARSE_CAP_V1
#define ROUTE_CLASSIFIER_SPARSE_CAP_V1 128u
#endif
#if ROUTE_CLASSIFIER_SPARSE_CAP_V1 > ROUTE_CLASSIFIER_FEATURE_DIM_V1
#error "sparse feature capacity cannot exceed feature dimension"
#endif
_Static_assert(sizeof(float) == sizeof(uint32_t), "packed ABI requires 32-bit float");
_Static_assert(
    ROUTE_CLASSIFIER_FEATURE_DIM_V1 <= UINT16_MAX,
    "feature buckets must fit the sparse uint16 index");
_Static_assert(ROUTE_CLASSIFIER_SPARSE_CAP_V1 > 0u, "sparse capacity must be positive");
_Static_assert(
    ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MIN_V1 <=
        ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1,
    "character n-gram bounds are invalid");
_Static_assert(
    ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1 <= 9u,
    "character n-gram hash prefix supports one digit");

static unsigned char ascii_lower(unsigned char value) {
    return (unsigned char)(route_classifier_ascii_properties_v1[value] & UINT16_C(0xff));
}

static int token_byte(unsigned char value) {
    return (route_classifier_ascii_properties_v1[value] &
            ROUTE_CLASSIFIER_ASCII_TOKEN_V1) != 0u;
}

static uint32_t fnv1a_byte(uint32_t hash, unsigned char value) {
    return (hash ^ value) * UINT32_C(16777619);
}

static uint32_t hash_token(
    const char *prefix,
    const char *left,
    size_t left_len,
    const char *right,
    size_t right_len
) {
    uint32_t hash = UINT32_C(2166136261);
    size_t index;
    for (index = 0; prefix[index] != '\0'; ++index) {
        hash = fnv1a_byte(hash, (unsigned char)prefix[index]);
    }
    for (index = 0; index < left_len; ++index) {
        hash = fnv1a_byte(hash, ascii_lower((unsigned char)left[index]));
    }
    if (right != NULL) {
        hash = fnv1a_byte(hash, (unsigned char)'_');
        for (index = 0; index < right_len; ++index) {
            hash = fnv1a_byte(hash, ascii_lower((unsigned char)right[index]));
        }
    }
    return hash;
}

#if ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1
static uint32_t hash_char_ngram(
    size_t ngram_size,
    const unsigned char *ngram
) {
    uint32_t hash = UINT32_C(2166136261);
    size_t index;
    hash = fnv1a_byte(hash, (unsigned char)'c');
    hash = fnv1a_byte(hash, (unsigned char)('0' + ngram_size));
    hash = fnv1a_byte(hash, (unsigned char)':');
    for (index = 0; index < ngram_size; ++index) {
        hash = fnv1a_byte(hash, ngram[index]);
    }
    return hash;
}
#endif

static uint32_t policy_prefix_flags(const char *text, size_t text_len) {
    size_t offset = 0;
    size_t index;
    uint16_t state = 0;
    uint32_t flags = 0;
    while (offset < text_len &&
           (text[offset] == ' ' || text[offset] == '\t' || text[offset] == '\r' ||
            text[offset] == '\n')) {
        ++offset;
    }
    for (index = offset; index < text_len; ++index) {
        uint8_t symbol =
            route_classifier_policy_symbols_v1[(unsigned char)text[index]];
        if (symbol == UINT8_MAX) {
            break;
        }
        state = route_classifier_policy_prefix_dfa_v1[state][(size_t)symbol];
        if (state == UINT16_MAX) {
            break;
        }
        flags |= (uint32_t)route_classifier_policy_prefix_exact_outputs_v1[state];
        if (index + 1 == text_len ||
            !token_byte((unsigned char)text[index + 1])) {
            flags |=
                (uint32_t)route_classifier_policy_prefix_boundary_outputs_v1[state];
        }
    }
    return flags;
}

static uint32_t policy_contains_flags(const char *text, size_t text_len) {
    uint16_t state = 0;
    uint32_t flags = 0;
    size_t index;
    for (index = 0; index < text_len; ++index) {
        uint8_t symbol =
            route_classifier_policy_symbols_v1[(unsigned char)text[index]];
        if (symbol == UINT8_MAX) {
            state = 0;
            continue;
        }
        state = route_classifier_policy_dfa_v1[state][(size_t)symbol];
        flags |= (uint32_t)route_classifier_policy_outputs_v1[state];
    }
    return flags;
}

static int policy_contains_retrieval_word(const char *text, size_t text_len) {
    uint8_t state = 0;
    size_t index;
    for (index = 0; index < text_len; ++index) {
        unsigned char value = ascii_lower((unsigned char)text[index]);
        if (!token_byte(value)) {
            if (state != UINT8_MAX &&
                route_classifier_policy_word_outputs_v1[state] != 0u) {
                return 1;
            }
            state = 0;
            continue;
        }
        if (state == UINT8_MAX) {
            continue;
        }
        if (value < (unsigned char)'a' || value > (unsigned char)'z') {
            state = UINT8_MAX;
            continue;
        }
        state = route_classifier_policy_word_dfa_v1[state]
                                                   [(size_t)(value - (unsigned char)'a')];
    }
    return state != UINT8_MAX && route_classifier_policy_word_outputs_v1[state] != 0u;
}

typedef struct route_classifier_result {
    uint32_t internal_version;
    uint32_t route;
    float logits[3];
    float confidence;
    uint32_t feature_count;
    uint32_t policy_flags;
} route_classifier_result;

static void policy_result(
    route_classifier_result *result,
    uint32_t route,
    uint32_t flags
) {
    result->route = route;
    result->logits[0] = 0.0f;
    result->logits[1] = 0.0f;
    result->logits[2] = 0.0f;
    result->logits[route] = 16.0f;
    result->confidence = 1.0f;
    result->policy_flags = flags;
}

static int apply_policy(
    const char *text,
    size_t text_len,
    route_classifier_result *result
) {
    uint32_t prefix_flags;
    uint32_t contains_flags;
    prefix_flags = policy_prefix_flags(text, text_len);
    if ((prefix_flags & ROUTE_CLASSIFIER_POLICY_MATCH_TOOL_V1) != 0u) {
        policy_result(result, ROUTE_CLASSIFIER_ESCALATE_V1, ROUTE_CLASSIFIER_POLICY_TOOL_V1);
        return 1;
    }
    contains_flags = policy_contains_flags(text, text_len);
    if ((prefix_flags & ROUTE_CLASSIFIER_POLICY_MATCH_GENERATIVE_V1) != 0u ||
        (contains_flags & ROUTE_CLASSIFIER_POLICY_MATCH_GENERATIVE_V1) != 0u) {
        policy_result(
            result,
            ROUTE_CLASSIFIER_ESCALATE_V1,
            ROUTE_CLASSIFIER_POLICY_GENERATIVE_V1);
        return 1;
    }
    if ((contains_flags & ROUTE_CLASSIFIER_POLICY_MATCH_RETRIEVAL_V1) != 0u) {
        policy_result(
            result,
            ROUTE_CLASSIFIER_RETRIEVE_THEN_ESCALATE_V1,
            ROUTE_CLASSIFIER_POLICY_RETRIEVAL_MARKER_V1);
        return 1;
    }
    if (policy_contains_retrieval_word(text, text_len)) {
        policy_result(
            result,
            ROUTE_CLASSIFIER_RETRIEVE_THEN_ESCALATE_V1,
            ROUTE_CLASSIFIER_POLICY_RETRIEVAL_QUESTION_V1);
        return 1;
    }
    if ((prefix_flags & ROUTE_CLASSIFIER_POLICY_MATCH_ANSWER_V1) != 0u ||
        (contains_flags & ROUTE_CLASSIFIER_POLICY_MATCH_ANSWER_V1) != 0u) {
        policy_result(result, ROUTE_CLASSIFIER_ANSWER_V1, ROUTE_CLASSIFIER_POLICY_ANSWER_V1);
        return 1;
    }
    return 0;
}

typedef struct route_sparse_features {
    float values[ROUTE_CLASSIFIER_FEATURE_DIM_V1];
    uint16_t touched[ROUTE_CLASSIFIER_SPARSE_CAP_V1];
    uint32_t n_touched;
    uint32_t feature_count;
    uint32_t token_count;
    int dense_fallback;
} route_sparse_features;

static void sparse_add(
    route_sparse_features *features,
    uint32_t bucket,
    float amount
) {
    if (features->values[bucket] == 0.0f) {
        if (features->n_touched < ROUTE_CLASSIFIER_SPARSE_CAP_V1) {
            features->touched[features->n_touched++] = (uint16_t)bucket;
        } else {
            features->dense_fallback = 1;
        }
    }
    features->values[bucket] += amount;
    ++features->feature_count;
}

#if ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1
typedef struct route_char_window {
    unsigned char bytes[ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1];
    size_t length;
} route_char_window;

static void char_window_add(
    route_char_window *window,
    unsigned char value,
    route_sparse_features *features
) {
    size_t ngram_size;
    if (window->length < ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1) {
        window->bytes[window->length++] = value;
    } else {
        memmove(
            window->bytes,
            window->bytes + 1,
            ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1 - 1u);
        window->bytes[ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1 - 1u] = value;
    }
    for (ngram_size = ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MIN_V1;
         ngram_size <= ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_MAX_V1;
         ++ngram_size) {
        uint32_t bucket;
        if (window->length < ngram_size) {
            continue;
        }
        bucket = hash_char_ngram(
                     ngram_size,
                     window->bytes + window->length - ngram_size) %
                 ROUTE_CLASSIFIER_FEATURE_DIM_V1;
        sparse_add(features, bucket, ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAM_WEIGHT_V1);
    }
}
#endif

static uint32_t build_features(
    const char *text,
    size_t text_len,
    route_sparse_features *features
) {
    const char *previous = NULL;
    size_t previous_len = 0;
    size_t position = 0;
    size_t index;
    double norm_squared = 0.0;
#if ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1
    route_char_window char_window = {{0}, 0};
#endif

    memset(features->values, 0, sizeof(features->values));
    features->n_touched = 0;
    features->feature_count = 0;
    features->token_count = 0;
    features->dense_fallback = 0;
#if ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1
    char_window_add(&char_window, (unsigned char)' ', features);
#endif

    while (position < text_len) {
        const char *token;
        size_t token_len;
        uint32_t bucket;
        while (position < text_len && !token_byte((unsigned char)text[position])) {
            ++position;
        }
        if (position == text_len) {
            break;
        }
        token = text + position;
        while (position < text_len && token_byte((unsigned char)text[position])) {
            ++position;
        }
        token_len = (size_t)((text + position) - token);
        ++features->token_count;
        bucket = hash_token("u:", token, token_len, NULL, 0) %
                 ROUTE_CLASSIFIER_FEATURE_DIM_V1;
        sparse_add(features, bucket, 1.0f);
        if (previous != NULL) {
            bucket = hash_token("b:", previous, previous_len, token, token_len) %
                     ROUTE_CLASSIFIER_FEATURE_DIM_V1;
            sparse_add(features, bucket, 0.5f);
#if ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1
            char_window_add(&char_window, (unsigned char)' ', features);
#endif
        }
#if ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1
        for (index = 0; index < token_len; ++index) {
            char_window_add(
                &char_window,
                ascii_lower((unsigned char)token[index]),
                features);
        }
#endif
        previous = token;
        previous_len = token_len;
    }
#if ROUTE_CLASSIFIER_FEATURE_CHAR_NGRAMS_V1
    char_window_add(&char_window, (unsigned char)' ', features);
#endif

    if (features->dense_fallback) {
        for (index = 0; index < ROUTE_CLASSIFIER_FEATURE_DIM_V1; ++index) {
            norm_squared += (double)features->values[index] * (double)features->values[index];
        }
        if (norm_squared > 0.0) {
            float inverse_norm = 1.0f / sqrtf((float)norm_squared);
            for (index = 0; index < ROUTE_CLASSIFIER_FEATURE_DIM_V1; ++index) {
                features->values[index] *= inverse_norm;
            }
        }
    } else {
        for (index = 0; index < features->n_touched; ++index) {
            float value = features->values[features->touched[index]];
            norm_squared += (double)value * (double)value;
        }
        if (norm_squared > 0.0) {
            float inverse_norm = 1.0f / sqrtf((float)norm_squared);
            for (index = 0; index < features->n_touched; ++index) {
                features->values[features->touched[index]] *= inverse_norm;
            }
        }
    }
    return features->feature_count;
}

static float sparse_dot(
    const route_sparse_features *features,
    const int16_t weights[ROUTE_CLASSIFIER_FEATURE_DIM_V1]
) {
    float logit = 0.0f;
    size_t index;
    if (features->dense_fallback) {
        for (index = 0; index < ROUTE_CLASSIFIER_FEATURE_DIM_V1; ++index) {
            logit +=
                ((float)weights[index] / ROUTE_CLASSIFIER_WEIGHT_SCALE_V1) *
                features->values[index];
        }
        return logit;
    }
    for (index = 0; index < features->n_touched; ++index) {
        uint32_t bucket = features->touched[index];
        logit +=
            ((float)weights[bucket] / ROUTE_CLASSIFIER_WEIGHT_SCALE_V1) *
            features->values[bucket];
    }
    return logit;
}

static int route_classifier_evaluate(
    const char *text,
    size_t text_len,
    route_classifier_result *result
) {
    route_sparse_features features;
    float probability_sum = 0.0f;
    float max_logit;
    uint32_t best_non_answer;
    size_t route;

    if (result == NULL || (text == NULL && text_len != 0)) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->internal_version = 1;
    if (text_len == 0) {
        policy_result(
            result,
            ROUTE_CLASSIFIER_ESCALATE_V1,
            ROUTE_CLASSIFIER_POLICY_EMPTY_V1);
        return 0;
    }
    if (apply_policy(text, text_len, result)) {
        return 0;
    }

    result->feature_count = build_features(text, text_len, &features);
    if (features.token_count == 0u) {
        policy_result(
            result,
            ROUTE_CLASSIFIER_ESCALATE_V1,
            ROUTE_CLASSIFIER_POLICY_EMPTY_V1);
        return 0;
    }
    for (route = 0; route < 3; ++route) {
        float logit =
            (float)route_classifier_bias_q_v1[route] / ROUTE_CLASSIFIER_WEIGHT_SCALE_V1;
        logit += sparse_dot(&features, route_classifier_weights_q_v1[route]);
        result->logits[route] = logit;
    }
    max_logit = result->logits[0];
    result->route = ROUTE_CLASSIFIER_ANSWER_V1;
    for (route = 1; route < 3; ++route) {
        if (result->logits[route] > max_logit) {
            max_logit = result->logits[route];
            result->route = (uint32_t)route;
        }
    }
    for (route = 0; route < 3; ++route) {
        probability_sum += expf(result->logits[route] - max_logit);
    }
    result->confidence = 1.0f / probability_sum;
    if (result->route == ROUTE_CLASSIFIER_ANSWER_V1 &&
        result->confidence < ROUTE_CLASSIFIER_ANSWER_THRESHOLD_V1) {
        best_non_answer = ROUTE_CLASSIFIER_ESCALATE_V1;
        if (result->logits[ROUTE_CLASSIFIER_RETRIEVE_THEN_ESCALATE_V1] >
            result->logits[best_non_answer]) {
            best_non_answer = ROUTE_CLASSIFIER_RETRIEVE_THEN_ESCALATE_V1;
        }
        result->route = best_non_answer;
        result->confidence =
            expf(result->logits[best_non_answer] - max_logit) / probability_sum;
    }
    return 0;
}

uint64_t route_classifier_packed_v1(const char *text, size_t text_len) {
    route_classifier_result result;
    uint32_t confidence_bits = 0;
    uint64_t packed;
    if (route_classifier_evaluate(text, text_len, &result) != 0) {
        return UINT64_MAX;
    }
    memcpy(&confidence_bits, &result.confidence, sizeof(confidence_bits));
    packed = (uint64_t)(result.route & UINT32_C(0x3));
    packed |= (uint64_t)(result.policy_flags & UINT32_C(0xff)) << 8;
    packed |= (uint64_t)confidence_bits << 32;
    return packed;
}
