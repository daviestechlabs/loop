/* Strict, I/O-free encounter adapters for the manager's state transaction. */
#ifndef PT_ENCOUNTER_H
#define PT_ENCOUNTER_H
#include "pt_dnd_state.h"

int pt_encounter_path(const char *directory, const char *input, char *path, size_t cap,
                       int *create, int *read_only);
int pt_encounter_transition(const char *before, char *after, size_t cap, pt_call *call);
int pt_encounter_canonical(const char *json, char *out, size_t cap, pt_dnd_identity *identity);
#define PT_INITIATIVE_DRAWS_CAP 512u
int pt_encounter_is_initiative(const char *input);
/* Entropy occurs only in preparation, never in transition or recovery. */
int pt_encounter_prepare_initiative(const char *before, const char *campaign, const pt_call *call,
                                    char draws[PT_INITIATIVE_DRAWS_CAP]);
int pt_encounter_initiative_transition(const char *before, const char *campaign, const char *draws,
                                       char *after, size_t cap, pt_call *call);
#endif
