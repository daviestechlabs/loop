#include "dnd_campaign.h"

#include <string.h>

static int is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int dnd_camp_id_ok_v1(const char *id) {
    size_t n, i;
    if (!id || !id[0]) return 0;
    n = strlen(id);
    if (n > 64) return 0;
    if (!is_alnum(id[0])) return 0;
    for (i = 1; i < n; i++) {
        char c = id[i];
        if (!is_alnum(c) && c != '.' && c != '_' && c != '-') return 0;
    }
    return 1;
}

static const char *const k_scopes[] = {
    "shared_rulebook", "owned_rulebook", "campaign_canon", "session_transcript",
    "character_memory", NULL
};

int dnd_camp_knowledge_scope_ok_v1(const char *scope) {
    size_t i;
    if (!scope || !scope[0]) return 0;
    for (i = 0; k_scopes[i]; i++) {
        if (strcmp(k_scopes[i], scope) == 0) return 1;
    }
    return 0;
}

int dnd_camp_pronunciation_ok_v1(const char *value) {
    size_t i, runes = 0;
    if (!value || !value[0]) return 0;
    for (i = 0; value[i];) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x80) {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == ' ' || c == '-' || c == '\'' || c == '.')) {
                return 0;
            }
            i++;
            runes++;
        } else {
            return 0; /* ASCII-only policy matching Go validator */
        }
    }
    return runes > 0 && runes <= 240;
}

int dnd_camp_metadata_ok_v1(const dnd_camp_metadata_v1 *m) {
    if (!m) return DND_CAMP_ERR_ARGUMENT;
    if (!m->name[0] || strlen(m->name) > 200) return DND_CAMP_ERR_VALIDATION;
    if (!m->ruleset[0] || strlen(m->ruleset) > 120) return DND_CAMP_ERR_VALIDATION;
    if (strlen(m->description) > 4000) return DND_CAMP_ERR_VALIDATION;
    if (strlen(m->current_scene) > 500) return DND_CAMP_ERR_VALIDATION;
    if (m->house_rule_count < 0 || m->house_rule_count > 100) return DND_CAMP_ERR_VALIDATION;
    return DND_CAMP_OK;
}

static int scopes_sorted_unique(const dnd_camp_character_v1 *c) {
    int i;
    for (i = 0; i < c->n_scopes; i++) {
        if (!dnd_camp_knowledge_scope_ok_v1(c->scopes[i])) return 0;
        if (i > 0 && strcmp(c->scopes[i - 1], c->scopes[i]) >= 0) return 0;
    }
    return 1;
}

int dnd_camp_character_ok_v1(const dnd_camp_character_v1 *c) {
    int i;
    if (!c) return DND_CAMP_ERR_ARGUMENT;
    if (!dnd_camp_id_ok_v1(c->id)) return DND_CAMP_ERR_VALIDATION;
    if (!c->name[0] || strlen(c->name) > 200) return DND_CAMP_ERR_VALIDATION;
    if (strcmp(c->kind, "player") != 0 && strcmp(c->kind, "npc") != 0) return DND_CAMP_ERR_VALIDATION;
    if (strcmp(c->kind, "player") == 0 && !c->player_user_id[0]) return DND_CAMP_ERR_VALIDATION;
    if (strlen(c->player_user_id) > 256) return DND_CAMP_ERR_VALIDATION;
    if (strlen(c->species) > 120 || strlen(c->class_name) > 120) return DND_CAMP_ERR_VALIDATION;
    if (c->level < 0 || c->level > 20) return DND_CAMP_ERR_VALIDATION;
    if (c->armor_class < 0 || c->armor_class > 100) return DND_CAMP_ERR_VALIDATION;
    if (c->max_hp < 0 || c->max_hp > 100000) return DND_CAMP_ERR_VALIDATION;
    if (strlen(c->persona) > 4000 || strlen(c->voice_id) > 200 || strlen(c->speaking_style) > 500)
        return DND_CAMP_ERR_VALIDATION;
    if (c->pronunciation[0] && !dnd_camp_pronunciation_ok_v1(c->pronunciation))
        return DND_CAMP_ERR_VALIDATION;
    for (i = 0; i < 6; i++) {
        if (c->ability[i] < 1 || c->ability[i] > 30) return DND_CAMP_ERR_VALIDATION;
    }
    if (c->n_scopes < 0 || c->n_scopes > DND_CAMP_MAX_SCOPES) return DND_CAMP_ERR_VALIDATION;
    if (c->n_safety < 0 || c->n_safety > DND_CAMP_MAX_SAFETY) return DND_CAMP_ERR_VALIDATION;
    if (c->sheet_ref_count < 0 || c->sheet_ref_count > 20) return DND_CAMP_ERR_VALIDATION;
    if (!scopes_sorted_unique(c)) return DND_CAMP_ERR_VALIDATION;
    for (i = 0; i < c->n_safety; i++) {
        if (!c->safety[i][0] || strlen(c->safety[i]) > 500) return DND_CAMP_ERR_VALIDATION;
        if (i > 0 && strcmp(c->safety[i - 1], c->safety[i]) == 0) return DND_CAMP_ERR_VALIDATION;
    }
    return DND_CAMP_OK;
}

