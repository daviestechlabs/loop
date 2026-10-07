/* turn_budget.c — pure-C turn budget / deadline extraction. */

#include "turn_budget.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

static int parse_i64(const char *s, int64_t *out) {
    const char *p;
    int64_t v = 0;
    int any = 0;
    if (!s || !out) {
        return -1;
    }
    p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '+') {
        p++;
    }
    if (*p == '-') {
        return -1; /* budgets/deadlines must be positive */
    }
    while (*p >= '0' && *p <= '9') {
        int d = *p - '0';
        if (v > (INT64_C(9223372036854775807) - d) / 10) {
            return -1;
        }
        v = v * 10 + d;
        any = 1;
        p++;
    }
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!any || *p != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static int key_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

int turn_budget_extract_v1(
    const char *const *keys,
    const char *const *vals,
    size_t n,
    int64_t now_unix_ms,
    turn_budget_info_v1 *out
) {
    size_t i;
    int64_t budget_ms = 0;
    int64_t deadline_unix_ms = 0;
    int have_budget = 0;
    int have_deadline = 0;
    int inv_budget = 0;
    int inv_deadline = 0;

    if (!out) {
        return TURN_BUDGET_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (n > 0 && (!keys || !vals)) {
        return TURN_BUDGET_ERR_ARGUMENT;
    }

    for (i = 0; i < n; i++) {
        const char *k = keys[i];
        const char *v = vals[i];
        int64_t parsed = 0;
        if (!k || !v || v[0] == '\0') {
            continue;
        }
        if (key_eq(k, TURN_BUDGET_KEY_MS)) {
            if (parse_i64(v, &parsed) != 0 || parsed <= 0) {
                inv_budget = 1;
            } else {
                budget_ms = parsed;
                have_budget = 1;
            }
        } else if (key_eq(k, TURN_BUDGET_KEY_DEADLINE_UNIX_MS)) {
            if (parse_i64(v, &parsed) != 0 || parsed <= 0) {
                inv_deadline = 1;
            } else {
                deadline_unix_ms = parsed;
                have_deadline = 1;
            }
        }
    }

    out->invalid_budget_ms = inv_budget;
    out->invalid_deadline_unix_ms = inv_deadline;

    if (have_budget) {
        out->budget_ms = budget_ms;
        if (now_unix_ms > 0) {
            /* saturate add */
            if (budget_ms > INT64_C(9223372036854775807) - now_unix_ms) {
                out->effective_deadline_unix_ms = INT64_C(9223372036854775807);
            } else {
                out->effective_deadline_unix_ms = now_unix_ms + budget_ms;
            }
            out->source = TURN_BUDGET_SRC_BUDGET_MS;
            out->has_deadline = 1;
        }
    }
    if (have_deadline) {
        out->deadline_unix_ms = deadline_unix_ms;
        if (!out->has_deadline || deadline_unix_ms < out->effective_deadline_unix_ms) {
            out->effective_deadline_unix_ms = deadline_unix_ms;
            out->source = TURN_BUDGET_SRC_DEADLINE_UNIX_MS;
            out->has_deadline = 1;
        }
    }
    return TURN_BUDGET_OK;
}

int turn_budget_extract_kv_v1(
    const char *key,
    const char *val,
    int64_t now_unix_ms,
    turn_budget_info_v1 *out
) {
    if (!key) {
        return turn_budget_extract_v1(NULL, NULL, 0, now_unix_ms, out);
    }
    {
        const char *keys[1] = {key};
        const char *vals[1] = {val ? val : ""};
        return turn_budget_extract_v1(keys, vals, 1, now_unix_ms, out);
    }
}

int turn_budget_parse_ms_v1(const char *value, int64_t *budget_ms) {
    int64_t parsed;
    if (!budget_ms) return TURN_BUDGET_ERR_ARGUMENT;
    *budget_ms = 0;
    if (parse_i64(value, &parsed) != 0 || parsed <= 0)
        return TURN_BUDGET_ERR_ARGUMENT;
    *budget_ms = parsed;
    return TURN_BUDGET_OK;
}

int turn_budget_apply_default_ms_v1(
    turn_budget_info_v1 *info,
    int64_t budget_ms,
    int64_t now_unix_ms
) {
    if (!info || budget_ms <= 0 || now_unix_ms <= 0)
        return TURN_BUDGET_ERR_ARGUMENT;
    if (info->has_deadline) return TURN_BUDGET_OK;
    info->budget_ms = budget_ms;
    if (budget_ms > INT64_MAX - now_unix_ms)
        info->effective_deadline_unix_ms = INT64_MAX;
    else
        info->effective_deadline_unix_ms = now_unix_ms + budget_ms;
    info->source = TURN_BUDGET_SRC_BUDGET_MS;
    info->has_deadline = 1;
    return TURN_BUDGET_OK;
}

int turn_budget_apply_parent_v1(
    turn_budget_info_v1 *info,
    int64_t parent_deadline_unix_ms
) {
    if (!info) {
        return TURN_BUDGET_ERR_ARGUMENT;
    }
    if (parent_deadline_unix_ms <= 0) {
        return TURN_BUDGET_OK;
    }
    if (!info->has_deadline || parent_deadline_unix_ms < info->effective_deadline_unix_ms) {
        info->effective_deadline_unix_ms = parent_deadline_unix_ms;
        info->source = TURN_BUDGET_SRC_PARENT;
        info->has_deadline = 1;
    }
    return TURN_BUDGET_OK;
}

int64_t turn_budget_remaining_ms_v1(
    const turn_budget_info_v1 *info,
    int64_t now_unix_ms
) {
    if (!info || !info->has_deadline || now_unix_ms <= 0) {
        return -1;
    }
    if (info->effective_deadline_unix_ms <= now_unix_ms) {
        return 0;
    }
    return info->effective_deadline_unix_ms - now_unix_ms;
}
