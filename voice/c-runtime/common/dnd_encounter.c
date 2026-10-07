/* Owned state intents and strict signed-result projection. No I/O. */
#include "dnd_tools.h"
#include "cmp_json.h"
#include "utf8.h"
#include "dnd_initiative.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int state_intent(const char *text) {
    char command[128];
    size_t length;
    if (!text) return 0;
    while (*text == ' ') ++text;
    length = strlen(text);
    if (!length || length >= sizeof(command)) return 0;
    while (length && text[length - 1u] == ' ') --length;
    if (length && (text[length - 1u] == '.' || text[length - 1u] == '?' || text[length - 1u] == '!')) --length;
    for (size_t i = 0; i < length; ++i)
        command[i] = text[i] >= 'A' && text[i] <= 'Z' ? (char)(text[i] + ('a' - 'A')) : text[i];
    command[length] = '\0';
    const char *body = !strncmp(command, "please ", 7u) ? command + 7u : command;
    if (!strcmp(body, "show the initiative order") || !strcmp(body, "show initiative order") ||
        !strcmp(body, "refresh the encounter")) return 1;
    if (!strcmp(body, "show the campaign roster") || !strcmp(body, "load the campaign roster")) return 2;
    if (!strcmp(body, "roll initiative for the party") || !strcmp(body, "roll initiative")) return 3;
    if (!strcmp(body, "create the campaign")) return 4;
    if (!strcmp(body, "add a character to the campaign")) return 5;
    if (!strcmp(body, "advance the encounter")) return 6;
    if (!strcmp(body, "apply damage in the encounter")) return 7;
    if (!strcmp(body, "heal a participant in the encounter")) return 8;
    if (!strcmp(body, "add a condition in the encounter")) return 9;
    if (!strcmp(body, "remove a condition in the encounter")) return 10;
    if (!strcmp(body, "end the encounter")) return 11;
    return 0;
}

int dnd_encounter_intent(const char *text) { return state_intent(text) == 1; }
int dnd_roster_intent(const char *text) { return state_intent(text) == 2; }
int dnd_initiative_intent(const char *text) { return state_intent(text) == 3; }
int dnd_campaign_intent(const char *text) {
    int intent = state_intent(text);
    return intent == 4 ? DND_CAMPAIGN_CREATE : intent == 5 ? DND_CAMPAIGN_ADD_CHARACTER : 0;
}
int dnd_action_intent(const char *text) {
    int intent = state_intent(text);
    return intent >= 6 ? intent - 5 : 0;
}

int dnd_action_input(const dnd_encounter_action_c *r, const char *campaign, const char *encounter,
                      char *out, size_t capacity) {
    int n;
    size_t used;
    if (!out || !campaign || !encounter || !dnd_encounter_action_valid(r) ||
        !dnd_id_ok_v1(campaign) || !dnd_id_ok_v1(encounter)) return -1;
    n = snprintf(out, capacity, "{\"operation\":\"%s\",\"campaign_id\":\"%s\",\"encounter_id\":\"%s\","
        "\"expected_version\":%lld,\"include_previous\":true", dnd_action_name(r->operation), campaign, encounter,
        (long long)r->expected_version);
    if (n <= 0 || (size_t)n >= capacity) return -1;
    used = (size_t)n;
#define APPEND(...) do { n = snprintf(out + used, capacity - used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= capacity - used) { return -1; } used += (size_t)n; } while (0)
    if (r->participant_id[0]) { APPEND(",\"participant_id\":\"%s\"", r->participant_id); }
    if (r->amount) { APPEND(",\"amount\":%u", r->amount); }
    if (r->damage_type) { APPEND(",\"damage_type\":\"%s\"", dnd_damage_name(r->damage_type)); }
    if (r->condition) { APPEND(",\"condition\":\"%s\"", dnd_condition_name(r->condition)); }
    APPEND("}");
#undef APPEND
    return (int)used;
}

static int public_name(const char *name) {
    size_t length = strlen(name);
    if (!length || name[0] == ' ' || name[length - 1u] == ' ' ||
        !utf8_validate_v1((const uint8_t *)name, length)) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32u || *p == 127u) return 0;
    return 1;
}

