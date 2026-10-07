#ifndef PT_WORKER_H
#define PT_WORKER_H

#include "pt_store.h"

typedef struct pt_worker {
    char workspace_root[512]; /* single default workspace root (TOOL_WORKSPACE_ROOT) */
    char artifact_dir[512];
    char dnd_state_dir[512];
} pt_worker;

void pt_worker_init(pt_worker *w, const char *ws_root, const char *artifact_dir, const char *dnd_dir);

/* Execute tool; fills output_json/summary/error on call. Returns 0 success. */
int pt_worker_dispatch(pt_worker *w, pt_call *call);

#endif
