#include "turn_ndjson.h"
#include <stdio.h>
#include <string.h>

static void esc(char *out, size_t cap, size_t *pos, const char *s) {
    size_t i;
    if (!s) return;
    for (i = 0; s[i] && *pos + 2 < cap; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { out[(*pos)++] = '\\'; out[(*pos)++] = c; }
        else if ((unsigned char)c >= 0x20) out[(*pos)++] = c;
    }
}

int turn_ndjson_accepted_v1(
    const char *request_id,
    const char *event_subject,
    long long response_stream_id,
    long long accepted_at_ms,
    char *out,
    size_t out_cap
) {
    size_t pos = 0;
    int n;
    if (!out || out_cap < 64) return TURN_NDJSON_ERR;
    n = snprintf(out, out_cap,
        "{\"type\":\"accepted\",\"protocol_version\":\"turnstream.v1alpha1\",\"request_id\":\"");
    if (n < 0 || (size_t)n >= out_cap) return TURN_NDJSON_ERR;
    pos = (size_t)n;
    esc(out, out_cap, &pos, request_id ? request_id : "");
    n = snprintf(out + pos, out_cap - pos, "\",\"event_subject\":\"");
    if (n < 0 || (size_t)n >= out_cap - pos) return TURN_NDJSON_ERR;
    pos += (size_t)n;
    esc(out, out_cap, &pos, event_subject ? event_subject : "");
    n = snprintf(out + pos, out_cap - pos,
        "\",\"response_stream_id\":%lld,\"accepted_at\":%lld}\n",
        response_stream_id, accepted_at_ms);
    if (n < 0 || (size_t)n >= out_cap - pos) return TURN_NDJSON_ERR;
    return TURN_NDJSON_OK;
}

int turn_ndjson_error_v1(const char *message, char *out, size_t out_cap) {
    size_t pos = 0;
    int n;
    if (!out || out_cap < 32) return TURN_NDJSON_ERR;
    n = snprintf(out, out_cap,
        "{\"type\":\"error\",\"protocol_version\":\"turnstream.v1alpha1\",\"error\":\"");
    if (n < 0 || (size_t)n >= out_cap) return TURN_NDJSON_ERR;
    pos = (size_t)n;
    esc(out, out_cap, &pos, message ? message : "");
    if (pos + 3 >= out_cap) return TURN_NDJSON_ERR;
    out[pos++] = '"'; out[pos++] = '}'; out[pos++] = '\n'; out[pos] = '\0';
    return TURN_NDJSON_OK;
}
