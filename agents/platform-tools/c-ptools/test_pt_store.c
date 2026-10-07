#include "pt_store.h"
#include "toolstore.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void expect(int condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static int count_record(const pt_call *call, void *context) {
    size_t *count = context;
    if (!call->in_use || !call->tool_call_id[0]) return -1;
    ++*count;
    return 0;
}

int main(void) {
    static const char baseline[] =
        "{\"calls\":{\"legacy-1\":{\"tool_call_id\":\"legacy-1\","
        "\"state\":\"completed\",\"created_at\":1,\"updated_at\":2,"
        "\"output_json\":\"{\\\"text\\\":\\\"a } brace\\\"}\"}}}";
    pt_store *store = calloc(1u, sizeof(*store));
    pt_store *loaded = calloc(1u, sizeof(*loaded));
    pt_call *call = calloc(1u, sizeof(*call));
    char temporary[] = "/tmp/pt-store-contract-XXXXXX";
    char path[512], directory[512], filename[80], record[1024];
    char previous[sizeof(baseline)];
    FILE *file;
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    int i, history = PT_MAX_CALLS * 2;
    const char *history_env = getenv("PT_STORE_HISTORY_TEST_CALLS");
    if (history_env) {
        char *end = NULL;
        long value = strtol(history_env, &end, 10);
        if (!end || *end || value < PT_MAX_CALLS * 2 || value > 100000) return 1;
        history = (int)value;
    }
    if (!store || !loaded || !call || !mkdtemp(temporary)) return 1;
    snprintf(path, sizeof(path), "%s/calls.json", temporary);
    expect(ts_atomic_write(path, baseline, sizeof(baseline) - 1u, 0600) == TS_OK,
           "legacy baseline fixture");
    expect(pt_store_open(store, path) == 0 && pt_store_get(store, "legacy-1") != NULL,
           "legacy snapshot loads quoted braces");
    expect(stat(store->index_path, &st) == 0 && (st.st_mode & 0777) == 0600,
           "derived index has private file permissions");
    strcpy(call->tool_call_id, "new-1");
    strcpy(call->parent_turn_id, "turn-1");
    strcpy(call->parent_task_id, "task-1");
    strcpy(call->approval_id, "approval-1");
    strcpy(call->user_id, "alice");
    strcpy(call->session_id, "table-1");
    strcpy(call->agent_id, "dnd-agent");
    strcpy(call->tool_id, "dnd-dice-roll");
    strcpy(call->idempotency_key, "once-1");
    strcpy(call->input_json, "{\"expression\":\"2d6\"}");
    strcpy(call->summary, "quotes \" braces {} tabs\t newline\n UTF-8 Tára");
    memset(call->output_json, 'x', sizeof(call->output_json) - 1u);
    call->output_json[sizeof(call->output_json) - 2u] = '\\';
    call->state = PT_ST_COMPLETED;
    call->created_at = 10;
    call->updated_at = 20;
    call->in_use = 1;
    expect(pt_store_put(store, call) == 0, "maximum result record persists");
    expect(pt_store_open(loaded, path) == 0, "reload snapshot and shards");
    {
        pt_call *saved = pt_store_get(loaded, "new-1");
        expect(saved && memcmp(saved, call, sizeof(*call)) == 0,
               "restart preserves all record fields and bytes");
    }
    file = fopen(path, "rb");
    expect(file && fread(previous, 1u, sizeof(previous) - 1u, file) == sizeof(previous) - 1u &&
               memcmp(previous, baseline, sizeof(previous) - 1u) == 0,
           "writes preserve the original legacy snapshot");
    if (file) fclose(file);
    expect(ts_record_dir(path, directory, sizeof(directory)) == TS_OK &&
               ts_shard_name("new-1", filename, sizeof(filename)) == TS_OK,
           "derive hashed record location");
    snprintf(record, sizeof(record), "%s/%s", directory, filename);
    expect(stat(record, &st) == 0 && (st.st_mode & 0777) == 0600,
           "call record has private file permissions");
    expect(ts_atomic_write(record, "{", 1u, 0600) == TS_OK && pt_store_open(loaded, path) != 0,
           "truncated shard rejects startup");
    expect(pt_store_put(store, call) == 0, "restore the test record");
    {
        char wrong[1024];
        snprintf(wrong, sizeof(wrong), "%s/wrong.json", directory);
        expect(rename(record, wrong) == 0 && pt_store_open(loaded, path) != 0,
               "shard filename must bind the record identity");
        expect(rename(wrong, record) == 0, "restore shard name");
    }
    call->output_json[0] = '\0';
    for (i = 2; i < history; ++i) {
        snprintf(call->tool_call_id, sizeof(call->tool_call_id), "new-%d", i);
        snprintf(call->idempotency_key, sizeof(call->idempotency_key), "once-%d", i);
        expect(pt_store_put(store, call) == 0, "completed history can exceed the memory cache");
    }
    strcpy(call->tool_call_id, "after-cache-capacity");
    strcpy(call->idempotency_key, "after-cache-capacity");
    expect(pt_store_put(store, call) == 0,
           "new calls remain available after the cache fills");
    expect(pt_store_open(loaded, path) == 0 && pt_store_get(loaded, "after-cache-capacity") != NULL,
           "history beyond cache capacity survives restart");
    {
        pt_call *saved = pt_store_get(loaded, "new-1");
        expect(saved && strlen(saved->output_json) == PT_OUT - 1u && saved->output_json[PT_OUT - 2u] == '\\',
               "old maximum-size result stays intact after more calls and restart");
        saved = pt_store_by_idem(loaded, "alice", "table-1", "dnd-agent", "dnd-dice-roll", "once-1");
        expect(saved && strcmp(saved->tool_call_id, "new-1") == 0,
               "old idempotency key still resolves to its original result");
        expect(pt_store_by_idem(loaded, "bob", "table-1", "dnd-agent", "dnd-dice-roll", "once-1") == NULL,
               "history lookup keeps owner isolation");
    }
    {
        size_t visited = 0;
        expect(pt_store_visit(loaded, count_record, &visited) == 0 && visited == (size_t)history + 1u,
               "startup artifact traversal covers all retained history");
        strcpy(call->tool_call_id, "duplicate-retry");
        strcpy(call->idempotency_key, "once-1");
        expect(pt_store_put(loaded, call) != 0 && pt_store_get(loaded, "duplicate-retry") == NULL,
               "conflicting retry identity cannot create another durable record");
        expect(pt_store_get(loaded, "after-cache-capacity") != NULL,
               "a rejected retry conflict does not poison valid history");
        expect(pt_store_by_idem(loaded, "alice", "other-session", "dnd-agent", "dnd-dice-roll", "once-1") == NULL &&
               pt_store_by_idem(loaded, "alice", "table-1", "other-agent", "dnd-dice-roll", "once-1") == NULL &&
               pt_store_by_idem(loaded, "alice", "table-1", "dnd-agent", "other-tool", "once-1") == NULL,
               "all retry scope fields remain isolated");
    }
    /* Force a disk lookup after restart, then verify corrupted history blocks new writes. */
    expect(pt_store_open(loaded, path) == 0, "fresh index before corruption test");
    expect(ts_shard_name("new-2", filename, sizeof(filename)) == TS_OK, "derive cold record name");
    snprintf(record, sizeof(record), "%s/%s", directory, filename);
    expect(ts_atomic_write(record, "{", 1u, 0600) == TS_OK && pt_store_get(loaded, "new-2") == NULL,
           "cold malformed record fails closed");
    strcpy(call->tool_call_id, "after-corruption");
    strcpy(call->idempotency_key, "after-corruption");
    expect(pt_store_put(loaded, call) != 0, "failed index lookup cannot admit new side effects");
    pt_store_close(store);
    pt_store_close(loaded);
    dir = opendir(directory);
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            snprintf(record, sizeof(record), "%s/%s", directory, entry->d_name);
            unlink(record);
        }
        closedir(dir);
    }
    rmdir(directory);
    unlink(path);
    rmdir(temporary);
    free(store);
    free(loaded);
    free(call);
    if (failures) return 1;
    puts("ALL PASS durable tool store");
    return 0;
}
