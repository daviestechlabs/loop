/*
 * c-turnstream — pure-C turnstream framing + budget/error policy CLI (no Go).
 *
 *   c-turnstream accepted <request_id> <event_subject> <stream_id> <accepted_at_ms>
 *   c-turnstream error <message>
 *   c-turnstream event-name <token_or_id>
 *   c-turnstream agent-event-name <token_or_id>
 *   c-turnstream is-terminal <token_or_id>          # exit 0 if terminal, 1 if not
 *   c-turnstream is-agent-terminal <token_or_id>
 *   c-turnstream frame-encode <type_byte>           # stdin payload → stdout frame
 *   c-turnstream frame-decode                       # stdin frame → stdout type\n + payload
 *   c-turnstream budget-extract <now_unix_ms> [key val ...]
 *   c-turnstream budget-remaining <effective_unix_ms> <now_unix_ms>
 *   c-turnstream err-from-code <1..7>
 *   c-turnstream err-classify-msg <message>
 *   c-turnstream err-json <class> <message>
 */
#define _POSIX_C_SOURCE 200809L
#include "turn_budget.h"
#include "turn_error.h"
#include "turn_frame.h"
#include "turn_ndjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_all(unsigned char *buf, size_t cap, size_t *n_out) {
    size_t n = 0;
    while (n + 1 < cap) {
        size_t r = fread(buf + n, 1, cap - 1 - n, stdin);
        if (r == 0) break;
        n += r;
    }
    if (n_out) *n_out = n;
    return (feof(stdin) || ferror(stdin)) ? 0 : -1;
}