int dnd_camp_scene_npc_ok_v1(const dnd_camp_character_v1 *c) {
    if (!c) return DND_CAMP_ERR_ARGUMENT;
    if (strcmp(c->kind, "npc") != 0) return DND_CAMP_ERR_VALIDATION;
    if (!c->persona[0] || !c->voice_id[0] || !c->speaking_style[0] || c->n_scopes < 1)
        return DND_CAMP_ERR_VALIDATION;
    return dnd_camp_character_ok_v1(c);
}

int dnd_camp_session_recaps_ok_v1(const dnd_camp_session_recap_v1 *recs, int n) {
    int i, j;
    int64_t previous = 0;
    if (n < 0) return DND_CAMP_ERR_ARGUMENT;
    if (n > DND_CAMP_MAX_RECAPS) return DND_CAMP_ERR_VALIDATION;
    if (n > 0 && !recs) return DND_CAMP_ERR_ARGUMENT;
    for (i = 0; i < n; i++) {
        if (!dnd_camp_id_ok_v1(recs[i].session_id)) return DND_CAMP_ERR_VALIDATION;
        if (!dnd_camp_id_ok_v1(recs[i].source_turn_id)) return DND_CAMP_ERR_VALIDATION;
        if (recs[i].summary_rune_count <= 0 ||
            recs[i].summary_rune_count > DND_CAMP_MAX_RECAP_RUNES)
            return DND_CAMP_ERR_VALIDATION;
        if (recs[i].created_at_unix_ms <= 0) return DND_CAMP_ERR_VALIDATION;
        if (recs[i].created_at_unix_ms < previous) return DND_CAMP_ERR_VALIDATION;
        for (j = 0; j < i; j++) {
            if (strcmp(recs[j].session_id, recs[i].session_id) == 0)
                return DND_CAMP_ERR_VALIDATION;
        }
        previous = recs[i].created_at_unix_ms;
    }
    return DND_CAMP_OK;
}

int dnd_camp_upsert_session_recap_v1(dnd_camp_session_recap_v1 *arr, int n, int cap,
                                     const dnd_camp_session_recap_v1 *incoming) {
    int i, out_n = 0;
    dnd_camp_session_recap_v1 tmp[DND_CAMP_MAX_RECAPS + 1];
    if (!arr || !incoming || cap < 1) return -1;
    if (n < 0) n = 0;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) {
        if (strcmp(arr[i].session_id, incoming->session_id) == 0) continue;
        if (out_n < DND_CAMP_MAX_RECAPS + 1) tmp[out_n++] = arr[i];
    }
    if (out_n < DND_CAMP_MAX_RECAPS + 1) tmp[out_n++] = *incoming;
    if (out_n > DND_CAMP_MAX_RECAPS) {
        int drop = out_n - DND_CAMP_MAX_RECAPS;
        memmove(tmp, tmp + drop, (size_t)DND_CAMP_MAX_RECAPS * sizeof(tmp[0]));
        out_n = DND_CAMP_MAX_RECAPS;
    }
    if (out_n > cap) out_n = cap;
    memcpy(arr, tmp, (size_t)out_n * sizeof(arr[0]));
    return out_n;
}

static int id_in_list(const char known_ids[][DND_CAMP_ID_CAP], int n_known, const char *id) {
    int i;
    if (!id || !id[0]) return 0;
    for (i = 0; i < n_known; i++) {
        if (strcmp(known_ids[i], id) == 0) return 1;
    }
    return 0;
}

int dnd_camp_scene_director_ok_v1(const dnd_camp_scene_director_v1 *d,
                                  const char known_ids[][DND_CAMP_ID_CAP], int n_known) {
    int i, j;
    if (!d) return DND_CAMP_ERR_ARGUMENT;
    if (n_known < 0) return DND_CAMP_ERR_ARGUMENT;
    if (n_known > 0 && !known_ids) return DND_CAMP_ERR_ARGUMENT;
    if (d->n_active < 0 || d->n_active > DND_CAMP_MAX_SCENE_CAST) return DND_CAMP_ERR_VALIDATION;
    if (d->next_speaker_index < 0) return DND_CAMP_ERR_VALIDATION;
    if (d->n_active == 0 && d->next_speaker_index != 0) return DND_CAMP_ERR_VALIDATION;
    if (d->n_active > 0 && d->next_speaker_index >= d->n_active) return DND_CAMP_ERR_VALIDATION;
    for (i = 0; i < d->n_active; i++) {
        if (!dnd_camp_id_ok_v1(d->active_npc_ids[i])) return DND_CAMP_ERR_VALIDATION;
        for (j = 0; j < i; j++) {
            if (strcmp(d->active_npc_ids[j], d->active_npc_ids[i]) == 0)
                return DND_CAMP_ERR_VALIDATION;
        }
        if (!id_in_list(known_ids, n_known, d->active_npc_ids[i]))
            return DND_CAMP_ERR_VALIDATION;
    }
    if (!d->active_turn_id[0]) {
        if (d->active_speaker_id[0] || d->lease_expires_unix_ms != 0)
            return DND_CAMP_ERR_VALIDATION;
        return DND_CAMP_OK;
    }
    if (!dnd_camp_id_ok_v1(d->active_turn_id)) return DND_CAMP_ERR_VALIDATION;
    if (!id_in_list(d->active_npc_ids, d->n_active, d->active_speaker_id))
        return DND_CAMP_ERR_VALIDATION;
    if (d->lease_expires_unix_ms <= 0) return DND_CAMP_ERR_VALIDATION;
    return DND_CAMP_OK;
}