static int number(const cmp_json_object *object, const char *key, int32_t low, int32_t high, int32_t *out) {
    int64_t value;
    if (!cmp_json_object_i64(object, key, &value) || value < low || value > high) return 0;
    *out = (int32_t)value;
    return 1;
}

static int participant(const cmp_json_field *field, turn_encounter_participant_c *out) {
    cmp_json_object object;
    cmp_json_array conditions;
    cmp_json_field condition;
    int rc;
    memset(out, 0, sizeof(*out));
    if (!cmp_json_field_object(field, &object) || object.field_count != 6u ||
        !cmp_json_object_str(&object, "id", out->id, sizeof(out->id)) ||
        !cmp_json_object_str(&object, "name", out->name, sizeof(out->name)) ||
        !number(&object, "initiative", -100, 100, &out->initiative) ||
        !number(&object, "max_hp", 0, 100000, &out->max_hp) ||
        !number(&object, "current_hp", 0, out->max_hp, &out->current_hp) ||
        !cmp_json_field_array(cmp_json_object_field(&object, "conditions"), &conditions) ||
        !public_name(out->name)) return 0;
    while ((rc = cmp_json_array_next(&conditions, &condition)) == 1) {
        if (out->condition_count >= 16u ||
            !cmp_json_field_str(&condition, out->conditions[out->condition_count++], sizeof(out->conditions[0]))) return 0;
    }
    return rc == 0;
}

static int action_transition(const cmp_json_object *object, const dnd_encounter_action_c *request,
                              const turn_encounter_state_c *state, const turn_encounter_participant_c *participants) {
    cmp_json_object previous;
    cmp_json_array array;
    cmp_json_field field;
    turn_encounter_participant_c part;
    dnd_encounter_v1 *expected = calloc(1, sizeof(*expected));
    int ok = 0, rc;
    int32_t round, active;
    if (!expected || !dnd_encounter_action_valid(request)) goto done;
    if (!cmp_json_object_object(object, "previous_state", &previous) || previous.field_count != 7u ||
        !cmp_json_object_str(&previous, "campaign_id", expected->campaign_id, sizeof(expected->campaign_id)) ||
        !cmp_json_object_str(&previous, "encounter_id", expected->encounter_id, sizeof(expected->encounter_id)) ||
        !cmp_json_object_str(&previous, "status", expected->status, sizeof(expected->status)) || strcmp(expected->status, "active") ||
        !cmp_json_object_i64(&previous, "version", &expected->version) || expected->version != request->expected_version ||
        !number(&previous, "round", 1, INT32_MAX, &round) ||
        !number(&previous, "active_index", -1, (int32_t)DND_TURN_PARTICIPANTS_MAX - 1, &active) ||
        strcmp(expected->campaign_id, state->campaign_id) || strcmp(expected->encounter_id, state->encounter_id) ||
        !cmp_json_field_array(cmp_json_object_field(&previous, "participants"), &array)) goto done;
    expected->round = round;
    expected->active_index = active;
    /* Signed call identity already binds the actual owner. This local value only
     * satisfies the pure combat kernel's nonempty-owner invariant. */
    strcpy(expected->owner_user_id, "verified-owner");
    while ((rc = cmp_json_array_next(&array, &field)) == 1) {
        if (expected->n_participants >= (int)DND_TURN_PARTICIPANTS_MAX || !participant(&field, &part)) goto done;
        dnd_participant_v1 *p = &expected->participants[expected->n_participants++];
        strcpy(p->id, part.id); strcpy(p->name, part.name);
        p->initiative = part.initiative; p->max_hp = part.max_hp; p->current_hp = part.current_hp;
        p->n_conditions = (int)part.condition_count;
        memcpy(p->conditions, part.conditions, sizeof(p->conditions));
    }
    if (rc || dnd_encounter_validate_v1(expected) != DND_OK ||
        (request->operation == DND_ACTION_ADVANCE && expected->round == INT32_MAX &&
         expected->active_index == expected->n_participants - 1)) goto done;
    switch (request->operation) {
    case DND_ACTION_ADVANCE: rc = dnd_encounter_advance_v1(expected); break;
    case DND_ACTION_DAMAGE: rc = dnd_encounter_damage_v1(expected, request->participant_id, (int)request->amount); break;
    case DND_ACTION_HEAL: rc = dnd_encounter_heal_v1(expected, request->participant_id, (int)request->amount); break;
    case DND_ACTION_CONDITION_ADD: rc = dnd_encounter_condition_add_v1(expected, request->participant_id, dnd_condition_name(request->condition)); break;
    case DND_ACTION_CONDITION_REMOVE: rc = dnd_encounter_condition_remove_v1(expected, request->participant_id, dnd_condition_name(request->condition)); break;
    default: rc = dnd_encounter_end_v1(expected); break;
    }
    if (rc != DND_OK || expected->version != state->version || strcmp(expected->status, state->status) ||
        expected->round != state->round || expected->active_index != state->active_index ||
        (size_t)expected->n_participants != state->participant_count) goto done;
    for (size_t i = 0; i < state->participant_count; ++i) {
        const dnd_participant_v1 *a = &expected->participants[i];
        const turn_encounter_participant_c *b = &participants[i];
        if (strcmp(a->id, b->id) || strcmp(a->name, b->name) || a->initiative != b->initiative ||
            a->max_hp != b->max_hp || a->current_hp != b->current_hp || (size_t)a->n_conditions != b->condition_count) goto done;
        for (size_t j = 0; j < b->condition_count; ++j) if (strcmp(a->conditions[j], b->conditions[j])) goto done;
    }
    ok = 1;
done:
    free(expected);
    return ok;
}

