/* Strict campaign commands and collection-preserving state transitions. */
#define _POSIX_C_SOURCE 200809L
#include "pt_campaign.h"
#include "../c-campaign/dnd_campaign.h"
#include "../c-toolstore/toolstore.h"
#include "cmp_json.h"
#include "utf8.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CREATE, GET, UPDATE, SET_SCENE, UPSERT_CHARACTER, REMOVE_CHARACTER, GET_ROSTER, ADD_CHARACTER,
       SET_OBSERVATIONS, GET_PRESENCE, RESOLVE_PRESENCE, OPERATION_COUNT, MAX_CHARACTERS = 200, MAX_OBSERVATIONS = 20 };
static const char *const operations[] = {"create", "get", "update", "set_scene", "upsert_character", "remove_character", "get_roster", "add_character",
                                        "set_scene_observations", "get_scene_presence", "resolve_scene_presence"};
static const cmp_json_field empty_object = {.value = "{}", .value_len = 2u};
static const cmp_json_field empty_array = {.value = "[]", .value_len = 2u};

typedef struct {
    dnd_camp_metadata_v1 metadata;
    cmp_json_field house_rules;
    unsigned present;
} campaign_metadata;
/* The owner attests these facts. A turn reference does not verify a transcript. */
typedef struct {
    char character_id[DND_CAMP_ID_CAP], source_turn_id[DND_CAMP_ID_CAP];
    char presence[8], visibility[8];
} scene_observation;
typedef struct {
    int64_t version;
    char scene_id[DND_CAMP_ID_CAP];
    scene_observation entries[MAX_OBSERVATIONS];
    size_t count;
} scene_observations;
typedef struct {
    unsigned operation;
    char campaign_id[DND_CAMP_ID_CAP], scene[501];
    int64_t expected;
    campaign_metadata campaign;
    cmp_json_field character;
    char character_id[DND_CAMP_ID_CAP];
    char scene_id[DND_CAMP_ID_CAP];
    char character_name[201];
    scene_observations observations;
} campaign_command;
/* Collection spans borrow the validated input document until serialization. */
typedef struct {
    char campaign_id[DND_CAMP_ID_CAP], owner_user_id[PT_ID], status[16];
    int64_t version;
    campaign_metadata campaign;
    cmp_json_field characters, recaps;
    dnd_camp_scene_director_v1 director;
    char character_ids[MAX_CHARACTERS][DND_CAMP_ID_CAP];
    char known_npcs[MAX_CHARACTERS][DND_CAMP_ID_CAP];
    int n_characters, n_npcs;
    scene_observations observations;
} campaign_state;

static int fields(const cmp_json_object *o, const char *const *names, size_t count,
                  unsigned allowed, unsigned required) {
    unsigned seen = 0;
    for (size_t i = 0; i < o->field_count; ++i) {
        const cmp_json_field *f = &o->fields[i];
        size_t j;
        if (f->key_escaped) return 0;
        for (j = 0; j < count; ++j)
            if (strlen(names[j]) == f->key_len && !memcmp(names[j], f->key, f->key_len)) break;
        if (j == count || j >= 32u || !(allowed & (1u << j)) || (seen & (1u << j))) return 0;
        seen |= 1u << j;
    }
    return (seen & required) == required;
}
static int integer(const cmp_json_object *o, const char *key, int low, int high, int *out) {
    int64_t value;
    if (!cmp_json_object_i64(o, key, &value) || value < low || value > high) return 0;
    *out = (int)value;
    return 1;
}
static int owner_ok(const char *owner) {
    size_t n = strnlen(owner, PT_ID);
    if (!n || n >= PT_ID || owner[0] == ' ' || owner[n - 1u] == ' ' ||
        !utf8_validate_v1((const uint8_t *)owner, n)) return 0;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)owner[i] < 32u || owner[i] == 127) return 0;
    return 1;
}
static int rune_count(const char *value) {
    int count = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if ((*p & 0xc0u) != 0x80u) ++count;
    return count;
}

/* Decode keys before checking uniqueness, including escaped aliases. The key
 * arena cannot exceed the source span. Values use the schema's Unicode bound. */
