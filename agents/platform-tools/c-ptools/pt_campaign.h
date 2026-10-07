/* Strict, I/O-free campaign adapters for the manager's state transaction. */
#ifndef PT_CAMPAIGN_H
#define PT_CAMPAIGN_H
#include "pt_dnd_state.h"
#include "../c-dnd/dnd_initiative.h"

int pt_campaign_path(const char *directory, const char *input, char *path, size_t cap,
                      int *create, int *read_only);
int pt_campaign_transition(const char *before, char *after, size_t cap, pt_call *call);
int pt_campaign_canonical(const char *json, char *out, size_t cap, pt_dnd_identity *identity);
/* Dedicated read-only name resolution uses the same owned campaign snapshot. */
int pt_scene_presence_path(const char *directory, const char *input, char *path, size_t cap,
                           int *create, int *read_only);
int pt_scene_presence_transition(const char *before, char *after, size_t cap, pt_call *call);
/* Copy selected, fully validated roster records from one owned version. */
int pt_campaign_select(const char *json, const char *campaign, const char *owner, int64_t version,
                        const char ids[][DND_ID_CAP], size_t count, dnd_participant_v1 *out);
#endif
