/*
 * Pure-C conversation-orchestrator.
 * Subscribes: ai.turn.start, ai.turn.cancel
 * Accepts turns and sanitizes spoken text. The session gateway owns STT commits.
 * No Go runtime.
 */
#define _POSIX_C_SOURCE 200809L

#include "../common/service.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"

#include "speech_sanitize.h"
#include "turn_budget.h"
#include "turn_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    vbus_client *nc;
} orch_state;

static int64_t now_unix_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

/* Extract deadline from TurnStart metadata (and optional TURN_BUDGET_MS env default). */
static int extract_turn_budget(const turn_start_c *turn, turn_budget_info_v1 *info) {
    const char *keys[2];
    const char *vals[2];
    size_t n = 0;
    const char *env_budget;
    int64_t now = now_unix_ms();
    memset(info, 0, sizeof(*info));
    if (turn->has_meta_budget) {
        keys[n] = TURN_BUDGET_KEY_MS;
        vals[n] = turn->meta_budget_ms;
        n++;
    }
    if (turn->has_meta_deadline) {
        keys[n] = TURN_BUDGET_KEY_DEADLINE_UNIX_MS;
        vals[n] = turn->meta_deadline_unix_ms;
        n++;
    }
    if (n > 0) {
        if (turn_budget_extract_v1(keys, vals, n, now, info) != TURN_BUDGET_OK) return -1;
    }
    if (!info->has_deadline && (env_budget = getenv("TURN_BUDGET_MS")) != NULL && env_budget[0]) {
        if (turn_budget_extract_kv_v1(TURN_BUDGET_KEY_MS, env_budget, now, info) !=
            TURN_BUDGET_OK) return -1;
    }
    return info->invalid_budget_ms || info->invalid_deadline_unix_ms ? -1 : 0;
}

static void on_start(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    orch_state *st = (orch_state *)user;
    turn_start_c turn;
    turn_budget_info_v1 budget;
    turn_err_class_v1 errc;
    char clean[2048];
    size_t clean_len = 0;
    int64_t rem_ms;
    (void)subject;
    (void)reply;
    if (pb_decode_turn_start(data, data_len, &turn) != 0) {
        char ej[192];
        turn_err_from_code_v1(TURN_ERR_CODE_VALIDATION, &errc);
        turn_err_json_v1(&errc, "decode turn start failed", ej, sizeof(ej));
        svc_log("conversation-orchestrator", "reject %s", ej);
        return;
    }
    if (!turn.request_id[0]) {
        char ej[192];
        turn_err_from_code_v1(TURN_ERR_CODE_VALIDATION, &errc);
        turn_err_json_v1(&errc, "missing request_id", ej, sizeof(ej));
        svc_log("conversation-orchestrator", "reject %s", ej);
        return;
    }
    if (extract_turn_budget(&turn, &budget) != 0) {
        char ej[192];
        turn_err_from_code_v1(TURN_ERR_CODE_VALIDATION, &errc);
        turn_err_json_v1(&errc, "invalid turn budget metadata", ej, sizeof(ej));
        svc_log("conversation-orchestrator", "reject %s request=%s", ej, turn.request_id);
        return;
    }
    rem_ms = turn_budget_remaining_ms_v1(&budget, now_unix_ms());
    if (budget.has_deadline && rem_ms == 0) {
        char ej[192];
        turn_err_from_code_v1(TURN_ERR_CODE_DEADLINE, &errc);
        turn_err_json_v1(&errc, "turn deadline exceeded before accept", ej, sizeof(ej));
        svc_log("conversation-orchestrator", "reject %s request=%s", ej, turn.request_id);
        return;
    }
    if (turn.text[0] &&
        speech_sanitize_v1(turn.text, strlen(turn.text), 0, clean, sizeof(clean), &clean_len) ==
            SPEECH_SANITIZE_OK) {
        if (clean_len >= sizeof(clean)) clean_len = sizeof(clean) - 1;
        clean[clean_len] = '\0';
    }
    if (vbus_publish(st->nc, SUBJ_TURN_GENERATE, data, data_len) != 0) {
        svc_log(
            "conversation-orchestrator",
            "generate publish failed request=%s",
            turn.request_id);
        return;
    }
    if (budget.has_deadline) {
        svc_log(
            "conversation-orchestrator",
            "accepted turn request=%s session=%s text_len=%zu budget_rem_ms=%lld source=%d",
            turn.request_id,
            turn.session_id[0] ? turn.session_id : "?",
            clean_len ? clean_len : strlen(turn.text),
            (long long)rem_ms,
            budget.source
        );
    } else {
        svc_log(
            "conversation-orchestrator",
            "accepted turn request=%s session=%s text_len=%zu",
            turn.request_id,
            turn.session_id[0] ? turn.session_id : "?",
            clean_len ? clean_len : strlen(turn.text)
        );
    }
}

static void on_cancel(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    turn_cancel_c cancel;
    (void)user;
    (void)subject;
    (void)reply;
    if (pb_decode_turn_cancel(data, data_len, &cancel) != 0) {
        svc_log("conversation-orchestrator", "reject malformed cancel");
        return;
    }
    svc_log("conversation-orchestrator", "cancel observed request=%s", cancel.request_id);
}

int main(void) {
    vbus_stop_flag stop = 0;
    orch_state st;
    const char *queue;
    int run_rc;
    memset(&st, 0, sizeof(st));
    svc_install_signals(&stop);
    st.nc = svc_connect_bus();
    if (!st.nc) return 1;
    queue = svc_env("VBUS_QUEUE_GROUP", "conversation-orchestrators");
    if (vbus_subscribe(st.nc, SUBJ_TURN_START, queue, on_start, &st) != 0 ||
        vbus_subscribe(st.nc, SUBJ_TURN_CANCEL, queue, on_cancel, &st) != 0) {
        svc_log("conversation-orchestrator", "subscribe failed");
        vbus_close(st.nc);
        return 1;
    }
    svc_log("conversation-orchestrator", "pure-C turn orchestration");
    run_rc = vbus_run(st.nc, &stop);
    vbus_close(st.nc);
    return run_rc == 0 ? 0 : 1;
}
