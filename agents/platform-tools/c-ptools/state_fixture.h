/* Shared process fault fixture for the two built-in state tools. */
#ifndef PT_STATE_FIXTURE_H
#define PT_STATE_FIXTURE_H
#include "pt_manager.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>

int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to);
int __real_unlink(const char *path);
int __wrap_unlink(const char *path);
static int fixture_campaign, fixture_scene, fixture_committed, fixture_cleanup;
static const char *fixture_root;

ssize_t __real_getrandom(void *buffer, size_t length, unsigned int flags);
ssize_t __wrap_getrandom(void *buffer, size_t length, unsigned int flags);
ssize_t __wrap_getrandom(void *buffer, size_t length, unsigned int flags) {
    const char *mode = getenv("PT_TEST_ENTROPY");
    static size_t next;
    static const unsigned char words[] = {0u, 19u, 4u, 14u, 1u, 9u};
    if (mode && !strcmp(mode, "fail")) { errno = EIO; return -1; }
    if (mode && !strcmp(mode, "sequence")) {
        if (length != 4u || flags) { errno = EINVAL; return -1; }
        memset(buffer, 0, length);
        ((unsigned char *)buffer)[3] = words[next++ % sizeof(words)];
        return 4;
    }
    return __real_getrandom(buffer, length, flags);
}

static int fixture_ends(const char *value, const char *suffix) {
    size_t n = strlen(value), m = strlen(suffix);
    return n >= m && !strcmp(value + n - m, suffix);
}
static int fixture_journal(const char *path) {
    return fixture_ends(path, fixture_scene ? "/scene-query-transaction-v1.json" : fixture_campaign ? "/campaign-transaction-v1.json" : "/encounter-transaction-v1.json");
}
static void fixture_crash(const char *stage) {
    const char *wanted = getenv("PT_TEST_CRASH_AT");
    if (wanted && !strcmp(wanted, stage)) _exit(77);
}
int __wrap_rename(const char *from, const char *to) {
    int rc;
    if (fixture_journal(to)) fixture_crash("artifact");
    rc = __real_rename(from, to);
    if (!rc) {
        if (fixture_journal(to)) { fixture_committed = 1; fixture_crash("journal"); }
        else if (fixture_ends(to, fixture_campaign ? "/campaigns/table.json" : "/encounters/table/battle.json"))
            fixture_crash("state");
        else if (strstr(to, "/calls.d/")) {
            FILE *f = fopen(to, "rb");
            char *raw = malloc(PT_RECORD_CAP);
            pt_call call;
            size_t n = f && raw ? fread(raw, 1u, PT_RECORD_CAP - 1u, f) : 0;
            if (f) fclose(f);
            if (raw) {
                raw[n] = '\0';
                if (n && pt_store_record_decode(raw, &call) == 0)
                    fixture_crash(call.state == PT_ST_QUEUED ? "queued" : "complete");
            }
            free(raw);
        }
    }
    return rc;
}
int __wrap_unlink(const char *path) {
    int rc = __real_unlink(path);
    if (!rc && fixture_journal(path)) { fixture_cleanup = 1; fixture_crash("cleanup"); }
    return rc;
}
int __wrap_fsync(int fd) {
    const char *fail = getenv("PT_TEST_FAIL_FSYNC");
    char path[1024], descriptor[64];
    ssize_t n;
    (void)snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    n = readlink(descriptor, path, sizeof(path) - 1u);
    if (n > 0 && (size_t)n < sizeof(path)) {
        int reject;
        path[n] = '\0';
        reject = fail && (
            (!strcmp(fail, "1") && strstr(path, fixture_campaign ? "/campaigns/.table.json.tmp-" : "/encounters/table/.battle.json.tmp-")) ||
            (!strcmp(fail, "2") && fixture_ends(path, fixture_campaign ? "/campaigns" : "/encounters/table")) ||
            (!strcmp(fail, "journal") && strstr(path, fixture_scene ? "/.scene-query-transaction-v1.json.tmp-" : fixture_campaign ? "/.campaign-transaction-v1.json.tmp-" : "/.encounter-transaction-v1.json.tmp-")) ||
            (!strcmp(fail, "journal-dir") && fixture_committed && fixture_root && !strcmp(path, fixture_root)) ||
            (!strcmp(fail, "complete") && fixture_committed && strstr(path, "/calls.d/.")) ||
            (!strcmp(fail, "cleanup") && fixture_cleanup && fixture_root && !strcmp(path, fixture_root)));
        if (reject) { errno = EIO; return -1; }
    }
    return __real_fsync(fd);
}

