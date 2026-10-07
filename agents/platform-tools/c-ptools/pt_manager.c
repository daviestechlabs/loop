#include "pt_manager.h"
#include "pt_artifact.h"
#include "pt_dnd_state.h"

#include "../c-toolpolicy/toolpolicy.h"
#include "../c-toolstore/toolstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

int64_t pt_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int restore_call_private(const pt_call *call, void *context) {
    pt_manager *manager = context;
    if (call->state == PT_ST_COMPLETED && call->user_id[0] &&
        (!strcmp(call->tool_id, "dnd-dice-roll") || pt_dnd_state_tool(call->tool_id))) {
        /* Invalid artifacts stay unavailable; startup does not replace them. */
        (void)pt_artifact_restore_private(manager->worker.artifact_dir, call);
    }
    return 0;
}

int pt_manager_init(pt_manager *m, const char *store_path, const char *ws_root,
                    const char *artifact_dir, const char *dnd_dir) {
    pthread_mutexattr_t attributes;
    int result;
    if (!m)
        return -1;
    memset(m, 0, sizeof(*m));
    if (pt_store_open(&m->store, store_path && store_path[0] ? store_path
                                                             : "/tmp/c-ptools-calls.json") != 0)
        return -1;
    pt_worker_init(&m->worker, ws_root, artifact_dir, dnd_dir);
    if (pt_store_visit(&m->store, restore_call_private, m) != 0) goto fail;
    if (pt_dnd_recover(m->worker.dnd_state_dir, m->worker.artifact_dir, &m->store, 1) != 0)
        goto fail;
    if (pthread_mutexattr_init(&attributes) != 0) goto fail;
    result = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    if (result == 0) result = pthread_mutex_init(&m->mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    if (result != 0) goto fail;
    m->initialized = 1;
    return 0;
fail:
    pt_store_close(&m->store);
    return -1;
}

void pt_manager_destroy(pt_manager *m) {
    if (!m || !m->initialized) return;
    pthread_mutex_destroy(&m->mutex);
    pt_store_close(&m->store);
    m->initialized = 0;
}

int pt_manager_ready(pt_manager *m) {
    int ready;
    if (!m || !m->initialized || pthread_mutex_lock(&m->mutex) != 0) return 0;
    ready = m->store.index != NULL && !m->store.failed;
    pthread_mutex_unlock(&m->mutex);
    return ready;
}

static void jget(const char *json, const char *key, char *out, size_t cap) {
    char pat[80];
    const char *p, *q1, *q2;
    size_t wi = 0;
    out[0] = '\0';
    if (!json || !key || cap == 0)
        return;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return;
    p = strchr(p + strlen(pat), ':');
    if (!p)
        return;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return;
    q1 = p + 1;
    for (q2 = q1; *q2 && *q2 != '"'; q2++) {
        if (*q2 == '\\' && q2[1]) {
            q2++;
            if (wi + 1 < cap)
                out[wi++] = *q2;
            continue;
        }
        if (wi + 1 < cap)
            out[wi++] = *q2;
    }
    out[wi] = '\0';
}

static int json_has_top_level_key(const char *json, const char *key) {
    const char *p;
    size_t key_len;
    int depth = 0;
    if (!json || !key) return 0;
    key_len = strlen(key);
    for (p = json; *p; p++) {
        if (*p == '{') {
            depth++;
            continue;
        }
        if (*p == '}') {
            if (depth > 0) depth--;
            continue;
        }
        if (*p == '"') {
            const char *start = ++p;
            int escaped = 0;
            while (*p) {
                if (!escaped && *p == '"') break;
                if (!escaped && *p == '\\')
                    escaped = 1;
                else
                    escaped = 0;
                p++;
            }
            if (!*p) return 0;
            if (depth == 1 && (size_t)(p - start) == key_len &&
                memcmp(start, key, key_len) == 0) {
                const char *after = p + 1;
                while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')
                    after++;
                if (*after == ':') return 1;
            }
        }
    }
    return 0;
}

static void new_call_id(char *out, size_t cap) {
    snprintf(out, cap, "tool-%lld-%d", (long long)pt_now_ms(), (int)getpid());
}

static void event_subject(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "ai.tool.call.events.%s", id);
}

