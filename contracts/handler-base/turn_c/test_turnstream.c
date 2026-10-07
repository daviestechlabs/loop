#define _POSIX_C_SOURCE 200809L
#include "turn_budget.h"
#include "turn_error.h"
#include "turn_frame.h"
#include "turn_ndjson.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

int main(void) {
    char line[1024];
    unsigned char out[64];
    size_t n = 0, plen = 0, consumed = 0;
    uint8_t ft = 0;
    const uint8_t *pay = NULL;

    expect(strcmp(turn_event_name_v1("10"), "completed") == 0, "id 10 completed");
    expect(strcmp(turn_event_name_v1("4"), "text_delta") == 0, "id 4");
    expect(turn_event_type_id_v1("started") == 1, "type id started");
    expect(turn_event_type_id_v1("text_delta") == 4, "type id delta");
    expect(turn_event_type_id_v1("completed") == 10, "type id completed");
    expect(turn_event_type_id_v1("10") == 10, "type id from numeric");
    expect(turn_event_type_id_v1("nope") == 0, "type id unknown");
    expect(turn_event_is_terminal_v1("10") == 1, "terminal completed");
    expect(turn_event_is_terminal_v1("11") == 1, "terminal canceled");
    expect(turn_event_is_terminal_v1("12") == 1, "terminal failed");
    expect(turn_event_is_terminal_v1("4") == 0, "not terminal delta");
    expect(strcmp(turn_agent_task_event_name_v1("5"), "agent_task_completed") == 0, "agent name");
    expect(turn_agent_task_event_is_terminal_v1("5") == 1, "agent terminal");
    expect(turn_agent_task_event_is_terminal_v1("3") == 0, "agent progress not terminal");

    expect(turn_ndjson_accepted_v1("r1", "ai.turn.events.r1", 0, 1234, line, sizeof(line)) ==
               TURN_NDJSON_OK,
           "accepted");
    expect(strstr(line, "\"type\":\"accepted\"") != NULL, "accepted type");
    expect(strstr(line, "turnstream.v1alpha1") != NULL, "proto ver");
    expect(turn_ndjson_error_v1("boom", line, sizeof(line)) == TURN_NDJSON_OK, "error");
    expect(strstr(line, "\"error\":\"boom\"") != NULL, "error body");

    expect(turn_frame_encode_v1(TURN_FRAME_CONTROL_JSON, (const uint8_t *)"ab", 2, out, sizeof(out),
                                &n) == TURN_FRAME_OK,
           "enc");
    expect(n == 7, "enc len");
    expect(turn_frame_decode_v1(out, n, &ft, &pay, &plen, &consumed) == TURN_FRAME_OK, "dec");
    expect(ft == TURN_FRAME_CONTROL_JSON && plen == 2 && pay[0] == 'a', "dec fields");

    {
        const char *keys[] = {TURN_BUDGET_KEY_MS, TURN_BUDGET_KEY_DEADLINE_UNIX_MS};
        const char *vals[] = {"500", "101000"};
        turn_budget_info_v1 info;
        turn_err_class_v1 cls;
        expect(turn_budget_extract_v1(keys, vals, 2, 100000, &info) == TURN_BUDGET_OK, "budget extract");
        expect(info.budget_ms == 500, "budget ms");
        expect(info.has_deadline == 1, "has deadline");
        expect(info.effective_deadline_unix_ms == 100500, "effective earliest");
        expect(info.source == TURN_BUDGET_SRC_BUDGET_MS, "source budget");
        expect(turn_err_classify_msg_v1("boom", &cls) == TURN_ERR_OK, "classify boom");
        expect(strcmp(cls.class_str, TURN_ERR_CLASS_INTERNAL) == 0, "boom internal");
        expect(turn_err_classify_msg_v1("context deadline exceeded", &cls) == TURN_ERR_OK, "deadline");
        expect(strcmp(cls.class_str, TURN_ERR_CLASS_DEADLINE) == 0, "deadline class");
    }

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("ALL PASS c-turnstream unit\n");
    return 0;
}
