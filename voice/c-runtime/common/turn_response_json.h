/* Bounded public JSON encoding for the authenticated HTTP gateway. */
#ifndef VOICE_C_TURN_RESPONSE_JSON_H
#define VOICE_C_TURN_RESPONSE_JSON_H

#include "turn_response.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURN_ACCEPTANCE_JSON_CAPACITY 525u
#define TURN_RESPONSE_JSON_CAPACITY 32768u

typedef struct {
    uint64_t auth_us;
    uint64_t vbus_publish_us;
    uint64_t prepare_us;
    uint64_t admission_us;
    uint64_t capability_us;
    uint64_t encode_us;
} turn_acceptance_metrics;

/* Encode one accepted event for a validated request identifier. */
size_t turn_acceptance_json_encode(
    char *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    int64_t accepted_at,
    const turn_acceptance_metrics *metrics
);

/* Encode one filtered public event. Return zero when validation or capacity fails. */
size_t turn_response_json_encode(
    char *out,
    size_t out_cap,
    const turn_response_event *event,
    int64_t timestamp_ms
);

#ifdef __cplusplus
}
#endif

#endif