static void fill_start_resp(pt_start_resp *resp, const pt_call *c, int accepted, const char *err) {
    memset(resp, 0, sizeof(*resp));
    resp->accepted = accepted;
    resp->accepted_at = pt_now_ms();
    if (c) {
        snprintf(resp->tool_call_id, sizeof(resp->tool_call_id), "%s", c->tool_call_id);
        resp->state = c->state;
        event_subject(c->tool_call_id, resp->event_subject, sizeof(resp->event_subject));
        if (!accepted && err)
            snprintf(resp->error, sizeof(resp->error), "%s", err);
        else if (c->error[0])
            snprintf(resp->error, sizeof(resp->error), "%s", c->error);
    } else if (err) {
        snprintf(resp->error, sizeof(resp->error), "%s", err);
        resp->state = PT_ST_FAILED;
    }
}

static void write_resp(char *out, size_t cap, const pt_call *c, int accepted, const char *err) {
    if (!out || cap == 0)
        return;
    if (!accepted) {
        snprintf(out, cap,
                 "{\"tool_call_id\":\"%s\",\"accepted\":false,\"error\":\"%s\",\"state\":\"%s\"}",
                 c && c->tool_call_id[0] ? c->tool_call_id : "", err ? err : "rejected",
                 c ? pt_state_name(c->state) : "unspecified");
        return;
    }
    snprintf(out, cap,
             "{\"tool_call_id\":\"%s\",\"accepted\":true,\"state\":\"%s\","
             "\"event_subject\":\"ai.tool.call.events.%s\",\"summary\":\"%s\","
             "\"output_json\":%s%s%s,\"error\":\"%s\"}",
             c->tool_call_id, pt_state_name(c->state), c->tool_call_id, c->summary[0] ? c->summary : "",
             c->output_json[0] ? "" : "\"", c->output_json[0] ? c->output_json : "",
             c->output_json[0] ? "" : "\"", c->error);
}

static void publish_event(pt_manager *m, const pt_call *c, int type, const char *text) {
    (void)m;
    (void)c;
    (void)type;
    (void)text;
}

/* ToolCallEventType: COMPLETED=8 FAILED=9 CANCELED=10 APPROVAL_REQUESTED=2 */
enum {
    PT_EVT_APPROVAL_REQUESTED = 2,
    PT_EVT_COMPLETED = 8,
    PT_EVT_FAILED = 9,
    PT_EVT_CANCELED = 10
};

static int run_call(pt_manager *m, pt_call *c) {
    int rc;
    c->state = PT_ST_RUNNING;
    c->updated_at = pt_now_ms();
    if (pt_dnd_state_tool(c->tool_id)) {
        rc = pt_dnd_run(m->worker.dnd_state_dir, m->worker.artifact_dir, &m->store, c);
        if (rc == PT_DND_PENDING) return -1;
        if (rc == PT_DND_OK) {
            publish_event(m, c, PT_EVT_COMPLETED, c->summary);
            return 0;
        }
    } else rc = pt_worker_dispatch(&m->worker, c);
    if (rc == 0 && strcmp(c->tool_id, "dnd-dice-roll") == 0 &&
        pt_artifact_write(m->worker.artifact_dir, c) != 0) {
        rc = -1;
        c->output_json[0] = '\0';
        snprintf(c->error, sizeof(c->error), "dice artifact persistence failed");
    }
    if (rc == 0) {
        c->state = PT_ST_COMPLETED;
        c->error[0] = '\0';
    } else {
        c->state = PT_ST_FAILED;
        if (!c->error[0])
            snprintf(c->error, sizeof(c->error), "tool failed");
    }
    c->updated_at = pt_now_ms();
    if (pt_store_put(&m->store, c) != 0) return -1;
    publish_event(m, c, c->state == PT_ST_COMPLETED ? PT_EVT_COMPLETED : PT_EVT_FAILED,
                  c->summary[0] ? c->summary : c->error);
    return 0;
}