static int fixture_execute(const char *tool, int argc, char **argv, const char *input, size_t size) {
    pt_manager *manager;
    pt_start_req req = {0};
    pt_dispatch_resp response = {0};
    char store[512], artifacts[512];
    int rc, result = 1;
    if (argc < 3 || argc > 5 || size >= PT_INPUT || strlen(argv[1]) >= sizeof(store) - 32u ||
        strlen(argv[2]) >= sizeof(req.user_id)) return 1;
    fixture_scene = !strcmp(tool, "dnd-scene-presence");
    fixture_campaign = fixture_scene || !strcmp(tool, "dnd-campaign-state");
    fixture_root = argv[1];
    manager = calloc(1, sizeof(*manager));
    if (!manager) return 1;
    (void)snprintf(store, sizeof(store), "%s/calls.json", argv[1]);
    (void)snprintf(artifacts, sizeof(artifacts), "%s/artifacts", argv[1]);
    if (pt_manager_init(manager, store, argv[1], artifacts, argv[1])) { free(manager); return 1; }
    strcpy(req.user_id, argv[2]);
    if (argc >= 4) {
        if (strlen(argv[3]) >= sizeof(req.tool_call_id)) goto done;
        strcpy(req.tool_call_id, argv[3]);
    } else (void)snprintf(req.tool_call_id, sizeof(req.tool_call_id), "%s-test-%ld",
                          fixture_campaign ? "campaign" : "encounter", (long)getpid());
    strcpy(req.idempotency_key, req.tool_call_id);
    strcpy(req.agent_id, "dnd-agent");
    strcpy(req.tool_id, tool);
    strcpy(req.session_id, "table");
    {
        const char *parent = getenv("PT_TEST_PARENT_TURN_ID");
        if (parent) {
            if (strlen(parent) >= sizeof(req.parent_turn_id)) goto done;
            strcpy(req.parent_turn_id, parent);
        }
    }
    memcpy(req.input_json, input, size + 1u);
    if (argc == 5 && !strcmp(argv[4], "cancel")) {
        pt_cancel_req cancel = {0};
        pt_start_resp canceled;
        strcpy(cancel.tool_call_id, req.tool_call_id);
        strcpy(cancel.user_id, req.user_id);
        rc = pt_cancel_core(manager, &cancel, &canceled);
        response.accepted = canceled.accepted;
    } else rc = pt_execute_core(manager, &req, &response);
    if (argc == 5 && (!strcmp(argv[4], "cancel-pending") || !strcmp(argv[4], "read-pending") ||
                      !strcmp(argv[4], "other-pending"))) {
        pt_cancel_req cancel = {0};
        pt_start_resp canceled;
        pt_call *pending = pt_store_get(&manager->store, req.tool_call_id), observed;
        pt_start_req other = req;
        pt_dispatch_resp other_response;
        if (rc || response.accepted || !pending || pending->state != PT_ST_QUEUED) goto done;
        strcpy(cancel.tool_call_id, req.tool_call_id);
        strcpy(cancel.user_id, req.user_id);
        if (!strcmp(argv[4], "cancel-pending")) {
            if (pt_cancel_core(manager, &cancel, &canceled) || canceled.accepted || pending->state != PT_ST_QUEUED) goto done;
        } else if (!strcmp(argv[4], "read-pending")) {
            if (!pt_read_owned_call(manager, req.tool_call_id, req.user_id, &observed)) goto done;
        } else {
            strcpy(other.tool_call_id, "other-state");
            strcpy(other.idempotency_key, other.tool_call_id);
            strcpy(other.tool_id, fixture_campaign ? "dnd-encounter-state" : "dnd-campaign-state");
            strcpy(other.input_json, fixture_campaign ?
                "{\"operation\":\"start\",\"campaign_id\":\"table\",\"encounter_id\":\"battle\",\"expected_version\":0}" :
                "{\"operation\":\"get\",\"campaign_id\":\"table\"}");
            if (pt_execute_core(manager, &other, &other_response) || other_response.accepted ||
                pt_store_get(&manager->store, other.tool_call_id)) goto done;
        }
        if (unsetenv("PT_TEST_FAIL_FSYNC")) goto done;
        if (!strcmp(argv[4], "cancel-pending")) {
            if (pt_cancel_core(manager, &cancel, &canceled) || !canceled.accepted || canceled.state != PT_ST_COMPLETED) goto done;
        } else if (!strcmp(argv[4], "other-pending")) {
            if (pt_execute_core(manager, &other, &other_response) || !other_response.accepted) goto done;
        }
        if (pt_read_owned_call(manager, req.tool_call_id, req.user_id, &observed) || observed.state != PT_ST_COMPLETED) goto done;
        rc = 0;
        response.accepted = 1;
    }
    if (argc == 5) {
        pt_call *call = malloc(sizeof(*call));
        char *encoded = malloc(PT_RECORD_CAP);
        if (call && encoded && !pt_read_owned_call(manager, req.tool_call_id, req.user_id, call) &&
            pt_store_record_encode(encoded, PT_RECORD_CAP, call)) puts(encoded);
        free(call);
        free(encoded);
    } else if (!rc && response.accepted) puts(response.output_json);
    result = !rc && response.accepted ? 0 : 1;
done:
    pt_manager_destroy(manager);
    free(manager);
    return result;
}
#endif
