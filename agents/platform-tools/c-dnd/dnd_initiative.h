/* dnd_initiative.h — pure-C D&D initiative / encounter combat math.
 *
 * No heap. No I/O. Mirrors agents/platform-tools/dnd_initiative.go pure-CPU
 * rules (validate, sort, damage/heal, conditions, advance). Go NATS worker
 * residual remains until platform-tools is a c-* service.
 */
#ifndef PLATFORM_TOOLS_DND_INITIATIVE_H
#define PLATFORM_TOOLS_DND_INITIATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DND_OK = 0,
    DND_ERR_ARGUMENT = 1,
    DND_ERR_VALIDATION = 2,
    DND_ERR_NOT_FOUND = 3,
    DND_ERR_CAPACITY = 4,
    DND_ERR_STATE = 5
};

enum {
    DND_MAX_PARTICIPANTS = 256,
    DND_MAX_CONDITIONS = 16,
    DND_ID_CAP = 128,
    DND_NAME_CAP = 121,
    DND_SUMMARY_CAP = 256
};

typedef struct dnd_participant_v1 {
    char id[DND_ID_CAP];
    char name[DND_NAME_CAP];
    int initiative; /* -100..100 */
    int max_hp;
    int current_hp;
    char conditions[DND_MAX_CONDITIONS][32];
    int n_conditions;
} dnd_participant_v1;

typedef struct dnd_encounter_v1 {
    char campaign_id[DND_ID_CAP];
    char encounter_id[DND_ID_CAP];
    char owner_user_id[DND_ID_CAP];
    int64_t version;
    char status[16]; /* "active" | "ended" */
    int round;
    int active_index; /* -1 if none */
    dnd_participant_v1 participants[DND_MAX_PARTICIPANTS];
    int n_participants;
} dnd_encounter_v1;

/* Safe id: ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ */
int dnd_id_ok_v1(const char *id);

int dnd_condition_ok_v1(const char *condition);
int dnd_damage_type_ok_v1(const char *dtype);

int dnd_participant_ok_v1(const dnd_participant_v1 *p);

/* Sort initiative desc, id asc (stable for equal initiative via id). */
void dnd_sort_participants_v1(dnd_participant_v1 *ps, int n);

int dnd_participant_index_v1(const dnd_participant_v1 *ps, int n, const char *id);

/* Validate full encounter state. */
int dnd_encounter_validate_v1(const dnd_encounter_v1 *e);

/* Start empty active encounter at version 1, round 1, active_index -1. */
int dnd_encounter_start_v1(
    dnd_encounter_v1 *e,
    const char *campaign_id,
    const char *encounter_id,
    const char *owner_user_id
);

int dnd_encounter_add_v1(dnd_encounter_v1 *e, const dnd_participant_v1 *p);
int dnd_encounter_remove_v1(dnd_encounter_v1 *e, const char *participant_id);

/* Advance turn order; wraps round. Requires n_participants > 0 and status active. */
int dnd_encounter_advance_v1(dnd_encounter_v1 *e);

int dnd_encounter_end_v1(dnd_encounter_v1 *e);

/* Damage/heal amount must be > 0. HP clamped to [0, max_hp]. */
int dnd_encounter_damage_v1(dnd_encounter_v1 *e, const char *participant_id, int amount);
int dnd_encounter_heal_v1(dnd_encounter_v1 *e, const char *participant_id, int amount);

int dnd_encounter_condition_add_v1(
    dnd_encounter_v1 *e,
    const char *participant_id,
    const char *condition
);
int dnd_encounter_condition_remove_v1(
    dnd_encounter_v1 *e,
    const char *participant_id,
    const char *condition
);

/* Active participant id or empty string in out (NUL-terminated). */
int dnd_encounter_active_id_v1(const dnd_encounter_v1 *e, char *out, size_t out_cap);

/* Human summary line (NUL-terminated). */
int dnd_encounter_summary_v1(const dnd_encounter_v1 *e, char *out, size_t out_cap);

/* Bump version after successful mutation (callers that stage multi-step can set). */
void dnd_encounter_bump_version_v1(dnd_encounter_v1 *e);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_TOOLS_DND_INITIATIVE_H */