static int needs_approval(const char *tool_id) {
    return tool_id && strcmp(tool_id, "workspace-edit") == 0;
}

static int same_request(const pt_call *call, const pt_start_req *request) {
    return ts_same_request(
        call->idempotency_key, call->parent_task_id, call->parent_turn_id, call->user_id,
        call->session_id, call->agent_id, call->tool_id, call->input_json,
        request->idempotency_key, request->parent_task_id, request->parent_turn_id,
        request->user_id, request->session_id, request->agent_id, request->tool_id,
        request->input_json[0] ? request->input_json : "{}");
}

static void reuse_call(pt_manager *m, pt_call *existing, const pt_start_req *req,
                       pt_start_resp *resp) {
    if (!same_request(existing, req)) {
        fill_start_resp(resp, NULL, 0, "tool retry request conflict");
        return;
    }
    if (pt_dnd_state_tool(existing->tool_id)) {
        if (!pt_dnd_authorized(m->worker.dnd_state_dir, existing->tool_id, existing->input_json,
                               existing->user_id, existing->state != PT_ST_COMPLETED)) {
            fill_start_resp(resp, NULL, 0, "campaign authority is unavailable");
            return;
        }
        if (existing->state == PT_ST_QUEUED) {
            pt_call pending = *existing;
            if (run_call(m, &pending) != 0) {
                fill_start_resp(resp, NULL, 0, "D&D state recovery is pending");
                return;
            }
            existing = pt_store_get(&m->store, pending.tool_call_id);
        }
        if (!existing || (existing->state == PT_ST_COMPLETED &&
            pt_artifact_verify(m->worker.artifact_dir, existing) != 0)) {
            fill_start_resp(resp, NULL, 0, "D&D state artifact is unavailable");
            return;
        }
    }
    fill_start_resp(resp, existing, 1, NULL);
}