static int encounter_output(const char *json, const char *campaign_id, const char *encounter_id,
                            const dnd_initiative_request_c *request, const dnd_encounter_action_c *action,
                            dnd_tool_result *result) {
    turn_encounter_state_c state = {0};
    turn_encounter_participant_c participants[DND_TURN_PARTICIPANTS_MAX];
    cmp_json_object object;
    cmp_json_array array;
    cmp_json_field field;
    int rc, written;
    size_t fields = action ? 11u + (action->participant_id[0] ? 1u : 0u) + (action->amount ? 1u : 0u) +
        (action->condition ? 1u : 0u) + (action->damage_type ? 1u : 0u) : request ? 13u : 10u;
    if (!result || !campaign_id || !encounter_id) return -1;
    result->encounter_length = 0;
    result->initiative_length = 0;
    result->roster_length = 0;
    result->text[0] = '\0';
    if (!cmp_json_object_parse(json, &object) || object.field_count != fields ||
        !cmp_json_object_str(&object, "operation", state.operation, sizeof(state.operation)) ||
        !cmp_json_object_str(&object, "operation_id", state.tool_call_id, sizeof(state.tool_call_id)) ||
        !cmp_json_object_str(&object, "campaign_id", state.campaign_id, sizeof(state.campaign_id)) ||
        !cmp_json_object_str(&object, "encounter_id", state.encounter_id, sizeof(state.encounter_id)) ||
        !cmp_json_object_str(&object, "status", state.status, sizeof(state.status)) ||
        !cmp_json_object_str(&object, "active_participant_id", state.active_participant_id, sizeof(state.active_participant_id)) ||
        !cmp_json_object_i64(&object, "version", &state.version) ||
        !number(&object, "round", 1, INT32_MAX, &state.round) ||
        !number(&object, "active_index", -1, (int32_t)DND_TURN_PARTICIPANTS_MAX - 1, &state.active_index) ||
        !cmp_json_field_array(cmp_json_object_field(&object, "participants"), &array) ||
        strcmp(campaign_id, state.campaign_id) || strcmp(encounter_id, state.encounter_id) ||
        strcmp(result->call_id, state.tool_call_id) ||
        strcmp(state.operation, action ? dnd_action_name(action->operation) : request ? "roll_initiative" : "get")) return -1;
    while ((rc = cmp_json_array_next(&array, &field)) == 1) {
        if (state.participant_count >= DND_TURN_PARTICIPANTS_MAX ||
            !participant(&field, &participants[state.participant_count++])) return -1;
    }
    if (rc != 0) return -1;
    if (action) {
        char id[65], label[32];
        int64_t amount;
        if ((action->participant_id[0] && (!cmp_json_object_str(&object, "participant_id", id, sizeof(id)) || strcmp(id, action->participant_id))) ||
            (action->amount && (!cmp_json_object_i64(&object, "amount", &amount) || amount != action->amount)) ||
            (action->condition && (!cmp_json_object_str(&object, "condition", label, sizeof(label)) || strcmp(label, dnd_condition_name(action->condition)))) ||
            (action->damage_type && (!cmp_json_object_str(&object, "damage_type", label, sizeof(label)) || strcmp(label, dnd_damage_name(action->damage_type)))) ||
            !action_transition(&object, action, &state, participants)) return -1;
    }
    memcpy(state.output_sha256, result->output_sha256, sizeof(state.output_sha256));
    size_t length = pb_encode_turn_encounter(result->encounter_wire, sizeof(result->encounter_wire), &state, participants);
    if (!length) return -1;
    if (request) {
        turn_initiative_result_c receipt = {0};
        turn_initiative_roll_c rolls[DND_INITIATIVE_SELECTIONS_MAX];
        if (!dnd_initiative_request_valid(request) || request->expected_version >= DND_EXACT_VERSION_MAX ||
            state.version != request->expected_version + 1 || strcmp(state.status, "active") ||
            !cmp_json_object_i64(&object, "campaign_version", &receipt.campaign_version) ||
            receipt.campaign_version != request->campaign_version ||
            !cmp_json_object_str(&object, "campaign_sha256", receipt.campaign_sha256, sizeof(receipt.campaign_sha256)) ||
            !cmp_json_field_array(cmp_json_object_field(&object, "initiative_rolls"), &array)) return -1;
        while ((rc = cmp_json_array_next(&array, &field)) == 1) {
            cmp_json_object item;
            cmp_json_array numbers;
            cmp_json_field number_field;
            char entropy[16];
            int found = 0, nrc;
            double value;
            if (receipt.roll_count >= request->count) return -1;
            turn_initiative_roll_c *roll = &rolls[receipt.roll_count];
            const dnd_initiative_selection_c *selection = &request->selections[receipt.roll_count++];
            memset(roll, 0, sizeof(*roll));
            if (!cmp_json_field_object(&field, &item) || item.field_count != 6u ||
                !cmp_json_object_str(&item, "character_id", roll->character_id, sizeof(roll->character_id)) ||
                !cmp_json_object_str(&item, "expression", roll->expression, sizeof(roll->expression)) ||
                !cmp_json_object_str(&item, "entropy_source", entropy, sizeof(entropy)) || strcmp(entropy, "getrandom") ||
                !number(&item, "total", -100, 100, &roll->total) ||
                strcmp(selection->character_id, roll->character_id) || strcmp(selection->expression, roll->expression) ||
                !cmp_json_field_array(cmp_json_object_field(&item, "rolls"), &numbers)) return -1;
            while ((nrc = cmp_json_array_next(&numbers, &number_field)) == 1) {
                if (roll->roll_count >= 2u || !cmp_json_field_double(&number_field, &value) ||
                    value < 1 || value > 20 || value != (double)(uint32_t)value) return -1;
                roll->rolls[roll->roll_count++] = (uint32_t)value;
            }
            if (nrc || !cmp_json_field_array(cmp_json_object_field(&item, "kept_indices"), &numbers) ||
                cmp_json_array_next(&numbers, &number_field) != 1 ||
                !cmp_json_field_double(&number_field, &value) || (value != 0 && value != 1) ||
                cmp_json_array_next(&numbers, &number_field) != 0) return -1;
            roll->kept_index = (uint32_t)value;
            for (size_t i = 0; i < state.participant_count; ++i) {
                if (!strcmp(participants[i].id, roll->character_id)) {
                    if (participants[i].initiative != roll->total) return -1;
                    found = 1;
                }
            }
            if (!found) return -1;
        }
        if (rc || receipt.roll_count != request->count) return -1;
        result->initiative_length = pb_encode_turn_initiative(result->initiative_wire,
            sizeof(result->initiative_wire), &receipt, rolls);
        if (!result->initiative_length) return -1;
    }
    if (action && action->participant_id[0]) {
        const turn_encounter_participant_c *target = NULL;
        for (size_t i = 0; i < state.participant_count; ++i)
            if (!strcmp(participants[i].id, action->participant_id)) target = &participants[i];
        if (!target) return -1;
        if (action->operation == DND_ACTION_DAMAGE || action->operation == DND_ACTION_HEAL)
            written = snprintf(result->text, sizeof(result->text), "%s has %d of %d hit points after %u %s.",
                target->name, target->current_hp, target->max_hp, action->amount,
                action->operation == DND_ACTION_DAMAGE ? "damage" : "healing");
        else if (action->operation == DND_ACTION_CONDITION_ADD)
            written = snprintf(result->text, sizeof(result->text), "%s is now %s.", target->name, dnd_condition_name(action->condition));
        else written = snprintf(result->text, sizeof(result->text), "Removed %s from %s.", dnd_condition_name(action->condition), target->name);
    } else if (!strcmp(state.status, "ended"))
        written = snprintf(result->text, sizeof(result->text), "This encounter has ended at round %d.", state.round);
    else if (!state.participant_count)
        written = snprintf(result->text, sizeof(result->text), "Round %d. This encounter has no participants.", state.round);
    else if (state.active_index < 0)
        written = snprintf(result->text, sizeof(result->text), "Round %d has %zu %s. No turn is active.", state.round,
            state.participant_count, state.participant_count == 1u ? "participant" : "participants");
    else
        written = snprintf(result->text, sizeof(result->text), "Round %d. %s acts now. The initiative order has %zu %s.",
            state.round, participants[state.active_index].name, state.participant_count,
            state.participant_count == 1u ? "participant" : "participants");
    if (written <= 0 || (size_t)written >= sizeof(result->text)) return -1;
    result->encounter_length = length;
    return 0;
}

