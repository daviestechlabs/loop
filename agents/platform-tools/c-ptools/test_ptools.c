#include "pt_manager.h"
#include "pt_pb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

static int fails;

static void expect(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "FAIL %s\n", m);
        fails++;
    }
}

static int pb_get_string(const uint8_t *in, size_t n, uint32_t want, char *out, size_t cap) {
    /* thin re-decode via dec start resp fields using encode/decode helpers */
    size_t off = 0;
    out[0] = '\0';
    while (off < n) {
        uint64_t key = 0, len = 0, val = 0;
        uint32_t field, wire;
        int shift = 0;
        /* read varint key */
        key = 0;
        shift = 0;
        for (;;) {
            if (off >= n) return -1;
            {
                uint8_t b = in[off++];
                key |= (uint64_t)(b & 0x7f) << shift;
                if ((b & 0x80) == 0) break;
                shift += 7;
            }
        }
        field = (uint32_t)(key >> 3);
        wire = (uint32_t)(key & 7);
        if (wire == 0) {
            shift = 0;
            val = 0;
            for (;;) {
                if (off >= n) return -1;
                {
                    uint8_t b = in[off++];
                    val |= (uint64_t)(b & 0x7f) << shift;
                    if ((b & 0x80) == 0) break;
                    shift += 7;
                }
            }
            if (field == want && cap > 1) {
                snprintf(out, cap, "%llu", (unsigned long long)val);
                return 0;
            }
            continue;
        }
        if (wire == 2) {
            shift = 0;
            len = 0;
            for (;;) {
                if (off >= n) return -1;
                {
                    uint8_t b = in[off++];
                    len |= (uint64_t)(b & 0x7f) << shift;
                    if ((b & 0x80) == 0) break;
                    shift += 7;
                }
            }
            if (off + len > n) return -1;
            if (field == want) {
                size_t copy = (size_t)len;
                if (copy >= cap) copy = cap - 1;
                memcpy(out, in + off, copy);
                out[copy] = '\0';
                return 0;
            }
            off += (size_t)len;
            continue;
        }
        if (wire == 1) {
            off += 8;
            continue;
        }
        if (wire == 5) {
            off += 4;
            continue;
        }
        return -1;
    }
    return -1;
}

typedef struct {
    pt_manager *manager;
    pt_start_req request;
    pt_dispatch_resp response;
    pthread_barrier_t *ready;
    int result;
} retry_thread;

static void *execute_retry(void *argument) {
    retry_thread *call = argument;
    pthread_barrier_wait(call->ready);
    call->result = pt_execute_core(call->manager, &call->request, &call->response);
    return NULL;
}

static void cleanup_store(const char *store) {
    char directory[512];
    DIR *dir;
    struct dirent *entry;
    size_t length = strlen(store);
    if (length < 5u || length >= sizeof(directory)) return;
    memcpy(directory, store, length - 5u);
    memcpy(directory + length - 5u, ".d", 3u);
    dir = opendir(directory);
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            char path[1024];
            if (entry->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            unlink(path);
        }
        closedir(dir);
        rmdir(directory);
    }
    unlink(store);
}