static int start_core(pt_manager *m, const pt_start_req *req, pt_start_resp *resp) {
    pt_call c, *existing;
    char id[PT_ID];
    int64_t now;

    if (!m || !req || !resp) return -1;
    memset(&c, 0, sizeof(c));
    memset(resp, 0, sizeof(*resp));
    now = pt_now_ms();
    resp->accepted_at = now;

    if (req->deadline_unix_ms < 0 ||
        (req->deadline_unix_ms > 0 && req->deadline_unix_ms <= now)) {
        fill_start_resp(resp, NULL, 0, "tool deadline expired");
        return 0;
    }

    if (!req->tool_id[0]) {
        fill_start_resp(resp, NULL, 0, "tool_id required");
        return 0;
    }
    if (!req->agent_id[0]) {
        fill_start_resp(resp, NULL, 0, "agent_id is required for governed tool execution");
        return 0;
    }
    if (!tp_agent_allows_tool(req->agent_id, req->tool_id)) {
        fill_start_resp(resp, NULL, 0, "agent is not allowed to call tool");
        return 0;
    }
    if (pt_dnd_state_tool(req->tool_id) &&
        pt_dnd_recover(m->worker.dnd_state_dir, m->worker.artifact_dir, &m->store, 0) != 0) {
        fill_start_resp(resp, NULL, 0, "D&D state recovery is pending");
        return 0;
    }
    if (pt_dnd_state_tool(req->tool_id) &&
        !pt_dnd_authorized(m->worker.dnd_state_dir, req->tool_id, req->input_json, req->user_id, 1)) {
        fill_start_resp(resp, NULL, 0, "campaign authority is unavailable");
        return 0;
    }
    /* Caller snapshots cannot widen the compiled C policy authority. */
    if (req->caller_tool_present) {
        fill_start_resp(resp, NULL, 0, "caller tool snapshots are not accepted");
        return 0;
    }
    if (!tp_is_builtin_tool(req->tool_id)) {
        snprintf(c.tool_call_id, sizeof(c.tool_call_id), "%s", req->tool_call_id);
        fill_start_resp(resp, &c, 0, "unknown or non-builtin tool");
        return 0;
    }
    if (req->idempotency_key[0]) {
        existing = pt_store_by_idem(&m->store, req->user_id, req->session_id,
                                     req->agent_id, req->tool_id, req->idempotency_key);
        if (existing) {
            reuse_call(m, existing, req, resp);
            return 0;
        }
    }
    snprintf(id, sizeof(id), "%s", req->tool_call_id);
    existing = id[0] ? pt_store_get(&m->store, id) : NULL;
    if (existing) {
        reuse_call(m, existing, req, resp);
        return 0;
    }
    if (!id[0]) new_call_id(id, sizeof(id));
    if (!ts_valid_id(id)) {
        snprintf(c.tool_call_id, sizeof(c.tool_call_id), "%s", id);
        fill_start_resp(resp, &c, 0, "tool_call_id must be a safe 1-128 character identifier");
        return 0;
    }

    snprintf(c.tool_call_id, sizeof(c.tool_call_id), "%s", id);
    snprintf(c.idempotency_key, sizeof(c.idempotency_key), "%s", req->idempotency_key);
    snprintf(c.parent_task_id, sizeof(c.parent_task_id), "%s", req->parent_task_id);
    snprintf(c.parent_turn_id, sizeof(c.parent_turn_id), "%s", req->parent_turn_id);
    snprintf(c.user_id, sizeof(c.user_id), "%s", req->user_id);
    snprintf(c.session_id, sizeof(c.session_id), "%s", req->session_id);
    snprintf(c.agent_id, sizeof(c.agent_id), "%s", req->agent_id);
    snprintf(c.tool_id, sizeof(c.tool_id), "%s", req->tool_id);
    if (req->input_json[0])
        snprintf(c.input_json, sizeof(c.input_json), "%s", req->input_json);
    else
        snprintf(c.input_json, sizeof(c.input_json), "{}");
    c.state = PT_ST_REQUESTED;
    c.created_at = now;
    c.updated_at = now;
    c.in_use = 1;

    if (needs_approval(req->tool_id)) {
        c.state = PT_ST_AWAITING_APPROVAL;
        if (pt_store_put(&m->store, &c) != 0) {
            fill_start_resp(resp, NULL, 0, "store failed");
            return 0;
        }
        publish_event(m, &c, PT_EVT_APPROVAL_REQUESTED, "approval required");
        fill_start_resp(resp, &c, 1, NULL);
        return 0;
    }

    c.state = PT_ST_QUEUED;
    if (pt_store_put(&m->store, &c) != 0) {
        fill_start_resp(resp, NULL, 0, "store failed");
        return 0;
    }
    if (run_call(m, &c) != 0) {
        fill_start_resp(resp, NULL, 0, "store failed");
        return 0;
    }
    {
        pt_call *saved = pt_store_get(&m->store, c.tool_call_id);
        fill_start_resp(resp, saved ? saved : &c, 1, NULL);
    }
    return 0;
}

