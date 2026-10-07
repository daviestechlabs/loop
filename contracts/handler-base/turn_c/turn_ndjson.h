/* turn_ndjson.h — pure-C accepted/error NDJSON control envelopes (turnstream.v1alpha1). */
#ifndef HANDLER_BASE_TURN_NDJSON_H
#define HANDLER_BASE_TURN_NDJSON_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { TURN_NDJSON_OK = 0, TURN_NDJSON_ERR = 1 };
/* Writes one NDJSON line (including trailing newline) into out. */
int turn_ndjson_accepted_v1(
    const char *request_id,
    const char *event_subject,
    long long response_stream_id,
    long long accepted_at_ms,
    char *out,
    size_t out_cap
);
int turn_ndjson_error_v1(const char *message, char *out, size_t out_cap);
#ifdef __cplusplus
}
#endif
#endif