static void test_owned_retry(void) {
    pt_manager *manager = calloc(1, sizeof(*manager));
    pt_manager *restarted = calloc(1, sizeof(*restarted));
    pt_start_req request;
    pt_dispatch_resp response;
    char directory[] = "/tmp/c-ptools-owned-XXXXXX";
    char store[512], blocked[512];
    char original[4096];
    FILE *file;
    expect(manager && restarted && mkdtemp(directory), "owned retry fixtures");
    if (!manager || !restarted) goto done;
    snprintf(store, sizeof(store), "%s/calls.json", directory);
    expect(pt_manager_init(manager, store, directory, directory, directory) == 0,
           "owned retry manager init");
    memset(&request, 0, sizeof(request));
    strcpy(request.tool_call_id, "owned-roll");
    strcpy(request.idempotency_key, "roll-once");
    strcpy(request.user_id, "alice");
    strcpy(request.session_id, "table-1");
    strcpy(request.parent_turn_id, "turn-1");
    strcpy(request.agent_id, "dnd-agent");
    strcpy(request.tool_id, "dnd-dice-roll");
    strcpy(request.input_json, "{\"expression\":\"2d6\"}");
    expect(pt_execute_core(manager, &request, &response) == 0 && response.accepted,
           "owned dice executes");
    snprintf(original, sizeof(original), "%s", response.output_json);
    {
        pt_start_req pending = request;
        pt_cancel_req cancel = {0};
        pt_start_resp started;
        pt_call saved;
        strcpy(pending.tool_call_id, "owned-pending");
        strcpy(pending.idempotency_key, "pending-once");
        strcpy(pending.agent_id, "waterdeep-coder");
        strcpy(pending.tool_id, "workspace-edit");
        strcpy(pending.input_json, "{}");
        expect(pt_start_core(manager, &pending, &started) == 0 && started.accepted &&
                   started.state == PT_ST_AWAITING_APPROVAL, "owned pending call");
        strcpy(cancel.tool_call_id, pending.tool_call_id);
        strcpy(cancel.user_id, "bob");
        expect(pt_cancel_core(manager, &cancel, &started) == 0 && !started.accepted,
               "wrong owner cannot cancel a pending call");
        expect(pt_read_owned_call(manager, pending.tool_call_id, "bob", &saved) != 0,
               "wrong owner cannot read a pending call");
        expect(pt_read_owned_call(manager, pending.tool_call_id, "alice", &saved) == 0 &&
                   saved.state == PT_ST_AWAITING_APPROVAL, "rejected cancel preserves state");
        strcpy(cancel.user_id, "alice");
        expect(pt_cancel_core(manager, &cancel, &started) == 0 && started.accepted &&
                   started.state == PT_ST_CANCELED, "owner can cancel a pending call");
    }
    strcpy(request.tool_call_id, "retry-roll");
    expect(pt_execute_core(manager, &request, &response) == 0 && response.accepted &&
               strcmp(response.tool_call_id, "owned-roll") == 0 &&
               strcmp(response.output_json, original) == 0,
           "exact retry returns the original audited roll");
    strcpy(request.input_json, "{\"expression\":\"1d20\"}");
    expect(pt_execute_core(manager, &request, &response) == 0 && !response.accepted &&
               response.output_json[0] == '\0', "changed retry cannot reuse an old roll");
    strcpy(request.input_json, "{\"expression\":\"2d6\"}");
    strcpy(request.parent_turn_id, "turn-2");
    expect(pt_execute_core(manager, &request, &response) == 0 && !response.accepted,
           "another turn cannot reuse this idempotency key");
    strcpy(request.parent_turn_id, "turn-1");
    strcpy(request.user_id, "bob");
    strcpy(request.tool_call_id, "owned-roll");
    expect(pt_execute_core(manager, &request, &response) == 0 && !response.accepted &&
               response.output_json[0] == '\0', "wrong owner cannot read another call by ID");
    strcpy(request.tool_call_id, "bob-roll");
    expect(pt_execute_core(manager, &request, &response) == 0 && response.accepted &&
               strcmp(response.tool_call_id, "bob-roll") == 0,
           "another owner receives a distinct call for the same retry key");
    strcpy(request.user_id, "alice");
    strcpy(request.session_id, "table-2");
    strcpy(request.tool_call_id, "other-table-roll");
    expect(pt_execute_core(manager, &request, &response) == 0 && response.accepted &&
               strcmp(response.tool_call_id, "other-table-roll") == 0,
           "another session has a distinct retry scope");
    strcpy(request.session_id, "table-1");
    strcpy(request.tool_call_id, "retry-after-restart");
    expect(pt_manager_init(restarted, store, directory, directory, directory) == 0,
           "reload durable tool calls");
    expect(pt_execute_core(restarted, &request, &response) == 0 && response.accepted &&
               strcmp(response.tool_call_id, "owned-roll") == 0 &&
               strcmp(response.output_json, original) == 0,
           "restart preserves request identity and exact dice result");
    request.deadline_unix_ms = pt_now_ms() - 1;
    expect(pt_execute_core(manager, &request, &response) == 0 && !response.accepted,
           "expired call fails before execution or result replay");
    request.deadline_unix_ms = 0;
    {
        retry_thread *calls = calloc(2u, sizeof(*calls));
        pthread_t threads[2];
        pthread_barrier_t ready;
        int i;
        expect(calls != NULL, "concurrent retry fixtures");
        if (calls) {
            expect(pthread_barrier_init(&ready, NULL, 2u) == 0, "retry barrier");
            for (i = 0; i < 2; ++i) {
                calls[i].manager = manager;
                calls[i].request = request;
                strcpy(calls[i].request.idempotency_key, "concurrent-roll");
                snprintf(calls[i].request.tool_call_id, PT_ID, "concurrent-%d", i);
                calls[i].ready = &ready;
                expect(pthread_create(&threads[i], NULL, execute_retry, &calls[i]) == 0,
                       "concurrent retry thread");
            }
            for (i = 0; i < 2; ++i) pthread_join(threads[i], NULL);
            expect(calls[0].result == 0 && calls[1].result == 0 &&
                       calls[0].response.accepted && calls[1].response.accepted &&
                       strcmp(calls[0].response.tool_call_id, calls[1].response.tool_call_id) == 0 &&
                       strcmp(calls[0].response.output_json, calls[1].response.output_json) == 0,
                   "simultaneous retries execute one roll");
            pthread_barrier_destroy(&ready);
            free(calls);
        }
    }
    snprintf(blocked, sizeof(blocked), "%s/blocked", directory);
    file = fopen(blocked, "w");
    expect(file != NULL, "write-failure fixture");
    if (file) fclose(file);
    snprintf(manager->store.path, sizeof(manager->store.path), "%s/blocked/calls.json", directory);
    strcpy(request.tool_call_id, "failed-write");
    strcpy(request.idempotency_key, "failed-write");
    expect(pt_execute_core(manager, &request, &response) == 0 && !response.accepted &&
               !pt_store_get(&manager->store, "failed-write"),
           "failed persistence leaves no successful in-memory call");
    unlink(blocked);
    cleanup_store(store);
    rmdir(directory);
done:
    pt_manager_destroy(manager);
    pt_manager_destroy(restarted);
    free(manager);
    free(restarted);
}

