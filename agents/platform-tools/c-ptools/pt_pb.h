/* pt_pb.h — ToolCall protobuf subset (messages.proto), pure C, no Go. */
#ifndef PT_PB_H
#define PT_PB_H

#include "pt_store.h"

#include <stddef.h>
#include <stdint.h>

/* ToolCallStartRequest (fields 1–10 used). */
typedef struct pt_start_req {
    char tool_call_id[PT_ID];
    char idempotency_key[PT_ID];
    char parent_task_id[PT_ID];
    char parent_turn_id[PT_ID];
    char user_id[PT_ID];
    char session_id[PT_ID];
    char agent_id[PT_ID];
    char tool_id[PT_ID];
    char input_json[PT_INPUT];
    int64_t deadline_unix_ms;
    int caller_tool_present;
} pt_start_req;

/* ToolCallStartResponse */
typedef struct pt_start_resp {
    char tool_call_id[PT_ID];
    int accepted;
    int state; /* ToolCallState enum */
    char event_subject[320];
    int64_t accepted_at;
    char error[PT_STR];
} pt_start_resp;

/* ToolCallDispatchResponse */
typedef struct pt_dispatch_resp {
    char tool_call_id[PT_ID];
    int accepted;
    char output_json[PT_OUT];
    char summary[PT_STR];
    char error[PT_STR];
} pt_dispatch_resp;

/* ToolCallCancelRequest */
typedef struct pt_cancel_req {
    char tool_call_id[PT_ID];
    char user_id[PT_ID];
    char reason[PT_STR];
} pt_cancel_req;

/* ToolCallApprovalDecision */
typedef struct pt_approval_req {
    char tool_call_id[PT_ID];
    int approved;
    char approver_id[PT_ID];
    char reason[PT_STR];
    int64_t decided_at;
    char approval_id[PT_ID]; /* from metadata["approval_id"] if present */
} pt_approval_req;

/* ToolCallEvent (subset for publish) */
typedef struct pt_event {
    char tool_call_id[PT_ID];
    char parent_task_id[PT_ID];
    char parent_turn_id[PT_ID];
    char user_id[PT_ID];
    char session_id[PT_ID];
    char agent_id[PT_ID];
    char tool_id[PT_ID];
    int state;
    int type; /* ToolCallEventType */
    int32_t sequence;
    char text[PT_STR];
    char error[PT_STR];
    int64_t timestamp;
} pt_event;

int pt_pb_dec_start_req(const uint8_t *in, size_t n, pt_start_req *out);
size_t pt_pb_enc_start_resp(uint8_t *out, size_t cap, const pt_start_resp *r);

int pt_pb_dec_cancel_req(const uint8_t *in, size_t n, pt_cancel_req *out);
int pt_pb_dec_approval_req(const uint8_t *in, size_t n, pt_approval_req *out);

size_t pt_pb_enc_dispatch_resp(uint8_t *out, size_t cap, const pt_dispatch_resp *r);
size_t pt_pb_enc_event(uint8_t *out, size_t cap, const pt_event *e);

/* Round-trip helpers for tests: encode a minimal start request. */
size_t pt_pb_enc_start_req(uint8_t *out, size_t cap, const pt_start_req *r);
size_t pt_pb_enc_cancel_req(uint8_t *out, size_t cap, const pt_cancel_req *r);
size_t pt_pb_enc_approval_req(uint8_t *out, size_t cap, const pt_approval_req *r);

#endif
