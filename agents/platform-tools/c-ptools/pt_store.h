#ifndef PT_STORE_H
#define PT_STORE_H

#include <stddef.h>
#include <stdint.h>

#define PT_MAX_CALLS 128 /* Decoded record cache only; not a retained-history limit. */
#define PT_ID 160
#define PT_STR 512
#define PT_INPUT 8192
#define PT_OUT 65536

enum {
    PT_ST_REQUESTED = 1,
    PT_ST_AWAITING_APPROVAL = 2,
    PT_ST_APPROVED = 3,
    PT_ST_REJECTED = 4,
    PT_ST_QUEUED = 5,
    PT_ST_RUNNING = 6,
    PT_ST_COMPLETED = 7,
    PT_ST_FAILED = 8,
    PT_ST_CANCELED = 9
};

typedef struct pt_call {
    char tool_call_id[PT_ID];
    char idempotency_key[PT_ID];
    char parent_task_id[PT_ID];
    char parent_turn_id[PT_ID];
    char user_id[PT_ID];
    char session_id[PT_ID];
    char agent_id[PT_ID];
    char tool_id[PT_ID];
    char input_json[PT_INPUT];
    char output_json[PT_OUT];
    char output_sha256[65];
    char output_artifact[80];
    char summary[PT_STR];
    char error[PT_STR];
    char approval_id[PT_ID];
    int state;
    int64_t created_at;
    int64_t updated_at;
    int in_use;
} pt_call;

#define PT_RECORD_CAP (6u * sizeof(pt_call) + 1024u)

int pt_store_record_decode(const char *json, pt_call *call);
size_t pt_store_record_encode(char *json, size_t capacity, const pt_call *call);

typedef struct pt_store {
    char path[512];
    pt_call calls[PT_MAX_CALLS];
    struct sqlite3 *index;
    char index_path[544];
    size_t next_slot;
    int failed;
} pt_store;

/* Zero-initialize before the first open. Reopen closes the prior private index. */
int pt_store_open(pt_store *s, const char *path);
void pt_store_close(pt_store *s);
int pt_store_visit(pt_store *s, int (*visit)(const pt_call *, void *), void *context);
int pt_store_save(pt_store *s);
/* Borrowed pointers remain valid until another cache miss or write. */
pt_call *pt_store_get(pt_store *s, const char *id);
pt_call *pt_store_by_idem(pt_store *s, const char *user, const char *session,
                          const char *agent, const char *tool, const char *idem);
int pt_store_put(pt_store *s, const pt_call *c);

const char *pt_state_name(int state);

#endif