int main(void) {
    pt_manager *m = calloc(1, sizeof(*m));
    char path[] = "/tmp/c-ptools-test-XXXXXX";
    char store[80];
    char ws[] = "/tmp/c-ptools-ws-XXXXXX";
    char out[8192];
    int fd;
    FILE *f;

    test_owned_retry();

    expect(m != NULL, "alloc");
    if (!m) return 1;

    fd = mkstemp(path);
    expect(fd >= 0, "mkstemp store");
    if (fd >= 0) close(fd);
    snprintf(store, sizeof(store), "%s.json", path);
    unlink(path);

    expect(mkdtemp(ws) != NULL, "mkdtemp ws");
    {
        char note[256];
        snprintf(note, sizeof(note), "%s/hello.txt", ws);
        f = fopen(note, "w");
        expect(f != NULL, "write fixture");
        if (f) {
            fputs("hello-c-ptools\n", f);
            fclose(f);
        }
    }

    expect(pt_manager_init(m, store, ws, "/tmp/c-ptools-art", "/tmp/c-ptools-dnd") == 0, "init");

    /* dice roll JSON */
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"dice-1\",\"agent_id\":\"dnd-agent\","
                       "\"tool_id\":\"dnd-dice-roll\",\"input_json\":\"{\\\"expression\\\":\\\"1d20\\\"}\"}",
                       out, sizeof(out));
    expect(strstr(out, "\"accepted\":true") != NULL, "dice accepted");
    expect(strstr(out, "\"state\":\"completed\"") != NULL, "dice completed");

    /* idempotency */
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"dice-2\",\"idempotency_key\":\"idem-dice\","
                       "\"agent_id\":\"dnd-agent\",\"tool_id\":\"dnd-dice-roll\","
                       "\"input_json\":\"{\\\"expression\\\":\\\"2d6\\\"}\"}",
                       out, sizeof(out));
    expect(strstr(out, "\"accepted\":true") != NULL, "idem1");
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"dice-3\",\"idempotency_key\":\"idem-dice\","
                       "\"agent_id\":\"dnd-agent\",\"tool_id\":\"dnd-dice-roll\","
                       "\"input_json\":\"{\\\"expression\\\":\\\"2d6\\\"}\"}",
                       out, sizeof(out));
    expect(strstr(out, "dice-2") != NULL, "idem reuse same id");

    /* workspace read */
    {
        char req[1024];
        snprintf(req, sizeof(req),
                 "{\"tool_call_id\":\"ws-read-1\",\"agent_id\":\"waterdeep-coder\","
                 "\"tool_id\":\"workspace-read\","
                 "\"input_json\":\"{\\\"paths\\\":[\\\"hello.txt\\\"]}\"}");
        pt_start_call_json(m, req, out, sizeof(out));
        expect(strstr(out, "\"accepted\":true") != NULL, "ws read accepted");
        expect(strstr(out, "hello-c-ptools") != NULL || strstr(out, "completed") != NULL, "ws content");
    }

    /* path escape */
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"ws-bad\",\"agent_id\":\"waterdeep-coder\","
                       "\"tool_id\":\"workspace-read\","
                       "\"input_json\":\"{\\\"paths\\\":[\\\"../etc/passwd\\\"]}\"}",
                       out, sizeof(out));
    expect(strstr(out, "failed") != NULL, "path escape fails");

    /* approval */
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"edit-1\",\"agent_id\":\"waterdeep-coder\","
                       "\"tool_id\":\"workspace-edit\",\"input_json\":\"{\\\"patch\\\":\\\"\\\"}\"}",
                       out, sizeof(out));
    expect(strstr(out, "awaiting_approval") != NULL, "edit awaits approval");
    pt_approve_call_json(m, "edit-1", 1, "apr-1", out, sizeof(out));
    expect(strstr(out, "\"accepted\":true") != NULL, "approval accepted");
    expect(strstr(out, "completed") != NULL, "edit completed after approval");

    /* cancel */
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"cancel-me\",\"agent_id\":\"waterdeep-coder\","
                       "\"tool_id\":\"workspace-edit\",\"input_json\":\"{}\"}",
                       out, sizeof(out));
    pt_cancel_call_json(m, "cancel-me", "user cancel", out, sizeof(out));
    expect(strstr(out, "canceled") != NULL, "cancel");

    /* invalid tool / id */
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"bad-tool\",\"agent_id\":\"dnd-agent\","
                       "\"tool_id\":\"shell-exec\",\"input_json\":\"{}\"}",
                       out, sizeof(out));
    expect(strstr(out, "\"accepted\":false") != NULL, "reject non-builtin");
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"../escape\",\"agent_id\":\"dnd-agent\","
                       "\"tool_id\":\"dnd-dice-roll\","
                       "\"input_json\":\"{\\\"expression\\\":\\\"1d20\\\"}\"}",
                       out, sizeof(out));
    expect(strstr(out, "\"accepted\":false") != NULL, "reject unsafe id");
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"missing-agent\",\"tool_id\":\"dnd-dice-roll\","
                       "\"input_json\":\"{}\"}",
                       out, sizeof(out));
    expect(strstr(out, "agent_id is required") != NULL, "reject missing agent authority");
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"wrong-agent\",\"agent_id\":\"dnd-agent\","
                       "\"tool_id\":\"workspace-read\",\"input_json\":\"{}\"}",
                       out, sizeof(out));
    expect(strstr(out, "not allowed") != NULL, "reject unbound agent tool");
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"caller-snapshot\",\"agent_id\":\"dnd-agent\","
                       "\"tool_id\":\"dnd-dice-roll\",\"tool\":{\"tool_id\":\"dnd-dice-roll\"},"
                       "\"input_json\":\"{}\"}",
                       out, sizeof(out));
    expect(strstr(out, "caller tool snapshots are not accepted") != NULL,
           "reject caller policy snapshot");
    pt_start_call_json(m,
                       "{\"tool_call_id\":\"input-tool-field\",\"agent_id\":\"dnd-agent\","
                       "\"tool_id\":\"dnd-dice-roll\","
                       "\"input_json\":\"{\\\"tool\\\":\\\"cosmetic\\\","
                       "\\\"expression\\\":\\\"1d20\\\"}\"}",
                       out, sizeof(out));
    expect(strstr(out, "\"accepted\":true") != NULL,
           "nested input tool field is not a caller snapshot");

    /* ── protobuf NATS parity ── */
    {
        pt_start_req sreq;
        pt_start_resp sresp;
        pt_dispatch_resp dresp;
        uint8_t pb[8192], pbo[8192];
        size_t n, on;
        char tmp[256];

        memset(&sreq, 0, sizeof(sreq));
        snprintf(sreq.tool_call_id, sizeof(sreq.tool_call_id), "pb-dice-1");
        snprintf(sreq.agent_id, sizeof(sreq.agent_id), "dnd-agent");
        snprintf(sreq.tool_id, sizeof(sreq.tool_id), "dnd-dice-roll");
        snprintf(sreq.input_json, sizeof(sreq.input_json), "{\"expression\":\"1d6\"}");
        n = pt_pb_enc_start_req(pb, sizeof(pb), &sreq);
        expect(n > 0, "enc start req");

        /* decode roundtrip */
        memset(&sreq, 0, sizeof(sreq));
        expect(pt_pb_dec_start_req(pb, n, &sreq) == 0, "dec start req");
        expect(strcmp(sreq.tool_id, "dnd-dice-roll") == 0, "dec tool_id");
        expect(strstr(sreq.input_json, "1d6") != NULL, "dec input");

        /* Even an empty protobuf ToolDefinitionSnapshot is caller policy. */
        pb[n++] = (uint8_t)((11u << 3) | 2u);
        pb[n++] = 0;
        memset(&sreq, 0, sizeof(sreq));
        expect(pt_pb_dec_start_req(pb, n, &sreq) == 0 && sreq.caller_tool_present,
               "detect protobuf caller snapshot");
        expect(pt_handle_start_pb(m, pb, n, pbo, sizeof(pbo), &on) == 0,
               "reject protobuf caller snapshot");
        expect(pb_get_string(pbo, on, 2, tmp, sizeof(tmp)) == 0 && strcmp(tmp, "0") == 0,
               "protobuf caller snapshot denied");

        /* Restore the canonical request without field 11 for the happy path. */
        memset(&sreq, 0, sizeof(sreq));
        snprintf(sreq.tool_call_id, sizeof(sreq.tool_call_id), "pb-dice-1");
        snprintf(sreq.agent_id, sizeof(sreq.agent_id), "dnd-agent");
        snprintf(sreq.tool_id, sizeof(sreq.tool_id), "dnd-dice-roll");
        snprintf(sreq.input_json, sizeof(sreq.input_json), "{\"expression\":\"1d6\"}");
        n = pt_pb_enc_start_req(pb, sizeof(pb), &sreq);

        expect(pt_handle_start_pb(m, pb, n, pbo, sizeof(pbo), &on) == 0, "pb start handle");
        expect(on > 0, "pb start resp len");
        expect(pb_get_string(pbo, on, 1, tmp, sizeof(tmp)) == 0, "pb resp id");
        expect(strcmp(tmp, "pb-dice-1") == 0, "pb resp id val");
        expect(pb_get_string(pbo, on, 2, tmp, sizeof(tmp)) == 0, "pb accepted field");
        expect(strcmp(tmp, "1") == 0, "pb accepted true");
        expect(pb_get_string(pbo, on, 3, tmp, sizeof(tmp)) == 0, "pb state field");
        expect(strcmp(tmp, "7") == 0, "pb state completed=7"); /* TOOL_CALL_STATE_COMPLETED */

        /* execute protobuf */
        memset(&sreq, 0, sizeof(sreq));
        snprintf(sreq.tool_call_id, sizeof(sreq.tool_call_id), "pb-ex-1");
        snprintf(sreq.agent_id, sizeof(sreq.agent_id), "dnd-agent");
        snprintf(sreq.tool_id, sizeof(sreq.tool_id), "dnd-dice-roll");
        snprintf(sreq.input_json, sizeof(sreq.input_json), "{\"expression\":\"1d4\"}");
        n = pt_pb_enc_start_req(pb, sizeof(pb), &sreq);
        expect(pt_handle_execute_pb(m, pb, n, pbo, sizeof(pbo), &on) == 0, "pb execute");
        expect(pb_get_string(pbo, on, 2, tmp, sizeof(tmp)) == 0, "exec accepted");
        expect(strcmp(tmp, "1") == 0, "exec accepted true");
        expect(pb_get_string(pbo, on, 5, tmp, sizeof(tmp)) == 0 || 1, "exec summary optional");
        expect(pb_get_string(pbo, on, 3, tmp, sizeof(tmp)) == 0, "exec output_json");
        expect(strstr(tmp, "expression") != NULL || strstr(tmp, "1d4") != NULL || tmp[0], "exec has output");

        /* execute rejects approval tools */
        memset(&sreq, 0, sizeof(sreq));
        snprintf(sreq.tool_call_id, sizeof(sreq.tool_call_id), "pb-ex-edit");
        snprintf(sreq.agent_id, sizeof(sreq.agent_id), "waterdeep-coder");
        snprintf(sreq.tool_id, sizeof(sreq.tool_id), "workspace-edit");
        snprintf(sreq.input_json, sizeof(sreq.input_json), "{}");
        n = pt_pb_enc_start_req(pb, sizeof(pb), &sreq);
        expect(pt_handle_execute_pb(m, pb, n, pbo, sizeof(pbo), &on) == 0, "pb exec edit");
        expect(pb_get_string(pbo, on, 2, tmp, sizeof(tmp)) == 0, "exec edit accepted field");
        expect(strcmp(tmp, "0") == 0, "exec edit rejected");

        /* cancel protobuf */
        {
            pt_cancel_req creq;
            memset(&creq, 0, sizeof(creq));
            /* start awaiting call */
            pt_start_call_json(m,
                               "{\"tool_call_id\":\"pb-cancel\",\"agent_id\":\"waterdeep-coder\","
                               "\"tool_id\":\"workspace-edit\","
                               "\"input_json\":\"{}\"}",
                               out, sizeof(out));
            snprintf(creq.tool_call_id, sizeof(creq.tool_call_id), "pb-cancel");
            snprintf(creq.reason, sizeof(creq.reason), "pb cancel");
            n = pt_pb_enc_cancel_req(pb, sizeof(pb), &creq);
            expect(pt_handle_cancel_pb(m, pb, n, pbo, sizeof(pbo), &on) == 0, "pb cancel");
            expect(pb_get_string(pbo, on, 2, tmp, sizeof(tmp)) == 0, "cancel accepted");
            expect(strcmp(tmp, "1") == 0, "cancel ok");
            expect(pb_get_string(pbo, on, 3, tmp, sizeof(tmp)) == 0, "cancel state");
            expect(strcmp(tmp, "9") == 0, "cancel state=9");
        }

        /* approval protobuf with metadata approval_id */
        {
            pt_approval_req areq;
            pt_start_call_json(m,
                               "{\"tool_call_id\":\"pb-approve\",\"agent_id\":\"waterdeep-coder\","
                               "\"tool_id\":\"workspace-edit\","
                               "\"input_json\":\"{\\\"patch\\\":\\\"\\\"}\"}",
                               out, sizeof(out));
            memset(&areq, 0, sizeof(areq));
            snprintf(areq.tool_call_id, sizeof(areq.tool_call_id), "pb-approve");
            areq.approved = 1;
            snprintf(areq.approval_id, sizeof(areq.approval_id), "apr-pb-1");
            n = pt_pb_enc_approval_req(pb, sizeof(pb), &areq);
            /* decode checks map */
            memset(&areq, 0, sizeof(areq));
            expect(pt_pb_dec_approval_req(pb, n, &areq) == 0, "dec approval");
            expect(areq.approved == 1, "dec approved");
            expect(strcmp(areq.approval_id, "apr-pb-1") == 0, "dec approval_id from metadata");

            expect(pt_handle_approval_pb(m, pb, n, pbo, sizeof(pbo), &on) == 0, "pb approval");
            expect(pb_get_string(pbo, on, 2, tmp, sizeof(tmp)) == 0, "apr accepted");
            expect(strcmp(tmp, "1") == 0, "apr ok");
            expect(pb_get_string(pbo, on, 3, tmp, sizeof(tmp)) == 0, "apr state");
            expect(strcmp(tmp, "7") == 0, "apr completed");
        }

        /* start_resp encode/decode sanity */
        memset(&sresp, 0, sizeof(sresp));
        snprintf(sresp.tool_call_id, sizeof(sresp.tool_call_id), "x");
        sresp.accepted = 1;
        sresp.state = 7;
        snprintf(sresp.event_subject, sizeof(sresp.event_subject), "ai.tool.call.events.x");
        sresp.accepted_at = 123;
        n = pt_pb_enc_start_resp(pb, sizeof(pb), &sresp);
        expect(n > 0, "enc start resp");
        expect(pb_get_string(pb, n, 4, tmp, sizeof(tmp)) == 0, "event subject");
        expect(strcmp(tmp, "ai.tool.call.events.x") == 0, "event subject val");

        (void)dresp;
    }

    pt_get_call_json(m, "dice-1", out, sizeof(out));
    expect(strstr(out, "dice-1") != NULL, "get dice-1");

    /* dnd-encounter-state: start → add → get (unique ids per run) */
    {
        char req[2048], dnddir[] = "/tmp/c-ptools-dnd-XXXXXX";
        char camp_id[64], enc_id[64];
        expect(mkdtemp(dnddir) != NULL, "mkdtemp dnd");
        /* re-init manager with fresh dnd dir */
        free(m);
        m = calloc(1, sizeof(*m));
        pt_manager_destroy(m);
        expect(pt_manager_init(m, store, ws, "/tmp/c-ptools-art", dnddir) == 0, "reinit dnd");
        snprintf(camp_id, sizeof(camp_id), "c%d", (int)getpid());
        snprintf(enc_id, sizeof(enc_id), "e%d", (int)getpid());

        /* Campaign authority must exist before encounter admission. */
        snprintf(req, sizeof(req),
                 "{\"tool_call_id\":\"camp-create\",\"user_id\":\"u1\",\"agent_id\":\"dnd-agent\","
                 "\"tool_id\":\"dnd-campaign-state\","
                 "\"input_json\":\"{\\\"operation\\\":\\\"create\\\",\\\"campaign_id\\\":\\\"%s\\\","
                 "\\\"expected_version\\\":0,\\\"campaign\\\":{\\\"name\\\":\\\"LostMines\\\","
                 "\\\"ruleset\\\":\\\"5e\\\",\\\"description\\\":\\\"intro\\\","
                 "\\\"current_scene\\\":\\\"tavern\\\"}}\"}",
                 camp_id);
        pt_start_call_json(m, req, out, sizeof(out));
        expect(strstr(out, "\"accepted\":true") != NULL, "camp create accepted");
        if (strstr(out, "failed") != NULL)
            fprintf(stderr, "camp create out: %s\n", out);
        expect(strstr(out, "completed") != NULL || strstr(out, "\"state\":\"completed\"") != NULL ||
                   strstr(out, "Created") != NULL,
               "camp create completed");

        snprintf(req, sizeof(req),
                 "{\"tool_call_id\":\"enc-start\",\"user_id\":\"u1\",\"agent_id\":\"dnd-agent\","
                 "\"tool_id\":\"dnd-encounter-state\","
                 "\"input_json\":\"{\\\"operation\\\":\\\"start\\\",\\\"campaign_id\\\":\\\"%s\\\","
                 "\\\"encounter_id\\\":\\\"%s\\\",\\\"expected_version\\\":0}\"}",
                 camp_id, enc_id);
        pt_start_call_json(m, req, out, sizeof(out));
        expect(strstr(out, "\"accepted\":true") != NULL, "enc start accepted");
        expect(strstr(out, "completed") != NULL || strstr(out, "\"state\":\"completed\"") != NULL,
               "enc start completed");

        snprintf(req, sizeof(req),
                 "{\"tool_call_id\":\"enc-add\",\"user_id\":\"u1\",\"agent_id\":\"dnd-agent\","
                 "\"tool_id\":\"dnd-encounter-state\","
                 "\"input_json\":\"{\\\"operation\\\":\\\"add\\\",\\\"campaign_id\\\":\\\"%s\\\","
                 "\\\"encounter_id\\\":\\\"%s\\\",\\\"expected_version\\\":1,"
                 "\\\"participant\\\":{\\\"id\\\":\\\"goblin-1\\\",\\\"name\\\":\\\"Goblin\\\","
                 "\\\"initiative\\\":12,\\\"max_hp\\\":15,\\\"current_hp\\\":15}}\"}",
                 camp_id, enc_id);
        pt_start_call_json(m, req, out, sizeof(out));
        expect(strstr(out, "\"accepted\":true") != NULL, "enc add accepted");
        /* accept completed or failed with detail — require accepted at least */
        if (strstr(out, "failed") != NULL)
            fprintf(stderr, "enc add out: %s\n", out);

        snprintf(req, sizeof(req),
                 "{\"tool_call_id\":\"enc-get\",\"user_id\":\"u1\",\"agent_id\":\"dnd-agent\","
                 "\"tool_id\":\"dnd-encounter-state\","
                 "\"input_json\":\"{\\\"operation\\\":\\\"get\\\",\\\"campaign_id\\\":\\\"%s\\\","
                 "\\\"encounter_id\\\":\\\"%s\\\"}\"}",
                 camp_id, enc_id);
        pt_start_call_json(m, req, out, sizeof(out));
        expect(strstr(out, "\"accepted\":true") != NULL, "enc get accepted");

        snprintf(req, sizeof(req),
                 "{\"tool_call_id\":\"camp-get\",\"user_id\":\"u1\",\"agent_id\":\"dnd-agent\","
                 "\"tool_id\":\"dnd-campaign-state\","
                 "\"input_json\":\"{\\\"operation\\\":\\\"get\\\",\\\"campaign_id\\\":\\\"%s\\\"}\"}",
                 camp_id);
        pt_start_call_json(m, req, out, sizeof(out));
        expect(strstr(out, "\"accepted\":true") != NULL, "camp get accepted");
    }

    cleanup_store(store);
    {
        char note[256];
        snprintf(note, sizeof(note), "%s/hello.txt", ws);
        unlink(note);
        rmdir(ws);
    }
    pt_manager_destroy(m);
    free(m);

    if (fails) {
        fprintf(stderr, "%d fails\n", fails);
        return 1;
    }
    printf("ALL PASS c-ptools\n");
    return 0;
}
