/* turn_budget.h — pure-C ExtractBudget from turn metadata (no heap, no Go).
 *
 * Keys match messages.TurnBudgetMsMetadata / TurnDeadlineUnixMsMetadata:
 *   "turn_budget_ms"            positive int milliseconds from now
 *   "turn_deadline_unix_ms"     absolute unix epoch milliseconds
 * Effective deadline = earliest of (now+budget, absolute deadline).
 * Context/parent-deadline application stays in the host (Go handler or C service).
 */
#ifndef HANDLER_BASE_TURN_BUDGET_H
#define HANDLER_BASE_TURN_BUDGET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TURN_BUDGET_OK = 0,
    TURN_BUDGET_ERR_ARGUMENT = 1
};

enum {
    TURN_BUDGET_SRC_NONE = 0,
    TURN_BUDGET_SRC_BUDGET_MS = 1,
    TURN_BUDGET_SRC_DEADLINE_UNIX_MS = 2,
    TURN_BUDGET_SRC_PARENT = 3
};

#define TURN_BUDGET_KEY_MS "turn_budget_ms"
#define TURN_BUDGET_KEY_DEADLINE_UNIX_MS "turn_deadline_unix_ms"

typedef struct turn_budget_info_v1 {
    int64_t budget_ms;              /* 0 if unset/invalid */
    int64_t deadline_unix_ms;       /* 0 if unset/invalid */
    int64_t effective_deadline_unix_ms; /* 0 if none */
    int source;                     /* TURN_BUDGET_SRC_* */
    int has_deadline;               /* 1 if effective_deadline_unix_ms set */
    int invalid_budget_ms;          /* 1 if key present but unusable */
    int invalid_deadline_unix_ms;   /* 1 if key present but unusable */
} turn_budget_info_v1;

/*
 * Extract budget from a single metadata map encoded as parallel key/value arrays.
 * now_unix_ms is the reference clock (must be > 0 for budget_ms path).
 * Keys not listed are ignored. Missing keys leave fields zero.
 */
int turn_budget_extract_v1(
    const char *const *keys,
    const char *const *vals,
    size_t n,
    int64_t now_unix_ms,
    turn_budget_info_v1 *out
);

/*
 * Convenience: extract from one key and one value (or NULL value).
 * Equivalent to a 1-entry map when both non-NULL; empty map if key is NULL.
 */
int turn_budget_extract_kv_v1(
    const char *key,
    const char *val,
    int64_t now_unix_ms,
    turn_budget_info_v1 *out
);

/* Parse one positive millisecond duration for process configuration. */
int turn_budget_parse_ms_v1(const char *value, int64_t *budget_ms);

/* Apply a parsed process default only when metadata supplied no deadline. */
int turn_budget_apply_default_ms_v1(
    turn_budget_info_v1 *info,
    int64_t budget_ms,
    int64_t now_unix_ms
);

/*
 * Merge two budget sources: if parent_deadline_unix_ms > 0 and is earlier than
 * info->effective, take parent (source=PARENT, has_deadline=1). Does not set
 * "applied" — host decides whether to arm a timer.
 */
int turn_budget_apply_parent_v1(
    turn_budget_info_v1 *info,
    int64_t parent_deadline_unix_ms
);

/* Remaining ms until effective deadline; 0 if past; -1 if no deadline. */
int64_t turn_budget_remaining_ms_v1(
    const turn_budget_info_v1 *info,
    int64_t now_unix_ms
);

#ifdef __cplusplus
}
#endif

#endif /* HANDLER_BASE_TURN_BUDGET_H */