static int string_map(const cmp_json_field *field, unsigned maximum, int *count) {
    cmp_json_members members;
    cmp_json_field item, key;
    char *arena = NULL, *keys[100], value[4001];
    size_t used = 0;
    unsigned n = 0;
    int rc, result = 0;
    if (!field || maximum > 100u || field->value_len >= PT_OUT ||
        !cmp_json_field_members(field, &members)) return 0;
    arena = malloc(field->value_len);
    if (!arena) return 0;
    while ((rc = cmp_json_members_next(&members, &item)) == 1) {
        if (n >= maximum || !cmp_json_field_str(&item, value, sizeof(value)) ||
            !value[0] || rune_count(value) > 1000) goto done;
        key = (cmp_json_field){.value = item.key - 1u, .value_len = item.key_len + 2u};
        if (!cmp_json_field_str(&key, arena + used, field->value_len - used)) goto done;
        for (unsigned i = 0; i < n; ++i) if (!strcmp(keys[i], arena + used)) goto done;
        keys[n++] = arena + used;
        used += strlen(arena + used) + 1u;
    }
    if (rc == 0) { *count = (int)n; result = 1; }
done:
    free(arena);
    return result;
}
static int metadata_decode(const cmp_json_object *o, campaign_metadata *m, int retained) {
    static const char *const names[] = {"name", "ruleset", "description", "current_scene", "house_rules"};
    const cmp_json_field *rules;
    memset(m, 0, sizeof(*m));
    m->house_rules = empty_object;
    if (!fields(o, names, 5u, 31u, retained ? 31u : 3u) ||
        !cmp_json_object_str(o, "name", m->metadata.name, sizeof(m->metadata.name)) ||
        !cmp_json_object_str(o, "ruleset", m->metadata.ruleset, sizeof(m->metadata.ruleset))) return 0;
    for (unsigned i = 0; i < 5u; ++i) if (cmp_json_object_key_count(o, names[i])) m->present |= 1u << i;
    if ((m->present & 4u) && !cmp_json_object_str(o, "description", m->metadata.description,
                                                sizeof(m->metadata.description))) return 0;
    if ((m->present & 8u) && !cmp_json_object_str(o, "current_scene", m->metadata.current_scene,
                                                sizeof(m->metadata.current_scene))) return 0;
    rules = cmp_json_object_field(o, "house_rules");
    if (rules) m->house_rules = *rules;
    return string_map(&m->house_rules, 100u, &m->metadata.house_rule_count) &&
        dnd_camp_metadata_ok_v1(&m->metadata) == DND_CAMP_OK;
}
static int character_decode(const cmp_json_object *o, dnd_camp_character_v1 *c);
static int observations_decode(const cmp_json_field *field, scene_observations *s, int retained) {
    static const char *const names[] = {"scene_id", "entries", "version"};
    static const char *const entry_names[] = {"character_id", "presence", "visibility", "source_turn_id"};
    cmp_json_object object, entry;
    cmp_json_array array;
    cmp_json_field item;
    int rc;
    memset(s, 0, sizeof(*s));
    if (!cmp_json_field_object(field, &object) ||
        !fields(&object, names, 3u, retained ? 7u : 3u, retained ? 7u : 3u) ||
        !cmp_json_object_str(&object, "scene_id", s->scene_id, sizeof(s->scene_id)) ||
        !dnd_camp_id_ok_v1(s->scene_id) ||
        !cmp_json_field_array(cmp_json_object_field(&object, "entries"), &array) ||
        (retained && (!cmp_json_object_i64(&object, "version", &s->version) || s->version < 1))) return 0;
    while ((rc = cmp_json_array_next(&array, &item)) == 1) {
        scene_observation *e;
        if (s->count >= MAX_OBSERVATIONS || !cmp_json_field_object(&item, &entry) ||
            !fields(&entry, entry_names, 4u, 15u, 15u)) return 0;
        e = &s->entries[s->count];
        if (!cmp_json_object_str(&entry, "character_id", e->character_id, sizeof(e->character_id)) ||
            !dnd_camp_id_ok_v1(e->character_id) ||
            !cmp_json_object_str(&entry, "source_turn_id", e->source_turn_id, sizeof(e->source_turn_id)) ||
            !dnd_camp_id_ok_v1(e->source_turn_id) ||
            !cmp_json_object_str(&entry, "presence", e->presence, sizeof(e->presence)) ||
            (strcmp(e->presence, "present") && strcmp(e->presence, "absent") && strcmp(e->presence, "unknown")) ||
            !cmp_json_object_str(&entry, "visibility", e->visibility, sizeof(e->visibility)) ||
            (strcmp(e->visibility, "table") && strcmp(e->visibility, "dm"))) return 0;
        for (size_t i = 0; i < s->count; ++i)
            if (!strcmp(s->entries[i].character_id, e->character_id)) return 0;
        ++s->count;
    }
    return rc == 0;
}
static int read_operation(unsigned operation) {
    return operation == GET || operation == GET_ROSTER || operation == GET_PRESENCE || operation == RESOLVE_PRESENCE;
}
static int scene_name_ok(const char *name) {
    size_t n = strlen(name);
    if (!n || n > 200u || name[0] == ' ' || name[n - 1u] == ' ' ||
        !utf8_validate_v1((const uint8_t *)name, n)) return 0;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)name[i] < 32u || name[i] == 127) return 0;
    return 1;
}
static int command_decode(const char *json, campaign_command *c) {
    static const char *const names[] = {"operation", "campaign_id", "expected_version", "campaign", "scene",
                                       "character", "character_id", "scene_observations", "scene_id", "character_name"};
    static const unsigned masks[] = {15u, 3u, 15u, 23u, 39u, 71u, 3u, 39u, 135u, 327u, 771u};
    cmp_json_object o, metadata;
    char operation[32];
    memset(c, 0, sizeof(*c));
    if (!json || strnlen(json, PT_INPUT) >= PT_INPUT || !cmp_json_object_parse(json, &o) ||
        !cmp_json_object_str(&o, "operation", operation, sizeof(operation))) return 0;
    for (char *p = operation; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    for (c->operation = 0; c->operation < OPERATION_COUNT; ++c->operation)
        if (!strcmp(operation, operations[c->operation])) break;
    if (c->operation == OPERATION_COUNT || !fields(&o, names, 10u, masks[c->operation], masks[c->operation]) ||
        !cmp_json_object_str(&o, "campaign_id", c->campaign_id, sizeof(c->campaign_id)) ||
        !dnd_camp_id_ok_v1(c->campaign_id)) return 0;
    if (c->operation != GET && c->operation != GET_ROSTER && c->operation != RESOLVE_PRESENCE &&
        (!cmp_json_object_i64(&o, "expected_version", &c->expected) ||
        (c->operation == CREATE ? c->expected != 0 : c->expected < 1))) return 0;
    if ((c->operation == CREATE || c->operation == UPDATE) &&
        (!cmp_json_object_object(&o, "campaign", &metadata) || !metadata_decode(&metadata, &c->campaign, 0))) return 0;
    if (c->operation == UPSERT_CHARACTER || c->operation == ADD_CHARACTER) {
        dnd_camp_character_v1 character;
        const cmp_json_field *field = cmp_json_object_field(&o, "character");
        if (!cmp_json_field_object(field, &metadata) || !character_decode(&metadata, &character)) return 0;
        c->character = *field;
        strcpy(c->character_id, character.id);
    }
    if ((c->operation == REMOVE_CHARACTER || c->operation == GET_PRESENCE) &&
        (!cmp_json_object_str(&o, "character_id", c->character_id, sizeof(c->character_id)) ||
         !dnd_camp_id_ok_v1(c->character_id))) return 0;
    if (c->operation == SET_OBSERVATIONS &&
        !observations_decode(cmp_json_object_field(&o, "scene_observations"), &c->observations, 0)) return 0;
    if ((c->operation == GET_PRESENCE || c->operation == RESOLVE_PRESENCE) &&
        (!cmp_json_object_str(&o, "scene_id", c->scene_id, sizeof(c->scene_id)) || !dnd_camp_id_ok_v1(c->scene_id))) return 0;
    if (c->operation == RESOLVE_PRESENCE &&
        (!cmp_json_object_str(&o, "character_name", c->character_name, sizeof(c->character_name)) ||
         !scene_name_ok(c->character_name))) return 0;
    return c->operation != SET_SCENE || (cmp_json_object_str(&o, "scene", c->scene, sizeof(c->scene)) && c->scene[0]);
}
static int string_list(const cmp_json_field *field, char *values, size_t stride, int maximum, int *count) {
    cmp_json_array array;
    cmp_json_field item;
    int n = 0, rc;
    if (!cmp_json_field_array(field, &array)) return 0;
    while ((rc = cmp_json_array_next(&array, &item)) == 1) {
        char *value;
        if (n >= maximum) return 0;
        value = values + (size_t)n * stride;
        if (!cmp_json_field_str(&item, value, stride) || !value[0]) return 0;
        for (int i = 0; i < n; ++i) if (!strcmp(values + (size_t)i * stride, value)) return 0;
        ++n;
    }
    *count = n;
    return rc == 0;
}
static int character_decode(const cmp_json_object *o, dnd_camp_character_v1 *c) {
    static const char *const names[] = {"id", "name", "kind", "player_user_id", "species", "class",
        "level", "armor_class", "max_hp", "ability_scores", "sheet_refs", "persona",
        "allowed_knowledge_scopes", "voice_id", "speaking_style", "pronunciation", "safety_rules"};
    static const char *const abilities[] = {"charisma", "constitution", "dexterity", "intelligence", "strength", "wisdom"};
    cmp_json_object scores;
    char safety[20][2001];
    int n_safety;
    memset(c, 0, sizeof(*c));
    if (!fields(o, names, 17u, 131071u, 131063u)) return 0;
#define READ(key, member) if (!cmp_json_object_str(o, key, c->member, sizeof(c->member))) return 0
    READ("id", id); READ("name", name); READ("kind", kind); READ("species", species);
    READ("class", class_name); READ("persona", persona); READ("voice_id", voice_id);
    READ("speaking_style", speaking_style); READ("pronunciation", pronunciation);
    if (cmp_json_object_key_count(o, "player_user_id")) { READ("player_user_id", player_user_id); }
#undef READ
    if (c->pronunciation[0] && !((c->pronunciation[0] >= 'A' && c->pronunciation[0] <= 'Z') ||
        (c->pronunciation[0] >= 'a' && c->pronunciation[0] <= 'z') ||
        (c->pronunciation[0] >= '0' && c->pronunciation[0] <= '9'))) return 0;
    if (!integer(o, "level", 0, 20, &c->level) || !integer(o, "armor_class", 0, 100, &c->armor_class) ||
        !integer(o, "max_hp", 0, 100000, &c->max_hp) ||
        !cmp_json_object_object(o, "ability_scores", &scores) || !fields(&scores, abilities, 6u, 63u, 63u)) return 0;
    for (unsigned i = 0; i < 6u; ++i) if (!integer(&scores, abilities[i], 1, 30, &c->ability[i])) return 0;
    if (!string_map(cmp_json_object_field(o, "sheet_refs"), 20u, &c->sheet_ref_count) ||
        !string_list(cmp_json_object_field(o, "allowed_knowledge_scopes"), (char *)c->scopes,
                     sizeof(c->scopes[0]), DND_CAMP_MAX_SCOPES, &c->n_scopes) ||
        !string_list(cmp_json_object_field(o, "safety_rules"), (char *)safety, sizeof(safety[0]), 20, &n_safety)) return 0;
    for (int i = 0; i < n_safety; ++i) if (rune_count(safety[i]) > 500) return 0;
    /* The host validates complete 500-character safety strings above. The kernel's
     * older 64-byte scratch slots do not constrain or truncate retained values. */
    return dnd_camp_character_ok_v1(c) == DND_CAMP_OK;
}
static int characters_decode(const cmp_json_field *field, campaign_state *s) {
    cmp_json_array array;
    cmp_json_field item;
    cmp_json_object object;
    dnd_camp_character_v1 character;
    int rc;
    if (!cmp_json_field_array(field, &array)) return 0;
    while ((rc = cmp_json_array_next(&array, &item)) == 1) {
        if (s->n_characters >= MAX_CHARACTERS || !cmp_json_field_object(&item, &object) ||
            !character_decode(&object, &character)) return 0;
        for (int i = 0; i < s->n_characters; ++i) if (!strcmp(s->character_ids[i], character.id)) return 0;
        strcpy(s->character_ids[s->n_characters++], character.id);
        if (dnd_camp_scene_npc_ok_v1(&character) == DND_CAMP_OK)
            strcpy(s->known_npcs[s->n_npcs++], character.id);
    }
    return rc == 0;
}
static int recaps_decode(const cmp_json_field *field) {
    static const char *const names[] = {"session_id", "source_turn_id", "summary", "created_at_unix_ms"};
    dnd_camp_session_recap_v1 recaps[DND_CAMP_MAX_RECAPS] = {0};
    cmp_json_array array;
    cmp_json_field item;
    cmp_json_object object;
    char summary[4u * DND_CAMP_MAX_RECAP_RUNES + 1u];
    int n = 0, rc;
    if (!cmp_json_field_array(field, &array)) return 0;
    while ((rc = cmp_json_array_next(&array, &item)) == 1) {
        dnd_camp_session_recap_v1 *recap;
        if (n >= DND_CAMP_MAX_RECAPS || !cmp_json_field_object(&item, &object) ||
            !fields(&object, names, 4u, 15u, 15u)) return 0;
        recap = &recaps[n++];
        if (!cmp_json_object_str(&object, "session_id", recap->session_id, sizeof(recap->session_id)) ||
            !cmp_json_object_str(&object, "source_turn_id", recap->source_turn_id, sizeof(recap->source_turn_id)) ||
            !cmp_json_object_str(&object, "summary", summary, sizeof(summary)) ||
            !cmp_json_object_i64(&object, "created_at_unix_ms", &recap->created_at_unix_ms)) return 0;
        recap->summary_rune_count = rune_count(summary);
    }
    return rc == 0 && dnd_camp_session_recaps_ok_v1(recaps, n) == DND_CAMP_OK;
}
static int director_decode(const cmp_json_object *o, campaign_state *s) {
    static const char *const names[] = {"active_npc_ids", "next_speaker_index", "active_speaker_id",
                                       "active_turn_id", "lease_expires_unix_ms"};
    dnd_camp_scene_director_v1 *d = &s->director;
    int legacy_empty = o->field_count == 1u;
    if (!fields(o, names, 5u, 31u, legacy_empty ? 1u : 31u) ||
        !string_list(cmp_json_object_field(o, "active_npc_ids"), (char *)d->active_npc_ids,
                     sizeof(d->active_npc_ids[0]), DND_CAMP_MAX_SCENE_CAST, &d->n_active)) return 0;
    if (legacy_empty) return d->n_active == 0;
    return integer(o, "next_speaker_index", 0, 19, &d->next_speaker_index) &&
        cmp_json_object_str(o, "active_speaker_id", d->active_speaker_id, sizeof(d->active_speaker_id)) &&
        cmp_json_object_str(o, "active_turn_id", d->active_turn_id, sizeof(d->active_turn_id)) &&
        cmp_json_object_i64(o, "lease_expires_unix_ms", &d->lease_expires_unix_ms) &&
        dnd_camp_scene_director_ok_v1(d, (const char (*)[DND_CAMP_ID_CAP])s->known_npcs, s->n_npcs) == DND_CAMP_OK;
}
static int observations_known(const campaign_state *s, const scene_observations *observations) {
    for (size_t i = 0; i < observations->count; ++i) {
        int found = 0;
        for (int j = 0; j < s->n_characters; ++j)
            if (!strcmp(observations->entries[i].character_id, s->character_ids[j])) found = 1;
        if (!found) return 0;
    }
    return 1;
}
static int state_decode(const char *json, campaign_state *s) {
    static const char *const names[] = {"campaign_id", "owner_user_id", "version", "status", "campaign",
                                       "characters", "session_recaps", "scene_director", "scene_observations"};
    cmp_json_object o, metadata, director;
    const cmp_json_field *characters, *recaps;
    memset(s, 0, sizeof(*s));
    if (!json || strnlen(json, PT_OUT) >= PT_OUT || !cmp_json_object_parse(json, &o) ||
        !fields(&o, names, 9u, 511u, 255u) ||
        !cmp_json_object_str(&o, "campaign_id", s->campaign_id, sizeof(s->campaign_id)) ||
        !dnd_camp_id_ok_v1(s->campaign_id) ||
        !cmp_json_object_str(&o, "owner_user_id", s->owner_user_id, sizeof(s->owner_user_id)) ||
        !owner_ok(s->owner_user_id) || !cmp_json_object_i64(&o, "version", &s->version) || s->version < 1 ||
        !cmp_json_object_str(&o, "status", s->status, sizeof(s->status)) ||
        (strcmp(s->status, "active") && strcmp(s->status, "archived")) ||
        !cmp_json_object_object(&o, "campaign", &metadata) || !metadata_decode(&metadata, &s->campaign, 1) ||
        !cmp_json_object_object(&o, "scene_director", &director)) return 0;
    characters = cmp_json_object_field(&o, "characters");
    recaps = cmp_json_object_field(&o, "session_recaps");
    if (!characters_decode(characters, s) || !recaps_decode(recaps) || !director_decode(&director, s)) return 0;
    const cmp_json_field *observations = cmp_json_object_field(&o, "scene_observations");
    if (observations && (!observations_decode(observations, &s->observations, 1) ||
        s->observations.version > s->version ||
        (s->observations.version == s->version && !observations_known(s, &s->observations)))) return 0;
    s->characters = *characters;
    s->recaps = *recaps;
    return 1;
}

typedef struct { char *out; size_t cap, used; int failed; } json_writer;
static void append(json_writer *w, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void append(json_writer *w, const char *format, ...) {
    va_list args;
    int n;
    if (w->failed) return;
    va_start(args, format);
    n = vsnprintf(w->out + w->used, w->cap - w->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->cap - w->used) { w->failed = 1; return; }
    w->used += (size_t)n;
}
static void string(json_writer *w, const char *value) {
    char escaped[6u * 4000u + 1u];
    if (cmp_json_escape_exact(value, escaped, sizeof(escaped))) { w->failed = 1; return; }
    append(w, "\"%s\"", escaped);
}
static void raw(json_writer *w, const cmp_json_field *field) {
    if (!field->value || field->value_len >= PT_OUT) { w->failed = 1; return; }
    append(w, "%.*s", (int)field->value_len, field->value);
}
/* Replace one complete record in place, append a new ID, or remove one ID.
 * Other records retain their validated bytes. The caller owns the new span. */
static int roster_apply(campaign_state *s, const campaign_command *c, char *out, size_t cap) {
    cmp_json_array array;
    cmp_json_field item, result;
    json_writer w = {out, cap, 0, cap == 0};
    int index = -1, emitted = 0, rc;
    for (int i = 0; i < s->n_characters; ++i)
        if (!strcmp(s->character_ids[i], c->character_id)) index = i;
    if (!out || !cap || (index < 0 && (c->operation == REMOVE_CHARACTER || s->n_characters >= MAX_CHARACTERS)) ||
        (index >= 0 && c->operation == ADD_CHARACTER) ||
        !cmp_json_field_array(&s->characters, &array)) return 0;
    append(&w, "[");
    for (int i = 0; (rc = cmp_json_array_next(&array, &item)) == 1; ++i) {
        if (i == index && c->operation == REMOVE_CHARACTER) continue;
        if (emitted++) append(&w, ",");
        raw(&w, i == index ? &c->character : &item);
    }
    if (rc != 0) return 0;
    if (index < 0) {
        if (emitted) append(&w, ",");
        raw(&w, &c->character);
    }
    append(&w, "]");
    if (w.failed) return 0;
    result = (cmp_json_field){.value = out, .value_len = w.used};
    s->n_characters = s->n_npcs = 0;
    if (!characters_decode(&result, s) ||
        dnd_camp_scene_director_ok_v1(&s->director, (const char (*)[DND_CAMP_ID_CAP])s->known_npcs,
                                     s->n_npcs) != DND_CAMP_OK) return 0;
    s->characters = result;
    return 1;
}
static void observations_encode(json_writer *w, const scene_observations *s) {
    append(w, "{\"version\":%lld,\"scene_id\":", (long long)s->version); string(w, s->scene_id);
    append(w, ",\"entries\":[");
    for (size_t i = 0; i < s->count; ++i) {
        const scene_observation *e = &s->entries[i];
        append(w, "%s{\"character_id\":", i ? "," : ""); string(w, e->character_id);
        append(w, ",\"presence\":"); string(w, e->presence);
        append(w, ",\"visibility\":"); string(w, e->visibility);
        append(w, ",\"source_turn_id\":"); string(w, e->source_turn_id);
        append(w, "}");
    }
    append(w, "]}");
}
/* ASCII names compare without case. Other UTF-8 bytes compare exactly.
 * Ambiguous names never select the first record or reveal a private match. */
static int same_scene_name(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + ('a' - 'A'));
        if (x != y) return 0;
    }
    return *a == *b;
}
static int scene_character_id(const campaign_state *s, const char *name, char out[DND_CAMP_ID_CAP]) {
    cmp_json_array array;
    cmp_json_field item;
    cmp_json_object object;
    dnd_camp_character_v1 character;
    int rc, matches = 0;
    out[0] = '\0';
    if (!cmp_json_field_array(&s->characters, &array)) return 0;
    while ((rc = cmp_json_array_next(&array, &item)) == 1) {
        if (!cmp_json_field_object(&item, &object) || !character_decode(&object, &character)) return 0;
        if (same_scene_name(character.name, name)) {
            if (++matches > 1) { out[0] = '\0'; return 1; }
            strcpy(out, character.id);
        }
    }
    return rc == 0;
}
/* Existing result shapes stay stable. Only the new write echoes observations. */
static size_t state_encode(char *out, size_t cap, const campaign_state *s,
                           const campaign_command *c, const char *call_id) {
    json_writer w = {out, cap, 0, cap == 0};
    const dnd_camp_metadata_v1 *m = &s->campaign.metadata;
    const dnd_camp_scene_director_v1 *d = &s->director;
    if (!out || !cap) return 0;
    out[0] = '\0';
    append(&w, "{");
    if (c) {
        append(&w, "\"operation\":"); string(&w, operations[c->operation]);
        append(&w, ",\"operation_id\":"); string(&w, call_id); append(&w, ",");
    } else { append(&w, "\"owner_user_id\":"); string(&w, s->owner_user_id); append(&w, ","); }
    append(&w, "\"campaign_id\":"); string(&w, s->campaign_id);
    append(&w, ",\"version\":%lld,\"status\":", (long long)s->version); string(&w, s->status);
    if (c && (c->operation == GET_PRESENCE || c->operation == RESOLVE_PRESENCE)) {
        const scene_observation *selected = NULL;
        char resolved[DND_CAMP_ID_CAP];
        const char *character_id = c->character_id;
        if (c->operation == RESOLVE_PRESENCE) {
            if (!scene_character_id(s, c->character_name, resolved)) { out[0] = '\0'; return 0; }
            character_id = resolved;
        }
        if (!strcmp(s->status, "active") && s->observations.version == s->version &&
            !strcmp(s->observations.scene_id, c->scene_id)) {
            for (size_t i = 0; i < s->observations.count; ++i) {
                const scene_observation *e = &s->observations.entries[i];
                if (!strcmp(e->character_id, character_id) && !strcmp(e->visibility, "table")) selected = e;
            }
        }
        append(&w, ",\"scene_id\":"); string(&w, c->scene_id);
        if (c->operation == GET_PRESENCE) { append(&w, ",\"character_id\":"); string(&w, c->character_id); }
        else { append(&w, ",\"character_name\":"); string(&w, c->character_name); }
        append(&w, ",\"presence\":"); string(&w, selected ? selected->presence : "unknown");
        append(&w, ",\"source_turn_id\":");
        string(&w, selected && strcmp(selected->presence, "unknown") ? selected->source_turn_id : "");
        append(&w, "}");
        if (w.failed) { out[0] = '\0'; return 0; }
        return w.used;
    }
    if (c && c->operation == GET_ROSTER) {
        cmp_json_array array;
        cmp_json_field item;
        cmp_json_object object;
        dnd_camp_character_v1 character;
        int rc, emitted = 0;
        append(&w, ",\"characters\":[");
        if (!cmp_json_field_array(&s->characters, &array)) { out[0] = '\0'; return 0; }
        while ((rc = cmp_json_array_next(&array, &item)) == 1) {
            if (!cmp_json_field_object(&item, &object) || !character_decode(&object, &character)) {
                out[0] = '\0'; return 0;
            }
            append(&w, "%s{\"id\":", emitted++ ? "," : ""); string(&w, character.id);
            append(&w, ",\"name\":"); string(&w, character.name);
            append(&w, ",\"kind\":"); string(&w, character.kind);
            append(&w, ",\"max_hp\":%d}", character.max_hp);
        }
        append(&w, "]}");
        if (rc || w.failed) { out[0] = '\0'; return 0; }
        return w.used;
    }
    append(&w, ",\"campaign\":{\"name\":"); string(&w, m->name);
    append(&w, ",\"ruleset\":"); string(&w, m->ruleset);
    append(&w, ",\"description\":"); string(&w, m->description);
    append(&w, ",\"current_scene\":"); string(&w, m->current_scene);
    append(&w, ",\"house_rules\":"); raw(&w, &s->campaign.house_rules);
    append(&w, "},\"characters\":"); raw(&w, &s->characters);
    append(&w, ",\"session_recaps\":"); raw(&w, &s->recaps);
    append(&w, ",\"scene_director\":{\"active_npc_ids\":[");
    for (int i = 0; i < d->n_active; ++i) { append(&w, "%s", i ? "," : ""); string(&w, d->active_npc_ids[i]); }
    append(&w, "],\"next_speaker_index\":%d,\"active_speaker_id\":", d->next_speaker_index); string(&w, d->active_speaker_id);
    append(&w, ",\"active_turn_id\":"); string(&w, d->active_turn_id);
    append(&w, ",\"lease_expires_unix_ms\":%lld}", (long long)d->lease_expires_unix_ms);
    if (s->observations.version && (!c || c->operation == SET_OBSERVATIONS)) {
        append(&w, ",\"scene_observations\":"); observations_encode(&w, &s->observations);
    }
    append(&w, "}");
    if (w.failed) { out[0] = '\0'; return 0; }
    return w.used;
}