int dnd_encounter_output(const char *json, const char *campaign_id, const char *encounter_id,
                         dnd_tool_result *result) {
    return encounter_output(json, campaign_id, encounter_id, NULL, NULL, result);
}

int dnd_initiative_output(const char *json, const char *campaign_id, const char *encounter_id,
    const dnd_initiative_request_c *request, dnd_tool_result *result) {
    if (!dnd_initiative_request_valid(request)) return -1;
    return encounter_output(json, campaign_id, encounter_id, request, NULL, result);
}

int dnd_action_output(const char *json, const char *campaign_id, const char *encounter_id,
    const dnd_encounter_action_c *request, dnd_tool_result *result) {
    if (!dnd_encounter_action_valid(request)) return -1;
    return encounter_output(json, campaign_id, encounter_id, NULL, request, result);
}

int dnd_campaign_input(const dnd_campaign_request_c *r, const char *campaign_id,
                        const char *user_id, char *out, size_t capacity) {
    char name[401], ruleset[241], species[241], class_name[241], owner[257];
    int n;
    if (!r || !campaign_id || !user_id || !out || !dnd_campaign_request_valid(r) ||
        !dnd_request_id_valid(campaign_id, strlen(campaign_id) + 1u)) return -1;
    if (r->operation == DND_CAMPAIGN_CREATE) {
        if (cmp_json_escape_exact(r->data.campaign.name, name, sizeof(name)) ||
            cmp_json_escape_exact(r->data.campaign.ruleset, ruleset, sizeof(ruleset))) return -1;
        n = snprintf(out, capacity, "{\"operation\":\"create\",\"campaign_id\":\"%s\",\"expected_version\":0,"
            "\"campaign\":{\"name\":\"%s\",\"ruleset\":\"%s\"}}", campaign_id, name, ruleset);
    } else {
        const dnd_character_create_c *c = &r->data.character;
        if (cmp_json_escape_exact(c->name, name, sizeof(name)) ||
            cmp_json_escape_exact(c->species, species, sizeof(species)) ||
            cmp_json_escape_exact(c->class_name, class_name, sizeof(class_name)) ||
            cmp_json_escape_exact(c->kind == 1u ? user_id : "", owner, sizeof(owner))) return -1;
        n = snprintf(out, capacity, "{\"operation\":\"add_character\",\"campaign_id\":\"%s\",\"expected_version\":%lld,"
            "\"character\":{\"id\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"player_user_id\":\"%s\","
            "\"species\":\"%s\",\"class\":\"%s\",\"level\":%u,\"armor_class\":%u,\"max_hp\":%u,"
            "\"ability_scores\":{\"strength\":%u,\"dexterity\":%u,\"constitution\":%u,\"intelligence\":%u,\"wisdom\":%u,\"charisma\":%u},"
            "\"sheet_refs\":{},\"persona\":\"\",\"allowed_knowledge_scopes\":[],\"voice_id\":\"\","
            "\"speaking_style\":\"\",\"pronunciation\":\"\",\"safety_rules\":[]}}",
            campaign_id, (long long)r->expected_version, c->id, name, c->kind == 1u ? "player" : "npc", owner,
            species, class_name, c->level, c->armor_class, c->max_hp,
            c->abilities[0], c->abilities[1], c->abilities[2], c->abilities[3], c->abilities[4], c->abilities[5]);
    }
    return n > 0 && (size_t)n < capacity ? n : -1;
}

