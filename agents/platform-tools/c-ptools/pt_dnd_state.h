/* Bounded recovery for the two built-in D&D state tools. One serialized writer. */
#ifndef PT_DND_STATE_H
#define PT_DND_STATE_H
#include "pt_store.h"

enum { PT_DND_REJECTED = -1, PT_DND_OK = 0, PT_DND_PENDING = 1 };

typedef struct {
    char owner[PT_ID], campaign[128], object[128];
    int64_t version;
} pt_dnd_identity;

int pt_dnd_state_tool(const char *tool_id);
/* Complete command syntax, without state access. */
int pt_dnd_command_valid(const char *tool_id, const char *input);
/* Campaign ownership comes only from validated server state. allow_create
 * permits a new campaign's absent state, never an absent encounter parent. */
int pt_dnd_authorized(const char *state_dir, const char *tool_id, const char *input,
                       const char *user, int allow_create);
int pt_dnd_recover(const char *state_dir, const char *artifact_dir, pt_store *store, int restore_private);
/* Require a durable queued record. Pending I/O must not become terminal failure. */
int pt_dnd_run(const char *state_dir, const char *artifact_dir, pt_store *store, pt_call *call);
#endif