static int execute_core(pt_manager *m, const pt_start_req *req, pt_dispatch_resp *resp) {
    pt_start_resp start;
    pt_call *c;
    if (!m || !req || !resp) return -1;
    memset(resp, 0, sizeof(*resp));
    if (needs_approval(req->tool_id)) {
        snprintf(resp->tool_call_id, sizeof(resp->tool_call_id), "%s", req->tool_call_id);
        resp->accepted = 0;
        snprintf(resp->error, sizeof(resp->error), "approval-required tools cannot use synchronous execution");
        return 0;
    }
    pt_start_core(m, req, &start);
    snprintf(resp->tool_call_id, sizeof(resp->tool_call_id), "%s", start.tool_call_id);
    if (!start.accepted) {
        resp->accepted = 0;
        snprintf(resp->error, sizeof(resp->error), "%s", start.error);
        return 0;
    }
    c = pt_store_get(&m->store, start.tool_call_id);
    if (!c) {
        resp->accepted = 0;
        snprintf(resp->error, sizeof(resp->error), "missing call after start");
        return 0;
    }
    if (c->state == PT_ST_COMPLETED) {
        resp->accepted = 1;
        snprintf(resp->output_json, sizeof(resp->output_json), "%s", c->output_json);
        snprintf(resp->summary, sizeof(resp->summary), "%s", c->summary);
        return 0;
    }
    resp->accepted = 0;
    snprintf(resp->error, sizeof(resp->error), "%s", c->error[0] ? c->error : pt_state_name(c->state));
    return 0;
}

static int cancel_core(pt_manager *m, const pt_cancel_req *req, pt_start_resp *resp) {
    pt_call *c;
    pt_call next;
    if (!m || !req || !resp) return -1;
    memset(resp, 0, sizeof(*resp));
    c = pt_store_get(&m->store, req->tool_call_id);
    if (!c || strcmp(c->user_id, req->user_id) != 0) {
        fill_start_resp(resp, NULL, 0, "not found");
        snprintf(resp->tool_call_id, sizeof(resp->tool_call_id), "%s", req->tool_call_id);
        return 0;
    }
    if (pt_dnd_state_tool(c->tool_id)) {
        /* A committed transaction must finish; cancel cannot invalidate its journal. */
        if (pt_dnd_recover(m->worker.dnd_state_dir, m->worker.artifact_dir, &m->store, 0) != 0) {
            fill_start_resp(resp, NULL, 0, "D&D state recovery is pending");
            return 0;
        }
        c = pt_store_get(&m->store, req->tool_call_id);
        if (!c || !pt_dnd_authorized(m->worker.dnd_state_dir, c->tool_id, c->input_json, c->user_id,
                                     c->state != PT_ST_COMPLETED) ||
            (c->state == PT_ST_COMPLETED && pt_artifact_verify(m->worker.artifact_dir, c) != 0)) {
            fill_start_resp(resp, NULL, 0, "D&D state artifact is unavailable");
            return 0;
        }
    }
    if (ts_is_terminal(c->state) || c->state == PT_ST_COMPLETED || c->state == PT_ST_FAILED ||
        c->state == PT_ST_CANCELED || c->state == PT_ST_REJECTED) {
        /* Go returns accepted with last event state for terminal */
        fill_start_resp(resp, c, 1, NULL);
        return 0;
    }
    next = *c;
    c = &next;
    c->state = PT_ST_CANCELED;
    snprintf(c->error, sizeof(c->error), "%s", req->reason[0] ? req->reason : "canceled");
    c->updated_at = pt_now_ms();
    if (pt_store_put(&m->store, c) != 0) {
        fill_start_resp(resp, NULL, 0, "store failed");
        return 0;
    }
    publish_event(m, c, PT_EVT_CANCELED, c->error);
    fill_start_resp(resp, c, 1, NULL);
    return 0;
}

