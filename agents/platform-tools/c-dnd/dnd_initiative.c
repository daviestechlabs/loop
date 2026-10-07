/* dnd_initiative.c — pure-C initiative / combat kernel. */

#include "dnd_initiative.h"
#include "../../../contracts/handler-base/c-pb/pb_dnd_action.h"

#include <stdio.h>
#include <string.h>

static int is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int dnd_id_ok_v1(const char *id) {
    size_t n, i;
    if (!id || !id[0]) {
        return 0;
    }
    n = strlen(id);
    if (n > 64) {
        return 0;
    }
    if (!is_alnum(id[0])) {
        return 0;
    }
    for (i = 1; i < n; i++) {
        char c = id[i];
        if (!is_alnum(c) && c != '.' && c != '_' && c != '-') {
            return 0;
        }
    }
    return 1;
}

int dnd_condition_ok_v1(const char *condition) {
    return condition && dnd_condition_number(condition) != 0;
}

int dnd_damage_type_ok_v1(const char *dtype) {
    return dtype && dnd_damage_number(dtype) != 0;
}

static int conditions_sorted_unique(const dnd_participant_v1 *p) {
    for (int i = 0; i < p->n_conditions; ++i) {
        if (!dnd_condition_ok_v1(p->conditions[i]) ||
            (i > 0 && strcmp(p->conditions[i - 1], p->conditions[i]) >= 0)) return 0;
    }
    return 1;
}

int dnd_participant_ok_v1(const dnd_participant_v1 *p) {
    size_t name_len;
    if (!p) {
        return 0;
    }
    if (!dnd_id_ok_v1(p->id)) {
        return 0;
    }
    name_len = strlen(p->name);
    if (name_len == 0 || name_len > 120) {
        return 0;
    }
    /* trim-space check: reject leading/trailing space */
    if (p->name[0] == ' ' || p->name[name_len - 1] == ' ') {
        return 0;
    }
    if (p->initiative < -100 || p->initiative > 100) {
        return 0;
    }
    if (p->max_hp < 0 || p->max_hp > 100000) {
        return 0;
    }
    if (p->current_hp < 0 || p->current_hp > p->max_hp) {
        return 0;
    }
    if (p->n_conditions < 0 || p->n_conditions > DND_MAX_CONDITIONS) {
        return 0;
    }
    return conditions_sorted_unique(p);
}

static int participant_less(const dnd_participant_v1 *a, const dnd_participant_v1 *b) {
    /* sort key: higher initiative first; then id asc */
    if (a->initiative != b->initiative) {
        return a->initiative > b->initiative;
    }
    return strcmp(a->id, b->id) < 0;
}

void dnd_sort_participants_v1(dnd_participant_v1 *ps, int n) {
    int i, j;
    if (!ps || n <= 1) {
        return;
    }
    for (i = 1; i < n; i++) {
        dnd_participant_v1 key = ps[i];
        j = i - 1;
        while (j >= 0 && participant_less(&key, &ps[j])) {
            ps[j + 1] = ps[j];
            j--;
        }
        ps[j + 1] = key;
    }
}

