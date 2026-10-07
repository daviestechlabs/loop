#ifndef SYSTEMONE_H
#define SYSTEMONE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lab-only Laya SystemOne client for cascade-router.
 * Empty SYSTEMONE_BASE_URL (and unset JEV_BASE_URL alias) => no-op skip.
 * Path is exactly "/v1/systemone". Companions/agent-workers are phase 2.
 */

enum {
    SYSTEMONE_SHORTLIST_K_DEFAULT = 20,
    SYSTEMONE_BASE_URL_CAP = 256,
    SYSTEMONE_PATH_CAP = 32,
    SYSTEMONE_ROUTE_CHOICE_CAP = 48,
    SYSTEMONE_DEFAULT_TIMEOUT_MS = 800,
    SYSTEMONE_RESPONSE_BODY_CAP = 8192,
    SYSTEMONE_REQUEST_BODY_CAP = 4096
};

typedef enum systemone_status_v1 {
    SYSTEMONE_STATUS_SKIPPED_UNSET_V1 = 0,
    SYSTEMONE_STATUS_READY_V1 = 1,
    SYSTEMONE_STATUS_REJECT_LOW_ENTROPY_CONFIDENCE_V1 = 2,
    SYSTEMONE_STATUS_ERROR_V1 = 3
} systemone_status_v1;

typedef struct systemone_config_v1 {
    char base_url[SYSTEMONE_BASE_URL_CAP];
    int enabled;
    int min_entropy_confidence_set;
    float min_entropy_confidence;
    int shortlist_k;
} systemone_config_v1;

typedef struct systemone_decision_v1 {
    char route_choice[SYSTEMONE_ROUTE_CHOICE_CAP];
    float route_confidence;
    float score;
    float noul;
    /* Soft route probs when response includes answers.route.probabilities. */
    float route_prob_answer;
    float route_prob_escalate;
    float route_prob_retrieve_then_escalate;
    int has_route;
    int has_route_confidence;
    int has_score;
    int has_noul;
    int has_route_probabilities; /* 1 only when parsed from wire; never invented */
} systemone_decision_v1;

const char *systemone_path_v1(void);
void systemone_config_from_env_v1(systemone_config_v1 *out);
int systemone_is_enabled_v1(const systemone_config_v1 *cfg);
systemone_status_v1 systemone_build_post_url_v1(const systemone_config_v1 *cfg,
                                                char *out_url,
                                                size_t out_cap);
size_t systemone_shortlist_topk_v1(const systemone_config_v1 *cfg,
                                   const uint32_t *ranked_in,
                                   size_t ranked_len,
                                   uint32_t *ranked_out,
                                   size_t ranked_out_cap);
systemone_status_v1 systemone_entropy_gate_v1(const systemone_config_v1 *cfg,
                                              float entropy_confidence);
systemone_status_v1 systemone_lab_request_stub_v1(const systemone_config_v1 *cfg,
                                                  char *out_url,
                                                  size_t out_cap);

/* Live POST {base}/v1/systemone (model=laya-typed-decisions). Unset => skip. */
systemone_status_v1 systemone_lab_request_v1(const systemone_config_v1 *cfg,
                                             const char *state_text,
                                             size_t state_len,
                                             systemone_decision_v1 *out_decision,
                                             int timeout_ms);
systemone_status_v1 systemone_parse_response_v1(const char *json,
                                                size_t json_len,
                                                systemone_decision_v1 *out_decision);
/* Enum numerics match route_classifier_route_v1: 1=escalate, 2=retrieve_then_escalate. */
systemone_status_v1 systemone_map_to_cascade_route_v1(
    const systemone_config_v1 *cfg,
    const systemone_decision_v1 *decision,
    const char **out_route_name,
    size_t *out_route_name_len,
    uint32_t *out_route_enum);
/* Optional FT JSONL when SYSTEMONE_FT_JSONL_PATH is set (lab → RustFS prefix). */
void systemone_ft_emit_jsonl_v1(const systemone_config_v1 *cfg,
                                const char *request_id,
                                const char *state_text,
                                size_t state_len,
                                const systemone_decision_v1 *decision,
                                systemone_status_v1 status,
                                const char *c_route_name);

#ifdef __cplusplus
}
#endif

#endif