static int setup_character_matches(const cmp_json_object *item, const dnd_character_create_c *c, const char *user) {
    static const char *const abilities[] = {"strength", "dexterity", "constitution", "intelligence", "wisdom", "charisma"};
    static const char *const empty[] = {"persona", "voice_id", "speaking_style", "pronunciation"};
    char value[257];
    int64_t number_value;
    cmp_json_object scores, refs;
    cmp_json_array array;
    cmp_json_field field;
    if (item->field_count != 17u ||
        !cmp_json_object_str(item, "player_user_id", value, sizeof(value)) || strcmp(value, c->kind == 1u ? user : "") ||
        !cmp_json_object_str(item, "species", value, sizeof(value)) || strcmp(value, c->species) ||
        !cmp_json_object_str(item, "class", value, sizeof(value)) || strcmp(value, c->class_name) ||
        !cmp_json_object_i64(item, "level", &number_value) || number_value != c->level ||
        !cmp_json_object_i64(item, "armor_class", &number_value) || number_value != c->armor_class ||
        !cmp_json_object_object(item, "ability_scores", &scores) || scores.field_count != 6u ||
        !cmp_json_object_object(item, "sheet_refs", &refs) || refs.field_count) return 0;
    for (size_t i = 0; i < 6u; ++i)
        if (!cmp_json_object_i64(&scores, abilities[i], &number_value) || number_value != c->abilities[i]) return 0;
    for (size_t i = 0; i < 4u; ++i)
        if (!cmp_json_object_str(item, empty[i], value, sizeof(value)) || value[0]) return 0;
    return cmp_json_field_array(cmp_json_object_field(item, "allowed_knowledge_scopes"), &array) &&
        cmp_json_array_next(&array, &field) == 0 &&
        cmp_json_field_array(cmp_json_object_field(item, "safety_rules"), &array) && cmp_json_array_next(&array, &field) == 0;
}