static int approve_core(pt_manager *m, const pt_approval_req *req, pt_start_resp *resp) {
    pt_call *c;
    pt_call next;
    char approval_id[PT_ID];
    if (!m || !req || !resp) return -1;
    memset(resp, 0, sizeof(*resp));
    c = pt_store_get(&m->store, req->tool_call_id);
    if (!c) {
        fill_start_resp(resp, NULL, 0, "not found");
        snprintf(resp->tool_call_id, sizeof(resp->tool_call_id), "%s", req->tool_call_id);
        return 0;
    }
    if (c->state != PT_ST_AWAITING_APPROVAL) {
        fill_start_resp(resp, c, 1, NULL);
        return 0;
    }
    next = *c;
    c = &next;
    if (!req->approved) {
        c->state = PT_ST_REJECTED;
        snprintf(c->error, sizeof(c->error), "%s", req->reason[0] ? req->reason : "rejected");
        c->updated_at = pt_now_ms();
        if (pt_store_put(&m->store, c) != 0) {
            fill_start_resp(resp, NULL, 0, "store failed");
            return 0;
        }
        fill_start_resp(resp, c, 1, NULL);
        return 0;
    }
    if (req->approval_id[0])
        snprintf(approval_id, sizeof(approval_id), "%s", req->approval_id);
    else
        snprintf(approval_id, sizeof(approval_id), "approval-%lld", (long long)pt_now_ms());
    snprintf(c->approval_id, sizeof(c->approval_id), "%s", approval_id);
    if (!strstr(c->input_json, "approval_id")) {
        char tmp[PT_INPUT];
        size_t n = strlen(c->input_json);
        if (n > 0 && c->input_json[n - 1] == '}' && n + 64 < sizeof(tmp)) {
            snprintf(tmp, sizeof(tmp), "%.*s,\"approval_id\":\"%s\"}", (int)n - 1, c->input_json,
                     approval_id);
            snprintf(c->input_json, sizeof(c->input_json), "%s", tmp);
        }
    }
    c->state = PT_ST_APPROVED;
    c->updated_at = pt_now_ms();
    if (pt_store_put(&m->store, c) != 0 || run_call(m, c) != 0) {
        fill_start_resp(resp, NULL, 0, "store failed");
        return 0;
    }
    {
        pt_call *saved = pt_store_get(&m->store, req->tool_call_id);
        fill_start_resp(resp, saved ? saved : c, 1, NULL);
    }
    return 0;
}

static void parse_start_json(const char *req_json, pt_start_req *req) {
    memset(req, 0, sizeof(*req));
    jget(req_json, "tool_call_id", req->tool_call_id, sizeof(req->tool_call_id));
    jget(req_json, "idempotency_key", req->idempotency_key, sizeof(req->idempotency_key));
    jget(req_json, "tool_id", req->tool_id, sizeof(req->tool_id));
    jget(req_json, "agent_id", req->agent_id, sizeof(req->agent_id));
    jget(req_json, "user_id", req->user_id, sizeof(req->user_id));
    jget(req_json, "session_id", req->session_id, sizeof(req->session_id));
    jget(req_json, "parent_task_id", req->parent_task_id, sizeof(req->parent_task_id));
    jget(req_json, "parent_turn_id", req->parent_turn_id, sizeof(req->parent_turn_id));
    jget(req_json, "input_json", req->input_json, sizeof(req->input_json));
    req->caller_tool_present = json_has_top_level_key(req_json, "tool");
    if (!req->input_json[0]) {
        const char *inp = strstr(req_json, "\"input\"");
        if (inp) {
            const char *brace = strchr(inp, '{');
            if (brace) {
                int depth = 0;
                const char *p = brace;
                for (; *p; p++) {
                    if (*p == '{')
                        depth++;
                    else if (*p == '}') {
                        depth--;
                        if (depth == 0) {
                            size_t n = (size_t)(p - brace + 1);
                            if (n >= sizeof(req->input_json)) n = sizeof(req->input_json) - 1;
                            memcpy(req->input_json, brace, n);
                            req->input_json[n] = '\0';
                            break;
                        }
                    }
                }
            }
        }
    }
    if (!req->input_json[0]) snprintf(req->input_json, sizeof(req->input_json), "{}");
}

static int start_call_json(pt_manager *m, const char *req_json, char *out, size_t cap) {
    pt_start_req req;
    pt_start_resp resp;
    pt_call *c;
    if (!m || !req_json || !out) return -1;
    parse_start_json(req_json, &req);
    pt_start_core(m, &req, &resp);
    c = resp.tool_call_id[0] ? pt_store_get(&m->store, resp.tool_call_id) : NULL;
    if (resp.accepted && c)
        write_resp(out, cap, c, 1, NULL);
    else
        write_resp(out, cap, c, 0, resp.error);
    return 0;
}