int pt_campaign_path(const char *directory, const char *input, char *path, size_t cap,
                      int *create, int *read_only) {
    campaign_command command;
    int n;
    if (!directory || !directory[0] || !command_decode(input, &command)) return 0;
    *create = command.operation == CREATE;
    *read_only = read_operation(command.operation);
    n = snprintf(path, cap, "%s/campaigns/%s.json", directory, command.campaign_id);
    return n > 0 && (size_t)n < cap;
}

/* The scene tool can read only. Its adapter cannot create or mutate a campaign. */
int pt_scene_presence_path(const char *directory, const char *input, char *path, size_t cap,
                           int *create, int *read_only) {
    campaign_command command;
    return command_decode(input, &command) && command.operation == RESOLVE_PRESENCE &&
        pt_campaign_path(directory, input, path, cap, create, read_only) && !*create && *read_only;
}
int pt_scene_presence_transition(const char *before, char *after, size_t cap, pt_call *call) {
    campaign_command command;
    if (!call || !command_decode(call->input_json, &command) || command.operation != RESOLVE_PRESENCE) return 0;
    return pt_campaign_transition(before, after, cap, call);
}

int pt_campaign_transition(const char *before, char *after, size_t cap, pt_call *call) {
    campaign_command command;
    campaign_state *state = NULL;
    char *roster = NULL;
    int result = 0;
    const char *error = "invalid campaign command";
    call->output_json[0] = call->summary[0] = call->error[0] = '\0';
    if (!owner_ok(call->user_id) ||
        strnlen(call->tool_call_id, sizeof(call->tool_call_id)) >= sizeof(call->tool_call_id) ||
        !ts_valid_id(call->tool_call_id) || !command_decode(call->input_json, &command)) goto done;
    state = calloc(1, sizeof(*state));
    if (!state) goto done;
    error = "campaign state is unavailable or invalid";
    if (command.operation == CREATE) {
        if (before) goto done;
        strcpy(state->campaign_id, command.campaign_id);
        strcpy(state->owner_user_id, call->user_id);
        strcpy(state->status, "active");
        state->version = 1;
        state->campaign = command.campaign;
        state->characters = state->recaps = empty_array;
    } else {
        if (!before || !state_decode(before, state) || strcmp(state->campaign_id, command.campaign_id)) goto done;
        error = "campaign belongs to a different governed user";
        if (strcmp(state->owner_user_id, call->user_id)) goto done;
        if (command.operation == GET_PRESENCE && command.expected != state->version) {
            error = "campaign version conflict";
            goto done;
        }
        if (!read_operation(command.operation)) {
            error = "campaign version conflict or counter exhausted";
            if (command.expected != state->version || state->version == INT64_MAX || strcmp(state->status, "active")) goto done;
            if (command.operation == UPDATE) {
                campaign_metadata *m = &state->campaign, *incoming = &command.campaign;
                strcpy(m->metadata.name, incoming->metadata.name);
                strcpy(m->metadata.ruleset, incoming->metadata.ruleset);
                if (incoming->present & 4u) strcpy(m->metadata.description, incoming->metadata.description);
                if (incoming->present & 8u) strcpy(m->metadata.current_scene, incoming->metadata.current_scene);
                if (incoming->present & 16u) {
                    m->house_rules = incoming->house_rules;
                    m->metadata.house_rule_count = incoming->metadata.house_rule_count;
                }
            } else if (command.operation == SET_SCENE) strcpy(state->campaign.metadata.current_scene, command.scene);
            else if (command.operation == SET_OBSERVATIONS) {
                error = "scene observations require existing campaign characters";
                if (!observations_known(state, &command.observations)) goto done;
                state->observations = command.observations;
                state->observations.version = state->version + 1;
            }
            else {
                error = "character change is invalid, exceeds capacity, or conflicts with the scene cast";
                roster = malloc(PT_OUT);
                if (!roster || !roster_apply(state, &command, roster, PT_OUT)) goto done;
            }
            ++state->version;
        }
    }
    error = "campaign output exceeds capacity";
    if (!state_encode(after, cap, state, NULL, NULL) ||
        !state_encode(call->output_json, sizeof(call->output_json), state, &command, call->tool_call_id)) goto done;
    (void)snprintf(call->summary, sizeof(call->summary), "Campaign %s v%lld.",
                   state->campaign.metadata.name, (long long)state->version);
    result = 1;
done:
    if (!result) {
        call->output_json[0] = call->summary[0] = '\0';
        (void)snprintf(call->error, sizeof(call->error), "%s", error);
    }
    free(roster);
    free(state);
    return result;
}

