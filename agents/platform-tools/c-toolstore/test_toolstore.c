#define _POSIX_C_SOURCE 200809L
#include "toolstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

static void test_valid_id(void) {
    expect(ts_valid_id("call-sharded") == 1, "good id");
    expect(ts_valid_id("A") == 1, "single");
    expect(ts_valid_id("") == 0, "empty");
    expect(ts_valid_id("-bad") == 0, "leading dash");
    expect(ts_valid_id("bad id") == 0, "space");
    expect(ts_valid_id("x:y_z.1-2") == 1, "allowed chars");
}

static void test_shard_name(void) {
    char out[TS_SHARD_NAME];
    /* sha256("call-sharded") + .json */
    const char *want = "c4f6a68630366621c00049628b8c0a8ca825f8263cc47f1f394eb136f4e96680.json";
    expect(ts_shard_name("call-sharded", out, sizeof(out)) == TS_OK, "shard ok");
    expect(strcmp(out, want) == 0, "shard hex matches go");
    expect(ts_shard_name("-nope", out, sizeof(out)) == TS_ERR, "bad id shard");
}

static void test_record_dir(void) {
    char out[TS_PATH];
    expect(ts_record_dir("/tmp/tool-calls.json", out, sizeof(out)) == TS_OK, "rd ok");
    expect(strcmp(out, "/tmp/tool-calls.d") == 0, "rd strip ext");
    expect(ts_record_dir("tool-calls.json", out, sizeof(out)) == TS_OK, "rd rel");
    expect(strcmp(out, "tool-calls.d") == 0, "rd rel val");
    expect(ts_record_dir("/tmp/noext", out, sizeof(out)) == TS_OK, "rd noext");
    expect(strcmp(out, "/tmp/noext.d") == 0, "rd noext val");
}

static void test_idem(void) {
    char out[TS_IDEM];
    char expect_key[TS_IDEM];
    expect(ts_idempotency_key("u", "s", "a", "t", "", out, sizeof(out)) == TS_OK, "empty idem");
    expect(out[0] == '\0', "empty out");
    expect(ts_idempotency_key("u", "s", "a", "t", "k", out, sizeof(out)) == TS_OK, "idem ok");
    snprintf(expect_key, sizeof(expect_key), "u%cs%ca%ct%ck", 0x1f, 0x1f, 0x1f, 0x1f);
    expect(strcmp(out, expect_key) == 0, "idem join");
}

static void test_same(void) {
    expect(ts_same_request("k", "task", "turn", "u", "s", "a", "t", "{}", "k", "task", "turn", "u", "s", "a",
                           "t", "{}") == 1,
           "same");
    expect(ts_same_request("k", "task", "turn", "u", "s", "a", "t", "{}", "k", "task", "turn", "u", "s", "a",
                           "t", "{x}") == 0,
           "diff input");
}

static void test_terminal(void) {
    expect(ts_is_terminal(7) == 1, "completed");
    expect(ts_is_terminal(8) == 1, "failed");
    expect(ts_is_terminal(9) == 1, "canceled");
    expect(ts_is_terminal(4) == 1, "rejected");
    expect(ts_is_terminal(6) == 0, "running");
    expect(ts_is_terminal_name("completed") == 1, "name completed");
    expect(ts_is_terminal_name("running") == 0, "name running");
}

static void test_check_shard(void) {
    char name[TS_SHARD_NAME];
    expect(ts_shard_name("call-one", name, sizeof(name)) == TS_OK, "name");
    expect(ts_check_shard_identity("call-one", name) == TS_OK, "match base");
    expect(ts_check_shard_identity("call-one", "wrong.json") == TS_ERR, "mismatch");
    {
        char path[TS_PATH];
        snprintf(path, sizeof(path), "/tmp/%s", name);
        expect(ts_check_shard_identity("call-one", path) == TS_OK, "match path");
    }
}

static void test_atomic_write(void) {
    char path[] = "/tmp/c-toolstore-test-XXXXXX";
    int fd = mkstemp(path);
    const char *payload = "{\n  \"ok\": true\n}";
    struct stat st;
    char buf[64];
    FILE *f;
    expect(fd >= 0, "mkstemp");
    if (fd >= 0)
        close(fd);
    unlink(path);
    /* Use path as destination (no longer exists). */
    expect(ts_atomic_write(path, payload, strlen(payload), 0600) == TS_OK, "write");
    expect(stat(path, &st) == 0, "stat");
    expect((st.st_mode & 0777) == 0600, "mode 0600");
    f = fopen(path, "r");
    expect(f != NULL, "open");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        expect(strcmp(buf, payload) == 0, "content");
    }
    unlink(path);
}

static void test_bounded_path(void) {
    char out[TS_PATH];
    char mode[16];
    char ext[16];

    expect(ts_bounded_path("/ws", "foo/bar", out, sizeof(out)) == TS_OK &&
               strcmp(out, "/ws/foo/bar") == 0,
           "bounded ok");
    expect(ts_bounded_path("/ws", "foo/../bar", out, sizeof(out)) == TS_OK &&
               strcmp(out, "/ws/bar") == 0,
           "bounded clean");
    expect(ts_bounded_path("/ws", "../etc", out, sizeof(out)) == TS_ERR, "bounded escape");
    expect(ts_bounded_path("/ws", "/abs", out, sizeof(out)) == TS_ERR, "bounded abs");
    expect(ts_bounded_path("/ws", ".", out, sizeof(out)) == TS_ERR, "bounded root");
    expect(ts_bounded_path("/ws", "", out, sizeof(out)) == TS_ERR, "bounded empty");

    expect(ts_edit_mode(1, mode, sizeof(mode)) == TS_OK && strcmp(mode, "dry-ran") == 0, "edit dry");
    expect(ts_edit_mode(0, mode, sizeof(mode)) == TS_OK && strcmp(mode, "applied") == 0, "edit app");
    expect(ts_artifact_extension("application/json", ext, sizeof(ext)) == TS_OK &&
               strcmp(ext, ".json") == 0,
           "ext json");
    expect(ts_artifact_extension("text/x-diff", ext, sizeof(ext)) == TS_OK &&
               strcmp(ext, ".patch") == 0,
           "ext patch");
    expect(ts_artifact_extension("text/plain", ext, sizeof(ext)) == TS_OK && strcmp(ext, ".txt") == 0,
           "ext txt");
}

int main(void) {
    test_valid_id();
    test_shard_name();
    test_record_dir();
    test_idem();
    test_same();
    test_terminal();
    test_check_shard();
    test_atomic_write();
    test_bounded_path();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("ALL PASS c-toolstore unit\n");
    return 0;
}