static int roster_output(const char *json, const char *campaign_id, const char *user_id,
                          const dnd_campaign_request_c *request, dnd_tool_result *result) {
    turn_campaign_roster_c state = {0};
    turn_campaign_character_c characters[DND_TURN_CHARACTERS_MAX];
    cmp_json_object object;
    cmp_json_array array;
    cmp_json_field field;
    char operation[32];
    const char *expected = !request ? "get_roster" : request->operation == DND_CAMPAIGN_CREATE ? "create" : "add_character";
    int rc, written, added = 0;
    if (!result || !campaign_id) return -1;
    result->encounter_length = result->initiative_length = result->roster_length = 0;
    result->text[0] = '\0';
    if (!cmp_json_object_parse(json, &object) || object.field_count != (request ? 9u : 6u) ||
        !cmp_json_object_str(&object, "operation", operation, sizeof(operation)) || strcmp(operation, expected) ||
        !cmp_json_object_str(&object, "operation_id", state.tool_call_id, sizeof(state.tool_call_id)) ||
        !cmp_json_object_str(&object, "campaign_id", state.campaign_id, sizeof(state.campaign_id)) ||
        !cmp_json_object_str(&object, "status", state.status, sizeof(state.status)) ||
        !cmp_json_object_i64(&object, "version", &state.version) ||
        !cmp_json_field_array(cmp_json_object_field(&object, "characters"), &array) ||
        strcmp(campaign_id, state.campaign_id) || strcmp(result->call_id, state.tool_call_id)) return -1;
    if (request && (state.version != request->expected_version + 1 || strcmp(state.status, "active"))) return -1;
    if (request) {
        cmp_json_object campaign, director;
        cmp_json_array recaps;
        if (!cmp_json_object_object(&object, "campaign", &campaign) || campaign.field_count != 5u ||
            !cmp_json_object_object(&object, "scene_director", &director) || director.field_count != 5u ||
            !cmp_json_field_array(cmp_json_object_field(&object, "session_recaps"), &recaps)) return -1;
        if (request->operation == DND_CAMPAIGN_CREATE) {
            cmp_json_object rules;
            cmp_json_array cast;
            cmp_json_field entry;
            char empty[2];
            int64_t value;
            if (!cmp_json_object_str(&campaign, "description", empty, sizeof(empty)) || empty[0] ||
                !cmp_json_object_str(&campaign, "current_scene", empty, sizeof(empty)) || empty[0] ||
                !cmp_json_object_object(&campaign, "house_rules", &rules) || rules.field_count ||
                cmp_json_array_next(&recaps, &entry) != 0 ||
                !cmp_json_field_array(cmp_json_object_field(&director, "active_npc_ids"), &cast) || cmp_json_array_next(&cast, &entry) != 0 ||
                !cmp_json_object_i64(&director, "next_speaker_index", &value) || value ||
                !cmp_json_object_i64(&director, "lease_expires_unix_ms", &value) || value ||
                !cmp_json_object_str(&director, "active_speaker_id", empty, sizeof(empty)) || empty[0] ||
                !cmp_json_object_str(&director, "active_turn_id", empty, sizeof(empty)) || empty[0]) return -1;
        }
    }
    if (request && request->operation == DND_CAMPAIGN_CREATE) {
        cmp_json_object campaign;
        char name[201], ruleset[121];
        if (!cmp_json_object_object(&object, "campaign", &campaign) || campaign.field_count != 5u ||
            !cmp_json_object_str(&campaign, "name", name, sizeof(name)) || strcmp(name, request->data.campaign.name) ||
            !cmp_json_object_str(&campaign, "ruleset", ruleset, sizeof(ruleset)) || strcmp(ruleset, request->data.campaign.ruleset)) return -1;
    }
    while ((rc = cmp_json_array_next(&array, &field)) == 1) {
        cmp_json_object item;
        if (state.character_count >= DND_TURN_CHARACTERS_MAX) return -1;
        turn_campaign_character_c *character = &characters[state.character_count++];
        memset(character, 0, sizeof(*character));
        if (!cmp_json_field_object(&field, &item) ||
            (request ? (item.field_count != 16u && item.field_count != 17u) : item.field_count != 4u) ||
            !cmp_json_object_str(&item, "id", character->id, sizeof(character->id)) ||
            !cmp_json_object_str(&item, "name", character->name, sizeof(character->name)) ||
            !cmp_json_object_str(&item, "kind", character->kind, sizeof(character->kind)) ||
            !number(&item, "max_hp", 0, 100000, &character->max_hp) || !public_name(character->name)) return -1;
        if (request && request->operation == DND_CAMPAIGN_ADD_CHARACTER && !strcmp(character->id, request->data.character.id)) {
            const dnd_character_create_c *c = &request->data.character;
            if (strcmp(character->name, c->name) || strcmp(character->kind, c->kind == 1u ? "player" : "npc") ||
                character->max_hp != (int32_t)c->max_hp || !setup_character_matches(&item, c, user_id)) return -1;
            ++added;
        }
    }
    if (rc) return -1;
    if (request && (request->operation == DND_CAMPAIGN_CREATE ? state.character_count != 0u : added != 1)) return -1;
    memcpy(state.output_sha256, result->output_sha256, sizeof(state.output_sha256));
    size_t length = pb_encode_turn_roster(result->roster_wire, sizeof(result->roster_wire), &state, characters);
    if (!length) return -1;
    written = snprintf(result->text, sizeof(result->text), "The %scampaign roster has %zu %s.",
        !strcmp(state.status, "archived") ? "archived " : "", state.character_count,
        state.character_count == 1u ? "character" : "characters");
    if (written <= 0 || (size_t)written >= sizeof(result->text)) return -1;
    result->roster_length = length;
    return 0;
}

int dnd_roster_output(const char *json, const char *campaign_id, dnd_tool_result *result) {
    return roster_output(json, campaign_id, NULL, NULL, result);
}

int dnd_campaign_output(const char *json, const char *campaign_id, const char *user_id,
    const dnd_campaign_request_c *request, dnd_tool_result *result) {
    if (!user_id || !dnd_campaign_request_valid(request)) return -1;
    return roster_output(json, campaign_id, user_id, request, result);
}