static int get_call_json(pt_manager *m, const char *id, char *out, size_t cap) {
    pt_call *c;
    if (!m || !id || !out) return -1;
    c = pt_store_get(&m->store, id);
    if (!c) {
        snprintf(out, cap, "{\"error\":\"not found\",\"tool_call_id\":\"%s\"}", id);
        return -1;
    }
    write_resp(out, cap, c, 1, NULL);
    return 0;
}

static int cancel_call_json(pt_manager *m, const char *id, const char *reason, char *out, size_t cap) {
    pt_cancel_req req;
    pt_start_resp resp;
    pt_call *c;
    if (!m || !id || !out) return -1;
    memset(&req, 0, sizeof(req));
    snprintf(req.tool_call_id, sizeof(req.tool_call_id), "%s", id);
    snprintf(req.reason, sizeof(req.reason), "%s", reason && reason[0] ? reason : "canceled");
    pt_cancel_core(m, &req, &resp);
    c = resp.tool_call_id[0] ? pt_store_get(&m->store, resp.tool_call_id) : NULL;
    if (resp.accepted && c)
        write_resp(out, cap, c, 1, NULL);
    else
        write_resp(out, cap, c, 0, resp.error[0] ? resp.error : "cancel failed");
    return 0;
}

static int approve_call_json(pt_manager *m, const char *id, int approved, const char *approval_id,
                         char *out, size_t cap) {
    pt_approval_req req;
    pt_start_resp resp;
    pt_call *c;
    if (!m || !id || !out) return -1;
    memset(&req, 0, sizeof(req));
    snprintf(req.tool_call_id, sizeof(req.tool_call_id), "%s", id);
    req.approved = approved;
    if (approval_id) snprintf(req.approval_id, sizeof(req.approval_id), "%s", approval_id);
    pt_approve_core(m, &req, &resp);
    c = resp.tool_call_id[0] ? pt_store_get(&m->store, resp.tool_call_id) : NULL;
    if (resp.accepted && c)
        write_resp(out, cap, c, 1, NULL);
    else
        write_resp(out, cap, c, 0, resp.error[0] ? resp.error : "approval failed");
    return 0;
}

/* Serialize the complete transaction, including JSON response snapshots. */
#define PT_TRANSACTION(call) \
    int result; \
    if (!m || !m->initialized || pthread_mutex_lock(&m->mutex) != 0) return -1; \
    result = (call); \
    pthread_mutex_unlock(&m->mutex); \
    return result

int pt_start_core(pt_manager *m, const pt_start_req *req, pt_start_resp *resp) {
    PT_TRANSACTION(start_core(m, req, resp));
}

int pt_read_owned_call(pt_manager *m, const char *id, const char *user, pt_call *out) {
    pt_call *call;
    int result = -1;
    if (!m || !m->initialized || !id || !user || !out ||
        pthread_mutex_lock(&m->mutex) != 0) return -1;
    call = pt_store_get(&m->store, id);
    if (call && strcmp(call->user_id, user) == 0 &&
        pt_dnd_state_tool(call->tool_id)) {
        if (pt_dnd_recover(m->worker.dnd_state_dir, m->worker.artifact_dir, &m->store, 0) != 0)
            call = NULL;
        else {
            call = pt_store_get(&m->store, id);
            if (call && !pt_dnd_authorized(m->worker.dnd_state_dir, call->tool_id, call->input_json,
                                            call->user_id, call->state != PT_ST_COMPLETED)) call = NULL;
        }
    }
    if (call && strcmp(call->user_id, user) == 0 &&
        (!pt_dnd_state_tool(call->tool_id) || call->state != PT_ST_COMPLETED ||
         pt_artifact_verify(m->worker.artifact_dir, call) == 0)) {
        *out = *call;
        result = 0;
    }
    pthread_mutex_unlock(&m->mutex);
    return result;
}

int pt_execute_core(pt_manager *m, const pt_start_req *req, pt_dispatch_resp *resp) {
    PT_TRANSACTION(execute_core(m, req, resp));
}

