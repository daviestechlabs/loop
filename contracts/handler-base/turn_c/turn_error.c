/* turn_error.c — pure-C turn error classification. */

#include "turn_error.h"

#include <stdio.h>
#include <string.h>

static int contains_ci(const char *hay, const char *needle) {
    size_t nlen, hlen, i, j;
    if (!hay || !needle || !needle[0]) {
        return 0;
    }
    nlen = strlen(needle);
    hlen = strlen(hay);
    if (nlen > hlen) {
        return 0;
    }
    for (i = 0; i + nlen <= hlen; i++) {
        for (j = 0; j < nlen; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

int turn_err_from_code_v1(turn_err_code_v1 code, turn_err_class_v1 *out) {
    if (!out) {
        return TURN_ERR_ARGUMENT;
    }
    out->code = code;
    out->retryable = 0;
    switch (code) {
    case TURN_ERR_CODE_VALIDATION:
        out->class_str = TURN_ERR_CLASS_VALIDATION;
        break;
    case TURN_ERR_CODE_DEADLINE:
        out->class_str = TURN_ERR_CLASS_DEADLINE;
        break;
    case TURN_ERR_CODE_CANCELED:
        out->class_str = TURN_ERR_CLASS_CANCELED;
        break;
    case TURN_ERR_CODE_NOT_FOUND:
        out->class_str = TURN_ERR_CLASS_NOT_FOUND;
        break;
    case TURN_ERR_CODE_DOWNSTREAM:
        out->class_str = TURN_ERR_CLASS_DOWNSTREAM;
        out->retryable = 1;
        break;
    case TURN_ERR_CODE_TRANSIENT:
        out->class_str = TURN_ERR_CLASS_TRANSIENT;
        out->retryable = 1;
        break;
    case TURN_ERR_CODE_NONE:
        out->class_str = "";
        out->code = TURN_ERR_CODE_NONE;
        break;
    case TURN_ERR_CODE_INTERNAL:
    default:
        out->code = TURN_ERR_CODE_INTERNAL;
        out->class_str = TURN_ERR_CLASS_INTERNAL;
        break;
    }
    return TURN_ERR_OK;
}

int turn_err_classify_msg_v1(const char *msg, turn_err_class_v1 *out) {
    if (!out) {
        return TURN_ERR_ARGUMENT;
    }
    if (!msg || !msg[0]) {
        return turn_err_from_code_v1(TURN_ERR_CODE_NONE, out);
    }
    if (contains_ci(msg, "deadline exceeded") || contains_ci(msg, "context deadline") ||
        contains_ci(msg, "etimedout") || contains_ci(msg, "timeout")) {
        /* timeout-style: treat as deadline; retryable only for transport timeout wording */
        turn_err_from_code_v1(TURN_ERR_CODE_DEADLINE, out);
        if (contains_ci(msg, "timeout") && !contains_ci(msg, "deadline exceeded")) {
            out->retryable = 1;
        }
        return TURN_ERR_OK;
    }
    if (contains_ci(msg, "context canceled") || contains_ci(msg, "cancelled") ||
        contains_ci(msg, "canceled")) {
        return turn_err_from_code_v1(TURN_ERR_CODE_CANCELED, out);
    }
    if (contains_ci(msg, "not found") || contains_ci(msg, "not_found")) {
        return turn_err_from_code_v1(TURN_ERR_CODE_NOT_FOUND, out);
    }
    if (contains_ci(msg, "unavailable") || contains_ci(msg, "connection refused") ||
        contains_ci(msg, "downstream")) {
        return turn_err_from_code_v1(TURN_ERR_CODE_DOWNSTREAM, out);
    }
    if (contains_ci(msg, "temporary") || contains_ci(msg, "try again") ||
        contains_ci(msg, "unavailable for legal")) {
        return turn_err_from_code_v1(TURN_ERR_CODE_TRANSIENT, out);
    }
    if (contains_ci(msg, "validation") || contains_ci(msg, "invalid") ||
        contains_ci(msg, "missing ") || contains_ci(msg, "bad request")) {
        return turn_err_from_code_v1(TURN_ERR_CODE_VALIDATION, out);
    }
    return turn_err_from_code_v1(TURN_ERR_CODE_INTERNAL, out);
}

static void json_escape_append(char *out, size_t cap, size_t *pos, const char *s) {
    size_t i;
    if (!s) {
        return;
    }
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (*pos + 2 >= cap) {
            return;
        }
        if (c == '"' || c == '\\') {
            out[(*pos)++] = '\\';
            out[(*pos)++] = c;
        } else if ((unsigned char)c < 0x20) {
            /* drop control chars */
        } else {
            out[(*pos)++] = c;
        }
    }
}

int turn_err_json_v1(
    const turn_err_class_v1 *cls,
    const char *message,
    char *out,
    size_t out_cap
) {
    size_t pos = 0;
    const char *type;
    if (!out || out_cap == 0) {
        return TURN_ERR_ARGUMENT;
    }
    out[0] = '\0';
    type = (cls && cls->class_str && cls->class_str[0]) ? cls->class_str : TURN_ERR_CLASS_INTERNAL;
    if (out_cap < 32) {
        return TURN_ERR_ARGUMENT;
    }
    /* {"error":true,"message":"...","type":"..."} */
    pos = 0;
    {
        int n = snprintf(out, out_cap, "{\"error\":true,\"message\":\"");
        if (n < 0 || (size_t)n >= out_cap) {
            return TURN_ERR_ARGUMENT;
        }
        pos = (size_t)n;
    }
    json_escape_append(out, out_cap, &pos, message ? message : "");
    if (pos + 16 >= out_cap) {
        out[out_cap - 1] = '\0';
        return TURN_ERR_ARGUMENT;
    }
    out[pos++] = '"';
    out[pos++] = ',';
    {
        int n = snprintf(out + pos, out_cap - pos, "\"type\":\"%s\"}", type);
        if (n < 0 || (size_t)n >= out_cap - pos) {
            out[out_cap - 1] = '\0';
            return TURN_ERR_ARGUMENT;
        }
        pos += (size_t)n;
    }
    out[pos] = '\0';
    (void)pos;
    return TURN_ERR_OK;
}