int main(int argc, char **argv) {
    char line[65536];

    if (argc < 2) {
        fprintf(stderr, "usage: c-turnstream accepted|error|event-name|...\n");
        return 2;
    }
    if (strcmp(argv[1], "accepted") == 0) {
        long long stream = 0, at = 0;
        if (argc < 6) {
            fprintf(stderr, "usage: c-turnstream accepted <rid> <subject> <stream_id> <accepted_at_ms>\n");
            return 2;
        }
        stream = strtoll(argv[4], NULL, 10);
        at = strtoll(argv[5], NULL, 10);
        if (turn_ndjson_accepted_v1(argv[2], argv[3], stream, at, line, sizeof(line)) != TURN_NDJSON_OK)
            return 1;
        fputs(line, stdout);
        return 0;
    }
    if (strcmp(argv[1], "error") == 0) {
        const char *msg = argc >= 3 ? argv[2] : "";
        if (turn_ndjson_error_v1(msg, line, sizeof(line)) != TURN_NDJSON_OK) return 1;
        fputs(line, stdout);
        return 0;
    }
    if (strcmp(argv[1], "event-name") == 0) {
        if (argc < 3) return 2;
        puts(turn_event_name_v1(argv[2]));
        return 0;
    }
    if (strcmp(argv[1], "event-type-id") == 0) {
        if (argc < 3) return 2;
        printf("%d\n", turn_event_type_id_v1(argv[2]));
        return 0;
    }
    if (strcmp(argv[1], "agent-event-name") == 0) {
        if (argc < 3) return 2;
        puts(turn_agent_task_event_name_v1(argv[2]));
        return 0;
    }
    if (strcmp(argv[1], "is-terminal") == 0) {
        if (argc < 3) return 2;
        return turn_event_is_terminal_v1(argv[2]) ? 0 : 1;
    }
    if (strcmp(argv[1], "is-agent-terminal") == 0) {
        if (argc < 3) return 2;
        return turn_agent_task_event_is_terminal_v1(argv[2]) ? 0 : 1;
    }
    if (strcmp(argv[1], "frame-encode") == 0) {
        unsigned char payload[TURN_FRAME_MAX_BYTES];
        unsigned char out[TURN_FRAME_MAX_BYTES + 8];
        size_t plen = 0, olen = 0;
        int type = 1;
        if (argc >= 3) type = atoi(argv[2]);
        if (read_all(payload, sizeof(payload), &plen) != 0) {
            fprintf(stderr, "payload too large\n");
            return 1;
        }
        if (plen > TURN_FRAME_MAX_BYTES) return 1;
        if (turn_frame_encode_v1((uint8_t)type, payload, plen, out, sizeof(out), &olen) != TURN_FRAME_OK)
            return 1;
        fwrite(out, 1, olen, stdout);
        return 0;
    }
    if (strcmp(argv[1], "frame-decode") == 0) {
        unsigned char in[TURN_FRAME_MAX_BYTES + 8];
        size_t n = 0, plen = 0, consumed = 0;
        uint8_t ftype = 0;
        const uint8_t *pay = NULL;
        if (read_all(in, sizeof(in), &n) != 0) return 1;
        if (turn_frame_decode_v1(in, n, &ftype, &pay, &plen, &consumed) != TURN_FRAME_OK) return 1;
        printf("%u\n", (unsigned)ftype);
        if (pay && plen) fwrite(pay, 1, plen, stdout);
        return 0;
    }
    if (strcmp(argv[1], "budget-extract") == 0) {
        /* budget-extract <now_ms> [key val ...] */
        int64_t now_ms;
        turn_budget_info_v1 info;
        const char *keys[32];
        const char *vals[32];
        size_t n = 0;
        int i;
        const char *src;
        if (argc < 3)
            return 2;
        now_ms = (int64_t)atoll(argv[2]);
        for (i = 3; i + 1 < argc && n < 32; i += 2) {
            keys[n] = argv[i];
            vals[n] = argv[i + 1];
            n++;
        }
        if (turn_budget_extract_v1(keys, vals, n, now_ms, &info) != TURN_BUDGET_OK)
            return 1;
        switch (info.source) {
        case TURN_BUDGET_SRC_BUDGET_MS:
            src = TURN_BUDGET_KEY_MS;
            break;
        case TURN_BUDGET_SRC_DEADLINE_UNIX_MS:
            src = TURN_BUDGET_KEY_DEADLINE_UNIX_MS;
            break;
        case TURN_BUDGET_SRC_PARENT:
            src = "parent_context";
            break;
        default:
            src = "";
            break;
        }
        printf("budget_ms=%lld\n", (long long)info.budget_ms);
        printf("deadline_unix_ms=%lld\n", (long long)info.deadline_unix_ms);
        printf("effective_unix_ms=%lld\n", (long long)info.effective_deadline_unix_ms);
        printf("source=%s\n", src);
        printf("has_deadline=%d\n", info.has_deadline);
        printf("invalid_budget=%d\n", info.invalid_budget_ms);
        printf("invalid_deadline=%d\n", info.invalid_deadline_unix_ms);
        return 0;
    }
    if (strcmp(argv[1], "budget-remaining") == 0) {
        turn_budget_info_v1 info;
        int64_t rem;
        if (argc < 4)
            return 2;
        memset(&info, 0, sizeof(info));
        info.effective_deadline_unix_ms = (int64_t)atoll(argv[2]);
        info.has_deadline = info.effective_deadline_unix_ms > 0 ? 1 : 0;
        rem = turn_budget_remaining_ms_v1(&info, (int64_t)atoll(argv[3]));
        printf("%lld\n", (long long)rem);
        return 0;
    }
    if (strcmp(argv[1], "err-from-code") == 0) {
        turn_err_class_v1 cls;
        int code;
        if (argc < 3)
            return 2;
        code = atoi(argv[2]);
        if (turn_err_from_code_v1((turn_err_code_v1)code, &cls) != TURN_ERR_OK)
            return 1;
        printf("%s\t%d\n", cls.class_str ? cls.class_str : "", cls.retryable);
        return 0;
    }
    if (strcmp(argv[1], "err-classify-msg") == 0) {
        turn_err_class_v1 cls;
        const char *msg;
        if (argc < 3)
            return 2;
        msg = argv[2];
        if (turn_err_classify_msg_v1(msg, &cls) != TURN_ERR_OK)
            return 1;
        printf("%s\t%d\n", cls.class_str ? cls.class_str : "", cls.retryable);
        return 0;
    }
    if (strcmp(argv[1], "err-json") == 0) {
        turn_err_class_v1 cls;
        const char *class_str;
        const char *msg;
        if (argc < 4)
            return 2;
        class_str = argv[2];
        msg = argv[3];
        memset(&cls, 0, sizeof(cls));
        cls.class_str = class_str;
        cls.retryable = 0;
        if (turn_err_json_v1(&cls, msg, line, sizeof(line)) != TURN_ERR_OK)
            return 1;
        fputs(line, stdout);
        return 0;
    }
    fprintf(stderr, "unknown op\n");
    return 2;
}
