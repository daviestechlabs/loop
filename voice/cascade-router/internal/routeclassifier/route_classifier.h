#ifndef ROUTE_CLASSIFIER_H
#define ROUTE_CLASSIFIER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum route_classifier_route_v1 {
    ROUTE_CLASSIFIER_ANSWER_V1 = 0,
    ROUTE_CLASSIFIER_ESCALATE_V1 = 1,
    ROUTE_CLASSIFIER_RETRIEVE_THEN_ESCALATE_V1 = 2
} route_classifier_route_v1;

enum route_classifier_policy_v1 {
    ROUTE_CLASSIFIER_POLICY_TOOL_V1 = 1u << 0,
    ROUTE_CLASSIFIER_POLICY_ANSWER_V1 = 1u << 1,
    ROUTE_CLASSIFIER_POLICY_GENERATIVE_V1 = 1u << 2,
    ROUTE_CLASSIFIER_POLICY_RETRIEVAL_MARKER_V1 = 1u << 3,
    ROUTE_CLASSIFIER_POLICY_RETRIEVAL_QUESTION_V1 = 1u << 4,
    ROUTE_CLASSIFIER_POLICY_EMPTY_V1 = 1u << 5
};

/*
 * Classify a UTF-8 byte span into one packed allocation-free result: route
 * bits 0..1, policy flags bits 8..15, and IEEE-754 confidence bits 32..63.
 * Tokenization is deliberately ASCII and stable: the voice/STT routing
 * vocabulary is English, while unknown Unicode bytes behave as separators.
 * UINT64_MAX reports an invalid pointer/length pair.
 */
uint64_t route_classifier_packed_v1(const char *text, size_t text_len);

#ifdef __cplusplus
}
#endif

#endif