int dnd_participant_index_v1(const dnd_participant_v1 *ps, int n, const char *id) {
    int i;
    if (!ps || !id) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(ps[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int dnd_encounter_validate_v1(const dnd_encounter_v1 *e) {
    int i;
    if (!e) {
        return DND_ERR_ARGUMENT;
    }
    if (!dnd_id_ok_v1(e->campaign_id) || !dnd_id_ok_v1(e->encounter_id) ||
        !e->owner_user_id[0] || e->owner_user_id[0] == ' ') {
        return DND_ERR_VALIDATION;
    }
    if (e->version < 1 || e->round < 1) {
        return DND_ERR_VALIDATION;
    }
    if (strcmp(e->status, "active") != 0 && strcmp(e->status, "ended") != 0) {
        return DND_ERR_VALIDATION;
    }
    if (e->n_participants < 0 || e->n_participants > DND_MAX_PARTICIPANTS) {
        return DND_ERR_VALIDATION;
    }
    if (e->n_participants == 0 && e->active_index != -1) {
        return DND_ERR_VALIDATION;
    }
    if (e->active_index < -1 || e->active_index >= e->n_participants) {
        return DND_ERR_VALIDATION;
    }
    if (strcmp(e->status, "ended") == 0 && e->active_index != -1) {
        return DND_ERR_VALIDATION;
    }
    for (i = 0; i < e->n_participants; i++) {
        int j;
        if (!dnd_participant_ok_v1(&e->participants[i])) {
            return DND_ERR_VALIDATION;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(e->participants[j].id, e->participants[i].id) == 0) {
                return DND_ERR_VALIDATION;
            }
        }
        if (i > 0) {
            const dnd_participant_v1 *prev = &e->participants[i - 1];
            const dnd_participant_v1 *cur = &e->participants[i];
            if (prev->initiative < cur->initiative ||
                (prev->initiative == cur->initiative && strcmp(prev->id, cur->id) > 0)) {
                return DND_ERR_VALIDATION;
            }
        }
    }
    return DND_OK;
}

static void copy_id(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || cap == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void dnd_encounter_bump_version_v1(dnd_encounter_v1 *e) {
    if (e) {
        e->version++;
    }
}

int dnd_encounter_start_v1(
    dnd_encounter_v1 *e,
    const char *campaign_id,
    const char *encounter_id,
    const char *owner_user_id
) {
    if (!e || !dnd_id_ok_v1(campaign_id) || !dnd_id_ok_v1(encounter_id) || !owner_user_id ||
        !owner_user_id[0]) {
        return DND_ERR_ARGUMENT;
    }
    memset(e, 0, sizeof(*e));
    copy_id(e->campaign_id, sizeof(e->campaign_id), campaign_id);
    copy_id(e->encounter_id, sizeof(e->encounter_id), encounter_id);
    copy_id(e->owner_user_id, sizeof(e->owner_user_id), owner_user_id);
    e->version = 1;
    memcpy(e->status, "active", 7);
    e->round = 1;
    e->active_index = -1;
    e->n_participants = 0;
    return dnd_encounter_validate_v1(e);
}

int dnd_encounter_add_v1(dnd_encounter_v1 *e, const dnd_participant_v1 *p) {
    dnd_participant_v1 copy;
    char active_id[DND_ID_CAP] = "";
    if (!e || !p) {
        return DND_ERR_ARGUMENT;
    }
    if (strcmp(e->status, "active") != 0) {
        return DND_ERR_STATE;
    }
    if (e->n_participants >= DND_MAX_PARTICIPANTS) {
        return DND_ERR_CAPACITY;
    }
    if (!dnd_participant_ok_v1(p)) {
        return DND_ERR_VALIDATION;
    }
    if (dnd_participant_index_v1(e->participants, e->n_participants, p->id) >= 0) {
        return DND_ERR_VALIDATION;
    }
    if (e->active_index >= 0 && e->active_index < e->n_participants)
        copy_id(active_id, sizeof(active_id), e->participants[e->active_index].id);
    copy = *p;
    e->participants[e->n_participants++] = copy;
    dnd_sort_participants_v1(e->participants, e->n_participants);
    if (active_id[0])
        e->active_index = dnd_participant_index_v1(e->participants, e->n_participants, active_id);
    dnd_encounter_bump_version_v1(e);
    return dnd_encounter_validate_v1(e);
}

int dnd_encounter_remove_v1(dnd_encounter_v1 *e, const char *participant_id) {
    int idx, i;
    char active_id[DND_ID_CAP];
    if (!e || !participant_id) {
        return DND_ERR_ARGUMENT;
    }
    if (strcmp(e->status, "active") != 0) {
        return DND_ERR_STATE;
    }
    idx = dnd_participant_index_v1(e->participants, e->n_participants, participant_id);
    if (idx < 0) {
        return DND_ERR_NOT_FOUND;
    }
    active_id[0] = '\0';
    if (e->active_index >= 0 && e->active_index < e->n_participants) {
        copy_id(active_id, sizeof(active_id), e->participants[e->active_index].id);
    }
    for (i = idx; i < e->n_participants - 1; i++) {
        e->participants[i] = e->participants[i + 1];
    }
    e->n_participants--;
    if (e->n_participants == 0) {
        e->active_index = -1;
    } else if (active_id[0]) {
        int a = dnd_participant_index_v1(e->participants, e->n_participants, active_id);
        if (a < 0) {
            /* removed active → next slot (idx) or wrap */
            if (idx >= e->n_participants) {
                e->active_index = 0;
            } else {
                e->active_index = idx;
            }
        } else {
            e->active_index = a;
        }
    }
    dnd_encounter_bump_version_v1(e);
    return dnd_encounter_validate_v1(e);
}

int dnd_encounter_advance_v1(dnd_encounter_v1 *e) {
    if (!e) {
        return DND_ERR_ARGUMENT;
    }
    if (strcmp(e->status, "active") != 0 || e->n_participants == 0) {
        return DND_ERR_STATE;
    }
    if (e->active_index < 0) {
        e->active_index = 0;
    } else {
        e->active_index++;
        if (e->active_index >= e->n_participants) {
            e->active_index = 0;
            e->round++;
        }
    }
    dnd_encounter_bump_version_v1(e);
    return dnd_encounter_validate_v1(e);
}

int dnd_encounter_end_v1(dnd_encounter_v1 *e) {
    if (!e) {
        return DND_ERR_ARGUMENT;
    }
    memcpy(e->status, "ended", 6);
    e->active_index = -1;
    dnd_encounter_bump_version_v1(e);
    return dnd_encounter_validate_v1(e);
}

static int find_mut(dnd_encounter_v1 *e, const char *id, int *out_idx) {
    int idx;
    if (!e || !id) {
        return DND_ERR_ARGUMENT;
    }
    if (strcmp(e->status, "active") != 0) {
        return DND_ERR_STATE;
    }
    idx = dnd_participant_index_v1(e->participants, e->n_participants, id);
    if (idx < 0) {
        return DND_ERR_NOT_FOUND;
    }
    *out_idx = idx;
    return DND_OK;
}

int dnd_encounter_damage_v1(dnd_encounter_v1 *e, const char *participant_id, int amount) {
    int idx, rc;
    if (amount <= 0) {
        return DND_ERR_VALIDATION;
    }
    rc = find_mut(e, participant_id, &idx);
    if (rc != DND_OK) {
        return rc;
    }
    e->participants[idx].current_hp -= amount;
    if (e->participants[idx].current_hp < 0) {
        e->participants[idx].current_hp = 0;
    }
    dnd_encounter_bump_version_v1(e);
    return DND_OK;
}

int dnd_encounter_heal_v1(dnd_encounter_v1 *e, const char *participant_id, int amount) {
    int idx, rc;
    if (amount <= 0) {
        return DND_ERR_VALIDATION;
    }
    rc = find_mut(e, participant_id, &idx);
    if (rc != DND_OK) {
        return rc;
    }
    if (amount >= e->participants[idx].max_hp - e->participants[idx].current_hp) {
        e->participants[idx].current_hp = e->participants[idx].max_hp;
    } else e->participants[idx].current_hp += amount;
    dnd_encounter_bump_version_v1(e);
    return DND_OK;
}

int dnd_encounter_condition_add_v1(
    dnd_encounter_v1 *e,
    const char *participant_id,
    const char *condition
) {
    int idx, rc, i, j;
    dnd_participant_v1 *p;
    if (!dnd_condition_ok_v1(condition)) {
        return DND_ERR_VALIDATION;
    }
    rc = find_mut(e, participant_id, &idx);
    if (rc != DND_OK) {
        return rc;
    }
    p = &e->participants[idx];
    for (i = 0; i < p->n_conditions; i++) {
        if (strcmp(p->conditions[i], condition) == 0) {
            return DND_ERR_VALIDATION; /* duplicate */
        }
    }
    if (p->n_conditions >= DND_MAX_CONDITIONS) {
        return DND_ERR_CAPACITY;
    }
    /* insert sorted */
    for (i = 0; i < p->n_conditions; i++) {
        if (strcmp(condition, p->conditions[i]) < 0) {
            break;
        }
    }
    for (j = p->n_conditions; j > i; j--) {
        memcpy(p->conditions[j], p->conditions[j - 1], 32);
    }
    copy_id(p->conditions[i], 32, condition);
    p->n_conditions++;
    dnd_encounter_bump_version_v1(e);
    return DND_OK;
}

int dnd_encounter_condition_remove_v1(
    dnd_encounter_v1 *e,
    const char *participant_id,
    const char *condition
) {
    int idx, rc, i, j;
    dnd_participant_v1 *p;
    rc = find_mut(e, participant_id, &idx);
    if (rc != DND_OK) {
        return rc;
    }
    p = &e->participants[idx];
    for (i = 0; i < p->n_conditions; i++) {
        if (strcmp(p->conditions[i], condition) == 0) {
            for (j = i; j < p->n_conditions - 1; j++) {
                memcpy(p->conditions[j], p->conditions[j + 1], 32);
            }
            p->n_conditions--;
            dnd_encounter_bump_version_v1(e);
            return DND_OK;
        }
    }
    return DND_ERR_NOT_FOUND;
}

int dnd_encounter_active_id_v1(const dnd_encounter_v1 *e, char *out, size_t out_cap) {
    if (!out || out_cap == 0) {
        return DND_ERR_ARGUMENT;
    }
    out[0] = '\0';
    if (!e || e->active_index < 0 || e->active_index >= e->n_participants) {
        return DND_OK;
    }
    copy_id(out, out_cap, e->participants[e->active_index].id);
    return DND_OK;
}

int dnd_encounter_summary_v1(const dnd_encounter_v1 *e, char *out, size_t out_cap) {
    if (!e || !out || out_cap == 0) {
        return DND_ERR_ARGUMENT;
    }
    if (strcmp(e->status, "ended") == 0) {
        snprintf(out, out_cap, "Ended encounter %s at version %lld.", e->encounter_id,
                 (long long)e->version);
        return DND_OK;
    }
    if (e->n_participants == 0) {
        snprintf(out, out_cap, "Encounter %s has an empty initiative order.", e->encounter_id);
        return DND_OK;
    }
    if (e->active_index < 0 || e->active_index >= e->n_participants) {
        snprintf(
            out,
            out_cap,
            "Encounter %s has %d participant(s); initiative is ready to advance.",
            e->encounter_id,
            e->n_participants
        );
        return DND_OK;
    }
    {
        const dnd_participant_v1 *a = &e->participants[e->active_index];
        snprintf(
            out,
            out_cap,
            "Round %d; %s is active at initiative %d; %d participant(s).",
            e->round,
            a->name,
            a->initiative,
            e->n_participants
        );
    }
    return DND_OK;
}