int pt_cancel_core(pt_manager *m, const pt_cancel_req *req, pt_start_resp *resp) {
    PT_TRANSACTION(cancel_core(m, req, resp));
}

int pt_approve_core(pt_manager *m, const pt_approval_req *req, pt_start_resp *resp) {
    PT_TRANSACTION(approve_core(m, req, resp));
}

int pt_start_call_json(pt_manager *m, const char *json, char *out, size_t cap) {
    PT_TRANSACTION(start_call_json(m, json, out, cap));
}

int pt_get_call_json(pt_manager *m, const char *id, char *out, size_t cap) {
    PT_TRANSACTION(get_call_json(m, id, out, cap));
}

int pt_cancel_call_json(pt_manager *m, const char *id, const char *reason, char *out, size_t cap) {
    PT_TRANSACTION(cancel_call_json(m, id, reason, out, cap));
}

int pt_approve_call_json(pt_manager *m, const char *id, int approved,
                         const char *approval_id, char *out, size_t cap) {
    PT_TRANSACTION(approve_call_json(m, id, approved, approval_id, out, cap));
}
#undef PT_TRANSACTION

int pt_handle_start_pb(pt_manager *m, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                       size_t *out_len) {
    pt_start_req req;
    pt_start_resp resp;
    if (!out_len) return -1;
    *out_len = 0;
    if (pt_pb_dec_start_req(in, in_len, &req) != 0) {
        memset(&resp, 0, sizeof(resp));
        resp.accepted = 0;
        snprintf(resp.error, sizeof(resp.error), "decode failed");
        *out_len = pt_pb_enc_start_resp(out, out_cap, &resp);
        return 0;
    }
    pt_start_core(m, &req, &resp);
    *out_len = pt_pb_enc_start_resp(out, out_cap, &resp);
    return 0;
}

int pt_handle_execute_pb(pt_manager *m, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                         size_t *out_len) {
    pt_start_req req;
    pt_dispatch_resp resp;
    if (!out_len) return -1;
    *out_len = 0;
    if (pt_pb_dec_start_req(in, in_len, &req) != 0) {
        memset(&resp, 0, sizeof(resp));
        resp.accepted = 0;
        snprintf(resp.error, sizeof(resp.error), "decode failed");
        *out_len = pt_pb_enc_dispatch_resp(out, out_cap, &resp);
        return 0;
    }
    pt_execute_core(m, &req, &resp);
    *out_len = pt_pb_enc_dispatch_resp(out, out_cap, &resp);
    return 0;
}

int pt_handle_cancel_pb(pt_manager *m, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                        size_t *out_len) {
    pt_cancel_req req;
    pt_start_resp resp;
    if (!out_len) return -1;
    *out_len = 0;
    if (pt_pb_dec_cancel_req(in, in_len, &req) != 0) {
        memset(&resp, 0, sizeof(resp));
        resp.accepted = 0;
        snprintf(resp.error, sizeof(resp.error), "decode failed");
        *out_len = pt_pb_enc_start_resp(out, out_cap, &resp);
        return 0;
    }
    pt_cancel_core(m, &req, &resp);
    *out_len = pt_pb_enc_start_resp(out, out_cap, &resp);
    return 0;
}

int pt_handle_approval_pb(pt_manager *m, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                          size_t *out_len) {
    pt_approval_req req;
    pt_start_resp resp;
    if (!out_len) return -1;
    *out_len = 0;
    if (pt_pb_dec_approval_req(in, in_len, &req) != 0) {
        memset(&resp, 0, sizeof(resp));
        resp.accepted = 0;
        snprintf(resp.error, sizeof(resp.error), "decode failed");
        *out_len = pt_pb_enc_start_resp(out, out_cap, &resp);
        return 0;
    }
    pt_approve_core(m, &req, &resp);
    *out_len = pt_pb_enc_start_resp(out, out_cap, &resp);
    return 0;
}