int pt_campaign_canonical(const char *json, char *out, size_t cap, pt_dnd_identity *identity) {
    campaign_state *state = calloc(1, sizeof(*state));
    int result = 0;
    if (!state || !state_decode(json, state) || !state_encode(out, cap, state, NULL, NULL)) goto done;
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->owner, state->owner_user_id);
    strcpy(identity->campaign, state->campaign_id);
    identity->version = state->version;
    result = 1;
done:
    free(state);
    return result;
}

int pt_campaign_select(const char *json, const char *campaign, const char *owner, int64_t version,
                        const char ids[][DND_ID_CAP], size_t count, dnd_participant_v1 *out) {
    campaign_state *state = NULL;
    cmp_json_array array;
    cmp_json_field item;
    cmp_json_object object;
    dnd_camp_character_v1 character;
    size_t found = 0;
    int result = 0, rc;
    if (!campaign || !owner || !ids || !out || !count || count > MAX_CHARACTERS) return 0;
    state = calloc(1, sizeof(*state));
    if (!state || !state_decode(json, state) || strcmp(state->campaign_id, campaign) ||
        strcmp(state->owner_user_id, owner) || state->version != version || strcmp(state->status, "active") ||
        !cmp_json_field_array(&state->characters, &array)) goto done;
    for (size_t i = 0; i < count; ++i) {
        if (!dnd_camp_id_ok_v1(ids[i])) goto done;
        for (size_t j = 0; j < i; ++j) if (!strcmp(ids[i], ids[j])) goto done;
    }
    for (int index = 0; (rc = cmp_json_array_next(&array, &item)) == 1; ++index) {
        if (index >= state->n_characters) goto done;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(state->character_ids[index], ids[i])) continue;
            if (!cmp_json_field_object(&item, &object) || !character_decode(&object, &character) ||
                strlen(character.name) >= sizeof(out[i].name)) goto done;
            memset(&out[i], 0, sizeof(out[i]));
            strcpy(out[i].id, character.id);
            strcpy(out[i].name, character.name);
            out[i].max_hp = out[i].current_hp = character.max_hp;
            ++found;
        }
    }
    result = rc == 0 && found == count;
done:
    free(state);
    return result;
}
