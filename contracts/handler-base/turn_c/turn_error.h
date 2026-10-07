/* turn_error.h — pure-C stable turn error classes (no heap, no Go).
 *
 * Class strings match contracts/handler-base/turn/errors.go ErrorClass values
 * used in metrics and ErrorResponse.type.
 */
#ifndef HANDLER_BASE_TURN_ERROR_H
#define HANDLER_BASE_TURN_ERROR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TURN_ERR_OK = 0,
    TURN_ERR_ARGUMENT = 1
};

/* Stable platform classes (NUL-terminated string constants). */
#define TURN_ERR_CLASS_VALIDATION "validation_error"
#define TURN_ERR_CLASS_DEADLINE "deadline_exceeded"
#define TURN_ERR_CLASS_CANCELED "canceled"
#define TURN_ERR_CLASS_NOT_FOUND "not_found"
#define TURN_ERR_CLASS_DOWNSTREAM "downstream_unavailable"
#define TURN_ERR_CLASS_TRANSIENT "transient"
#define TURN_ERR_CLASS_INTERNAL "internal"

typedef enum turn_err_code_v1 {
    TURN_ERR_CODE_NONE = 0,
    TURN_ERR_CODE_VALIDATION = 1,
    TURN_ERR_CODE_DEADLINE = 2,
    TURN_ERR_CODE_CANCELED = 3,
    TURN_ERR_CODE_NOT_FOUND = 4,
    TURN_ERR_CODE_DOWNSTREAM = 5,
    TURN_ERR_CODE_TRANSIENT = 6,
    TURN_ERR_CODE_INTERNAL = 7
} turn_err_code_v1;

typedef struct turn_err_class_v1 {
    turn_err_code_v1 code;
    const char *class_str; /* static lifetime */
    int retryable;         /* 1/0 */
} turn_err_class_v1;

/* Map explicit class code → class string + default retryability. */
int turn_err_from_code_v1(turn_err_code_v1 code, turn_err_class_v1 *out);

/*
 * Classify from a free-form message / errno-style token (case-sensitive tokens).
 * Recognizes: context deadline exceeded, context canceled, timeout, temporary,
 * not found, unavailable, validation / invalid / missing, else internal.
 */
int turn_err_classify_msg_v1(const char *msg, turn_err_class_v1 *out);

/*
 * Format a minimal JSON error envelope (no heap):
 *   {"error":true,"message":"...","type":"validation_error"}
 * Returns 0 on success; out always NUL-terminated when out_cap > 0.
 */
int turn_err_json_v1(
    const turn_err_class_v1 *cls,
    const char *message,
    char *out,
    size_t out_cap
);

#ifdef __cplusplus
}
#endif

#endif /* HANDLER_BASE_TURN_ERROR_H */
