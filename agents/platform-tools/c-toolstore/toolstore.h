/* toolstore.h — pure-C platform-tools CAS/path policy (no Go). */
#ifndef PLATFORM_TOOLS_TOOLSTORE_H
#define PLATFORM_TOOLS_TOOLSTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TS_OK = 0,
    TS_ERR = 1
};

#define TS_ID_MAX 128
#define TS_SHARD_NAME 80 /* 64 hex + ".json" + NUL */
#define TS_PATH 512
#define TS_IDEM 1024
#define TS_MSG 256

/* tool_call_id: ^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$ */
int ts_valid_id(const char *id);

/* SHA-256(call_id) hex + ".json" into out (cap >= TS_SHARD_NAME). */
int ts_shard_name(const char *call_id, char *out, size_t cap);

/* store path → record dir: strip last extension, append ".d". */
int ts_record_dir(const char *store_path, char *out, size_t cap);

/* Join user|session|agent|tool|idem with 0x1f; empty idem → empty out, returns TS_OK. */
int ts_idempotency_key(const char *user_id, const char *session_id, const char *agent_id,
                       const char *tool_id, const char *idem_key, char *out, size_t cap);

/*
 * Same tool request: compare 8 string fields.
 * Returns 1 if equal, 0 if not.
 */
int ts_same_request(const char *idem_a, const char *task_a, const char *turn_a, const char *user_a,
                    const char *session_a, const char *agent_a, const char *tool_a, const char *input_a,
                    const char *idem_b, const char *task_b, const char *turn_b, const char *user_b,
                    const char *session_b, const char *agent_b, const char *tool_b, const char *input_b);

/* Terminal states: 4 rejected, 7 completed, 8 failed, 9 canceled (protobuf enums). */
int ts_is_terminal(int state);
/* Also accept protobuf name / short name. */
int ts_is_terminal_name(const char *name);

/* Filename basename must equal ts_shard_name(call_id). */
int ts_check_shard_identity(const char *call_id, const char *filename);

/* Atomic write: mkdir parent, fsync temp, rename, fsync parent. mode e.g. 0600. */
int ts_atomic_write(const char *path, const void *data, size_t len, unsigned mode);

/*
 * Workspace-bounded relative path (pure policy, no realpath):
 *   reject absolute raw, NUL, empty, or clean that escapes root via "..".
 *   reject cleaned path that is empty/"." (root itself).
 * Writes root + "/" + cleaned into out (POSIX separators).
 */
int ts_bounded_path(const char *root, const char *raw, char *out, size_t cap);

/* dry_run 1 → "dry-ran", else "applied". */
int ts_edit_mode(int dry_run, char *out, size_t cap);

/*
 * Content-Type → artifact extension:
 *   application/json → .json, text/x-diff → .patch, else .txt
 */
int ts_artifact_extension(const char *content_type, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
